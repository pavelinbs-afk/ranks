#include "lr_core.h"
#include "commands.h"
#include "chat.h"
#include "config.h"
#include "db.h"
#include "events.h"
#include "menu.h"
#include "players.h"

#include <convar.h>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// chat commands
// ---------------------------------------------------------------------------

static bool ParseMenuKey(const char* text, int* outKey)
{
	if (!text || (text[0] != '!' && text[0] != '/'))
		return false;
	if (text[1] < '1' || text[1] > '9')
		return false;
	if (text[2] != '\0' && text[2] != ' ')
		return false;
	*outKey = text[1] - '0';
	return true;
}

static void CmdRank(int iSlot)
{
	Menu_OpenRank(iSlot);
}

static void CmdLevel(int iSlot)
{
	Menu_OpenLvl(iSlot);
}

static void CmdSession(int iSlot)
{
	Menu_OpenSession(iSlot);
}

static void CmdTop(int iSlot, bool byTime)
{
	Menu_OpenTop(iSlot, byTime);
}

static void CmdResetStats(int iSlot)
{
	PlayerInfo& p = g_Players[iSlot];
	if (!p.loaded)
	{
		LRCenterPhrase(iSlot, "NotLoaded");
		return;
	}
	if (!g_Cfg.showResetStats)
		return;

	time_t now = time(nullptr);
	if (p.resetCooldownUntil > now)
	{
		int left = (int)(p.resetCooldownUntil - now);
		LRCenterPhrase(iSlot, "ResetStatsCooldown", left / 3600, left / 60 % 60);
		return;
	}

	p.resetCooldownUntil = now + g_Cfg.resetCooldown;
	ResetPlayerStats(iSlot);
	LRCenterPhrase(iSlot, "ResetStatsDone");
}

// Copies the bare command word ("!session extra" -> "session").
static void ParseCmdWord(const char* text, char* out, int outSize)
{
	V_snprintf(out, outSize, "%s", text + 1);
	for (char* c = out; *c; c++)
	{
		if (*c == ' ')
		{
			*c = '\0';
			break;
		}
	}
}

static bool RunChatCommand(int iSlot, const char* text)
{
	if (iSlot < 0 || iSlot >= LR_MAXPLAYERS)
		return false;
	if (text[0] != '!' && text[0] != '/')
		return false;

	int menuKey = 0;
	if (ParseMenuKey(text, &menuKey) && Menu_IsActive(iSlot))
	{
		Menu_TryHandleKey(iSlot, menuKey);
		return true;
	}

	char cmd[32];
	ParseCmdWord(text, cmd, sizeof(cmd));

	if (!V_stricmp(cmd, "rank") || !V_stricmp(cmd, "ранг"))
		CmdRank(iSlot);
	else if (!V_stricmp(cmd, "lvl") || !V_stricmp(cmd, "level") || !V_stricmp(cmd, "лвл"))
		CmdLevel(iSlot);
	else if (!V_stricmp(cmd, "top"))
		CmdTop(iSlot, false);
	else if (!V_stricmp(cmd, "toptime"))
		CmdTop(iSlot, true);
	else if (!V_stricmp(cmd, "session"))
		CmdSession(iSlot);
	else if (!V_stricmp(cmd, "resetstats") || !V_stricmp(cmd, "rs"))
		CmdResetStats(iSlot);
	else
		return false;

	return true;
}

bool Commands_IsChatCommand(int iSlot, const char* text)
{
	if (!text || (text[0] != '!' && text[0] != '/'))
		return false;

	int menuKey = 0;
	if (ParseMenuKey(text, &menuKey) && iSlot >= 0 && Menu_IsActive(iSlot))
		return true;

	char cmd[32];
	ParseCmdWord(text, cmd, sizeof(cmd));

	static const char* known[] = {
		"rank", "ранг", "lvl", "level", "лвл",
		"top", "toptime", "session", "resetstats", "rs",
	};
	for (const char* k : known)
	{
		if (!V_stricmp(cmd, k))
			return true;
	}
	return false;
}

// ---------------------------------------------------------------------------
// Deferred execution: the say hook fires before the engine echoes the player's
// own chat line, so output produced there would appear *above* the "!session"
// they typed. We stash the command and run it on the next frame instead.
// ---------------------------------------------------------------------------

struct PendingCmd
{
	int iSlot;
	uint64_t steam64; // guards against the slot being reused before we run
	char text[64];
};

static std::vector<PendingCmd> s_Pending;

void Commands_QueueChat(int iSlot, const char* text)
{
	if (iSlot < 0 || iSlot >= LR_MAXPLAYERS)
		return;

	PendingCmd p;
	p.iSlot = iSlot;
	p.steam64 = g_Players[iSlot].steam64;
	V_snprintf(p.text, sizeof(p.text), "%s", text);
	s_Pending.push_back(p);
}

