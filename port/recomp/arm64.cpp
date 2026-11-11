// ARM64 machine code from the IR, laid out as the x86-64 back end's (x64.cpp): every temporary
// has a stack slot, every IR operation loads its operands into w0/w1 (or the argument registers)
// and keeps its result in w0, and the CPU's state (CoiCpu) is in x19 throughout. ARM64 sets its
// flags at 32 or 64 bits only, so add, sub and cmp at a byte or word shift both operands to the
// top of a register first; then N, Z and V are the 68000's, and so is C, except that a subtract
// sets ARM's carry when nothing was borrowed (the 68000's C is then `lo`). The rarer
// flag-setting operations call the runtime (native_helpers.c).
//
// The frame, the blocks and budget checks, dead flags and the tables are shared with the x86-64
// back end (jit.h).
#include "arm64.h"

#include "jit.h"

#include <cstddef>
#include <cstring>
#include <map>
#include <set>
#include <stdexcept>
#include <vector>

namespace recomp {

namespace {

enum Reg { W0 = 0, W1 = 1, W2 = 2, W3 = 3, X9 = 9, X10 = 10, X11 = 11, X16 = 16, X17 = 17, X19 = 19, X20 = 20,
           X29 = 29, X30 = 30, SP = 31, ZR = 31 };
enum Cond { EQ = 0, NE = 1, HS = 2, LO = 3, MI = 4, PL = 5, VS = 6, VC = 7, LT = 11 };

class Asm {
public:
    std::vector<uint8_t> b;

    size_t pos() const { return b.size(); }
    void op(uint32_t w) { for (int i = 0; i < 4; i++) b.push_back(uint8_t(w >> (8 * i))); }

    // Branches: the instruction's offset, for patch(). `kind` 26: b/bl; 19: b.cond, cbz, cbnz.
    struct Fix { size_t at; int kind; };
    void patch(const Fix& f, size_t target)
    {
        const int64_t rel = (int64_t(target) - int64_t(f.at)) / 4;
        const int64_t lim = int64_t(1) << (f.kind - 1);
        if (rel < -lim || rel >= lim)
            throw std::runtime_error("arm64: a branch is out of range");
        uint32_t w;
        std::memcpy(&w, &b[f.at], 4);
        if (f.kind == 26)
            w |= uint32_t(rel) & 0x3FFFFFF;
        else
            w |= (uint32_t(rel) & 0x7FFFF) << 5;
        std::memcpy(&b[f.at], &w, 4);
    }
    Fix jmp() { const size_t at = pos(); op(0x14000000); return {at, 26}; }
    Fix bcond(int cond) { const size_t at = pos(); op(0x54000000 | uint32_t(cond)); return {at, 19}; }
    Fix cbz(int rt, bool x = false) { const size_t at = pos(); op((x ? 0xB4000000u : 0x34000000u) | uint32_t(rt)); return {at, 19}; }
    Fix cbnz(int rt, bool x = false) { const size_t at = pos(); op((x ? 0xB5000000u : 0x35000000u) | uint32_t(rt)); return {at, 19}; }

    void movz(int rd, uint32_t imm16, int hw, bool x) { op((x ? 0xD2800000u : 0x52800000u) | uint32_t(hw) << 21 | (imm16 & 0xFFFF) << 5 | uint32_t(rd)); }
    void movk(int rd, uint32_t imm16, int hw, bool x) { op((x ? 0xF2800000u : 0x72800000u) | uint32_t(hw) << 21 | (imm16 & 0xFFFF) << 5 | uint32_t(rd)); }
    void mov_imm32(int rd, uint32_t v)                  // (the register's upper half cleared)
    {
        movz(rd, v & 0xFFFF, 0, false);
        if (v >> 16)
            movk(rd, v >> 16, 1, false);
    }
    void mov_imm64(int rd, uint64_t v)
    {
        movz(rd, uint32_t(v & 0xFFFF), 0, true);
        for (int hw = 1; hw < 4; hw++)
            if ((v >> (16 * hw)) & 0xFFFF)
                movk(rd, uint32_t(v >> (16 * hw)) & 0xFFFF, hw, true);
    }
    void mov32(int rd, int rm) { op(0x2A0003E0 | uint32_t(rm) << 16 | uint32_t(rd)); }    // orr wd, wzr, wm
    void mov64(int rd, int rm) { op(0xAA0003E0 | uint32_t(rm) << 16 | uint32_t(rd)); }

