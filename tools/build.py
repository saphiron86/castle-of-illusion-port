"""Assemble src/main.asm into build/coi.bin and check it against the original ROM.

Usage: python tools/build.py
"""
import re
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
BUILD = ROOT / "build"
OBJECT = BUILD / "coi.p"
OUTPUT = BUILD / "coi.bin"
LISTING = BUILD / "coi.lst"


def find_tool(name):
    local = ROOT / "tools" / "bin" / (name + ".exe")
    if local.exists():
        return str(local)
    return shutil.which(name)


def z80_regions():
    """Names of the regions in config/regions.txt marked z80@<load address>."""
    names = []
    for line in (ROOT / "config" / "regions.txt").read_text().splitlines():
        words = line.split("#", 1)[0].split()
        if len(words) > 3 and words[3].startswith("z80@"):
            names.append(words[2])
    return names


def main():
    asl, p2bin = find_tool("asl"), find_tool("p2bin")
    if asl is None or p2bin is None:
        print("asl/p2bin not found (looked in tools/bin and PATH): see tools/build_asl.py.")
        return 1

    if not (BUILD / "baserom.bin").exists():
        if subprocess.call([sys.executable, str(ROOT / "tools" / "romcheck.py")]) != 0:
            return 1

    # The disassembly is generated from your ROM and config/, and is not in the repository:
    # make it on the first build.
    if not (ROOT / "src" / "layout.asm").exists():
        if subprocess.call([sys.executable, str(ROOT / "tools" / "disasm.py")]) != 0:
            return 1

    # Z80 code is assembled on its own first; src/layout.asm bincludes the result.
    for name in z80_regions():
        obj, out = BUILD / (name + ".p"), BUILD / (name + ".bin")
        cmd = [asl, "-xx", "-q", "-U", "-L", "-i", ".", "-olist", str(BUILD / (name + ".lst")),
               "-o", str(obj), "src/%s.asm" % name]
        if subprocess.call(cmd, cwd=ROOT) != 0:
            return 1
        if subprocess.call([p2bin, "-q", str(obj), str(out)], cwd=ROOT) != 0:
            return 1

    # -xx: detailed errors, -q: quiet, -U: case-sensitive symbols, -L: listing,
    # -i .: resolve include paths from the project root
    cmd = [asl, "-xx", "-q", "-U", "-L", "-i", ".", "-olist", str(LISTING),
           "-o", str(OBJECT), "src/main.asm"]
    if subprocess.call(cmd, cwd=ROOT) != 0:
        return 1
    if subprocess.call([p2bin, "-q", str(OBJECT), str(OUTPUT)], cwd=ROOT) != 0:
        return 1

    result = subprocess.call([sys.executable, str(ROOT / "tools" / "romcheck.py"), "--verify", str(OUTPUT)])
    if result != 0:
        report_first_shift()
    return result


def report_first_shift():
    """Generated labels carry their original address in their name (loc_XXXXXX),
    so the first one assembled elsewhere shows where the output started to drift."""
    previous = None
    label = re.compile(r"^(?:\(\d+\))?\s*\d+/\s*([0-9A-F]+) :\s+.*?\b(?:loc|sub|tbl|dat)_([0-9A-F]{6}):")
    for line in LISTING.read_text(errors="replace").splitlines():
        m = label.match(line)
        if not m:
            continue
        if int(m.group(1), 16) != int(m.group(2), 16):
            print("First misplaced label: expected at $%s, assembled at $%s." % (m.group(2), m.group(1)))
            if previous:
                print("The size change is between %s and $%s." % (previous, m.group(2)))
            return
        previous = "$" + m.group(2)


if __name__ == "__main__":
    sys.exit(main())
