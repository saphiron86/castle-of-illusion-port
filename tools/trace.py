"""Recursive-descent code tracer for the 68000 ROM.

Starts at the code labels in config/labels.txt, follows every branch and call it
can resolve, and writes a code map to build/trace/. Indirect jumps it cannot
resolve are listed so they can be added to config/labels.txt by hand.

Usage: python tools/trace.py
"""
import re
import sys
from collections import deque
from pathlib import Path

import capstone

ROOT = Path(__file__).resolve().parent.parent
BASEROM = ROOT / "build" / "baserom.bin"
LABELS = ROOT / "config" / "labels.txt"
DIRECTIVES = ROOT / "config" / "tracer.txt"
OUT_DIR = ROOT / "build" / "trace"

ROM_END = 0x80000

RETURNS = {"rts", "rte", "rtr"}
CONDITIONAL = re.compile(r"^(b(hi|ls|cc|cs|ne|eq|vc|vs|pl|mi|ge|lt|gt|le)|db\w+)$")
ABS_TARGET = re.compile(r"^\$([0-9a-f]+)(\.[wl]|\(pc\))?$")
PC_TABLE = re.compile(r"^\$([0-9a-f]+)\(pc, d\d\.w\)$")
# move.w table(pc,dN.w),dM: the load that precedes both kinds of word jump table
TABLE_LOAD = re.compile(r"^\$([0-9a-f]+)\(pc, d\d\.w\), (d\d)$")
MOVEA = re.compile(r"^(d\d), (a\d)$")
LEA = re.compile(r"^\$([0-9a-f]+)(\.l|\(pc\)), (a\d)$")
ADDA_INDEXED = re.compile(r"^\((a\d), d\d\.w\), (a\d)$")
DEST_AREG = re.compile(r", (a\d)$")
MAX_TABLE = 256


def load_labels():
    labels = {}
    for line in LABELS.read_text().splitlines():
        line = line.split("#", 1)[0].split()
        if line:
            labels[int(line[0], 16)] = (line[1], line[2] if len(line) > 2 else "code")
    return labels


def load_directives():
    ramcopies = []
    tables = []
    helpers = {}
    targets = {}
    for line in DIRECTIVES.read_text().splitlines():
        words = line.split("#", 1)[0].split()
        if not words:
            continue
        if words[0] == "ramcopy":
            options = dict(w.split("=", 1) for w in words[4:])
            ramcopies.append(tuple(int(w, 16) for w in words[1:4]) + (int(options.get("stubs", "0")),))
        elif words[0] == "helper":
            helpers[int(words[1], 16)] = (words[2], words[3], int(words[4], 16))
        elif words[0] == "targets":
            targets[int(words[1], 16)] = [int(w, 16) for w in words[2].split(",")]
        elif words[0] == "table":
            options = dict(w.split("=", 1) for w in words[4:])
            count = int(options["count"]) if "count" in options else None
            skip = {int(s) for s in options.get("skip", "").split(",") if s}
            base = int(options["base"], 16) if "base" in options else None
            tables.append((int(words[1], 16), words[2], int(words[3], 16), count, skip, base))
        else:
            raise ValueError("unknown directive in %s: %s" % (DIRECTIVES.name, line))
    return ramcopies, tables, helpers, targets


def branch_target(insn):
    """Return the absolute target of a branch/jump, or None if it is indirect."""
    operand = insn.op_str.split(", ")[-1] if insn.mnemonic.startswith("db") else insn.op_str
    m = ABS_TARGET.match(operand)
    if not m:
        return None
    target = int(m.group(1), 16)
    if m.group(2) == ".w" and target >= 0x8000:
        target |= 0xFFFF0000
    return target


