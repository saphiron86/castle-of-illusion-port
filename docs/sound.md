# Sound: how the game talks to the Z80

The 68000 does not play music or sound effects itself. At boot it loads a driver into Z80 RAM, and afterwards it only posts small requests into a mailbox there. The PC port can therefore emulate the Z80, the YM2612 and the SN76489, and route the 68000's mailbox writes into the emulated Z80 RAM.

Both sides are disassembled: the 68000 code in `src/`, and the Z80 driver in `src/z80driver.asm` (names in `config/z80.txt`). Statements below come from that code; anything not confirmed is marked as such.

## Loading the driver

`LoadSoundDriver` (`$079EE0`, called once from `GameInit`):

1. Holds the Z80 in reset (`Z80_Reset`) and requests its bus (`Z80_BusReq`), waiting until the request is granted.
2. Copies four blocks from ROM into Z80 RAM:

   | ROM label      | ROM address | Z80 address | Size   | Contents |
   |----------------|-------------|-------------|--------|----------|
   | `Z80Driver`    | `$0784B0`   | `$0000`     | `$EF0` | Driver code and tables (`src/z80driver.asm`) |
   | `Z80Data_0F00` | `$0793A0`   | `$0F00`     | `$880` | Not yet examined (probably music and instrument data) |
   | `Z80Data_1900` | `$079C20`   | `$1900`     | `$2C0` | Not yet examined |
   | `Z80Data_1C00` | `$079F76`   | `$1C00`     | `$00F` | Initial mailbox and driver state |

3. Pulses the reset line and releases the bus, so the Z80 starts running at `$0000`.

## How the driver keeps time

The driver is paced by the **YM2612's timers**, not by the vertical blank interrupt:

- `z80_Init` (`$0053`) sets `Drv_StartDelay` (`$1FFF`) to 3 and waits, with interrupts enabled, until `z80_VBlankIRQ` has counted it down. So the Z80 interrupt must fire once per frame from the start.
- `z80_MainLoop` (`$006F`) then polls the YM2612 status register (`$4000`) forever:
  - **timer B** overflow: check the pause state (`z80_CheckPause`), handle new requests (`z80_ProcessRequests`), then run `$0100`;
  - **timer A** overflow: per-tick work at `$00AD`, then `Drv_TickCounter` (`$1C16`) is counted down; when it reaches zero, the full update `z80_UpdateFrame` (`$00D9`) runs and the counter is reloaded from `Drv_TickPeriod` (`$1C17`). `z80_UpdateFrame` also calls `z80_CheckPause` and `z80_ProcessRequests`.
- `z80_VBlankIRQ` (`$0038`) would also run `z80_UpdateFrame`, but only while `Drv_IrqUpdateOff` (`$1C07`) is 0. The mailbox seed sets it to `$80` and nothing in the traced Z80 or 68000 code clears it, so that path appears unused.

**Port requirement:** the YM2612 emulation must implement timers A and B and their status flags faithfully, or the music will run at the wrong tempo or not at all.

## The mailbox at Z80 `$1C00`

| Z80 address | 68000 name         | Z80 name            | Written by | Meaning |
|-------------|--------------------|---------------------|------------|---------|
| `$1C0C`     | `Z80_MusicRequest` | `Mbox_MusicRequest` | `PlaySound`; data command `$F4` | Music request, IDs `$81`-`$9F` |
| `$1C0A`     | `Z80_SfxRequest`   | `Mbox_SfxRequest`   | `PlaySound`, `UpdateSoundPause` | Sound effects and special commands, IDs `$A0`-`$D3` |
| `$1C13`     | `Z80_Paused`       | `Mbox_Paused`       | `UpdateSoundPause` | 1 while the game is paused |
| `$1C12`     | `Z80_PauseState`   | `Drv_PauseState`    | `UpdateSoundPause`; the driver | Driver pause state (below) |
| `$1C08`     | `Z80_LoopRelease`  | `Mbox_LoopRelease`  | `SetSoundLoopRelease`, code at `$00D920`; the driver clears it | Lets a looping sound finish |

Each 68000 write requests the bus, writes one byte and releases the bus. `PlaySound`, `SetSoundLoopRelease` and the code at `$00D920` also wait for the bus grant and mask interrupts around the access; `UpdateSoundPause` does neither, and waits a few `nop`s after the request instead. None of the traced 68000 code reads the mailbox.

### Music

The game stores an ID in `SoundRequest` (`$FFF40E`) and calls `PlaySound` (`$079F86`), directly (100 call sites) or through the RAM stub `PlaySound_RAM` (`$FFFF8A`, 20 uses). IDs below `$A0` go to `$1C0C`.

