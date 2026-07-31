#include "lr_core.h"
#include "tab.h"
#include "schema.h"

#include <inetchannel.h>
#include <networksystem/inetworkserializer.h>
#include <networksystem/netmessage.h>

#define IN_SCOREBOARD_BIT 33 // +showscores button

// Resolved once each; this file runs over every player on every game frame.
static SchemaField s_fPawn         {"CBasePlayerController", "m_hPawn"};
static SchemaField s_fMoveServices {"CBasePlayerPawn", "m_pMovementServices"};
static SchemaField s_fButtons      {"CPlayer_MovementServices", "m_nButtons"};
static SchemaField s_fButtonStates {"CInButtonState", "m_pButtonStates"};
static SchemaField s_fInvServices  {"CCSPlayerController", "m_pInventoryServices"};
static SchemaField s_fPersonaLevel {"CCSPlayerController_InventoryServices", "m_nPersonaDataPublicLevel"};
static SchemaField s_fCompWins     {"CCSPlayerController", "m_iCompetitiveWins"};
static SchemaField s_fCompRankType {"CCSPlayerController", "m_iCompetitiveRankType"};
static SchemaField s_fCompRanking  {"CCSPlayerController", "m_iCompetitiveRanking"};

void Tab_SendRevealAll(uint64_t mask)
{
	if (!mask || !g_pNetworkMessages || !g_pGameEventSystem)
		return;

	INetworkMessageInternal* pMsg = g_pNetworkMessages->FindNetworkMessagePartial("ServerRankRevealAll");
	if (!pMsg)
		return;

	uint64 clients = mask;
	CNetMessage* data = pMsg->AllocateMessage();
	g_pGameEventSystem->PostEventAbstract(CSplitScreenSlot(-1), false, LR_MAXPLAYERS, &clients,
		pMsg, data, 0, NetChannelBufType_t::BUF_RELIABLE);
	delete data;
}

static uint64_t ReadButtons(CEntityInstance* pController)
{
	if (!g_pGameEntitySystem)
		return 0;

	CEntityHandle hPawn = s_fPawn.Get<CEntityHandle>(pController);
	CEntityInstance* pPawn = g_pGameEntitySystem->GetEntityInstance(hPawn);
	if (!pPawn)
		return 0;

	void* pMoveServices = s_fMoveServices.Get<void*>(pPawn);
	if (!pMoveServices)
		return 0;

	int32_t offButtons = s_fButtons.Offset();
	int32_t offStates = s_fButtonStates.Offset();
	if (offButtons < 0 || offStates < 0)
		return 0;

	return *reinterpret_cast<uint64_t*>(reinterpret_cast<uintptr_t>(pMoveServices) + offButtons + offStates);
}

// Persona level badge (слева от ника): пишем m_nPersonaDataPublicLevel внутри
// InventoryServices и метим грязным поле m_pInventoryServices на контроллере —
// аналог CSSharp Utilities.SetStateChanged(controller, "CCSPlayerController",
// "m_pInventoryServices"). ServerRankRevealAll для этого бейджа не нужен.
static void Tab_SetPersonaLevel(CEntityInstance* pController, int badge)
{
	void* pInv = s_fInvServices.Get<void*>(pController);
	if (!pInv)
		return;

	int32_t offField = s_fPersonaLevel.Offset();
	int32_t offSvc   = s_fInvServices.Offset();
	if (offField < 0 || offSvc < 0)
		return;

	int32_t* pLevel = reinterpret_cast<int32_t*>(reinterpret_cast<uintptr_t>(pInv) + offField);
	if (*pLevel == badge)
		return; // no change, no dirty

	*pLevel = badge;
	Schema_NetworkStateChanged(pController, offSvc);
}

void Tab_OnGameFrame()
{
	if (!g_TabCfg.enabled || !g_bCoreReady)
		return;

	uint64_t revealMask = 0;

	for (int i = 0; i < LR_MAXPLAYERS; i++)
	{
		PlayerInfo& p = g_Players[i];
		if (!p.loaded)
			continue;

		CEntityInstance* pController = GetControllerBySlot(i);
		if (!pController)
			continue;

		int rankType, rankValue;
		int maxMapped = (int)g_TabCfg.values.size();
		if (p.level <= maxMapped && p.level >= 1)
		{
			rankType = g_TabCfg.type;
			rankValue = g_TabCfg.values[p.level - 1];
			if (!rankValue)
				rankValue = p.level;
		}
		else
		{
			rankType = g_TabCfg.aboveType;
			rankValue = g_TabCfg.aboveUseExp ? p.st.exp : g_TabCfg.aboveValue;
		}

		// lr_tab_test: предпросмотр произвольного значения бейджа
		if (p.tabOverride > 0)
			rankValue = p.tabOverride;

		if (g_TabCfg.mode == 1)
		{
			// persona: бейдж уровня профиля; competitive-колонку не трогаем
			Tab_SetPersonaLevel(pController, rankValue);
			continue;
		}

		s_fCompWins.SetNetworked<int32_t>(pController, g_TabCfg.wins);
		s_fCompRankType.SetNetworked<int8_t>(pController, (int8_t)rankType);
		s_fCompRanking.SetNetworked<int32_t>(pController, rankValue);

		if (!p.revealSent)
		{
			p.revealSent = true;
			revealMask |= 1ull << i;
		}

		// re-reveal when the player opens the scoreboard
		uint64_t buttons = ReadButtons(pController);
		if ((buttons & (1ull << IN_SCOREBOARD_BIT)) && !(p.oldButtons & (1ull << IN_SCOREBOARD_BIT)))
			revealMask |= 1ull << i;
		p.oldButtons = buttons;
	}

	Tab_SendRevealAll(revealMask);
}
