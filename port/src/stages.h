// Stages as players number them (1-28, in the order they are played) and the game's own
// stage index ($FFFF42, 0-27), which it maps to a level and act through the tables at
// $00A008/$00A024.
#pragma once

#include <cstdint>

constexpr int kStageCount = 28;

// Game stage index for each stage, in play order. The game's index order differs in two
// places (the next-stage table at $0098BE shows the real order): in The Enchanted Forest,
// indexes 1-3 are played as 3, 1, 2 (index 0 -> 2 -> 3 -> 1 -> 4), and in The Library,
// index 21 comes second (13 -> 21 -> 14 ...).
constexpr uint8_t kStageIndex[kStageCount] = {
    0, 2, 3, 1, 4,                              // The Enchanted Forest, stages 1-5
    5, 6, 7, 8,                                 // Toyland, 6-9
    9, 10, 11, 12,                              // The Storm, 10-13
    13, 21, 14, 15, 16, 17, 18, 19, 20, 22,     // The Library, 14-23
    23, 24, 25, 26,                             // The Castle, 24-27
    27,                                         // final boss, 28
};

// Stage number (1-28) for a game stage index, or 0 if it is out of range.
inline int stage_number(int index)
{
    for (int i = 0; i < kStageCount; i++)
        if (kStageIndex[i] == index)
            return i + 1;
    return 0;
}

struct Level {
    const char* name;
    int first, last;                            // stage numbers; the last is the boss
};

constexpr Level kLevels[] = {
    {"The Enchanted Forest", 1, 5},
    {"Toyland", 6, 9},
    {"The Storm", 10, 13},
    {"The Library", 14, 23},
    {"The Castle", 24, 27},
    {"Final Boss", 28, 28},
};
