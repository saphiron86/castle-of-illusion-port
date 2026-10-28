"""Locate the user's ROM, verify it is the expected US release, and copy it to build/baserom.bin.

Usage: python tools/romcheck.py [--verify BUILT_ROM]
"""
import argparse
import hashlib
import shutil
import struct
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
ROM_DIR = ROOT / "rom"
BASEROM = ROOT / "build" / "baserom.bin"

EXPECTED_SHA1 = "4ac3687634a5acc55ac7f156c6de9749158713e4"
EXPECTED_SIZE = 0x80000


def sha1(data):
    return hashlib.sha1(data).hexdigest()


def header_checksum(data):
    words = struct.unpack(">%dH" % ((len(data) - 0x200) // 2), data[0x200:])
    return sum(words) & 0xFFFF


def find_rom():
    for path in sorted(ROM_DIR.glob("*.bin")):
        data = path.read_bytes()
        if sha1(data) == EXPECTED_SHA1:
            return path, data
    return None, None


def report(data):
    stored = struct.unpack(">H", data[0x18E:0x190])[0]
    print("  size      %d bytes" % len(data))
    print("  sha1      %s" % sha1(data))
    print("  checksum  stored %04X, computed %04X" % (stored, header_checksum(data)))


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--verify", type=Path, help="compare a built ROM against the original")
    args = parser.parse_args()

    path, data = find_rom()
    if data is None:
        print("No matching ROM in %s (expected SHA-1 %s)." % (ROM_DIR, EXPECTED_SHA1))
        return 1

    if args.verify is None:
        BASEROM.parent.mkdir(exist_ok=True)
        shutil.copyfile(path, BASEROM)
        print("OK: %s -> %s" % (path.name, BASEROM.relative_to(ROOT)))
        report(data)
        return 0

    built = args.verify.read_bytes()
    if built == data:
        print("MATCH: %s is identical to the original ROM." % args.verify)
        return 0

    print("MISMATCH: %s differs from the original ROM." % args.verify)
    report(built)
    diffs = [i for i in range(min(len(built), len(data))) if built[i] != data[i]]
    for offset in diffs[:16]:
        print("  %06X: built %02X, original %02X" % (offset, built[offset], data[offset]))
    if len(diffs) > 16:
        print("  ... %d differing bytes in total" % len(diffs))
    if len(built) != len(data):
        print("  size differs: built %d, original %d" % (len(built), len(data)))
    return 1


if __name__ == "__main__":
    sys.exit(main())
