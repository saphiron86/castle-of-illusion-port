---
title: Building the port
nav_order: 7
---

# Building the port

The port (`port/`) runs the game's 68000 code as native code from its own translator, with the Musashi interpreter alongside; it has its own VDP renderer, an SDL3 window and a Dear ImGui menu. Sound runs the game's own Z80 driver on a Z80 core, with ymfm for the YM2612 and a built-in SN76489 PSG. The hqx upscaler is libretro's GLSL version (LGPL 2.1 or later), fetched with the other libraries and built in unchanged.

It needs CMake and a C++17 compiler (on Windows the Visual Studio Build Tools are enough) and Python 3.10 or later. The window is drawn with OpenGL 3.3.

## Windows

```bash
python tools/fetch_deps.py
```

```bash
cmake -S port -B build/port
```

```bash
cmake --build build/port --config Release --target coi
```

Run `build/port/Release/coi.exe`. It finds your ROM in a `rom` folder beside the program, or in `rom/` of the current folder or up to three folders above it (so the repository's `rom/` works). Without a ROM it shows a message and quits. On Windows it opens only the game window; add `--console` to also show a terminal window with its messages (the terminal it was started from, or a new one). Output redirected to a file or a pipe works either way.

## Linux

The same commands build `build/port/coi` with GCC or Clang; SDL needs the X11, Wayland, ALSA, PulseAudio and PipeWire development packages to support them.

The release download is built in Valve's Steam Runtime SDK, so it runs on any distribution with glibc 2.29 or later. With podman (or docker):

```bash
podman run --rm -v "$PWD":/src:Z -w /src registry.gitlab.steamos.cloud/steamrt/sniper/sdk:latest sh -c "cmake -S port -B build/linux -G Ninja -DCMAKE_BUILD_TYPE=Release && ninja -C build/linux coi"
```

## Android

The Android app is built with Gradle from `android/`, after the desktop build. See [android/README.md](https://github.com/saphiron86/castle-of-illusion-port/blob/main/android/README.md) for the requirements, signing and the app's log.

## Build options

| Option | Effect |
|--------|--------|
| `-DCOI_JIT=OFF` | Build without the translator: the interpreter runs everything. See [Native code](native.md) |
| `-DCOI_READABLE=ON` | Generate the game's code as readable C from your ROM (in `rom/`) and run that. See [Readable C](decompile.md) |

The translator carries no game code, only lists of addresses (`config/recomp*.txt`). The readable C is generated into `build/readable/generated/` on your own machine and is never in the repository or the downloads.
