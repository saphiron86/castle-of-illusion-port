# Decompiling into readable C

The step after the translator (docs/native.md): the game's 68000 code as C a person can read
and change, generated on the player's own machine from their own ROM, that still runs the game
exactly as the console does: every frame the same as the interpreter, interrupts on the same
instruction, the same cycles.

## Decisions (agreed 2026-11-03)

- **Generated locally.** The C is game code, so it is never committed or shipped. The repository
  holds the tool that writes it (`coi_recomp --readable`, from the same front end as the
  translator) and what makes it readable: names, types and notes for addresses (`config/`,
  addresses only, as `config/labels.txt` and `config/ram.txt` are). The C goes into the build
  folder, like the C back end's code today.
- **Cycle-exact.** The readable code is checked the way the translator is: the frame hashes
  against the interpreter, the replays, the probes, the sweep. Readable is not allowed to mean
  approximate.

## How readable code stays exact

The translator's code can stop before any instruction (when a scanline's cycles run out) and
carry on from there later. Readable C can't: an `if` or a loop has no place to resume in the
middle. So each function exists twice in the generated code:

- **Readable**, entered only at its start (a call, a jump to it). It counts cycles per stretch of
  straight-line code, and before each stretch checks that the most it can use still fits the
  slice; if it might not, it hands the rest of this call to the second copy, at that address.
- **Resumable**, the C back end's code as now (every instruction a label, gotos), which runs
  instruction by instruction to the exact point the 68000 would stop, and on from there in the
  next slice. It returns to the same caller, so the readable caller just carries on.

Most of the time is spent in readable code: a call that a slice's end interrupts finishes in the
resumable copy, but every new call starts readable again. Both copies come from the same lowered
code (lower.cpp), so they can't disagree about what an instruction does.

## Readability, in stages

1. **Structure.** Each function's blocks as `if`/`else`, loops (`dbra` as a counted loop,
   backward branches as `do`/`while`), and `switch` for the jump tables `config/recomp.txt` and
   the tracer know; `goto` only where nothing else fits. Registers as `D0`-`D7`, `A0`-`A7`;
   sized parts as `D0.w`-like helpers; memory through `read8/16/32`, `write8/16/32`.
2. **Conditions.** A compare or test whose flags only the next branch reads becomes the C
   comparison itself (`if ((int16_t)D0 < 5)`), using the dead-flag analysis the JIT has; flags
   are only kept where something else reads them.
3. **Names.** Functions, jump tables and RAM variables from `config/labels.txt`,
   `config/ram.txt` and `config/hardware.txt` (VDP and I/O ports); structure fields for the
   object slots (`$FFD000`...) once those are mapped.
4. **Types and locals.** Registers that a function only uses as temporaries become C locals;
   arguments and results documented per function.
5. **The port's patches as source.** Widescreen, Boss Rush, cheats and smooth swinging are ROM
   patches now; in decompiled code they become C changes behind settings: first as variants
   generated from each setting's image (below, "One build for every setting"), then, where it
   reads better, hand-written C in their place, each checked against its variant (agreed
   2026-11-04; the cheats are by hand now, below, "Patches by hand").

## Building and checking it

With your ROM in `rom/` (a 512 KB `.bin`, as for the port itself):

```bash
cmake -S port -B build/readable -DCOI_READABLE=ON
```

```bash
cmake --build build/readable --config Release --target coi
```

`-DCOI_READABLE_ROM=<file>` names the ROM instead. The build makes two tools and runs them:
`coi_family` writes the image family (`build/readable/family/`: the ROM with no setting on and
one image per setting, as the port patches it, `port/src/rom_family.cpp`), and
`coi_recomp --readable` translates it, with the functions in `config/recomp_functions.txt`,
into `generated/readable.c` and `generated/resume.c` in the build folder. That C is the game's
code: it stays in the build folder and is never committed or shared. The game then runs it
(translated code is on by default; `--no-native` runs the interpreter instead) with any mix of
settings; for any other ROM image (an MD+ pack's ROM patches the game's code) it runs the
translator's machine code instead, as a build without the readable C does. Desktop builds only.

