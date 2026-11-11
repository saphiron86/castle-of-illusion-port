// x86-64 machine code from the IR. Simple on purpose, for now: every temporary has a stack slot,
// every IR operation loads its operands into eax/ecx (or the argument registers) and stores its
// result. The CPU's state (CoiCpu) is in rbx throughout. add, sub and cmp at a byte, word or long
// set x86's carry, overflow, sign and zero exactly as the 68000 sets C, V, N and Z, so those
// (and the logic group's N and Z) are done inline with setcc; the rarer flag-setting operations
// call the runtime (native_helpers.c).
//
// The frame, the blocks and budget checks, dead flags and the tables are shared with the ARM64
// back end (jit.h).
#include "x64.h"

#include "jit.h"

#include <cstddef>
#include <cstring>
#include <map>
#include <set>
#include <vector>

namespace recomp {

namespace {

enum Reg { RAX = 0, RCX = 1, RDX = 2, RBX = 3, RSP = 4, RSI = 6, RDI = 7, R8 = 8, R9 = 9 };

#ifdef _WIN32
const int kArg[4] = {RCX, RDX, R8, R9};     // Microsoft x64: and 32 bytes of shadow space
#else
const int kArg[4] = {RDI, RSI, RDX, RCX};   // System V
#endif

class Asm {
public:
    std::vector<uint8_t> b;

    size_t pos() const { return b.size(); }
    void u8(int v) { b.push_back(uint8_t(v)); }
    void u32(uint32_t v) { for (int i = 0; i < 4; i++) u8(int(v >> (8 * i)) & 0xFF); }
    void u64(uint64_t v) { for (int i = 0; i < 8; i++) u8(int(v >> (8 * i)) & 0xFF); }
    void patch(size_t at, size_t target)
    {
        const int32_t rel = int32_t(int64_t(target) - int64_t(at + 4));
        std::memcpy(&b[at], &rel, 4);
    }

    void rex(bool w, int reg, int rm)
    {
        const int r = 0x40 | (w ? 8 : 0) | ((reg & 8) ? 4 : 0) | ((rm & 8) ? 1 : 0);
        if (r != 0x40)
            u8(r);
    }
    // ModRM for [base + disp32], with `reg` in the reg field.
    void mem(int reg, int base, int32_t disp)
    {
        u8(0x80 | ((reg & 7) << 3) | (base & 7));
        if ((base & 7) == RSP)
            u8(0x24);
        u32(uint32_t(disp));
    }
    void modrm_rr(int reg, int rm) { u8(0xC0 | ((reg & 7) << 3) | (rm & 7)); }

    void mov_imm32(int r, uint32_t v) { if (r & 8) u8(0x41); u8(0xB8 + (r & 7)); u32(v); }
    void mov_imm64(int r, uint64_t v) { u8(0x48 | ((r & 8) ? 1 : 0)); u8(0xB8 + (r & 7)); u64(v); }
    void load32(int r, int base, int32_t d) { rex(false, r, base); u8(0x8B); mem(r, base, d); }
    void store32(int r, int base, int32_t d) { rex(false, r, base); u8(0x89); mem(r, base, d); }
    void load64(int r, int base, int32_t d) { rex(true, r, base); u8(0x8B); mem(r, base, d); }
    void mov64(int dst, int src) { rex(true, src, dst); u8(0x89); modrm_rr(src, dst); }
    void mov32(int dst, int src) { rex(false, src, dst); u8(0x89); modrm_rr(src, dst); }
    // add 01, or 09, and 21, sub 29, xor 31, cmp 39, test 85: dst op= src
    void alu(int op, int dst, int src) { rex(false, src, dst); u8(op); modrm_rr(src, dst); }
    void call_abs(const void* fn) { mov_imm64(RAX, uint64_t(reinterpret_cast<uintptr_t>(fn))); u8(0xFF); u8(0xD0); }