On timer B ticks and in `z80_UpdateFrame`, `z80_ProcessRequests` checks `$1C0C`. A nonzero value goes to `z80_StartMusic` (`$0532`), which subtracts `$81` (ignoring anything lower), resets playback (`$07D5`), clears the mailbox and starts the song from a pointer table. Sound data command `$F4` writes `$1C0C` itself, so a song can queue the next one.

### Sound effects and special commands

IDs `$A0` and up go to `$1C0A`. `z80_PickSound` (`$086D`) moves a request from `$1C0A` or the second slot `$1C0B` into `Drv_PendingSound` (`$1C09`), and `z80_ProcessRequests` dispatches it:

| ID range    | Handler | Notes |
|-------------|---------|-------|
| below `$80` | none    | ignored |
| `$81`-`$9F` | `$0532` | same song start as `$1C0C` |
| `$A0`-`$C6` | `$05F0` | sound effects |
| `$C7`-`$CF` | `$05E7` | a second sound effect handler, not yet examined |
| `$D0`-`$D3` | `z80_SpecialTable` (`$0509`) | special commands; the game uses `$D0` and `$D1` |
| `$D4`-`$D8` | none    | pass the range check but index past the 4-entry table; never used |
| `$D9` and up | none   | ignored |

The game uses IDs `$81`-`$9E` and `$A0`-`$D1`.

### Pausing

`PauseFlag` (`$FFF41D`) bit 0 is toggled when bit 7 of `$FFF309` is set, probably by the Start button. `UpdateSoundPause` (`$009770`) then:

- on pause: requests sound `$C1` through `$1C0A` and sets `$1C13` to 1;
- on unpause: writes `$80` to `$1C12` and sets `$1C13` to 0.

`Drv_PauseState` (`$1C12`) is handled by `z80_CheckPause`, on timer B ticks and in `z80_UpdateFrame`:

| Value | Meaning |
|-------|---------|
| 0     | running |
| 1     | pause requested: run `$082B` once, then set 2 |
| 2     | paused: skip the rest of the update |
| `$80` | resume: set 0 and restore the six channels at `$1C70` (`z80_Resume`) |

The 68000 never writes 1. Sound data command `$FC` (`z80_Cmd_FC`) does: if `$1C13` is clear it writes `$80` (resume); if it is set, it writes 1 (pause), except that operand 5 resumes instead and operand 4 takes a separate path at `$0A2B`. Most likely the pause sound `$C1` contains `$FC`, so the music stops when the pause sound plays; this has not been checked against the sound data.

### Looping sounds

Sound data command `$FD` (`z80_Cmd_FD`) is a conditional jump: unless `$1C08` is set, it jumps back to the address in its operand, like the unconditional jump `$F6`. When `$1C08` is set, it clears it and continues past the operand.

The 68000 uses this through `SetSoundLoopRelease` (`$002412`): it writes 0 just before requesting sound `$B3`, so the sound keeps looping, and 1 when an object flag is set, which lets it play out. The code at `$00D920` also writes 1.

## Other Z80 bus requests

Most `Z80_BusReq` writes in the game (over 70) are not mailbox traffic. VInt and the VDP helpers request the bus around DMA transfers and release it afterwards, which keeps the Z80 off the bus during DMA on the real console. In the port these requests only need to be accepted; the emulated Z80 can keep running.

## What the port needs

- A Z80 core with 8 KB of RAM, loaded as above when the game calls `LoadSoundDriver`.
- YM2612 emulation **with timers A and B and their status flags**, and SN76489 emulation, at Z80 addresses `$4000`-`$4003` and `$7F11`.
- The Z80 interrupt raised once per frame (needed at least for the start-up delay).
- 68000 writes to `$A00000`-`$A01FFF` going to the emulated Z80 RAM. `Z80_BusReq` bit 0 should read 0 once the bus is granted, and `Z80_Reset` should hold and release the Z80.
- About 59,700 Z80 cycles per frame (3.58 MHz / 60); the driver's tempo comes from the YM2612 timers, so audio stays correct as long as the YM2612 is clocked consistently with the Z80.
- The driver also writes the 68000 bank register (`$6000`) to read data from 68000 ROM. The traced code does this; which data it reads has not been examined yet.

## Open questions

- What the `$C7`-`$CF` handler (`$05E7`) and the special commands `$D0`-`$D3` do.
- What the Z80 reads through the bank register, and what `Z80Data_0F00` and `Z80Data_1900` contain.
- The remaining 15% of `Z80Driver` that the tracer has not reached as code.
