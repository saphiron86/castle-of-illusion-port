---
title: Widescreen
parent: Technical notes
nav_order: 4
---

# Widescreen: what assumes a 320-pixel screen

The original renders 320 x 224. A 16:9 picture at 224 lines is about 400 pixels wide. This document records which parts of the game depend on the screen width, from the disassembly, and what the PC port has to change.

Plan assumed below: keep the original view centred and add **40 pixels on each side**, so the visible area runs from camera X - 40 to camera X + 360. The camera itself (the left edge of the original 320-pixel view) does not move.

## Cameras and scrolling

There are two camera structures, `$40` bytes each, at `CameraA` (`$FFFA80`, plane A, the foreground) and `CameraB` (`$FFFAC0`, plane B, the background):

| Offset | Contents |
|--------|----------|
| `$00`  | Y (word) |
| `$04`  | X (word; `CameraA_X`, `CameraB_X`) |
| `$08`  | level layout pointer |
| `$0C`  | level height in 16-pixel blocks (used by `InitCamera`) |
| `$0E`  | layout row width, in blocks |
| `$10`  | 16 x 16 block definitions pointer |
| `$14`  | Y at the last update (row streaming) |
| `$18`  | X at the last update (column streaming, spawning) |
| `$1C`  | Y velocity |
| `$1E`  | X velocity; its sign is the scroll direction |
| `$20`  | minimum Y |
| `$22`  | minimum X |
| `$24`  | maximum Y |
| `$26`  | maximum X |
| `$28`  | VDP command for a pending row |
| `$2C`  | VDP command for a pending column |
| `$3E`  | bit 0: row pending |
| `$3F`  | bit 0: column pending |

`UpdateCamera` (`$00F872`) adds the velocities, clamps the position to the limits, and then calls `StreamColumn` and sets up a row when Y crosses a 16-pixel boundary.

VInt writes the vertical scroll from `CameraA`/`CameraB` Y and the first horizontal scroll entry from their X (negated). The VDP is set to per-line horizontal scrolling (register 11 = `$03`). Several routines derive `CameraB_X` from `CameraA_X` for parallax (for example x 7/8 and x 5/4 at `$00FBAA` and `$00FBBA`); none of that depends on the screen width.

## Plane streaming

The planes are 64 x 32 tiles (512 x 256 pixels), with plane A at VRAM `$C000` and plane B at `$E000` (from the VDP commands in `StreamColumn`; the 32-row column buffers and the `$80` row stride agree).

`StreamColumn` (`$00F982`, `a6` = camera) runs when the camera X crosses a 16-pixel boundary. Moving right, it loads block column `X/16 + 24` (384 pixels right of the camera); moving left, block column `X/16 - 8` (128 pixels left). The tile-map offset `((X/4) + $60) & $7C` wraps within the 512-pixel plane, so both land in the same slot. The column is built in RAM (`$FF1000` for plane A, `$FF1200` for plane B) and written by `FlushPlaneUpdates` (`$00FA9A`) during the next VInt. `FlushPlaneUpdates` writes pending rows the same way, from `$28`/`$3E` and buffers at `$FF1100`/`$FF1300`; the code that builds those rows has not been traced yet.

The loaded window is therefore **camera X - 128 to camera X + 384**. The widened view (X - 40 to X + 360) fits inside it, with 88 pixels of lead on the left and 24 on the right. Allowing for the 16-pixel update step, the right margin is 8 pixels, so:

- **400 pixels (40 per side) works with the original 64-tile planes and streaming unchanged.**
- Anything wider than about 416 pixels (48 per side) needs a wider virtual plane in the port's renderer, with `StreamColumn`'s `+24` / `-8` block offsets, the `$60` column offset and the `$7C` wrap mask changed to match.

Not yet checked: whether every level uses `StreamColumn` (there is a second column setter at `$00EE7C`, and the screens in `src/module.asm` may handle scrolling differently), and the row streamer's vertical margins (unaffected by widescreen, but unverified).

## Objects

`DrawObject` (`$001AB6`) runs for each object: it advances the animation, converts the position to sprite coordinates (`X + $80 - CameraA_X`, the same for Y) and builds the sprites.

