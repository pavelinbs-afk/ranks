#include "lr_core.h"
#include "commands.h"
#include "config.h"
#include "players.h"
#include "tab.h"

#include <convar.h>
#include <cstdlib>

bool g_bCustomRoundActive = false;

static CConVar<bool> g_cvLrCustomRoundActive(
	"lr_custom_round_active",
	FCVAR_NOTIFY,
	"1 while VIP custom round is active (LevelsRanks C# polls this)",
	false);

static int ResolveTabSlot(const char* token)
{
	if (!token || !*token)
		return -1;

	char* end = nullptr;
	unsigned long long v = strtoull(token, &end, 10);
	if (end && *end == '\0')
	{
		if (v <= 63)
			return (int)v;
		return FindSlotBySteam64(v);
	}
	return -1;
}

CON_COMMAND_F(lr_tab_set, "lr_tab_set <slot|steamid64> <level> [exp] — C# LevelsRanks → TAB icon", FCVAR_GAMEDLL)
{
	if (args.ArgC() < 3)
	{
		Msg("Usage: lr_tab_set <slot|steamid64> <level> [exp]\n");
		return;
	}

	int iSlot = ResolveTabSlot(args[1]);
	int level = atoi(args[2]);
	int exp = args.ArgC() >= 4 ? atoi(args[3]) : -1;

	if (iSlot < 0 || iSlot >= LR_MAXPLAYERS || !g_Players[iSlot].loaded)
	{
		Msg("[LR] lr_tab_set: player not online/loaded (%s)\n", args[1]);
		return;
	}

	Tab_SetPlayerLevel(iSlot, level, exp);
	Msg("[LR] lr_tab_set slot=%d level=%d exp=%d\n", iSlot, g_Players[iSlot].level,
		g_Players[iSlot].st.exp);
}

CON_COMMAND_F(lr_tab_refresh, "lr_tab_refresh <slot|steamid64> — force TAB re-apply", FCVAR_GAMEDLL)
{
	if (args.ArgC() < 2)
	{
		Msg("Usage: lr_tab_refresh <slot|steamid64>\n");
		return;
	}

	int iSlot = ResolveTabSlot(args[1]);
	if (iSlot < 0 || iSlot >= LR_MAXPLAYERS || !g_Players[iSlot].loaded)
	{
		Msg("[LR] lr_tab_refresh: player not online/loaded (%s)\n", args[1]);
		return;
	}

	Tab_RefreshPlayer(iSlot);
	Msg("[LR] lr_tab_refresh slot=%d\n", iSlot);
}

CON_COMMAND_F(lr_tab_test, "lr_tab_test <steamid64> <value> — preview TAB badge (0 = clear)", FCVAR_GAMEDLL)
{
	if (args.ArgC() < 3)
	{
		Msg("Usage: lr_tab_test <steamid64> <value>\n");
		return;
	}

	uint64_t steam64 = strtoull(args[1], nullptr, 10);
	int badge = atoi(args[2]);
	if (badge < 0 || badge > 999999999)
	{
		Msg("[LR] badge value must be 0..999999999\n");
		return;
	}

	int iSlot = FindSlotBySteam64(steam64);
	if (iSlot < 0 || !g_Players[iSlot].loaded)
	{
		Msg("[LR] player %llu is not online/loaded\n", (unsigned long long)steam64);
		return;
	}

	g_Players[iSlot].tabOverride = badge;
	g_Players[iSlot].tabIconsApplied = false;
	g_Players[iSlot].revealSent = false;
	CGlobalVars* gv = GetGlobals();
	float now = gv ? gv->curtime : 0.0f;
	g_Players[iSlot].tabIconsAt = now;
	g_Players[iSlot].tabRefreshUntil = now + g_TabCfg.iconsRefresh;

	Msg("[LR] tab_test steam=%llu slot=%d badge=%d\n",
		(unsigned long long)steam64, iSlot, badge);
}

CON_COMMAND_F(lr_customround, "lr_customround <0|1> — VIP custom round (LevelsRanks XP gate)", FCVAR_GAMEDLL)
{
	if (args.ArgC() < 2)
	{
		Msg("Usage: lr_customround <0|1>\n");
		return;
	}

	g_bCustomRoundActive = atoi(args[1]) != 0;
	g_cvLrCustomRoundActive.Set(g_bCustomRoundActive);
	Msg("[LR] custom_round=%d\n", g_bCustomRoundActive ? 1 : 0);
}

CON_COMMAND_F(lr_status, "lr_core TAB status", FCVAR_GAMEDLL)
{
	int loaded = 0;
	for (int i = 0; i < LR_MAXPLAYERS; i++)
	{
		if (g_Players[i].loaded)
			loaded++;
	}

	Msg("[LR] TAB-only ready=%d players_loaded=%d custom_round=%d\n",
		g_bCoreReady ? 1 : 0, loaded, g_bCustomRoundActive ? 1 : 0);
	Msg("[LR] tab enabled=%d mode=%s type=%d wins=%d workshop_id=%s icons_delay=%.0f values=%d\n",
		g_TabCfg.enabled ? 1 : 0,
		g_TabCfg.mode == 1 ? "persona" : "competitive",
		g_TabCfg.type, g_TabCfg.wins,
		g_TabCfg.workshopId[0] ? g_TabCfg.workshopId : "(empty)",
		g_TabCfg.iconsDelay, (int)g_TabCfg.values.size());
}

CON_COMMAND_F(lr_reload, "Reload tab.ini", FCVAR_GAMEDLL)
{
	LoadTabConfig();

	CGlobalVars* gv = GetGlobals();
	float now = gv ? gv->curtime : 0.0f;
	for (int i = 0; i < LR_MAXPLAYERS; i++)
	{
		if (!g_Players[i].loaded)
			continue;
		g_Players[i].revealSent = false;
		g_Players[i].tabIconsApplied = false;
		g_Players[i].tabIconsAt = now + g_TabCfg.iconsDelay;
		g_Players[i].tabRefreshUntil = g_Players[i].tabIconsAt + g_TabCfg.iconsRefresh;
	}
	Msg("[LR] tab.ini reloaded\n");
}