For development, one image alone (`-DCOI_NATIVE=ON -DCOI_DECOMP=ON
-DCOI_NATIVE_ROM=<image>`, the image from `coi --no-mdplus --cheat invincibility --save-rom
patched.bin --frames 1` with the test run's settings), or a family made by hand (`coi
--save-rom-family DIR --frames 1`, then `DIR/base.bin` as `COI_NATIVE_ROM`), and
`-DCOI_NATIVE_SWEEP=<trace>/subroutines.txt` for a fresh trace's list. Without `COI_DECOMP` the
same build writes plain C (the C back end). Everything docs/native.md's Testing section lists
applies: the replays, `--state-hash` against the interpreter, probes.

The pieces: `port/recomp/readable.cpp` (the emitter), `port/recomp/readable_rt.h` (the names
the C is written in: `D0`-`A7`, `read16`, `set_w`, `sub_flags`, `NE()`, `dbra`, `CYCLES`,
`BUDGET`, `CALL`, `JUMP`), and `coi_recomp --readable`, which also gives each instruction its
assembly as a comment (Musashi's disassembler) and each function its name from
`config/labels.txt` (`sub_XXXXXX` otherwise). The readable functions are `static` (a game's
names can be anything, a Windows API function's included); the resumable copy reaches them
through `coi_native_lookup`.

## One build for every setting

The port's settings that patch the ROM (widescreen, smooth swinging, each cheat; Boss Rush is
always in, switched on at run time) each make a different image, so the C generated from one
image ran only with those settings, and the sweep needed a build per image. Now:

- `coi --save-rom-family DIR` writes `base.bin` (no setting on) and one image per setting
  alone, with `family.txt` naming them in a fixed order (`kRomSettings` in main.cpp): setting
  k is bit k. The settings never touch the same bytes, and the image with all of them on is
  exactly the base with each one's changes (checked: 0 bytes differ), so any mix is these
  together.
- `coi_recomp --readable` given `DIR/base.bin` reads the family. What a setting adds outside
  the base's code (widescreen's routines at `$07FED6`-`$07FFD1`, smooth swinging's at
  `$0C0000`, midair jump's at `$090000`, their data) joins the base image, so every function is
  in one program. A function whose code a setting changes (any byte of it, in either image)
  is written again from that setting's image, readable and resumable, as `Name__setting`
  (`TouchHurtsMickey__invincibility`); the base function starts with
  `if (coi_native_settings & bit) return Name__setting(c, at);`. 92 variants: widescreen 22,
  smooth swinging 8, invincibility 18, infinite power 16, infinite tries 18, the rest 1-4 each.
  The resume table maps an address to the first function holding it; where a setting's version
  of that function doesn't hold it (the instruction is another function's with the setting on),
  the variant hands it on (33 such).
- The runtime accepts the family when the base image and each setting's image that is on are
  the ones it was made from, and the ROM running is that mix; `coi_native_settings` is then the
  settings' bits. A setting switched in play rebuilds the ROM, as before, and the bits follow.

Each variant is the exact translation of its own image, so exactness holds as it does for one
image; the checks are the same, per setting.

Checked (2026-11-04), all in the one build: the 15-stage run with no setting on, each setting
alone and all of them (moon jump aside, which midair jump switches off), each identical to the
interpreter on all 13,530 frames, strict, 100.0% translated; the sweep, strict, 4:3 and with
widescreen on: 90 of 90 identical each, 100.0%, the scenarios without cheats included (one
build covers them now); widescreen and smooth swinging each switched twice in play: identical
on all 10,000 frames. The single-image build unchanged (replays 524 of 524, the 15-stage run
identical). On the way: a function the base reaches only through callers a setting changes is
missing from that setting's program unless asked for (each setting's program now takes every
base function), and the resume points above (which the lockstep check, `COI_NATIVE_CHECK`,
found at frame 134).

## Patches by hand

The cheats are a few bytes each, so a variant (a whole function again, for an instruction or a
constant) hides what they change. `config/patches.txt` holds them as hand-written C instead: the
port's own code, keyed by address, no game code. Two kinds:

- **An instruction** (`@ <address> <setting> [note]`, then the C): run in place of the instruction
  at that address while the setting is on, in readable_rt.h's names. In every function that
  holds the instruction, readable and resumable:

  (For example, the infinite-items cheat replaces the instruction that takes one off
  `ThrowCount` with a test of it: inside `if (SETTING(infinite_items))` its cycles and a
  `test_flags` of the count; in the `else`, the game's own subtract, as generated.)

  The C does what the patched 68000 code does, exactly: its cycles, every flag, register and
  memory access. It goes on to the next instruction, or ends with a `return` or `JUMP` (an `rts`
  in the patch: then there is no `else`, and the game's code follows at the same level). A patch
  of several instructions marks where each next one starts with `STEP(address)`, so the
  resumable copy can stop there as the 68000 can (invincibility's three `nop`s in place of a
  `bset`).

  The setting's code may replace more than one of the game's instructions: widescreen puts a
  `jsr` to its own routine and a `nop` where two of the game's instructions were. The entry then
  replaces all of the game's instructions up to where the setting's and the game's start one at
  the same place again (or up to past where the setting's code leaves); they are the `else`, and
  they must be straight code nothing else enters. A call to one of the setting's own routines is
  `CALL(<name>, <return address>)`, by its name in `labels.txt` (the resumable copy calls it
  through the lookup); the routine itself stays generated code, one function in the base program.
  After the call a new stretch starts (as after any call), and a `STEP` after it is
  `STEP_AFTER_CALL`, which checks the budget again, since the callee's cycles weren't in it:

  (For example, widescreen's camera minimum: inside `if (SETTING(widescreen))` its cycles,
  `CALL(WideCameraMin, <return address>)` and `STEP_AFTER_CALL`; in the `else`, the game's two
  instructions, as generated, then a `BUDGET` check for the rest of the stretch.)
- **A named value** (`= <address> <setting> <name> <value> [note]`): the instruction stays, and
  the one constant the setting changes in it gets a name, `#define WalkLimit (SETTING(walk_faster)
  ? 0x25000 : 0x16000)`, so the code reads `if (D1 < WalkLimit)`, `D0 = StartPower;`. The
  instruction is generated as before, with the name where the constant was.

`SETTING(name)` is the setting's bit in `coi_native_settings` (the image family's order, defined
at the top of the generated files). The generator checks each entry against the setting's image:
the setting changes bytes of the base instructions it replaces (any it changes elsewhere still
make variants of the functions holding them); an instruction
patch's `CYCLES` add up to what the setting's code there takes (its `STEP`s where its
instructions start); a named value's instruction lowers exactly as the base's but for that
constant, which it reads from the base. Those bytes are then no variant's, so a setting all of
whose changes are in the file has no variants at all. Inside the generated C, a patched
instruction is a point where every flag and register is exact (no comparison folded across it,
no register kept in a local across it), since the C may leave flags as they were where the
game's instruction would have set them.

What is by hand (2026-11-04): invincibility (the three `rts`s, the hazard's `nop`s), infinite
power, tries, items and continues (each a `tst` or a `bra.s` over the subtraction), full power
(`StartPower`, `HardStartPower`) and walk faster (`WalkLimit`, `WalkSlowAbove`). What stays a
variant: widescreen (22 functions) and smooth swinging (8), whose changes are routines and data
rather than an instruction here and there; moon jump, which turns two branches into `nop`s (the
structured code is built on those branches; a variant reads better than a branch that may not
branch); and midair jump, which replaces two instructions with a call to its own helper and
changes where the instructions after start. 33 variants in all (from 92).

Then (2026-11-05) widescreen and smooth swinging. Their bounds and sizes are named values:
`ClipLeft`/`ClipRight` (ClipSpriteX), `ViewBlocks` (IsRectVisibleA and B), stage 15's building
tops (`TopsHideLeft`/`Right`, `TopsFromLeft`, `TopsToRight`), `SpawnRowCells`, and smooth
swinging's `SwingScript`. Their calls to their own routines are instruction patches of the kind
above: widescreen's camera limits (`WideCameraMin`, `WideCameraMax`), the player's left wall
(`WidePlayerLeftWall`), the cameras clamped before the first fill (`WideClampCameras`), the
view's first block (`WideViewStartA`/`B`), the spawn row's start (`WideSpawnRowStart`) and the
column test (a `jmp` to `WideSpawnColumn`); smooth swinging's starting step (`SwingStartStep`),
Mickey's offset (a `jmp` to `SwingOffset`) and his pose (`SwingPoseStep`). The routines
themselves (widescreen's at `$07FED6`-`$07FFD1`, smooth swinging's at `$0C0000`, midair jump's at
`$090000`) were variants too, copies of the same code, since the base image has `$FF` there; a
routine the base program has only from a setting's image is now no variant (only that
setting's code reaches it). What stays a variant: widescreen's `DrawObject`, whose deletion range
is a `jsr` and four `nop`s in place of a `cmpi`, two branches (`bcc.w`, `bmi.w`) and a `tst`, two
of the `nop`s starting where the game's `tst` and `bmi.w` do (as moon jump's: the structured
code is built on those branches, and a resume at `$001B1A` would have to tell the `tst` from the
`nop` by the setting); moon jump; and midair jump's one function. 3 variants in all (from 33).

Checked (2026-11-05), in the one build: the 15-stage run with no setting on, each setting alone
and all of them, identical to the interpreter on all 13,530 frames, strict, 100.0% translated;
replays with none, widescreen, smooth swinging, both, and both with seven cheats: 527, 529, 527,
529 and 525 of each, plain and stopped partway; the sweep, strict, 4:3 and with widescreen on:
90 of 90 identical each, 100.0%; widescreen and smooth swinging each switched twice in play:
identical on all 10,000 frames. A build that printed each patch the first time it ran showed
every widescreen and smooth swinging patch running, in at least one of the two copies, but
Mickey's pose (`$00B73A`): no test run has him hold a vine (the sweep's routes place him by
poking his position). A run that does (stage 1, right held, a jump every 60 frames held for 20,
3,000 frames: he swings from frame 1415) runs it in both copies, and is identical to the
interpreter with no setting, widescreen, smooth swinging and both (strict, 100.0%), its replays
too, plain and stopped partway. That run is now one of the sweep's scenarios, twice
(`stage01_vine` with smooth swinging, `stage01_vine_classic` without), so `tools/native_sweep.py`
checks it every time: through the readable C, strict, 4:3 and `--wide`, all four identical,
100.0% translated, Mickey in his swing state (`$FFDC03` = 37/38) in each (2026-11-05).

Checked (2026-11-04), in the one build: the 15-stage run with no setting on, each setting alone
and all of them together, identical to the interpreter on all 13,530 frames, strict, 100.0%
translated; replays (with invincibility, with every cheat, with none) 524, 525 and 527 of each,
plain and stopped partway; the sweep, strict, 4:3 and with widescreen on: 90 of 90 identical
each, 100.0%. And, since the 15-stage run never throws or continues, each cheat added to the
sweep's scenarios that reach its code (the stage routes with infinite items, the deaths with
infinite power, tries, continues and full power, the Hard and Practice routes with full power):
all identical. A build that printed each instruction patch the first time it ran showed every
one running (most in both copies; infinite tries' falling-off one only readable, infinite
continues' only resumable) but the hazard's: no test run walks into a deadly block with
invincibility on (stage 24's route does without it, but only after a knock back that
invincibility prevents), so that one is checked by its cycles against the image and in play:
on stage 24 with invincibility, `--native`, Mickey standing in the deadly block ran the patch
1,264 times (the write to `$F44A` from `$00BFFE` right after it, seen with `--watch`), nothing
marked him dead and he played on as normal (2026-11-04). The named values were seen in effect
(power 5 on Hard with full power).

On ARM64 (the family build cross-compiled for ARM64 Linux, run in `qemu-aarch64`, against the
Windows interpreter), 2026-11-04, with no setting on, with invincibility and widescreen, and
with widescreen, smooth swinging and seven cheats (all but moon jump and infinite continues): the 15-stage run
identical on all 13,530 frames each, strict, 100.0% translated; replays 527, 524 and 525 of
each, plain and stopped partway.

## Status

Stage 1, first slice (2026-11-03): 1,734 functions, about 30,000 instructions.

| | |
|-|-|
| Structure | 305 `do`/`while` loops (277 of them `dbra`), 18 `for (;;)`, 155 `if`/`else`, about 73,000 `if` blocks, 146 `switch`es for jump tables; 1,933 `goto`s between instructions and 262 inside them remain |
| Replays, 15 stages | 524 of 524 calls match, and 524 of 524 when stopped partway (468 handed to the resumable copy) |
| The game, 15 stages, `--native` | identical to the interpreter on all 13,530 frames, 100.0% translated, nothing handed to Musashi |
| 68000 time, same run | 0.83 s (the JIT: about 1.25 s on the same day) |

Found on the way: a function's code can start below its entry (`NemDec_WriteVDP` at `$07FC70`
loops back into `$07FBF8`), so a readable function starts with a `goto` to its entry when it
isn't the lowest address.

Stage 2 and the first of stage 3 (2026-11-04):

- **Flags only where they can be read.** Within a stretch, a flag the next flag operation
  overwrites before anything reads it isn't computed (the JIT's analysis); at a stretch's ends,
  calls, returns and anything that can stop, all are exact, so wherever the 68000 can be seen
  (a scanline's end, an interrupt pushing SR) its flags are the console's. A `move` whose flags
  die is a plain assignment, an `add`/`sub` plain arithmetic, a dead `cmp`/`tst`/`btst` nothing.
- **Comparisons.** A branch right after a `cmp`, `tst` or move (or a `sub`, for `eq`/`ne`/`mi`/
  `pl`) in the same stretch tests the values themselves: `if ((uint8_t)D1 >= 0xFC)`. The
  flags are still set where something after may read them. Conditions are inverted rather than
  negated (`(uint16_t)D6 < 9`, not `!((uint16_t)D6 >= 9)`).
- **Masks only where a bit beyond them is used.** Which bits of each value anything uses (a
  word written to a register part, memory or a sized helper uses 16): `set_w(D1, D1 + D1)`, not
  `set_w(D1, (D1 & 0xFFFF) + (D1 & 0xFFFF))`.
- **Fixed addresses by name** (`config/ram.txt`, `config/hardware.txt`): `write8(VBlankWait, 1)`,
  `read16(GameState)`, `VDP_Ctrl`, `Z80_BusReq`.

| | first slice | now |
|-|-|-|
| Lines | 384,000 | 305,000 |
| Flag helpers (`*_flags`) | about 30,000 | 9,100 |
| Branches as comparisons | 0 | about 2,400 |
| Replays (plain, paused) | 524 / 524 | 524 / 524 |
| The 15-stage run | identical | identical |
| 68000 time, that run | 0.83 s | 0.70 s |

Then (2026-11-04, later):

- **Short branches inside an instruction as `if`s**: a test that skips straight code to a label
  right after it (the privilege check and the interrupt check of an SR write, a stub check)
  is `if (...) { ... }`; 198 such `goto`s left (from 262).
- **Constants folded**: `if (3 < v1)`, not `if (((0x2300 >> 8) & 7) < v1)`.
- **Only what can be observed**: an instruction with no memory access, call, stop or exit
  shows its address and assembly as a comment (no `AT`, which only matters when the port looks
  at the PC: at the bus, at a stop); its cycles are added with the next instruction's, and the
  cycles a branch not taken gives back go on to the code after it, so each stretch counts its
  cycles once or twice rather than once per instruction. Every `BUDGET`, memory access, call
  and exit still sees the exact count. 25,500 `CYCLES` lines (from about 32,000), 24,500 `AT`
  (from 30,000); 297,000 lines.
- **Checked with a second set of settings** as well (4:3, no cheats, so Mickey takes hits):
  the 15-stage run identical to the interpreter on all 13,530 frames, replays 527 of 527 (472
  stopped partway); with the first settings, 524 of 524 both ways and identical as before.

- **Notes from `config/`**: a function's comment in `labels.txt` goes above it
  (`/* MainLoop: call the current game state, then wait for vblank */`, 46 so far), and an
  address's comment in `ram.txt` or `hardware.txt` beside its `#define`. More notes there make
  the generated C more readable without touching the generator.

