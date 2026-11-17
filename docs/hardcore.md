---
title: Hardcore
nav_order: 5.5
---

# Hardcore

The game's options screen (Start at the title) has a fourth difficulty, HARDCORE, after HARD: B and C go round NORMAL, HARD, HARDCORE and PRACTICE. It plays as Hard, with every Hard rule of the game, and also:

- Every level starts with 1 power.
- One try for the whole game, and no continues: a lost try ends it.
- No extra tries: the extra-try items give nothing, and the score awards none.

Hardcore and [Boss Rush](boss-rush.md) don't go together: ticking BOSS RUSH switches HARDCORE to HARD, and choosing HARDCORE unticks BOSS RUSH.

The choice stays for the rest of the session, as the screen's other choices do, and the attract demo is never played as Hardcore.

## How it works

The code is in `port/src/hardcore.asm`, assembled with Boss Rush's (`tools/boss_rush_asm.py`) and placed at `$0D1000`, past the end of the ROM; `port/src/boss_rush.cpp` patches the game to use it. The game's difficulty (`$FFFF0D`) stays 1 (Hard), and a byte at `$FFFFFF28` marks HARDCORE as chosen. Starting the game sets `$FFFFFF29` for a Hardcore game; then each level start sets power and tries to 1, continues to none (`$FFFFFF40` = 1) and the next score award out of reach, and the extra-try item checks the byte before giving anything.
