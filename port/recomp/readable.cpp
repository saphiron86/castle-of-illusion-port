// The readable C back end (coi_recomp --readable, docs/decompile.md): each function as structured
// C (if/else, do/while, the 68000's registers by name, expressions instead of temporaries), entered
// at its start only. Before each stretch of straight-line code it checks the cycle budget, and if
// the slice might end inside, it hands the rest of the call to the resumable copy (the C back
// end's code, coi_rs_XXXXXX), which stops where the 68000 would. Both copies come from the same
// lowered code (lower.cpp), so they do the same thing.

#include "readable.h"

#include "native_rt.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <functional>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>

namespace recomp {

namespace {

std::string hex6(uint32_t a)
{
    char b[16];
    std::snprintf(b, sizeof(b), "%06X", a & 0xFFFFFF);
    return b;
}

std::string num(uint32_t v)
{
    char b[24];
    if (v < 10)
        std::snprintf(b, sizeof(b), "%u", v);
    else if (v >= 0xFFFFFF00u)                          // small negative numbers as such
        std::snprintf(b, sizeof(b), "-%u", 0u - v);
    else
        std::snprintf(b, sizeof(b), "0x%X", v);
    return b;
}

std::string addr(uint32_t v)
{
    char b[24];
    std::snprintf(b, sizeof(b), "0x%06X", v);
    return b;
}

const char* const kReg[16] = {"D0", "D1", "D2", "D3", "D4", "D5", "D6", "D7",
                              "A0", "A1", "A2", "A3", "A4", "A5", "A6", "A7"};
const char* const kCond[16] = {"1", "0", "HI()", "LS()", "CC()", "CS()", "NE()", "EQ()",
                               "VC()", "VS()", "PL()", "MI()", "GE()", "LT()", "GT()", "LE()"};

// What an expression depends on and what a statement changes.
enum : uint32_t {
    kRegs = 0xFFFF,         // one bit per register
    kFlags = 1u << 16,
    kSr = 1u << 17,         // the system byte, the USP
    kMem = 1u << 18,
    kAll = 0xFFFFFFFFu,
};

// The flags, as the JIT's dead-flag analysis has them (jit.cpp): which an operation writes, and
// which operations neither read them nor leave the straight line.
enum : uint32_t { kFX = 1, kFN = 2, kFZ = 4, kFV = 8, kFC = 16, kAllFlags = 31 };

uint32_t flags_written(Ir op)
{
    switch (op) {
    case Ir::AddF: case Ir::SubF: case Ir::SetCcr: return kAllFlags;
    case Ir::CmpF: case Ir::LogicF: return kFN | kFZ | kFV | kFC;
    case Ir::SetZ: return kFZ;
    default: return 0;
    }
}

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

// Anything else that sets flags (for finding the last operation before a branch that did).
bool sets_flags(Ir op)
{
    return flags_written(op) || op == Ir::AddxF || op == Ir::SubxF || op == Ir::ShiftF || op == Ir::Divu || op == Ir::Divs
           || op == Ir::SetSr || op == Ir::Interrupt || op == Ir::Exception || op == Ir::StopCpu;
}

// The last flag-setting operation of an instruction, as a branch right after can test it as a
// comparison: cmp (flags of d - s), tst and the moves (of v), sub (of its result).
struct FlagRecord {
    bool valid = false;
    Ir op = Ir::Mov;
    int size = 0;
    std::string s, d;               // CmpF: the operands; LogicF and SubF: d, the value tested
    bool s_imm = false, d_imm = false;
    uint32_t s_val = 0, d_val = 0;
};

// `x` (an expression of `size` bytes) as a C value of that size, signed or not.
std::string as_size(const std::string& x0, int size, bool sgn, bool imm, uint32_t v)
{
    const uint32_t m = size == 1 ? 0xFFu : size == 2 ? 0xFFFFu : 0xFFFFFFFFu;
    if (imm) {
        v &= m;
        if (!sgn) return num(v);
        const int32_t sv = size == 1 ? int8_t(v) : size == 2 ? int16_t(v) : int32_t(v);
        return std::to_string(sv);
    }
    std::string x = x0;
    const std::string mask = size == 1 ? " & 0xFF)" : size == 2 ? " & 0xFFFF)" : "";
    if (!mask.empty() && x.size() > mask.size() + 1 && x.front() == '('
        && x.compare(x.size() - mask.size(), mask.size(), mask) == 0) {
        const std::string inner = x.substr(1, x.size() - mask.size() - 1);
        int depth = 0;
        bool balanced = true;
        for (char ch : inner) {
            if (ch == '(') depth++;
            else if (ch == ')' && --depth < 0) balanced = false;
        }
        if (balanced && depth == 0) x = inner;
    }
    const bool simple = x.find(' ') == std::string::npos;
    const std::string px = simple ? x : "(" + x + ")";
    if (size == 4) return sgn ? "(int32_t)" + px : px;
    return std::string(sgn ? (size == 1 ? "(int8_t)" : "(int16_t)") : (size == 1 ? "(uint8_t)" : "(uint16_t)")) + px;
}

// Condition k on the record's flags as a C comparison ("" if it has none).
std::string comparison(const FlagRecord& r, uint32_t k)
{
    if (!r.valid) return "";
    auto U = [&](bool d) { return d ? as_size(r.d, r.size, false, r.d_imm, r.d_val) : as_size(r.s, r.size, false, r.s_imm, r.s_val); };
    auto S = [&](bool d) { return d ? as_size(r.d, r.size, true, r.d_imm, r.d_val) : as_size(r.s, r.size, true, r.s_imm, r.s_val); };
    if (r.op == Ir::CmpF) {
        switch (k) {
        case 2: return U(true) + " > " + U(false);
        case 3: return U(true) + " <= " + U(false);
        case 4: return U(true) + " >= " + U(false);
        case 5: return U(true) + " < " + U(false);
        case 6: return U(true) + " != " + U(false);
        case 7: return U(true) + " == " + U(false);
        case 12: return S(true) + " >= " + S(false);
        case 13: return S(true) + " < " + S(false);
        case 14: return S(true) + " > " + S(false);
        case 15: return S(true) + " <= " + S(false);
        default: return "";
        }
    }
    if (r.op == Ir::LogicF) {
        switch (k) {
        case 2: case 6: return U(true) + " != 0";
        case 3: case 7: return U(true) + " == 0";
        case 10: case 12: return S(true) + " >= 0";
        case 11: case 13: return S(true) + " < 0";
        case 14: return S(true) + " > 0";
        case 15: return S(true) + " <= 0";
        default: return "";
        }
    }
    if (r.op == Ir::SubF) {                         // (the result: Z and N only)
        switch (k) {
        case 6: return U(true) + " != 0";
        case 7: return U(true) + " == 0";
        case 10: return S(true) + " >= 0";
        case 11: return S(true) + " < 0";
        default: return "";
        }
    }
    return "";
}

// The opposite condition: a comparison turned round, a flag test by its partner, or !(c).
std::string negate(const std::string& c)
{
    static const char* pairs[][2] = {{"NE()", "EQ()"}, {"CC()", "CS()"}, {"HI()", "LS()"}, {"GE()", "LT()"},
                                     {"GT()", "LE()"}, {"MI()", "PL()"}, {"VC()", "VS()"}};
    for (const auto& p : pairs) {
        if (c == p[0]) return p[1];
        if (c == p[1]) return p[0];
    }
    // Exactly one comparison at the top level (outside parentheses).
    int depth = 0, found = 0;
    size_t at = 0, len = 0;
    for (size_t i = 0; i < c.size(); i++) {
        const char ch = c[i];
        if (ch == '(') depth++;
        else if (ch == ')') depth--;
        else if (depth == 0 && i + 1 < c.size() && (ch == '=' || ch == '!' || ch == '<' || ch == '>')) {
            const bool two = c[i + 1] == '=';
            if ((ch == '=' || ch == '!') && !two) continue;
            if ((ch == '<' || ch == '>') && (c[i + 1] == ch)) { i++; continue; }      // (a shift)
            if (ch == '>' && i > 0 && c[i - 1] == '-') continue;                     // (->)
            found++;
            at = i;
            len = two ? 2 : 1;
            if (two) i++;
        } else if (depth == 0 && (ch == '&' || ch == '|') && i + 1 < c.size() && c[i + 1] == ch) {
            return "!(" + c + ")";
        }
    }
    if (found == 1 && c.find('?') == std::string::npos) {
        const std::string op = c.substr(at, len);
        const std::string inv = op == "==" ? "!=" : op == "!=" ? "==" : op == "<" ? ">=" : op == ">=" ? "<"
                              : op == ">" ? "<=" : ">";
        return c.substr(0, at) + inv + c.substr(at + len);
    }
    if (c.size() > 1 && c[0] == '!' && c.find(' ') == std::string::npos) return c.substr(1);
    return "!" + (c.find(' ') == std::string::npos || (c.front() == '(' && c.back() == ')') ? c : "(" + c + ")");
}

// Object fields (config/objects.txt): A0 + offset, A0 being the object the game is running.
const std::map<uint32_t, std::string>* g_fields = nullptr;
std::set<uint32_t>* g_fields_used = nullptr;
// The fields of the kind of object the function being written belongs to, if any.
const std::map<uint32_t, std::string>* g_kind_fields = nullptr;
std::map<std::string, uint32_t>* g_kind_fields_used = nullptr;

// Names for fixed addresses (config/ram.txt, config/hardware.txt), used for reads and writes.
const std::map<uint32_t, std::string>* g_addresses = nullptr;
std::set<std::string>* g_addresses_used = nullptr;

std::string address_of(uint32_t v)
{
    if (g_addresses) {
        auto it = g_addresses->find(v & 0xFFFFFF);
        if (it != g_addresses->end()) {
            if (g_addresses_used) g_addresses_used->insert(it->second);
            return it->second;
        }
    }
    return num(v);
}

// An address a lea or pea loads (absolute or PC-relative), by name where it has one: as it is,
// or as a .w operand gives a RAM address ($FFxxxx sign-extended, the top byte set).
std::string loaded_address(uint32_t v)
{
    if (g_addresses) {
        auto it = g_addresses->find(v & 0xFFFFFF);
        if (it != g_addresses->end() && (v == (v & 0xFFFFFF) || v == ((v & 0xFFFFFF) | 0xFF000000u))) {
            if (g_addresses_used) g_addresses_used->insert(it->second);
            return v == (v & 0xFFFFFF) ? it->second : "WORD_ADDRESS(" + it->second + ")";
        }
    }
    return num(v);
}

struct Expr {
    std::string s;
    uint32_t deps = 0;      // what it reads that can change
    uint32_t effects = 0;   // what evaluating it changes (a helper with flags, a read's bus)
    bool impure = false;    // a read or a helper call: only inlined into the next operation
};

// A register value kept in a C local rather than the CPU state (Function_::find_locals): from the
// instruction that writes it to the one that writes the register again, nothing can see the
// register, so the instructions between read the local. Per instruction: its GetRegs of `reg`
// with index in (from, to) read `name`; the SetReg at `declare` (if any) defines it.
struct Local {
    int reg = 0;
    long from = -1, to = 0;
    long declare = -1;
    std::string name;
};

// One instruction's IR as C statements, the temporaries folded into expressions where that
// keeps the order of everything that matters.
class Renderer {
public:
    // `live`: per operation, the flags it writes that something may still read (null: all).
    Renderer(const IrBlock& b, std::vector<std::string>& out, std::function<std::string(uint32_t)> callee,
             std::function<std::string(uint32_t)> label, std::string indent, int& vars, const std::vector<uint32_t>* live,
             const std::vector<Local>* locals = nullptr)
        : b(b), out(out), callee(std::move(callee)), label(std::move(label)), indent(std::move(indent)), vars(vars), live(live),
          locals(locals)
    {
        std::map<uint32_t, int> all;
        for (const IrInst& i : b.code)
            for (const Val* v : {&i.a, &i.b})
                if (v->kind == Val::Temp)
                    all[v->n]++;
        // Flag operations nothing reads, which do nothing else: left out (their operands not counted).
        for (size_t n = 0; n < b.code.size(); n++) {
            const IrInst& i = b.code[n];
            if (flags_live(n))
                continue;
            if (i.op == Ir::CmpF || i.op == Ir::SetZ || (i.op == Ir::LogicF && all[i.dst] == 0))
                dropped.insert(n);
        }
        for (size_t n = 0; n < b.code.size(); n++)
            if (!dropped.count(n))
                for (const Val* v : {&b.code[n].a, &b.code[n].b})
                    if (v->kind == Val::Temp)
                        uses[v->n]++;
        // Which bits of each value anything uses (a word written to a register part or memory, a
        // helper that masks its operands to the size): a mask that keeps all of those is left out.
        for (size_t n = b.code.size(); n-- > 0;) {
            if (dropped.count(n)) continue;
            const IrInst& i = b.code[n];
            const uint32_t m = i.size == 1 ? 0xFFu : i.size == 2 ? 0xFFFFu : 0xFFFFFFFFu;
            const uint32_t d = yields(i.op) && demand.count(i.dst) ? demand.at(i.dst) : 0xFFFFFFFFu;
            uint32_t low = d;                               // (carries only go up)
            for (int bit = 31; bit >= 0; bit--)
                if (d & (1u << bit)) { low = bit == 31 ? 0xFFFFFFFFu : (2u << bit) - 1; break; }
            auto want = [&](const Val& v, uint32_t bits) { if (v.kind == Val::Temp) demand[v.n] |= bits; };
            switch (i.op) {
            case Ir::SetRegPart: want(i.a, m); break;
            case Ir::Write: want(i.a, 0xFFFFFFFFu); want(i.b, m); break;
            case Ir::AddF: case Ir::SubF: case Ir::CmpF: case Ir::AddxF: case Ir::SubxF: want(i.a, m); want(i.b, m); break;
            case Ir::LogicF: want(i.a, m); break;
            case Ir::ShiftF: want(i.a, m); want(i.b, 0xFFFFFFFFu); break;
            case Ir::Sext: want(i.a, m); break;
            case Ir::MulU16: case Ir::MulS16: want(i.a, 0xFFFF); want(i.b, 0xFFFF); break;
            case Ir::Add: case Ir::Sub: case Ir::Shl: want(i.a, low); want(i.b, i.op == Ir::Shl ? 0xFFFFFFFFu : low); break;
            case Ir::And: want(i.a, i.b.kind == Val::Imm ? d & i.b.n : d); want(i.b, d); break;
            case Ir::Or: case Ir::Xor: want(i.a, d); want(i.b, d); break;
            case Ir::Mov: want(i.a, d); break;
            default: want(i.a, 0xFFFFFFFFu); want(i.b, 0xFFFFFFFFu); break;
            }
        }
        // The last flag-setting operation, if a branch after can test it as a comparison.
        for (size_t n = b.code.size(); n-- > 0;)
            if (sets_flags(b.code[n].op)) {
                const Ir op = b.code[n].op;
                bool regs_after = false;
                for (size_t m = n + 1; m < b.code.size(); m++)
                    regs_after |= !flag_neutral(b.code[m].op) || b.code[m].op == Ir::SetReg || b.code[m].op == Ir::SetRegPart
                                  || b.code[m].op == Ir::Push32 || b.code[m].op == Ir::Pop32;
                if ((op == Ir::CmpF || op == Ir::LogicF || op == Ir::SubF) && !regs_after && !dropped.count(n))
                    record_at = n;
                break;
            }
    }

