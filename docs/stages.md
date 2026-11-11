---
title: Stages
nav_order: 4
---

# Stages

| Level                    | Stages | Boss stage |
|--------------------------|--------|------------|
| 1. The Enchanted Forest  | 1-4    | 5          |
| 2. Toyland               | 6-8    | 9          |
| 3. The Storm             | 10-12  | 13         |
| 4. The Library           | 14-22  | 23         |
| 5. The Castle            | 24-26  | 27         |
| Final Boss               |        | 28         |

Stages are numbered in the order they are played, and stages 27 and 28 are back-to-back boss fights.

## Starting at a given stage

The menu's **Debug > Go to stage** starts a new game at any stage straight away, from anywhere.

From the command line, `--stage N` starts the first level of a new game at stage N (1-28) instead of the first one. Press Start through the title as usual; when the game reaches its first level start, it loads stage N, and from there the game carries on as normal (finishing it leads to stage N + 1).

```bash
coi --stage 14
```

`--show-state` shows the current `GameState` in the window title, and **Debug > Show Coordinates** shows the stage and Mickey's position; both help when reporting where something happened.

## The game's own numbering

Internally the game numbers its stages 0-27 at `$FFFF42` (mapped to a level and act through the tables at `$00A008`/`$00A024`) in a slightly different order: in The Enchanted Forest, stages 2-4 are internal 2, 3 and 1, and in The Library, stage 15 is internal 21 (stages 16-22 are 14-20). The port's table is in `port/src/stages.h`.
