---
title: Boss Rush
nav_order: 5
---

# Boss Rush

The game's options screen (Start at the title) has a fourth row, BOSS RUSH; B or C ticks it. With it ticked, Start plays the six boss fights back to back: the Old Oak Tree, Jack-in-the-Box, the Storm Men, the Licorice Dragon, the Oaf and Mizrabel (stages 5, 9, 13, 23, 27 and 28).

- Normal and Hard only: Practice has no bosses, so it can't be ticked there (switching to Practice unticks it). Each difficulty's bosses are the game's own.
- One try for the whole rush, no continues and no extra tries. Power starts as in a new game; on Normal it refills to that at every boss, on Hard it carries over.
- Each boss's clock runs from the moment you get control until it is defeated (pauses don't count) and shows in the HUD in place of the score. There is no gem to collect: 5 seconds after a defeat the screen fades and the next fight starts.
- After Mizrabel, her fade to white leads to a results screen in the style of the level-complete screen, with each boss's time and the total counting up to 1/100 s; a button then returns to the Sega logo. The ending isn't played.

The tick stays on until you untick it or restart the port, like the screen's other settings.

## How it's made

The new code is 68000 assembly in `port/src/boss_rush.asm` and `boss_rush_results.asm`, placed at `$0D0000`, past the ROM and clear of Smooth Swinging and MD+ packs; `python tools/boss_rush_asm.py` reassembles it with asl into `boss_rush_code.h` (committed, so building the port doesn't need asl). The patches that call it are in `port/src/boss_rush.cpp`.