    size_t jmp() { u8(0xE9); u32(0); return pos() - 4; }
    size_t jcc(int cc) { u8(0x0F); u8(0x80 | cc); u32(0); return pos() - 4; }
};

enum Cc { kO = 0x0, kC = 0x2, kZ = 0x4, kNZ = 0x5, kS = 0x8, kL = 0xC };

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
static_assert(offsetof(CoiCpu, c) == offsetof(CoiCpu, v) + 1, "V and C are stored together");
constexpr int32_t kOffSrHigh = int32_t(offsetof(CoiCpu, sr_high));
constexpr int32_t kOffUsp = int32_t(offsetof(CoiCpu, usp));

class Emitter {
public:
    Emitter(uint32_t temps, bool trace, const std::map<uint32_t, size_t>& slot_of, std::vector<CoiNativeFn>& slots)
        : frame((32 + 4 * temps + 15) & ~15u), trace(trace), slot_of(slot_of), slots(slots) {}

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
        // Entry: c in rbx, the frame; at != 0 resumes there.
        a.u8(0x53);                                         // push rbx
        a.u8(0x48); a.u8(0x81); a.u8(0xEC); a.u32(frame);  // sub rsp, frame
        a.mov64(RBX, kArg[0]);
        a.alu(0x85, kArg[1], kArg[1]);                      // test at, at
        const size_t to_entry = a.jcc(kZ);
        a.mov32(kArg[0], kArg[1]);
        a.call_abs(reinterpret_cast<const void*>(&jit::find));
        a.u8(0x48); a.u8(0x85); a.u8(0xC0);                 // test rax, rax
        const size_t unknown = a.jcc(kZ);
        a.u8(0xFF); a.u8(0xE0);                             // jmp rax
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

        // Exits: with 0 (stopped), or with eax.
        const size_t exit0 = a.pos();
        a.alu(0x31, RAX, RAX);                              // xor eax, eax
        const size_t exit = a.pos();
        a.u8(0x48); a.u8(0x81); a.u8(0xC4); a.u32(frame);  // add rsp, frame
        a.u8(0x5B);                                         // pop rbx
        a.u8(0xC3);                                         // ret
        for (size_t at : exit0_fix) a.patch(at, exit0);
        for (size_t at : exit_fix) a.patch(at, exit);
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
    std::vector<std::pair<size_t, uint32_t>> insn_fix;      // jumps to blocks
    std::vector<std::pair<size_t, uint32_t>> careful_fix;   // jumps to checked instructions

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
        a.load64(RAX, RBX, kOffLimit);
        a.u8(0x48); a.u8(0x85); a.u8(0xC0);                 // test rax, rax
        const size_t none = a.jcc(kZ);
        a.load64(RCX, RBX, kOffCycles);
        a.u8(0x48); a.u8(0x81); a.u8(0xC1); a.u32(most);   // add rcx, most
        a.u8(0x48); a.u8(0x39); a.u8(0xC1);                 // cmp rcx, rax
        const size_t fits = a.jcc(kL);
        careful_fix.push_back({a.jmp(), addr});
        a.patch(none, a.pos());
        a.patch(fits, a.pos());
    }
    std::vector<size_t> exit0_fix, exit_fix;
    // The temporary eax holds (-1: none known), so the next operation needn't load it again.
    // Forgotten at labels, calls, and after any operation that doesn't end by storing eax.
    int64_t in_rax = -1;
    bool stored = false;

    static int32_t slot(uint32_t t) { return 32 + 4 * int32_t(t); }

    void load(int r, const Val& v)
    {
        if (v.kind == Val::Temp && int64_t(v.n) == in_rax) {
            if (r != RAX)
                a.mov32(r, RAX);
            return;
        }
        if (v.kind == Val::Imm)
            a.mov_imm32(r, v.n);
        else if (v.kind == Val::Temp)
            a.load32(r, RSP, slot(v.n));
        if (r == RAX)
            in_rax = v.kind == Val::Temp ? int64_t(v.n) : -1;
    }
    void store(const IrInst& i)
    {
        a.store32(RAX, RSP, slot(i.dst));
        in_rax = i.dst;
        stored = true;
    }
    void call(const void* fn)
    {
        a.call_abs(fn);
        in_rax = -1;
    }
    void and_imm(int r, uint32_t m) { if (r & 8) a.u8(0x41); a.u8(0x81); a.u8(0xE0 | (r & 7)); a.u32(m); }

