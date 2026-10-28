"""Z80 decoder, tracer and AS source generator for the sound driver.

Decoding follows the usual x/y/z/p/q split of the opcode byte. Instructions AS
cannot reproduce byte for byte (undocumented forms, duplicate ED encodings,
prefixes on instructions that do not use them) are decoded with raw=True and
emitted as db.

Used by tools/disasm.py for regions marked z80@<load address>.
"""
from collections import deque

R = ["b", "c", "d", "e", "h", "l", "(hl)", "a"]
RP = ["bc", "de", "hl", "sp"]
RP2 = ["bc", "de", "hl", "af"]
CC = ["nz", "z", "nc", "c", "po", "pe", "p", "m"]
ALU = ["add a,", "adc a,", "sub ", "sbc a,", "and ", "xor ", "or ", "cp "]
ROT = ["rlc", "rrc", "rl", "rr", "sla", "sra", "sll", "srl"]
BLI = [["ldi", "cpi", "ini", "outi"], ["ldd", "cpd", "ind", "outd"],
       ["ldir", "cpir", "inir", "otir"], ["lddr", "cpdr", "indr", "otdr"]]


class Insn:
    """One decoded instruction.

    text uses placeholders {addr} for a 16-bit address operand and {target}
    for a relative/absolute branch target, filled in by the generator.
    """
    def __init__(self, address, size, text, kind="", target=None, addr=None, raw=False):
        self.address = address
        self.size = size
        self.text = text
        self.kind = kind          # "", "jp", "jr", "call", "ret", "stop" (unconditional end)
        self.target = target      # branch target
        self.addr = addr          # 16-bit data address operand (ld a,(nn), ld hl,nn ...)
        self.raw = raw


def n8(v):
    return "$%02X" % v


def disp(d):
    d = d - 256 if d >= 128 else d
    return "%s$%02X" % ("-" if d < 0 else "+", abs(d))


def decode(mem, pc):
    """Decode the instruction at pc (Z80 address) from mem (bytes, index = Z80 address)."""
    def byte(i):
        return mem[pc + i] if pc + i < len(mem) else 0

    def word(i):
        return byte(i) | byte(i + 1) << 8

    op = byte(0)
    if op == 0xCB:
        return decode_cb(byte(1), pc, 2, "hl", None)
    if op == 0xED:
        return decode_ed(byte(1), pc, word)
    if op in (0xDD, 0xFD):
        index = "ix" if op == 0xDD else "iy"
        nxt = byte(1)
        if nxt == 0xCB:
            return decode_cb(byte(3), pc, 4, index, byte(2))
        inner = decode_main(nxt, pc, lambda i: byte(i + 1), lambda i: word(i + 1), index)
        inner.size += 1
        return inner
    return decode_main(op, pc, byte, word, None)