    // Register forms: 32-bit add 0B, adds 2B, sub 4B, subs 6B, and 0A, orr 2A, eor 4A, ands 6A
    // (the top byte; | 0x80 for 64 bits).
    void rrr(uint32_t top, int rd, int rn, int rm) { op(top << 24 | uint32_t(rm) << 16 | uint32_t(rn) << 5 | uint32_t(rd)); }
    void add_imm(int rd, int rn, uint32_t imm12, bool x) { op((x ? 0x91000000u : 0x11000000u) | imm12 << 10 | uint32_t(rn) << 5 | uint32_t(rd)); }
    void sub_imm(int rd, int rn, uint32_t imm12, bool x) { op((x ? 0xD1000000u : 0x51000000u) | imm12 << 10 | uint32_t(rn) << 5 | uint32_t(rd)); }
    void add_sxtw(int xd, int xn, int wm) { op(0x8B20C000 | uint32_t(wm) << 16 | uint32_t(xn) << 5 | uint32_t(xd)); }
    // Logical immediates with `ones` low bits set (1-31): and 12, orr 32, eor 52, ands 72.
    void logic_imm(uint32_t top, int rd, int rn, int ones) { op(top << 24 | uint32_t(ones - 1) << 10 | uint32_t(rn) << 5 | uint32_t(rd)); }
    void and_mask(int rd, int rn, int ones) { logic_imm(0x12, rd, rn, ones); }
    void lslv(int rd, int rn, int rm) { op(0x1AC02000 | uint32_t(rm) << 16 | uint32_t(rn) << 5 | uint32_t(rd)); }
    void lsrv(int rd, int rn, int rm) { op(0x1AC02400 | uint32_t(rm) << 16 | uint32_t(rn) << 5 | uint32_t(rd)); }
    void ubfm(int rd, int rn, int immr, int imms) { op(0x53000000 | uint32_t(immr) << 16 | uint32_t(imms) << 10 | uint32_t(rn) << 5 | uint32_t(rd)); }
    void sbfm(int rd, int rn, int immr, int imms) { op(0x13000000 | uint32_t(immr) << 16 | uint32_t(imms) << 10 | uint32_t(rn) << 5 | uint32_t(rd)); }
    void lsl(int rd, int rn, int sh) { ubfm(rd, rn, (32 - sh) & 31, 31 - sh); }
    void lsr(int rd, int rn, int sh) { ubfm(rd, rn, sh, 31); }
    void uxt(int rd, int rn, int size) { ubfm(rd, rn, 0, 8 * size - 1); }     // size 1 or 2
    void sxt(int rd, int rn, int size) { sbfm(rd, rn, 0, 8 * size - 1); }
    void mul(int rd, int rn, int rm, bool x) { op((x ? 0x9B007C00u : 0x1B007C00u) | uint32_t(rm) << 16 | uint32_t(rn) << 5 | uint32_t(rd)); }
    void udiv64(int rd, int rn, int rm) { op(0x9AC00800 | uint32_t(rm) << 16 | uint32_t(rn) << 5 | uint32_t(rd)); }
    void cset(int rd, int cond) { op(0x1A9F07E0 | uint32_t(cond ^ 1) << 12 | uint32_t(rd)); }
    void cmp32(int rn, int rm) { rrr(0x6B, ZR, rn, rm); }
    void cmp64(int rn, int rm) { rrr(0xEB, ZR, rn, rm); }
    void tst32(int rn, int rm) { rrr(0x6A, ZR, rn, rm); }