    FlagRecord record;          // (after run: the last flag operation's, if recorded)

    // Renders operations [from, to).
    void run(size_t from, size_t to)
    {
        for (size_t n = from; n < to; n++) {
            if (dispatch && is_case(n)) {
                cases(n, to);
                break;
            }
            // A branch over straight code to a label right after it: an if.
            const IrInst& i = b.code[n];
            if ((i.op == Ir::BrIf || i.op == Ir::BrIfNot) && !dropped.count(n)) {
                const size_t m = label_at(i.k, n + 1);
                if (m != size_t(-1) && m < to && straight(n, m)) {
                    const std::string c = strip(value(i.a));
                    settle_all();                   // (values used after it: variables first)
                    line("if (" + (i.op == Ir::BrIfNot ? c : negate(c)) + ") {");
                    const std::string outer = indent;
                    indent += "    ";
                    for (size_t k = n + 1; k < m; k++) op(k);
                    flush_cycles();
                    indent = outer;
                    line("}");
                    n = m;                          // (the label itself: nothing else goes there)
                    continue;
                }
            }
            op(n);
        }
        if (!hold)
            flush_cycles();
    }

    // A computed jmp/jsr's known targets as a switch (lower.cpp tests them one by one).
    bool dispatch = false;
    // A lea or pea of a fixed address (absolute or PC-relative): its immediate is that address.
    bool loads_address = false;
    // Cycles held over (an instruction nothing can observe the cycles of): added to the next.
    bool hold = false;
    void carry(int64_t n) { if (n) { cycles += n; cycles_pending = true; } }
    int64_t held() const { return cycles_pending ? cycles : 0; }

    // The value of operand `v` (consumed).
    std::string value(const Val& v)
    {
        if (v.kind == Val::Imm)
            return loads_address ? loaded_address(v.n) : num(v.n);
        auto it = env.find(v.n);
        std::string s = it->second.s;
        if (--uses[v.n] == 0)
            env.erase(it);
        return s;
    }

    void line(const std::string& s) { if (!hold) flush_cycles(); out.push_back(indent + s); }

    // Temporary t is a name (a patch's named value), not a number.
    void preset(uint32_t t, const std::string& name) { env[t] = Expr{name, 0, 0, false}; }

private:
    // Operation n starts a target's test: Eq(t, value) and a BrIfNot on it past the target's code.
    bool is_case(size_t n) const
    {
        if (n + 1 >= b.code.size()) return false;
        const IrInst& e = b.code[n];
        const IrInst& br = b.code[n + 1];
        return e.op == Ir::Eq && e.a.kind == Val::Temp && e.b.kind == Val::Imm && br.op == Ir::BrIfNot
               && br.a.kind == Val::Temp && br.a.n == e.dst && label_at(br.k, n + 2) != size_t(-1);
    }

    size_t label_at(uint32_t k, size_t from) const
    {
        for (size_t m = from; m < b.code.size(); m++)
            if (b.code[m].op == Ir::Label && b.code[m].k == k) return m;
        return size_t(-1);
    }

