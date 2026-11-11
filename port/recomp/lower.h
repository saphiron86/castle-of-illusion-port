// Lowering one decoded 68000 instruction to the intermediate form (ir.h).
#pragma once

#include "decode.h"
#include "ir.h"

#include <cstdint>
#include <optional>
#include <vector>

namespace recomp {

// What lowering needs to know about the rest of the program.
class LowerContext {
public:
    virtual ~LowerContext() = default;
    // A call or jump target, with RAM stubs replaced by the routine they jump to.
    virtual uint32_t resolve(uint32_t target) const = 0;
    // `target` is a RAM stub (its jmp is an instruction of its own on the 68000).
    virtual bool is_stub(uint32_t target) const = 0;
    // The function at `entry` is translated.
    virtual bool translated(uint32_t entry) const = 0;
    // The targets of the computed jsr / jmp at `address` (empty if none are known).
    virtual const std::vector<uint32_t>& call_targets(uint32_t address) const = 0;
    virtual const std::vector<uint32_t>& jump_targets(uint32_t address) const = 0;
    // The instruction at `address`.
    virtual Insn insn_at(uint32_t address) const = 0;
};

// The instruction's code, from its start (Trace, Limit, its cycles) to its end. It doesn't
// include the fall-through to the next instruction.
IrBlock lower(const LowerContext& context, const Insn& in);

// The instruction ends its block: no fall-through.
bool ends_block(const Insn& in);

// A jmp/jsr target known from the instruction itself.
std::optional<uint32_t> static_target(const Insn& in);

}  // namespace recomp
