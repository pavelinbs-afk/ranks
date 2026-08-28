#include "lr_core.h"
#include "players.h"
#include "chat.h"
#include "db.h"
#include "events.h"
#include "schema.h"
#include "tab.h"
#include "wallet.h"

#include <climits>

PlayerInfo g_Players[LR_MAXPLAYERS];
int g_iDBCountPlayers = 0;
bool g_bCustomRoundActive = false;

// Kept outside PlayerInfo on purpose: PlayerInfo::Reset() assigns a fresh
// instance over itself, which would roll the counter back to zero.
static uint32_t s_PlayerGen[LR_MAXPLAYERS] = {0};

uint32_t PlayerGeneration(int iSlot)
{
	return (iSlot >= 0 && iSlot < LR_MAXPLAYERS) ? s_PlayerGen[iSlot] : 0;
}

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

bool IsPlayerReady(int iSlot)
{
	if (iSlot < 0 || iSlot >= LR_MAXPLAYERS)
		return false;
	PlayerInfo& p = g_Players[iSlot];
	if (!p.loaded || !p.steam64)
		return false;

	// ClientPutInServer already supplied a non-zero XUID and LoadPlayer has
	// finished the database lookup. IsClientFullyAuthenticated() can still be
	// false after the player is active on some CS2 builds, which made every
	// ChangeExp call fail silently.
	return GetControllerBySlot(iSlot) != nullptr;
}

void NormalizeRankState(int& level, int& exp)
{
	int rankCount = (int)g_Cfg.ranksExp.size();
	if (rankCount <= 0)
		return;
	if (exp < 0)
		exp = 0;

	// Ranks contains absolute cumulative-XP thresholds. rank_21 = 60000 means
	// level 21 at 60000 total XP — do not sum rank values together.
	level = 1;
	for (int i = 1; i < rankCount; i++)
	{
		if (exp < g_Cfg.ranksExp[i])
			break;
		level = i + 1;
	}
}

int ExpForNextLevel(int level)
{
	int rankCount = (int)g_Cfg.ranksExp.size();
	if (level < 1 || level >= rankCount)
		return 0;

	return g_Cfg.ranksExp[level];
}

int GetPlayerTeamNum(int iSlot)
{
	CEntityInstance* pController = GetControllerBySlot(iSlot);
	if (!pController)
		return 0;
	return Schema_Get<uint8_t>(pController, "CBaseEntity", "m_iTeamNum");
}

bool IsPlayerOnActiveTeam(int iSlot)
{
	int team = GetPlayerTeamNum(iSlot);
	// 2 = T, 3 = CT. Spectator (1) and hide/none (0) do not count.
	return team == 2 || team == 3;
}

int64_t TotalPlaytime(int iSlot)
{
	if (iSlot < 0 || iSlot >= LR_MAXPLAYERS)
		return 0;
	PlayerInfo& p = g_Players[iSlot];
	return p.dbPlaytime + p.sessionActiveSec;
}

int64_t SessionTime(int iSlot)
{
	if (iSlot < 0 || iSlot >= LR_MAXPLAYERS)
		return 0;
	PlayerInfo& p = g_Players[iSlot];
	if (!p.connectTime)
		return 0;
	return (int64_t)(time(nullptr) - p.connectTime);
}

float StatKD(int kills, int deaths)
{
	return kills / (deaths ? (float)deaths : 1.0f);
}

void FormatDuration(int64_t seconds, char* out, int outSize)
{
	if (seconds < 0)
		seconds = 0;
	int h = (int)(seconds / 3600);
	int m = (int)(seconds / 60 % 60);
	int s = (int)(seconds % 60);

	if (h > 0)
		V_snprintf(out, outSize, "%iч %02iм", h, m);
	else
		V_snprintf(out, outSize, "%iм %02iс", m, s);
}

void FormatPlaytimeLong(int64_t seconds, char* out, int outSize)
{
	if (seconds < 0)
		seconds = 0;
	int h = (int)(seconds / 3600);
	int m = (int)(seconds / 60 % 60);
	int s = (int)(seconds % 60);
	V_snprintf(out, outSize, "%i ч. %i м. %i сек.", h, m, s);
}

