// Each 68000 instruction as the intermediate form: what it reads and writes, in the order the
// 68000 (as Musashi runs it) does, with the cycles Musashi counts.
#include "lower.h"

#include <optional>
#include <stdexcept>
#include <string>

namespace recomp {

std::optional<uint32_t> static_target(const Insn& in)
{
    switch (in.dst.mode) {
    case Mode::AbsW: case Mode::AbsL: return in.dst.value & 0xFFFFFF;
    case Mode::PcDisp: return (in.dst.value + uint32_t(in.dst.disp)) & 0xFFFFFF;
    default: return std::nullopt;
    }
}

namespace {

// The same target as the 68000's PC holds it: all 32 bits (a .w address is sign-extended, so a
// RAM stub called as $FFB4.w is at $FFFFFFB4), where the bus sees only the low 24.
uint32_t full_target(const Insn& in)
{
    return in.dst.mode == Mode::PcDisp ? in.dst.value + uint32_t(in.dst.disp) : in.dst.value;
}

}  // namespace

namespace {

uint32_t shift_kind(Op op)
{
    switch (op) {
    case Op::Asl: return 0; case Op::Asr: return 1; case Op::Lsl: return 2; case Op::Lsr: return 3;
    case Op::Rol: return 4; case Op::Ror: return 5; case Op::Roxl: return 6; default: return 7;
    }
}

uint32_t mask(int size) { return size == 1 ? 0xFFu : size == 2 ? 0xFFFFu : 0xFFFFFFFFu; }

bool is_memory(const Ea& e) { return e.mode != Mode::Dn && e.mode != Mode::An && e.mode != Mode::Imm; }

class Lowering {
public:
    Lowering(const LowerContext& context, const Insn& in) : cx(context), in(in) { b.address = in.address; }

    // A bne/beq branching back to a tst of work RAM just before it: a loop waiting for an
    // interrupt to change that byte, word or long. Translated code runs between interrupts and
    // the loop writes nothing, so every turn reads the same; the tst.
    std::optional<Insn> idle_test() const
    {
        if (in.target >= in.address || (in.cond != 6 && in.cond != 7))
            return std::nullopt;
        const Insn t = cx.insn_at(in.target);
        if (t.op != Op::Tst || t.address + uint32_t(t.length) != in.address)
            return std::nullopt;
        if (t.dst.mode != Mode::AbsW && t.dst.mode != Mode::AbsL)
            return std::nullopt;
        if ((t.dst.value & 0xFFFFFF) < 0xE00000)
            return std::nullopt;
        return t;
    }

    IrBlock run()
    {
        emit(Ir::Trace, {}, {}, 0, in.address);
        emit(Ir::Limit, {}, {}, 0, in.address);
        emit(Ir::Cycles, {}, {}, 0, in.opcode);
        body();
        return std::move(b);
    }

private:
    const LowerContext& cx;
    const Insn& in;
    IrBlock b;

    // --- building ---

    Val emit(Ir op, Val x = {}, Val y = {}, int size = 0, uint32_t k = 0, uint32_t k2 = 0)
    {
        IrInst i{op, uint8_t(size), 0, x, y, k, k2};
        Val result;
        if (yields(op)) {
            i.dst = b.temps++;
            result = Val::temp(i.dst);
        }
        b.code.push_back(i);
        return result;
    }
    static Val imm(uint32_t v) { return Val::imm(v); }
    uint32_t label() { return b.labels++; }
    void place(uint32_t l) { emit(Ir::Label, {}, {}, 0, l); }

