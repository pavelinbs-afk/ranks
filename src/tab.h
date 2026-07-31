#pragma once

#include <cstdint>

// Applies competitive-rank fields to controllers and reveals ranks:
// call every game frame.
void Tab_OnGameFrame();

// Arm the post-connect delay for workshop skillgroup icons.
// Call once when the player's stats finish loading from the DB.
void Tab_OnPlayerLoaded(int iSlot);

// Ask clients in the mask to reveal scoreboard ranks.
void Tab_SendRevealAll(uint64_t mask);
