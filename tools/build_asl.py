"""Build the Macroassembler AS tools (asl, p2bin) from source with clang, without make.

Mirrors what the upstream Makefile does in its default (makedefs.mi) mode, where
message catalogs are compiled into the binaries:
  1. build the host tool rescomp,
  2. turn every *.mes catalog into the .msh/.rsc headers the sources include,
  3. compile and link asl and p2bin.

Usage: python tools/build_asl.py <path to unpacked asl-current source> [work dir]
"""
import os
import re
import shutil
import subprocess
import sys
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
BIN_DIR = ROOT / "tools" / "bin"

CLANG = shutil.which("clang") or r"C:\Program Files\LLVM\bin\clang.exe"
# bpemu.c uses _getdrive/_getdcwd without including <direct.h> under clang
CFLAGS = ["-O2", "-w", "-D_CRT_SECURE_NO_WARNINGS", "-D_CRT_NONSTDC_NO_WARNINGS",
          "-include", "direct.h"]

ASL_LISTS = ["ASM_OBJECTS", "AS_OBJECTS", "ST_OBJECTS", "CODE_OBJECTS", "NLS_OBJECTS"]
P2BIN_LISTS = ["P2BIN_OBJECTS", "NLS_OBJECTS"]   # the Makefile links NLS_OBJECTS into every tool
RESCOMP_SOURCES = ["rescomp.c", "be_le.c", "strutil.c", "dynstr.c", "bpemu.c"]


def object_list(makedefs, name):
    """Return the source names listed in `name = \\ ...` in makedefs.src."""
    m = re.search(r"^%s\s*=\s*\\\n((?:.*\\\n)*.*\n)" % name, makedefs, re.M)
    if not m:
        raise ValueError("no %s in makedefs.src" % name)
    return re.findall(r"\)(\w+)\$\((?:TARG|HOST)_OBJEXTENSION\)", m.group(1))


def run(cmd, cwd):
    result = subprocess.run(cmd, cwd=cwd, capture_output=True, text=True)
    if result.returncode != 0:
        raise RuntimeError("%s\n%s%s" % (" ".join(map(str, cmd)), result.stdout, result.stderr))


def compile_all(sources, src, obj, extra):
    def one(name):
        out = obj / (name + ".o")
        run([CLANG, *CFLAGS, *extra, "-c", "%s.c" % name, "-o", str(out)], src)
        return out
    with ThreadPoolExecutor(max_workers=os.cpu_count()) as pool:
        return list(pool.map(one, sources))


def link(objects, out, cwd):
    run([CLANG, *map(str, objects), "-o", str(out)], cwd)


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 1
    src = Path(sys.argv[1]).resolve()
    work = Path(sys.argv[2]).resolve() if len(sys.argv) > 2 else src / "build_clang"
    obj = work / "obj"
    obj.mkdir(parents=True, exist_ok=True)
    makedefs = (src / "makedefs.src").read_text(errors="replace")

    print("1/3 rescomp")
    rescomp = work / "rescomp.exe"
    link(compile_all([s[:-2] for s in RESCOMP_SOURCES], src, obj, []), rescomp, src)

    print("2/3 message catalogs")
    for mes in sorted(src.glob("*.mes")):
        if mes.name == "header.mes":
            continue
        for flag, ext in (("-i", ".msh"), ("-h", ".rsc")):
            run([str(rescomp), mes.name, flag, str(obj / (mes.stem + ext))], src)

    print("3/3 asl and p2bin")
    extra = ["-I" + str(obj), "-D_USE_MSH", '-DINCDIR="include/asl"']
    asl_sources = sorted({n for lst in ASL_LISTS for n in object_list(makedefs, lst)})
    p2bin_sources = sorted({n for lst in P2BIN_LISTS for n in object_list(makedefs, lst)})
    objects = dict(zip(asl_sources, compile_all(asl_sources, src, obj, extra)))
    missing = [n for n in p2bin_sources if n not in objects]
    objects.update(zip(missing, compile_all(missing, src, obj, extra)))

    BIN_DIR.mkdir(parents=True, exist_ok=True)
    link([objects[n] for n in asl_sources], BIN_DIR / "asl.exe", src)
    link([objects[n] for n in p2bin_sources], BIN_DIR / "p2bin.exe", src)
    print("Built %s and %s" % (BIN_DIR / "asl.exe", BIN_DIR / "p2bin.exe"))
    return 0


if __name__ == "__main__":
    sys.exit(main())