    Val get(int r) { return emit(Ir::GetReg, {}, {}, 0, uint32_t(r)); }
    void set(int r, Val v) { emit(Ir::SetReg, v, {}, 0, uint32_t(r)); }
    Val add(Val x, Val y) { return emit(Ir::Add, x, y); }
    Val sub(Val x, Val y) { return emit(Ir::Sub, x, y); }
    Val and_(Val x, Val y) { return emit(Ir::And, x, y); }
    Val or_(Val x, Val y) { return emit(Ir::Or, x, y); }
    Val xor_(Val x, Val y) { return emit(Ir::Xor, x, y); }
    Val shl(Val x, Val y) { return emit(Ir::Shl, x, y); }
    Val shr(Val x, Val y) { return emit(Ir::Shr, x, y); }
    Val sext(int size, Val x) { return size == 4 ? x : emit(Ir::Sext, x, {}, size); }
    Val load(int size, Val addr) { return emit(Ir::Read, addr, {}, size); }
    void store(int size, Val addr, Val v) { emit(Ir::Write, addr, v, size); }
    Val logic(int size, Val v) { return emit(Ir::LogicF, v, {}, size); }
    Val cond(int c) { return emit(Ir::Cond, {}, {}, 0, uint32_t(c)); }
    void cycles(Val v) { emit(Ir::AddCycles, v); }

    // Stops before this instruction, for the interpreter to run it.
    void stop() { emit(Ir::Stop, {}, {}, 0, in.address, in.opcode); }

    // Before writing `sr` (a word) to the status register: stop unless it stays in supervisor
    // mode with tracing off and lets no waiting interrupt in (Musashi takes one at once when its
    // level is above the new mask; a non-maskable one, level 8 here, always).
    // A privileged instruction: in user mode, the privilege violation (8) instead, with the
    // instruction's own PC in its frame, as Musashi's.
    void privileged()
    {
        const uint32_t ok = label();
        emit(Ir::BrIf, and_(emit(Ir::GetSrHigh), imm(0x20)), {}, 0, ok);
        exception(8, in.address);
        place(ok);
    }

    // An exception, as Musashi takes it: its frame's PC is `pc` (the next instruction for trap,
    // trapv, chk and a divide by zero; the instruction's own for illegal, line A and line F).
    void exception(uint32_t vector, uint32_t pc)
    {
        emit(Ir::Return, emit(Ir::Exception, imm(pc), {}, 0, vector, in.opcode));
    }

    // After an SR write: an interrupt waiting above the new mask is taken at once, as Musashi's
    // m68ki_set_sr does, returning to `pc`; translated code goes on in the handler.
    void interrupt_if_due(Val sr, Val pc)
    {
        const Val due = emit(Ir::Ltu, and_(shr(sr, imm(8)), imm(7)), emit(Ir::IrqLevel));
        const uint32_t no = label();
        emit(Ir::BrIfNot, due, {}, 0, no);
        emit(Ir::Return, emit(Ir::Interrupt, pc));
        place(no);
    }

    // --- operands ---

    Val index(const Ea& e)
    {
        const Val r = get(e.index);
        return e.index_long ? r : sext(2, r);
    }

    // A memory operand's address, with any increment or decrement done.
    Val address(const Ea& e, int size)
    {
        const int an = 8 + e.reg;
        const uint32_t step = (size == 1 && e.reg == 7) ? 2 : uint32_t(size);
        switch (e.mode) {
        case Mode::Ind: return get(an);
        case Mode::PostInc: { const Val a = get(an); set(an, add(a, imm(step))); return a; }
        case Mode::PreDec: { const Val a = sub(get(an), imm(step)); set(an, a); return a; }
        case Mode::Disp: return add(get(an), imm(uint32_t(e.disp)));
        case Mode::Index: return add(add(get(an), imm(uint32_t(e.disp))), index(e));
        case Mode::AbsW: case Mode::AbsL: return imm(e.value);
        case Mode::PcDisp: return imm(e.value + uint32_t(e.disp));
        case Mode::PcIndex: return add(imm(e.value + uint32_t(e.disp)), index(e));
        default: throw std::runtime_error("no address for this operand");
        }
    }

    // An operand ready to read (and write): its address computed once if it's in memory.
    struct Operand {
        Ea ea;
        int size;
        Val addr;
    };

