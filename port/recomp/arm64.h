// ARM64 machine code from the IR, in executable memory (Android and other ARM64 systems).
#pragma once

#include "native_rt.h"
#include "program.h"

#include <string>

namespace recomp {

// Translates the program; null (and why in `error`) if it can't. `trace`: calls the harness's
// trace before each instruction (COI_NATIVE_CHECK), at some cost.
// `frame`: the frame to use, to merge with another translation (jit::merge); 0 for its own.
const CoiNativeCode* translate_arm64(const Program& program, bool trace, std::string* error, uint32_t frame = 0);

}  // namespace recomp