    // Loads and stores at [base + off]: 1, 2, 4 or 8 bytes (loads zero-extend).
    void ls(bool load, int size, int rt, int rn, int32_t off)
    {
        const int sz = size == 1 ? 0 : size == 2 ? 1 : size == 4 ? 2 : 3;
        const uint32_t base = uint32_t(sz) << 30 | 0x38000000u | (load ? 0x00400000u : 0u);
        if (off >= 0 && off % size == 0 && off / size < 4096)
            op(base | 0x01000000u | uint32_t(off / size) << 10 | uint32_t(rn) << 5 | uint32_t(rt));
        else if (off >= -256 && off < 256)
            op(base | (uint32_t(off) & 0x1FF) << 12 | uint32_t(rn) << 5 | uint32_t(rt));
        else
            throw std::runtime_error("arm64: an offset is out of range");
    }
    void call_abs(const void* fn)
    {
        mov_imm64(X16, uint64_t(reinterpret_cast<uintptr_t>(fn)));
        op(0xD63F0000 | X16 << 5);                      // blr x16
    }
};

int32_t off_reg(uint32_t r) { return r < 8 ? int32_t(offsetof(CoiCpu, d) + 4 * r) : int32_t(offsetof(CoiCpu, a) + 4 * (r - 8)); }
constexpr int32_t kOffCycles = int32_t(offsetof(CoiCpu, cycles));
constexpr int32_t kOffLimit = int32_t(offsetof(CoiCpu, cycle_limit));
constexpr int32_t kOffFail = int32_t(offsetof(CoiCpu, fail));
constexpr int32_t kOffMissing = int32_t(offsetof(CoiCpu, missing));
constexpr int32_t kOffJump = int32_t(offsetof(CoiCpu, jump));
constexpr int32_t kOffPc = int32_t(offsetof(CoiCpu, pc));
constexpr int32_t kOffX = int32_t(offsetof(CoiCpu, x));
constexpr int32_t kOffN = int32_t(offsetof(CoiCpu, n));
constexpr int32_t kOffZ = int32_t(offsetof(CoiCpu, z));
constexpr int32_t kOffV = int32_t(offsetof(CoiCpu, v));
constexpr int32_t kOffC = int32_t(offsetof(CoiCpu, c));
constexpr int32_t kOffSrHigh = int32_t(offsetof(CoiCpu, sr_high));
constexpr int32_t kOffUsp = int32_t(offsetof(CoiCpu, usp));

class Emitter {
public:
    Emitter(uint32_t temps, bool trace, const std::map<uint32_t, size_t>& slot_of, std::vector<CoiNativeFn>& slots)
        : frame((4 * temps + 15) & ~15u), trace(trace), slot_of(slot_of), slots(slots)
    {
        if (frame >= 4096)
            throw std::runtime_error("arm64: the frame is too big");
    }

    const std::vector<uint8_t>& code() const { return a.b; }
    std::vector<std::pair<size_t, uint32_t>> marks;         // each instruction's code: offset, address (top bit: checked)