    void cases(size_t n, size_t to)
    {
        const uint32_t t = b.code[n].a.n;
        // A case that only jumps to the address it tests for is what the default does anyway
        // (a jump to that address, through the same lookup, with the same cycles): left out.
        auto plain_jump = [&](size_t at) {
            const size_t end = label_at(b.code[at + 1].k, at + 2);
            return end == at + 3 && b.code[at + 2].op == Ir::JumpOut && b.code[at + 2].k == b.code[at].b.n;
        };
        size_t kept = 0, folded = 0;
        for (size_t m = n; m < to && is_case(m) && b.code[m].a.n == t; m = label_at(b.code[m + 1].k, m + 2) + 1)
            (plain_jump(m) ? folded : kept)++;
        if (folded)
            out.push_back(indent + "/* " + std::to_string(folded) + " known targets, each a function of its own: the jump below */");
        if (kept)
            line("switch (" + strip(operand(Val::temp(t)).s) + ") {");
        std::set<uint32_t> seen;
        const std::string inner = indent;
        while (n < to && is_case(n) && b.code[n].a.n == t) {
            const uint32_t v = b.code[n].b.n;
            value(b.code[n].a);                     // (the test's use of t)
            const size_t end = label_at(b.code[n + 1].k, n + 2);
            if (plain_jump(n)) {
                n = end + 1;
                continue;
            }
            // A call's case ends with a Br past the default: a break.
            size_t body_end = end;
            const bool call = end > n + 2 && b.code[end - 1].op == Ir::Br;
            if (call) body_end = end - 1;
            if (seen.insert(v).second) {
                out.push_back(inner + "case " + num(v) + ":");
                indent = inner + "    ";
                for (size_t m = n + 2; m < body_end; m++) op(m);
                flush_cycles();
                if (call) line("break;");
                indent = inner;
            } else {
                for (size_t m = n + 2; m < body_end; m++) skip(m);
            }
            n = end + 1;
        }
        if (kept) {
            out.push_back(inner + "default:");
            indent = inner + "    ";
        }
        // The rest: any other target (to the end, or the Label the calls' Br goes to).
        size_t last = to;
        if (last > n && b.code[last - 1].op == Ir::Label) last--;
        for (size_t m = n; m < last; m++) op(m);
        flush_cycles();
        indent = inner;
        if (kept)
            out.push_back(inner + "}");
    }

    // Operations (n, m) are straight code (no labels or branches), and only operation n goes to
    // label m.
    bool straight(size_t n, size_t m) const
    {
        for (size_t k = n + 1; k < m; k++) {
            const Ir op = b.code[k].op;
            if (op == Ir::Label || op == Ir::Br || op == Ir::BrIf || op == Ir::BrIfNot || op == Ir::Goto || op == Ir::GotoIf)
                return false;
        }
        const uint32_t l = b.code[m].k;
        for (size_t k = 0; k < b.code.size(); k++)
            if (k != n && (b.code[k].op == Ir::Br || b.code[k].op == Ir::BrIf || b.code[k].op == Ir::BrIfNot) && b.code[k].k == l)
                return false;
        return true;
    }

    // Every pending value still to be used, as a variable now.
    void settle_all()
    {
        for (auto& [t, e] : env)
            if (uses[t] > 0 && e.s.rfind('v', 0) != 0)
                materialize(t, e);
    }

    // A duplicate case's code, not rendered: its values still consumed.
    void skip(size_t m)
    {
        const IrInst& i = b.code[m];
        for (const Val* v : {&i.a, &i.b})
            if (v->kind == Val::Temp && env.count(v->n)) value(*v);
    }

    const IrBlock& b;
    std::vector<std::string>& out;
    std::function<std::string(uint32_t)> callee, label;
    std::string indent;
    std::map<uint32_t, int> uses;
    std::map<uint32_t, Expr> env;
    int64_t cycles = 0;
    bool cycles_pending = false;
    int& vars;                  // (the function's: every variable's name is its own)
    const std::vector<uint32_t>* live;
    const std::vector<Local>* locals;
    std::set<size_t> dropped;
    // The local that register k's GetReg at n reads, or that its SetReg at n defines.
    const Local* local_read(int k, size_t n) const
    {
        if (locals)
            for (const Local& l : *locals)
                if (l.reg == k && long(n) > l.from && long(n) < l.to) return &l;
        return nullptr;
    }
    const Local* local_defined(int k, size_t n) const
    {
        if (locals)
            for (const Local& l : *locals)
                if (l.reg == k && l.declare == long(n)) return &l;
        return nullptr;
    }
    std::map<uint32_t, uint32_t> demand;    // per temporary: the bits anything uses
    bool pushed_for_call = false;           // (a Push32 left to the Call right after it)
    bool needs(uint32_t t, uint32_t mask) const { auto it = demand.find(t); return it == demand.end() || (it->second & ~mask); }
    size_t record_at = size_t(-1);

    bool flags_live(size_t n) const { return !live || n >= live->size() ? true : (*live)[n] != 0; }
    uint32_t mask_of(int size) const { return size == 1 ? 0xFFu : size == 2 ? 0xFFFFu : 0xFFFFFFFFu; }

    // The operands of operation n, made stable for the record (a read becomes a variable).
    void steady(size_t n)
    {
        for (const Val* v : {&b.code[n].a, &b.code[n].b})
            if (v->kind == Val::Temp && env.count(v->n) && env.at(v->n).impure)
                materialize(v->n, env.at(v->n));
    }
    void note(const IrInst& i, const Val& s, const Val& d)
    {
        record.valid = true;
        record.op = i.op;
        record.size = i.size;
        record.s = s.kind == Val::Temp ? operand(s).s : "";
        record.d = d.kind == Val::Temp ? operand(d).s : "";
        record.s_imm = s.kind == Val::Imm;
        record.d_imm = d.kind == Val::Imm;
        record.s_val = s.n;
        record.d_val = d.n;
    }

    void flush_cycles()
    {
        if (!cycles_pending)
            return;
        cycles_pending = false;
        if (cycles)
            out.push_back(indent + "CYCLES(" + std::to_string(cycles) + ");");
        cycles = 0;
    }

    const Expr& operand(const Val& v) { static const Expr none; return v.kind == Val::Temp ? env.at(v.n) : none; }

    // The value of operation n goes straight into a register part or memory of that size (which
    // keeps only those bits anyway).
    bool consumed_sized(size_t n, int size) const
    {
        if (n + 1 >= b.code.size()) return false;
        const IrInst& next = b.code[n + 1];
        const uint32_t t = b.code[n].dst;
        if (uses.count(t) == 0 || uses.at(t) != 1) return false;
        if (next.op == Ir::SetRegPart && next.size == size && next.a.kind == Val::Temp && next.a.n == t) return true;
        if (next.op == Ir::Write && next.size == size && next.b.kind == Val::Temp && next.b.n == t
            && !(next.a.kind == Val::Temp && next.a.n == t)) return true;
        return false;
    }

    // Before a statement that changes `effects`: pending expressions that depend on them become
    // variables now, holding the value they have at this point.
    void settle(uint32_t effects, const IrInst* consumer)
    {
        for (auto& [t, e] : env) {
            if (!(e.deps & effects) && !(effects == kAll && e.impure))
                continue;
            // (The statement's own operands are evaluated before it changes anything.)
            if (consumer && ((consumer->a.kind == Val::Temp && consumer->a.n == t) ||
                             (consumer->b.kind == Val::Temp && consumer->b.n == t)))
                continue;
            materialize(t, e);
        }
    }

    void materialize(uint32_t t, Expr& e)
    {
        const std::string name = "v" + std::to_string(++vars);
        line("const uint32_t " + name + " = " + strip(e.s) + ";");
        e = Expr{name, 0, 0, false};
    }

    static std::string strip(const std::string& s)
    {
        if (s.size() >= 2 && s.front() == '(' && s.back() == ')') {
            int depth = 0;
            for (size_t i = 0; i < s.size(); i++) {
                if (s[i] == '(') depth++;
                else if (s[i] == ')' && --depth == 0 && i + 1 < s.size()) return s;
            }
            return s.substr(1, s.size() - 2);
        }
        return s;
    }

    // Defines temporary `t`: inlined where it's used if that keeps the order right, else a variable.
    void define(size_t n, uint32_t t, Expr e)
    {
        if (uses[t] == 0) {                     // its value unused: only what it does
            if (e.impure)
                line(strip(e.s) + ";");
            return;
        }
        if (e.impure) {
            // Only into the very next operation, and only if nothing else that operation reads
            // depends on what this changes.
            bool ok = uses[t] == 1 && n + 1 < b.code.size();
            if (ok) {
                const IrInst& next = b.code[n + 1];
                const bool used = (next.a.kind == Val::Temp && next.a.n == t) || (next.b.kind == Val::Temp && next.b.n == t);
                ok = used;
                for (const Val* v : {&next.a, &next.b})
                    if (ok && v->kind == Val::Temp && v->n != t && (operand(*v).deps & e.effects))
                        ok = false;
            }
            if (!ok) {
                env[t] = e;
                materialize(t, env[t]);
                return;
            }
        } else if (uses[t] > 1) {
            env[t] = e;
            materialize(t, env[t]);
            return;
        }
        env[t] = e;
    }

    Expr pure(std::string s, uint32_t deps) { return Expr{std::move(s), deps, 0, false}; }
    Expr helper(std::string s, uint32_t deps, uint32_t effects) { return Expr{std::move(s), deps, effects, true}; }

    // An operand's value if it's known now (an immediate, or arithmetic on them).
    bool known(const Val& v, uint32_t& out)
    {
        if (v.kind == Val::Imm) { out = v.n; return true; }
        if (v.kind != Val::Temp || !env.count(v.n)) return false;
        const std::string& s = env.at(v.n).s;
        if (s.empty() || s.find_first_not_of("0123456789xABCDEF-") != std::string::npos) return false;
        try {
            out = s[0] == '-' ? 0u - uint32_t(std::stoul(s.substr(1), nullptr, 0)) : uint32_t(std::stoul(s, nullptr, 0));
        } catch (...) {
            return false;
        }
        return true;
    }