    Operand operand(const Ea& e, int size)
    {
        Operand op{e, size, {}};
        if (is_memory(e))
            op.addr = address(e, size);
        return op;
    }

    Val read(const Operand& op)
    {
        switch (op.ea.mode) {
        case Mode::Dn: return and_(get(op.ea.reg), imm(mask(op.size)));
        case Mode::An: return and_(get(8 + op.ea.reg), imm(mask(op.size)));
        case Mode::Imm: return imm(op.ea.value & mask(op.size));
        default: return load(op.size, op.addr);
        }
    }

    void write(const Operand& op, Val v)
    {
        switch (op.ea.mode) {
        case Mode::Dn: emit(Ir::SetRegPart, v, {}, op.size, uint32_t(op.ea.reg)); break;
        case Mode::An: set(8 + op.ea.reg, v); break;
        case Mode::Imm: throw std::runtime_error("write to an immediate");
        default: store(op.size, op.addr, v);
        }
    }

    // --- calls and jumps ---

    // Going through a RAM stub runs its jmp (xxx).l on the 68000: those cycles still count,
    // and as an instruction of its own the budget may run out before it (then this stops at
    // the stub, for the interpreter to run it when the CPU next gets cycles).
    // A RAM stub's jmp, an instruction of its own: the budget is checked before it, and if it
    // stops there, the PC is the stub's address as the call left it (`pc`, all 32 bits).
    // The stub must still be the jmp the boot code wrote (to the routine translated for it): the
    // game never rewrites one, but if anything has, this stops at the stub for the interpreter
    // (the nop's cycles added, then given back by the stop: the cycles used so far stand).
    void through_stub(uint32_t target, uint32_t pc)
    {
        if (cx.is_stub(target)) {
            emit(Ir::Limit, {}, {}, 0, pc);
            const uint32_t at = target & 0xFFFFFF, ok = label(), rewritten = label();
            emit(Ir::BrIfNot, emit(Ir::Eq, load(2, imm(at)), imm(0x4EF9)), {}, 0, rewritten);
            emit(Ir::BrIf, emit(Ir::Eq, load(4, imm((at + 2) & 0xFFFFFF)), imm(cx.resolve(target))), {}, 0, ok);
            place(rewritten);
            emit(Ir::Cycles, {}, {}, 0, 0x4E71);
            emit(Ir::Stop, {}, {}, 0, pc, 0x4E71);
            place(ok);
            emit(Ir::Cycles, {}, {}, 0, 0x4EF9);
        }
    }

    // Translated code runs at 24-bit addresses: the interpreter runs code reached at a PC with
    // its top byte set (it keeps those bits, and pushes them with any call), except a RAM stub,
    // whose jmp clears them.
    bool can_go(uint32_t target, uint32_t pc) const { return pc <= 0xFFFFFF || cx.is_stub(target); }

    // A call to `target` (`pc` as the 68000 holds it) returning to `ret`; if it isn't
    // translated, this stops at the call.
    void call(uint32_t target, uint32_t pc, uint32_t ret)
    {
        const uint32_t fn = cx.resolve(target);
        if (!cx.translated(fn) || !can_go(target, pc)) {
            emit(Ir::SetMissing, {}, {}, 0, fn);
            stop();
            return;
        }
        emit(Ir::Push32, imm(ret));
        through_stub(target, pc);
        emit(Ir::Call, {}, {}, 0, fn, ret);
    }

    // A jmp to `target` (`pc` as the 68000 holds it): out of this function.
    void jump(uint32_t target, uint32_t pc)
    {
        const uint32_t fn = cx.resolve(target);
        if (!cx.translated(fn) || !can_go(target, pc)) {
            emit(Ir::SetMissing, {}, {}, 0, fn);
            stop();
            return;
        }
        through_stub(target, pc);
        emit(Ir::JumpOut, {}, {}, 0, fn);
    }

