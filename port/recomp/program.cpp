#include "program.h"

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <stdexcept>

namespace recomp {

namespace {

std::string hex6(uint32_t a)
{
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%06X", a & 0xFFFFFF);
    return buf;
}

[[noreturn]] void fail(const std::string& message)
{
    throw std::runtime_error(message);
}

}  // namespace

void read_config(const std::string& all, const std::string& name, Config& config)
{
    std::istringstream in(all);
    std::string text;
    while (std::getline(in, text)) {
        text = text.substr(0, text.find('#'));
        std::istringstream words(text);
        std::string kind;
        if (!(words >> kind))
            continue;
        std::vector<uint32_t> values;
        std::string w;
        while (words >> w)
            values.push_back(uint32_t(std::stoul(w, nullptr, 16)));
        if (kind == "function" && values.size() == 1)
            config.functions.push_back(values[0]);
        else if (kind == "stubs" && values.size() == 3)
            config.stubs.push_back(values);
        else if ((kind == "jump" || kind == "call") && values.size() >= 2)
            (kind == "jump" ? config.jumps : config.calls).emplace(values[0],
                std::vector<uint32_t>(values.begin() + 1, values.end()));
        else
            fail("bad line in " + name + ": " + text);
    }
}

void load_config(const std::string& path, Config& config)
{
    std::ifstream in(path);
    if (!in)
        fail("can't read " + path);
    std::ostringstream text;
    text << in.rdbuf();
    read_config(text.str(), path, config);
}

std::vector<uint32_t> read_function_list(const std::string& all)
{
    std::istringstream in(all);
    std::vector<uint32_t> list;
    std::string text;
    while (std::getline(in, text))
        if (text.size() >= 6 && text[0] != '#')
            list.push_back(uint32_t(std::stoul(text, nullptr, 16)) & 0xFFFFFF);
    return list;
}

std::vector<uint32_t> load_function_list(const std::string& path)
{
    std::ifstream in(path);
    if (!in)
        fail("can't read " + path);
    std::ostringstream text;
    text << in.rdbuf();
    return read_function_list(text.str());
}

Program::Program(const Rom& rom, const Config& config) : rom_(rom), config_(config)
{
    for (const auto& st : config_.stubs)
        for (uint32_t i = 0; i < st[2]; i++) {
            const uint32_t at = st[1] + 6 * i;
            if (rom_.word(at) != 0x4EF9)
                fail("stub " + std::to_string(i) + " at $" + hex6(at) + " is not jmp (xxx).l");
            stubs_[(st[0] + 6 * i) & 0xFFFFFF] = rom_.lng(at + 2) & 0xFFFFFF;
        }
}

uint32_t Program::resolve(uint32_t t) const
{
    auto it = stubs_.find(t & 0xFFFFFF);
    return it == stubs_.end() ? t : it->second;
}

const std::vector<uint32_t>& Program::call_targets(uint32_t address) const
{
    auto it = config_.calls.find(address);
    return it == config_.calls.end() ? none_ : it->second;
}

const std::vector<uint32_t>& Program::jump_targets(uint32_t address) const
{
    auto it = config_.jumps.find(address);
    return it == config_.jumps.end() ? none_ : it->second;
}

void Program::add(uint32_t entry)
{
    translate(entry);
    while (!pending_.empty()) {
        const uint32_t t = pending_.back();
        pending_.pop_back();
        try {
            translate(t);
        } catch (const std::exception&) {
            // left out: calls to it stop the translated code
        }
    }
}

void Program::add_all(const std::vector<uint32_t>& list, std::vector<std::pair<uint32_t, std::string>>* skipped)
{
    for (uint32_t e : config_.functions) {
        add(e);
        entries_.push_back(e);
    }
    for (uint32_t listed : list) {
        const uint32_t a = resolve(listed);     // RAM stubs ($FFFF60...) stand for the routines they jump to
        if (std::find(entries_.begin(), entries_.end(), a) != entries_.end())
            continue;
        try {
            add(a);
            entries_.push_back(a);
        } catch (const std::exception& ex) {
            if (skipped)
                skipped->push_back({a, ex.what()});
        }
    }
}

void Program::translate(uint32_t entry)
{
    if (auto it = failed_.find(entry); it != failed_.end())
        fail(it->second);
    if (functions_.count(entry))
        return;
    if (entry >= rom_.bytes.size() || (entry & 1)) {
        failed_[entry] = "$" + hex6(entry) + " is not code in the ROM";
        fail(failed_[entry]);
    }
    functions_[entry] = Function{};     // reserved, so a call back to it stops here
    try {
        add_body(entry);
    } catch (const std::exception& ex) {
        functions_.erase(entry);
        failed_[entry] = ex.what();
        throw;
    }
}

// The instructions reachable from `entry`: through branches and dbcc, past calls, and through
// computed jumps' known targets. Calls and jumps out of it are functions of their own.
void Program::add_body(uint32_t entry)
{
    Function f;
    f.entry = entry;
    std::vector<uint32_t> work{entry};
    std::vector<uint32_t> callees;
    while (!work.empty()) {
        const uint32_t a = work.back();
        work.pop_back();
        if (f.insns.count(a))
            continue;
        const Insn in = decode(rom_, a);
        if (in.op == Op::Unknown)
            fail("unknown instruction at $" + hex6(a) + " in function $" + hex6(entry));
        f.insns[a] = in;
        const uint32_t next = a + uint32_t(in.length);
        if ((in.op == Op::Bra || in.op == Op::Bsr || in.op == Op::Bcc || in.op == Op::DBcc) && (in.target & 1))
            continue;                       // a branch to an odd address: translated code stops before it
        switch (in.op) {
        case Op::Bra: work.push_back(in.target); break;
        case Op::Bcc: case Op::DBcc: work.push_back(in.target); work.push_back(next); break;
        case Op::Bsr: callees.push_back(resolve(in.target)); work.push_back(next); break;
        case Op::Jsr:
            if (auto t = static_target(in)) callees.push_back(resolve(*t));
            else if (config_.calls.count(a)) for (uint32_t t : config_.calls.at(a)) callees.push_back(resolve(t));
            else unresolved_.insert(a);     // translated code stops there: the interpreter runs it
            work.push_back(next);
            break;
        case Op::Jmp:
            if (auto t = static_target(in)) callees.push_back(resolve(*t));
            else if (config_.jumps.count(a)) for (uint32_t t : config_.jumps.at(a)) callees.push_back(resolve(t));
            else unresolved_.insert(a);
            break;
        case Op::Rts: case Op::Rte: case Op::Rtr: case Op::Illegal: case Op::Stop: case Op::Trap:
        case Op::LineA: case Op::LineF:
            break;
        default:
            work.push_back(next);
        }
    }
    functions_[entry] = f;
    pending_.insert(pending_.end(), callees.begin(), callees.end());
}

}  // namespace recomp