    std::string binop(const IrInst& i, const char* op)
    {
        uint32_t x = 0, y = 0;
        if (known(i.a, x) && known(i.b, y)) {
            const std::string o = op;
            const bool folds = o == "+" || o == "-" || o == "&" || o == "|" || o == "^" || ((o == "<<" || o == ">>") && y < 32);
            if (folds) {
                value(i.a);
                value(i.b);
                const uint32_t r = o == "+" ? x + y : o == "-" ? x - y : o == "&" ? x & y : o == "|" ? x | y
                                 : o == "^" ? x ^ y : o == "<<" ? x << y : x >> y;
                return num(r);
            }
        }
        const std::string a = value(i.a), b2 = value(i.b);
        if (i.op == Ir::Add && a == "A0" && i.b.kind == Val::Imm && g_kind_fields && g_kind_fields->count(i.b.n)) {
            const std::string& name = g_kind_fields->at(i.b.n);
            if (g_kind_fields_used) (*g_kind_fields_used)[name] = i.b.n;
            return "(A0 + " + name + ")";
        }
        if (i.op == Ir::Add && a == "A0" && i.b.kind == Val::Imm && g_fields && g_fields->count(i.b.n)) {
            if (g_fields_used) g_fields_used->insert(i.b.n);
            return "(A0 + " + g_fields->at(i.b.n) + ")";
        }
        if (i.op == Ir::Add && i.b.kind == Val::Imm && i.b.n == 0) return a;
        if (i.op == Ir::And && i.b.kind == Val::Imm && i.b.n == 0xFFFFFFFFu) return a;
        if (i.op == Ir::Add && i.b.kind == Val::Imm && i.b.n >= 0xFFFF0000u)
            return "(" + a + " - " + num(0u - i.b.n) + ")";
        return "(" + a + " " + op + " " + b2 + ")";
    }

    uint32_t deps_of(const IrInst& i)
    {
        uint32_t d = 0;
        for (const Val* v : {&i.a, &i.b})
            if (v->kind == Val::Temp)
                d |= operand(*v).deps;
        return d;
    }