class Tracer:
    def __init__(self, rom, labels, ramcopies=(), tables=(), helpers=None, targets=None):
        self.rom = rom
        self.labels = labels
        self.ramcopies = ramcopies
        self.tables = tables
        self.helpers = helpers or {}  # helper address -> (table kind, table register, jump site)
        self.helper_sites = {h[2] for h in self.helpers.values()}
        self.targets = targets or {}  # indirect jump site -> known destinations
        self.handled = {t[2] for t in tables} | set(self.targets)   # jumps covered by directives
        self.helper_calls = []    # (call site, helper, table) for the report
        self.stale = []           # (table, index, target) of skipped table entries
        self.md = capstone.Cs(capstone.CS_ARCH_M68K, capstone.CS_MODE_M68K_000)
        self.insns = {}           # address -> capstone instruction
        self.owner = {}           # byte address -> start of the instruction covering it
        self.subroutines = set()
        self.jump_tables = {}     # table address -> (kind, list of targets)
        self.table_bases = {}     # rel8 table address -> address its offsets are relative to
        self.data = set()         # byte addresses known to be table data
        self.indirect = []        # (address, text) of unresolved jumps
        self.problems = []        # (address, text)
        self.queue = deque()

    def add(self, address, source):
        address &= 0xFFFFFF       # the 68000 has a 24-bit address bus
        for ram, rom, size, _ in self.ramcopies:
            if ram <= address < ram + size:
                address = rom + (address - ram)
                break
        if not 0x200 <= address < ROM_END or address & 1:
            self.problems.append((source, "target $%X is outside ROM or odd" % address))
            return
        if address in self.insns:
            return
        if address in self.data:
            self.problems.append((source, "target $%X is inside a jump table" % address))
            return
        if address in self.owner:
            self.problems.append((source, "target $%X is inside the instruction at $%X"
                                  % (address, self.owner[address])))
            return
        self.queue.append(address)

    def decode(self, address):
        for insn in self.md.disasm(self.rom[address:address + 10], address, count=1):
            return insn
        return None

    def read_jump_table(self, table, source):
        """Follow a `jmp table(pc,dN.w)` table of bra instructions."""
        targets = []
        address = table
        while address < ROM_END:
            insn = self.decode(address)
            if insn is None or insn.mnemonic not in ("bra.w", "bra.b"):
                break
            targets.append(address)
            address += insn.size
        self.jump_tables[table] = ("bra", targets)
        if not targets:
            self.indirect.append((source, "jump table at $%X holds no bra instructions" % table))
        for target in targets:
            self.add(target, source)

    def read_byte_table(self, table, base, count, source):
        """Follow a table of unsigned byte offsets from `base` (jmp base(pc,dN.w))."""
        targets = [base + b for b in self.rom[table:table + count]]
        self.jump_tables[table] = ("rel8", targets)
        self.table_bases[table] = base
        self.data.update(range(table, table + count))
        for target in targets:
            self.add(target, source)

    def read_word_table(self, table, kind, source, limit=None, count=None, skip=()):
        """Follow a table of 16-bit entries: offsets from the table ("rel16") or
        zero-extended absolute addresses ("abs16"). The table is assumed to end at
        the lowest target that lies after its start, since code follows it.
        For "abs16x2" the entries are abs16 sub-tables, which are followed in turn;
        each sub-table also ends where the next one starts."""
        targets = []
        size = 4 if kind == "abs32" else 2
        if count is not None:
            end = table + size * count
        else:
            end = min(table + size * MAX_TABLE, limit or ROM_END)
        address = table
        while address < end:
            value = int.from_bytes(self.rom[address:address + size], "big")
            if kind == "rel16":
                # adda.w and (an,dn.w) indexing sign-extend the 16-bit offset
                target = table + (value - 0x10000 if value & 0x8000 else value)
            else:
                target = value
            index = (address - table) // size
            address += size
            if index in skip:
                self.stale.append((table, index, target))
                continue
            if target & 1 or not 0x200 <= target < ROM_END:
                address -= size
                break
            targets.append(target)
            if count is None and table < target < end:
                end = target
        if address != end:
            self.problems.append((source, "%s table at $%X stops at $%X, before $%X"
                                  % (kind, table, address, end)))
        self.jump_tables[table] = (kind, targets)
        self.data.update(range(table, address))
        if kind == "abs16x2":
            # Each sub-table ends where the next one starts, or at the lowest
            # code address already found, whichever comes first.
            starts = sorted(set(targets))
            lowest_code = ROM_END
            for target in starts:
                later = [s for s in starts if s > target]
                limit = min(later[0] if later else ROM_END, lowest_code)
                self.read_word_table(target, "abs16", source, limit)
                for code in self.jump_tables[target][1]:
                    self.subroutines.add(code)
                    if code > target:
                        lowest_code = min(lowest_code, code)
            return
        for target in targets:
            self.add(target, source)

    def run(self):
        for address, (_, kind) in sorted(self.labels.items()):
            if kind == "code":
                self.subroutines.add(address)
                self.add(address, address)
        for table, kind, site, count, skip, base in self.tables:
            if kind == "rel8":
                self.read_byte_table(table, base, count, site)
            else:
                self.read_word_table(table, kind, site, count=count, skip=skip)
        for _, rom, _, stubs in self.ramcopies:
            for i in range(stubs):            # 6-byte jmp (abs).l stubs, called or not
                self.add(rom + 6 * i, rom)
        for site, destinations in self.targets.items():
            for target in destinations:
                self.add(target, site)

        while self.queue:
            address = self.queue.popleft()
            table_load = None     # (table, data register) from the last move.w table(pc,dN.w),dM
            table_reg = None      # (table, address register) after movea.l dM,aK
            leas = {}             # address register -> table address from lea, in this run
            inline_rel = None     # (table, register) after adda.w (aN,dM.w),aN
            while address not in self.insns:
                insn = self.decode(address)
                if insn is None:
                    self.problems.append((address, "undecodable instruction"))
                    break
                clash = [a for a in range(address, address + insn.size) if a in self.owner]
                if clash:
                    self.problems.append((address, "instruction overlaps code at $%X"
                                          % self.owner[clash[0]]))
                    break
                self.insns[address] = insn
                for a in range(address, address + insn.size):
                    self.owner[a] = address

                mnemonic = insn.mnemonic.split(".")[0]
                follow = insn.address + insn.size
                if mnemonic in RETURNS:
                    break

                # A table pattern only counts when its instructions are adjacent,
                # so a register reloaded in between can't be mistaken for a table.
                load, reg = table_load, table_reg
                table_load = table_reg = None
                m = TABLE_LOAD.match(insn.op_str) if insn.mnemonic == "move.w" else None
                if m:
                    table_load = (int(m.group(1), 16), m.group(2))
                    address = follow
                    continue
                m = MOVEA.match(insn.op_str) if insn.mnemonic == "movea.l" else None
                if m and load and load[1] == m.group(1):
                    table_reg = (load[0], m.group(2))
                    address = follow
                    continue

                # lea table,aN / adda.w (aN,dM.w),aN / jmp (aN): an inline rel16 table
                rel, inline_rel = inline_rel, None
                m = ADDA_INDEXED.match(insn.op_str) if insn.mnemonic == "adda.w" else None
                if m and m.group(1) == m.group(2) and m.group(1) in leas:
                    inline_rel = (leas.pop(m.group(1)), m.group(1))
                    address = follow
                    continue
                m = LEA.match(insn.op_str) if insn.mnemonic == "lea.l" else None
                if m:
                    leas[m.group(3)] = int(m.group(1), 16)
                    address = follow
                    continue
                m = DEST_AREG.search(insn.op_str)
                if m:
                    leas.pop(m.group(1), None)

                if mnemonic in ("bra", "jmp", "bsr", "jsr") or CONDITIONAL.match(mnemonic):
                    target = branch_target(insn)
                    if target is not None and target in self.helpers:
                        kind, register, _ = self.helpers[target]
                        if register in leas:
                            self.helper_calls.append((address, target, leas[register]))
                            self.read_word_table(leas[register], kind, address)
                        else:
                            self.indirect.append((address, "call to helper $%X without lea into %s"
                                                  % (target, register)))
                    if target is None:
                        m = PC_TABLE.match(insn.op_str)
                        if address in self.handled:
                            pass              # a table directive in config/tracer.txt covers it
                        elif mnemonic in ("jmp", "jsr") and m and load and load[0] == int(m.group(1), 16):
                            self.read_word_table(load[0], "rel16", address)
                        elif mnemonic in ("jmp", "jsr") and rel and insn.op_str == "(%s)" % rel[1]:
                            self.read_word_table(rel[0], "rel16", address)
                        elif mnemonic == "jmp" and m:
                            self.read_jump_table(int(m.group(1), 16), address)
                        elif reg and insn.op_str == "(%s)" % reg[1]:
                            self.read_word_table(reg[0], "abs16", address)
                            for target in self.jump_tables[reg[0]][1]:
                                self.subroutines.add(target)
                        elif address not in self.handled and address not in self.helper_sites:
                            self.indirect.append((address, "%s %s" % (insn.mnemonic, insn.op_str)))
                        if mnemonic == "jmp":
                            break
                    else:
                        if mnemonic in ("bsr", "jsr"):
                            self.subroutines.add(target)
                        self.add(target, address)
                        if mnemonic in ("bra", "jmp"):
                            break
                address = follow

    def ranges(self):
        spans = []
        for address in sorted(self.insns):
            end = address + self.insns[address].size
            if spans and spans[-1][1] == address:
                spans[-1][1] = end
            else:
                spans.append([address, end])
        return spans


