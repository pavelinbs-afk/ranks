#pragma once

#include <cstdint>

// Applies competitive-rank fields to controllers and reveals ranks:
// call every game frame.
void Tab_OnGameFrame();

// Arm the post-connect delay for workshop skillgroup icons.
void Tab_OnPlayerLoaded(int iSlot);

// Re-arm TAB refresh after a level change (custom skillgroup icons).
void Tab_OnLevelChanged(int iSlot);

// Ask clients in the mask to reveal scoreboard ranks.
void Tab_SendRevealAll(uint64_t mask);
