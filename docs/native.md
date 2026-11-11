---
title: Native code
parent: Technical notes
nav_order: 1
---

# Native code (the translator)

The port can run the game's 68000 code as native code instead of through the Musashi
interpreter. The translator is built in by default (`-DCOI_JIT=ON`; `-DCOI_JIT=OFF` leaves it
out) and runs the game unless the port is started with `--no-native`. The interpreter stays
in, for whatever translated code can't run and while the ROM is being translated.

## How it works for players

The app carries a translator: as it starts, it translates the player's own ROM into native code
(x86-64 or ARM64), on a thread while the interpreter runs the boot and the Sega logo, and runs
that from then on. The repository and the downloads never contain game code: only the
translator and lists of addresses (`config/recomp.txt`, `config/recomp_functions.txt`,
`config/recomp_sites.txt`). The machine code comes from the port's own code generators (no
compiler is shipped with the app), so everything the port is built from stays under
permissive licences. Every frame it runs is the interpreter's exactly: same memory, same
registers, same cycles.

## Design

- **Front end** (`port/recomp/decode.*`, `program.*`): our own 68000 decoder (Capstone's 68000
  operand details aren't exact enough), then per function the instructions reachable from its
  entry: branches, `dbcc`, calls, and computed jumps from `config/recomp.txt` or the tracer's
  `build/trace/sites.txt` (every jump table `tools/trace.py` resolves, by the instruction that
  jumps through it).
- **Intermediate form** (`port/recomp/ir.h`, `lower.cpp`): each instruction lowered to a short
  list of three-address operations on 32-bit temporaries, in the order the 68000 reads and
  writes; operations that set the flags are the runtime's helpers, so Musashi's exact
  behaviour lives in one place. Every back end starts from it.
- **Machine-code back ends** (`--jit`): the app's own translator, IR to machine code in
  executable memory: `port/recomp/x64.cpp` (Windows and System V calling conventions) and
  `arm64.cpp` (Android, ARM64 Linux), on what they share in `jit.cpp` (`jit.h`). Plain so far:
  a stack slot per temporary (with the last result kept in a register for the next
  operation); add, sub, cmp and the logic group set the flags inline (x86 sets carry,
  overflow, sign and zero exactly as the 68000 sets C, V, N and Z at each size; ARM64 does at
  32 bits, so bytes and words are shifted to the top first, and a subtract's carry is the
  68000's inverted), the simple conditions read them inline, memory goes straight to the
  port's bus, and the rarer flag-setting operations call the runtime (`native_helpers.c`).
  Every function has the same frame, so code can resume at any translated instruction through
  any function's entry. The cycle budget is checked once per block (a run of instructions
  entered only at its first) against the most cycles the block can use; a block that might not
  fit runs from a second copy that checks before every instruction. In the unchecked copy,
  flags that are overwritten before anything could read them aren't stored (not while the
  harness traces). The build picks the back end for its CPU; `COI_JIT_ARCH=arm64` translates
  for the other one without running it, and `COI_JIT_DUMP=FILE` writes the code out for a
  disassembler. `-DCOI_JIT=ON` builds the translator and the harness; no ROM is needed to
  build, and `--jit` and `--native` are on by default. The configuration and the tracer's lists are built in: `config/recomp.txt`, and
  `config/recomp_functions.txt` and `recomp_sites.txt`, which `tools/trace.py` writes from its
  code map (addresses only, like `config/labels.txt`; run it again when the labels change).
  For trying others: `COI_RECOMP_CONFIG`/`COI_RECOMP_SWEEP` (with `sites.txt` beside the
  list), or a `recomp/` folder (`recomp.txt`, `subroutines.txt`, `sites.txt`; on Android in
  `Android/data/<app>/files/`).
- **What isn't translated.** Nothing the game runs in play: the boot code too since
  `move to usp`/`move from usp` are translated (the user stack pointer is `CoiCpu::usp`,
  Musashi's `REG_USP` while in supervisor mode, through `coi_m68k_usp`), and strict mode (below)
  proves it over the sweep. Translated code runs in supervisor mode only (the game never
  leaves it): an SR write that would clear S or set T stops before it, and `coi_native_enter`
  leaves user mode to the interpreter. A computed `jsr`/`jmp` with no known targets (no
  `call`/`jump` line, nothing from the tracer, which reads only the original ROM) doesn't stop
  the function from being translated; translated code stops there and the interpreter goes on.
  The port's own code needs its jumps in `config/recomp.txt` for that reason (Boss Rush's
  results screen, `$0D05D8`). `COI_JIT_LIST=FILE` lists every instruction translated, the
  functions left out and why, and those sites.
