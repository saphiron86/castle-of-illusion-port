---
title: The disassembly
nav_order: 8
---

# The disassembly

A disassembly of the game, generated from your ROM, that reassembles to a byte-identical copy. It is the groundwork the port was built on.

The generated files are not in the repository: `python tools/disasm.py` writes them from your ROM and `config/` (`tools/build.py` runs it on the first build). Edit the config (names, regions, tracer directives, RAM and hardware names), not the generated `.asm`. The hand-written `src/main.asm` and `src/header.asm` are tracked; the cartridge header itself (`$100-$1FF`) is included from your ROM.

## Building the ROM

Requirements: Python 3.10+ and [Macroassembler AS](http://john.ccac.rwth-aachen.de:8000/as/) (`asl` and `p2bin`), either on `PATH` or in `tools/bin/`.

To build AS from its official C source with clang (no `make` needed), unpack `asl-current.tar.gz` and run:

```bash
python tools/build_asl.py path/to/asl-current
```

Then build the ROM:

```bash
python tools/build.py
```

This copies your ROM to `build/baserom.bin`, generates the disassembly if it isn't there yet, assembles `src/main.asm` to `build/coi.bin`, and checks that the output is byte-identical to the original.

## Status

| Range               | State                                                        |
|---------------------|--------------------------------------------------------------|
| `$000000-$0001FF`   | Vector table and header (`src/header.asm`)                   |
| `$000200-$00066B`   | Boot code, main loop, RAM stubs (`src/boot.asm`, generated)  |
| `$00066C-$00900F`   | Object routines (`src/objects_a.asm`, generated)             |
| `$009010-$00A5E3`   | Utilities and game state routines (`src/core.asm`, generated) |
| `$00A5E4-$00CF3D`   | VDP helpers, object code (`src/engine.asm`, generated)       |
| `$00CF3E-$00D6E3`   | VInt and HInt (`src/vint.asm`, generated)                    |
| `$00D6E4-$00FFFF`   | More engine code (`src/system.asm`, generated)               |
| `$010000-$0144AB`   | Object routines from the long table (`src/objects_b.asm`, generated) |
| `$058994-$059101`   | Small code island (`src/code_58.asm`, generated)             |
| `$06D52A-$06D78D`   | Small code island (`src/code_6D.asm`, generated)             |
| `$0784B0-$07939F`   | Z80 sound driver (`src/z80driver.asm`, generated; assembled separately to `build/z80driver.bin`) |
| `$079EE0-$07C471`   | Sound driver loader and the GameState_18-26 routines (`src/module.asm`, generated) |
| `$07FBAE-$07FE6B`   | Graphics decompressor (`src/nemdec.asm`, generated)          |
| everything else from `$0144AC` | Binary include: graphics, level and sound data |

Inside generated regions, bytes the tracer has not reached as code are kept as `dc.b` data.

## Tracing code

```bash
python tools/trace.py
```

Follows code from the entry points and tables in `config/`, and writes a code map, subroutine list, jump tables and unresolved jumps to `build/trace/`. It also writes the translator's two lists, `config/recomp_functions.txt` and `config/recomp_sites.txt` (addresses only), which the port builds in.