    void op(size_t n)
    {
        const IrInst& i = b.code[n];
        const std::string S = std::to_string(i.size);
        if (dropped.count(n))
            return;
        if (n == record_at) {
            steady(n);
            if (i.op == Ir::CmpF) note(i, i.a, i.b);
            else if (i.op == Ir::LogicF) note(i, Val{}, i.a);
        }
        // A flag operation whose flags nothing reads: only its value.
        if (!flags_live(n) && (i.op == Ir::AddF || i.op == Ir::SubF || i.op == Ir::LogicF)) {
            const uint32_t d = deps_of(i);
            const std::string mask = i.size == 4 ? "" : " & " + num(mask_of(i.size));
            if (i.op == Ir::LogicF) {
                const Expr e = i.a.kind == Val::Temp ? operand(i.a) : pure(num(i.a.n), 0);
                const std::string a = value(i.a);
                Expr r = e;
                if (!mask.empty() && needs(i.dst, mask_of(i.size))) r.s = "(" + a + mask + ")";
                define(n, i.dst, r);
                return;
            }
            uint32_t eff = 0;
            bool imp = false;
            for (const Val* v : {&i.a, &i.b})
                if (v->kind == Val::Temp) { eff |= operand(*v).effects; imp |= operand(*v).impure; }
            const std::string a = value(i.a), c2 = value(i.b);
            std::string s = "(" + c2 + (i.op == Ir::AddF ? " + " : " - ") + a + ")";
            if (!mask.empty() && needs(i.dst, mask_of(i.size))) s = "(" + s + mask + ")";
            define(n, i.dst, Expr{s, d, eff, imp});
            return;
        }
        // A statement: what it changes settles what depends on that first.
        auto stmt = [&](uint32_t effects, const std::function<std::string()>& text) {
            uint32_t e = effects;
            for (const Val* v : {&i.a, &i.b})
                if (v->kind == Val::Temp)
                    e |= operand(*v).effects;
            settle(e, &i);
            line(text());
        };
        if (yields(i.op)) {
            const uint32_t d = deps_of(i);
            switch (i.op) {
            case Ir::Mov: { Expr e = i.a.kind == Val::Temp ? operand(i.a) : pure(num(i.a.n), 0); value(i.a); define(n, i.dst, e); return; }
            case Ir::GetReg:
                if (const Local* l = local_read(int(i.k), n)) { define(n, i.dst, pure(l->name, 0)); return; }
                define(n, i.dst, pure(kReg[i.k], 1u << i.k));
                return;
            case Ir::Add: define(n, i.dst, pure(binop(i, "+"), d)); return;
            case Ir::Sub: define(n, i.dst, pure(binop(i, "-"), d)); return;
            case Ir::And:
                if (i.b.kind == Val::Imm && !needs(i.dst, i.b.n)) {     // (keeps every bit used)
                    Expr e = i.a.kind == Val::Temp ? operand(i.a) : pure(num(i.a.n), 0);
                    value(i.a);
                    define(n, i.dst, e);
                    return;
                }
                define(n, i.dst, pure(binop(i, "&"), d));
                return;
            case Ir::Or: define(n, i.dst, pure(binop(i, "|"), d)); return;
            case Ir::Xor: define(n, i.dst, pure(binop(i, "^"), d)); return;
            case Ir::Shl: define(n, i.dst, pure(binop(i, "<<"), d)); return;
            case Ir::Shr: define(n, i.dst, pure(binop(i, ">>"), d)); return;
            case Ir::Eq: define(n, i.dst, pure(binop(i, "=="), d)); return;
            case Ir::Ne: define(n, i.dst, pure(binop(i, "!="), d)); return;
            case Ir::Ltu: define(n, i.dst, pure(binop(i, "<"), d)); return;
            case Ir::MulU16: { const std::string a = value(i.a), c2 = value(i.b);
                define(n, i.dst, pure("((" + a + " & 0xFFFF) * (" + c2 + " & 0xFFFF))", d)); return; }
            case Ir::MulS16: { const std::string a = value(i.a), c2 = value(i.b);
                define(n, i.dst, pure("(uint32_t)((int32_t)(int16_t)" + a + " * (int32_t)(int16_t)" + c2 + ")", d)); return; }
            case Ir::Sext: { const std::string a = value(i.a);
                define(n, i.dst, pure(i.size == 1 ? "sx8(" + a + ")" : i.size == 2 ? "sx16(" + a + ")" : a, d)); return; }
            case Ir::Read: { const std::string a = i.a.kind == Val::Imm ? address_of(i.a.n) : value(i.a);
                define(n, i.dst, helper("read" + std::to_string(i.size * 8) + "(" + strip(a) + ")", d | kMem, kMem)); return; }
            case Ir::GetSrHigh: define(n, i.dst, pure("(uint32_t)c->sr_high", kSr)); return;
            case Ir::GetUsp: define(n, i.dst, pure("c->usp", kSr)); return;
            case Ir::AddF: case Ir::SubF: case Ir::AddxF: case Ir::SubxF: {
                const char* f = i.op == Ir::AddF ? "add_flags" : i.op == Ir::SubF ? "sub_flags" : i.op == Ir::AddxF ? "addx_flags" : "subx_flags";
                const std::string a = value(i.a), c2 = value(i.b);
                define(n, i.dst, helper(std::string(f) + "(" + S + ", " + strip(a) + ", " + strip(c2) + ")", d | kFlags, kFlags));
                if (n == record_at && i.op == Ir::SubF) {   // (the result, kept for the branch)
                    if (!env.count(i.dst)) return;
                    materialize(i.dst, env.at(i.dst));
                    record = FlagRecord{true, Ir::SubF, i.size, "", env.at(i.dst).s, false, false, 0, 0};
                }
                return;
            }
            case Ir::LogicF: { const std::string a = value(i.a);
                define(n, i.dst, helper("test_flags(" + S + ", " + strip(a) + ")", d, kFlags)); return; }
            case Ir::ShiftF: { const std::string a = value(i.a), c2 = value(i.b);
                define(n, i.dst, helper("shift_flags(" + std::to_string(i.k) + ", " + S + ", " + strip(a) + ", " + strip(c2) + ")", d | kFlags, kFlags));
                return; }
            case Ir::Cond: define(n, i.dst, pure(kCond[i.k & 15], kFlags)); return;
            case Ir::GetCcr: define(n, i.dst, pure("CCR()", kFlags)); return;
            case Ir::MuluCycles: { const std::string a = value(i.a); define(n, i.dst, pure("(uint32_t)rt_mulu_cycles(" + strip(a) + ")", d)); return; }
            case Ir::MulsCycles: { const std::string a = value(i.a); define(n, i.dst, pure("(uint32_t)rt_muls_cycles(" + strip(a) + ")", d)); return; }
            case Ir::Divu: case Ir::Divs: { const std::string a = value(i.a);
                define(n, i.dst, helper(std::string(i.op == Ir::Divu ? "rt_divu" : "rt_divs") + "(c, " + std::to_string(i.k) + ", " + strip(a) + ")",
                                        d | kFlags | (1u << i.k), kFlags | (1u << i.k)));
                return; }
            case Ir::Pop32: define(n, i.dst, helper("pop32()", kMem | (1u << 15), 1u << 15)); return;
            case Ir::Interrupt: { const std::string a = value(i.a); settle(kAll, &i);
                define(n, i.dst, helper("coi_native_interrupt(c, " + strip(a) + ")", kAll, kAll)); return; }
            case Ir::Exception: { const std::string a = value(i.a); settle(kAll, &i);
                define(n, i.dst, helper("coi_native_exception(c, " + num(i.k) + ", " + strip(a) + ", " + num(i.k2) + ")", kAll, kAll)); return; }
            case Ir::StopCpu: { const std::string a = value(i.a), c2 = value(i.b); settle(kAll, &i);
                define(n, i.dst, helper("coi_native_stop(c, " + strip(a) + ", " + strip(c2) + ")", kAll, kAll)); return; }
            case Ir::IrqLevel: define(n, i.dst, helper("coi_native_irq_level()", kAll, 0)); return;
            default: break;
            }
        }
        auto r = [&](const std::string& s) { return strip(s); };
        switch (i.op) {
        case Ir::SetReg:
            if (const Local* l = local_defined(int(i.k), n)) {
                stmt(0, [&] { return "const uint32_t " + l->name + " = " + r(value(i.a)) + ";"; });
                break;
            }
            stmt(1u << i.k, [&] { return std::string(kReg[i.k]) + " = " + r(value(i.a)) + ";"; });
            break;
        case Ir::SetRegPart:
            if (const Local* l = local_defined(int(i.k), n)) {
                stmt(0, [&] { return "const uint32_t " + l->name + " = " + r(value(i.a)) + ";"; });
                break;
            }
            stmt(1u << i.k, [&] {
                const std::string v = r(value(i.a));
                return i.size == 1 ? "set_b(" + std::string(kReg[i.k]) + ", " + v + ");"
                     : i.size == 2 ? "set_w(" + std::string(kReg[i.k]) + ", " + v + ");"
                                   : std::string(kReg[i.k]) + " = " + v + ";";
            });
            break;
        case Ir::Write: stmt(kMem, [&] { const std::string a = i.a.kind == Val::Imm ? address_of(i.a.n) : r(value(i.a)), v = r(value(i.b));
            return "write" + std::to_string(i.size * 8) + "(" + a + ", " + v + ");"; }); break;
        case Ir::CmpF: stmt(kFlags, [&] { const std::string a = r(value(i.a)), v = r(value(i.b));
            return "cmp_flags(" + S + ", " + a + ", " + v + ");"; }); break;
        case Ir::SetCcr: stmt(kFlags, [&] { return "set_ccr(" + r(value(i.a)) + ");"; }); break;
        case Ir::SetSrHigh: stmt(kSr, [&] { return "c->sr_high = (uint16_t)(" + r(value(i.a)) + ");"; }); break;
        case Ir::SetSr: stmt(kAll, [&] { return "rt_set_sr(c, " + r(value(i.a)) + ");"; }); break;
        case Ir::SetUsp: stmt(kSr, [&] { return "c->usp = " + r(value(i.a)) + ";"; }); break;
        case Ir::SetZ: stmt(kFlags, [&] { const std::string a = value(i.a), v = value(i.b);
            return "c->z = (" + a + " & " + v + ") == 0;"; }); break;
        case Ir::Push32:
            // The return address of the call right after it: the call pushes it itself.
            if (i.a.kind == Val::Imm && n + 1 < b.code.size() && b.code[n + 1].op == Ir::Call && b.code[n + 1].k2 == i.a.n) {
                pushed_for_call = true;
                break;
            }
            stmt(kMem | (1u << 15), [&] { return "push32(" + r(value(i.a)) + ");"; });
            break;
        case Ir::Cycles: cycles += m68ki_cycles[0][i.k & 0xFFFF]; cycles_pending = true; break;
        case Ir::AddCycles:
            if (uint32_t k = 0; known(i.a, k)) { if (i.a.kind == Val::Temp) value(i.a); cycles += int32_t(k); cycles_pending = true; }
            else stmt(0, [&] { return "CYCLES((int32_t)" + value(i.a) + ");"; });
            break;
        case Ir::Spin: stmt(kAll, [&] { return "rt_spin(c, " + num(i.k) + ");"; }); break;
        case Ir::Idle: stmt(kAll, [&] { const std::string a = r(value(i.a)), v = r(value(i.b));
            return "RT_IDLE(c, " + a + ", " + v + ", " + S + ", " + num(i.k) + ", " + num(i.k2) + ");"; }); break;
        case Ir::Label: line("L_" + hex6(b.address) + "_" + std::to_string(i.k) + ": ;"); break;
        case Ir::Br: line("goto L_" + hex6(b.address) + "_" + std::to_string(i.k) + ";"); break;
        case Ir::BrIf: { const std::string a = r(value(i.a)); line("if (" + a + ") goto L_" + hex6(b.address) + "_" + std::to_string(i.k) + ";"); break; }
        case Ir::BrIfNot: { const std::string a = value(i.a); line("if (!" + a + ") goto L_" + hex6(b.address) + "_" + std::to_string(i.k) + ";"); break; }
        case Ir::Goto: line("goto " + label(i.k) + ";"); break;
        case Ir::GotoIf: { const std::string a = r(value(i.a)); line("if (" + a + ") goto " + label(i.k) + ";"); break; }
        case Ir::Trace: break;                      // (the instruction's AT line)
        case Ir::Limit: if (i.k != b.address) line("RT_LIMIT(c, " + addr(i.k) + ");"); break;
        case Ir::Stop: line("RT_STOP(c, " + addr(i.k) + ", " + num(i.k2) + ");"); break;
        case Ir::SetMissing: line("c->missing = " + addr(i.k) + ";"); break;
        case Ir::Return: { const std::string a = r(value(i.a)); settle(kAll, nullptr); line("return " + a + ";"); break; }
        case Ir::JumpOut: line("JUMP(" + addr(i.k) + ");"); break;
        case Ir::JumpDyn: { const std::string a = r(value(i.a)); line("JUMP(" + a + ");"); break; }
        case Ir::Call:
            settle(kAll, nullptr);
            line(std::string(pushed_for_call ? "CALL(" : "CALLED(") + callee(i.k) + ", " + addr(i.k2) + ");");
            pushed_for_call = false;
            break;
        default: line("/* ? */"); break;
        }
    }
};

// One instruction, and how it leaves: falls through, branches (conditionally), or ends.
struct Node {
    uint32_t address = 0;
    const Insn* in = nullptr;
    IrBlock ir;
    enum Kind { Plain, Branch, DbLoop, Jump, End } kind = Plain;
    uint32_t target = 0;
    size_t split = 0;       // Branch: the GotoIf's index (before: its own code; after: not taken)
    std::vector<int> preds;
    bool calls = false;
    std::vector<uint32_t> live;     // per operation: the flags it writes that may still be read
    const Patch* patch = nullptr;   // a hand-written patch in this instruction's place (setting on)
    const Patch* value = nullptr;   // a named value in it (its temporary: value_temp)
    uint32_t value_temp = 0;
    int cover = 0;                  // a patch of several instructions: how many after this one it replaces
    bool covered = false;           // one of those (the game's code there, only where the setting is off)
};

class Function_ {
public:
    Function_(const Program& program, const Function& f, const ReadableNames& names, std::vector<std::string>& out,
              std::string suffix = "", std::vector<std::pair<uint32_t, std::string>> dispatch = {}, uint32_t own_bits = 0)
        : program(program), f(f), names(names), out(out), suffix(std::move(suffix)), dispatch(std::move(dispatch))
    {
        // (a setting with a variant of this function, or this variant's own: no patch of its own here)
        for (const auto& [bit, s] : this->dispatch) own_bits |= 1u << bit;
        for (const auto& [a, in] : f.insns) {
            Node n;
            n.address = a;
            n.in = &in;
            n.ir = lower(program, in);
            if (auto p = names.patches.find(a); p != names.patches.end() && !(own_bits & (1u << p->second.bit))) {
                // (only where the instruction is the one the patch replaces)
                const auto& rom = program.rom().bytes;
                bool same = uint32_t(in.length) <= p->second.original.size();
                for (size_t q = 0; same && q < p->second.original.size(); q++)
                    same = a + q < rom.size() && rom[a + q] == p->second.original[q];
                if (same && p->second.is_value()) {
                    n.value = &p->second;
                    name_value(n.ir, p->second, n.value_temp);
                } else if (same) {
                    if (ends_block(in) || n.ir.code.empty())
                        throw std::runtime_error("a patch at $" + hex6(a) + " replaces an instruction that doesn't fall through");
                    n.patch = &p->second;
                }
            }
            for (const IrInst& i : n.ir.code)
                if (i.op == Ir::Call) n.calls = true;
            if (in.op == Op::Bcc && !(in.target & 1)) {
                for (size_t k = 0; k < n.ir.code.size(); k++)
                    if (n.ir.code[k].op == Ir::GotoIf) { n.kind = Node::Branch; n.split = k; n.target = n.ir.code[k].k; }
            } else if (in.op == Op::DBcc && !(in.target & 1)) {
                n.kind = Node::DbLoop;
                n.target = in.target;
            } else if (in.op == Op::Bra && !(in.target & 1)) {
                n.kind = Node::Jump;
                n.target = in.target;
            } else if (ends_block(in)) {
                n.kind = Node::End;
            }
            index[a] = int(nodes.size());
            nodes.push_back(std::move(n));
        }
        // Predecessors: falling through, and every branch inside an instruction's code.
        for (size_t k = 0; k < nodes.size(); k++) {
            Node& n = nodes[k];
            const bool falls = n.kind != Node::Jump && n.kind != Node::End && k + 1 < nodes.size()
                               && nodes[k + 1].address == n.address + uint32_t(n.in->length);
            if (falls)
                nodes[k + 1].preds.push_back(int(k));
            for (const IrInst& i : n.ir.code)
                if ((i.op == Ir::Goto || i.op == Ir::GotoIf) && index.count(i.k))
                    nodes[index.at(i.k)].preds.push_back(int(k));
        }
        // The entry is entered from outside (it need not be the lowest address: code can loop
        // back into instructions before it).
        nodes[size_t(index.at(f.entry))].preds.push_back(-1);
        // A patch in place of several instructions: the ones after its first, straight code that
        // only it falls into, are the game's code where the setting is off (as one with it).
        for (size_t k = 0; k < nodes.size(); k++) {
            Node& n = nodes[k];
            if (!n.patch || n.patch->leaves) continue;
            const uint32_t end = n.address + uint32_t(n.patch->original.size());
            for (uint32_t a = n.address + uint32_t(n.in->length); a < end;) {
                const int j = at(a);
                if (j != int(k) + n.cover + 1 || nodes[size_t(j)].kind != Node::Plain || nodes[size_t(j)].calls
                    || nodes[size_t(j)].preds != std::vector<int>{j - 1} || nodes[size_t(j)].patch || nodes[size_t(j)].value
                    || ends_block(*nodes[size_t(j - 1)].in))
                    throw std::runtime_error("the patch at $" + hex6(n.address) + " replaces $" + hex6(a)
                                             + ", which isn't straight code only it falls into");
                nodes[size_t(j)].covered = true;
                n.cover++;
                a += uint32_t(nodes[size_t(j)].in->length);
            }
        }
        // The budget's stretches, as the JIT has them: from the entry, every branch target, after
        // anything that doesn't fall through, after calls; each with the most cycles it can use.
        leaders.insert(f.entry);
        for (size_t k = 0; k < nodes.size(); k++) {
            const Node& n = nodes[k];
            if (k == 0 || nodes[k - 1].address + uint32_t(nodes[k - 1].in->length) != n.address
                || ends_block(*nodes[k - 1].in) || nodes[k - 1].calls)
                leaders.insert(n.address);
            for (const IrInst& i : n.ir.code)
                if (i.op == Ir::Goto || i.op == Ir::GotoIf) leaders.insert(i.k);
            if (n.patch && n.patch->calls && !n.patch->leaves)     // (after a patch's call: as after any)
                leaders.insert(n.address + uint32_t(n.patch->original.size()));
        }
        // Flags: within a stretch, a flag the next flag operation overwrites before anything can
        // read it isn't needed; at a stretch's ends (where the code can stop, or be entered) all
        // are, so wherever the 68000 could be seen, its flags are exact (as the JIT has it).
        for (size_t k = nodes.size(); k-- > 0;) {
            uint32_t live = kAllFlags;
            size_t first = k;
            while (first > 0 && !leaders.count(nodes[first].address)) first--;
            for (size_t j = k + 1; j-- > first;) {
                Node& n = nodes[j];
                n.live.assign(n.ir.code.size(), kAllFlags);
                for (size_t m = n.ir.code.size(); m-- > 0;) {
                    const IrInst& i = n.ir.code[m];
                    if (i.op == Ir::Limit && i.k == n.address)
                        continue;                       // (the stretch's check stands for it)
                    const uint32_t w = flags_written(i.op);
                    if (w) {
                        n.live[m] = live & w;
                        live &= ~w;
                    } else if (!flag_neutral(i.op)) {
                        live = kAllFlags;
                    }
                }
                if (n.patch) live = kAllFlags;          // (a patch may leave any flag as it was)
            }
            k = first;
            if (k == 0) break;
        }
        uint32_t leader = f.entry;
        for (const Node& n : nodes) {
            if (leaders.count(n.address)) leader = n.address;
            uint32_t m = 0;
            for (const IrInst& i : n.ir.code)
                if (i.op == Ir::Cycles) m += m68ki_cycles[0][i.k & 0xFFFF];
                else if (i.op == Ir::AddCycles) m += i.a.kind == Val::Imm ? (int32_t(i.a.n) > 0 ? i.a.n : 0) : 128;
            if (n.patch) m = std::max<uint32_t>(m, uint32_t(n.patch->cycles));
            most[leader] += m;
        }
        find_locals();
    }

