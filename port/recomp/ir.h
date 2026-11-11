// The recompiler's intermediate form: each 68000 instruction lowered (lower.cpp) to a short
// list of three-address operations on 32-bit temporaries, which every back end (C for checking,
// machine code for the app) turns into code. Operations that set the 68000's flags are the
// runtime's helpers (native_rt.h), so their exact Musashi behaviour lives in one place.
#pragma once

#include <cstdint>
#include <vector>

namespace recomp {

// An operand: a temporary of the instruction being lowered, or an immediate.
struct Val {
    enum Kind : uint8_t { None, Temp, Imm };
    Kind kind = None;
    uint32_t n = 0;
    static Val temp(uint32_t i) { return {Temp, i}; }
    static Val imm(uint32_t v) { return {Imm, v}; }
};

enum class Ir : uint8_t {
    // dst = ... (plain 32-bit arithmetic, no flags)
    Mov,            // a
    GetReg,         // register k (0-7 d0-d7, 8-15 a0-a7)
    Add, Sub, And, Or, Xor, Shl, Shr,   // a op b
    Eq, Ne, Ltu,    // a op b: 1 or 0
    MulU16, MulS16, // the 68000's mulu/muls: 16 x 16 bits to 32
    Sext,           // a sign-extended from `size` bytes
    Read,           // memory: `size` bytes at a
    GetSrHigh,      // the status register's system byte
    GetUsp,         // the user stack pointer (translated code runs in supervisor mode)
    // dst = runtime helper (flags as Musashi sets them)
    AddF, SubF, AddxF, SubxF,   // b + a, b - a; size
    LogicF,         // N and Z of a, V and C cleared; returns a
    ShiftF,         // a shifted (kind k: asl asr lsl lsr rol ror roxl roxr) by b; size
    Cond,           // condition k holds: 1 or 0
    GetCcr,
    MuluCycles, MulsCycles,     // the extra cycles for a source a
    Divu, Divs,     // divides register k by a; 0 (and no change) if a is 0, else 1
    Pop32,          // pops a long
    Interrupt,      // takes the interrupt an SR write just let in, returning to a: the handler
    Exception,      // takes exception (vector) k of the instruction (opcode) k2, its frame's PC a:
                    // the handler
    StopCpu,        // stop: the SR = a, waiting for an interrupt, the next instruction at b: where
                    // the CPU goes on (b, or an interrupt's handler)
    IrqLevel,       // the interrupt waiting: its level 0-7, or 8 for a non-maskable one
    // effects
    SetReg,         // register k = a
    SetRegPart,     // the low `size` bytes of data register k = a
    Write,          // memory: `size` bytes at a = b
    CmpF,           // the flags of b - a; size
    SetCcr,         // CCR = a
    SetSrHigh,      // the system byte = a
    SetSr,          // SR = a, the stack pointers swapped if the mode changes
    SetUsp,         // the user stack pointer = a
    SetZ,           // Z = (a & b) == 0 (btst and friends)
    Push32,         // pushes a
    Cycles,         // the cycles Musashi's table gives opcode k
    AddCycles,      // cycles += a (signed)
    Spin,           // a bra or jmp (opcode k) to itself: the rest of the slice, as Musashi spends it
    Idle,           // a busy-wait loop's branch back (opcode k2) to the tst (opcode k) just before
                    // it, when a is nonzero (taken, and the tst reading b now would loop again):
                    // as many whole turns of the loop as the cycle budget leaves room for, at
                    // once, and if any, the flags of that tst (of `size` bytes b)
    // control within the instruction (local labels k)
    Label, Br, BrIf, BrIfNot,   // BrIf/BrIfNot test a
    // control between instructions and functions
    Goto, GotoIf,   // to the instruction at k (GotoIf: when a is nonzero)
    Trace,          // the instruction at k starts (the port's PC, the harness's trace)
    Limit,          // stop before the instruction at k if the cycle budget is spent
    Stop,           // stop before the instruction at k (opcode k2), giving its cycles back
    SetMissing,     // the code at k isn't translated (for the report)
    Return,         // rts: returns to a
    JumpOut,        // jmp out of the function, to the function at k
    JumpDyn,        // jmp out of the function, to the address a (known only when it runs)
    Call,           // calls the function at k (its return address pushed), which must return to k2
};

struct IrInst {
    Ir op;
    uint8_t size = 0;
    uint32_t dst = 0;       // the temporary an operation that yields a value writes
    Val a, b;
    uint32_t k = 0, k2 = 0;
};

// Operations that yield a value (into dst).
inline bool yields(Ir op)
{
    return op <= Ir::IrqLevel;
}

// One 68000 instruction, lowered.
struct IrBlock {
    uint32_t address = 0;
    std::vector<IrInst> code;
    uint32_t temps = 0;     // temporaries used: 0 .. temps-1
    uint32_t labels = 0;    // local labels used: 0 .. labels-1
};

}  // namespace recomp