    // The port's bus, as Musashi calls it (the address to 24 bits, the value to its size).
    void bus_read(const IrInst& i)
    {
        load(kArg[0], i.a);
        and_imm(kArg[0], 0xFFFFFF);
        call(reinterpret_cast<const void*>(i.size == 1 ? &m68k_read_memory_8 : i.size == 2 ? &m68k_read_memory_16 : &m68k_read_memory_32));
        store(i);
    }
    void bus_write(const IrInst& i)
    {
        load(kArg[0], i.a);
        and_imm(kArg[0], 0xFFFFFF);
        load(kArg[1], i.b);
        if (i.size < 4)
            and_imm(kArg[1], i.size == 1 ? 0xFFu : 0xFFFFu);
        call(reinterpret_cast<const void*>(i.size == 1 ? &m68k_write_memory_8 : i.size == 2 ? &m68k_write_memory_16 : &m68k_write_memory_32));
    }

    // A runtime helper: c and up to three arguments; the result in eax.
    void helper(CoiHelper fn, Val x = {}, Val y = {}, Val z = {})
    {
        a.mov64(kArg[0], RBX);
        load(kArg[1], x);
        load(kArg[2], y);
        load(kArg[3], z);
        call(reinterpret_cast<const void*>(fn));
    }
    void helper_value(const IrInst& i, CoiHelper fn, Val x = {}, Val y = {}, Val z = {})
    {
        helper(fn, x, y, z);
        store(i);
    }

    void setcc(int cc, int32_t off) { a.u8(0x0F); a.u8(0x90 | cc); a.mem(0, RBX, off); }    // set<cc> byte [rbx+off]

    // eax op= ecx at the instruction's size (add 01, sub 29, test 85; the byte forms are one less,
    // except test's 84).
    void sized(int op, int size)
    {
        if (size == 2) a.u8(0x66);
        a.u8(size == 1 ? (op == 0x85 ? 0x84 : op - 1) : op);
        a.u8(0xC8);
    }
    void zero_extend(int size)                              // eax = the result's low `size` bytes
    {
        if (size == 1) { a.u8(0x0F); a.u8(0xB6); a.u8(0xC0); }
        else if (size == 2) { a.u8(0x0F); a.u8(0xB7); a.u8(0xC0); }
    }
    // add/sub (d op s, d in eax, s in ecx) with the 68000's N, Z, V, C (and X, unless cmp).
    void arith(const IrInst& i, int op, bool set_x)
    {
        const uint32_t need = needed(i);
        load(RAX, i.b);
        load(RCX, i.a);
        sized(op, i.size);
        if (need & jit::kFN) setcc(kS, kOffN);
        if (need & jit::kFZ) setcc(kZ, kOffZ);
        if (need & jit::kFV) setcc(kO, kOffV);
        if (need & jit::kFC) setcc(kC, kOffC);
        if (set_x && (need & jit::kFX)) setcc(kC, kOffX);
    }

    void exit_stopped(uint32_t address)                     // c->fail = address, return 0
    {
        a.u8(0xC7); a.mem(0, RBX, kOffFail); a.u32(address);
        exit0_fix.push_back(a.jmp());
    }