- **Object fields** (`config/objects.txt`, offsets and names only): `A0 + offset` is named
  where the offset is a known field, A0 being the object the game is running (the engine's
  convention; where a routine uses A0 for something else, the name is wrong but the code isn't).
  Seven fields so far, from what docs/widescreen.md and `ApplyVelocity` establish (`ObjFlags`,
  `ObjTileBase`, `ObjX`, `ObjY`, `ObjVelX`, `ObjVelY`, `ObjSpawnEntry`): about 2,100 uses.

- **Calls as one line**: `CALL(fn, ret)` pushes the return address itself (2,456 calls);
  `CALLED` is left for the 86 through RAM stubs, whose `jmp` is checked between the push and
  the call.
- **Jump tables folded**: a known target whose case only jumps to the address it tests for is
  what the jump after the switch does anyway (the same lookup, the same cycles), so it's left
  out, with a count in a comment; the object dispatch at `$001C82` is `JUMP(v2)` with "633
  known targets". 8 `switch`es are left (calls, stub targets, code not translated), and 2,047
  `case`s (from 72,836).

153,000 lines now (from 384,000 in the first slice). Checked again: replays 524 of 524 both
ways, the 15-stage run identical on all 13,530 frames, 68000 time 0.67 s.

Stage 4 begun (2026-11-04, later):

- **Register values as C locals.** From an instruction that writes a register in full to the next
  that does, inside straight code (each instruction's only way in is the one before, no stretch
  starts between), nothing can see the register if nothing between calls, returns, jumps,
  stops, checks the budget or touches the stack. So there the value is a `const` local, named
  after the register (`d0_4`, `a1_2`), and the instructions between read it. The register
  keeps its old value until the write that ends the local, which is exact; at every point the
  68000 can be seen, all registers are the console's. One exception is checked: the port's
  frame interpolation reads A0 whenever the game writes the sprite list, so A0 is never a local
  across a memory write. 1,746 locals, mostly A1 and A6 walking through tables
  (`a1_4 = a1_3 + sx16(read16(...))`); no analysis across functions was needed after all, since
  a local never outlives its stretch.

  (A field read and an add to another field look like `read32(A0 + ObjVelX)` and
  `write32(v1, read32(v1) + d0_1)`, with each instruction's assembly as a comment.)

- **More object fields**: `ObjRoutine` ($3, the index the routine dispatch at `$1C74` jumps
  through), the animation's `ObjAnimScript`, `ObjAnim`, `ObjAnimFrame`, `ObjAnimTimer` ($6,
  $A-$C, from `AnimateObject` and `DrawObject`), and `ObjTimer` ($22, a word counted down 220
  times in the code, each followed by a branch on it reaching 0). Thirteen fields now. The
  most used offsets still unnamed ($29, $28 bit flags, $26, $2E, $32, $36) mean different
  things to different kinds of object, so they need naming per routine, not per slot.

- **The sweep through the readable C** (`tools/native_sweep.py --build build/decomp --no-jit`):
  one build per ROM image, since the C is generated from one image: a build from the image
  with invincibility for the scenarios that use it, one from the image without cheats for the
  other 8 (boot, title, options, deaths, pause). 4:3, strict: 90 of 90 identical to the
  interpreter, 100.0% translated, no handovers. With widescreen on: the 82 with invincibility
  identical, 100.0%. Checked again with locals and the new fields: replays 524 of 524 (plain,
  paused), the 15-stage run identical on all 13,530 frames.

- **Objects by kind.** `RunObject` (`$1904`) runs the object in a slot: the word at `(A0)` is
  its ID, and its code comes from `ObjectCodeLow` (`$1D40`, a word for each of IDs `$00`-`$C7`)
  or `ObjectCodeHigh` (`$1ED0`, a long each from `$C8`). An object's routines are a table it
  loads with `lea (d16,PC),A1` before jumping to `RunRoutine` (`$1C74`, absolute words) or
  `RunRoutineRel` (`$1C84`, offsets from the table), which picks entry `ObjRoutine`. Those
  addresses are named in `config/labels.txt`; from them the generator names each object's code
  `ObjXX` (XX its ID) and each routine its table lists `ObjXX_N` (N the `ObjRoutine` that
  selects it), with a note on each, wherever `labels.txt` gives no name of its own: 208
  objects (238 IDs; IDs sharing code are noted) and 707 routines, so 740 functions are still
  `sub_` (from 1,659). Names only: the generated code is otherwise the same, and checked again
  (replays 524 of 524, the 15-stage run identical).

- **What each object is** (`config/object_ids.txt`: ID, name, a few words and the stages it
  appears in): 80 IDs, first named from the sprite sets the sweep's captures drew (each ID's
  frames side by side; the pictures stay with the player's captures, the file holds only
  words), then corrected by someone who knows the game. `ObjMushroom` and `ObjMushroom_2`
  rather than `Obj50` and `Obj50_2`; the bosses as their parts (`ObjJackInTheBoxGloves`,
  `ObjMizrabel` with 18 routines). Code IDs share is named after the first with a name. Four
  IDs that draw nothing in the captures (`$0F`, `$91`, `$94`, `$97`: controllers or
  triggers, by the look of it) and two unclear ones (`$59`, `$5A`) keep their numbers, as do the
  IDs no capture saw. Changing a name in the file changes the C.