void Commands_ProcessQueue()
{
	if (s_Pending.empty())
		return;

	// Take a copy: a command may queue more work (or a player may disconnect).
	std::vector<PendingCmd> batch;
	batch.swap(s_Pending);

	for (const PendingCmd& p : batch)
	{
		if (g_Players[p.iSlot].steam64 != p.steam64)
			continue; // player left, slot reused
		RunChatCommand(p.iSlot, p.text);
	}
}

// ---------------------------------------------------------------------------
// server console commands (integration surface for C# plugins / the website)
// ---------------------------------------------------------------------------

CON_COMMAND_F(lr_exp, "lr_exp <steamid64> <give|take|set> <amount>", FCVAR_GAMEDLL)
{
	if (args.ArgC() < 4)
	{
		Msg("Usage: lr_exp <steamid64> <give|take|set> <amount>\n");
		return;
	}

	uint64_t steam64 = strtoull(args[1], nullptr, 10);
	int amount = atoi(args[3]);
	const char* mode = args[2];

	int iSlot = FindSlotBySteam64(steam64);
	if (iSlot >= 0 && g_Players[iSlot].loaded)
	{
		int delta = 0;
		if (!V_stricmp(mode, "give"))
			delta = amount;
		else if (!V_stricmp(mode, "take"))
			delta = -amount;
		else if (!V_stricmp(mode, "set"))
			delta = amount - g_Players[iSlot].st.exp;
		else
		{
			Msg("[LR] unknown mode: %s\n", mode);
			return;
		}

		// ChangeExp refuses players who are not fully authenticated yet — do not
		// report success in that case, the caller would record a phantom grant.
		if (!ChangeExp(iSlot, delta, delta >= 0 ? "AdminGive" : "AdminTake", true))
		{
			Msg("[LR] %llu is online but not ready yet (still loading / not authenticated), nothing changed\n",
				(unsigned long long)steam64);
			return;
		}

		if (g_Cfg.saveMode)
			SavePlayer(iSlot);
		Msg("[LR] %llu exp is now %i\n", (unsigned long long)steam64, g_Players[iSlot].st.exp);
		return;
	}

	// offline player: apply directly in the DB (level column syncs on next join)
	if (!g_bCoreReady)
	{
		Msg("[LR] core is not ready\n");
		return;
	}

	int expMode = !V_stricmp(mode, "give") ? 1 : (!V_stricmp(mode, "take") ? -1 : 0);
	if (V_stricmp(mode, "give") && V_stricmp(mode, "take") && V_stricmp(mode, "set"))
	{
		Msg("[LR] unknown mode: %s\n", mode);
		return;
	}

	char q[256];
	V_snprintf(q, sizeof(q), "SELECT `rank`, `value` FROM `%s` WHERE `steamid64` = %llu LIMIT 1;",
		g_Cfg.tableName, (unsigned long long)steam64);
	DB_Query(q, [steam64, amount, expMode](const DBResult& r) {
		if (!r.ok)
		{
			Msg("[LR] lr_exp db error: %s\n", r.error.c_str());
			return;
		}
		if (!r.RowCount())
		{
			Msg("[LR] lr_exp: steamid64 %llu not found in db\n", (unsigned long long)steam64);
			return;
		}

		int rankCount = (int)g_Cfg.ranksExp.size();
		int level = r.GetInt(0, 0);
		int exp = r.GetInt(0, 1);
		if (level < 1) level = 1;
		if (level > rankCount) level = rankCount;

		if (expMode > 0)
			exp += amount;
		else if (expMode < 0)
			exp -= amount;
		else
			exp = amount;
		NormalizeRankState(level, exp);

		char q2[384];
		V_snprintf(q2, sizeof(q2),
			"UPDATE `%s` SET `rank` = %i, `value` = %i WHERE `steamid64` = %llu;",
			g_Cfg.tableName, level, exp, (unsigned long long)steam64);
		DB_Query(q2, [steam64](const DBResult& r2) {
			if (!r2.ok)
				Msg("[LR] lr_exp db error: %s\n", r2.error.c_str());
			else
				Msg("[LR] lr_exp: updated offline player %llu\n", (unsigned long long)steam64);
		});
	});
}

CON_COMMAND_F(lr_tab_test, "lr_tab_test <steamid64> <value> — предпросмотр TAB-бейджа (0 = снять)", FCVAR_GAMEDLL)
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
		Msg("[LR] badge value must be 0..999999999 (skillgroup ID)\n");
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
	// Preview immediately: skip mount delay for lr_tab_test.
	g_Players[iSlot].tabIconsAt = now;
	g_Players[iSlot].tabRefreshUntil = now + g_TabCfg.iconsRefresh;
	if (badge)
		Msg("[LR] tab override for %llu: %i\n", (unsigned long long)steam64, badge);
	else
		Msg("[LR] tab override for %llu: off\n", (unsigned long long)steam64);
}