def decode_main(op, pc, byte, word, index):
    """Unprefixed opcode, or the opcode after a DD/FD prefix when index is set."""
    x, y, z = op >> 6, (op >> 3) & 7, op & 7
    p, q = y >> 1, y & 1
    raw = False
    size = 1
    hl = index or "hl"

    def reg(n, d_offset=1):
        # (hl) becomes (ix+d); with a prefix, h/l would be undocumented ixh/ixl
        nonlocal size, raw
        if n == 6 and index:
            size_needed = d_offset + 1
            size = max(size, size_needed)
            return "(%s%s)" % (index, disp(byte(d_offset)))
        if index and n in (4, 5):
            raw = True
        return R[n]

    text, kind, target, addr = None, "", None, None
    uses_hl = False
    if x == 0:
        if z == 0:
            if y == 0:
                text = "nop"
            elif y == 1:
                text = "ex af,af'"
            else:
                size = 2
                target = (pc + 2 + (byte(1) - 256 if byte(1) >= 128 else byte(1))) & 0xFFFF
                kind = "jr"
                if y == 2:
                    text = "djnz {target}"
                elif y == 3:
                    text, kind = "jr {target}", "jr_always"
                else:
                    text = "jr %s,{target}" % CC[y - 4]
        elif z == 1:
            if q == 0:
                size = 3
                addr = word(1)
                text = "ld %s,{addr}" % (hl if p == 2 else RP[p])
                uses_hl = p == 2
            else:
                text = "add %s,%s" % (hl, hl if p == 2 else RP[p])
                uses_hl = True
        elif z == 2:
            forms = [["ld (bc),a", "ld (de),a", "ld ({addr}),%s" % hl, "ld ({addr}),a"],
                     ["ld a,(bc)", "ld a,(de)", "ld %s,({addr})" % hl, "ld a,({addr})"]]
            text = forms[q][p]
            if p >= 2:
                size = 3
                addr = word(1)
            uses_hl = p == 2
        elif z == 3:
            text = "%s %s" % ("inc" if q == 0 else "dec", hl if p == 2 else RP[p])
            uses_hl = p == 2
        elif z in (4, 5):
            text = "%s %s" % ("inc" if z == 4 else "dec", reg(y))
            uses_hl = y in (4, 5, 6)
        elif z == 6:
            dst = reg(y)
            n_at = 2 if (index and y == 6) else 1
            size = max(size, n_at + 1)
            text = "ld %s,%s" % (dst, n8(byte(n_at)))
            uses_hl = y in (4, 5, 6)
        else:
            text = ["rlca", "rrca", "rla", "rra", "daa", "cpl", "scf", "ccf"][y]
    elif x == 1:
        if y == 6 and z == 6:
            text, kind = "halt", ""
        else:
            # with an index prefix only one side may be (ix+d); the other keeps plain h/l
            if index and (y == 6 or z == 6):
                src = reg(z) if z == 6 else R[z]
                dst = reg(y) if y == 6 else R[y]
            else:
                src, dst = reg(z), reg(y)
            text = "ld %s,%s" % (dst, src)
            uses_hl = y in (4, 5, 6) or z in (4, 5, 6)
    elif x == 2:
        text = ALU[y] + reg(z)
        uses_hl = z in (4, 5, 6)
    else:
        if z == 0:
            text, kind = "ret %s" % CC[y], "ret_cc"
        elif z == 1:
            if q == 0:
                text = "pop %s" % (hl if p == 2 else RP2[p])
                uses_hl = p == 2
            else:
                if p == 0:
                    text, kind = "ret", "stop"
                elif p == 1:
                    text = "exx"
                elif p == 2:
                    text, kind = "jp (%s)" % hl, "stop"
                    uses_hl = True
                else:
                    text = "ld sp,%s" % hl
                    uses_hl = True
        elif z == 2:
            size, target, kind = 3, word(1), "jp"
            text = "jp %s,{target}" % CC[y]
        elif z == 3:
            if y == 0:
                size, target, kind = 3, word(1), "jp_always"
                text = "jp {target}"
            elif y == 2:
                size, text = 2, "out (%s),a" % n8(byte(1))
            elif y == 3:
                size, text = 2, "in a,(%s)" % n8(byte(1))
            elif y == 4:
                text = "ex (sp),%s" % hl
                uses_hl = True
            elif y == 5:
                text = "ex de,hl"
            elif y == 6:
                text = "di"
            elif y == 7:
                text = "ei"
            else:
                raw = True          # CB after DD/FD is handled in decode(); here it is impossible
                text = "?"
        elif z == 4:
            size, target, kind = 3, word(1), "call"
            text = "call %s,{target}" % CC[y]
        elif z == 5:
            if q == 0:
                text = "push %s" % (hl if p == 2 else RP2[p])
                uses_hl = p == 2
            elif p == 0:
                size, target, kind = 3, word(1), "call"
                text = "call {target}"
            else:
                raw, text = True, "?"   # a prefix followed by another prefix
        elif z == 6:
            size, text = 2, ALU[y] + n8(byte(1))
        else:
            text, target, kind = "rst %s" % n8(y * 8), y * 8, "call"
    if index and not uses_hl:
        raw = True                  # the DD/FD prefix has no effect here; AS would drop it
    return Insn(pc, size, text, kind, target, addr, raw)