- **The helpers objects call most**, named in `config/labels.txt` from what their code does:
  animation (`SetAnimation`, 159 calls; `NextRoutineSetAnim`), `FindObject`, `Random`, the
  collision helpers (an object's hitboxes are `$20` and `$21(a0)`, indexes into a table of
  boxes at `$699CE`: `LoadHitbox`, `HitboxesOverlap`, `TouchesMickey`, `BouncedOn`, `HitByThrown`,
  `BlockAt` and `ProbeBlocks` for the level's blocks), `ThrowItem`, and `LoadPalette`. With them
  in `config/ram.txt`: `ThrowCount`, `Palette`, `TargetPalette`, `RandomSeed`, and `StageIndex`,
  `Level`, `Act` (from bus.cpp's stage select). 18 functions, called from about 700 places;
  722 still `sub_`. Names only: checked again (replays 524 of 524, the 15-stage run identical).
  The three named from play were checked in play: on stage 1, with `--watch` on the RAM each
  one writes (and the PC of the write), `BouncedOn` fired once, on a bounce; a pickup raised
  `ThrowCount` to 1, `ThrowItem` took it back to 0 and filled the first thrown-item slot; and
  the throw hitting an enemy cleared that slot through `HitByThrown`.

- **The next 16**, from their code, the original ROM and the notes in `port/src/cheats.cpp`:
  `TouchHurtsMickey` (70 calls) and `TouchHurtsMickey2`, which call `HurtMickey`; `RunAllObjects`;
  the fades (`StartPaletteFade`, `PaletteFadeStep`, `ScreenFade`, `FadeComponent`); `SetProbes`;
  `InitPlantObject` and `InitMushroomObject` (their animation tables are the ones the sprite
  sets of the trees, tulips and mushrooms use); and the decompression (`DecompressSets`,
  `DecompressSetsToRam`, `DecompressToBuffer`, and `EniDec`, Enigma tilemap decompression, with
  its bit reader). In `config/ram.txt`: `Power`, `Tries`, `MickeyFlashing`. 706 still `sub_`.
  Checked again: replays 524 of 524, the 15-stage run identical.
- **The C is the image's.** It is generated from the ROM as patched for the run, so the
  cheats are in it: with invincibility on (the usual test image), `TouchHurtsMickey` and
  `HurtMickey` return at once, as the cheat makes them. To read the game's own code, generate
  from an image without cheats (`build/decompnc`, say).

- **Addresses the code loads by name**: a `lea` or `pea` of a fixed address (absolute or
  PC-relative) names it as reads and writes do: `A2 = VDP_Ctrl` (52 times), `A1 =
  WORD_ADDRESS(Palette)`, the ROM's tables (`labels.txt`'s data labels: `ObjectCodeLow`,
  `LevelTable`). `WORD_ADDRESS` is what a `.w` operand gives for RAM, the address with its top
  byte set ($FFFFF780 for Palette at $FFF780), so the register is exactly the 68000's; only
  these instructions are named, so a number that happens to equal an address stays a number.
  Checked: replays 524 of 524 (paused too), the 15-stage run identical.

- **Mickey** (object `$01`, the slot at `$FFDC00`): his routine table (at `$AF5C`) is reached by
  a `jsr` to `RunRoutine`, which the generator now recognises as well as a branch or jump (127
  more routines named across the objects, 834 in all). His states come in pairs, the second
  facing left, and were found by logging `$FFDC03` while scripted presses played stage 1:
  `MickeyStand`, `MickeyWalk`, `MickeyJump`, `MickeyJumpMoving`, `MickeyFallMoving`,
  `MickeyCrouch`, `MickeyLand`, `MickeyBounce`, `MickeyThrow`, `MickeyThrowInAir`, with
  `MickeyStart` (state 0) and `MickeyFall` (by its code); and the per-frame helpers they share
  (`MickeyOnGround`, `MickeyInAir`, `MickeyOnGroundStill`, `MickeyStop`), with three from
  `cheats.cpp` (`MickeyJumpRise`, `MickeyFellOff`, `MickeyHazard`). Ten more were found by
  playing (a live view printing each state change, from `--watch` on `$FFDC02`): the throws
  from a running jump and crouching, `MickeySlope`, `MickeyDying` (sinking into jelly or a
  liquid, a fatal hit), `MickeyPitFall`, `MickeyHurt`, `MickeySwing` (a vine or rope),
  `MickeySwim`, `MickeyCarried` (an underwater tunnel's set path), and `MickeySlip` (a small
  slide from standing while his footing settles). And the finale, 47 to 50, the only states
  without a facing-left pair: after the final boss (`$FFF45A` bit 0) Mickey falls to the floor,
  walks to his place, waits, and the last state brings in object `$6C`
  (`MickeyFinaleFall`/`Walk`/`Wait`/`End`). The last two were found by running the sweep's
  scenarios with his state logged: 45, caught by stage 12's raging current (then played to
  check: dragged back, one power lost), and 43, a passage between floors that an invisible pair
  of objects `$8F` starts in stage 15's arched doorway on Hard. All 28 of his states are named.
  Then his helpers by what calls them: the pad handlers per kind of state
  (`MickeyGroundInput`, `MickeyAirInput`, `MickeyCrouchInput`...), the one-line state setters
  (`MickeyToStand`, `MickeyToHurt`..., found in the ROM by their shape: state N, its
  animation, `MickeySetAnimation`), the landing and ledge checks, and each state's animation
  picker (`MickeyAnimJump`...); with `PadInput` and `MickeyGround` in `ram.txt`. A new kind in
  `labels.txt`, `label`, names code the states jump into: the generator makes those
  functions, but they are no subroutines to the tracer, so a `code` label would change the
  next trace. Then the rest of his code: the checks that start a state (`MickeyCheckWater`,
  `MickeyCheckSwing`, `MickeyCheckCurrent`...), his physics (`MickeyGravity`, `MickeyFriction`,
  `MickeyMoveY`, the swimming moves), the pad by direction (`MickeyWalkInput`,
  `MickeyPadLeft`...), the jump (`MickeyCheckJump`), the throw (`MickeyThrowSpawn`), and the
  blocks: what each type of block does to him through a table per side (`MickeyFloorBlock`,
  `MickeyLeftBlock`..., `BlockHurts`, `BlockPit`, `MickeyBumpHead`), and the solid objects
  (`ObjFlags` bit 5). 30 of his 121 functions are left, small fragments whose part the code
  alone doesn't settle (slope flags, side pushes, the thrown item's own block checks); 488
  `sub_` in all.