- **Busy-wait loops.** A `bne`/`beq` back to a `tst` of work RAM just before it (the game's
  `WaitVBlank`) waits for an interrupt to change that memory. Translated code runs between
  interrupts and the loop writes nothing, so the branch does all the turns of the loop the
  budget leaves room for at once: the cycles, and the `tst`'s flags, are what stepping would
  give. The flags at the branch can be from before an interrupt that returned there (the
  VBlank handler clears the byte and comes back to the `bne`), so the branch reads the byte
  again to decide.
- **Runtime** (`port/recomp/native_rt.h`): the register file and every operation's result and
  flags, following Musashi exactly; memory through the port's bus. Cycles are counted as
  Musashi counts them: its per-opcode table plus its few dynamic extras (untaken branches,
  `dbcc` loop/expiry, `scc` true on a register, shift counts, `movem` registers, `mulu`/`muls`
  operand bits).
- **Stopping and resuming.** Translated code can stop before any instruction, with registers,
  flags, memory and cycles exactly what the interpreter's would be there: when its cycle budget
  runs out, at an instruction it doesn't translate, at a call to code it doesn't have. Every
  return address is on the 68000's own stack, so the interpreter can simply carry on from that
  address, and translated code can start again at any of its instructions (each function takes
  an `at` address; `coi_native_resume[]` lists every translated instruction).
- **Running in the game** (`--native`, the default in builds with `-DCOI_JIT=ON`): Musashi's
  instruction hook (`port/src/musashi_conf.h`) calls `coi_native_enter` before each
  instruction. Where translated code covers the address it runs, chaining from function to
  function, until the cycles left in the current scanline's slice are used up; it hands back
  the cycles used and Musashi's loop carries on from where it stopped. A call or jump through
  a RAM stub stops before the stub's `jmp` when the budget is spent, since that `jmp` is an
  instruction of its own on the 68000 and may fall in the next slice; the next slice then
  starts at the stub, and `coi_native_enter` does the `jmp` itself (its 12 cycles, then the
  routine at the address the stub holds in RAM, as translated code does with budget left).
