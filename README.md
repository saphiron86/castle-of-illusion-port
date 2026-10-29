# Castle of Illusion: PC port

> **AI disclosure:** This project was made with extensive use of AI. Most of the code, tools and documentation were written by Claude, Anthropic's AI coding assistant, under my direction: I chose the features, play-tested every change and decided what went in. Expect the kinds of mistakes AI-written code can contain, and please report any you find in [Issues](https://github.com/saphiron86/castle-of-illusion-port/issues).

An unofficial PC port of the US Sega Genesis release of *Castle of Illusion Starring Mickey Mouse* (1990), with widescreen, smoother motion and display options, and the tools to disassemble the game from your own ROM.

**No game data is included.** This repository and the release downloads contain no ROM, graphics, sound, level data or disassembled game code. The port runs the game from your own dump of the cartridge, and the disassembly is generated on your computer from that dump. See [rom/README.md](rom/README.md) for the ROM that is expected.

## Playing (Windows)

1. Download the latest `coi-port-windows.zip` from the [Releases](https://github.com/saphiron86/castle-of-illusion-port/releases) page and unzip it anywhere.
2. Put your own dump of the US release (a 524,288-byte `.bin` file) in the `rom` folder beside `coi.exe`.
3. Start `coi.exe`. Press F1 for the menu, F2/F3/F4 for the Quick Settings.

Settings are saved in `%APPDATA%\CastleOfIllusion\Port\settings.ini`. To use an MD+ soundtrack pack, put it in a folder named `mdplus` beside `coi.exe` (see [MD+ soundtrack packs](#md-soundtrack-packs)).

## Legal

This is a fan project. It is not affiliated with, endorsed or sponsored by Disney or Sega. *Castle of Illusion*, Mickey Mouse, Sega and Genesis are trademarks of their respective owners, used here only to name the game this port runs. You must supply your own copy of the game; please do not ask for, or share, ROMs.

The project's own code is under the MIT License ([LICENSE](LICENSE)). The port is built with third-party libraries under their own licenses: SDL (zlib), Musashi and SoftFloat, ymfm (BSD 3-Clause), z80 (MIT), Dear ImGui (MIT), libretro's hqx shaders (LGPL 2.1 or later) and torcado's cleanEdge shader (MIT); see [THIRD_PARTY_NOTICES.txt](THIRD_PARTY_NOTICES.txt).

## The disassembly

A disassembly of the game, generated from your ROM, that reassembles to a byte-identical copy. It is the groundwork the port was built on.

### Status

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

The generated files are not in the repository: `python tools/disasm.py` writes them from your ROM and `config/` (`tools/build.py` runs it on the first build). Edit the config (names, regions, tracer directives, RAM and hardware names), not the generated `.asm`. The hand-written `src/main.asm` and `src/header.asm` are tracked; the cartridge header itself ($100-$1FF) is included from your ROM.

## Notes for the port

- [docs/sound.md](docs/sound.md): how the game loads the Z80 sound driver and sends it requests, and what a Z80-emulation sound layer needs.
- [docs/widescreen.md](docs/widescreen.md): what depends on the 320-pixel screen width (cameras, plane streaming, object ranges, sprite clipping) and what a 400-pixel port has to change.

## Building

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

## Building the PC port

The port (`port/`) runs the game's 68000 code on the Musashi interpreter, with its own VDP renderer and an SDL3 window, and a Dear ImGui menu; the hqx upscaler is libretro's GLSL version (LGPL 2.1 or later), fetched with the other libraries and built in unchanged. Sound runs the game's own Z80 driver on a Z80 core, with ymfm for the YM2612 and a built-in SN76489 PSG. It needs CMake and a C++17 compiler; on Windows the Visual Studio Build Tools are enough. The window is drawn with OpenGL 3.3, which any graphics driver from the last decade provides.

```bash
python tools/fetch_deps.py
```

```bash
cmake -S port -B build/port
```

```bash
cmake --build build/port --config Release --target coi
```

Run `build/port/Release/coi.exe`; it finds your ROM in a `rom` folder beside the program, or in `rom/` of the current folder or up to three folders above it (so the repository's `rom/` works). Without a ROM it shows a message and quits. On Windows it opens only the game window; add `--console` to also show a terminal window with its messages (the terminal it was started from, or a new one). Output redirected to a file or a pipe works either way.

| Genesis | Keyboard | Controller (Xbox layout) |
|---------|----------|--------------------------|
| D-pad   | arrows   | D-pad or left stick      |
| A       | Z        | X                        |
| B       | X        | A                        |
| C       | C        | B                        |
| Start   | Enter    | Menu                     |

Controllers go through SDL's gamepad support (XInput, DirectInput and other Windows input APIs), can be connected at any time, and work alongside the keyboard; the console shows when one connects.

### The F1 menu

F1, or the View button on a controller, shows or hides a menu bar over the game, which keeps running underneath. Use it with the mouse, with the arrow keys and Enter once it has focus (the game doesn't get those keys while it does), or with a controller: the D-pad or left stick moves, A selects, and B goes back, closing the menu from the top level. While the menu is open, controllers drive only the menu; after it closes, they reach the game again once all their buttons are released.

| Menu  | Item            | Shortcut            | What it does |
|-------|-----------------|---------------------|--------------|
| Game  | Reset           | Ctrl+R              | Restarts the game as if the console were switched off and on, dropping any stage chosen with `--stage` or the Debug menu |
| Game  | Exit to desktop | Alt+F4              | Closes the port |
| Quick Settings | Enhanced | F2 | Widescreen and every graphics enhancement at its default (Smooth Swinging, Smooth Scrolling, Clean Rock Edges, Smooth Fades and Remove Sprite Limit on; Upscaler, Scanlines and the CRT shader off). This is the setting on first launch |
| Quick Settings | Vanilla | F3 | The original game: 4:3, with Smooth Swinging, Smooth Scrolling, Clean Rock Edges, Smooth Fades, Remove Sprite Limit, the Upscaler, Scanlines and the CRT shader off |
| Quick Settings | Custom | F4 | Your own choices for those settings, as they were before you last picked Enhanced or Vanilla. Changing any of them yourself switches to Custom. Quick Settings leaves the other Video settings, Gameplay, Audio and the cheats alone |
| Video | Widescreen      |                     | Switches between 400 and 320 pixels wide, immediately, even mid-stage |
| Video | Fullscreen      | F11 or Alt+Enter    | Borderless fullscreen at the desktop resolution |
| Video | V-Sync          |                     | On by default. Each picture is shown in step with the display's refresh, so it never tears. On a display at about 60 Hz the game runs one frame per refresh (the sound is resampled by the tiny speed difference); at other rates game frames are timed as on the console and each refresh shows the latest one. If the driver ignores the request, pictures are still capped near the refresh rate |
| Video | Frame Rate | | Display (default): pictures follow the display's refresh; at a whole multiple of 60 Hz each game frame is shown for exactly that many refreshes, counted by refresh, not by the clock. 60, 120 or 240: the port times the pictures itself at that multiple of the console's 59.92 a second, whatever the display reports, for variable-refresh (G-Sync, FreeSync) displays; the game runs at exactly the console's speed. Above a display's maximum refresh rate not every picture can be shown; pictures that can't be drawn in time (240 with Smooth Scrolling is a lot of work) are skipped, so the game keeps its speed. `--frame-rate N` for one run |
| Video | High Refresh Interpolation | | At 120 or 240 pictures a second (Frame Rate, or a display at 120, 180 or 240 Hz with V-Sync), each game frame is shown as evenly spaced moments between the last two game frames instead of repeats: each line's scroll and each sprite (moved with the object that drew it) are placed part of the way along their movement, and with Smooth Scrolling the planes follow its fitted movement. The picture is up to one game frame (17 ms) behind. Off by default; `--interpolate` turns it on for one run |
| Video | Show FPS        |                     | A counter in the top right: pictures shown per second, and game frames per second when they differ. `--show-fps` shows it for one run |
| Enhancements > Gameplay | Easy Bounce Attack | | Every jump becomes the bounce attack without pressing Down: the port taps Down for one frame after take-off, as a player would. Holding the jump button still decides the height. Off by default; `--easy-bounce` starts with it on |
| Enhancements > Audio | MD+ Soundtrack | | Plays an MD+ soundtrack pack (see below) instead of the game's music; available when a pack is in `mdplus/`. On by default when one is there. Switching it restarts the game |
| Enhancements > Graphics | Smooth Swinging | | On by default. Vines and ropes swing at 60 frames a second instead of the original 10, with Mickey and the camera moving every frame. The game only has 9 rope poses, stepped every 6 frames; the port builds the poses in between (each rope link moved part of the way) and patches the game to use them, so the swing still takes as long and Mickey's own drawings change when they always did. Off by default; `--smooth-swing` starts with it on. Details in `port/src/smooth_swing.cpp` |
| Enhancements > Graphics | Smooth Scrolling | | On by default. The game scrolls in whole pixels, so at in-between speeds the picture moves unevenly (the foreground 1, 2, 1, 2 pixels a frame while running, the background 1, 1, 1, 0). This draws each line at 4x horizontal resolution and moves each layer by fractions of a pixel to where its recent movement says it should be, so scrolling moves in even steps. Corrections are under a pixel, so starts and stops look as before. Horizontal scrolling only. Off by default; `--smooth-scroll` starts with it on. Details in `port/src/smooth_scroll.cpp` |
| Enhancements > Graphics | Clean Rock Edges | | On by default. In the forest stages the rock walls' edge tiles use black for the gaps between stones, so where a rock ends (against the background or the bushes in front) those gaps show as black notches in the original. With this on, black connected to the outer side of a rock edge tile (or within 3 pixels of transparency) takes the colour of what lies beyond it: the background, a bush, or the dark filler beside the rock. Off gives the original picture; `--vanilla-rock-edges` turns it off for one run |
| Enhancements > Graphics | Smooth Fades | | On by default. The game fades between screens by stepping each colour one of the console's eight levels at a time, every few frames, so fades show as a handful of jumps. With this on, the colours ease from one step to the next over the time the game takes between steps (so a fade finishes a few frames later than in the original); other palette changes, such as palette cycling and flashes, show at once. Off gives the original fades; `--vanilla-fades` turns it off for one run |
| Enhancements > Graphics | Remove Sprite Limit | | On by default. The console draws at most 20 sprites on a line and drops the rest, so on busy lines (more of them in the widened view) parts of sprites vanish or flicker. With this on, every sprite on the line is drawn. Off by default |
| Enhancements > Graphics | Upscaler | | Off, HQ2x, HQ3x, HQ4x or cleanEdge. HQ2x-4x: Maxim Stepin's hqx filter, which redraws the pixel art at 2, 3 or 4 times the size with smoothed edges and diagonals; the result is then stretched to the window. cleanEdge: torcado's filter, which keeps the art's hard edges and colours and redraws its diagonals and slopes as clean lines, at the window's size (rounded up, up to 6 times); checkerboard and hatched dithering is left as it is. Its Line Width slider (0.45-1.14, default 1.00) makes those lines thinner or bolder, and `--clean-edge-width W` sets it for one run. Used only while the CRT shader is off. With Smooth Scrolling on, each layer (planes A and B and the sprites, in front of and behind the others) is enlarged on its own and then moved by its fraction of a pixel, so scrolling stays smooth; sprite outlines are then smoothed against the transparency around them rather than against the scenery. `--hqx N` sets it for one run and turns the CRT shader off |
| Enhancements > Graphics | Scanlines | | Off, 25%, 50%, 75% or 100%: dark lines between the game's lines, like Sonic 3 A.I.R.'s scanline option, without the CRT shader's other effects (100% makes them black). Used only while the CRT shader is off. `--scanlines N` sets it for one run and turns the CRT shader off |
| Enhancements > Graphics | CRT Shader | F12 | Draws the picture like a CRT television: scanlines, a phosphor mask and a soft glow, blended in linear light. Written for this port after Timothy Lottes' public-domain CRT shader. Off by default; `--crt` turns it on for one run |
| Enhancements > Graphics | CRT Shader Settings... | | A window with sliders for scanline and pixel sharpness, glow, brightness, the mask type (slot, aperture grille, stretched VGA, VGA or none), its strength and size, and screen curvature (flat by default), plus a reset button. Changes show at once and are saved |
| Cheats | Invincibility | | Enemies and deadly hazards don't hurt Mickey (no contact, no damage, no hazard death). Falling off the stage still costs a try |
| Cheats | Infinite Power | | Hits knock Mickey back but take no power (Game Genie ALKA-AA9Y) |
| Cheats | Infinite Tries | | Losing a life, falls included, costs no try (ALKT-AA2A, SC8T-AYXR) |
| Cheats | Infinite Items | | Throwing doesn't use up items (SC6A-AY0L) |
| Cheats | Infinite Continues | | The continue screen doesn't use up continues |
| Cheats | Start with Full Power | | Every life starts with all five power stars, on any difficulty |
| Cheats | Walk Faster | | A steady 2.3 pixels a frame instead of 1.4. Based on AMFA-AABN/ADFA-BAVR, which alone makes the speed saw-tooth between the two; the two speed comparisons and the slow-down threshold are raised as well, so Mickey still stops when you let go |
| Cheats | Moon Jump | | Holding jump keeps Mickey rising (RHEA-A6ZW, RHEA-A60G). Switches Jump in Midair off |
| Cheats | Jump in Midair | | Each press of jump in the air starts a new jump, held for height like a normal one (the first jump too). Unlike the Game Genie version (the moon jump codes plus A1EA-AAF0), which rises only on the frame jump is pressed. Switches Moon Jump off |
| Debug | Go to stage     |                     | Starts a new game at the chosen stage straight away, from anywhere (title, attract demo or mid-game), with fresh lives, continues and score; the current stage is ticked |
| Debug | Open Settings Folder | | Opens the folder that holds `settings.ini` (on Windows `%APPDATA%\CastleOfIllusion\Port`) in the file manager |

Switching widescreen during a stage also updates the camera limits, so the stage plays as if it had started in that mode. The newly shown strips at the sides can show leftover tiles until the view has scrolled across them.

Widescreen, Fullscreen, Easy Bounce Attack, Smooth Swinging, Smooth Scrolling, Clean Rock Edges, Smooth Fades, Remove Sprite Limit, V-Sync, Frame Rate, High Refresh Interpolation, MD+ Soundtrack, Show FPS, the Upscaler (and cleanEdge's Line Width), Scanlines and the CRT shader and its settings, Quick Settings (and Custom's choices), and the cheats are remembered between runs, in `%APPDATA%\CastleOfIllusion\Port\settings.ini` (one `key = value` line each; delete the file to go back to the defaults). `--no-widescreen`, `--easy-bounce`, `--smooth-swing`, `--smooth-scroll`, `--no-smooth-scroll`, `--vanilla-rock-edges`, `--vanilla-fades`, `--show-fps`, `--interpolate`, `--no-interpolate`, `--frame-rate N`, `--mdplus`, `--no-mdplus`, `--cheat NAME` (invincibility, infinite-power, infinite-tries, infinite-items, infinite-continues, full-power, walk-faster, moon-jump, midair-jump; repeat for more), `--crt`, `--scanlines`, `--hqx` (5 = cleanEdge) and `--clean-edge-width` apply to that run only and don't change the saved settings.

### Starting at a given stage

`--stage N` starts the first level of a new game at stage N (1-28) instead of the first one. Press Start through the title as usual; when the game reaches its first level start, it loads stage N, and from there the game carries on as normal (finishing it leads to stage N + 1).

| Level                    | Stages | Boss stage |
|--------------------------|--------|------------|
| 1. The Enchanted Forest  | 1-4    | 5          |
| 2. Toyland               | 6-8    | 9          |
| 3. The Storm             | 10-12  | 13         |
| 4. The Library           | 14-22  | 23         |
| 5. The Castle            | 24-26  | 27         |
| Final Boss               |        | 28         |

Stages are numbered in the order they are played, and stages 27 and 28 are back-to-back boss fights. Internally the game numbers its stages 0-27 at `$FFFF42` (mapped to a level and act through the tables at `$00A008`/`$00A024`) in a slightly different order: in The Enchanted Forest, stages 2-4 are internal 2, 3 and 1, and in The Library, stage 15 is internal 21 (stages 16-22 are 14-20). The port's table is in `port/src/stages.h`.

```bash
build/port/Release/coi.exe --stage 14
```

`--show-state` shows the current `GameState` in the window title, which helps when reporting where something happened.

Widescreen (400 x 224) is on by default during level play; other screens are shown at 320 pixels with black borders. `--no-widescreen` starts at the original 320 x 224 (the F1 menu can switch widescreen on). The patches behind it are listed in `port/src/patches.cpp` and explained in [docs/widescreen.md](docs/widescreen.md).

### Boss Rush

The game's options screen (Start at the title) has a fourth row, BOSS RUSH; B or C ticks it. With it ticked, Start plays the six boss fights back to back: the Old Oak Tree, Jack-in-the-Box, the Storm Men, the Licorice Dragon, the Oaf and Mizrabel (stages 5, 9, 13, 23, 27 and 28).

- Normal and Hard only: Practice has no bosses, so it can't be ticked there (switching to Practice unticks it). Each difficulty's bosses are the game's own.
- One try for the whole rush, no continues and no extra tries. Power starts as in a new game; on Normal it refills to that at every boss, on Hard it carries over.
- Each boss's clock runs from the moment you get control until it is defeated (pauses don't count) and shows in the HUD in place of the score. There is no gem to collect: 5 seconds after a defeat the screen fades and the next fight starts.
- After Mizrabel, her fade to white leads to a results screen in the style of the level-complete screen, with each boss's time and the total counting up to 1/100 s; a button then returns to the Sega logo. The ending isn't played.

The tick stays on until you untick it or restart the port, like the screen's other settings. The new code is 68000 assembly in `port/src/boss_rush.asm` and `boss_rush_results.asm`, placed at `$0D0000`, past the ROM and clear of Smooth Swinging and MD+ packs; `python tools/boss_rush_asm.py` reassembles it with asl into `boss_rush_code.h` (committed, so building the port doesn't need asl). The patches that call it are in `port/src/boss_rush.cpp`.

### MD+ soundtrack packs

MD+ is the CD-audio feature of the MegaSD and Mega EverDrive Pro cartridges: fan-made packs pair a patched ROM with a new soundtrack, which the cartridge plays from WAV files when the game asks. The port does the cartridge's part. A pack is not included; like the ROM, you supply it. Extract it into a folder named `mdplus` beside `rom/` (it holds a `.cue` sheet, the patched `.md` ROM and one WAV per track; `mdplus/` and `*.7z` are ignored by git). The pack's ROM must be this game with the pack's changes (it is checked against yours), and the port's own patches and cheats still apply on top. The game's sound effects play as usual; its music is replaced. Smooth Swinging's data moved to `$0C0000`, clear of the packs' code at `$080000`.

## Tracing code

```bash
python tools/trace.py
```

Follows code from the entry points and tables in `config/`, and writes a code map, subroutine list, jump tables and unresolved jumps to `build/trace/`.

## Roadmap

1. Matching disassembly: identify and label all code and data.
2. Static recompilation of 68000 code to C, with SDL-based VDP, YM2612 and PSG back ends.
3. Widescreen: a wider VDP renderer, extended tile streaming, object activation ranges and camera limits.