- **Fields per kind of object** (`config/object_fields.txt`: ID, offset, name, a note). The
  generator knows which functions belong to which object: its code and the routines its table
  lists, and then, until nothing changes, any function whose callers all belong to the same
  object (its own helpers: `MickeyAirSteer` is in no table, but only Mickey's code calls it).
  In those functions `A0 + offset` takes the kind's name first, the general one after. Mickey's
  three (`MickeyPath`, `MickeyPassageSteps`, `MickeyRequests`) are the first; he keeps most of his
  state in RAM variables. Two more general fields came out of the survey: `ObjSpawnArg` ($29, the
  spawn-list entry's second byte, which `SpawnCell` copies and each kind reads its own way: 156
  uses) and `ObjFrame` ($30). Across the enemies, $28, $2E, $32, $36 and $3A follow a common
  pattern; reading the mushroom, the bat, the toy soldier and the butterfly split them. Three are
  every enemy's, through the contact code they all jump into (`EnemyContact`: beaten when
  `ObjBeaten`, a bounce or a thrown item says so, else it hurts Mickey; then `EnemyToSparkle`
  adds `ObjPoints` to `Score` and turns it into a Sparkle, which deletes itself when
  `ObjLifetime` runs out). $32 and $36 are each kind's own: a starting X and an acceleration for
  the mushroom, where the bat hangs, the toy soldier's hop (`object_fields.txt`). Then
  LetterA, the knight (`KnightMace`: the mace it threw, beaten with it), SpringHeadSide,
  GummyFish, the butterfly, the dragon boss's hits and attack pattern, and stage 27's boss
  (`OafLegsHits`: at 16 it sets `BossBeaten`, which makes every enemy count as beaten):
  eleven kinds, 27 fields. On the way, the shortcuts objects use to pick an animation
  (`SetAnimation0`-`SetAnimation10`, `NextRoutineAnim0`-`NextRoutineAnim12`, at `$1D0A`-`$1D3A`).
  The kinds with the most unnamed fields left ($1B, $1E, $6F, $71, $77, $A8, $AF, $B1) never
  appear in play: watching every slot's ID through each stage's sweep route on Normal and Hard
  found none of them spawned. Their code is timers and routine changes; without seeing them,
  their fields stay numbers. A pattern most share: `$26`/`$27` a timer
  and the routine to go back to, `$32`/`$36` a home position or a speed kept for later.

