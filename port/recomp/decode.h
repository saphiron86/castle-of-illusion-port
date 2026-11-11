// A 68000 instruction decoder for the recompiler: what each instruction does, its size and
// its operands, worked out from the opcode bits (Capstone's 68000 details are not exact
// enough to translate from). Shared by every back end.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace recomp {

// Addressing modes, as the 68000 encodes them (mode 7 split by register).
enum class Mode : uint8_t {
    None, Dn, An, Ind, PostInc, PreDec, Disp, Index, AbsW, AbsL, PcDisp, PcIndex, Imm,
};

struct Ea {
    Mode mode = Mode::None;
    int reg = 0;            // Dn/An number (An modes: the address register)
    int32_t disp = 0;       // Disp/Index/PcDisp/PcIndex displacement
    int index = 0;          // Index/PcIndex: 0-7 = d0-d7, 8-15 = a0-a7
    bool index_long = false;
    uint32_t value = 0;     // AbsW/AbsL: the address (AbsW sign-extended); Imm: the value;
                            // PcDisp/PcIndex: the base (address of the extension word)
};

enum class Op : uint8_t {
    Unknown,
    // data movement
    Move, Movea, Moveq, Movem, Lea, Pea, Exg, Swap, Clr, Link, Unlk,
    // arithmetic and logic: dst = dst op src
    Add, Adda, Addq, Addx, Sub, Suba, Subq, Subx, Cmp, Cmpa, Cmpm, And, Or, Eor,
    Neg, Negx, Not, Ext, Tst, Tas, Mulu, Muls, Divu, Divs, Abcd, Sbcd, Nbcd,
    // shifts and rotates (count in src: Imm or Dn; memory forms shift by one)
    Asl, Asr, Lsl, Lsr, Rol, Ror, Roxl, Roxr,
    // bits (bit number in src)
    Btst, Bchg, Bclr, Bset,
    // flow
    Bra, Bsr, Bcc, DBcc, Scc, Jmp, Jsr, Rts, Rte, Rtr, Trap, Nop,
    // status register
    AndiCcr, OriCcr, EoriCcr, AndiSr, OriSr, EoriSr, MoveToCcr, MoveToSr, MoveFromSr,
    MoveToUsp, MoveFromUsp,     // move An,usp / move usp,An (src / dst: the An)
    Stop, Reset, Illegal,
    Trapv, Chk, LineA, LineF,   // chk.w: bound in src, Dn in dst
};

struct Insn {
    uint32_t address = 0;
    int length = 2;         // bytes, with extension words
    Op op = Op::Unknown;
    int size = 0;           // 1, 2 or 4 (0: none)
    Ea src, dst;            // single-operand instructions use dst
    int cond = 0;           // Bcc/DBcc/Scc condition (0 = true ... 15 = le)
    uint32_t target = 0;    // Bra/Bsr/Bcc/DBcc
    uint16_t reglist = 0;   // Movem: bit 0 = d0 ... bit 15 = a7 (whatever the encoding order)
    bool to_memory = false; // Movem direction
    uint16_t opcode = 0;
};

// Reads big-endian words from the ROM image.
struct Rom {
    const std::vector<uint8_t>& bytes;
    uint16_t word(uint32_t a) const { return a + 1 < bytes.size() ? uint16_t(bytes[a] << 8 | bytes[a + 1]) : 0xFFFF; }
    uint32_t lng(uint32_t a) const { return uint32_t(word(a)) << 16 | word(a + 2); }
};

Insn decode(const Rom& rom, uint32_t address);
// The 68000's opcode table to decode against (Musashi's m68ki_cycles[0], or the copy in
// cycles68000.c): an opcode it gives no cycles is no 68000 instruction (an addressing mode the
// instruction doesn't take, a 68020 one), so it decodes as the illegal-instruction exception, as
// a 68000 takes it. Null (the default): not checked.
void use_opcode_table(const uint8_t* cycles);
std::string describe(const Insn& insn);    // for messages and comments

}  // namespace recomp
