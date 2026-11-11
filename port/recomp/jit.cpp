#include "jit.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <sys/mman.h>
#include <unistd.h>
#endif

namespace recomp::jit {

namespace {

Jit* g_jit = nullptr;                                       // the translation that runs
std::mutex g_all_mutex;
std::vector<Jit*> g_all;                                    // every translation made, kept

// Which flags an operation writes outright (it reads none of them).
uint32_t flags_written(Ir op)
{
    switch (op) {
    case Ir::AddF: case Ir::SubF: case Ir::SetCcr: return kAllFlags;
    case Ir::CmpF: case Ir::LogicF: return kFN | kFZ | kFV | kFC;
    case Ir::SetZ: return kFZ;
    default: return 0;
    }
}

// Operations that neither read the flags nor leave the straight line.
bool flag_neutral(Ir op)
{
    switch (op) {
    case Ir::Mov: case Ir::GetReg: case Ir::Add: case Ir::Sub: case Ir::And: case Ir::Or: case Ir::Xor:
    case Ir::Shl: case Ir::Shr: case Ir::Eq: case Ir::Ne: case Ir::Ltu: case Ir::MulU16: case Ir::MulS16:
    case Ir::Sext: case Ir::Read: case Ir::GetSrHigh: case Ir::GetUsp: case Ir::MuluCycles: case Ir::MulsCycles:
    case Ir::Pop32: case Ir::IrqLevel: case Ir::SetReg: case Ir::SetRegPart: case Ir::Write: case Ir::SetSrHigh: case Ir::SetUsp: case Ir::Push32:
    case Ir::Cycles: case Ir::AddCycles: case Ir::Trace: case Ir::SetMissing:
        return true;
    default:
        return false;
    }
}

}  // namespace

CoiNativeFn lookup(uint32_t address)
{
    const auto& v = g_jit->entry_at;
    return address / 2 < v.size() && !(address & 1) ? v[address / 2] : nullptr;
}

uint32_t run(CoiCpu* c, CoiNativeFn fn, uint32_t at)
{
    uint32_t r = fn(c, at);
    while (c->jump && !c->fail) {
        c->jump = 0;
        if (!(fn = lookup(r))) {
            c->fail = r;
            return 0;
        }
        r = fn(c, 0);
    }
    return r;
}

const void* find(uint32_t at)
{
    const auto& v = g_jit->resume_code;
    return at / 2 < v.size() && !(at & 1) ? v[at / 2] : nullptr;     // (odd: an address error on a 68000)
}

uint32_t most_cycles(const IrBlock& b)
{
    uint32_t n = 0;
    for (const IrInst& i : b.code)
        if (i.op == Ir::Cycles) n += m68ki_cycles[0][i.k & 0xFFFF];
        else if (i.op == Ir::AddCycles) n += i.a.kind == Val::Imm ? (int32_t(i.a.n) > 0 ? i.a.n : 0) : 128;
    return n;
}

Layout layout(const Function& f, const std::map<uint32_t, IrBlock>& blocks, bool trace)
{
    Layout l;
    l.leaders.insert(f.entry);
    const Insn* prev = nullptr;
    bool prev_calls = false;
    for (const auto& [addr, in] : f.insns) {
        if (!prev || prev->address + uint32_t(prev->length) != addr || ends_block(*prev) || prev_calls)
            l.leaders.insert(addr);
        prev_calls = false;
        for (const IrInst& i : blocks.at(addr).code)
            if (i.op == Ir::Goto || i.op == Ir::GotoIf) l.leaders.insert(i.k);
            else if (i.op == Ir::Call) prev_calls = true;
        prev = &in;
    }
    uint32_t leader = f.entry;
    for (const auto& [addr, in] : f.insns) {
        if (l.leaders.count(addr))
            leader = addr;
        l.most[leader] += most_cycles(blocks.at(addr));
    }

    if (trace)
        return l;
    std::vector<const IrInst*> run;
    auto finish = [&] {
        uint32_t live = kAllFlags;                          // the next block may read them
        for (auto it = run.rbegin(); it != run.rend(); ++it) {
            const IrInst& i = **it;
            const uint32_t w = flags_written(i.op);
            if (w) {
                l.live_flags[&i] = live & w;
                live &= ~w;
            } else if (!flag_neutral(i.op)) {
                live = kAllFlags;
            }
        }
        run.clear();
    };
    for (const auto& [addr, in] : f.insns) {
        if (l.leaders.count(addr))
            finish();
        for (const IrInst& i : blocks.at(addr).code)
            if (!(i.op == Ir::Limit && i.k == addr))         // not in the unchecked copy
                run.push_back(&i);
    }
    finish();
    return l;
}

uint8_t* executable(const std::vector<uint8_t>& code)
{
#ifdef _WIN32
    void* p = VirtualAlloc(nullptr, code.size(), MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!p)
        return nullptr;
    std::memcpy(p, code.data(), code.size());
    DWORD old;
    if (!VirtualProtect(p, code.size(), PAGE_EXECUTE_READ, &old))
        return nullptr;
    FlushInstructionCache(GetCurrentProcess(), p, code.size());
#else
    void* p = mmap(nullptr, code.size(), PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED)
        return nullptr;
    std::memcpy(p, code.data(), code.size());
    __builtin___clear_cache(static_cast<char*>(p), static_cast<char*>(p) + code.size());
    if (mprotect(p, code.size(), PROT_READ | PROT_EXEC) != 0)
        return nullptr;
#endif
    return static_cast<uint8_t*>(p);
}

const CoiNativeCode* install(const Program& program, std::unique_ptr<Jit> jit, const std::vector<uint8_t>& code,
                             const std::map<uint32_t, size_t>& start, const std::map<uint32_t, size_t>& insn_at,
                             const std::vector<std::pair<size_t, uint32_t>>& marks, std::string* error)
{
    // COI_JIT_DUMP=FILE: the code as emitted, for a disassembler.
    if (const char* dump = std::getenv("COI_JIT_DUMP"))
        if (FILE* f = std::fopen(dump, "wb")) {
            std::fwrite(code.data(), 1, code.size(), f);
            std::fclose(f);
        }
    jit->code = executable(code);
    if (!jit->code) {
        if (error) *error = "no executable memory";
        return nullptr;
    }
#ifndef _WIN32
    // COI_PERF_MAP: name each instruction's code for perf (/tmp/perf-PID.map).
    if (std::getenv("COI_PERF_MAP")) {
        char path[64];
        std::snprintf(path, sizeof(path), "/tmp/perf-%d.map", int(getpid()));
        if (FILE* m = std::fopen(path, "w")) {
            for (size_t k = 0; k < marks.size(); k++) {
                const size_t end = k + 1 < marks.size() ? marks[k + 1].first : code.size();
                if (end > marks[k].first)
                    std::fprintf(m, "%llx %zx m68k_%06X%s\n", (unsigned long long)(uintptr_t)(jit->code + marks[k].first),
                                 end - marks[k].first, marks[k].second & 0xFFFFFF, marks[k].second >> 31 ? "_checked" : "");
            }
            std::fclose(m);
        }
    }
#else
    (void)marks;
#endif
    const auto& functions = program.functions();
    std::map<uint32_t, size_t> slot_of;
    for (const auto& [entry, f] : functions)
        slot_of.emplace(entry, slot_of.size());
    for (const auto& [entry, at] : start) {
        const CoiNativeFn fn = reinterpret_cast<CoiNativeFn>(jit->code + at);
        jit->slots[slot_of.at(entry)] = fn;
        jit->entries[entry] = fn;
    }
    // The resume table: each instruction with a function to enter (its entry jumps there).
    std::map<uint32_t, CoiNativeFn> first;
    for (const auto& [entry, f] : functions)
        for (const auto& [addr, in] : f.insns)
            first.emplace(addr, jit->entries.at(entry));
    for (const auto& [addr, fn] : first)
        jit->resume.push_back({addr, fn});
    const auto& rom = program.rom().bytes;
    jit->entry_at.assign(rom.size() / 2 + 1, nullptr);
    for (const auto& [addr, fn] : jit->entries)
        if (addr / 2 < jit->entry_at.size())
            jit->entry_at[addr / 2] = fn;
    jit->resume_code.assign(rom.size() / 2 + 1, nullptr);
    for (const auto& [addr, at] : insn_at)
        if (addr / 2 < jit->resume_code.size())
            jit->resume_code[addr / 2] = jit->code + at;
    jit->entry_list = program.entries();

    jit->tables = CoiNativeCode{lookup, run, jit->resume.data(), int(jit->resume.size()),
                                jit->entry_list.data(), int(jit->entry_list.size()), uint32_t(rom.size()),
                                coi_native_hash(rom.data(), uint32_t(rom.size()))};
    std::lock_guard<std::mutex> lock(g_all_mutex);
    g_all.push_back(jit.release());
    return &g_all.back()->tables;
}

bool shape(const CoiNativeCode* code, uint32_t* temps, bool* trace)
{
    std::lock_guard<std::mutex> lock(g_all_mutex);
    for (Jit* j : g_all)
        if (&j->tables == code) {
            *temps = j->temps;
            *trace = j->trace;
            return true;
        }
    return false;
}

void merge(const CoiNativeCode* base, const CoiNativeCode* extra)
{
    std::lock_guard<std::mutex> lock(g_all_mutex);
    Jit* b = nullptr;
    Jit* e = nullptr;
    for (Jit* j : g_all) {
        if (&j->tables == base) b = j;
        if (&j->tables == extra) e = j;
    }
    if (!b || !e || b == e)
        return;
    for (const auto& [addr, fn] : e->entries)
        if (b->entries.emplace(addr, fn).second && addr / 2 < b->entry_at.size() && !(addr & 1))
            b->entry_at[addr / 2] = fn;
    for (const CoiResume& r : e->resume)
        if (r.address / 2 < b->resume_code.size() && !b->resume_code[r.address / 2]) {
            b->resume_code[r.address / 2] = e->resume_code[r.address / 2];
            b->resume.push_back(r);
        }
    b->tables.resume = b->resume.data();
    b->tables.resume_count = int(b->resume.size());
}

void select(const CoiNativeCode* code)
{
    std::lock_guard<std::mutex> lock(g_all_mutex);
    for (Jit* j : g_all)
        if (&j->tables == code)
            g_jit = j;
}

}  // namespace recomp::jit
