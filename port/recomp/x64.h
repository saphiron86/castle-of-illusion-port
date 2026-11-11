// The app's own translator, x86-64: the program's IR (lower.cpp) to machine code in executable
// memory, run through the same tables (CoiNativeCode) as every translation.
#pragma once

#include "native_rt.h"
#include "program.h"

#include <string>

namespace recomp {

// Translates every function of `program`; null (and the reason in *error) if it can't. The
// code lives until the program ends. Needs m68k_init to have filled Musashi's cycle table.
// With `trace`, the code reports each instruction to the harness while coi_native_tracing is
// set (replays, lockstep checks); without, it doesn't look.
// `frame`: the frame to use, to merge with another translation (jit::merge); 0 for its own.
const CoiNativeCode* translate_x64(const Program& program, bool trace, std::string* error, uint32_t frame = 0);

}  // namespace recomp
