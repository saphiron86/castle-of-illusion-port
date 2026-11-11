// The readable C back end (docs/decompile.md): the program's functions as structured C, in the
// names port/recomp/readable_rt.h defines. Used by coi_recomp --readable.
#pragma once

#include "program.h"

#include <functional>
#include <map>
#include <set>
#include <vector>
#include <string>

namespace recomp {

// A setting's ROM patch written by hand (config/patches.txt, docs/decompile.md "Patches by hand"):
// while the setting's bit is set, `lines` (C in readable_rt.h's names) run in place of the
// instruction at `address`, whose bytes in the base image are `original`. The C either goes on to
// the next instruction (address + original's length) or leaves the function (`leaves`: it ends
// with a return or a JUMP). `cycles`: the CYCLES it adds (the generator checks them against the
// setting's image). `original` can hold several of the base's instructions, where the setting's
// code there starts its instructions elsewhere (a jsr and a nop in place of two instructions):
// then it is all of them, up to where both start one at the same place again (or the setting's
// leave). `calls`: its C has a CALL (a call to one of the setting's routines), so what follows it
// is a stretch of its own, and a STEP after the call checks the budget again (the callee's cycles
// weren't in the stretch's). `rs_lines`: the C for the resumable copy (its CALLs through the
// lookup).
struct Patch {
    uint32_t address = 0;
    std::string setting;                // (as a C name: infinite_items)
    uint32_t bit = 0;
    std::string note;
    std::vector<std::string> lines, rs_lines;
    std::vector<uint8_t> original;
    bool leaves = false, calls = false;
    int cycles = 0;
    std::vector<uint32_t> steps;        // STEP(address): where the setting's next instructions start
    // A named value (`=` in patches.txt) instead: the instruction stays, and the one constant in it
    // that the setting changes is `value_name`, (SETTING(setting) ? new_value : old_value). `places`:
    // where the lowered instruction has it (operation, 0 for operand a or 1 for b).
    std::string value_name;
    uint32_t old_value = 0, new_value = 0;
    std::vector<std::pair<size_t, int>> places;
    bool is_value() const { return !value_name.empty(); }
};

// Puts temporary `t` in place of a value patch's constant in `b` (its instruction's lowered code).
inline void name_value(IrBlock& b, const Patch& p, uint32_t& t)
{
    t = b.temps++;
    for (const auto& [op, which] : p.places)
        (which ? b.code[op].b : b.code[op].a) = Val::temp(t);
}

struct ReadableNames {
    std::function<std::string(uint32_t)> function;      // a function's C name
    std::function<std::string(uint32_t)> disassemble;   // the instruction at an address, as assembly
    std::map<uint32_t, std::string> addresses;          // fixed addresses' names (24-bit)
    std::map<uint32_t, std::string> address_notes, function_notes;  // their comments in config/
    std::map<uint32_t, std::string> fields, field_notes;    // object fields by offset (config/objects.txt)
    // Fields of one kind of object (config/object_fields.txt): per object ID, by offset; and which
    // object each function belongs to (its code, its routines, what only they call).
    std::map<uint32_t, std::map<uint32_t, std::string>> kind_fields;
    std::map<std::string, std::string> kind_field_notes;   // by name
    std::map<uint32_t, uint32_t> function_kind;            // function entry -> object ID
    std::map<uint32_t, Patch> patches;                  // hand-written patches by address
    std::vector<std::string> settings;                  // the image family's settings in bit order (C names)
    size_t width(uint32_t) const { return 2; }          // (spaces before the assembly comment)
};

// A setting's variant (docs/decompile.md): the program as that setting's image has it, and the
// functions it changes. Each is written again as NAME<suffix> (and coi_rs_XXXXXX<suffix>), and
// the base function hands its calls to it while bit `bit` of coi_native_settings is set.
struct ReadableVariant {
    const Program* program = nullptr;
    std::string suffix;
    uint32_t bit = 0;
    std::set<uint32_t> entries;
    std::function<std::string(uint32_t)> disassemble;   // (its own image's assembly)
};

// The readable functions (each `uint32_t NAME(CoiCpu* c, uint32_t at)`, handing `at` != 0 to
// coi_rs_XXXXXX, the resumable copy).
std::string emit_readable(const Program& program, const ReadableNames& names,
                          const std::vector<ReadableVariant>& variants = {});

}  // namespace recomp