    // Emits a function; returns its entry's offset, and each instruction's offset in `insn_at`.
    size_t function(const Function& f, const std::map<uint32_t, IrBlock>& blocks, std::map<uint32_t, size_t>& insn_at)
    {
        const size_t start = a.pos();
        insn.clear();
        careful.clear();
        insn_fix.clear();
        careful_fix.clear();
        exit0_fix.clear();
        exit_fix.clear();
        // Entry: c in x19, the frame; at != 0 resumes there.
        a.op(0xA9BF7BFD);                                   // stp x29, x30, [sp, #-16]!
        a.op(0xA9BF53F3);                                   // stp x19, x20, [sp, #-16]!
        a.sub_imm(SP, SP, frame, true);
        a.mov64(X19, W0);
        const Asm::Fix to_entry = a.cbz(W1);
        a.mov32(W0, W1);
        a.call_abs(reinterpret_cast<const void*>(&jit::find));
        const Asm::Fix unknown = a.cbz(W0, true);
        a.op(0xD61F0000);                                   // br x0
        a.patch(to_entry, a.pos());
        a.patch(unknown, a.pos());
        insn_fix.push_back({a.jmp(), f.entry});

        const jit::Layout l = jit::layout(f, blocks, trace);
        const auto& leaders = l.leaders;
        layout_ = &l;

        // Unchecked: each block once checked at its start.
        uint32_t expected = 0xFFFFFFFF;
        for (const auto& [addr, in] : f.insns) {
            if (expected != 0xFFFFFFFF && addr != expected)
                jump_to(expected, leaders);
            if (leaders.count(addr)) {
                insn[addr] = a.pos();
                check_block(l.most.at(addr), addr);
            }
            block(blocks.at(addr), false);
            expected = ends_block(in) ? 0xFFFFFFFF : addr + uint32_t(in.length);
        }
        if (expected != 0xFFFFFFFF)
            jump_to(expected, leaders);

        // Checked: before every instruction (and every flag stored); back to the unchecked copy
        // at the next block.
        layout_ = nullptr;
        expected = 0xFFFFFFFF;
        for (const auto& [addr, in] : f.insns) {
            if (expected != 0xFFFFFFFF && (addr != expected || leaders.count(addr)))
                jump_to(expected, leaders);
            careful[addr] = a.pos();
            insn_at[addr] = a.pos();
            block(blocks.at(addr), true);
            expected = ends_block(in) ? 0xFFFFFFFF : addr + uint32_t(in.length);
        }
        if (expected != 0xFFFFFFFF)
            jump_to(expected, leaders);

        // Exits: with 0 (stopped), or with w0.
        const size_t exit0 = a.pos();
        a.movz(W0, 0, 0, false);
        const size_t exit = a.pos();
        a.add_imm(SP, SP, frame, true);
        a.op(0xA8C153F3);                                   // ldp x19, x20, [sp], #16
        a.op(0xA8C17BFD);                                   // ldp x29, x30, [sp], #16
        a.op(0xD65F03C0);                                   // ret
        for (const auto& at : exit0_fix) a.patch(at, exit0);
        for (const auto& at : exit_fix) a.patch(at, exit);
        for (const auto& [at, target] : insn_fix) a.patch(at, insn.at(target));
        for (const auto& [at, target] : careful_fix) a.patch(at, careful.at(target));
        return start;
    }

private:
    Asm a;
    uint32_t frame;
    bool trace;
    const std::map<uint32_t, size_t>& slot_of;
    std::vector<CoiNativeFn>& slots;
    std::map<uint32_t, size_t> insn;                        // this function's blocks, unchecked
    std::map<uint32_t, size_t> careful;                     // its instructions, checked
    std::vector<std::pair<Asm::Fix, uint32_t>> insn_fix;    // jumps to blocks
    std::vector<std::pair<Asm::Fix, uint32_t>> careful_fix; // jumps to checked instructions
    std::vector<Asm::Fix> exit0_fix, exit_fix;

    const jit::Layout* layout_ = nullptr;                   // the unchecked copy's (null: store every flag)
    uint32_t needed(const IrInst& i) const { return layout_ ? layout_->needed(i) : jit::kAllFlags; }

    // Continues at the instruction at `target`: a block's check, or the checked copy.
    void jump_to(uint32_t target, const std::set<uint32_t>& leaders)
    {
        if (leaders.count(target)) insn_fix.push_back({a.jmp(), target});
        else careful_fix.push_back({a.jmp(), target});
    }

    // At a block's start: if `most` more cycles might reach the budget, the checked copy.
    void check_block(uint32_t most, uint32_t addr)
    {
        a.ls(true, 8, X9, X19, kOffLimit);
        const Asm::Fix none = a.cbz(X9, true);
        a.ls(true, 8, X10, X19, kOffCycles);
        a.mov_imm32(X11, most);
        a.rrr(0x8B, X10, X10, X11);                         // add x10, x10, x11
        a.cmp64(X10, X9);
        const Asm::Fix fits = a.bcond(LT);
        careful_fix.push_back({a.jmp(), addr});
        a.patch(none, a.pos());
        a.patch(fits, a.pos());
    }

    // The temporary w0 holds (-1: none known), so the next operation needn't load it again.
    // Forgotten at labels, calls, and after any operation that doesn't end by storing w0.
    int64_t in_w0 = -1;
    bool stored = false;