CON_COMMAND_F(lr_reset, "lr_reset <all|exp|stats> — wipe database statistics", FCVAR_GAMEDLL)
{
	if (args.ArgC() < 2)
	{
		Msg("Usage: lr_reset <all|exp|stats>\n");
		return;
	}
	if (!g_bCoreReady)
	{
		Msg("[LR] core is not ready\n");
		return;
	}

	char q[512];
	const char* mode = args[1];
	if (!V_stricmp(mode, "all"))
		V_snprintf(q, sizeof(q), "TRUNCATE TABLE `%s`;", g_Cfg.tableName);
	else if (!V_stricmp(mode, "exp"))
		V_snprintf(q, sizeof(q), "UPDATE `%s` SET `value` = 0, `rank` = 1;",
			g_Cfg.tableName);
	else if (!V_stricmp(mode, "stats"))
		V_snprintf(q, sizeof(q), "UPDATE `%s` SET `kills` = 0, `deaths` = 0, `shoots` = 0, `hits` = 0, "
			"`headshots` = 0, `assists` = 0, `round_win` = 0, `round_lose` = 0;", g_Cfg.tableName);
	else
	{
		Msg("[LR] unknown reset mode: %s\n", mode);
		return;
	}

	DB_Query(q, [](const DBResult& r) {
		if (!r.ok)
		{
			Msg("[LR] lr_reset failed: %s\n", r.error.c_str());
			return;
		}
		// Reload everyone who is online. flushCurrent = false: the table was
		// just wiped, writing the pre-reset values back would undo it.
		for (int i = 0; i < LR_MAXPLAYERS; i++)
		{
			if (g_Players[i].steam64)
				LoadPlayer(i, g_Players[i].steam64, false);
		}
		Msg("[LR] database statistics cleared\n");
	});
}

CON_COMMAND_F(lr_reload, "Reload lr_core configs", FCVAR_GAMEDLL)
{
	if (!LoadAllConfigs())
	{
		Msg("[LR] config reload FAILED, check warnings above\n");
		return;
	}

	// The Ranks list may have changed length: re-derive every level so nothing
	// keeps an index into the old, longer list.
	CGlobalVars* gv = GetGlobals();
	float now = gv ? gv->curtime : 0.0f;
	for (int i = 0; i < LR_MAXPLAYERS; i++)
	{
		if (g_Players[i].loaded)
		{
			CheckRank(i, false);
			g_Players[i].revealSent = false;
			g_Players[i].tabIconsApplied = false;
			g_Players[i].tabIconsAt = now + g_TabCfg.iconsDelay;
			g_Players[i].tabRefreshUntil = g_Players[i].tabIconsAt + g_TabCfg.iconsRefresh;
		}
	}

	Msg("[LR] configs reloaded\n");
}

CON_COMMAND_F(lr_status, "lr_core status", FCVAR_GAMEDLL)
{
	int loaded = 0, online = 0;
	for (int i = 0; i < LR_MAXPLAYERS; i++)
	{
		if (g_Players[i].steam64)
			online++;
		if (g_Players[i].loaded)
			loaded++;
	}
	Msg("[LR] ready=%d db_connected=%d players_online=%d players_loaded=%d db_players=%d allow_stats=%d custom_round=%d\n",
		g_bCoreReady, DB_IsConnected(), online, loaded, g_iDBCountPlayers, g_bAllowStatistic, g_bCustomRoundActive);
}

CON_COMMAND_F(lr_customround, "lr_customround <0|1> — VIP custom round state (blocks XP when lr_block_customround=1)", FCVAR_GAMEDLL)
{
	if (args.ArgC() < 2)
	{
		Msg("Usage: lr_customround <0|1>\n");
		return;
	}

	int value = atoi(args[1]);
	g_bCustomRoundActive = value != 0;
	CheckAllowStatistic(false);
	Msg("[LR] custom_round=%d allow_stats=%d\n", g_bCustomRoundActive ? 1 : 0, g_bAllowStatistic);
}

// ---------------------------------------------------------------------------
// lr_stats: one JSON line per player, for the C# plugins and the website.
// Works for offline players too (read straight from the DB).
// ---------------------------------------------------------------------------

static std::string JsonEscape(const char* in)
{
	std::string out;
	for (const char* c = in; *c; c++)
	{
		switch (*c)
		{
			case '"':  out += "\\\""; break;
			case '\\': out += "\\\\"; break;
			case '\n': out += "\\n"; break;
			case '\r': out += "\\r"; break;
			case '\t': out += "\\t"; break;
			default:
				if ((unsigned char)*c < 0x20)
					continue; // drop control chars, keep utf-8 bytes as-is
				out += *c;
		}
	}
	return out;
}

