// What the machine-code back ends (x64.cpp, arm64.cpp) share: the tables translated code runs
// through, executable memory, each function's blocks and budget checks, which flags each
// operation has to store, and the driver that lowers the program and installs the code.
//
// Every function has the same frame, so code that stopped partway can be resumed at any
// translated instruction through any function's entry: the entry jumps to that instruction's
// code wherever it is, and every function's exit is the same.
//
// The cycle budget: a function's code is in blocks, runs of instructions entered only at the
// first (the entry, branch targets, after calls, wherever the code before doesn't just fall
// through). A block checks once that the most cycles it can use still leave the budget unspent,
// and then runs without checking; if they might not, it runs from a second copy of the
// function's code that checks before every instruction, as the 68000 would stop. Resuming
// partway goes to that second copy too.
//
// Within a block of the unchecked copy, a flag that the next flag-setting operation overwrites
// before anything could read it isn't stored (unless the harness traces, which compares the
// flags after every instruction). Anything that might read the flags, leave the block or be
// arrived at from elsewhere counts as reading them all, so wherever the code can stop or go on
// to other code, the flags are exactly the 68000's.
#pragma once

#include "lower.h"
#include "native_rt.h"
#include "program.h"

#include <algorithm>
#include <cstdint>
#include <map>
#include <memory>
#include <set>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

namespace recomp::jit {

// The translated program: its code and the tables it runs through.
struct Jit {
    uint8_t* code = nullptr;
    std::vector<CoiNativeFn> slots;                         // per function, its entry (for calls)
    std::unordered_map<uint32_t, CoiNativeFn> entries;      // 68000 address -> function
    std::vector<CoiNativeFn> entry_at;                      // the same per ROM word, for lookup
    std::vector<uint8_t*> resume_code;                      // per ROM word: its instruction's code
    std::vector<CoiResume> resume;
    std::vector<uint32_t> entry_list;
    CoiNativeCode tables{};
    uint32_t temps = 0;                                     // the frame's temporaries
    bool trace = false;
};

// Every translation's frame has room for at least this many temporaries, so code translated
// later (on demand) has the same frame and the two can resume in each other's instructions.
constexpr uint32_t kFrameTemps = 128;

// The frame and tracing a translation was made with (false if it isn't a JIT translation).
bool shape(const CoiNativeCode* code, uint32_t* temps, bool* trace);

// Adds `extra`'s functions and instructions to `base`'s tables wherever base has none, so code
// translated on demand runs through base (made with base's shape). Only while no translated
// code runs.
void merge(const CoiNativeCode* base, const CoiNativeCode* extra);

// Called from translated code: the function at a 68000 address (null if none), a function run
// with its tail jumps followed, and the code of the instruction at `at` (null if not translated).
CoiNativeFn lookup(uint32_t address);
uint32_t run(CoiCpu* c, CoiNativeFn fn, uint32_t at);
const void* find(uint32_t at);

// The flags as bits.
enum : uint32_t { kFX = 1, kFN = 2, kFZ = 4, kFV = 8, kFC = 16, kAllFlags = 31 };

// One function's blocks: their first instructions, the most cycles each can use, and (for the
// unchecked copy, unless tracing) which flags each flag-setting operation has to store.
struct Layout {
    std::set<uint32_t> leaders;
    std::map<uint32_t, uint32_t> most;
    std::map<const IrInst*, uint32_t> live_flags;
    uint32_t needed(const IrInst& i) const
    {
        auto it = live_flags.find(&i);
        return it == live_flags.end() ? kAllFlags : it->second;
    }
};
Layout layout(const Function& f, const std::map<uint32_t, IrBlock>& blocks, bool trace);

// The most cycles an instruction can use (its dynamic extras at their most: shift counts,
// mulu/muls source bits, scc).
uint32_t most_cycles(const IrBlock& b);

// The code in executable memory (null if there is none to be had).
uint8_t* executable(const std::vector<uint8_t>& code);

// The emitted program, installed: its tables; marks name each instruction's code for perf
// (COI_PERF_MAP, not on Windows). It runs once select() picks it. Safe on any thread.
const CoiNativeCode* install(const Program& program, std::unique_ptr<Jit> jit, const std::vector<uint8_t>& code,
                             const std::map<uint32_t, size_t>& start, const std::map<uint32_t, size_t>& insn_at,
                             const std::vector<std::pair<size_t, uint32_t>>& marks, std::string* error);

// Makes a translation (translate's result) the one that runs: translated code finds its way
// through one translation's tables at a time. Only while no translated code runs.
void select(const CoiNativeCode* code);

// Translates the program with a back end's Emitter (on any thread; select() runs it):
//   Emitter(uint32_t temps, bool trace, const std::map<uint32_t, size_t>& slot_of, std::vector<CoiNativeFn>& slots)
//   size_t function(const Function&, const std::map<uint32_t, IrBlock>&, std::map<uint32_t, size_t>& insn_at)
//   const std::vector<uint8_t>& code() const;
//   std::vector<std::pair<size_t, uint32_t>> marks;        // offset, address (top bit: checked copy)
// `slots` is sized before emitting and doesn't move, so code can call through its entries.
// `temps`: the frame to use (another translation's, to merge with it), or 0 for the program's own.
template <class Emitter>
const CoiNativeCode* translate(const Program& program, bool trace, std::string* error, uint32_t frame = 0)
{
    try {
        auto jit = std::make_unique<Jit>();
        const auto& functions = program.functions();

        // Lower everything first: the frame (the same for every function) has room for the
        // most temporaries any instruction uses.
        std::map<uint32_t, std::map<uint32_t, IrBlock>> lowered;
        uint32_t temps = 1;
        for (const auto& [entry, f] : functions)
            for (const auto& [addr, in] : f.insns) {
                IrBlock b = lower(program, in);
                temps = std::max(temps, b.temps);
                lowered[entry].emplace(addr, std::move(b));
            }

        temps = std::max(temps, kFrameTemps);
        if (frame && temps > frame)
            throw std::runtime_error("the code needs a bigger frame than the translation it would join");
        if (frame)
            temps = frame;
        jit->temps = temps;
        jit->trace = trace;
        std::map<uint32_t, size_t> slot_of;
        for (const auto& [entry, f] : functions)
            slot_of.emplace(entry, slot_of.size());
        jit->slots.assign(slot_of.size(), nullptr);
        Emitter emitter(temps, trace, slot_of, jit->slots);
        std::map<uint32_t, size_t> start;
        std::map<uint32_t, size_t> insn_at;                 // each instruction once: the first function's
        for (const auto& [entry, f] : functions) {
            std::map<uint32_t, size_t> here;
            start[entry] = emitter.function(f, lowered.at(entry), here);
            for (const auto& [addr, at] : here)
                insn_at.emplace(addr, at);
        }
        return install(program, std::move(jit), emitter.code(), start, insn_at, emitter.marks, error);
    } catch (const std::exception& ex) {
        if (error) *error = ex.what();
        return nullptr;
    }
}

}  // namespace recomp::jit