    static int32_t slot(uint32_t t) { return 4 * int32_t(t); }

    // Loads go into the other registers first and w0 last, so w0 still holds what it held.
    void load(int r, const Val& v)
    {
        if (v.kind == Val::Temp && int64_t(v.n) == in_w0) {
            if (r != W0)
                a.mov32(r, W0);
            return;
        }
        if (v.kind == Val::Imm)
            a.mov_imm32(r, v.n);
        else if (v.kind == Val::Temp)
            a.ls(true, 4, r, SP, slot(v.n));
        if (r == W0)
            in_w0 = v.kind == Val::Temp ? int64_t(v.n) : -1;
    }
    void store(const IrInst& i)
    {
        a.ls(false, 4, W0, SP, slot(i.dst));
        in_w0 = i.dst;
        stored = true;
    }
    void call(const void* fn)
    {
        a.call_abs(fn);
        in_w0 = -1;
    }

    // The port's bus, as Musashi calls it (the address to 24 bits, the value to its size).
    void bus_read(const IrInst& i)
    {
        load(W0, i.a);
        a.and_mask(W0, W0, 24);
        call(reinterpret_cast<const void*>(i.size == 1 ? &m68k_read_memory_8 : i.size == 2 ? &m68k_read_memory_16 : &m68k_read_memory_32));
        store(i);
    }
    void bus_write(const IrInst& i)
    {
        load(W1, i.b);
        if (i.size < 4)
            a.and_mask(W1, W1, 8 * i.size);
        load(W0, i.a);
        a.and_mask(W0, W0, 24);
        call(reinterpret_cast<const void*>(i.size == 1 ? &m68k_write_memory_8 : i.size == 2 ? &m68k_write_memory_16 : &m68k_write_memory_32));
    }

    // A runtime helper: c and up to three arguments; the result in w0.
    void helper(CoiHelper fn, Val x = {}, Val y = {}, Val z = {})
    {
        load(W3, z);
        load(W2, y);
        load(W1, x);
        a.mov64(W0, X19);
        in_w0 = -1;
        call(reinterpret_cast<const void*>(fn));
    }
    void helper_value(const IrInst& i, CoiHelper fn, Val x = {}, Val y = {}, Val z = {})
    {
        helper(fn, x, y, z);
        store(i);
    }

    void setcc(int cond, int32_t off)                       // byte [x19+off] = cond
    {
        a.cset(X9, cond);
        a.ls(false, 1, X9, X19, off);
    }

    // add/sub (d op s, d = b, s = a) with the 68000's N, Z, V, C (and X, unless cmp); the result
    // (its low `size` bytes) in w0.
    void arith(const IrInst& i, bool sub, bool set_x)
    {
        const uint32_t need = needed(i);
        load(W1, i.a);
        load(W0, i.b);
        const uint32_t top = sub ? 0x6B : 0x2B;             // subs / adds
        const int sh = 32 - 8 * i.size;
        if (sh) {
            a.lsl(X9, W0, sh);
            a.lsl(X10, W1, sh);
            a.rrr(top, X11, X9, X10);
        } else {
            a.rrr(top, X11, W0, W1);
        }
        const int carry = sub ? LO : HS;
        if (need & jit::kFN) setcc(MI, kOffN);
        if (need & jit::kFZ) setcc(EQ, kOffZ);
        if (need & jit::kFV) setcc(VS, kOffV);
        if (need & jit::kFC) setcc(carry, kOffC);
        if (set_x && (need & jit::kFX)) setcc(carry, kOffX);
        if (sh) a.lsr(W0, X11, sh);
        else a.mov32(W0, X11);
        in_w0 = -1;
    }

    void exit_stopped(uint32_t address)                     // c->fail = address, return 0
    {
        a.mov_imm32(X9, address);
        a.ls(false, 4, X9, X19, kOffFail);
        exit0_fix.push_back(a.jmp());
    }