def decode_cb(op, pc, size, hl, d):
    x, y, z = op >> 6, (op >> 3) & 7, op & 7
    raw = False
    if d is not None:
        operand = "(%s%s)" % (hl, disp(d))
        if z != 6:
            raw = True              # undocumented DDCB forms that also copy to a register
    else:
        operand = R[z]
    if x == 0:
        if y == 6:
            raw = True              # sll is undocumented
        text = "%s %s" % (ROT[y], operand)
    else:
        text = "%s %d,%s" % (["", "bit", "res", "set"][x], y, operand)
    return Insn(pc, size, text, raw=raw)


def decode_ed(op, pc, word):
    x, y, z = op >> 6, (op >> 3) & 7, op & 7
    p, q = y >> 1, y & 1
    size, raw, kind, addr = 2, False, "", None
    text = "?"
    if x == 1:
        if z == 0:
            text = "in %s,(c)" % R[y]
            raw = y == 6
        elif z == 1:
            text = "out (c),%s" % R[y]
            raw = y == 6
        elif z == 2:
            text = "%s hl,%s" % ("sbc" if q == 0 else "adc", RP[p])
        elif z == 3:
            size, addr = 4, word(2)
            text = "ld ({addr}),%s" % RP[p] if q == 0 else "ld %s,({addr})" % RP[p]
            raw = p == 2            # AS uses the shorter unprefixed form for hl
        elif z == 4:
            text, raw = "neg", y != 0
        elif z == 5:
            text, kind = ("reti" if y == 1 else "retn"), "stop"
            raw = y not in (0, 1)
        elif z == 6:
            modes = {0: "0", 2: "1", 3: "2"}
            text, raw = "im %s" % modes.get(y, "0"), y not in modes
        else:
            text = ["ld i,a", "ld r,a", "ld a,i", "ld a,r", "rrd", "rld", "?", "?"][y]
            raw = y >= 6
    elif x == 2 and z <= 3 and y >= 4:
        text = BLI[y - 4][z]
    else:
        raw = True
    return Insn(pc, size, text, kind, addr=addr, raw=raw)


class Z80Tracer:
    """Recursive-descent trace over a Z80 memory image."""
    def __init__(self, mem, start, end, entries):
        self.mem = mem
        self.start, self.end = start, end
        self.insns = {}
        self.owner = {}
        self.calls = set(entries)
        self.indirect = []
        self.problems = []
        self.tables = {}          # table address -> list of targets (dw entries)
        self.data = set()         # bytes known to be table data
        self.queue = deque(entries)

    def read_table(self, table, source):
        """A dw table used through ld hl,table / rst $28 / jp (hl). It is assumed
        to end at the lowest target that lies after it, since code follows."""
        targets = []
        end = self.end
        address = table
        while address + 1 < end:
            target = self.mem[address] | self.mem[address + 1] << 8
            if not self.start <= target < self.end:
                break
            targets.append(target)
            if table < target < end:
                end = target
            address += 2
        self.tables[table] = targets
        self.data.update(range(table, address))
        for target in targets:
            self.queue.append(target)

    def run(self):
        while self.queue:
            pc = self.queue.popleft()
            hl_table = None       # table address after ld hl,nn followed by rst $28
            last_ld_hl = None
            while self.start <= pc < self.end and pc not in self.insns:
                if pc in self.data:
                    self.problems.append((pc, "inside a jump table"))
                    break
                if pc in self.owner:
                    self.problems.append((pc, "inside the instruction at $%04X" % self.owner[pc]))
                    break
                insn = decode(self.mem, pc)
                if insn.raw and insn.text == "?":
                    self.problems.append((pc, "invalid opcode"))
                    break
                self.insns[pc] = insn
                for a in range(pc, pc + insn.size):
                    self.owner[a] = pc

                # ld hl,table / rst $28 (hl = word at table + 2*a) / ... / jp (hl)
                if insn.text == "ld hl,{addr}":
                    last_ld_hl, hl_table = insn.addr, None
                elif insn.text == "rst $28" and last_ld_hl is not None:
                    hl_table, last_ld_hl = last_ld_hl, None
                elif insn.text == "jp (hl)" and hl_table is not None:
                    self.read_table(hl_table, pc)
                    break
                elif "hl" in insn.text:
                    last_ld_hl = hl_table = None
                else:
                    last_ld_hl = None

                if insn.target is not None:
                    if insn.kind == "call":
                        self.calls.add(insn.target)
                    if self.start <= insn.target < self.end:
                        self.queue.append(insn.target)
                if insn.kind in ("stop", "jp_always", "jr_always"):
                    if insn.text.startswith("jp ("):
                        self.indirect.append((pc, insn.text))
                    break
                pc += insn.size
        return self


