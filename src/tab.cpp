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

static float s_flNextRevealAll = 0.0f;

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

// Force-dirty all three competitive-rank fields (even if values are unchanged).
// Needed after MultiAddonManager finishes mounting: the client may have already
// failed to open skillgroup{N}.vsvg_c once; a fresh networked write + reveal
// makes Panorama retry the resource.
static void Tab_ForceApplyCompetitive(CEntityInstance* pController, int wins, int rankType, int rankValue)
{
	int32_t offWins = s_fCompWins.Offset();
	int32_t offType = s_fCompRankType.Offset();
	int32_t offRank = s_fCompRanking.Offset();
	if (offWins < 0 || offType < 0 || offRank < 0)
		return;

	*reinterpret_cast<int32_t*>(reinterpret_cast<uintptr_t>(pController) + offWins) = wins;
	*reinterpret_cast<int8_t*>(reinterpret_cast<uintptr_t>(pController) + offType) = (int8_t)rankType;
	*reinterpret_cast<int32_t*>(reinterpret_cast<uintptr_t>(pController) + offRank) = rankValue;

	Schema_NetworkStateChanged(pController, offWins);
	Schema_NetworkStateChanged(pController, offType);
	Schema_NetworkStateChanged(pController, offRank);
}

static bool Tab_IsCustomSkillgroup(int rankValue)
{
	// Vanilla MM icons are 1..18. Anything above must come from the workshop addon.
	return rankValue > 18;
}

static bool Tab_IconsReady(const PlayerInfo& p, float now)
{
	if (g_TabCfg.iconsDelay <= 0.0f)
		return true;
	if (p.tabIconsAt <= 0.0f)
		return false;
	return now >= p.tabIconsAt;
}

void Tab_OnPlayerLoaded(int iSlot)
{
	if (iSlot < 0 || iSlot >= LR_MAXPLAYERS)
		return;

	CGlobalVars* gv = GetGlobals();
	float now = gv ? gv->curtime : 0.0f;
	g_Players[iSlot].revealSent = false;
	g_Players[iSlot].tabIconsApplied = false;
	g_Players[iSlot].tabIconsAt = now + g_TabCfg.iconsDelay;
	g_Players[iSlot].tabRefreshUntil = g_Players[iSlot].tabIconsAt + g_TabCfg.iconsRefresh;
}

void Tab_OnLevelChanged(int iSlot)
{
	if (iSlot < 0 || iSlot >= LR_MAXPLAYERS || !g_Players[iSlot].loaded)
		return;

	CGlobalVars* gv = GetGlobals();
	float now = gv ? gv->curtime : 0.0f;
	g_Players[iSlot].revealSent = false;
	g_Players[iSlot].tabIconsApplied = false;
	g_Players[iSlot].tabIconsAt = now + g_TabCfg.iconsDelay;
	g_Players[iSlot].tabRefreshUntil = g_Players[iSlot].tabIconsAt + g_TabCfg.iconsRefresh;
}

void Tab_OnGameFrame()
{
	if (!g_TabCfg.enabled || !g_bCoreReady)
		return;

	CGlobalVars* gv = GetGlobals();
	float now = gv ? gv->curtime : 0.0f;

	uint64_t revealMask = 0;
	const bool periodicReveal = (g_TabCfg.revealInterval > 0.0f && now >= s_flNextRevealAll);
	if (periodicReveal)
		s_flNextRevealAll = now + g_TabCfg.revealInterval;

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

		const bool customIcon = (rankType == 12 && Tab_IsCustomSkillgroup(rankValue));
		const bool iconsReady = !customIcon || Tab_IconsReady(p, now);

		// Wait for MultiAddonManager to finish the client reconnect/download
		// cycle before pointing Panorama at skillgroup{N}.vsvg_c. An early miss
		// is cached by ResourceSystem and stays broken for the rest of the session.
		if (!iconsReady)
			continue;

		const bool inRefresh = g_TabCfg.iconsRefresh > 0.0f
			&& p.tabRefreshUntil > 0.0f
			&& now < p.tabRefreshUntil;
		const bool needForce = !p.tabIconsApplied || (inRefresh && periodicReveal);

		if (needForce)
		{
			Tab_ForceApplyCompetitive(pController, g_TabCfg.wins, rankType, rankValue);
			p.tabIconsApplied = true;
			revealMask |= 1ull << i;
		}
		else
		{
			s_fCompWins.SetNetworked<int32_t>(pController, g_TabCfg.wins);
			s_fCompRankType.SetNetworked<int8_t>(pController, (int8_t)rankType);
			s_fCompRanking.SetNetworked<int32_t>(pController, rankValue);
		}

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

		if (periodicReveal)
			revealMask |= 1ull << i;
	}

	Tab_SendRevealAll(revealMask);
}