    void emit()
    {
        const std::string name = names.function(f.entry) + suffix;
        if (auto it = names.function_notes.find(f.entry); it != names.function_notes.end())
            out.push_back("/* " + name + ": " + it->second + (suffix.empty() ? "" : " (with " + suffix.substr(2) + ")") + " */");
        else if (!suffix.empty())
            out.push_back("/* " + name + ": " + names.function(f.entry) + " with " + suffix.substr(2) + " on */");
        out.push_back("static uint32_t " + name + "(CoiCpu* c, uint32_t at)");
        out.push_back("{");
        for (const auto& [bit, s] : dispatch)       // (a setting's own version of this function)
            out.push_back("    if (coi_native_settings & " + num(1u << bit) + ") return " + names.function(f.entry) + s + "(c, at);");
        out.push_back("    if (at)");
        out.push_back("        return coi_rs_" + hex6(f.entry) + suffix + "(c, at);");
        const size_t start = out.size();
        if (nodes[0].address != f.entry)
            out.push_back("    goto " + label_of(f.entry) + ";");
        region(0, int(nodes.size()), 1, -1);
        out.push_back("}");
        // Labels only where something still jumps.
        for (size_t k = start; k < out.size(); k++)
            if (out[k].rfind("@@L", 0) == 0) {
                const uint32_t a = std::stoul(out[k].substr(3), nullptr, 16);
                out[k] = gotos.count(a) ? "L_" + hex6(a) + ":" : "@@drop";
            }
        out.erase(std::remove(out.begin() + std::ptrdiff_t(start), out.end(), "@@drop"), out.end());
        out.push_back("");
    }

private:
    const Program& program;
    const Function& f;
    const ReadableNames& names;
    std::vector<std::string>& out;
    std::string suffix;                                         // (a setting's version: "__name")
    std::vector<std::pair<uint32_t, std::string>> dispatch;     // (the base: its settings' versions)
    std::vector<Node> nodes;
    std::map<uint32_t, int> index;
    std::set<uint32_t> leaders, gotos;
    std::map<uint32_t, uint32_t> most;
    int vars = 0;

    static std::string pad(int depth) { return std::string(size_t(depth) * 4, ' '); }

    std::string label_of(uint32_t a)
    {
        gotos.insert(a);
        return "L_" + hex6(a);
    }

    // All of nodes [lo, hi]'s predecessors are in [from, to].
    bool closed(int lo, int hi, int from, int to) const
    {
        for (int k = lo; k <= hi; k++)
            for (int p : nodes[size_t(k)].preds)
                if (p < from || p > to)
                    return false;
        return true;
    }

    int at(uint32_t a) const { auto it = index.find(a); return it == index.end() ? -1 : it->second; }

    // The instruction's header (its budget check, its address and assembly), then its code up to
    // `to` (all of it, or up to its branch).
    // An instruction nothing outside can observe: no memory, no calls, nothing that can stop or
    // leave. Its address isn't set for the port (only its assembly shown), and its cycles are
    // added with the next instruction's.
    static bool quiet(const Node& n)
    {
        if (n.kind != Node::Plain || n.patch || n.covered) return false;
        for (const IrInst& i : n.ir.code)
            switch (i.op) {
            case Ir::Trace: case Ir::Cycles: case Ir::AddCycles: case Ir::Mov: case Ir::GetReg: case Ir::Add: case Ir::Sub:
            case Ir::And: case Ir::Or: case Ir::Xor: case Ir::Shl: case Ir::Shr: case Ir::Eq: case Ir::Ne: case Ir::Ltu:
            case Ir::MulU16: case Ir::MulS16: case Ir::Sext: case Ir::SetReg: case Ir::SetRegPart: case Ir::AddF: case Ir::SubF:
            case Ir::AddxF: case Ir::SubxF: case Ir::LogicF: case Ir::ShiftF: case Ir::CmpF: case Ir::Cond: case Ir::GetCcr:
            case Ir::SetCcr: case Ir::SetZ: case Ir::MuluCycles: case Ir::MulsCycles:
                break;
            case Ir::Limit:
                if (i.k != n.address) return false;
                break;
            default:
                return false;
            }
        return true;
    }

    int64_t carry = 0;
    void flush_carry(int depth)
    {
        if (carry) out.push_back(pad(depth) + "CYCLES(" + std::to_string(carry) + ");");
        carry = 0;
    }

    void head(const Node& n, int depth)
    {
        out.push_back("@@L" + hex6(n.address));
        if (leaders.count(n.address))
            out.push_back(pad(depth) + "BUDGET(" + std::to_string(most.at(n.address)) + ", " + addr(n.address) + ", coi_rs_"
                          + hex6(f.entry) + suffix + ");");
        out.push_back(pad(depth) + "AT(" + addr(n.address) + ");" + std::string(names.width(n.address), ' ') + "/* "
                      + names.disassemble(n.address) + " */");
    }

    // A node where a loop could start (a branch back to it): loops are leaders anyway.
    bool at_loop_head(int, int, int) const { return false; }

    // Register values only the next few instructions read, as C locals (struct Local). From the
    // instruction that writes a register in full to the next that does, within straight code
    // (each instruction's only way in is the one before, no stretch starts between), where
    // nothing between can see the CPU's registers: no call, return, jump, stop or check, no
    // stack access, and for A0 no memory write (the port's frame interpolation reads A0 as the
    // game writes the sprite list). The register keeps its old value meanwhile; the write that
    // ends it is exact, as are all registers at every point the 68000 can be seen.
    std::map<uint32_t, std::vector<Local>> locals;
    std::map<int, int> local_count;

    // An operation that leaves the CPU's registers unseen (for register k: whether it reads or
    // writes it otherwise than in full is checked by the caller).
    static bool unseen(const Node& n, const IrInst& i, int k)
    {
        switch (i.op) {
        case Ir::Trace: case Ir::Cycles: case Ir::AddCycles: case Ir::Mov: case Ir::GetReg: case Ir::Add: case Ir::Sub:
        case Ir::And: case Ir::Or: case Ir::Xor: case Ir::Shl: case Ir::Shr: case Ir::Eq: case Ir::Ne: case Ir::Ltu:
        case Ir::MulU16: case Ir::MulS16: case Ir::Sext: case Ir::SetReg: case Ir::SetRegPart: case Ir::AddF: case Ir::SubF:
        case Ir::AddxF: case Ir::SubxF: case Ir::LogicF: case Ir::ShiftF: case Ir::CmpF: case Ir::Cond: case Ir::GetCcr:
        case Ir::SetCcr: case Ir::SetZ: case Ir::MuluCycles: case Ir::MulsCycles: case Ir::Read:
            return true;
        case Ir::Write:
            return k != 8;
        case Ir::Limit:
            return i.k == n.address;
        default:
            return false;
        }
    }