class Z80Generator:
    """Emit AS source for one Z80 block. Labels are z80_<kind>_XXXX by Z80 address."""
    def __init__(self, tracer, rom, rom_start, names=None, data_names=None):
        self.t = tracer
        self.rom = rom
        self.rom_start = rom_start
        self.names = dict(names or {})        # Z80 address -> label (code)
        self.data_names = dict(data_names or {})   # Z80 address -> name (RAM, mailbox, ports)
        self.used_external = set()
        for a in tracer.calls:
            self.names.setdefault(a, "z80_sub_%04X" % a)
        for insn in tracer.insns.values():
            if insn.target is not None and insn.kind != "call" or (insn.kind == "call" and insn.target is not None):
                self.names.setdefault(insn.target, "z80_loc_%04X" % insn.target)

    def label(self, address):
        if address in self.t.owner and self.t.owner[address] != address:
            owner = self.t.owner[address]
            return "%s+%d" % (self.label(owner), address - owner)
        name = self.names.setdefault(address, "z80_loc_%04X" % address)
        if not (self.t.start <= address < self.t.end):
            self.used_external.add(address)
        return name

    def address_operand(self, value):
        if value in self.data_names:
            return self.data_names[value]
        return "$%04X" % value

    def lines(self):
        out = []
        pc = self.t.start
        data = []

        def flush():
            for i in range(0, len(data), 16):
                out.append("\tdb\t" + ",".join("$%02X" % b for b in data[i:i + 16]))
            data.clear()

        # labels are only placed where something refers to them
        referenced = {i.target for i in self.t.insns.values() if i.target is not None} | self.t.calls
        for table, targets in self.t.tables.items():
            self.names.setdefault(table, "z80_tbl_%04X" % table)
            referenced.add(table)
            for target in targets:
                self.names.setdefault(target, "z80_loc_%04X" % target)
                referenced.add(target)
        while pc < self.t.end:
            if pc in referenced and pc in self.names:
                flush()
                if pc in self.t.calls:
                    out.append("")
                out.append("%s:" % self.names[pc])
            if pc in self.t.tables:
                flush()
                for target in self.t.tables[pc]:
                    out.append("\tdw\t%s" % self.label(target))
                pc += 2 * len(self.t.tables[pc])
                continue
            insn = self.t.insns.get(pc)
            if insn is not None:
                flush()
                raw_bytes = self.rom[self.rom_start + pc - self.t.start:][:insn.size]
                if insn.raw:
                    out.append("\tdb\t%s\t; %s (not reproducible by AS)" % (
                        ",".join("$%02X" % b for b in raw_bytes), insn.text))
                else:
                    text = insn.text
                    if "{target}" in text:
                        text = text.replace("{target}", self.label(insn.target))
                    if "{addr}" in text:
                        text = text.replace("{addr}", self.address_operand(insn.addr))
                    out.append("\t" + text.replace(" ", "\t", 1))
                pc += insn.size
                continue
            data.append(self.mem_byte(pc))
            pc += 1
        flush()
        return out

    def mem_byte(self, pc):
        return self.t.mem[pc]