static void RefreshName(int iSlot)
{
	const char* name = g_pEngine->GetClientConVarValue(CPlayerSlot(iSlot), "name");
	if (name && *name)
		V_snprintf(g_Players[iSlot].name, sizeof(g_Players[iSlot].name), "%s", name);
}

void CheckRank(int iSlot, bool bNotify)
{
	PlayerInfo& p = g_Players[iSlot];
	if (g_Cfg.ranksExp.empty())
		return;

	int oldLevel = p.level;
	NormalizeRankState(p.level, p.st.exp);
	int newLevel = p.level;
	if (newLevel == oldLevel)
		return;

	Tab_OnLevelChanged(iSlot);

	if (bNotify && oldLevel > 0)
	{
		bool up = newLevel > oldLevel;

		if (up && g_Cfg.showLevelUp)
			LRPrintPhrase(iSlot, "LevelUp", newLevel);
		else if (!up && g_Cfg.showLevelDown)
			LRPrintPhrase(iSlot, "LevelDown", newLevel);

		if ((up && g_Cfg.showAllLevelUp) || (!up && g_Cfg.showAllLevelDown))
		{
			RefreshName(iSlot);
			for (int i = 0; i < LR_MAXPLAYERS; i++)
			{
				if (i != iSlot && g_Players[i].loaded)
				{
					if (up)
						LRPrintPhrase(i, "LevelUpAll", p.name, newLevel);
					else
						LRPrintPhrase(i, "LevelDownAll", p.name, newLevel);
				}
			}
		}

		if (g_Cfg.saveMode)
			SavePlayer(iSlot);
	}

	ApiFireLevelChanged(iSlot, newLevel, oldLevel);
}

static void QueueWalletCoins(int iSlot, int amount, const char* grantKind, const char* idempotencyKey)
{
	if (amount <= 0 || !grantKind || !idempotencyKey || !g_Cfg.walletEnabled)
		return;

	PlayerInfo& p = g_Players[iSlot];
	if (!p.steam64)
		return;

	Wallet_QueueGrant(iSlot, p.steam64, amount, grantKind, idempotencyKey);
}

bool ChangeExp(int iSlot, int delta, const char* phraseKey, bool bypassRestrictions,
	int coins, const char* coinGrantKind)
{
	if (!IsPlayerReady(iSlot))
		return false;
	if (!bypassRestrictions && !g_bAllowStatistic)
		return false;

	int scaledDelta = delta;
	if (!bypassRestrictions)
		scaledDelta = ScaleExpByPlayerCount(delta);

	int scaledCoins = coins;
	if (!bypassRestrictions && scaledCoins > 0)
		scaledCoins = ScaleExpByPlayerCount(scaledCoins);

	if (scaledDelta == 0 && scaledCoins <= 0)
		return true;

	PlayerInfo& p = g_Players[iSlot];
	int applied = 0;

	if (scaledDelta != 0)
	{
		int oldExp = p.st.exp;

		int64_t rawExp = (int64_t)p.st.exp + scaledDelta;
		if (rawExp > INT_MAX) rawExp = INT_MAX;
		if (rawExp < 0) rawExp = 0;
		p.st.exp = (int)rawExp;

		applied = p.st.exp - oldExp;
		p.roundExp += applied;
		p.sess.exp += applied;

		CheckRank(iSlot);
		ApiFireExpChanged(iSlot, applied, p.st.exp);
	}

	if (scaledCoins > 0 && coinGrantKind)
	{
		char idem[160];
		if (!strcmp(coinGrantKind, "lr_multikill"))
		{
			V_snprintf(idem, sizeof(idem), "lr:%i:%llu:multikill:%i:%lld",
				g_Cfg.serverId, (unsigned long long)p.steam64, p.killStreak, (long long)time(nullptr));
		}
		else
		{
			V_snprintf(idem, sizeof(idem), "lr:%i:%llu:%s:%lld",
				g_Cfg.serverId, (unsigned long long)p.steam64, coinGrantKind, (long long)time(nullptr));
		}
		QueueWalletCoins(iSlot, scaledCoins, coinGrantKind, idem);
	}

	if (g_Cfg.showUsualMessage == 1 && phraseKey && (applied != 0 || scaledCoins > 0))
	{
		if (applied != 0)
		{
			char sDelta[16];
			V_snprintf(sDelta, sizeof(sDelta), "%s%i", applied > 0 ? "+" : "", applied);

			char coinPart[64] = "";
			if (scaledCoins > 0)
			{
				char sCoins[16];
				V_snprintf(sCoins, sizeof(sCoins), "+%i", scaledCoins);
				V_snprintf(coinPart, sizeof(coinPart), " и {GOLD}%s{DEFAULT} коинов", sCoins);
			}

			LRCenterPhrase(iSlot, phraseKey, sDelta, coinPart, p.st.exp);
		}
		else if (scaledCoins > 0)
		{
			char sCoins[16];
			V_snprintf(sCoins, sizeof(sCoins), "+%i", scaledCoins);
			const char* coinPhrase = "CoinInterval";
			if (coinGrantKind && !strcmp(coinGrantKind, "lr_multikill"))
				coinPhrase = "CoinMultiKill";
			else if (coinGrantKind && !strcmp(coinGrantKind, "lr_mvp"))
				coinPhrase = "CoinMvp";
			LRCenterPhrase(iSlot, coinPhrase, sCoins);
		}
	}

	return scaledDelta != 0 || scaledCoins > 0;
}

