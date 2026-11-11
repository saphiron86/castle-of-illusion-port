#include "decode.h"

#include <cstdio>

namespace recomp {

namespace {

// Reads an operand's extension words, advancing `pos` past them.
struct Reader {
    const Rom& rom;
    uint32_t pos;
    uint16_t word() { uint16_t w = rom.word(pos); pos += 2; return w; }
    uint32_t lng() { uint32_t l = rom.lng(pos); pos += 4; return l; }
};

// mode/reg fields to an operand; false if the combination doesn't exist.
bool read_ea(Reader& r, int mode, int reg, int size, Ea& ea)
{
    ea = Ea{};
    ea.reg = reg;
    switch (mode) {
    case 0: ea.mode = Mode::Dn; return true;
    case 1: ea.mode = Mode::An; return true;
    case 2: ea.mode = Mode::Ind; return true;
    case 3: ea.mode = Mode::PostInc; return true;
    case 4: ea.mode = Mode::PreDec; return true;
    case 5: ea.mode = Mode::Disp; ea.disp = int16_t(r.word()); return true;
    case 6: {
        ea.mode = Mode::Index;
        const uint16_t ext = r.word();
        ea.index = ext >> 12;
        ea.index_long = ext & 0x0800;
        ea.disp = int8_t(ext & 0xFF);
        return !(ext & 0x0700);
    }
    case 7:
        switch (reg) {
        case 0: ea.mode = Mode::AbsW; ea.value = uint32_t(int32_t(int16_t(r.word()))); return true;
        case 1: ea.mode = Mode::AbsL; ea.value = r.lng(); return true;
        case 2: ea.mode = Mode::PcDisp; ea.value = r.pos; ea.disp = int16_t(r.word()); return true;
        case 3: {
            ea.mode = Mode::PcIndex;
            ea.value = r.pos;
            const uint16_t ext = r.word();
            ea.index = ext >> 12;
            ea.index_long = ext & 0x0800;
            ea.disp = int8_t(ext & 0xFF);
            return !(ext & 0x0700);
        }
        case 4:
            ea.mode = Mode::Imm;
            if (size == 4)
                ea.value = r.lng();
            else if (size == 2)
                ea.value = r.word();
            else
                ea.value = r.word() & 0xFF;
            return true;
        }
    }
    return false;
}

int size_bits(int bits) { return bits == 0 ? 1 : bits == 1 ? 2 : bits == 2 ? 4 : 0; }

Ea dreg(int n) { Ea e; e.mode = Mode::Dn; e.reg = n; return e; }
Ea areg(int n) { Ea e; e.mode = Mode::An; e.reg = n; return e; }
Ea imm(uint32_t v) { Ea e; e.mode = Mode::Imm; e.value = v; return e; }

const uint8_t* g_opcode_cycles = nullptr;

}  // namespace

void use_opcode_table(const uint8_t* cycles)
{
    g_opcode_cycles = cycles;
}

Insn decode(const Rom& rom, uint32_t address)
{
    Insn in;
    in.address = address;
    Reader r{rom, address};
    const uint16_t op = r.word();
    in.opcode = op;
    const int mode = (op >> 3) & 7, reg = op & 7, reg9 = (op >> 9) & 7;
    auto unknown = [&] { in.op = Op::Unknown; in.length = 2; return in; };
    auto done = [&] { in.length = int(r.pos - address); return in; };
    auto ea_dst = [&](int size) { return read_ea(r, mode, reg, size, in.dst); };
    auto ea_src = [&](int size) { return read_ea(r, mode, reg, size, in.src); };

    if ((op >> 12) == 0xA) { in.op = Op::LineA; return done(); }   // the line A and line F exceptions
    if ((op >> 12) == 0xF) { in.op = Op::LineF; return done(); }
    // (reset has no cycles in the table: Musashi counts them as it runs it.)
    if (g_opcode_cycles && !g_opcode_cycles[op] && op != 0x4E70) { in.op = Op::Illegal; return done(); }
    switch (op >> 12) {
    case 0x0: {
        if (op & 0x0100) {                          // dynamic bit operations (movep not handled)
            if (mode == 1)
                return unknown();
            static const Op ops[] = {Op::Btst, Op::Bchg, Op::Bclr, Op::Bset};
            in.op = ops[(op >> 6) & 3];
            in.src = dreg(reg9);
            in.size = mode == 0 ? 4 : 1;
            if (!ea_dst(in.size))
                return unknown();
            return done();
        }
        if (reg9 == 4) {                            // static bit operations
            static const Op ops[] = {Op::Btst, Op::Bchg, Op::Bclr, Op::Bset};
            in.op = ops[(op >> 6) & 3];
            in.src = imm(r.word() & 0xFF);
            in.size = mode == 0 ? 4 : 1;
            if (!ea_dst(in.size))
                return unknown();
            return done();
        }
        const int size = size_bits((op >> 6) & 3);
        if (!size)
            return unknown();
        if ((op & 0xFF) == 0x3C || (op & 0xFF) == 0x7C) {  // to CCR / SR
            const bool sr = (op & 0xFF) == 0x7C;
            const uint16_t v = r.word();
            in.src = imm(sr ? v : v & 0xFF);
            switch (reg9) {
            case 0: in.op = sr ? Op::OriSr : Op::OriCcr; break;
            case 1: in.op = sr ? Op::AndiSr : Op::AndiCcr; break;
            case 5: in.op = sr ? Op::EoriSr : Op::EoriCcr; break;
            default: return unknown();
            }
            return done();
        }
        static const Op ops[] = {Op::Or, Op::And, Op::Sub, Op::Add, Op::Unknown, Op::Eor, Op::Cmp, Op::Unknown};
        in.op = ops[reg9];
        if (in.op == Op::Unknown)
            return unknown();
        in.size = size;
        read_ea(r, 7, 4, size, in.src);
        if (!ea_dst(size) || in.dst.mode == Mode::An)
            return unknown();
        return done();
    }
    case 0x1: case 0x2: case 0x3: {
        const int size = (op >> 12) == 1 ? 1 : (op >> 12) == 3 ? 2 : 4;
        in.size = size;
        if (!ea_src(size))
            return unknown();
        const int dmode = (op >> 6) & 7;
        if (!read_ea(r, dmode, reg9, size, in.dst))
            return unknown();
        in.op = dmode == 1 ? Op::Movea : Op::Move;
        if (in.op == Op::Movea && size == 1)
            return unknown();
        return done();
    }
    case 0x4: {
        if ((op & 0xF1C0) == 0x41C0) {
            in.op = Op::Lea;
            in.size = 4;
            if (!ea_src(4))
                return unknown();
            in.dst = areg(reg9);
            return done();
        }
        if ((op & 0xF1C0) == 0x4180) {              // chk.w <ea>,Dn
            in.op = Op::Chk;
            in.size = 2;
            if (!ea_src(2))
                return unknown();
            in.dst = dreg(reg9);
            return done();
        }
        switch (op & 0xFFC0) {
        case 0x40C0: in.op = Op::MoveFromSr; in.size = 2; if (!ea_dst(2)) return unknown(); return done();
        case 0x44C0: in.op = Op::MoveToCcr; in.size = 2; if (!ea_src(2)) return unknown(); return done();
        case 0x46C0: in.op = Op::MoveToSr; in.size = 2; if (!ea_src(2)) return unknown(); return done();
        case 0x4800: in.op = Op::Nbcd; in.size = 1; if (!ea_dst(1)) return unknown(); return done();
        case 0x4AC0:
            if (op == 0x4AFC) { in.op = Op::Illegal; return done(); }
            in.op = Op::Tas; in.size = 1; if (!ea_dst(1)) return unknown(); return done();
        case 0x4E80: in.op = Op::Jsr; if (!ea_dst(4)) return unknown(); return done();
        case 0x4EC0: in.op = Op::Jmp; if (!ea_dst(4)) return unknown(); return done();
        }
        if ((op & 0xFFF8) == 0x4840) { in.op = Op::Swap; in.size = 4; in.dst = dreg(reg); return done(); }
        if ((op & 0xFFC0) == 0x4840) { in.op = Op::Pea; in.size = 4; if (!ea_src(4)) return unknown(); return done(); }
        if ((op & 0xFFF8) == 0x4880) { in.op = Op::Ext; in.size = 2; in.dst = dreg(reg); return done(); }
        if ((op & 0xFFF8) == 0x48C0) { in.op = Op::Ext; in.size = 4; in.dst = dreg(reg); return done(); }
        if ((op & 0xFB80) == 0x4880) {              // movem
            in.op = Op::Movem;
            in.size = (op & 0x40) ? 4 : 2;
            in.to_memory = !(op & 0x0400);
            uint16_t mask = r.word();
            if (in.to_memory && mode == 4) {        // -(An): mask is a7..d0 from bit 0
                uint16_t rev = 0;
                for (int i = 0; i < 16; i++)
                    if (mask & (1 << i))
                        rev |= uint16_t(1 << (15 - i));
                mask = rev;
            }
            in.reglist = mask;
            if (!read_ea(r, mode, reg, in.size, in.to_memory ? in.dst : in.src))
                return unknown();
            return done();
        }
        if ((op & 0xFF00) == 0x4000 || (op & 0xFF00) == 0x4200 || (op & 0xFF00) == 0x4400
            || (op & 0xFF00) == 0x4600 || (op & 0xFF00) == 0x4A00) {
            const int size = size_bits((op >> 6) & 3);
            if (!size)
                return unknown();
            in.size = size;
            switch (op & 0xFF00) {
            case 0x4000: in.op = Op::Negx; break;
            case 0x4200: in.op = Op::Clr; break;
            case 0x4400: in.op = Op::Neg; break;
            case 0x4600: in.op = Op::Not; break;
            default: in.op = Op::Tst; break;
            }
            if (!ea_dst(size))
                return unknown();
            return done();
        }
        if ((op & 0xFFF0) == 0x4E40) { in.op = Op::Trap; in.src = imm(op & 15); return done(); }
        if ((op & 0xFFF8) == 0x4E50) { in.op = Op::Link; in.dst = areg(reg); in.src = imm(uint32_t(int32_t(int16_t(r.word())))); return done(); }
        if ((op & 0xFFF8) == 0x4E60) { in.op = Op::MoveToUsp; in.size = 4; in.src = areg(reg); return done(); }
        if ((op & 0xFFF8) == 0x4E68) { in.op = Op::MoveFromUsp; in.size = 4; in.dst = areg(reg); return done(); }
        if ((op & 0xFFF8) == 0x4E58) { in.op = Op::Unlk; in.dst = areg(reg); return done(); }
        switch (op) {
        case 0x4E70: in.op = Op::Reset; return done();
        case 0x4E71: in.op = Op::Nop; return done();
        case 0x4E72: in.op = Op::Stop; in.src = imm(r.word()); return done();
        case 0x4E73: in.op = Op::Rte; return done();
        case 0x4E75: in.op = Op::Rts; return done();
        case 0x4E76: in.op = Op::Trapv; return done();
        case 0x4E77: in.op = Op::Rtr; return done();
        }
        return unknown();
    }
    case 0x5: {
        const int bits = (op >> 6) & 3;
        if (bits == 3) {
            in.cond = (op >> 8) & 15;
            if (mode == 1) {
                in.op = Op::DBcc;
                in.dst = dreg(reg);
                const uint32_t base = r.pos;        // relative to the displacement word
                in.target = base + uint32_t(int32_t(int16_t(r.word())));
                return done();
            }
            in.op = Op::Scc;
            in.size = 1;
            if (!ea_dst(1))
                return unknown();
            return done();
        }
        in.size = size_bits(bits);
        in.op = (op & 0x0100) ? Op::Subq : Op::Addq;
        in.src = imm(reg9 ? reg9 : 8);
        if (!ea_dst(in.size) || (in.size == 1 && in.dst.mode == Mode::An))
            return unknown();
        return done();
    }
    case 0x6: {
        const int c = (op >> 8) & 15;
        int32_t disp = int8_t(op & 0xFF);
        const uint32_t base = r.pos;
        if (disp == 0)
            disp = int16_t(r.word());
        else if (disp == -1)
            return unknown();                       // 68020 32-bit displacement
        in.target = base + uint32_t(disp);
        in.op = c == 0 ? Op::Bra : c == 1 ? Op::Bsr : Op::Bcc;
        in.cond = c;
        return done();
    }
    case 0x7:
        if (op & 0x0100)
            return unknown();
        in.op = Op::Moveq;
        in.size = 4;
        in.src = imm(uint32_t(int32_t(int8_t(op & 0xFF))));
        in.dst = dreg(reg9);
        return done();
    case 0x8: case 0x9: case 0xB: case 0xC: case 0xD: {
        const int hi = op >> 12;
        const int opmode = (op >> 6) & 7;
        if (opmode == 3 || opmode == 7) {
            in.size = (hi == 0x8 || hi == 0xC) ? 2 : (opmode == 3 ? 2 : 4);
            switch (hi) {
            case 0x8: in.op = opmode == 3 ? Op::Divu : Op::Divs; break;
            case 0xC: in.op = opmode == 3 ? Op::Mulu : Op::Muls; break;
            case 0x9: in.op = Op::Suba; break;
            case 0xB: in.op = Op::Cmpa; break;
            case 0xD: in.op = Op::Adda; break;
            }
            if (!ea_src(in.size))
                return unknown();
            in.dst = (hi == 0x8 || hi == 0xC) ? dreg(reg9) : areg(reg9);
            return done();
        }
        in.size = size_bits(opmode & 3);
        const bool to_ea = opmode & 4;
        if (to_ea && (mode == 0 || mode == 1)) {    // the register-register special forms
            if (hi == 0xC && (opmode == 5 || opmode == 6)) {   // exg
                in.op = Op::Exg;
                in.size = 4;
                const int m = (op >> 3) & 0x1F;
                if (m == 0x08) { in.src = dreg(reg9); in.dst = dreg(reg); }
                else if (m == 0x09) { in.src = areg(reg9); in.dst = areg(reg); }
                else if (m == 0x11) { in.src = dreg(reg9); in.dst = areg(reg); }
                else return unknown();
                return done();
            }
            if (hi == 0xB && mode == 1) {           // cmpm (Ay)+,(Ax)+
                in.op = Op::Cmpm;
                in.src.mode = Mode::PostInc; in.src.reg = reg;
                in.dst.mode = Mode::PostInc; in.dst.reg = reg9;
                return done();
            }
            if (hi == 0xB) {                        // eor Dn,Dn
                in.op = Op::Eor;
                in.src = dreg(reg9);
                in.dst = dreg(reg);
                return done();
            }
            // addx/subx/abcd/sbcd: Dy,Dx or -(Ay),-(Ax)
            switch (hi) {
            case 0x8: in.op = Op::Sbcd; in.size = 1; break;
            case 0xC: in.op = Op::Abcd; in.size = 1; break;
            case 0x9: in.op = Op::Subx; break;
            case 0xD: in.op = Op::Addx; break;
            }
            if (mode == 0) { in.src = dreg(reg); in.dst = dreg(reg9); }
            else {
                in.src.mode = Mode::PreDec; in.src.reg = reg;
                in.dst.mode = Mode::PreDec; in.dst.reg = reg9;
            }
            return done();
        }
        switch (hi) {
        case 0x8: in.op = Op::Or; break;
        case 0x9: in.op = Op::Sub; break;
        case 0xB: in.op = to_ea ? Op::Eor : Op::Cmp; break;
        case 0xC: in.op = Op::And; break;
        case 0xD: in.op = Op::Add; break;
        }
        if (to_ea) {
            in.src = dreg(reg9);
            if (!ea_dst(in.size))
                return unknown();
        } else {
            if (!ea_src(in.size))
                return unknown();
            in.dst = dreg(reg9);
        }
        return done();
    }
    case 0xE: {
        static const Op left[] = {Op::Asl, Op::Lsl, Op::Roxl, Op::Rol};
        static const Op right[] = {Op::Asr, Op::Lsr, Op::Roxr, Op::Ror};
        const bool to_left = op & 0x0100;
        if (((op >> 6) & 3) == 3) {                 // memory, by one
            const int type = (op >> 9) & 3;
            if (op & 0x0800)
                return unknown();
            in.op = to_left ? left[type] : right[type];
            in.size = 2;
            in.src = imm(1);
            if (!ea_dst(2))
                return unknown();
            return done();
        }
        const int type = (op >> 3) & 3;
        in.op = to_left ? left[type] : right[type];
        in.size = size_bits((op >> 6) & 3);
        in.src = (op & 0x20) ? dreg(reg9) : imm(reg9 ? reg9 : 8);
        in.dst = dreg(reg);
        return done();
    }
    }
    return unknown();
}

std::string describe(const Insn& in)
{
    char buf[64];
    std::snprintf(buf, sizeof(buf), "$%06X: op %d size %d (opcode $%04X)", in.address, int(in.op), in.size, in.opcode);
    return buf;
}

}  // namespace recomp
