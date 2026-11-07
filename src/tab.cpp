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
static bool s_LoggedRevealMsg = false;
static bool s_LoggedOffsets = false;

void Tab_SendRevealAll(uint64_t mask)
{
	if (!mask || !g_pNetworkMessages || !g_pGameEventSystem)
		return;

	// FakeRanks-RevealAll / CSS use the full protobuf name.
	INetworkMessageInternal* pMsg = g_pNetworkMessages->FindNetworkMessagePartial("CCSUsrMsg_ServerRankRevealAll");
	if (!pMsg)
		pMsg = g_pNetworkMessages->FindNetworkMessagePartial("ServerRankRevealAll");
	if (!pMsg)
	{
		if (!s_LoggedRevealMsg)
		{
			Warning("[LR] ServerRankRevealAll netmsg not found — TAB icons may stay hidden\n");
			s_LoggedRevealMsg = true;
		}
		return;
	}

	uint64 clients = mask;
	CNetMessage* data = pMsg->AllocateMessage();
	if (!data)
		return;
	g_pGameEventSystem->PostEventAbstract(CSplitScreenSlot(-1), false, LR_MAXPLAYERS, &clients,
		pMsg, data, 0, NetChannelBufType_t::BUF_RELIABLE);
	delete data;
}

static uint64_t ReadButtons(CEntityInstance* pController)
{
	if (!g_pGameEntitySystem || !pController)
		return 0;

	CEntityHandle hPawn = s_fPawn.Get<CEntityHandle>(pController);
	if (!hPawn.IsValid())
		return 0;

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
		return;

	*pLevel = badge;
	Schema_NetworkStateChanged(pController, offSvc);
}

// Write competitive fields. ABKAM/CSS set CompetitiveWins + RankType + Ranking
// and dirty each networked prop; dirtying only RankType can leave icon stale.
// Returns true when values changed (caller should ServerRankRevealAll).
static bool Tab_ApplyCompetitive(CEntityInstance* pController, int wins, int rankType, int rankValue, bool forceDirty)
{
	int32_t offWins = s_fCompWins.Offset();
	int32_t offType = s_fCompRankType.Offset();
	int32_t offRank = s_fCompRanking.Offset();
	if (offWins < 0 || offType < 0 || offRank < 0)
	{
		if (!s_LoggedOffsets)
		{
			Warning("[LR] competitive schema offsets missing wins=%d type=%d rank=%d\n",
				offWins, offType, offRank);
			s_LoggedOffsets = true;
		}
		return false;
	}

	if (!s_LoggedOffsets)
	{
		ConMsg("[LR] TAB schema OK wins@%d type@%d rank@%d\n", offWins, offType, offRank);
		s_LoggedOffsets = true;
	}

	auto* pWins = reinterpret_cast<int32_t*>(reinterpret_cast<uintptr_t>(pController) + offWins);
	auto* pType = reinterpret_cast<uint8_t*>(reinterpret_cast<uintptr_t>(pController) + offType);
	auto* pRank = reinterpret_cast<int32_t*>(reinterpret_cast<uintptr_t>(pController) + offRank);

	const uint8_t type8 = (uint8_t)rankType;
	const bool changed = (*pWins != wins) || (*pType != type8) || (*pRank != rankValue);
	*pWins = wins;
	*pType = type8;
	*pRank = rankValue;

	if (forceDirty || changed)
	{
		Schema_NetworkStateChanged(pController, offWins);
		Schema_NetworkStateChanged(pController, offType);
		Schema_NetworkStateChanged(pController, offRank);
	}
	return changed;
}

static bool Tab_IsCustomSkillgroup(int rankValue)
{
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

void Tab_SetPlayerLevel(int iSlot, int level, int exp)
{
	if (iSlot < 0 || iSlot >= LR_MAXPLAYERS)
		return;

	PlayerInfo& p = g_Players[iSlot];
	if (!p.loaded)
		return;

	if (level < 1)
		level = 1;
	if (level > 999)
		level = 999;

	p.level = level;
	if (exp >= 0)
		p.st.exp = exp;

	p.tabOverride = 0;
	Tab_OnLevelChanged(iSlot);
}

void Tab_RefreshPlayer(int iSlot)
{
	if (iSlot < 0 || iSlot >= LR_MAXPLAYERS || !g_Players[iSlot].loaded)
		return;
	Tab_OnLevelChanged(iSlot);
}

void Tab_OnGameFrame()
{
	if (!g_TabCfg.enabled || !g_bCoreReady)
		return;

	CGlobalVars* gv = GetGlobals();
	float now = gv ? gv->curtime : 0.0f;

	uint64_t revealMask = 0;
	bool rankDataChanged = false;
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

		if (p.tabOverride > 0)
			rankValue = p.tabOverride;

		if (g_TabCfg.mode == 1)
		{
			Tab_SetPersonaLevel(pController, rankValue);
			continue;
		}

		bool customIcon = (rankType == 12 && Tab_IsCustomSkillgroup(rankValue));

		// Do NOT remap custom IDs when workshop_id is empty — mm_extra_addons alone
		// can still deliver VPK. Remapping hid skillgroup50+ even with a correct addon
		// (and produced zero FILEOPEN, because skillgroup69 was never requested).
		if (customIcon && !g_TabCfg.workshopId[0])
		{
			static bool s_LoggedNoWorkshop = false;
			if (!s_LoggedNoWorkshop)
			{
				Warning("[LR] tab.ini workshop_id empty — still writing custom skillgroup IDs; "
					"ensure mm_extra_addons has the ranks workshop (e.g. 3776604952)\n");
				s_LoggedNoWorkshop = true;
			}
		}

		// Always write Comp* like ABKAM OnTick. Do NOT skip custom IDs until iconsDelay —
		// with the addon already on the client there is no FILEOPEN, and the gate just
		// left ranking at 0 so TAB stayed blank. Delay only widens the force-refresh window.
		const bool delayDone = !customIcon || Tab_IconsReady(p, now);
		const bool inRefresh = delayDone
			&& g_TabCfg.iconsRefresh > 0.0f
			&& p.tabRefreshUntil > 0.0f
			&& now < p.tabRefreshUntil;
		const bool needForce = !p.tabIconsApplied || (inRefresh && periodicReveal);

		const bool changed = Tab_ApplyCompetitive(pController, g_TabCfg.wins, rankType, rankValue, needForce);
		if (needForce)
			p.tabIconsApplied = true;

		// RevealAll unlocks the recipient's scoreboard to SHOW everyone's Comp* fields.
		// Cruze: on TAB press. ABKAM: when this player's values change.
		if (!p.revealSent || changed)
		{
			p.revealSent = true;
			rankDataChanged = true;
			revealMask |= 1ull << i;
		}

		uint64_t buttons = ReadButtons(pController);
		if ((buttons & (1ull << IN_SCOREBOARD_BIT)) && !(p.oldButtons & (1ull << IN_SCOREBOARD_BIT)))
			revealMask |= 1ull << i;
		p.oldButtons = buttons;

		if (periodicReveal)
			revealMask |= 1ull << i;
	}

	if (revealMask)
	{
		// Rank field updates must be revealed to every loaded client; otherwise only the
		// changed slot refreshes TAB and others keep a blank icon (no FILEOPEN either).
		if (rankDataChanged || periodicReveal)
		{
			for (int i = 0; i < LR_MAXPLAYERS; i++)
			{
				if (g_Players[i].loaded)
					revealMask |= 1ull << i;
			}
		}
		Tab_SendRevealAll(revealMask);
	}
}