    void find_locals()
    {
        auto full = [](const IrInst& i, int k) {
            return int(i.k) == k && (i.op == Ir::SetReg || (i.op == Ir::SetRegPart && i.size == 4));
        };
        auto touches = [](const IrInst& i, int k) {     // (otherwise than a GetReg or a full write)
            return int(i.k) == k && (i.op == Ir::SetRegPart || i.op == Ir::Divu || i.op == Ir::Divs);
        };
        for (size_t x = 0; x < nodes.size(); x++) {
            const Node& nx = nodes[x];
            if (nx.kind != Node::Plain || nx.calls || nx.patch || nx.covered) continue;
            bool calm = true;
            for (const IrInst& i : nx.ir.code) calm = calm && unseen(nx, i, -1);
            if (!calm) continue;
            for (int k = 0; k < 16; k++) {
                long m = -1;
                for (size_t q = 0; q < nx.ir.code.size(); q++)
                    if (full(nx.ir.code[q], k)) m = long(q);
                if (m < 0) continue;
                bool ok = true;
                int reads = 0;
                for (size_t q = size_t(m) + 1; ok && q < nx.ir.code.size(); q++) {
                    const IrInst& i = nx.ir.code[q];
                    ok = unseen(nx, i, k) && !touches(i, k);
                    reads += i.op == Ir::GetReg && int(i.k) == k;
                }
                // The instructions after, up to the next full write.
                std::vector<std::pair<size_t, long>> span;  // (node, its full write's index or -1)
                long z = -1;
                for (size_t j = x + 1; ok && z < 0 && j < nodes.size(); j++) {
                    const Node& nj = nodes[j];
                    if (nj.kind != Node::Plain || nj.calls || nj.patch || nj.covered || leaders.count(nj.address) || nj.preds.size() != 1
                        || nj.preds[0] != int(j - 1)) {
                        ok = false;
                        break;
                    }
                    for (size_t q = 0; ok && q < nj.ir.code.size(); q++) {
                        const IrInst& i = nj.ir.code[q];
                        if (full(i, k)) { z = long(q); break; }
                        ok = unseen(nj, i, k) && !touches(i, k);
                        reads += i.op == Ir::GetReg && int(i.k) == k;
                    }
                    span.push_back({j, z});
                }
                if (!ok || z < 0 || reads == 0) continue;
                static const char* lower[] = {"d0", "d1", "d2", "d3", "d4", "d5", "d6", "d7",
                                              "a0", "a1", "a2", "a3", "a4", "a5", "a6", "sp"};
                const std::string name = std::string(lower[k]) + "_" + std::to_string(++local_count[k]);
                locals[nx.address].push_back(Local{k, m, long(nx.ir.code.size()), m, name});
                for (const auto& [j, w] : span)
                    locals[nodes[j].address].push_back(Local{k, -1, w < 0 ? long(nodes[j].ir.code.size()) : w, -1, name});
            }
        }
    }

    Renderer renderer(const Node& n, int depth)
    {
        auto l = locals.find(n.address);
        Renderer r(n.ir, out, [this](uint32_t fn) { return names.function(fn); },
                   [this](uint32_t a) { return label_of(a); }, pad(depth), vars, &n.live,
                   l == locals.end() ? nullptr : &l->second);
        r.carry(carry);                         // (what the instructions before held over)
        carry = 0;
        if (n.value) r.preset(n.value_temp, n.value->value_name);
        r.dispatch = (n.in->op == Op::Jmp || n.in->op == Op::Jsr) && !static_target(*n.in);   // (computed)
        r.loads_address = (n.in->op == Op::Lea || n.in->op == Op::Pea)
                          && (n.in->src.mode == Mode::AbsW || n.in->src.mode == Mode::AbsL || n.in->src.mode == Mode::PcDisp);
        return r;
    }

    // The condition a branch node takes, rendered with its own code before it.
    std::string condition(const Node& n, int depth, Renderer& r)
    {
        if (n.kind == Node::DbLoop) {
            // Its code up to the dbcc itself (the trace, cycles), then the test as one call.
            size_t k = 0;
            while (k < n.ir.code.size() && n.ir.code[k].op != Ir::BrIf) k++;
            r.run(0, k);
            const int cond = n.in->cond, reg = n.in->dst.reg;
            return cond == 1 ? "dbra(" + std::to_string(reg) + ")" : "dbcc(" + std::to_string(cond) + ", " + std::to_string(reg) + ")";
        }
        // A test right after the instruction whose flags it reads (same stretch): as a comparison.
        const int me = at(n.address);
        std::string compared;
        if (!leaders.count(n.address) && me > 0 && last_node == me - 1 && last.valid) {
            bool plain = true;
            uint32_t cond = 0xFFFF;
            for (size_t m = 0; m < n.split; m++) {
                const IrInst& i = n.ir.code[m];
                plain = plain && !sets_flags(i.op) && i.op != Ir::Idle;
                if (i.op == Ir::Cond && i.dst == n.ir.code[n.split].a.n && n.ir.code[n.split].a.kind == Val::Temp) cond = i.k;
            }
            for (size_t m = n.split; m < n.ir.code.size(); m++)
                plain = plain && n.ir.code[m].op != Ir::Idle;
            if (plain && cond != 0xFFFF)
                compared = comparison(last, cond);
        }
        r.run(0, n.split);
        const std::string v = r.value(n.ir.code[n.split].a);
        return compared.empty() ? v : compared;
    }

    FlagRecord last;            // the last plain node's flag record, and which node it was
    int last_node = -1;

    // Nodes [from, to) at `depth`. `loop_at`: the node whose loop is being emitted (not again).
    void region(int from, int to, int depth, int loop_at)
    {
        int k = from;
        while (k < to) {
            const Node& n = nodes[size_t(k)];
            if (quiet(n) && !leaders.count(n.address)) {
                out.push_back("@@L" + hex6(n.address));
                out.push_back(pad(depth) + "/* " + addr(n.address) + ": " + names.disassemble(n.address) + " */");
                Renderer r = renderer(n, depth);
                r.hold = true;
                r.carry(carry);
                carry = 0;
                r.run(0, n.ir.code.size());
                carry = r.held();
                last = r.record;
                last_node = k;
                k++;
                continue;
            }
            if (leaders.count(n.address) || n.kind == Node::DbLoop || at_loop_head(k, to, loop_at))
                flush_carry(depth);
            // A loop starting here: the last node in this region that branches back to it, with
            // nothing entering the loop from outside except at its start.
            if (k != loop_at) {
                int e = -1;
                for (int j = to - 1; j >= k && e < 0; j--) {
                    const Node& m = nodes[size_t(j)];
                    if ((m.kind == Node::Branch || m.kind == Node::DbLoop || m.kind == Node::Jump) && m.target == n.address)
                        e = j;
                }
                if (e >= 0 && closed(k + 1, e, k, e)) {
                    const Node& end = nodes[size_t(e)];
                    if (end.kind == Node::Jump) {
                        out.push_back(pad(depth) + "for (;;) {");
                        region(k, e, depth + 1, k);
                        head(end, depth + 1);
                        Renderer r = renderer(end, depth + 1);
                        r.run(0, end.ir.code.size() - 1);       // (all but its Goto)
                        out.push_back(pad(depth) + "}");
                    } else {
                        const size_t open = out.size();
                        const int first_var = vars + 1;
                        out.push_back(pad(depth) + "do {");
                        region(k, e, depth + 1, k);
                        head(end, depth + 1);
                        Renderer r = renderer(end, depth + 1);
                        const std::string c = condition(end, depth + 1, r);
                        // A condition on a variable of the loop's own can't be the while's (C's
                        // scopes): then the loop ends with a break instead.
                        bool inner = false;
                        for (size_t p = c.find('v'); p != std::string::npos && !inner; p = c.find('v', p + 1))
                            if ((p == 0 || !std::isalnum((unsigned char)c[p - 1])) && p + 1 < c.size()
                                && std::isdigit((unsigned char)c[p + 1]) && std::stoi(c.substr(p + 1)) >= first_var)
                                inner = true;
                        if (inner) {
                            out[open] = pad(depth) + "for (;;) {";
                            out.push_back(pad(depth + 1) + "if (" + negate(c) + ")");
                            out.push_back(pad(depth + 2) + "break;");
                            out.push_back(pad(depth) + "}");
                        } else {
                            out.push_back(pad(depth) + "} while (" + c + ");");
                        }
                        if (end.kind == Node::Branch) {
                            Renderer t = renderer(end, depth);
                            t.hold = true;
                            t.run(end.split + 1, end.ir.code.size());   // not taken
                            carry += t.held();
                        }
                    }
                    k = e + 1;
                    continue;
                }
            }
            head(n, depth);
            if (n.patch) {
                // The setting's hand-written C in the instruction's place (config/patches.txt); the
                // game's own code as the other way, or after it where the C leaves the function.
                const Patch& p = *n.patch;
                flush_carry(depth);
                out.push_back(pad(depth) + "if (SETTING(" + p.setting + ")) {" + (p.note.empty() ? "" : "  /* " + p.note + " */"));
                bool called = false;
                for (std::string l : p.lines) {
                    // (a STEP after a call: the budget checked again there)
                    for (size_t q = l.find("STEP("); called && q != std::string::npos; q = l.find("STEP(", q + 1)) {
                        const size_t e = l.find(')', q);
                        l = l.substr(0, q) + "STEP_AFTER_CALL(" + l.substr(q + 5, e - q - 5) + ", coi_rs_" + hex6(f.entry) + suffix
                            + ")" + l.substr(e + 1);
                    }
                    called = called || l.find("CALL(") != std::string::npos;
                    out.push_back(l.empty() ? "" : pad(depth + 1) + l);
                }
                if (p.leaves) {
                    out.push_back(pad(depth) + "}");
                    Renderer r = renderer(n, depth);
                    r.run(0, n.ir.code.size());
                    last = r.record;
                } else {
                    out.push_back(pad(depth) + "} else {");
                    Renderer e = renderer(n, depth + 1);
                    e.run(0, n.ir.code.size());
                    for (int j = 1; j <= n.cover; j++) {    // (the rest of the game's instructions it replaces)
                        const Node& m = nodes[size_t(k + j)];
                        head(m, depth + 1);
                        Renderer em = renderer(m, depth + 1);
                        em.run(0, m.ir.code.size());
                    }
                    out.push_back(pad(depth) + "}");
                    last = FlagRecord{};        // (either way: the flags, not a comparison)
                }
                last_node = k + n.cover;
                k += 1 + n.cover;
                continue;
            }
            Renderer r = renderer(n, depth);
            if (n.kind == Node::Branch || n.kind == Node::DbLoop) {
                const std::string c = condition(n, depth, r);
                const int t = at(n.target);
                // Forward over a stretch nothing else enters: if (!c) { ... }.
                if (t > k && t <= to && closed(k + 1, t - 1, k, t - 1)) {
                    const Node* last = t - 1 > k ? &nodes[size_t(t - 1)] : nullptr;
                    const int u = last && last->kind == Node::Jump ? at(last->target) : -1;
                    const bool has_else = u > t && u <= to && closed(t, t, k, k) && closed(t + 1, u - 1, t, u - 1);
                    out.push_back(pad(depth) + "if (" + negate(c) + ") {");
                    if (n.kind == Node::Branch) {
                        Renderer nt = renderer(n, depth + 1);
                        nt.hold = true; nt.carry(carry); carry = 0; nt.run(n.split + 1, n.ir.code.size()); carry = nt.held();
                    }
                    if (has_else) {
                        region(k + 1, t - 1, depth + 1, -1);
                        head(*last, depth + 1);
                        Renderer lr = renderer(*last, depth + 1);
                        lr.run(0, last->ir.code.size() - 1);
                        out.push_back(pad(depth) + "} else {");
                        region(t, u, depth + 1, -1);
                        out.push_back(pad(depth) + "}");
                        k = u;
                    } else {
                        region(k + 1, t, depth + 1, -1);
                        out.push_back(pad(depth) + "}");
                        k = t;
                    }
                    continue;
                }
                if (n.kind == Node::DbLoop) {
                    out.push_back(pad(depth) + "if (" + c + ")");
                    out.push_back(pad(depth + 1) + "goto " + label_of(n.target) + ";");
                } else {
                    out.push_back(pad(depth) + "if (" + strip_parens(c) + ")");
                    out.push_back(pad(depth + 1) + "goto " + label_of(n.target) + ";");
                    Renderer nt = renderer(n, depth);
                    nt.hold = true; nt.carry(carry); carry = 0; nt.run(n.split + 1, n.ir.code.size()); carry = nt.held();
                }
                k++;
                continue;
            }
            if (n.kind == Node::Jump && at(n.target) == to && to < int(nodes.size())) {
                r.run(0, n.ir.code.size() - 1);             // (the region goes on there anyway)
                k++;
                continue;
            }
            r.run(0, n.ir.code.size());
            last = r.record;
            last_node = k;
            k++;
        }
        flush_carry(depth);
    }