- **The port's own code by its own names.** Boss Rush (at `$0D0000`) is assembled from
  `port/src/boss_rush.asm`; `tools/boss_rush_asm.py` now also writes `config/port_labels.txt`
  from the assembler's symbol table (its code labels, the RAM it names, and its entry table as
  `DrawOptionsEntry`...), which the generator reads after `labels.txt` (whose names come first).
  The asm's notes on the game's routines went into `labels.txt` and `ram.txt` too (`UpdateScore`,
  `DrawHud`, `ClearVRAM`, `HudDirty`, `PadPressed`). 429 `sub_` left (from 488).
- **The game's frame of play and level start, from the top**: what `GameState_0E` and
  `GameState_0C` call, in order (`DemoPlayback`, `UpdateBlockAnims`, `UpdatePaletteCycles`,
  `CheckGemTaken`, `CheckNextPart`; `SetLevelAndAct`, `InitHud`, `LoadLevelArt`,
  `PlayStageMusic`), with their RAM (`DemoMode`, `GemTaken`, `LibraryDoor`, `PartEnded`,
  `PadHeld`). 417 left.
- **The game states**, found by logging `GameState` through the sweep's boot, title, options,
  death, ending and Boss Rush scenarios and capturing a picture inside each: the Sega logo, the
  attract story, the title screen, the copyright screen, the options, the opening story, the
  castle hall between stages, the game over, the ending. `GameState_XX` keeps its name (the
  docs refer to them so) with what it is as its note; the six in the module at `$07A000` that
  states $18-$26 jump to are named for them (`GameOver`, `Copyright`, `CastleHall`,
  `StageClear`...); state $2C is Boss Rush's results. Then the module's own copies of the
  engine's helpers (`SetAnimationB` and its shortcuts, `FillLongs32b`, `CopyLongs32`) and
  `FadeInSetup`, `ClearRamBuffer`; the castle hall's (`LoadLevelPalette`, `SpawnObjectList`,
  `CastleHallRebuild`). The screens themselves are thin (a fade, then the objects): their work
  is in the objects on them, numbered kinds of their own. 394 `sub_` left.
