#pragma once

#include <cstdint>

class CEntityInstance;

int FindSlotBySteam64(uint64_t steam64);
void Steam64ToSteamId(uint64_t steam64, char* out, int outSize);
CEntityInstance* GetControllerBySlot(int iSlot);
