#pragma once

#include <cstdint>

// Creates the table (single complete CREATE — the Pisex duplicate-`online`
// bug fix), migrates legacy tables, resets online flags, counts players,
// then flips g_bCoreReady and loads already-connected players.
void DB_Bootstrap();

// Re-query the player's positions in the exp/time tops.
void RefreshTopPositions(int iSlot);

int FindSlotBySteam64(uint64_t steam64);

// "STEAM_1:X:Y" from a 64-bit account id
void Steam64ToSteamId(uint64_t steam64, char* out, int outSize);
