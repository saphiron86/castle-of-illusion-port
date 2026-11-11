// The recompiler's front end: which 68000 functions there are and what they contain, from the
// ROM and the configuration (config/recomp.txt, the tracer's lists). Shared by every back
// end.
#pragma once

#include "decode.h"
#include "lower.h"

#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace recomp {

// config/recomp.txt, one directive per line, addresses in hex (every number is hex), # comments:
//   function ADDR             translate the function starting at ADDR (and what it calls)
//   jump INSN T1 T2 ...       the computed jmp at INSN goes to one of T1, T2, ...
//   call INSN T1 T2 ...       the computed jsr at INSN calls one of T1, T2, ...
//   stubs RAM ROM COUNT       COUNT `jmp (routine).l` stubs copied from ROM to RAM at boot:
//                             a call or jump to RAM + 6n goes straight to stub n's routine
struct Config {
    std::vector<uint32_t> functions;
    std::map<uint32_t, std::vector<uint32_t>> jumps, calls;
    std::vector<std::vector<uint32_t>> stubs;   // RAM, ROM, count
};

// Reads a config file (or the tracer's sites.txt, in the same format) into `config`; jump and
// call lines don't replace ones already there. Throws std::runtime_error on errors. read_config
// takes the text itself (`name` for messages).
void load_config(const std::string& path, Config& config);
void read_config(const std::string& text, const std::string& name, Config& config);

// The addresses in a list of functions (the tracer's subroutines.txt: an address per line,
// lines starting with # left out), from a file or from the text itself.
std::vector<uint32_t> load_function_list(const std::string& path);
std::vector<uint32_t> read_function_list(const std::string& text);

struct Function {
    uint32_t entry = 0;
    std::map<uint32_t, Insn> insns;
};

class Program : public LowerContext {
public:
    Program(const Rom& rom, const Config& config);      // throws if a stub isn't a jmp

    // Adds the function at `entry` (throwing if it can't be translated), then everything it
    // calls, leaving out callees that can't be translated (calls to them stop translated code).
    void add(uint32_t entry);

    // Adds what the config lists (throwing if any fails), then each function of `list` it can;
    // the functions that couldn't be translated, and why, go to `skipped`.
    void add_all(const std::vector<uint32_t>& list, std::vector<std::pair<uint32_t, std::string>>* skipped);

    const std::map<uint32_t, Function>& functions() const { return functions_; }
    const std::vector<uint32_t>& entries() const { return entries_; }     // the functions asked for
    const Rom& rom() const { return rom_; }
    const std::map<uint32_t, std::string>& failed() const { return failed_; }     // left out, and why
    // Computed jsr/jmp with no known targets (no 'call'/'jump' line, nothing from the tracer):
    // translated code stops at them and the interpreter goes on.
    const std::set<uint32_t>& unresolved() const { return unresolved_; }

    uint32_t resolve(uint32_t target) const override;
    bool is_stub(uint32_t target) const override { return stubs_.count(target & 0xFFFFFF) != 0; }
    bool translated(uint32_t entry) const override { return functions_.count(entry) != 0; }
    const std::vector<uint32_t>& call_targets(uint32_t address) const override;
    const std::vector<uint32_t>& jump_targets(uint32_t address) const override;
    Insn insn_at(uint32_t address) const override { return decode(rom_, address); }

private:
    const Rom& rom_;
    Config config_;
    std::map<uint32_t, uint32_t> stubs_;            // RAM stub -> its routine
    std::map<uint32_t, Function> functions_;
    std::map<uint32_t, std::string> failed_;        // functions that couldn't be translated, and why
    std::set<uint32_t> unresolved_;
    std::vector<uint32_t> pending_;                 // callees still to translate
    std::vector<uint32_t> entries_;
    const std::vector<uint32_t> none_;

    void translate(uint32_t entry);
    void add_body(uint32_t entry);
};

}  // namespace recomp