    // A computed jsr/jmp's known targets, each as the PC could hold it: a work RAM address
    // also sign-extended from a word.
    static std::vector<std::pair<uint32_t, uint32_t>> as_pc(const std::vector<uint32_t>& targets)
    {
        std::vector<std::pair<uint32_t, uint32_t>> v;
        for (uint32_t t : targets) {
            v.push_back({t, t});
            if (t >= 0xFF8000 && t <= 0xFFFFFF)
                v.push_back({t, t | 0xFF000000});
        }
        return v;
    }

    // --- the instructions ---

    void body()
    {
        const int sz = in.size;
        const uint32_t next = in.address + uint32_t(in.length);
        // A branch to an odd address (an address error on a 68000; Musashi runs on from the odd
        // PC): not translated, the interpreter runs it (Program doesn't follow it either).
        if ((in.op == Op::Bra || in.op == Op::Bsr || in.op == Op::Bcc || in.op == Op::DBcc) && (in.target & 1)) {
            stop();
            return;
        }
        switch (in.op) {
        case Op::Move: {
            const Val v = read(operand(in.src, sz));
            const Operand d = operand(in.dst, sz);
            if (in.dst.mode == Mode::PreDec && sz == 4) {   // as the 68000: two words, the low one first
                store(2, add(d.addr, imm(2)), v);
                store(2, d.addr, shr(v, imm(16)));
            } else {
                write(d, v);
            }
            logic(sz, v);
            break;
        }
        case Op::Movea: {
            const Val v = read(operand(in.src, sz));
            set(8 + in.dst.reg, sz == 2 ? sext(2, v) : v);
            break;
        }
        case Op::Moveq:
            set(in.dst.reg, imm(in.src.value));
            logic(4, imm(in.src.value));
            break;
        case Op::Lea:
            set(8 + in.dst.reg, address(in.src, 4));
            break;
        case Op::Pea:
            emit(Ir::Push32, address(in.src, 4));
            break;
        case Op::Clr:
            write(operand(in.dst, sz), imm(0));
            logic(sz, imm(0));
            break;
        case Op::Add: case Op::Sub: case Op::And: case Op::Or: case Op::Eor: case Op::Cmp: {
            const Val s = read(operand(in.src, sz));
            const Operand d = operand(in.dst, sz);
            const Val dv = read(d);
            Val r;
            switch (in.op) {
            case Op::Add: r = emit(Ir::AddF, s, dv, sz); break;
            case Op::Sub: r = emit(Ir::SubF, s, dv, sz); break;
            case Op::And: r = logic(sz, and_(dv, s)); break;
            case Op::Or: r = logic(sz, or_(dv, s)); break;
            case Op::Eor: r = logic(sz, xor_(dv, s)); break;
            default: emit(Ir::CmpF, s, dv, sz); break;
            }
            if (in.op != Op::Cmp)
                write(d, r);
            break;
        }
        case Op::Addq: case Op::Subq: {
            if (in.dst.mode == Mode::An) {
                const int an = 8 + in.dst.reg;
                set(an, in.op == Op::Addq ? add(get(an), imm(in.src.value)) : sub(get(an), imm(in.src.value)));
                break;
            }
            const Operand d = operand(in.dst, sz);
            const Val dv = read(d);
            write(d, emit(in.op == Op::Addq ? Ir::AddF : Ir::SubF, imm(in.src.value), dv, sz));
            break;
        }
        case Op::Adda: case Op::Suba: case Op::Cmpa: {
            const Val s = sext(sz, read(operand(in.src, sz)));
            const int an = 8 + in.dst.reg;
            if (in.op == Op::Adda) set(an, add(get(an), s));
            else if (in.op == Op::Suba) set(an, sub(get(an), s));
            else emit(Ir::CmpF, s, get(an), 4);
            break;
        }
        case Op::Cmpm: {
            const Val s = read(operand(in.src, sz));
            emit(Ir::CmpF, s, read(operand(in.dst, sz)), sz);
            break;
        }
        case Op::Addx: case Op::Subx: {
            const Val s = read(operand(in.src, sz));
            const Operand d = operand(in.dst, sz);
            const Val dv = read(d);
            write(d, emit(in.op == Op::Addx ? Ir::AddxF : Ir::SubxF, s, dv, sz));
            break;
        }
        case Op::Neg: case Op::Negx: case Op::Not: case Op::Tst: {
            const Operand d = operand(in.dst, sz);
            const Val dv = read(d);
            if (in.op == Op::Tst) { logic(sz, dv); break; }
            Val r;
            if (in.op == Op::Neg) r = emit(Ir::SubF, dv, imm(0), sz);
            else if (in.op == Op::Negx) r = emit(Ir::SubxF, dv, imm(0), sz);
            else r = logic(sz, xor_(dv, imm(0xFFFFFFFFu)));
            write(d, r);
            break;
        }
        case Op::Tas: {
            const Operand d = operand(in.dst, 1);
            const Val dv = read(d);
            logic(1, dv);
            write(d, or_(dv, imm(0x80)));
            break;
        }
        case Op::Ext: {
            const int r = in.dst.reg;
            if (sz == 2) {
                const Val v = and_(sext(1, get(r)), imm(0xFFFF));
                emit(Ir::SetRegPart, v, {}, 2, uint32_t(r));
                logic(2, v);
            } else {
                const Val v = sext(2, get(r));
                set(r, v);
                logic(4, v);
            }
            break;
        }
        case Op::Swap: {
            const int r = in.dst.reg;
            const Val v = get(r);
            const Val s = or_(shl(v, imm(16)), shr(v, imm(16)));
            set(r, s);
            logic(4, s);
            break;
        }
        case Op::Exg: {
            const int x = in.src.mode == Mode::Dn ? in.src.reg : 8 + in.src.reg;
            const int y = in.dst.mode == Mode::Dn ? in.dst.reg : 8 + in.dst.reg;
            const Val vx = get(x);
            const Val vy = get(y);
            set(x, vy);
            set(y, vx);
            break;
        }
        case Op::Mulu: case Op::Muls: {
            const Val s = read(operand(in.src, 2));
            const int r = in.dst.reg;
            cycles(emit(in.op == Op::Mulu ? Ir::MuluCycles : Ir::MulsCycles, s));
            const Val p = emit(in.op == Op::Mulu ? Ir::MulU16 : Ir::MulS16, s, get(r));
            set(r, p);
            logic(4, p);
            break;
        }
        case Op::Divu: case Op::Divs: {
            // A divide by zero takes the exception (5), after the operand is read; the flags and
            // the register are left as they were.
            const Val s = read(operand(in.src, 2));
            const uint32_t done = label();
            emit(Ir::BrIf, emit(in.op == Op::Divu ? Ir::Divu : Ir::Divs, s, {}, 0, uint32_t(in.dst.reg)), {}, 0, done);
            exception(5, next);
            place(done);
            break;
        }
        case Op::Asl: case Op::Asr: case Op::Lsl: case Op::Lsr: case Op::Rol: case Op::Ror: case Op::Roxl: case Op::Roxr: {
            const Val count = in.src.mode == Mode::Imm ? imm(in.src.value) : and_(get(in.src.reg), imm(63));
            const Operand d = operand(in.dst, sz);
            if (in.dst.mode == Mode::Dn)
                cycles(shl(count, imm(1)));
            write(d, emit(Ir::ShiftF, read(d), count, sz, shift_kind(in.op)));
            break;
        }
        case Op::Btst: case Op::Bchg: case Op::Bclr: case Op::Bset: {
            const uint32_t bits = in.dst.mode == Mode::Dn ? 31 : 7;
            const Val bit = in.src.mode == Mode::Imm ? imm(in.src.value & bits) : and_(get(in.src.reg), imm(bits));
            const Operand d = operand(in.dst, sz);
            const Val dv = read(d);
            const Val m = shl(imm(1), bit);
            emit(Ir::SetZ, dv, m);
            if (in.op == Op::Bchg) write(d, xor_(dv, m));
            else if (in.op == Op::Bclr) write(d, and_(dv, xor_(m, imm(0xFFFFFFFFu))));
            else if (in.op == Op::Bset) write(d, or_(dv, m));
            break;
        }
        case Op::Scc: {
            const Operand d = operand(in.dst, 1);
            const Val t = cond(in.cond);
            write(d, and_(sub(imm(0), t), imm(0xFF)));
            if (in.dst.mode == Mode::Dn && in.cond >= 2)
                cycles(shl(t, imm(1)));
            break;
        }
        case Op::Movem: movem(); break;
        case Op::Link: {
            const int an = 8 + in.dst.reg;
            emit(Ir::Push32, get(an));
            set(an, get(15));
            set(15, add(get(15), imm(in.src.value)));
            break;
        }
        case Op::Unlk: {
            const int an = 8 + in.dst.reg;
            set(15, get(an));
            set(an, emit(Ir::Pop32));
            break;
        }
        case Op::MoveToCcr:
            emit(Ir::SetCcr, read(operand(in.src, 2)));
            break;
        case Op::MoveFromSr: {
            const Operand d = operand(in.dst, 2);
            write(d, or_(shl(emit(Ir::GetSrHigh), imm(8)), emit(Ir::GetCcr)));
            break;
        }
        case Op::MoveToUsp: privileged(); emit(Ir::SetUsp, get(8 + in.src.reg)); break;
        case Op::MoveFromUsp: privileged(); set(8 + in.dst.reg, emit(Ir::GetUsp)); break;
        case Op::Rtr: {
            // As Musashi's: the CCR (a word, its low byte) and then the PC from the stack.
            const Val sp = get(15);
            const Val ccr = load(2, sp);
            const Val pc = load(4, add(sp, imm(2)));
            set(15, add(sp, imm(6)));
            emit(Ir::SetCcr, ccr);
            emit(Ir::Return, pc);
            break;
        }
        case Op::Reset:
            // The reset line pulsed (nothing in the port listens: Musashi's M68K_EMULATE_RESET
            // is off), for Musashi's CYC_RESET (132 on the 68000) on top of its own cycles.
            privileged();
            cycles(imm(132));
            break;
        case Op::MoveToSr: case Op::AndiSr: case Op::OriSr: case Op::EoriSr: {
            // Privileged (the operand isn't read in user mode). The mode may change (the stack
            // pointers swapped); a waiting interrupt it lets in is taken at once.
            privileged();
            Val v;
            if (in.op == Op::MoveToSr && in.src.mode == Mode::Imm) v = imm(in.src.value & 0xFFFF);
            else if (in.op == Op::MoveToSr) v = and_(read(operand(in.src, 2)), imm(0xFFFF));
            else {
                const Val sr = or_(shl(emit(Ir::GetSrHigh), imm(8)), emit(Ir::GetCcr));
                v = in.op == Op::AndiSr ? and_(sr, imm(in.src.value))
                    : in.op == Op::OriSr ? or_(sr, imm(in.src.value)) : xor_(sr, imm(in.src.value));
            }
            emit(Ir::SetSr, v);
            interrupt_if_due(v, imm(next));
            break;
        }
        case Op::Trap: exception(32 + in.src.value, next); break;
        case Op::Trapv: {
            const uint32_t done = label();
            emit(Ir::BrIfNot, cond(9), {}, 0, done);           // V set
            exception(7, next);
            place(done);
            break;
        }
        case Op::Illegal: exception(4, in.address); break;
        case Op::LineA: exception(10, in.address); break;
        case Op::LineF: exception(11, in.address); break;
        case Op::Chk: {
            // As Musashi's chk.w: Z of Dn, V and C cleared; below 0 or above the bound (signed
            // words), N set if below 0 (clear otherwise) and the exception (6) taken.
            const Val bound = emit(Ir::Sext, read(operand(in.src, 2)), {}, 2);
            const Val dn = emit(Ir::Sext, get(in.dst.reg), {}, 2);
            const Val z = emit(Ir::Eq, and_(dn, imm(0xFFFF)), imm(0));
            const Val ccr = or_(and_(emit(Ir::GetCcr), imm(0x18)), shl(z, imm(2)));
            emit(Ir::SetCcr, ccr);
            const Val neg = shr(dn, imm(31));
            const Val above = emit(Ir::Ltu, xor_(bound, imm(0x80000000)), xor_(dn, imm(0x80000000)));
            const uint32_t done = label();
            emit(Ir::BrIfNot, or_(neg, above), {}, 0, done);
            emit(Ir::SetCcr, or_(and_(ccr, imm(0x17)), shl(neg, imm(3))));
            exception(6, next);
            place(done);
            break;
        }
        case Op::Stop: {
            // Privileged. The SR as move to sr sets it, then the CPU waits: an interrupt it lets in
            // is taken at once; the slice ends either way.
            privileged();
            const Val v = imm(in.src.value & 0xFFFF);
            emit(Ir::Return, emit(Ir::StopCpu, v, imm(next)));
            break;
        }
        case Op::AndiCcr: emit(Ir::SetCcr, and_(emit(Ir::GetCcr), imm(in.src.value))); break;
        case Op::OriCcr: emit(Ir::SetCcr, or_(emit(Ir::GetCcr), imm(in.src.value))); break;
        case Op::EoriCcr: emit(Ir::SetCcr, xor_(emit(Ir::GetCcr), imm(in.src.value))); break;
        case Op::Nop: break;
        case Op::Bra:
            if (in.target == in.address)
                emit(Ir::Spin, {}, {}, 0, in.opcode);
            emit(Ir::Goto, {}, {}, 0, in.target);
            break;
        case Op::Bcc: {
            const Val taken = cond(in.cond);
            if (const auto tst = idle_test()) {
                // The flags may be from before an interrupt (which returned here): what the
                // tst reads now decides whether the turns after this one loop again.
                const int sz = tst->size;
                const Val v = and_(load(sz, imm(tst->dst.value)), imm(sz == 1 ? 0xFFu : sz == 2 ? 0xFFFFu : 0xFFFFFFFFu));
                const Val again = emit(in.cond == 6 ? Ir::Ne : Ir::Eq, v, imm(0));
                emit(Ir::Idle, and_(taken, again), v, sz, tst->opcode, in.opcode);
            }
            emit(Ir::GotoIf, taken, {}, 0, in.target);
            cycles(imm(in.length == 2 ? uint32_t(-2) : 2u));     // not taken
            break;
        }
        case Op::DBcc: {
            const int r = in.dst.reg;
            const uint32_t end = label(), expired = label();
            emit(Ir::BrIf, cond(in.cond), {}, 0, end);
            const Val w = and_(sub(get(r), imm(1)), imm(0xFFFF));
            emit(Ir::SetRegPart, w, {}, 2, uint32_t(r));
            emit(Ir::BrIfNot, emit(Ir::Ne, w, imm(0xFFFF)), {}, 0, expired);
            cycles(imm(uint32_t(-2)));
            emit(Ir::Goto, {}, {}, 0, in.target);
            place(expired);
            cycles(imm(2));
            place(end);
            break;
        }
        case Op::Bsr: call(in.target, in.target, next); break;
        case Op::Jsr: {
            if (auto t = static_target(in)) { call(*t, full_target(in), next); break; }
            const Val t = address(in.dst, 4);
            const uint32_t done = label();
            for (const auto& [target, pc] : as_pc(cx.call_targets(in.address))) {
                const uint32_t other = label();
                emit(Ir::BrIfNot, emit(Ir::Eq, t, imm(pc)), {}, 0, other);
                call(target, pc, next);
                emit(Ir::Br, {}, {}, 0, done);
                place(other);
            }
            // Any other target: the call made, then a jump there (translated on demand if
            // there's no code for it yet); its rts returns to whoever runs it.
            emit(Ir::Push32, imm(next));
            emit(Ir::JumpDyn, t);
            place(done);
            break;
        }
        case Op::Jmp: {
            if (auto t = static_target(in)) {
                if (*t == in.address)
                    emit(Ir::Spin, {}, {}, 0, in.opcode);
                jump(*t, full_target(in));
                break;
            }
            const Val t = address(in.dst, 4);
            for (const auto& [target, pc] : as_pc(cx.jump_targets(in.address))) {
                const uint32_t other = label();
                emit(Ir::BrIfNot, emit(Ir::Eq, t, imm(pc)), {}, 0, other);
                jump(target, pc);
                place(other);
            }
            emit(Ir::JumpDyn, t);                  // any other target (on demand)
            break;
        }
        case Op::Rts: emit(Ir::Return, emit(Ir::Pop32)); break;     // (all 32 bits, as the PC holds them)
        case Op::Rte: {
            // As Musashi's 68000 rte: the SR, then the PC, from the stack; then the jump and the
            // SR, with the interrupt check (an interrupt it lets in returns to the popped PC).
            privileged();
            const Val sp = get(15);
            const Val sr = load(2, sp);
            const Val pc = load(4, add(sp, imm(2)));
            set(15, add(sp, imm(6)));
            emit(Ir::SetSr, sr);
            interrupt_if_due(sr, pc);
            emit(Ir::Return, pc);
            break;
        }
        default:
            stop();     // instructions translated code doesn't run (yet): the interpreter does
        }
    }