static void PrintStatsJson(uint64_t steam64, const char* name, int level, int exp, int coins,
	int posTop, int total, int kills, int deaths, int headshots, int assists,
	int64_t playtime, int64_t sessionTime, bool online)
{
	Msg("[LR_STATS] {\"steamid64\":\"%llu\",\"name\":\"%s\",\"level\":%i,\"exp\":%i,\"coins\":%i,"
		"\"pos_top\":%i,\"total_players\":%i,\"kills\":%i,\"deaths\":%i,\"headshots\":%i,"
		"\"assists\":%i,\"kd\":%.2f,\"playtime\":%lld,\"session_time\":%lld,\"online\":%s}\n",
		(unsigned long long)steam64, JsonEscape(name).c_str(), level, exp, coins,
		posTop, total, kills, deaths, headshots, assists, StatKD(kills, deaths),
		(long long)playtime, (long long)sessionTime, online ? "true" : "false");
}

CON_COMMAND_F(lr_stats, "lr_stats <steamid64> — player stats as JSON (top place, kills, hs, deaths, kd, playtime)", FCVAR_GAMEDLL)
{
	if (args.ArgC() < 2)
	{
		Msg("Usage: lr_stats <steamid64>\n");
		return;
	}

	uint64_t steam64 = strtoull(args[1], nullptr, 10);
	if (!steam64)
	{
		Msg("[LR] invalid steamid64: %s\n", args[1]);
		return;
	}
	if (!g_bCoreReady)
	{
		Msg("[LR] core is not ready\n");
		return;
	}

	int iSlot = FindSlotBySteam64(steam64);

	// Online: in-memory totals are fresher than the DB; only the top position
	// needs a round-trip (it moves as other players earn exp).
	if (iSlot >= 0 && g_Players[iSlot].loaded)
	{
		char q[512];
		V_snprintf(q, sizeof(q),
			"SELECT (SELECT COUNT(`steam`) FROM `%s` WHERE (`rank` > %i OR (`rank` = %i AND `value` >= %i)) AND `lastconnect`), "
			"(SELECT COUNT(`steam`) FROM `%s` WHERE `lastconnect`);",
			g_Cfg.tableName, g_Players[iSlot].level, g_Players[iSlot].level,
			g_Players[iSlot].st.exp, g_Cfg.tableName);

		DB_Query(q, [iSlot, steam64, gen = PlayerGeneration(iSlot)](const DBResult& r) {
			PlayerInfo& p = g_Players[iSlot];
			if (p.steam64 != steam64 || !p.loaded || PlayerGeneration(iSlot) != gen)
				return; // left while the query was in flight

			int posTop = (r.ok && r.RowCount()) ? r.GetInt(0, 0) : p.posTop;
			int total = (r.ok && r.RowCount()) ? r.GetInt(0, 1) : g_iDBCountPlayers;
			p.posTop = posTop;

			PrintStatsJson(p.steam64, p.name, p.level, p.st.exp, p.coins, posTop, total,
				p.st.kills, p.st.deaths, p.st.headshots, p.st.assists,
				TotalPlaytime(iSlot), SessionTime(iSlot), true);
		});
		return;
	}

	// Offline: everything from the DB.
	char q[1024];
	V_snprintf(q, sizeof(q),
		"SELECT `name`, `rank`, `value`, `coins`, `kills`, `deaths`, `headshots`, `assists`, `playtime`, "
		"(SELECT COUNT(`steam`) FROM `%s` WHERE (`rank` > `p`.`rank` OR (`rank` = `p`.`rank` AND `value` >= `p`.`value`)) AND `lastconnect`), "
		"(SELECT COUNT(`steam`) FROM `%s` WHERE `lastconnect`) "
		"FROM `%s` `p` WHERE `steamid64` = %llu LIMIT 1;",
		g_Cfg.tableName, g_Cfg.tableName, g_Cfg.tableName, (unsigned long long)steam64);

	DB_Query(q, [steam64](const DBResult& r) {
		if (!r.ok)
		{
			Msg("[LR] lr_stats db error: %s\n", r.error.c_str());
			return;
		}
		if (!r.RowCount())
		{
			Msg("[LR_STATS] {\"steamid64\":\"%llu\",\"found\":false}\n", (unsigned long long)steam64);
			return;
		}

		PrintStatsJson(steam64, r.Get(0, 0), r.GetInt(0, 1), r.GetInt(0, 2), r.GetInt(0, 3),
			r.GetInt(0, 9), r.GetInt(0, 10),
			r.GetInt(0, 4), r.GetInt(0, 5), r.GetInt(0, 6), r.GetInt(0, 7),
			r.GetInt64(0, 8), 0, false);
	});
}