bool GrantIntervalCoins(int iSlot)
{
	if (!IsPlayerReady(iSlot))
		return false;
	if (!g_bAllowStatistic)
		return false;

	int amount = ScaleExpByPlayerCount(g_Cfg.coinsIntervalAmount);
	if (amount <= 0)
		return false;

	PlayerInfo& p = g_Players[iSlot];
	int64_t bucket = TotalPlaytime(iSlot) / g_Cfg.coinsIntervalSec;
	char idem[160];
	V_snprintf(idem, sizeof(idem), "lr:%i:%llu:interval:%lld",
		g_Cfg.serverId, (unsigned long long)p.steam64, (long long)bucket);

	QueueWalletCoins(iSlot, amount, "lr_interval", idem);

	if (g_Cfg.showUsualMessage == 1)
	{
		char sDelta[16];
		V_snprintf(sDelta, sizeof(sDelta), "+%i", amount);
		LRCenterPhrase(iSlot, "CoinInterval", sDelta);
	}

	return true;
}

void TickActivePlaytime()
{
	if (!g_bCoreReady)
		return;

	static time_t s_lastTick = 0;
	time_t now = time(nullptr);
	if (now == s_lastTick)
		return;
	s_lastTick = now;

	for (int i = 0; i < LR_MAXPLAYERS; i++)
	{
		PlayerInfo& p = g_Players[i];
		if (!p.loaded || !IsPlayerReady(i))
			continue;
		if (!IsPlayerOnActiveTeam(i))
			continue;

		p.sessionActiveSec++;

		if (g_Cfg.coinsIntervalSec <= 0 || g_Cfg.coinsIntervalAmount <= 0)
			continue;

		p.activeSecSinceCoin++;
		if (p.activeSecSinceCoin < g_Cfg.coinsIntervalSec)
			continue;

		p.activeSecSinceCoin = 0;
		if (GrantIntervalCoins(i))
		{
			if (g_Cfg.saveMode)
				SavePlayer(i);
		}
	}
}

void PulseOnlinePresence()
{
	if (!g_bCoreReady || !DB_IsConnected())
		return;

	static float s_nextPulse = 0.0f;
	CGlobalVars* gv = GetGlobals();
	if (!gv || gv->curtime < s_nextPulse)
		return;
	s_nextPulse = gv->curtime + 15.0f;

	time_t now = time(nullptr);
	for (int i = 0; i < LR_MAXPLAYERS; i++)
	{
		PlayerInfo& p = g_Players[i];
		if (!p.loaded || !p.steam64)
			continue;

		char q[320];
		V_snprintf(q, sizeof(q),
			"UPDATE `%s` SET `online` = %i, `lastconnect` = %lld WHERE `steam` = '%s';",
			g_Cfg.tableName, g_Cfg.serverId, (long long)now, p.steamId);
		DB_Query(q);
	}
}