    void movem()
    {
        const int sz = in.size;
        const Ea& e = in.to_memory ? in.dst : in.src;
        const int an = 8 + e.reg;
        int count = 0;
        for (int i = 0; i < 16; i++)
            count += (in.reglist >> i) & 1;
        cycles(imm(uint32_t(count * (sz == 4 ? 8 : 4))));
        if (in.to_memory && e.mode == Mode::PreDec) {
            // As the 68000 (and Musashi) does it: a long register goes as two word writes, the
            // low word first.
            Val p = get(an);
            for (int i = 15; i >= 0; i--)
                if (in.reglist & (1 << i)) {
                    if (sz == 4) {
                        p = sub(p, imm(4));
                        const Val r = get(i);
                        store(2, add(p, imm(2)), r);
                        store(2, p, shr(r, imm(16)));
                    } else {
                        p = sub(p, imm(2));
                        store(2, p, get(i));
                    }
                }
            set(an, p);
        } else if (in.to_memory) {
            Val p = address(e, sz);
            for (int i = 0; i < 16; i++)
                if (in.reglist & (1 << i)) {
                    store(sz, p, get(i));
                    p = add(p, imm(uint32_t(sz)));
                }
        } else {
            Val p = e.mode == Mode::PostInc ? get(an) : address(e, sz);
            for (int i = 0; i < 16; i++)
                if (in.reglist & (1 << i)) {
                    set(i, sext(sz, load(sz, p)));
                    p = add(p, imm(uint32_t(sz)));
                }
            if (e.mode == Mode::PostInc)
                set(an, p);
        }
    }
};

}  // namespace

IrBlock lower(const LowerContext& context, const Insn& in)
{
    return Lowering(context, in).run();
}

bool ends_block(const Insn& in)
{
    if ((in.op == Op::Bra || in.op == Op::Bsr || in.op == Op::Bcc || in.op == Op::DBcc) && (in.target & 1))
        return true;                        // (a branch to an odd address: stopped before)
    switch (in.op) {
    case Op::Bra: case Op::Jmp: case Op::Rts: case Op::Rte: case Op::Rtr: case Op::Illegal: case Op::Stop: case Op::Trap:
    case Op::LineA: case Op::LineF:
        return true;
    default:
        return false;
    }
}

}  // namespace recomp
