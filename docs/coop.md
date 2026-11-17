---
title: Co-op
nav_order: 5.2
---

# Two-player co-op

A second player can join the game at any time during a level and play a second Mickey, in blue shorts, on the same screen. Co-op is on Windows and Linux (including the Steam Deck); it isn't on Android yet.

> **You may run into bugs in this mode.** Many edge cases were found and fixed, but there may still be game-breaking ones. Please report any problems in [Issues](https://github.com/saphiron86/castle-of-illusion-port/issues), with the stage and what both players were doing (Debug > Show Coordinates helps).

## Joining and leaving

- **Joining:** during a level, Player 2 presses Start on a controller Player 1 isn't using, or Tab on the keyboard. When a free controller is connected, the top right corner says "Player 2 press Start to join". In stage 1, while Player 1 plays on a controller, it says "Player 2 press Enter to join" for 5 seconds: Player 2 can take the keyboard's first layout.
- **Leaving:** Player 2 holds Start. After a second "Hold Start to quit" appears, and after 5 seconds Player 2 leaves. Unplugging Player 2's controller also makes them leave. Player 1 carries on alone, as before.
- Player 2's Start never pauses the game, except while Player 1 is out of tries.
- Resetting the game, going back to the title and a game over all end co-op.
- Co-op isn't offered in a Hardcore game.

## Controls

| | Player 1 | Player 2 |
|---|---|---|
| Keyboard | arrows, Z/X/C, Enter | W/A/S/D, F/G/H, Tab |
| Bubble (keyboard) | Right Shift (Left Shift too when Player 2 isn't on the keyboard) | Left Shift |
| Controller | any controller Player 2 isn't using | the controller Player 2 joined with |
| Bubble (controller) | Y | Y |

Any mix of keyboard and controllers works. Two players on the same keyboard isn't recommended: many keyboards can't register that many keys held at once.

## How it plays

- **The camera** keeps both players in view. A player who falls behind is dragged along by the edge of the screen; one held by a wall is pinned there until the other comes back or bubbles them along.
- **Power and tries** are each player's own, shown as P1 (red) and P2 (blue) at the bottom of the screen. Items and the score are shared.
- **Losing a try** (power running out or a fall into a pit) costs that player a try. A few seconds later they float back in from off screen in a bubble, which the other player pops by touching it. The bubble passes through walls and nothing can hurt it; out of it, the player starts with the difficulty's power and flashes for a moment.
- **The bubble button** puts a player into a bubble whenever they like, handy when stuck behind a wall or left behind.
- **Out of tries:** "Player 1, hold Start to continue" (top left) or "Player 2, hold Start to continue" (top right). Holding Start for 2 seconds continues, with the difficulty's starting tries, back in a bubble. Continues are unlimited in co-op.
- **Both players down at once** restarts the level if either has tries left; otherwise the difficulty's usual continue and game over rules apply.
- **Either player** can finish a stage or collect a boss's gem.
- **Bosses** can hurt and be hit by both players. The ones that chase the player pick one of the two at random every few seconds.
- **The final boss** is fought by both players, each with their own orb; Player 2 leaves as the screen fades to white, and the ending plays as it does alone.

## How it works

Player 2 is Mickey's own code run a second time each frame, with Player 2's state swapped into Mickey's place, so Player 2 moves, jumps and bounces exactly as Mickey does. The added code is in `port/src/coop.asm`, assembled with Boss Rush's (`tools/boss_rush_asm.py`) and placed at `$0D2000`, past the end of the ROM; `port/src/boss_rush.cpp` patches the game to use it. The patches are only in the ROM image the port builds while Player 2 has joined, so a single player game runs exactly the original code. Player 2's input and joining and leaving are on the port's side (`port/src/coop.cpp`), and the blue shorts are recoloured as the sprites are drawn (`port/src/vdp.cpp`).