void ClearPlayerOnlinePresence(int iSlot)
{
	if (iSlot < 0 || iSlot >= LR_MAXPLAYERS)
		return;

	PlayerInfo& p = g_Players[iSlot];
	if (!p.steam64 || !*p.steamId)
		return;

	time_t now = time(nullptr);
	char q[320];
	V_snprintf(q, sizeof(q),
		"UPDATE `%s` SET `online` = 0, `lastconnect` = %lld WHERE `steam` = '%s';",
		g_Cfg.tableName, (long long)now, p.steamId);
	DB_Query(q);
}

void GiveTimeExp()
{
	if (!g_bCoreReady || g_Cfg.timeExpAmount == 0 || g_Cfg.timeExpInterval <= 0)
		return;

	static time_t s_lastCheck = 0;
	time_t now = time(nullptr);
	if (now == s_lastCheck)
		return;
	s_lastCheck = now;

	for (int i = 0; i < LR_MAXPLAYERS; i++)
	{
		PlayerInfo& p = g_Players[i];
		if (!p.loaded || !IsPlayerReady(i))
			continue;

		if (p.timeExpAt == 0)
		{
			p.timeExpAt = now + g_Cfg.timeExpInterval;
			continue;
		}
		if (now < p.timeExpAt)
			continue;

		p.timeExpAt = now + g_Cfg.timeExpInterval;
		ChangeExp(i, g_Cfg.timeExpAmount, "TimeExp", true);
		if (g_Cfg.saveMode)
			SavePlayer(i);
	}
}

// ---------------------------------------------------------------------------
// DB I/O
// ---------------------------------------------------------------------------

void RefreshTopPositions(int iSlot)
{
	PlayerInfo& p = g_Players[iSlot];
	if (!p.loaded)
		return;

	uint64_t steam64 = p.steam64;
	uint32_t gen = s_PlayerGen[iSlot];
	int64_t playtime = TotalPlaytime(iSlot);

	char q[512];
	V_snprintf(q, sizeof(q),
		"SELECT "
		"(SELECT COUNT(`steam`) FROM `%s` WHERE (`rank` > %i OR (`rank` = %i AND `value` >= %i)) AND `lastconnect`) AS exppos, "
		"(SELECT COUNT(`steam`) FROM `%s` WHERE `playtime` >= %lld AND `lastconnect`) AS timepos;",
		g_Cfg.tableName, p.level, p.level, p.st.exp,
		g_Cfg.tableName, (long long)playtime);

	DB_Query(q, [iSlot, steam64, gen](const DBResult& r) {
		PlayerInfo& p = g_Players[iSlot];
		if (!r.ok || !r.RowCount() || p.steam64 != steam64 || s_PlayerGen[iSlot] != gen)
			return;

		int oldTop = p.posTop, oldTopTime = p.posTopTime;
		p.posTop = r.GetInt(0, 0);
		p.posTopTime = r.GetInt(0, 1);
		if (oldTop)
			p.sessPosTop += oldTop - p.posTop;
		if (oldTopTime)
			p.sessPosTopTime += oldTopTime - p.posTopTime;
	});
}

