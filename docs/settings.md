---
title: Settings
nav_order: 3
---

# Settings
{: .no_toc }

F1, or the Back (View) button on a controller, shows or hides a menu bar over the game, which keeps running underneath. Use it with the mouse, with the arrow keys and Enter once it has focus (the game doesn't get those keys while it does), or with a controller: the D-pad or left stick moves, A selects, and B goes back, closing the menu from the top level. While the menu is open, controllers drive only the menu; after it closes, they reach the game again once all their buttons are released. On Android, tap the menu button or press Back.

1. TOC
{:toc}

## Game

**Reset** (Ctrl+R)
: Restarts the game as if the console were switched off and on, dropping any stage chosen with `--stage` or the Debug menu.

**Exit to desktop** (Alt+F4)
: Closes the port.

## Quick Settings

**Enhanced** (F2)
: Widescreen and every graphics enhancement at its default: Smooth Swinging, Smooth Scrolling, Clean Rock Edges, Smooth Fades, Smooth Water, Smooth Glass and Remove Sprite Limit on; Upscaler, Scanlines and the CRT shader off. This is the setting on first launch.

**Vanilla** (F3)
: The original game: 4:3, with all of those enhancements, the Upscaler, Scanlines and the CRT shader off.

**Custom** (F4)
: Your own choices for those settings, as they were before you last picked Enhanced or Vanilla. Changing any of them yourself switches to Custom. Quick Settings leaves the other Video settings, Gameplay, Audio and the cheats alone.

## Video

**Widescreen**
: Switches between 400 and 320 pixels wide, immediately, even mid-stage. Switching during a stage also updates the camera limits, so the stage plays as if it had started in that mode; the newly shown strips at the sides can show leftover tiles until the view has scrolled across them. `--no-widescreen` starts at the original 320 x 224. How it works: [Widescreen](widescreen.md).

**Fullscreen** (F11 or Alt+Enter)
: Borderless fullscreen at the desktop resolution. Linux starts fullscreen, Windows in a window.

**V-Sync**
: On by default. Each picture is shown in step with the display's refresh, so it never tears. On a display at about 60 Hz the game runs one frame per refresh (the sound is resampled by the tiny speed difference); at other rates game frames are timed as on the console and each refresh shows the latest one. If the driver ignores the request, pictures are still capped near the refresh rate.

**Frame Rate**
: Display (default): pictures follow the display's refresh; at a whole multiple of 60 Hz each game frame is shown for exactly that many refreshes, counted by refresh, not by the clock. 60, 120 or 240: the port times the pictures itself at that multiple of the console's 59.92 a second, whatever the display reports, for variable-refresh (G-Sync, FreeSync) displays; the game runs at exactly the console's speed. Above a display's maximum refresh rate not every picture can be shown; pictures that can't be drawn in time (240 with Smooth Scrolling is a lot of work) are skipped, so the game keeps its speed. `--frame-rate N` for one run.

**High Refresh Interpolation**
: At 120 or 240 pictures a second (Frame Rate, or a display at 120, 180 or 240 Hz with V-Sync), each game frame is shown as evenly spaced moments between the last two game frames instead of repeats: each line's scroll and each sprite (moved with the object that drew it) are placed part of the way along their movement, and with Smooth Scrolling the planes follow its fitted movement. The picture is up to one game frame (17 ms) behind. Off by default; `--interpolate` turns it on for one run.

**Show FPS**
: A counter in the top right: pictures shown per second, and game frames per second when they differ. `--show-fps` shows it for one run.

## Enhancements

### Gameplay

**Easy Bounce Attack**
: Every jump becomes the bounce attack without pressing Down: the port taps Down for one frame after take-off, as a player would. Holding the jump button still decides the height. Off by default; `--easy-bounce` turns it on for one run.

### Audio

**MD+ Soundtrack**
: Plays an MD+ soundtrack pack instead of the game's music; available when a pack is installed, and on by default when one is. Switching it restarts the game. See [MD+ soundtrack packs](mdplus.md). On Android, **Import MD+ Pack...** is here too.

### Graphics

**Smooth Swinging**
: On by default. Vines and ropes swing at 60 frames a second instead of the original 10, with Mickey and the camera moving every frame. The game only has 9 rope poses, stepped every 6 frames; the port builds the poses in between (each rope link moved part of the way) and patches the game to use them, so the swing still takes as long and Mickey's own drawings change when they always did. `--smooth-swing` turns it on for one run.

**Smooth Scrolling**
: On by default. The game scrolls in whole pixels, so at in-between speeds the picture moves unevenly (the foreground 1, 2, 1, 2 pixels a frame while running, the background 1, 1, 1, 0). This draws each line at 4x horizontal resolution and moves each layer by fractions of a pixel to where its recent movement says it should be, so scrolling moves in even steps. Corrections are under a pixel, so starts and stops look as before. Horizontal scrolling only. `--smooth-scroll` and `--no-smooth-scroll` for one run.

**Clean Rock Edges**
: On by default. In the forest stages the rock walls' edge tiles use black for the gaps between stones, so where a rock ends (against the background or the bushes in front) those gaps show as black notches in the original. With this on, black connected to the outer side of a rock edge tile (or within 3 pixels of transparency) takes the colour of what lies beyond it: the background, a bush, or the dark filler beside the rock. `--vanilla-rock-edges` turns it off for one run.

**Smooth Fades**
: On by default. The game fades between screens by stepping each colour one of the console's eight levels at a time, every few frames, so fades show as a handful of jumps. With this on, the colours ease from one step to the next over the time the game takes between steps (so a fade finishes a few frames later than in the original); other palette changes, such as palette cycling and flashes, show at once. `--vanilla-fades` turns it off for one run.

**Smooth Water**
: On by default. The game draws some of its water as a dithered pattern in front of everything but the HUD: a checkerboard of one colour in the tea (stages 20-22), the flooded castle (stages 24-25), the jello pits in Toyland (stages 6-9) and the foam where the cave falls land (stage 10), and streaks with gaps in the waterfalls (stages 11-13). With this on, that water is see-through instead: each of its pixels takes the colour and coverage of the water around it (3 by 3 pixels), laid over what lies beneath with that coverage as its opacity, so a checkerboard becomes an even 50% tint, the foam fades out smoothly and the waterfalls become a see-through sheet. Mickey and the enemies show through it, dimmed, as the dithering suggests.
: The water also moves more smoothly. The big waterfalls of stages 11-13 flow by palette cycling, their colours moving one place every 3 frames; between steps each colour eases towards its next value, so the streaks glide instead of stepping. The small waterfalls in the background of stages 11 and 13 and the trickles down the walls in stages 24-25 cycle a few colours every 16-17 frames; there each step is morphed column by column (the shades slide down to where the next step puts them). The water's surface in stages 24-25 has two animation frames, swapped every 16 frames; the frames in between are morphed column by column too, so the crest rises and falls. The waterspouts in stage 10, which the game animates only by swapping each for its mirror image every few frames (and bobbing it a pixel every 4), are drawn as a smooth loop between the two poses, without the bob; drawing only, the game still moves them as before. The cream at the bottom of stages 15 and 23 has three animation frames, swapped every 12 frames, each its picture moved 48 pixels left; in between, each row is morphed towards the next frame (pixels moving no more than 24 pixels, so its bands sway side to side as their colours blend) and the surface rises and falls in place. The cream stays solid.
: Water the game draws solid (the river in stage 12) is unchanged. `--vanilla-water` turns it off for one run.

**Smooth Glass**
: On by default. The library's glass jars (stages 14 and 16-19) are drawn the same way: their glass is a checkerboard of one colour in the middle and solid at the rim. With this on it is see-through, as Smooth Water does for water, so the wallpaper and anything behind a jar show through it evenly. `--vanilla-glass` turns it off for one run.

**Remove Sprite Limit**
: On by default. The console draws at most 20 sprites on a line and drops the rest, so on busy lines (more of them in the widened view) parts of sprites vanish or flicker. With this on, every sprite on the line is drawn.

**Upscaler**
: Off, HQ2x, HQ3x, HQ4x or cleanEdge. HQ2x-4x: Maxim Stepin's hqx filter, which redraws the pixel art at 2, 3 or 4 times the size with smoothed edges and diagonals; the result is then stretched to the window. cleanEdge: torcado's filter, which keeps the art's hard edges and colours and redraws its diagonals and slopes as clean lines, at the window's size (rounded up, up to 6 times); checkerboard and hatched dithering is left as it is. Its Line Width slider (0.45-1.14, default 1.00) makes those lines thinner or bolder, and `--clean-edge-width W` sets it for one run.
: Used only while the CRT shader is off. With Smooth Scrolling on, each layer (planes A and B and the sprites, in front of and behind the others) is enlarged on its own and then moved by its fraction of a pixel, so scrolling stays smooth; sprite outlines are then smoothed against the transparency around them rather than against the scenery. `--hqx N` sets it for one run (5 = cleanEdge) and turns the CRT shader off. On Android, HQ3x and HQ4x aren't offered and cleanEdge turns Smooth Scrolling off.

**Scanlines**
: Off, 25%, 50%, 75% or 100%: dark lines between the game's lines, without the CRT shader's other effects (100% makes them black). Used only while the CRT shader is off. `--scanlines N` sets it for one run and turns the CRT shader off.

**CRT Shader** (F12)
: Draws the picture like a CRT television: scanlines, a phosphor mask and a soft glow, blended in linear light. Written for this port after Timothy Lottes' public-domain CRT shader. Off by default; `--crt` turns it on for one run.

**CRT Shader Settings...**
: A window with sliders for scanline and pixel sharpness, glow, brightness, the mask type (slot, aperture grille, stretched VGA, VGA or none), its strength and size, and screen curvature (flat by default), plus a reset button. Changes show at once and are saved.

## Cheats

**Invincibility**
: Enemies and deadly hazards don't hurt Mickey (no contact, no damage, no hazard death). Falling off the stage still costs a try.

**Infinite Power**
: Hits knock Mickey back but take no power (Game Genie ALKA-AA9Y).

**Infinite Tries**
: Losing a life, falls included, costs no try (ALKT-AA2A, SC8T-AYXR).

**Infinite Items**
: Throwing doesn't use up items (SC6A-AY0L).

**Infinite Continues**
: The continue screen doesn't use up continues.

**Start with Full Power**
: Every life starts with all five power stars, on any difficulty.

**Walk Faster**
: A steady 2.3 pixels a frame instead of 1.4. Based on AMFA-AABN/ADFA-BAVR, which alone makes the speed saw-tooth between the two; the two speed comparisons and the slow-down threshold are raised as well, so Mickey still stops when you let go.

**Moon Jump**
: Holding jump keeps Mickey rising (RHEA-A6ZW, RHEA-A60G). Switches Jump in Midair off.

**Jump in Midair**
: Each press of jump in the air starts a new jump, held for height like a normal one (the first jump too). Unlike the Game Genie version (the moon jump codes plus A1EA-AAF0), which rises only on the frame jump is pressed. Switches Moon Jump off.

## Debug

**Go to stage**
: Starts a new game at the chosen stage straight away, from anywhere (title, attract demo or mid-game), with fresh lives, continues and score; the current stage is ticked. See [Stages](stages.md).

**Open Settings Folder**
: Opens the folder that holds `settings.ini` in the file manager.

**Show Coordinates**
: Shows the stage (and the game's own index for it, `$FFFF42`), Mickey's position in the level and the camera's in the top right, below the FPS counter when that is on; outside a stage, the `GameState`. Useful when reporting where something happened. `--show-coordinates` shows it for one run.

## Saved settings

Widescreen, Fullscreen, Easy Bounce Attack, the graphics enhancements, V-Sync, Frame Rate, High Refresh Interpolation, MD+ Soundtrack, Show FPS, Show Coordinates, the Upscaler (and cleanEdge's Line Width), Scanlines, the CRT shader and its settings, Quick Settings (and Custom's choices), and the cheats are remembered between runs, one `key = value` line each in `settings.ini`. Delete the file to go back to the defaults.

| Platform | Settings file |
|----------|---------------|
| Windows  | `%APPDATA%\CastleOfIllusion\Port\settings.ini` |
| Linux and Steam Deck | `~/.local/share/CastleOfIllusion/Port/settings.ini` |
| Android  | inside the app's storage |

## Command-line options

These apply to one run only and don't change the saved settings.

| Option | Effect |
|--------|--------|
| `--no-widescreen` | Start at 320 x 224 |
| `--easy-bounce` | Easy Bounce Attack on |
| `--smooth-swing` | Smooth Swinging on |
| `--smooth-scroll`, `--no-smooth-scroll` | Smooth Scrolling on or off |
| `--vanilla-rock-edges`, `--vanilla-fades`, `--vanilla-water`, `--vanilla-glass` | That enhancement off |
| `--show-fps`, `--show-coordinates` | Show the counter or the coordinates |
| `--interpolate`, `--no-interpolate` | High Refresh Interpolation on or off |
| `--frame-rate N` | Frame Rate 60, 120 or 240 |
| `--mdplus`, `--no-mdplus` | MD+ soundtrack on or off |
| `--cheat NAME` | A cheat: invincibility, infinite-power, infinite-tries, infinite-items, infinite-continues, full-power, walk-faster, moon-jump, midair-jump (repeat for more) |
| `--crt` | CRT shader on |
| `--scanlines N` | Scanlines at N% |
| `--hqx N` | Upscaler: 2-4 for HQ2x-HQ4x, 5 for cleanEdge |
| `--clean-edge-width W` | cleanEdge's Line Width |
| `--stage N` | Start the first level at stage N ([Stages](stages.md)) |
| `--show-state` | The current `GameState` in the window title |
| `--no-native` | Run the game's code on the interpreter instead of the translator ([Native code](native.md)) |
| `--console` | Windows: also show a terminal with the port's messages |
