---
title: Technical notes
nav_order: 9
has_children: true
---

# Technical notes

How the port works inside, written from the disassembly and the port's code.

- [Native code](native.md): the port's own 68000 translator (x86-64 and ARM64), and how it stays frame for frame the same as the interpreter.
- [Readable C](decompile.md): generating the game's code as readable C from your own ROM.
- [Sound](sound.md): how the game loads the Z80 sound driver and sends it requests, and what a Z80-emulation sound layer needs.
- [Widescreen](widescreen.md): what depends on the 320-pixel screen width (cameras, plane streaming, object ranges, sprite clipping) and what a 400-pixel port has to change.