void LoadPlayer(int iSlot, uint64_t steam64, bool flushCurrent)
{
	PlayerInfo& p = g_Players[iSlot];

	if (flushCurrent && p.loaded && p.steam64 == steam64)
		SavePlayer(iSlot);

	s_PlayerGen[iSlot]++;
	p.Reset();
	if (!steam64)
		return;

	p.steam64 = steam64;
	Steam64ToSteamId(steam64, p.steamId, sizeof(p.steamId));
	p.connectTime = time(nullptr);

	if (!g_bCoreReady)
		return;

	char q[1024];
	V_snprintf(q, sizeof(q),
		"SELECT `value`, `rank`, `kills`, `deaths`, `shoots`, `hits`, `headshots`, `assists`, "
		"`round_win`, `round_lose`, `playtime`, `coins`, `reset_cooldown`, "
		"(SELECT COUNT(`steam`) FROM `%s` WHERE (`rank` > `p`.`rank` OR (`rank` = `p`.`rank` AND `value` >= `p`.`value`)) AND `lastconnect`) AS exppos, "
		"(SELECT COUNT(`steam`) FROM `%s` WHERE `playtime` >= `p`.`playtime` AND `lastconnect`) AS timepos "
		"FROM `%s` `p` WHERE `steam` = '%s' LIMIT 1;",
		g_Cfg.tableName, g_Cfg.tableName, g_Cfg.tableName, p.steamId);

	DB_Query(q, [iSlot, steam64, gen = s_PlayerGen[iSlot]](const DBResult& r) {
		PlayerInfo& p = g_Players[iSlot];
		if (p.steam64 != steam64 || s_PlayerGen[iSlot] != gen)
			return;
		if (!r.ok)
			return;

		RefreshName(iSlot);

		if (r.RowCount())
		{
			p.st.exp       = r.GetInt(0, 0);
			p.level        = r.GetInt(0, 1);
			p.st.kills     = r.GetInt(0, 2);
			p.st.deaths    = r.GetInt(0, 3);
			p.st.shoots    = r.GetInt(0, 4);
			p.st.hits      = r.GetInt(0, 5);
			p.st.headshots = r.GetInt(0, 6);
			p.st.assists   = r.GetInt(0, 7);
			p.st.roundWin  = r.GetInt(0, 8);
			p.st.roundLose = r.GetInt(0, 9);
			p.dbPlaytime   = r.GetInt64(0, 10);
			p.resetCooldownUntil = (time_t)r.GetInt64(0, 12);
			p.posTop       = r.GetInt(0, 13);
			p.posTopTime   = r.GetInt(0, 14);

			NormalizeRankState(p.level, p.st.exp);
			p.loaded = true;

			char q2[1024];
			V_snprintf(q2, sizeof(q2),
				"UPDATE `%s` SET `online` = %i, `lastconnect` = %lld, `steamid64` = %llu, `name` = '%s', `rank` = %i, `value` = %i, `xp_cumulative` = 1 "
				"WHERE `steam` = '%s';",
				g_Cfg.tableName, g_Cfg.serverId, (long long)time(nullptr),
				(unsigned long long)p.steam64, DB_Escape(p.name).c_str(), p.level, p.st.exp, p.steamId);
			DB_Query(q2);
		}
		else
		{
			p.st = PlayerStats{};
			p.level = 1;
			p.dbPlaytime = 0;
			p.loaded = true;
			g_iDBCountPlayers++;

			char q2[768];
			V_snprintf(q2, sizeof(q2),
				"INSERT INTO `%s` (`steam`, `steamid64`, `name`, `value`, `rank`, `lastconnect`, `online`, `xp_cumulative`) "
				"VALUES ('%s', %llu, '%s', %i, %i, %lld, %i, 1) "
				"ON DUPLICATE KEY UPDATE `steamid64` = VALUES(`steamid64`), `online` = VALUES(`online`);",
				g_Cfg.tableName, p.steamId, (unsigned long long)p.steam64,
				DB_Escape(p.name).c_str(), p.st.exp, p.level, (long long)time(nullptr), g_Cfg.serverId);
			DB_Query(q2);
		}

		Tab_OnPlayerLoaded(iSlot);
		ApiFirePlayerLoaded(iSlot, steam64);
	});
}

void SavePlayer(int iSlot, bool disconnect)
{
	PlayerInfo& p = g_Players[iSlot];

	if (!p.loaded || !p.steam64 || !g_bCoreReady)
	{
		if (disconnect)
			p.Reset();
		return;
	}

	RefreshName(iSlot);

	time_t now = time(nullptr);
	int64_t totalPlaytime = TotalPlaytime(iSlot);

	char q[1536];
	V_snprintf(q, sizeof(q),
		"INSERT INTO `%s` (`steam`, `steamid64`, `name`, `value`, `rank`, `kills`, `deaths`, `shoots`, `hits`, "
		"`headshots`, `assists`, `round_win`, `round_lose`, `playtime`, `coins`, `lastconnect`, `online`, `reset_cooldown`, `xp_cumulative`) "
		"VALUES ('%s', %llu, '%s', %i, %i, %i, %i, %i, %i, %i, %i, %i, %i, %lld, %i, %lld, %i, %lld, 1) "
		"ON DUPLICATE KEY UPDATE "
		"`steamid64` = VALUES(`steamid64`), `name` = VALUES(`name`), `value` = VALUES(`value`), "
		"`rank` = VALUES(`rank`), `kills` = VALUES(`kills`), `deaths` = VALUES(`deaths`), "
		"`shoots` = VALUES(`shoots`), `hits` = VALUES(`hits`), `headshots` = VALUES(`headshots`), "
		"`assists` = VALUES(`assists`), `round_win` = VALUES(`round_win`), `round_lose` = VALUES(`round_lose`), "
		"`playtime` = VALUES(`playtime`), `coins` = VALUES(`coins`), `lastconnect` = VALUES(`lastconnect`), `online` = VALUES(`online`), "
		"`reset_cooldown` = VALUES(`reset_cooldown`), `xp_cumulative` = 1;",
		g_Cfg.tableName, p.steamId, (unsigned long long)p.steam64, DB_Escape(p.name).c_str(),
		p.st.exp, p.level, p.st.kills, p.st.deaths, p.st.shoots, p.st.hits,
		p.st.headshots, p.st.assists, p.st.roundWin, p.st.roundLose,
		(long long)totalPlaytime, 0,
		(long long)now,
		disconnect ? 0 : g_Cfg.serverId, (long long)p.resetCooldownUntil);

	DB_Query(q);

	if (disconnect)
	{
		p.Reset();
	}
	else
	{
		p.dbPlaytime = totalPlaytime;
		p.sessionActiveSec = 0;
		RefreshTopPositions(iSlot);
	}
}