    // An instruction's code; `checked`: with its own budget check (a RAM stub's jmp keeps its
    // check either way).
    void block(const IrBlock& block, bool checked)
    {
        marks.push_back({a.pos(), block.address | (checked ? 0x80000000u : 0)});
        std::vector<size_t> label(block.labels, 0);
        std::vector<std::pair<size_t, uint32_t>> label_fix;
        in_rax = -1;                                            // an instruction is a jump target
        for (const IrInst& i : block.code) {
            const Val S = Val::imm(i.size);
            stored = false;
            switch (i.op) {
            case Ir::Mov: load(RAX, i.a); store(i); break;
            case Ir::GetReg: a.load32(RAX, RBX, off_reg(i.k)); store(i); break;
            case Ir::GetUsp: a.load32(RAX, RBX, kOffUsp); store(i); break;
            case Ir::Add: case Ir::Sub: case Ir::And: case Ir::Or: case Ir::Xor: {
                load(RAX, i.a);
                load(RCX, i.b);
                const int op = i.op == Ir::Add ? 0x01 : i.op == Ir::Sub ? 0x29 : i.op == Ir::And ? 0x21 : i.op == Ir::Or ? 0x09 : 0x31;
                a.alu(op, RAX, RCX);
                store(i);
                break;
            }
            case Ir::Shl: case Ir::Shr:
                load(RAX, i.a);
                load(RCX, i.b);
                a.u8(0xD3); a.u8(i.op == Ir::Shl ? 0xE0 : 0xE8);      // shl/shr eax, cl
                store(i);
                break;
            case Ir::Eq: case Ir::Ne: case Ir::Ltu:
                load(RAX, i.a);
                load(RCX, i.b);
                a.alu(0x39, RAX, RCX);                              // cmp eax, ecx
                a.u8(0x0F); a.u8(i.op == Ir::Eq ? 0x94 : i.op == Ir::Ne ? 0x95 : 0x92); a.u8(0xC0);   // setcc al
                a.u8(0x0F); a.u8(0xB6); a.u8(0xC0);                 // movzx eax, al
                store(i);
                break;
            case Ir::MulU16: case Ir::MulS16: {
                load(RAX, i.a);
                load(RCX, i.b);
                const int ext = i.op == Ir::MulU16 ? 0xB7 : 0xBF;   // movzx / movsx from 16 bits
                a.u8(0x0F); a.u8(ext); a.u8(0xC0);
                a.u8(0x0F); a.u8(ext); a.u8(0xC9);
                a.u8(0x0F); a.u8(0xAF); a.u8(0xC1);                 // imul eax, ecx
                store(i);
                break;
            }
            case Ir::Sext:
                load(RAX, i.a);
                a.u8(0x0F); a.u8(i.size == 1 ? 0xBE : 0xBF); a.u8(0xC0);   // movsx eax, al/ax
                store(i);
                break;
            case Ir::Read: bus_read(i); break;
            case Ir::GetSrHigh: a.u8(0x0F); a.u8(0xB7); a.mem(RAX, RBX, kOffSrHigh); store(i); break;
            case Ir::AddF: case Ir::SubF:
                arith(i, i.op == Ir::AddF ? 0x01 : 0x29, true);
                zero_extend(i.size);
                store(i);
                break;
            case Ir::AddxF: helper_value(i, coi_h_addx, S, i.a, i.b); break;
            case Ir::SubxF: helper_value(i, coi_h_subx, S, i.a, i.b); break;
            case Ir::LogicF:                                        // N, Z from the value; V, C clear
                load(RAX, i.a);
                if (i.size == 2) a.u8(0x66);
                a.u8(i.size == 1 ? 0x84 : 0x85); a.u8(0xC0);        // test at the size
                if (needed(i) & jit::kFN) setcc(kS, kOffN);
                if (needed(i) & jit::kFZ) setcc(kZ, kOffZ);
                if ((needed(i) & (jit::kFV | jit::kFC)) == (jit::kFV | jit::kFC)) {                    // V = C = 0
                    a.u8(0x66); a.u8(0xC7); a.mem(0, RBX, kOffV); a.u8(0); a.u8(0);
                } else if (needed(i) & jit::kFV) {
                    a.u8(0xC6); a.mem(0, RBX, kOffV); a.u8(0);
                } else if (needed(i) & jit::kFC) {
                    a.u8(0xC6); a.mem(0, RBX, kOffC); a.u8(0);
                }
                zero_extend(i.size);
                store(i);
                break;
            case Ir::ShiftF: helper_value(i, coi_h_shift, Val::imm(i.k | uint32_t(i.size) << 8), i.a, i.b); break;
            case Ir::Cond:
                if (i.k <= 1) {
                    a.mov_imm32(RAX, i.k == 0 ? 1 : 0);
                } else if (i.k >= 4 && i.k <= 11) {                 // one flag: cc cs ne eq vc vs pl mi
                    const int32_t flag = i.k <= 5 ? kOffC : i.k <= 7 ? kOffZ : i.k <= 9 ? kOffV : kOffN;
                    a.u8(0x0F); a.u8(0xB6); a.mem(RAX, RBX, flag);  // movzx eax, byte [flag]
                    if (!(i.k & 1)) { a.u8(0x83); a.u8(0xF0); a.u8(0x01); }   // xor eax, 1
                } else {
                    helper(coi_h_cond, Val::imm(i.k));
                }
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
            case Ir::SetReg: load(RAX, i.a); a.store32(RAX, RBX, off_reg(i.k)); break;
            case Ir::SetUsp: load(RAX, i.a); a.store32(RAX, RBX, kOffUsp); break;
            case Ir::SetRegPart:
                load(RAX, i.a);
                if (i.size == 4) a.store32(RAX, RBX, off_reg(i.k));
                else if (i.size == 2) { a.u8(0x66); a.u8(0x89); a.mem(RAX, RBX, off_reg(i.k)); }
                else { a.u8(0x88); a.mem(RAX, RBX, off_reg(i.k)); }
                break;
            case Ir::Write: bus_write(i); break;
            case Ir::CmpF: arith(i, 0x29, false); break;
            case Ir::SetCcr: helper(coi_h_set_ccr, i.a); break;
            case Ir::SetSrHigh: load(RAX, i.a); a.u8(0x66); a.u8(0x89); a.mem(RAX, RBX, kOffSrHigh); break;
            case Ir::SetZ:
                load(RAX, i.a);
                load(RCX, i.b);
                a.alu(0x85, RAX, RCX);                              // test eax, ecx
                a.u8(0x0F); a.u8(0x94); a.u8(0xC0);                 // setz al
                a.u8(0x88); a.mem(RAX, RBX, kOffZ);
                break;
            case Ir::Push32: helper(coi_h_push32, i.a); break;
            case Ir::Spin: helper(coi_h_spin, Val::imm(i.k)); break;
            case Ir::SetSr: helper(coi_h_set_sr, i.a); break;
            case Ir::Cycles:                                        // add qword [rbx+cycles], n
                a.u8(0x48); a.u8(0x81); a.mem(0, RBX, kOffCycles); a.u32(m68ki_cycles[0][i.k & 0xFFFF]);
                break;
            case Ir::AddCycles:
                load(RAX, i.a);
                a.u8(0x48); a.u8(0x63); a.u8(0xC0);                 // movsxd rax, eax
                a.u8(0x48); a.u8(0x01); a.mem(RAX, RBX, kOffCycles);
                break;
            case Ir::Idle: {                                    // as RT_IDLE
                if (trace)
                    break;
                const uint32_t branch = m68ki_cycles[0][i.k2 & 0xFFFF];
                const uint32_t turn = m68ki_cycles[0][i.k & 0xFFFF] + branch;
                load(RAX, i.a);
                a.alu(0x85, RAX, RAX);                          // test eax, eax
                const size_t not_taken = a.jcc(kZ);
                a.load64(RAX, RBX, kOffLimit);
                a.u8(0x48); a.u8(0x85); a.u8(0xC0);             // test rax, rax
                const size_t no_limit = a.jcc(kZ);
                a.load64(RCX, RBX, kOffCycles);
                a.u8(0x48); a.u8(0x29); a.u8(0xC8);             // sub rax, rcx
                a.u8(0x48); a.u8(0x05); a.u32(branch - 1);      // add rax, branch - 1
                a.u8(0x31); a.u8(0xD2);                         // xor edx, edx
                a.mov_imm32(RCX, turn);
                a.u8(0x48); a.u8(0xF7); a.u8(0xF1);             // div rcx
                a.u8(0x48); a.u8(0x0F); a.u8(0xAF); a.u8(0xC1); // imul rax, rcx
                a.u8(0x48); a.u8(0x85); a.u8(0xC0);             // test rax, rax
                const size_t none = a.jcc(kZ);
                a.u8(0x48); a.u8(0x01); a.mem(RAX, RBX, kOffCycles);
                helper(coi_h_logic, S, i.b);                    // the tst's flags
                a.patch(not_taken, a.pos());
                a.patch(no_limit, a.pos());
                a.patch(none, a.pos());
                break;
            }
            case Ir::Label: label[i.k] = a.pos(); break;
            case Ir::Br: label_fix.push_back({a.jmp(), i.k}); break;
            case Ir::BrIf: case Ir::BrIfNot:
                load(RAX, i.a);
                a.alu(0x85, RAX, RAX);
                label_fix.push_back({a.jcc(i.op == Ir::BrIf ? kNZ : kZ), i.k});
                break;
            case Ir::Goto: insn_fix.push_back({a.jmp(), i.k}); break;
            case Ir::GotoIf:
                load(RAX, i.a);
                a.alu(0x85, RAX, RAX);
                insn_fix.push_back({a.jcc(kNZ), i.k});
                break;
            case Ir::Trace: {
                a.u8(0xC7); a.mem(0, RBX, kOffPc); a.u32(i.k);      // c->pc = address
#ifdef COI_NATIVE_TRACE
                if (!trace)
                    break;
                a.mov_imm64(RAX, uint64_t(reinterpret_cast<uintptr_t>(&coi_native_tracing)));
                a.u8(0x83); a.u8(0x38); a.u8(0x00);                 // cmp dword [rax], 0
                const size_t skip = a.jcc(kZ);
                helper(coi_h_trace, Val::imm(i.k));
                a.patch(skip, a.pos());
#endif
                break;
            }
            case Ir::Limit: {
                if (!checked && i.k == block.address)
                    break;
                a.load64(RAX, RBX, kOffLimit);
                a.u8(0x48); a.u8(0x85); a.u8(0xC0);                 // test rax, rax
                const size_t none = a.jcc(kZ);
                a.u8(0x48); a.u8(0x39); a.mem(RAX, RBX, kOffCycles); // cmp [rbx+cycles], rax
                const size_t left = a.jcc(kL);
                exit_stopped(i.k);
                a.patch(none, a.pos());
                a.patch(left, a.pos());
                break;
            }
            case Ir::Stop:                                          // give the cycles back, stop
                a.u8(0x48); a.u8(0x81); a.mem(5, RBX, kOffCycles); a.u32(m68ki_cycles[0][i.k2 & 0xFFFF]);
                exit_stopped(i.k);
                break;
            case Ir::SetMissing: a.u8(0xC7); a.mem(0, RBX, kOffMissing); a.u32(i.k); break;
            case Ir::Return: load(RAX, i.a); exit_fix.push_back(a.jmp()); break;
            case Ir::JumpOut:
                a.u8(0xC6); a.mem(0, RBX, kOffJump); a.u8(1);
                a.mov_imm32(RAX, i.k);
                exit_fix.push_back(a.jmp());
                break;
            case Ir::JumpDyn:
                load(RAX, i.a);
                a.u8(0xC6); a.mem(0, RBX, kOffJump); a.u8(1);
                exit_fix.push_back(a.jmp());
                break;
            case Ir::Call: {
                a.mov64(kArg[0], RBX);
                a.mov_imm64(RAX, uint64_t(reinterpret_cast<uintptr_t>(&slots[slot_of.at(i.k)])));
                a.load64(kArg[1], RAX, 0);
                a.alu(0x31, kArg[2], kArg[2]);
                call(reinterpret_cast<const void*>(&jit::run));
                a.load32(RCX, RBX, kOffFail);
                a.alu(0x85, RCX, RCX);
                exit0_fix.push_back(a.jcc(kNZ));
                a.u8(0x3D); a.u32(i.k2);                            // cmp eax, ret
                const size_t ok = a.jcc(kZ);
                a.store32(RAX, RBX, kOffFail);                      // returned elsewhere: stop there
                exit0_fix.push_back(a.jmp());
                a.patch(ok, a.pos());
                break;
            }
            }
            if (!stored)
                in_rax = -1;
        }
        for (const auto& [at, l] : label_fix)
            a.patch(at, label[l]);
        in_rax = -1;
    }
};

}  // namespace

const CoiNativeCode* translate_x64(const Program& program, bool trace, std::string* error, uint32_t frame)
{
    return jit::translate<Emitter>(program, trace, error, frame);
}

}  // namespace recomp