- **The native loop.** With translated code running and the ROM image translated, the
  port's scanline loop (`cpu_execute`) doesn't call `m68k_execute` at all: it does what
  Musashi's would for the slice (a reset's cycles eaten first, uncounted; an interrupt due
  taken; then translated code, `native_run`, entered again wherever it stops, until the
  slice's cycles are used, at least one instruction). Between slices the CPU lives in
  `g_ns` (a `CoiCpu` and the PC); Musashi's registers are written back only when it has to
  run (`native_sync`): a slice translated code can't start (the ROM image not translated yet,
  code not translated, a computed jump to a target it doesn't know, Musashi stopped), the
  lockstep check (`COI_NATIVE_CHECK` runs through Musashi's hook as before), and the report and
  the replays at the end of a headless run. `native_sync` sets the SR before the registers, so
  a Musashi that has never been reset (in user mode after `m68k_init`) swaps its stack
  pointers first and doesn't scramble ours.
- **The reset** (start-up, the menu's, `--reset`) is the port's own with `--native`
  (`cpu_reset`), as Musashi's `m68k_pulse_reset` does it: the SSP from vector 0 and the PC
  from vector 1; trace off, supervisor mode, the mask at 7; the condition codes, D0-A6 and the
  USP kept (from user mode, the A7 in use becomes the USP); the reset's cycles (Musashi's
  exception table) taken off the next slice and not counted as used, as `m68k_execute` does.
  Musashi's own leftovers (stopped, its reset cycles) are cleared. Musashi resets only
  with `--no-native`, so strict mode never sees it reset. A slice handed to Musashi gives it the rest of the slice; its hook brings
  translated code back at the next instruction it can run (`g_native_skip` stops the hook
  trying again at the instruction it was just given), and the native loop takes the
  registers back at the next slice. The report counts those slices ("slices handed to
  Musashi"); strict mode fails on each. What reads the CPU between slices goes through
  `cpu_reg` (the frame hashes, `current_pc()`, `current_reg()`), which reads `g_ns` while
  it's the one that counts. The interrupt input is the port's own too (`set_irq`:
  `g_irq_level`, `g_nmi_pending`, read by `coi_native_irq_level`), kept in step with
  Musashi's for when it runs. The `--native` hook path (`coi_native_enter`) stays for those
  slices and for the lockstep check.
- **The other exceptions** are translated code's as well, though the game never takes one:
  `trap #n` (vectors 32-47), `trapv` with V set (7), `chk.w` out of bounds (6), a divide by
  zero (`divu`/`divs`, 5), `illegal` (4), line A (10) and line F (11). Each goes through
  `coi_native_exception` (`Ir::Exception`), as Musashi's `m68ki_exception_trap`/`_trapN`/
  `_illegal`/`_1010`/`_1111`: trace off and supervisor mode (the mask kept), the PC and then the
  SR pushed (the next instruction's PC for trap, trapv, chk and a divide by zero, after the
  operand is read, `(An)+`/`-(An)` included; the instruction's own for illegal and line A/F),
  the handler from the vector (no check for an empty one). Musashi gives the instruction's own
  cycles back, so each costs exactly its `CYC_EXCEPTION` (34 to 40). `chk.w` sets Z from Dn and
  clears V and C, and N only when it traps (as Musashi's undocumented flags); a divide by zero
  changes no flags. Code after `trap`, `illegal` and line A/F is only reached through a
  handler's `rte`, so the translator doesn't follow it (the game has none); it's translated on
  demand when it's reached. Musashi emulates neither address errors nor trace here (T is only
  kept, as a bit of the SR).
- **User mode and the privileged instructions.** `CoiCpu::usp` is the stack pointer not in use
  (the USP in supervisor mode, the SSP in user mode), as Musashi keeps `REG_SP_BASE`. Every SR
  write goes through `rt_set_sr` (`Ir::SetSr`), which swaps the two when S changes: `move to
  sr` (from any operand now, read only in supervisor mode), `andi`/`ori`/`eori` to SR, `rte`,
  `stop`; exceptions and interrupts go into supervisor mode the same way. In user mode the
  privileged instructions (those, `move usp` and `reset`) take the privilege violation (8)
  instead, with the instruction's own PC in the frame, as Musashi's. `rtr` pops the CCR and the
  PC; `reset` (the reset line: nothing in the port listens, Musashi's M68K_EMULATE_RESET is off)
  costs Musashi's `CYC_RESET`, 132 cycles, on top of its own.
- **Translating on demand.** Code translated code reaches without having it (a computed `jsr`
  or `jmp` to an address no list names: `Ir::JumpDyn`; a return or handler into code not in the
  lists; the lists left out altogether) is translated on the spot, from that address, with the
  current translation's settings (`native_on_demand`), and joins it (`jit::merge`): its
  functions and instructions go into the translation's tables wherever they have none, so it's
  instant the next time. The two can resume in each other's instructions because every
  translation's frame is the same size (`jit::kFrameTemps`, 128 temporaries, a 544-byte frame
  on x86-64). The
  report lists them ("translated on demand"); in play there are none. With the lists empty
  (`COI_RECOMP_SWEEP` pointing at an empty `subroutines.txt` and `sites.txt`), the 15-stage run
  translates 326 times on demand, about 1 s in all on this PC, and stays identical.
- **Without an interpreter** (`-DCOI_NO_INTERPRETER=ON`, Android `-PnoInterpreter=true`): Musashi
  isn't built or linked (only its header, for the register names), so translated code is the
  only CPU: always `--native --jit`, translated at once (at start-up and whenever a setting
  changes the ROM: no interpreter to run it meanwhile), the CPU state only ever `g_ns`, the
  cycle tables from `port/recomp/cycles68000.c`. That file is Musashi's tables written out by a
  development build (`coi --write-cycles port/recomp/cycles68000.c`), which checks it against
  Musashi's own at every start-up and won't start if they differ. Whatever translated code
  can't run (code that can't be translated even on demand, the ROM image not translated) stops
  the game with a message saying where and why (`native_fatal`), never silently. The
  test harness (replays, lockstep, the self-test, probes) isn't in it.
- **`stop`** sets the SR (staying in supervisor mode) and stops the CPU (`coi_native_stop`,
  `CoiCpu::stopped`); an interrupt the new mask lets in is taken at once, which ends the stop.
  Either way the slice ends as Musashi ends it: at least the instruction's cycles left, all
  used; fewer, what's left modulo them (`USE_ALL_CYCLES`); then its own counted on top. While
  stopped, the native loop spends each slice waiting (an interrupt at a slice's start ends it),
  as `m68k_execute` does, and Musashi's stopped state follows ours on a sync.
- **A `bra` or `jmp` to itself** (`Ir::Spin`) spends the rest of the slice as Musashi's does
  (`USE_ALL_CYCLES`): with a multiple of its 10 cycles left, the slice ends 10 cycles later than
  looping would. The game never times one so, so only the self-test found it.
- **Interrupts** are translated code's too. The port raises them between slices, and the 68000
  takes one at only two kinds of place: before a slice's first instruction (Musashi's
  `m68k_execute` checks on entry), and right after an SR write that lowers the mask below one
  waiting (`move to sr`, `andi`/`ori`/`eori` to SR, `rte`: Musashi's `m68ki_set_sr`). Both
  now go through `coi_native_interrupt` (`main.cpp`), which does what Musashi's
  `m68ki_exception_interrupt` does on translated code's registers: a non-maskable one taken
  once; acknowledged through `genesis_int_ack` (autovectored, as the port answers); the
  vector read; the PC (all 32 bits) and then the SR pushed; T cleared, S set, the mask set to
  the level; the exception's cycles (Musashi's table, 44 for an autovector) counted. At a
  slice's start the port's line loop (`cpu_execute`) does it before calling `m68k_execute`
  with what's left of the slice, which is exactly the slice Musashi would have run; after an
  SR write, translated code checks (`Ir::IrqLevel`, Musashi's own test) and returns into the
  handler (`Ir::Interrupt`), which runs translated in turn, stopping at its first instruction
  if the budget is spent, as Musashi's loop would end there. Inside a call, the callee
  returning somewhere other than its return address makes the caller stop there, so the chain
  resumes in the handler. Musashi is left with an interrupt only while it's stopped or eating
  a reset's cycles (never in play), or while the interpreter runs everything.
- **The PC's top byte.** Musashi keeps all 32 bits of the PC (a stub called as `$FFB4.w` runs
  at `$FFFFFFB4`, and a call pushes those bits), though the bus sees 24. Translated code stops
  with the PC as Musashi would hold it: a stub's full address, an `rts`'s or `rte`'s whole
  popped address. Code reached at a PC with its top byte set (other than a stub) is left to
  the interpreter.
- **The ROM it was made for.** The generated code carries the size and hash of the ROM image it
  was translated from; whenever the port rebuilds the ROM (the player switching widescreen or
  smooth swinging, Boss Rush, cheats) it checks, and with any other image the interpreter runs
  everything until the image matches again. With `--jit` the app translates on a thread while
  the interpreter runs: the image it starts with (during the boot and the Sega logo), and any
  image a setting changed in the game makes. Each translation's code runs from the first frame
  after it's ready (`native_poll`), and every image translated stays, so switching back is
  immediate. If translating fails, the interpreter runs everything. The harness
  (`--native-test`, `COI_NATIVE_CHECK`) and `COI_JIT_ARCH` translate at once instead.
  `jit::select` picks the translation translated code goes through, between frames.
- **The port's own hooks** that ask where the CPU is (the stage jump at the main loop, the
  `--watch` and VDP traces) call `current_pc()`, which knows translated code's position:
  Musashi's PPC isn't updated while translated code runs.

## Testing

Build with `-DCOI_JIT=ON` (the default; the interpreter stays in, as the reference). Runs with
`--no-native` are the interpreter's. All of these run headless:

- **Replays** (`--native-test N`, which runs the game on the interpreter): each call of a listed function (up to N per function) saves
  the CPU, RAM and VDP as it starts. When the run ends, each call is replayed from that state by
  Musashi alone (interrupts held off) and by the translated code, which the interpreter
  finishes if it stops partway; every memory write in order (VDP port writes included), the
  registers and SR, the cycles used, RAM and VDP memory must match. On the first mismatch it
  prints where the instruction traces of the two runs part. `COI_NATIVE_PAUSE=1` gives each
  call a budget that runs out partway, at a different point each call, as an interrupt would:
  this checks that stopping and resuming changes nothing.
- **The game itself** (`--native --state-hash FILE`): a hash of RAM, VRAM, CRAM, VSRAM and the
  CPU registers after every frame. The same run with `--no-native` must give the same file.
- **Lockstep** (`COI_NATIVE_CHECK=F` with `--native`): from frame F on, each stretch of
  translated code is also run by the interpreter from the same state for the same cycles, and
  the first difference (PC, registers, SR, cycles, RAM, VDP) is reported with the instruction
  traces of both. The controller ports, the Z80's RAM and its bus are saved and compared too;
  the stage jump (`--go-to-stage`) isn't rolled back, so start the check after its frame. Not
  with `COI_NATIVE_STRICT` (the check runs every stretch through Musashi, interrupts included).
- **The self-test** (`--native-selftest`, with any ROM given, which it doesn't use): the
  exceptions the game never takes, each against Musashi, on an image of our own built in
  memory (`native_selftest` in `main.cpp`): vectors; a handler per exception that records the
  frame's SR and PC, A7, its own SR and a count in RAM and returns (past the instruction, for
  illegal and line A/F); and eight routines: traps with four vectors; divides by zero from
  registers, `(An)`, `(An)+`, `-(An)`, an immediate and an absolute word, with a divide that
  doesn't trap among them; `chk.w` below 0, above, within, against a register, an address and a
  negative bound; `trapv` with V clear and set; `illegal`, line A, line F; `stop` with an
  interrupt raised later, then `stop` at mask 7 (stays stopped); `stop` with an interrupt
  already waiting; and all of them mid-block, among other instructions. Then `rtr`, the
  `reset` instruction, `move to sr` from memory and `(An)+`, user mode entered three ways (`move
  to sr`, `andi`, `eori`) with every privileged instruction tried there, a trap and an interrupt
  taken from it; and a `jsr` and `jmp` through registers into code no entry names (translated
  on demand). USP and SSP are compared too. Two tests run what the decoder doesn't take, listed
  as the tracer's functions are: `movep` (all four forms) and `bcc.s`/`bra.s` with displacement
  `$FF` (a branch to an odd address, which Musashi runs on from); each function is left out and
  the interpreter runs it, so those two may hand slices to Musashi, and all else must match. `COI_SELFTEST_DUMP=1` prints what the handlers saw. The image is translated
  with the JIT for the CPU it runs on; each routine runs interpreted (Musashi alone) and then
  translated (the native loop), slice by slice as the game runs, at several slice sizes (488,
  100, 61, 23, 7 and 1 cycles by default; `COI_SELFTEST_SLICES=a,b,...` for others), so the
  budget runs out all around the exceptions. Every slice's cycles, then D0-A7, PC, SR, the
  stopped state and RAM must match, and the translated run must hand nothing to Musashi. Exit
  code 0 when all match. With `COI_NATIVE_STRICT=1`, Musashi taking an interrupt in a translated
  run also stops it.
- **Strict** (`COI_NATIVE_STRICT=1` with `--native`): the run stops (exit code 3) the first
  time the interpreter starts a game instruction translated code could have run, or Musashi
  takes an interrupt (`genesis_int_ack` called from Musashi), printing the frame, the address,
  the opcode and why. Each translation is made at once instead of on a thread, so the boot and
  every image a setting switches to run translated from their first instruction. The one
  handover allowed, counted in the report at the end: a computed `jsr`/`jmp` to a target the
  translation doesn't know (the interpreter runs the jump; its target must then be translated,
  or the run stops there).