void ResetPlayerStats(int iSlot)
{
	PlayerInfo& p = g_Players[iSlot];
	if (!p.loaded)
		return;

	time_t cooldown = p.resetCooldownUntil;
	int oldLevel = p.level;
	p.st = PlayerStats{};
	p.level = 1;
	p.sess = PlayerStats{};
	p.dbPlaytime = 0;
	p.sessionActiveSec = 0;
	p.connectTime = time(nullptr);
	p.activeSecSinceCoin = 0;
	p.killStreak = 0;
	p.roundExp = 0;
	p.resetCooldownUntil = cooldown;

	SavePlayer(iSlot);
	if (oldLevel != p.level)
		ApiFireLevelChanged(iSlot, p.level, oldLevel);
}

// ---------------------------------------------------------------------------
// Bootstrap
// ---------------------------------------------------------------------------

static void MigrateColumn(const char* column, const char* alterDef)
{
	char q[512];
	V_snprintf(q, sizeof(q),
		"SELECT COUNT(*) FROM information_schema.COLUMNS "
		"WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = '%s' AND COLUMN_NAME = '%s';",
		g_Cfg.tableName, column);

	std::string alter = alterDef;
	DB_Query(q, [alter](const DBResult& r) {
		if (r.ok && r.RowCount() && r.GetInt(0, 0) == 0)
			DB_Query(alter);
	});
}

static void FinishBootstrap()
{
	char q[512];
	V_snprintf(q, sizeof(q), "UPDATE `%s` SET `online` = 0 WHERE `online` = %i;", g_Cfg.tableName, g_Cfg.serverId);
	DB_Query(q);

	if (g_Cfg.cleanDbDays > 0)
	{
		V_snprintf(q, sizeof(q), "UPDATE `%s` SET `lastconnect` = 0 WHERE `lastconnect` AND `lastconnect` < %lld;",
			g_Cfg.tableName, (long long)(time(nullptr) - (int64_t)g_Cfg.cleanDbDays * 86400));
		DB_Query(q);
	}

	V_snprintf(q, sizeof(q), "SELECT COUNT(`steam`) FROM `%s` WHERE `lastconnect`;", g_Cfg.tableName);
	DB_Query(q, [](const DBResult& r) {
		if (r.ok && r.RowCount())
			g_iDBCountPlayers = r.GetInt(0, 0);

		g_bCoreReady = true;
		LR_Log("core is ready (players in db: %i)", g_iDBCountPlayers);
		ApiFireCoreReady();

		for (int i = 0; i < LR_MAXPLAYERS; i++)
		{
			if (g_Players[i].steam64 && !g_Players[i].loaded)
				LoadPlayer(i, g_Players[i].steam64);
		}
	});
}

static void MarkXpAsCumulative()
{
	std::string q = "UPDATE `" + std::string(g_Cfg.tableName)
		+ "` SET `xp_cumulative` = 1 WHERE `xp_cumulative` = 0;";

	DB_Query(q, [](const DBResult& r) {
		if (!r.ok)
		{
			Warning("[LR] Failed to mark XP as cumulative: %s\n", r.error.c_str());
			return;
		}
		LR_Log("cumulative XP marker updated (%llu row(s))", (unsigned long long)r.affected);
		FinishBootstrap();
	});
}