def main():
    if not BASEROM.exists():
        print("build/baserom.bin is missing: run python tools/romcheck.py first.")
        return 1
    rom = BASEROM.read_bytes()
    tracer = Tracer(rom, load_labels(), *load_directives())
    tracer.run()

    OUT_DIR.mkdir(parents=True, exist_ok=True)
    spans = tracer.ranges()
    code_bytes = sum(end - start for start, end in spans)

    with open(OUT_DIR / "codemap.txt", "w") as f:
        f.write("# Traced code ranges (start end size)\n")
        for start, end in spans:
            f.write("%06X %06X %5X\n" % (start, end, end - start))
    with open(OUT_DIR / "subroutines.txt", "w") as f:
        for address in sorted(tracer.subroutines):
            name = tracer.labels.get(address, ("sub_%06X" % address,))[0]
            f.write("%06X %s\n" % (address, name))
    with open(OUT_DIR / "unresolved.txt", "w") as f:
        f.write("# Indirect jumps and calls the tracer could not follow\n")
        for address, text in sorted(tracer.indirect):
            f.write("%06X %s\n" % (address, text))
        f.write("\n# Problems\n")
        for address, text in sorted(tracer.problems):
            f.write("%06X %s\n" % (address, text))
        f.write("\n# Table entries skipped as stale (table, index, target)\n")
        for table, index, target in sorted(tracer.stale):
            f.write("%06X %3d %06X\n" % (table, index, target))
    with open(OUT_DIR / "jumptables.txt", "w") as f:
        for table, (kind, targets) in sorted(tracer.jump_tables.items()):
            f.write("%06X %-5s %3d entries: %s\n"
                    % (table, kind, len(targets), " ".join("%X" % t for t in targets)))
    with open(OUT_DIR / "disasm.txt", "w") as f:
        for address in sorted(tracer.insns):
            insn = tracer.insns[address]
            if address in tracer.subroutines:
                f.write("\n%s:\n" % tracer.labels.get(address, ("sub_%06X" % address,))[0])
            f.write("%06X  %-20s %-8s %s\n" % (address, insn.bytes.hex(), insn.mnemonic, insn.op_str))

    print("Traced %d instructions in %d ranges: %d bytes (%.1f%% of ROM)."
          % (len(tracer.insns), len(spans), code_bytes, 100.0 * code_bytes / len(rom)))
    print("%d subroutines, %d jump tables, %d unresolved indirect jumps, %d problems."
          % (len(tracer.subroutines), len(tracer.jump_tables), len(tracer.indirect), len(tracer.problems)))
    print("Output written to %s" % OUT_DIR.relative_to(ROOT))
    return 0


if __name__ == "__main__":
    sys.exit(main())
