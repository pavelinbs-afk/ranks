#include "lr_core.h"
#include "players.h"

PlayerInfo g_Players[LR_MAXPLAYERS];

static const uint64_t STEAM64_BASE = 76561197960265728ULL;

void Steam64ToSteamId(uint64_t steam64, char* out, int outSize)
{
	uint64_t acc = steam64 - STEAM64_BASE;
	V_snprintf(out, outSize, "STEAM_1:%u:%u", (uint32)(acc & 1), (uint32)(acc >> 1));
}

CEntityInstance* GetControllerBySlot(int iSlot)
{
	if (!g_pGameEntitySystem || iSlot < 0 || iSlot >= LR_MAXPLAYERS)
		return nullptr;
	return g_pGameEntitySystem->GetEntityInstance(CEntityIndex(iSlot + 1));
}

int FindSlotBySteam64(uint64_t steam64)
{
	for (int i = 0; i < LR_MAXPLAYERS; i++)
	{
		if (g_Players[i].steam64 == steam64)
			return i;
	}
	return -1;
}