- **Deletion range.** If the sprite X or Y is negative or `$280` and above, the object is deleted (`DeleteObject`, `$001C94`), which frees its spawn-list entry so it can spawn again. In screen terms: 128 pixels left or above the view, or 512 pixels right or below its top-left corner. For the widened view this still leaves 88 pixels on the left and 152 on the right, so **no change is needed**. Objects with bit 3 of byte 2 set are already in screen coordinates; bit 4 disables the check.
- **Sprite clipping. Needs a patch.** `ClipSpriteX` (`$001C34`) drops a sprite piece unless `$60 <= X < $1E0`, that is screen X -32 to 351. A piece is up to 32 pixels wide, so on the left one starting from screen X -72 can reach the widened view (which starts at -40), and on the right one starting up to 359 is visible. Widen the window to `$38`-`$200` (screen -72 to 383). (`$40` on the left was not enough: the credits' tree trunks, 32-pixel pieces, popped in 6 pixels deep.) `ClipSpriteY` (`$001C1E`) is vertical and does not change.
- **Sprite X range.** With the `$80` offset the widened view covers sprite X 88 to 487, inside the VDP's 9-bit range (0-511), so sprite coordinates need no format change. The port's renderer must simply draw sprites at X below 128, which the real VDP would hide.
- **Visibility tests. Need a patch.** `IsRectVisibleA` (`$00F32E`) and `IsRectVisibleB` (`$00DA9C`) treat a block rectangle as visible if it overlaps camera X/16 to camera X/16 + 20 blocks (`$14`, 320 pixels). For the widened view the range should start 3 blocks earlier and be 26 blocks wide (rounding 40 pixels up to whole blocks). What these routines gate has not been traced yet; things just outside the original view would currently be treated as off-screen.

- **Stage 15 building tops.** Two buildings in plane B in stage 15 have their tops drawn as sprites by objects `$BE` and `$BF` (`$00FDFC`, `$00FE0C`), which work in screen coordinates. Inactive, they look through a list of building X positions in plane B (`dat_00FED8`, `dat_00FEEA`) for one within 24 pixels left to 344 pixels right of `CameraB_X` (`sub_00FEB6`), and place their sprite there. Active, they move it by `CameraB`'s X velocity and hide again once the sprite leaves screen X -32 to 351. In the widened view the buildings reached the edges without their tops. **Patched:** both ranges are 40 pixels wider on each side (appear from 64 pixels left to 384 right, at `$00FEBC` and `$00FEC6`; hide past screen X -72 or 392, at `$00FE90` and `$00FE96`). Sprite pieces beyond the widened view are still dropped by `ClipSpriteX`. These sprites move with `CameraB_X`, not plane A, and sit over plane B's sky band, which scrolls at half that speed; for smooth scrolling the port recognises them by their tiles (32 from the base in the object's word at `$4`, while an object is active) and gives them their own sub-pixel fraction of `CameraB_X` (with layered hqx, their own layer), so they stay on their buildings.

## Object spawning

Spawn entries are stored in a grid of 128 x 128-pixel cells, `SpawnGrid` (`$FFCA00`): a byte width and height in cells, a list offset per cell, then the entries. `LoadLevelObjects` (`$06D6B4`) loads the grid for the level (decompressing it with `NemDec_ToRAM` when needed), spawns a fixed list of objects, and fills the initial columns from camera X - 113 to X + 512.

During play, `UpdateSpawns` (`$06D52A`, called from the game loop) calls `SpawnColumn` (`$06D5BA`) whenever `CameraA_X` crosses a 128-pixel boundary. `SpawnColumn` takes the grid column containing X + 384 when moving right, or X - 113 when moving left, and spawns five cells of it, from 128 pixels above the camera to 512 below, through `SpawnCell` (`$06D612`). `SpawnCell` skips entries already marked as spawned, finds a free slot in one of three pools (32 slots at `$FFD000`, 10 at `$FFDD80`, 32 at `$FFD400`, chosen by the entry's flags) and stores the entry's address at `$24` of the object, which `DeleteObject` later clears.

The column is taken at the moment of crossing, so moving right, the camera X is a multiple of 128 and the new cells span 384 to 511 pixels right of it: 64 pixels past the original view, but only 24 past the widened one just before the next crossing. Moving left, the crossing is at X = 128k - 1, X - 113 falls in cell k - 1, and the new cells span 0 to 127 pixels left of the camera: just in time for the original view, but 40 pixels late for the widened one. (An earlier version of these notes read this as "113 to 240 pixels left" and concluded no change was needed; play-testing showed otherwise.) Between X = 128k and 128k + 39 the left strip shows part of cell k - 1 before it spawns: in stage 16, the row of falling floor blocks (object `$0D`) was missing its leftmost blocks at the screen edge until the camera passed the cell boundary. Objects are deleted by `DrawObject` once they are more than 128 pixels left of the camera (X + 128 - camera X < 0; `DeleteObject` clears the spawned mark, so the entry can spawn again at a later crossing).

**Patched:** the same scheme runs 40 pixels further out on each side, so objects keep their original timing relative to the visible edges (enemies included: they spawn from the same grid). `UpdateSpawns` tests the crossing on camera X - 40 moving left and X + 40 moving right (`$06D53C` jumps to a routine at `$07FFA0`), so the left column spawns 40 to 167 pixels left of the camera and the right one 424 to 551 right. To match, `DrawObject` deletes objects only once they are more than 168 pixels left or 552 or more right (`$001B12` calls a routine at `$07FF8A`). (Spawning a second column moving right was tried first and dropped: it spawned every object, enemies too, up to a cell earlier than the console does.) Checked headlessly in stage 16: arriving at camera X 282-292 from the right without a vertical cell crossing, the original code lacks the blocks at X 208 and 240 and the patched code has them; the stage 1 autoplay run finishes on the same frame as before.

**Stage 25 keeps the console's timing moving right.** Its skeleton fish (object `$44`) are filed
in a column well right of where they sit (X 352 and 864), so the camera reaching that column
spawns them just off the left edge and they chase Mickey through two swims he only just
escapes on the console. Spawning 40 pixels early, they caught him every time. In stage 25 (index
24 at `$FFFF42`) the patched routine therefore tests the crossing on the original X when moving
right; moving left keeps X - 40. A column moving right still spawns at least 24 pixels beyond
the widened view's edge. Checked headlessly along the stage's route: both fish spawn on the
same frames as with widescreen off.

`$06D550`, called every frame from `UpdateSpawns`, is the vertical counterpart: when the camera Y crosses a 128-pixel boundary it spawns the row containing Y + 384 (moving down) or Y - 113 (moving up), across five cells from the cell left of the camera's to three right of it, which always covers at least 128 pixels left and 385 right of the camera. **Patched:** the row starts from the cell left of camera X - 40's (`$06D586` calls a routine at `$07FED6`) and takes six cells (`$06D59C`), so it reaches both widened edges as the columns do.

Some entries are filed one cell right of where their object sits, so they spawn late even on the console. In stage 26 (level 4, act 2), cell (6, 10) holds a yellow gear (object `$0D`, X 720) and an enemy that waits for the player (`$3F`, X 688), and cell (5, 10) is empty. On the console the gear spawns 336 pixels right of the camera, partly inside the view but mostly off its top; in the widened view it popped in at the right edge as the circling platform brought the camera back up. In widescreen the port moves that list to cell (5, 10) once the level has loaded (`fix_spawn_grid` in `main.cpp`; the grid is compressed in the ROM, so the RAM copy is fixed).

## Camera limits

`LoadLevelHeader` (`$00E896`) picks the level header from `LevelTable` (`$00E8F2`), indexed by the level (`$FFFF44`) and act (`$FFFF46`). A header holds a few setup words, the follow window (two longs), and 16 bytes of camera data each for `CameraA` and `CameraB`. `InitCamera` (`$00EEAC`) copies the camera data (Y, X, layout pointer, height and width in blocks) and sets:

- minimum Y (`$20`) to 0;
- minimum X (`$22`) to 0, or, if bit 31 of the start X is set, to the upper half of the start X (this is how the boss arenas lock the camera);
- maximum Y (`$24`) to the level height in pixels - 224 (`$E0`);
- maximum X (`$26`) to the level width in pixels - 320 (`$140`).

`UpdateCamera` clamps the camera to these every frame, minimum first, then maximum. Nothing else writes the limits: the only other access is a read of minimum X by `CameraFollow`, and `LoadLevelHeader` has a single caller, the level start in `GameState_0C` (just before `LoadLevelObjects`). So the limits, including the boss arena locks, are fixed per level header.

### Levels and arenas

Walking `LevelTable` gives 29 entries (some acts share a header). Every level is at least 512 pixels wide, so none is narrower than the widened view. Four entries set a minimum X, and all are small arenas:

| Level, act | Size (pixels) | Camera X range | With the patch below |
|------------|---------------|----------------|----------------------|
| 0, 5       | 512 x 256     | 160-192        | fixed at 152 |
| 1, 3       | 512 x 272     | fixed at 192   | fixed at 152 |
| 3, 9       | 3840 x 496 (last 384 px) | 3456-3520 | 3456-3480 |
| 5, 0       | 512 x 512     | fixed at 192   | fixed at 152 |

### Patch

So that the widened view (camera X - 40 to + 360) never shows anything outside the level:

- minimum X = max(original minimum, 40);
- maximum X = min(original maximum, level width - 360), that is `$168` instead of `$140` in `InitCamera`.

The 40 only belongs on minimums of 0: a nonzero minimum is already inside the level. In the three 512-pixel arenas the patched minimum ends up above the maximum, and because `UpdateCamera` applies the maximum last, the camera settles at the maximum (152). The widened view then covers 112-512, entirely inside the level, with the arena framed 40 pixels further left than the original. In the arenas the player's left wall does not move: `CameraFollow` keeps the player at least 16 pixels right of the camera minimum, which stays at 160 or 192. The extra area on the left is real level content the player cannot reach.

**The initial plane fill runs before the clamp.** `LoadLevelHeader` ends with `$00EDF0`, which fills both planes with 33 block columns from the camera's start X (blocks X/16 - 8 to X/16 + 24). That happens before `UpdateCamera` first clamps the camera, so with a raised minimum the camera then jumps from the start X (usually 0) to 40. Streaming only runs when bit 4 of X changes, which 0 to 40 does not, and the next stream (at X 48) loads block 27. Blocks 25 and 26 (X 400-432) were therefore never loaded, and the widened view showed stale tiles there as soon as the level scrolled. The port patches the start of `$00EDF0` to clamp both cameras to their limits first (minimum, then maximum, as `UpdateCamera` does; routine at `$07FF36`), so the fill starts where the camera will actually be. Checked by comparing VRAM: right after level start, the widescreen plane holds the same 128 tile entries for X 400-432 as the original game streams there itself.

**Switching widescreen on during a stage moves the camera the same way.** The new limits move the camera onto them in one step: 0 to 40 at a stage's start, or 40 to the left at a stage's right end (stage 23's arena, 3520 to 3480). Streaming loads at most one column for that step, so blocks of the new range were never loaded and showed stale tiles once the view reached them (in stage 1, blocks 25 and 26 at X 400-432, inside the view by camera X 249). The patch above only covers the initial fill, so the port (`refill_planes_if_due` in `port/src/main.cpp`) waits until `CameraA` is inside its new limits (about 18 frames after the level appears, at a stage's start), then writes the blocks of the new range (X/16 - 7 to X/16 + 24) that were not in the old one straight into VRAM. It writes them as `StreamColumn` (`$00F9B4`) does, including the upside-down plane A of `$FFFF9801` (`$00FA52`), and leaves plane B alone where it holds a fixed picture (bit 0 of `$FFFF9800`). Checked headlessly: for all 28 stages, started at 320 pixels and switched at frame 1200, planes A and B in the view match a run started in widescreen. Every column the game streams matches what the refill would write, upside down too. Runs that never switch are unchanged frame for frame, and RAM after a switch matches the unfixed build, so gameplay is the same. `$22` scenes and the ending are never refilled, since they are not level play (`GameState` `$0E`).

**The camera minimum is also the player's left wall.** `CameraFollow` (`$00F71E`) clamps the player to camera minimum X + 16, so raising a minimum of 0 to 40 on its own would move the player's wall from 16 to 56. Play-testing found this blocks progress at the start of world 2, where the player has to jump onto a step at the far left. The port therefore also patches `CameraFollow` to compute the player's wall from the original minimum: a minimum of exactly 40 can only come from the patch (the arenas use 160, 192 and 3456), so it is treated as 0.

### Follow window

`CameraFollow` (`$00F71A`) first clamps the player to the level (left edge camera minimum + 16, right edge width - 16, top 8, bottom height - 16). It then compares the player's screen position with `FollowWindow` (`$FFFF9820`, from the level header: right, left, bottom and top thresholds, for example 192/128/144/56) and sets `CameraA`'s velocity to the overshoot. The thresholds are relative to the original 320-pixel view; since the widened view keeps that view centred, **no change is needed**.

## Screens outside level play

Everything above applies once a level has been set up through `LoadLevelHeader` (`GameState_0C`). Other screens behave differently:

- **Static screens.** States `$00`, `$02`, `$04`, `$08`, `$14`, `$1C`, `$1E`, `$28` and `$2C` never reach the camera, streaming or object code (each reaches only a few hundred instructions). Their tile maps are drawn for the original 40 columns; what the 64-column planes hold in the extra 5 columns on each side is unknown.
- **Screens in `src/module.asm`.** States `$18`-`$26` jump into `module.asm`. Most read the controller and request sounds, like menus. State `$20` positions both cameras directly (around `$07ABE4`), and object code from `$07B332` onwards (dispatched through a table at `$07B21A`) moves the cameras along fixed paths, for example `CameraA_X` up to `$C0` with `CameraB` at half speed. These scripted moves bypass the level camera limits, so a widened view could show tile columns that were never drawn.
- **Static analysis cannot sort the rest.** The other fifteen states, including several of the `module.asm` ones, can all reach the camera and object code through shared routines, so reachability does not separate gameplay from object-driven scenes.

**Recommendation for the port:** show these screens at 320 pixels with borders (pillarbox) by default, and use the widened view only after `LoadLevelHeader` has set up a level. Individual scripted scenes can then be switched to widescreen once they have been checked on screen in the running port.

## Status in the port

Stage numbers are the 1-28 numbers used by `--stage` (see [Stages](stages.md)).

Implemented in `port/src/patches.cpp` and `port/src/main.cpp`:

- The `ClipSpriteX` window, the `InitCamera` limits (minimum X 40 through a small routine at `$07FF00`, maximum X width - 360), the player's left wall in `CameraFollow` (kept on the original minimum through a routine at `$07FF24`), the initial plane fill (cameras clamped first through a routine at `$07FF36`) and the `IsRectVisibleA`/`B` ranges (start 3 blocks earlier through routines at `$07FF10`/`$07FF1A`, 26 blocks wide) are patched in the in-memory ROM. Each patch checks the original bytes first, and the header checksum is recomputed because the boot code verifies it.
- The renderer draws 400 pixels (40 per side) while `GameState` is `$0E` (level play); every other state is shown at 320 pixels with black borders. `--no-widescreen` turns all of this off.

- **HUD.** The HUD is the window plane covering lines 192-223 (register 18 = `$98`, tile map at VRAM `$B000`): a chain border on tile rows 24 and 27, content on rows 25-26, all with the priority bit set. The renderer only has the window's 40 original columns, so outside them it extends any whole-line window: a row that repeats with a period of up to 8 tiles (the border, period 2) keeps repeating, and any other row repeats its edge tile (blank in the HUD), so the HUD reaches the screen edges with black between the borders.
- **Sprite masking.** The game hides sprites behind the HUD with the VDP's masking rule: sprite #0 at raw X 64 and sprite #1 at raw X 0 both cover lines 192-223, and an X-0 sprite hides every later sprite on its lines once a sprite with nonzero X has appeared there. The renderer now implements this (it was missing, so high-priority sprites such as the ghosts in world 3 drew over the HUD). Checked by drawing a high-priority sprite on the HUD lines with masking on and off: all 165 differing pixels are on HUD lines.

Checked headlessly: the game boots with the patches, the camera starts at X 40, and the side strips during level play contain level graphics rather than black or backdrop. After about ten seconds of holding right the camera is 10 pixels behind the unpatched run, probably because objects inside the wider visibility range start acting earlier; this needs play-testing.

- **Hallway between worlds.** Play with `--show-state` (which shows the `GameState` in the window title) identified the castle hallway as state `$22`, set up by `$20`; `$06` is the intro after boot and `$2A` the scene before the hallway. The hallway is drawn at 400 pixels too. Its camera is scripted: it moves from X -192 to 4 and stops, with `CameraB` at about half speed, while the 512-pixel planes are drawn once and never streamed. The original view already covers 516 pixels of plane A over the walk, crossing the planes' wrap point, so a 400-pixel view stays one continuous slice of the same loop; at the ends of the walk the extra strips show the adjacent part of the loop slightly earlier than the original would. There is no HUD (the window starts at line 224). Checked headlessly at camera X -192, -54 and 4: the side strips show scene content, and the pixel changes where they meet the original view are within the scene's normal range. (`$06` stays pillarboxed; `$16`, once widened on a wrong guess, turned out to be the scenes after the ending, and `$2A` the story before the hallway, both now widened as described below.)

- **Second play-test corrections.** `GameState` `$22` is not only the hallway: `$20` picks the scene by level (`$FFFF44`, tables at `$07AB68`, or `$07AB74` when `$FFFF0D` is 2). Levels 0-2 are hallway walks (the camera moves one way: -192 to 4, -6 to 148, 154 to 310); levels 3-5 show a scene that pans back and forth and is laid out for the original view, so widening it showed the wrong part of the plane beside a tower. Only levels 0-2 are widened now. The 512-pixel boss arenas (stages 5, 9 and 28) were framed 40 pixels left of the console's because maximum X = width - 360 fell below their locked minimum; `InitCamera` now keeps the original maximum in that case for the arenas locked at 192 (stages 9 and 28; routine at `$07FF6E`), so they are centred as on the console, while stage 5 (whose original camera moves between 160 and 192, and which would otherwise show 40 pixels past its right edge at 192) stays fixed at 152, entirely inside the arena, and the 40 pixels past the arena's right edge show what the plane holds there (it wraps round to the arena's left end). Headlessly, stage 5 shows no seam there, but stages 9 and 28 change sharply at exactly that column, so a visible seam is likely.

- **Per-scene strip fills.** Where a scene's plane holds the wrong thing past the original view, the renderer can remap plane A for pixels in the widened strips (`Vdp::extend_plane_a`, rules in `port/src/main.cpp`). `$22` is now widened for every level, and two rules exist:
  - *Castle exterior before the final boss* (`$22` for level 5, or level 3 when `$FFFF0D` is 2): the brick tower ends at plane A X 512, after which the plane starts over with the pale castle wall. In widescreen the tower is made 40 pixels wider with its own tiles: after its column 61 (plane X 488-495), 40 more pixels of brick, then its dark right edge (columns 62-63, X 496-511) moved to X 536-551, the end of the widened view at the scene's last camera X (192). The bricks repeat every 16 pixels (columns 60-61) and alternate by row, so rows 3-5 take them from rows 3-4 and rows 7 and below from rows 8-9; the last merlon's cap (row 2) and the cornice (row 6) repeat column 62, and rows 0-1 are sky. The door and everything left of column 62 are unchanged. Since the moved edge lies inside the original columns once the camera passes X 176, this rule also sees pixels inside them (`Vdp::extend_plane_a_inside`, widescreen only), and it unwraps plane X against the camera to tell the tower's end (X 496-551) from the pale castle's start (X 0-39), which share the same plane columns. (Repeating the tower's darkest column instead looked like a flat slab beside it.) At the start of the scene (camera X 0-39) the left strip would show the tower's end too, wrapped round to plane X 472-511, beside the pale castle's left edge at X 0; there the castle continues with its own columns instead: its rows repeat every 64 pixels, the panel rows around the door (plane rows 12-24) every 112, so the strip shows X 24-63 or 72-111 and joins column 0 without a seam. That fill is drawn in front of the sprites (`Vdp::strip_fill_in_front_left`), so Mickey does not show at the left edge before he comes out of the door. The rules work in plane coordinates, so they follow the scene as it pans. For level 5, `$22` first shows a hallway walk on the same state before loading the exterior, so the rules apply only while plane A holds the exterior's tower and castle tiles (`exterior_in_plane_a`); otherwise the widened tower's remap inside the view garbled a column of the hallway.
  - *Locked boss arenas* (centred like the console, with the player's reach ending at the strips): both strips become walls of the arena's own floor blocks, aligned to the floor's grid in plane coordinates so they line up with the floor and scroll with it vertically. Stage 9 (level 1, act 3) uses its 32 x 32 blocks (plane A X 192-223, Y 224-255, tiles `$101`-`$110`); stage 28, the final boss (level 5, act 0), uses its 64 x 32 floor slabs (plane A X 192-255, Y 464-495, tiles `$175`-`$189`). The final boss's hit flash briefly uses plane A rows Y 448-511 (off screen) as scratch space, so the walls are drawn from a copy of the block's name table entries, refreshed only while the block holds its own tiles (`update_wall_cells`, `Vdp::strip_cells`).
  - *Ending* (`GameState` `$2E`, played in the final arena with the same plane A scroll): widened with the final arena's walls, drawn in front of the sprites (`Vdp::strip_fill_in_front`). The witch bumps into the right wall where she bumps the original screen edge, and the characters run out behind the left wall instead of through it. Checked headlessly across the scene: every strip pixel above the HUD comes from the wall.

- **Scenes after the ending** (`GameState` `$16`, from the end of `$2E` until the game returns to the Sega logo): the witch flying Mickey and Minnie past the castle at night, the cliff where they land, and the forest walk under the credits are pictures drawn across planes 128 cells wide (register 16 = `$03`) and scrolled through the line scroll table (both cameras stay at 0 for the whole state), so the strips show more of the same picture, with no seams. Widened. "The End" (on black, with other graphics past the original columns) stays pillarboxed while its objects (`$80`, `$81`) are loaded (`after_ending_scene` in `main.cpp`); the switches fall on black frames (checked frame by frame headlessly: the whole 400-pixel picture is black at each one).
  - *Credits tree trunks.* The trunks in the forest are sprites (object `$2D`, tiles `$4372`), created every 250 frames by a spawner (`$008AA4`, frames 5243-11842 of the ending) at X 0, just off the original left edge, moving right a pixel a frame, and deleted after 380 frames, just off the right edge. In the widened view both ends fell inside the strips, so trunks popped in and out. **Patched** (`$008AFE` jumps to a routine at `$07FE6C`): each trunk lives 400 frames, past the right strip, and each firing also creates the next trunk at once, at X -250 with 650 frames to live, so it follows exactly the path the console's next trunk will (X 0 when that one is created) and slides in at the left edge. The first firing (on a black frame) keeps its own trunk too; a later firing reuses its slot for the next trunk, or frees it when the spawner will not fire again (its remaining life, `$22`, 250 or less), so no trunk appears that the console would not show. Checked headlessly: the original 320 columns are pixel-identical to the unpatched run on every frame compared, and trunks slide in and out of the strips. RAM differs only in the trunk objects during the credits, and in 11 stale bytes of unlinked sprite table entries (`$FFF52F`-`$F55D`) after them, gone by the end of the state.
  - *Night flight's small sprite.* Object `$6F` (the small witch with Mickey and Minnie) flies right 4 pixels a frame, then waits 100 frames at X 348, just off the original right edge, before becoming the big sprite at X 320; in the widened view it sat at the edge. **Patched** (`$007DA8` calls a routine at `$07FEC4`): while waiting it moves on 4 pixels a frame (X only, so its Y and fractions stay the console's), and leaves the view. The next step sets X afresh: RAM and VRAM are identical to the unpatched run from frame 2400 on. Remaining: the big sprite (bubble with the witch, object `$71`, created at the same moment) appears fully drawn at X 355-395, inside the right strip, where the console's edge hides it for about 10 frames. Making it enter earlier would shift the witch's own script by about 12 frames, so it is left as it is.
  - *Theatre* (the last scene: `$B8`, `$BB`, `$BC`, `$BD`). Plane B holds the curtain in rows 0-15 of columns 0-39 and lowers it with its vertical scroll (112 to 0); plane A holds the cinema screen and its frame (transparent outside it) in rows 0-15 and the stage edge, footlights and seats in rows 16-31. Past column 39 plane A is empty and plane B holds other graphics. **Widened** with a strip fill (`StripRule::TheatreFill`, plane B through the new `Vdp::extend_plane_b`): the curtain repeats every 12 columns, so the strips take the columns 12 in from each edge; the fringe (rows 3 and 4) ends with an end piece of its own at the right (columns 35-39), so there the fringe keeps repeating over columns 35-39 and on to the edge of the widened view, which cuts it as the left edge does (`Vdp::extend_plane_b_inside`). Moving the end piece out to the edge instead put it under the top of a different swag. The seats repeat every 4 columns and the footlights every 8 (columns 2, 10, 28 and 36), so plane A's strips take the columns 8 in: a footlight is cut by each screen edge. The curtain comes down across the full width with its own scroll. Checked headlessly across the scene and its setup (the strips stay black until the picture appears).
- **Story before the hallway** (`GameState` `$2A`; planes 128 cells wide, no scrolling). Plane A (all high priority, transparent between the trees) holds the canopy, the two trees and the ground in rows 0-17 of columns 0-39, then the text box; plane B holds the brick wall and the arch (columns 28-39), whose right pillar (columns 38-39, rows 8-15) has the priority bit: the toy soldiers (object `$6B`, created at X 299, 323 and 347 and walking left) come out from behind it, so they appear to come out of the door. **Widened** with a strip fill (`StripRule::StoryFill`): plane B's bricks repeat every 2 columns (the strips take columns 10 in on the left and 30 in on the right; row 2, offset by a column above the arch, 29 in); plane A's trees repeat every 26 columns, so the left strip takes the columns 26 in (the top two canopy rows 18 in), and on the right, where the picture ends with pieces of its own (columns 34-39), each row takes the columns that best follow them; below row 17 (the text box) the strips are empty. The right strip's bricks take the high priority (`Vdp::strip_b_priority_right`), so the soldiers stay hidden behind the new wall until they reach the pillar and come out of the door as on the console. Mickey is created at the left edge on the first frame of the fade-in, so he appears with the picture. The switches from and to the pillarboxed states around it fall on frames whose strips are black.
- **Scripted scenes from `$07B332`.** The object table at `$07B21A` is the director of `$22`'s scenes (its byte `$29` picks the starting step through `dat_07B4AE`: one per hallway walk, then the castle exterior, which starts at `$07B332`, and the end of a Practice game, steps `$07B444`-`$07B2BE`, which shows the rainbow and a text box, then returns to the Sega logo). All of them run in `$22`, which is widened already; on screen, the Practice ending's strips show the castle scene and the text box's border runs to the edges.
- **Checked and left pillarboxed** (forced to 400 pixels for comparison): the intro after boot (`$06`: its story pictures are small framed windows on black, the plane past them holds other parts of the picture; the title screen's cloud band has a seam 12 pixels past the right edge and nothing on the left that belongs to it), the gem and score screens after a boss (`$24`, `$26`: framed text on black, identical either way), and the static screens (Sega logo, title menus, options, copyright, Game Over and continue: text and logos on black, identical either way).

Not done yet: the 20-sprites-per-line limit is unchanged, so busy lines may lose sprites in the extra area.

## Summary for the port

| Item | Routine | 400-px change |
|------|---------|---------------|
| Plane size and streaming | `StreamColumn`, `FlushPlaneUpdates` | none (up to about 416 px) |
| Object deletion range | `DrawObject` | limits 40 pixels further out on each side (`$001B12`), to match the spawning change |
| Sprite X clip | `ClipSpriteX` | widen `$60`-`$1E0` |
| Sprite coordinates | `DrawObject` | none; renderer draws X < 128 |
| Block-rect visibility | `IsRectVisibleA`, `IsRectVisibleB` | start 3 blocks earlier, 26 blocks wide |
| Stage 15 building tops | objects `$BE`, `$BF` (`$00FDFC`, `$00FE0C`) | appear and hide 40 pixels further out on each side |
| Object spawning | `UpdateSpawns`, `SpawnColumn` | 40 pixels further out on each side, except moving right in stage 25 (`$06D53C`); rows from X - 40, six cells (`$06D586`, `$06D59C`); stage 26's misfiled gear moved to its own cell (`main.cpp`) |
| Camera limits | `InitCamera` | minimum X = max(original, 40), maximum X = width - 360, or the original maximum where that falls below the minimum (the 512-pixel arenas) |
| Player's left wall | `CameraFollow` | keep it on the original minimum + 16 |
| Initial plane fill | `$00EDF0` | clamp the cameras before filling |
| Boss arenas | `InitCamera` (start X bit 31) | covered by the same patch; three arenas framed 40 px further left |
| Camera follow window | `CameraFollow` | none |
| Static screens (9 game states) | none of the level code | pillarboxed (checked: text and logos on black) |
| Scripted scenes (`src/module.asm`, `$07B332` on) | `$22`'s scene director | widened with `$22` (castle exterior, Practice ending) |
| Scenes after the ending | `GameState` `$16` | widened; the credits' tree trunks start and end outside the strips, and the night flight's small sprite flies on while it waits (`$008AFE`, `$007DA8`); the theatre repeats its curtain, footlights and seats into the strips; "The End" pillarboxed while its objects are loaded |
| Gem and score screens | `GameState` `$24`, `$26` | pillarboxed (framed text on black) |
| Cutscenes between worlds | `GameState` `$22` | widened; the castle exterior widens the tower with its own bricks and continues the pale castle past its left |
| Stage 9 and final boss (28) arenas | level 1 act 3, level 5 act 0 | widened, centred, with walls of floor blocks in the strips |
| Stage 5 arena | level 0, act 5 | widened, camera fixed at 152 (inside the arena) |
| Ending | `GameState` `$2E` | widened, final arena's walls drawn in front of the sprites |
| Intro after boot | `GameState` `$06` | pillarboxed (framed pictures; checked) |
| Story before the hallway | `GameState` `$2A` | widened, repeating its wall, trees and ground into the strips; the soldiers stay behind the right strip's bricks |