- **Probes** (`--native-probe F1,F2,...` with `--native --jit`): translated code tried from every
  instruction it has, not only from where play reaches it. The machine (CPU, RAM, Z80 RAM, the
  bus's own state, VDP, the interrupt input) is saved at the start of each frame listed; from
  each state and each translated instruction, with the state's registers and again with small
  data and address registers into the object RAM, eight scanlines' slices run interpreted and
  then translated (the native loop, as in play), interrupts held off (SR `$27xx`). Every
  slice's cycles and end PC, D0-A7, PC, SR, USP, SSP, the stopped state, every write in order,
  RAM, Z80 RAM and VDP memory and registers must match. Code started out of its context does
  odd things (stray pointers, jumps into data, returns to wherever the stack says) and both
  must do the same odd things: that reaches the branches and states play doesn't. About 80 s
  for 15 states (20,000 instructions, 600,000 runs) on this PC. `COI_PROBE_STEP=N` (every Nth
  instruction), `COI_PROBE_SLICES=N` (8), `COI_PROBE_LIST=FILE` (each address and its result),
  `COI_PROBE_SEEN=FILE` (every instruction the interpreter ran in them), `COI_PROBE_ONLY=ADDR` (one address, with both instruction traces and their
  cycles; `COI_PROBE_DUMP=FILE` writes them in full), `COI_PROBE_SELF=1` (the interpreter
  against itself: the harness's own check), `COI_PROBE_NATIVE_TWICE=1` (translated code against
  itself: a first run that differs from a second points at on-demand translation).
- **The decoder against Musashi** (`--decode-check`): all 65,536 opcodes decoded (zero
  extension words) and set beside Musashi's opcode table: which it runs that the decoder
  rejects, and which the decoder takes that Musashi treats as illegal (`COI_DECODE_TABLE=1`:
  with the table, as the game decodes).
The run used throughout: 15 stages (1, 3, 5, ... 25, 27, 28) with jumps and bounce attacks:

```bash
coi --no-mdplus --cheat invincibility --frames 13530 --go-to-stage 30:1 ... --native-test 2
```

## Status

On Windows (x86-64) unless the row says otherwise; ARM64 was checked on ARM64 Linux in `qemu-aarch64` and on a OnePlus 9:

| Check | Result |
|-------|--------|
| Replays, 15 stages, 2 calls per function (also stopping partway) | 524 of 524 calls match exactly, 273 of 273 functions |
| The game, 15 stages, `--native` against the interpreter (x86-64, ARM64 in qemu, the OnePlus 9) | identical on all 13,530 frames, 100.0% of the 68000's cycles translated, nothing handed to the interpreter |
| Every stage on Normal and Hard, Practice, the ending, Game Over, the options, Boss Rush (92 scenarios), strict; plain, widescreen, MD+ | identical, 100.0% translated, no handovers |
| Settings switched in the game (widescreen, smooth swinging, resets) | identical; each image translated on a thread, and every image one setting away translated ahead |
| The self-test | every exception, `stop`, user mode and translating on demand identical to Musashi at slice sizes down to 1 cycle |
| Probes: translated code tried from every instruction it has, 15 states | 0 differ; 99% of the instructions play never runs are covered |
| Without an interpreter (`COI_NO_INTERPRETER`) | the 15-stage run identical on x86-64 and ARM64, translated at once (about 85 ms on a PC, 300 ms on the OnePlus 9) |
| Time | translating about 85 ms on a PC; the 68000's time about two thirds of the interpreter's on a PC and half on the OnePlus 9 |

Found on the way, all now matching Musashi: `move.l`/`movem.l` to `-(An)` write two words,
low word first; the `dbcc` target (C++'s unspecified evaluation order); calls into a function
the translator had already given up on; a call through a RAM stub still costs the stub's
`jmp` (12 cycles), and that `jmp` can fall in the next scanline; `m68k_set_reg(SR)` doesn't
check interrupts as an instruction writing SR does; the port's stage jump looked for the main
loop through Musashi's PPC. Every number in `config/recomp.txt` is hex, counts included.

## Next

1. **Speed.** A headless run's 68000 time is a few percent of the whole (the VDP's line
   rendering is about 70%). Temporaries in registers across an instruction, calls between
   functions without going through `jit::run` each time, the rarer flag operations inline.
   With `COI_PERF_MAP=1` (Linux) perf names each translated instruction's code
   (`m68k_<address>`, `_checked` for the checked copy).
2. **What the probes don't reach**: `movep` and a `bcc.s` with
   displacement `$FF` (a branch to an odd address) aren't decoded; the game has neither, a
   function with one is left out and the interpreter runs it exactly (the self-test's two
   handover tests); without an interpreter the game would stop with a message there. Interrupts
   while others wait: over the 92 sweep scenarios (4:3 and widescreen, on the interpreter) every
   interrupt is taken under mask 3 and none inside another handler; only stage 8 turns line
   interrupts on (37,056 taken), and only there does one wait while the vertical blank handler
   runs (4 lines) or while the game masks everything (7,460 lines). Stage 8 (Normal, Hard,
   Practice; 4:3 and widescreen) passes the lockstep check from its first frame to the end of
   each scenario: every stretch identical, 9,264 line interrupts each in Normal and Hard.
3. **A cache on disk**, only if translating ever gets slow: the code has the app's own addresses
   in it (the bus, the tables, other functions), so it would have to be saved relocatable.
