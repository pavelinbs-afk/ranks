#pragma once

#include <cstdint>

// Applies competitive-rank fields to controllers and reveals ranks:
// call every game frame.
void Tab_OnGameFrame();

// Ask clients in the mask to reveal scoreboard ranks.
void Tab_SendRevealAll(uint64_t mask);