static void EnsureCumulativeXpColumn()
{
	char q[512];
	V_snprintf(q, sizeof(q),
		"SELECT COUNT(*) FROM information_schema.COLUMNS "
		"WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = '%s' AND COLUMN_NAME = 'xp_cumulative';",
		g_Cfg.tableName);

	DB_Query(q, [](const DBResult& r) {
		if (!r.ok || !r.RowCount())
		{
			Warning("[LR] Failed to inspect cumulative XP schema: %s\n", r.error.c_str());
			return;
		}
		if (r.GetInt(0, 0) != 0)
		{
			MarkXpAsCumulative();
			return;
		}

		char alter[256];
		V_snprintf(alter, sizeof(alter),
			"ALTER TABLE `%s` ADD COLUMN `xp_cumulative` tinyint unsigned NOT NULL DEFAULT 0;",
			g_Cfg.tableName);
		DB_Query(alter, [](const DBResult& altered) {
			if (!altered.ok)
			{
				Warning("[LR] Failed to add cumulative XP marker: %s\n", altered.error.c_str());
				return;
			}
			MarkXpAsCumulative();
		});
	});
}

void DB_Bootstrap()
{
	char q[2048];
	V_snprintf(q, sizeof(q),
		"CREATE TABLE IF NOT EXISTS `%s` ("
		"`steam` varchar(22) NOT NULL DEFAULT '', "
		"`steamid64` bigint unsigned NOT NULL DEFAULT 0, "
		"`name` varchar(128) NOT NULL DEFAULT '', "
		"`value` int NOT NULL DEFAULT 0, "
		"`rank` int NOT NULL DEFAULT 0, "
		"`kills` int NOT NULL DEFAULT 0, "
		"`deaths` int NOT NULL DEFAULT 0, "
		"`shoots` int NOT NULL DEFAULT 0, "
		"`hits` int NOT NULL DEFAULT 0, "
		"`headshots` int NOT NULL DEFAULT 0, "
		"`assists` int NOT NULL DEFAULT 0, "
		"`round_win` int NOT NULL DEFAULT 0, "
		"`round_lose` int NOT NULL DEFAULT 0, "
		"`playtime` bigint NOT NULL DEFAULT 0, "
		"`coins` int NOT NULL DEFAULT 0, "
		"`lastconnect` bigint NOT NULL DEFAULT 0, "
		"`online` int NOT NULL DEFAULT 0, "
		"`reset_cooldown` bigint NOT NULL DEFAULT 0, "
		"`xp_cumulative` tinyint unsigned NOT NULL DEFAULT 0, "
		"PRIMARY KEY (`steam`), "
		"KEY `idx_steamid64` (`steamid64`), "
		"KEY `idx_value` (`value`)"
		") ENGINE=InnoDB DEFAULT CHARSET=%s;",
		g_Cfg.tableName, g_DBConfig.charset.c_str());

	DB_Query(q, [](const DBResult& r) {
		if (!r.ok)
		{
			Warning("[LR] Failed to create table: %s\n", r.error.c_str());
			return;
		}

		char alter[256];
		V_snprintf(alter, sizeof(alter), "ALTER TABLE `%s` ADD COLUMN `steamid64` bigint unsigned NOT NULL DEFAULT 0, ADD KEY `idx_steamid64` (`steamid64`);", g_Cfg.tableName);
		MigrateColumn("steamid64", alter);
		V_snprintf(alter, sizeof(alter), "ALTER TABLE `%s` ADD COLUMN `online` int NOT NULL DEFAULT 0;", g_Cfg.tableName);
		MigrateColumn("online", alter);
		V_snprintf(alter, sizeof(alter), "ALTER TABLE `%s` ADD COLUMN `reset_cooldown` bigint NOT NULL DEFAULT 0;", g_Cfg.tableName);
		MigrateColumn("reset_cooldown", alter);
		V_snprintf(alter, sizeof(alter), "ALTER TABLE `%s` ADD COLUMN `coins` int NOT NULL DEFAULT 0;", g_Cfg.tableName);
		MigrateColumn("coins", alter);

		EnsureCumulativeXpColumn();
	});
}