    // An instruction's code; `checked`: with its own budget check (a RAM stub's jmp keeps its
    // check either way).
    void block(const IrBlock& block, bool checked)
    {
        marks.push_back({a.pos(), block.address | (checked ? 0x80000000u : 0)});
        std::vector<size_t> label(block.labels, 0);
        std::vector<std::pair<Asm::Fix, uint32_t>> label_fix;
        in_w0 = -1;                                             // an instruction is a jump target
        for (const IrInst& i : block.code) {
            const Val S = Val::imm(i.size);
            stored = false;
            switch (i.op) {
            case Ir::Mov: load(W0, i.a); store(i); break;
            case Ir::GetReg: a.ls(true, 4, W0, X19, off_reg(i.k)); store(i); break;
            case Ir::GetUsp: a.ls(true, 4, W0, X19, kOffUsp); store(i); break;
            case Ir::Add: case Ir::Sub: case Ir::And: case Ir::Or: case Ir::Xor: {
                load(W1, i.b);
                load(W0, i.a);
                const uint32_t top = i.op == Ir::Add ? 0x0B : i.op == Ir::Sub ? 0x4B : i.op == Ir::And ? 0x0A : i.op == Ir::Or ? 0x2A : 0x4A;
                a.rrr(top, W0, W0, W1);
                store(i);
                break;
            }
            case Ir::Shl: case Ir::Shr:
                load(W1, i.b);
                load(W0, i.a);
                if (i.op == Ir::Shl) a.lslv(W0, W0, W1);
                else a.lsrv(W0, W0, W1);
                store(i);
                break;
            case Ir::Eq: case Ir::Ne: case Ir::Ltu:
                load(W1, i.b);
                load(W0, i.a);
                a.cmp32(W0, W1);
                a.cset(W0, i.op == Ir::Eq ? EQ : i.op == Ir::Ne ? NE : LO);
                store(i);
                break;
            case Ir::MulU16: case Ir::MulS16:
                load(W1, i.b);
                load(W0, i.a);
                if (i.op == Ir::MulU16) { a.uxt(W0, W0, 2); a.uxt(W1, W1, 2); }
                else { a.sxt(W0, W0, 2); a.sxt(W1, W1, 2); }
                a.mul(W0, W0, W1, false);
                store(i);
                break;
            case Ir::Sext:
                load(W0, i.a);
                a.sxt(W0, W0, i.size);
                store(i);
                break;
            case Ir::Read: bus_read(i); break;
            case Ir::GetSrHigh: a.ls(true, 2, W0, X19, kOffSrHigh); store(i); break;
            case Ir::AddF: case Ir::SubF:
                arith(i, i.op == Ir::SubF, true);
                store(i);
                break;
            case Ir::AddxF: helper_value(i, coi_h_addx, S, i.a, i.b); break;
            case Ir::SubxF: helper_value(i, coi_h_subx, S, i.a, i.b); break;
            case Ir::LogicF: {                                      // N, Z from the value; V, C clear
                load(W0, i.a);
                const int sh = 32 - 8 * i.size;
                if (sh) {
                    a.lsl(X9, W0, sh);
                    a.tst32(X9, X9);
                } else {
                    a.tst32(W0, W0);
                }
                if (needed(i) & jit::kFN) setcc(MI, kOffN);
                if (needed(i) & jit::kFZ) setcc(EQ, kOffZ);
                if (needed(i) & jit::kFV) a.ls(false, 1, ZR, X19, kOffV);
                if (needed(i) & jit::kFC) a.ls(false, 1, ZR, X19, kOffC);
                if (sh) {
                    a.uxt(W0, W0, i.size);
                    in_w0 = -1;
                }
                store(i);
                break;
            }
            case Ir::ShiftF: helper_value(i, coi_h_shift, Val::imm(i.k | uint32_t(i.size) << 8), i.a, i.b); break;
            case Ir::Cond:
                if (i.k <= 1) {
                    a.mov_imm32(W0, i.k == 0 ? 1 : 0);
                } else if (i.k >= 4 && i.k <= 11) {                 // one flag: cc cs ne eq vc vs pl mi
                    const int32_t flag = i.k <= 5 ? kOffC : i.k <= 7 ? kOffZ : i.k <= 9 ? kOffV : kOffN;
                    a.ls(true, 1, W0, X19, flag);
                    if (!(i.k & 1)) a.logic_imm(0x52, W0, W0, 1);  // eor w0, w0, #1
                } else {
                    helper(coi_h_cond, Val::imm(i.k));
                }
                in_w0 = -1;
                store(i);
                break;
            case Ir::GetCcr: helper_value(i, coi_h_ccr); break;
            case Ir::MuluCycles: helper_value(i, coi_h_mulu_cycles, i.a); break;
            case Ir::MulsCycles: helper_value(i, coi_h_muls_cycles, i.a); break;
            case Ir::Divu: helper_value(i, coi_h_divu, Val::imm(i.k), i.a); break;
            case Ir::Divs: helper_value(i, coi_h_divs, Val::imm(i.k), i.a); break;
            case Ir::Pop32: helper_value(i, coi_h_pop32); break;
            case Ir::IrqLevel: helper_value(i, coi_h_irq_level); break;
            case Ir::Interrupt: helper_value(i, coi_h_interrupt, i.a); break;
            case Ir::Exception: helper_value(i, coi_h_exception, Val::imm(i.k), i.a, Val::imm(i.k2)); break;
            case Ir::StopCpu: helper_value(i, coi_h_stop, i.a, i.b); break;
            case Ir::SetReg: load(W0, i.a); a.ls(false, 4, W0, X19, off_reg(i.k)); break;
            case Ir::SetUsp: load(W0, i.a); a.ls(false, 4, W0, X19, kOffUsp); break;
            case Ir::SetRegPart: load(W0, i.a); a.ls(false, i.size, W0, X19, off_reg(i.k)); break;
            case Ir::Write: bus_write(i); break;
            case Ir::CmpF: arith(i, true, false); break;
            case Ir::SetCcr: helper(coi_h_set_ccr, i.a); break;
            case Ir::SetSrHigh: load(W0, i.a); a.ls(false, 2, W0, X19, kOffSrHigh); break;
            case Ir::SetZ:
                load(W1, i.b);
                load(W0, i.a);
                a.tst32(W0, W1);
                setcc(EQ, kOffZ);
                break;
            case Ir::Push32: helper(coi_h_push32, i.a); break;
            case Ir::Spin: helper(coi_h_spin, Val::imm(i.k)); break;
            case Ir::SetSr: helper(coi_h_set_sr, i.a); break;
            case Ir::Cycles:
                a.ls(true, 8, X9, X19, kOffCycles);
                a.add_imm(X9, X9, m68ki_cycles[0][i.k & 0xFFFF], true);
                a.ls(false, 8, X9, X19, kOffCycles);
                break;
            case Ir::AddCycles:
                load(W0, i.a);
                a.ls(true, 8, X9, X19, kOffCycles);
                a.add_sxtw(X9, X9, W0);
                a.ls(false, 8, X9, X19, kOffCycles);
                break;
            case Ir::Idle: {                                        // as RT_IDLE
                if (trace)
                    break;
                const uint32_t branch = m68ki_cycles[0][i.k2 & 0xFFFF];
                const uint32_t turn = m68ki_cycles[0][i.k & 0xFFFF] + branch;
                load(W0, i.a);
                const Asm::Fix not_taken = a.cbz(W0);
                a.ls(true, 8, X9, X19, kOffLimit);
                const Asm::Fix no_limit = a.cbz(X9, true);
                a.ls(true, 8, X10, X19, kOffCycles);
                a.rrr(0xCB, X9, X9, X10);                           // sub x9, x9, x10
                a.mov_imm32(X11, branch - 1);
                a.rrr(0x8B, X9, X9, X11);                           // add x9, x9, x11
                a.mov_imm32(X11, turn);
                a.udiv64(X9, X9, X11);
                a.mul(X9, X9, X11, true);
                const Asm::Fix none = a.cbz(X9, true);
                a.rrr(0x8B, X10, X10, X9);                          // add x10, x10, x9
                a.ls(false, 8, X10, X19, kOffCycles);
                helper(coi_h_logic, S, i.b);                        // the tst's flags
                a.patch(not_taken, a.pos());
                a.patch(no_limit, a.pos());
                a.patch(none, a.pos());
                break;
            }
            case Ir::Label: label[i.k] = a.pos(); break;
            case Ir::Br: label_fix.push_back({a.jmp(), i.k}); break;
            case Ir::BrIf: case Ir::BrIfNot:
                load(W0, i.a);
                label_fix.push_back({i.op == Ir::BrIf ? a.cbnz(W0) : a.cbz(W0), i.k});
                break;
            case Ir::Goto: insn_fix.push_back({a.jmp(), i.k}); break;
            case Ir::GotoIf:
                load(W0, i.a);
                insn_fix.push_back({a.cbnz(W0), i.k});
                break;
            case Ir::Trace: {
                a.mov_imm32(X9, i.k);
                a.ls(false, 4, X9, X19, kOffPc);                    // c->pc = address
#ifdef COI_NATIVE_TRACE
                if (!trace)
                    break;
                a.mov_imm64(X16, uint64_t(reinterpret_cast<uintptr_t>(&coi_native_tracing)));
                a.ls(true, 4, X9, X16, 0);
                const Asm::Fix skip = a.cbz(X9);
                helper(coi_h_trace, Val::imm(i.k));
                a.patch(skip, a.pos());
#endif
                break;
            }
            case Ir::Limit: {
                if (!checked && i.k == block.address)
                    break;
                a.ls(true, 8, X9, X19, kOffLimit);
                const Asm::Fix none = a.cbz(X9, true);
                a.ls(true, 8, X10, X19, kOffCycles);
                a.cmp64(X10, X9);
                const Asm::Fix left = a.bcond(LT);
                exit_stopped(i.k);
                a.patch(none, a.pos());
                a.patch(left, a.pos());
                break;
            }
            case Ir::Stop:                                          // give the cycles back, stop
                a.ls(true, 8, X9, X19, kOffCycles);
                a.sub_imm(X9, X9, m68ki_cycles[0][i.k2 & 0xFFFF], true);
                a.ls(false, 8, X9, X19, kOffCycles);
                exit_stopped(i.k);
                break;
            case Ir::SetMissing: a.mov_imm32(X9, i.k); a.ls(false, 4, X9, X19, kOffMissing); break;
            case Ir::Return: load(W0, i.a); exit_fix.push_back(a.jmp()); break;
            case Ir::JumpOut:
                a.movz(X9, 1, 0, false);
                a.ls(false, 1, X9, X19, kOffJump);
                a.mov_imm32(W0, i.k);
                exit_fix.push_back(a.jmp());
                break;
            case Ir::JumpDyn:
                load(W0, i.a);
                a.movz(X9, 1, 0, false);
                a.ls(false, 1, X9, X19, kOffJump);
                exit_fix.push_back(a.jmp());
                break;
            case Ir::Call: {
                a.mov_imm64(X16, uint64_t(reinterpret_cast<uintptr_t>(&slots[slot_of.at(i.k)])));
                a.ls(true, 8, W1, X16, 0);
                a.movz(W2, 0, 0, false);
                a.mov64(W0, X19);
                call(reinterpret_cast<const void*>(&jit::run));
                a.ls(true, 4, X9, X19, kOffFail);
                exit0_fix.push_back(a.cbnz(X9));
                a.mov_imm32(X9, i.k2);
                a.cmp32(W0, X9);                                    // returned where it should?
                const Asm::Fix ok = a.bcond(EQ);
                a.ls(false, 4, W0, X19, kOffFail);                  // elsewhere: stop there
                exit0_fix.push_back(a.jmp());
                a.patch(ok, a.pos());
                break;
            }
            }
            if (!stored)
                in_w0 = -1;
        }
        for (const auto& [at, l] : label_fix)
            a.patch(at, label[l]);
        in_w0 = -1;
    }
};

}  // namespace

const CoiNativeCode* translate_arm64(const Program& program, bool trace, std::string* error, uint32_t frame)
{
    return jit::translate<Emitter>(program, trace, error, frame);
}

}  // namespace recomp