- **Level start, cameras and each stage's frame effect** (2026-11-05), from `GameState_0C` and
  Mickey's code down. `StageSetup` runs each stage's own set-up from a table by level and act
  (`StageSetup01`...`StageSetup28`, stage numbers from the stage select's tables at `$A008`
  and `$A024`; stages sharing one, `StageSetup15to18`, and the shared parts,
  `StageSetupLevel0`/`Level1`). The level header's first two words pick the rest: the first
  (`StageEffect`, `$FF9808`) the frame effect `UpdateScrolling` jumps to after the cameras
  (`StageEffect01`...`StageEffect28`: palette rotations, plane B's line scroll, stage 12's
  moving background in six steps, `EraseRequestedBlock`), the second (`CameraBMode`, `$FF9802`)
  how plane B's camera follows plane A's (`CameraBMode0`-`11`, a fraction of each axis noted on
  each; mode 9 adds the line-scroll wave, `BgWaveStep`). With them the plane fill and the
  column and row drawing (`FillPlanes`, `StreamColumnAt`, `DrawColumnBlocks`, `DrawRowBlocks`,
  `DrawBlocksToPlane`), the line-scroll modes (`BgScrollPerLine`, `BgScrollPerCell`,
  `BgScrollMode`), the block animations (`BlockAnims` at `$FF9400`: `BlockAnimStep`,
  `QueueBlockAnimDma`, the `DmaQueue` VInt empties), the palette cycles (`StartPaletteCycle`),
  and VInt's modes by `VIntMode` (`VIntMode1`-`6`, `VIntFinish`, `ReadPad`, `RandomStep`,
  `UploadMickeyFrame`). 96 names, 6 RAM names. The names say what the code does, not what it
  looks like in play (a stage's effect is named for its stage); what each looks like is for a
  play-test. Then the bosses' inner routines, from their routines down (`BossFlashColours`, the
  capped accelerations `AccelVelYMax5`/`AccelVelYMax6`/`AccelVelXMax6`, each boss placing its
  parts: `JackInTheBoxPlaceParts`, `DragonHeadPlacePart`, `OafLegsPlaceParts`; Mizrabel's and
  the totem's spawns, `StormMenRisingMove`), and the module at `$07A000`'s helpers
  (`RunRoutineRelB`, `ApplyVelocityB`, `DeleteObjectB`, `ClearObjectsB`, the tile and block
  maps `DrawByteTileMap`/`DrawBlockMap32`, the number digits `NumberDigits`, `ScoreDigits`,
  `ScreenFadeStep`): 31 more. 255 `sub_` left (from 382 in the build these rounds started from). Checked after each of the four rounds: the 15-stage run
  with no setting and with all of them identical, strict, 100.0%; replays 527 and 525, plain
  and stopped partway; at the end the sweep (92 scenarios) strict, 4:3 and `--wide`.
  Then VInt mode 3's one-off DMAs (`VIntDma1`-`8`, picked by `VIntDmaRequest` at `$FFF40D`,
  each noted with its source, length and destination; `MizrabelEraseTiles` asks for the last),
  the dragon's segments (`DragonSegmentFollow`, `DragonSegmentPose`, `DragonSegmentsAdd`,
  `DragonSegmentsRemove`) and `OafLegsWalkStep`. And the generator names the module's objects'
  routines as it does the engine's: their code at `RunObject`'s entry is a `jmp` into the module
  (after at most one instruction), and the rest dispatches through the module's copy,
  `RunRoutineRelB`, so `object_names` follows that `jmp` (the target is `ObjXXMain`) and takes
  `RunRoutineRelB` as a dispatcher (`Obj82_0`...`Obj87_3`; 64 functions, `Obj62Main` and
  `ObjMickeyIconMain` among them). 175 `sub_` left. Checked again: the 15-stage run, the
  replays, the sweep strict 4:3 and `--wide`, 92 of 92 each, all identical and 100.0%. Then the
  HUD's helpers (`ShowPower`, `ShowTries`, `TwoDigitTiles`, `FillHudRows`, `DrawHudLayout`,
  `CopyLongs4`, `CopyLongs4ToPort`): 167 `sub_` left; the 15-stage run and the replays
  identical again.

Ranking by calls has reached code with one purpose each (a level-block builder, a layer set-up at
`$FF9400`, one object's spawn helper): naming those needs reading them in their context, the
level loading and Mickey's own code, rather than the call counts. The level loading and
cameras are named now (`$FF9400` is the block animations). Left: the `$07A000` module's
screens and their objects (`Obj82`-`Obj87`), the biggest boss routines (the dragon's body,
the castle boss's), `GameState_10`'s helpers, the VInt modes' DMA stretches at `$D1EA`-`$D392`,
and fields per kind of object (what `$26`-`$36` hold for each).