    static std::string strip_parens(const std::string& s)
    {
        if (s.size() >= 2 && s.front() == '(' && s.back() == ')') return s.substr(1, s.size() - 2);
        return s;
    }
    static std::string paren(const std::string& s)
    {
        if (s.find(' ') == std::string::npos) return s;
        if (s.front() == '(' && s.back() == ')') return s;
        return "(" + s + ")";
    }
};

}  // namespace

std::string emit_readable(const Program& program, const ReadableNames& names, const std::vector<ReadableVariant>& variants)
{
    std::vector<std::string> out;
    // Fixed addresses by name, unless a function has that name.
    std::map<uint32_t, std::string> addresses;
    std::set<std::string> functions, used;
    for (const auto& [entry, f] : program.functions())
        functions.insert(names.function(entry));
    for (const auto& [a, name] : names.addresses)
        if (!functions.count(name))
            addresses[a] = name;
    g_addresses = &addresses;
    g_addresses_used = &used;
    std::map<uint32_t, std::string> fields;
    std::set<uint32_t> fields_used;
    std::map<std::string, uint32_t> kind_used;
    for (const auto& [off, name] : names.fields)
        if (!functions.count(name))
            fields[off] = name;
    g_fields = &fields;
    g_fields_used = &fields_used;
    std::vector<std::string> body;
    out.push_back("/* Generated by coi_recomp --readable from the player's ROM: game code, not for the repository.");
    out.push_back(" * Readable copies of the game's 68000 functions (docs/decompile.md). Each runs from its start;");
    out.push_back(" * the rest of a call that a scanline's end interrupts runs in the resumable copy (coi_rs_*). */");
    out.push_back("#include \"readable_rt.h\"");
    out.push_back("");
    if (!names.settings.empty()) {
        out.push_back("/* The port's settings that patch the ROM, as coi_native_settings has them: SETTING(name). */");
        for (size_t k = 0; k < names.settings.size(); k++)
            out.push_back("#define SETTING_" + names.settings[k] + " " + num(1u << k) + "u");
        out.push_back("");
    }
    {
        // The values a setting changes (config/patches.txt), by name.
        std::set<std::string> done;
        for (const auto& [a, p] : names.patches)
            if (p.is_value() && done.insert(p.value_name).second)
                out.push_back("#define " + p.value_name + " (SETTING(" + p.setting + ") ? " + num(p.new_value) + " : "
                              + num(p.old_value) + ")" + (p.note.empty() ? "" : "    /* " + p.note + " */"));
        if (!done.empty()) out.push_back("");
    }
    for (const auto& [entry, f] : program.functions())
        out.push_back("uint32_t coi_rs_" + hex6(entry) + "(CoiCpu* c, uint32_t at);");
    for (const ReadableVariant& v : variants)
        for (uint32_t entry : v.entries)
            out.push_back("uint32_t coi_rs_" + hex6(entry) + v.suffix + "(CoiCpu* c, uint32_t at);");
    for (const auto& [entry, f] : program.functions())
        out.push_back("static uint32_t " + names.function(entry) + "(CoiCpu* c, uint32_t at);");
    for (const ReadableVariant& v : variants)
        for (uint32_t entry : v.entries)
            out.push_back("static uint32_t " + names.function(entry) + v.suffix + "(CoiCpu* c, uint32_t at);");
    out.push_back("");
    for (const auto& [entry, f] : program.functions())
    {
        auto k = names.function_kind.find(entry);
        auto kf = k == names.function_kind.end() ? names.kind_fields.end() : names.kind_fields.find(k->second);
        g_kind_fields = kf == names.kind_fields.end() ? nullptr : &kf->second;
        g_kind_fields_used = &kind_used;
        std::vector<std::pair<uint32_t, std::string>> dispatch;
        for (const ReadableVariant& v : variants)
            if (v.entries.count(entry)) dispatch.push_back({v.bit, v.suffix});
        Function_(program, f, names, body, "", dispatch).emit();
        g_kind_fields = nullptr;
    }
    // Each setting's versions of the functions it changes, from its own image.
    for (const ReadableVariant& v : variants) {
        ReadableNames own = names;
        if (v.disassemble) own.disassemble = v.disassemble;
        for (uint32_t entry : v.entries) {
            auto k = names.function_kind.find(entry);
            auto kf = k == names.function_kind.end() ? names.kind_fields.end() : names.kind_fields.find(k->second);
            g_kind_fields = kf == names.kind_fields.end() ? nullptr : &kf->second;
            Function_(*v.program, v.program->functions().at(entry), own, body, v.suffix, {}, 1u << v.bit).emit();
            g_kind_fields = nullptr;
        }
    }
    // The hand-written patches' names of fixed addresses (whole words of their C).
    for (const auto& [a, p] : names.patches)
        for (const std::string& l : p.lines)
            for (const auto& [at, name] : addresses)
                for (size_t q = l.find(name); q != std::string::npos; q = l.find(name, q + 1)) {
                    auto word = [&](size_t i) { return i < l.size() && (std::isalnum((unsigned char)l[i]) || l[i] == '_'); };
                    if ((q == 0 || !word(q - 1)) && !word(q + name.size())) used.insert(name);
                }
    // The names of the fixed addresses the code uses (config/ram.txt, config/hardware.txt).
    for (const auto& [a, name] : addresses)
        if (used.count(name))
            out.push_back("#define " + name + " 0x" + hex6(a) + "u"
                          + (names.address_notes.count(a) ? "    /* " + names.address_notes.at(a) + " */" : ""));
    out.push_back("");
    out.push_back("/* Object fields (config/objects.txt): offsets from A0, the object the game is running. */");
    for (const auto& [off, name] : fields)
        if (fields_used.count(off))
            out.push_back("#define " + name + " " + num(off)
                          + (names.field_notes.count(off) ? "    /* " + names.field_notes.at(off) + " */" : ""));
    out.push_back("");
    if (!kind_used.empty()) {
        out.push_back("/* Fields of one kind of object (config/object_fields.txt), in its own functions. */");
        for (const auto& [name, off] : kind_used)
            out.push_back("#define " + name + " " + num(off)
                          + (names.kind_field_notes.count(name) ? "    /* " + names.kind_field_notes.at(name) + " */" : ""));
        out.push_back("");
    }
    out.insert(out.end(), body.begin(), body.end());
    g_fields = nullptr;
    g_fields_used = nullptr;
    g_addresses = nullptr;
    g_addresses_used = nullptr;
    std::ostringstream s;
    for (const std::string& l : out)
        s << l << "\n";
    return s.str();
}

}  // namespace recomp
