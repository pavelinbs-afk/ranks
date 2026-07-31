#include "lr_core.h"
#include "players.h"
#include "chat.h"
#include "db.h"
#include "schema.h"
#include "tab.h"

PlayerInfo g_Players[LR_MAXPLAYERS];
int g_iDBCountPlayers = 0;

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
	if (!GetControllerBySlot(iSlot))
		return false;
	return g_pEngine->IsClientFullyAuthenticated(CPlayerSlot(iSlot));
}

int LevelFromExp(int exp)
{
	int level = (int)g_Cfg.ranksExp.size();
	while (level > 1 && g_Cfg.ranksExp[level - 1] > exp)
		level--;
	return level;
}

int64_t TotalPlaytime(int iSlot)
{
	if (iSlot < 0 || iSlot >= LR_MAXPLAYERS)
		return 0;
	PlayerInfo& p = g_Players[iSlot];
	if (!p.sessionStart)
		return p.dbPlaytime;
	return p.dbPlaytime + (int64_t)(time(nullptr) - p.sessionStart);
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

	int newLevel = LevelFromExp(p.st.exp);
	int oldLevel = p.level;
	if (newLevel == oldLevel)
		return;

	p.level = newLevel;

	if (bNotify && oldLevel > 0)
	{
		bool up = newLevel > oldLevel;
		const char* rankName = Phrase(g_Cfg.ranksKey[newLevel - 1].c_str());

		if (up && g_Cfg.showLevelUp)
			LRPrintPhrase(iSlot, "LevelUp", rankName);
		else if (!up && g_Cfg.showLevelDown)
			LRPrintPhrase(iSlot, "LevelDown", rankName);

		if ((up && g_Cfg.showAllLevelUp) || (!up && g_Cfg.showAllLevelDown))
		{
			RefreshName(iSlot);
			for (int i = 0; i < LR_MAXPLAYERS; i++)
			{
				if (i != iSlot && g_Players[i].loaded)
					LRPrintPhrase(i, up ? "LevelUpAll" : "LevelDownAll", p.name, rankName);
			}
		}

		if (g_Cfg.saveMode)
			SavePlayer(iSlot);
	}

	ApiFireLevelChanged(iSlot, newLevel, oldLevel);
}

bool ChangeExp(int iSlot, int delta, const char* phraseKey, bool bypassRestrictions)
{
	if (!IsPlayerReady(iSlot))
		return false;
	if (!bypassRestrictions && !g_bAllowStatistic)
		return false;

	if (delta != 0)
	{
		PlayerInfo& p = g_Players[iSlot];
		int floorExp = g_Cfg.typeStatistics ? 400 : 0;
		int oldExp = p.st.exp;

		p.st.exp += delta;
		if (p.st.exp < floorExp)
			p.st.exp = floorExp;

		int applied = p.st.exp - oldExp;
		p.roundExp += applied;
		p.sess.exp += applied;

		CheckRank(iSlot);
		ApiFireExpChanged(iSlot, applied, p.st.exp);

		if (g_Cfg.showUsualMessage == 1 && phraseKey)
		{
			char sDelta[16];
			V_snprintf(sDelta, sizeof(sDelta), "%s%i", delta > 0 ? "+" : "", delta);
			LRPrintPhrase(iSlot, phraseKey, p.st.exp, sDelta);
		}
	}

	return true;
}

// Periodic exp for time spent in game. Granted with bypassRestrictions, so it
// does not depend on the player count (lr_minplayers_count) at all.
void GiveTimeExp()
{
	if (!g_bCoreReady || g_Cfg.timeExpAmount == 0 || g_Cfg.timeExpInterval <= 0)
		return;

	static time_t s_lastCheck = 0;
	time_t now = time(nullptr);
	if (now == s_lastCheck)
		return; // once per second is plenty
	s_lastCheck = now;

	for (int i = 0; i < LR_MAXPLAYERS; i++)
	{
		PlayerInfo& p = g_Players[i];
		if (!p.loaded || !IsPlayerReady(i))
			continue;

		if (p.timeExpAt == 0)
		{
			p.timeExpAt = now + g_Cfg.timeExpInterval; // first grant one interval from now
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
	int64_t playtime = TotalPlaytime(iSlot); // guards sessionStart == 0

	char q[512];
	V_snprintf(q, sizeof(q),
		"SELECT "
		"(SELECT COUNT(`steam`) FROM `%s` WHERE `value` >= %i AND `lastconnect`) AS exppos, "
		"(SELECT COUNT(`steam`) FROM `%s` WHERE `playtime` >= %lld AND `lastconnect`) AS timepos;",
		g_Cfg.tableName, p.st.exp, g_Cfg.tableName, (long long)playtime);

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

	// A map change re-runs ClientPutInServer for players who stayed, and the
	// Reset() below would drop everything not yet written out. With
	// lr_db_savedataplayer_mode 0 (save on disconnect only) that is a whole
	// map's worth of stats. Flush first; the DB worker is FIFO, so this write
	// lands before the SELECT below reads it back.
	if (flushCurrent && p.loaded && p.steam64 == steam64)
		SavePlayer(iSlot);

	s_PlayerGen[iSlot]++; // invalidate any query still in flight for this slot
	p.Reset();
	if (!steam64)
		return;

	p.steam64 = steam64;
	Steam64ToSteamId(steam64, p.steamId, sizeof(p.steamId));
	p.sessionStart = time(nullptr);
	p.connectTime = p.sessionStart; // session clock: never moved by saves

	if (!g_bCoreReady)
		return; // core-ready handler re-runs LoadPlayer for connected players

	char q[1024];
	V_snprintf(q, sizeof(q),
		"SELECT `value`, `kills`, `deaths`, `shoots`, `hits`, `headshots`, `assists`, "
		"`round_win`, `round_lose`, `playtime`, `reset_cooldown`, "
		"(SELECT COUNT(`steam`) FROM `%s` WHERE `value` >= `p`.`value` AND `lastconnect`) AS exppos, "
		"(SELECT COUNT(`steam`) FROM `%s` WHERE `playtime` >= `p`.`playtime` AND `lastconnect`) AS timepos "
		"FROM `%s` `p` WHERE `steam` = '%s' LIMIT 1;",
		g_Cfg.tableName, g_Cfg.tableName, g_Cfg.tableName, p.steamId);

	DB_Query(q, [iSlot, steam64, gen = s_PlayerGen[iSlot]](const DBResult& r) {
		PlayerInfo& p = g_Players[iSlot];
		if (p.steam64 != steam64 || s_PlayerGen[iSlot] != gen)
			return; // player left / slot reused while the query was in flight
		if (!r.ok)
			return;

		RefreshName(iSlot);

		if (r.RowCount())
		{
			p.st.exp       = r.GetInt(0, 0);
			p.st.kills     = r.GetInt(0, 1);
			p.st.deaths    = r.GetInt(0, 2);
			p.st.shoots    = r.GetInt(0, 3);
			p.st.hits      = r.GetInt(0, 4);
			p.st.headshots = r.GetInt(0, 5);
			p.st.assists   = r.GetInt(0, 6);
			p.st.roundWin  = r.GetInt(0, 7);
			p.st.roundLose = r.GetInt(0, 8);
			p.dbPlaytime   = r.GetInt64(0, 9);
			p.resetCooldownUntil = (time_t)r.GetInt64(0, 10);
			p.posTop       = r.GetInt(0, 11);
			p.posTopTime   = r.GetInt(0, 12);
			p.level = LevelFromExp(p.st.exp);
			p.loaded = true;

			// 1024, not 512: an escaped 127-char name alone can reach 254 bytes,
			// which left this query 8 bytes short of silent truncation.
			char q2[1024];
			V_snprintf(q2, sizeof(q2),
				"UPDATE `%s` SET `online` = %i, `lastconnect` = %lld, `steamid64` = %llu, `name` = '%s', `rank` = %i "
				"WHERE `steam` = '%s';",
				g_Cfg.tableName, g_Cfg.serverId, (long long)time(nullptr),
				(unsigned long long)p.steam64, DB_Escape(p.name).c_str(), p.level, p.steamId);
			DB_Query(q2);
		}
		else
		{
			int startExp = g_Cfg.typeStatistics ? 1000 : g_Cfg.startPoints;
			p.st = PlayerStats{};
			p.st.exp = startExp;
			p.level = LevelFromExp(startExp);
			p.dbPlaytime = 0;
			p.loaded = true;
			g_iDBCountPlayers++;

			char q2[768];
			V_snprintf(q2, sizeof(q2),
				"INSERT INTO `%s` (`steam`, `steamid64`, `name`, `value`, `rank`, `lastconnect`, `online`) "
				"VALUES ('%s', %llu, '%s', %i, %i, %lld, %i) "
				"ON DUPLICATE KEY UPDATE `steamid64` = VALUES(`steamid64`), `online` = VALUES(`online`);",
				g_Cfg.tableName, p.steamId, (unsigned long long)p.steam64,
				DB_Escape(p.name).c_str(), startExp, p.level, (long long)time(nullptr), g_Cfg.serverId);
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
		"`headshots`, `assists`, `round_win`, `round_lose`, `playtime`, `lastconnect`, `online`, `reset_cooldown`) "
		"VALUES ('%s', %llu, '%s', %i, %i, %i, %i, %i, %i, %i, %i, %i, %i, %lld, %lld, %i, %lld) "
		"ON DUPLICATE KEY UPDATE "
		"`steamid64` = VALUES(`steamid64`), `name` = VALUES(`name`), `value` = VALUES(`value`), "
		"`rank` = VALUES(`rank`), `kills` = VALUES(`kills`), `deaths` = VALUES(`deaths`), "
		"`shoots` = VALUES(`shoots`), `hits` = VALUES(`hits`), `headshots` = VALUES(`headshots`), "
		"`assists` = VALUES(`assists`), `round_win` = VALUES(`round_win`), `round_lose` = VALUES(`round_lose`), "
		"`playtime` = VALUES(`playtime`), `lastconnect` = VALUES(`lastconnect`), `online` = VALUES(`online`), "
		"`reset_cooldown` = VALUES(`reset_cooldown`);",
		g_Cfg.tableName, p.steamId, (unsigned long long)p.steam64, DB_Escape(p.name).c_str(),
		p.st.exp, p.level, p.st.kills, p.st.deaths, p.st.shoots, p.st.hits,
		p.st.headshots, p.st.assists, p.st.roundWin, p.st.roundLose,
		(long long)totalPlaytime, (long long)now,
		disconnect ? 0 : g_Cfg.serverId, (long long)p.resetCooldownUntil);

	DB_Query(q);

	if (disconnect)
	{
		p.Reset();
	}
	else
	{
		// keep in-memory playtime baseline in sync with what we just wrote
		p.dbPlaytime = totalPlaytime;
		p.sessionStart = now;
		RefreshTopPositions(iSlot);
	}
}

void ResetPlayerStats(int iSlot)
{
	PlayerInfo& p = g_Players[iSlot];
	if (!p.loaded)
		return;

	time_t cooldown = p.resetCooldownUntil;
	p.st = PlayerStats{};
	p.st.exp = g_Cfg.typeStatistics ? 1000 : g_Cfg.startPoints;
	p.sess = PlayerStats{};
	p.dbPlaytime = 0;
	p.sessionStart = time(nullptr);
	p.connectTime = p.sessionStart; // stats wiped -> the session starts over too
	p.killStreak = 0;
	p.roundExp = 0;
	p.resetCooldownUntil = cooldown;

	CheckRank(iSlot, false);
	SavePlayer(iSlot);
}

// ---------------------------------------------------------------------------
// Bootstrap: schema creation + legacy migration (the `online` bug fix)
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

void DB_Bootstrap()
{
	// Full schema in a single CREATE — never a blind ALTER afterwards.
	// (Pisex crashed on restart: CREATE already had `online`, then
	//  an unconditional ALTER ADD COLUMN `online` returned error 1060.)
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
		"`lastconnect` bigint NOT NULL DEFAULT 0, "
		"`online` int NOT NULL DEFAULT 0, "
		"`reset_cooldown` bigint NOT NULL DEFAULT 0, "
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

		// Legacy Pisex/SourcePawn tables may miss these columns.
		char alter[256];
		V_snprintf(alter, sizeof(alter), "ALTER TABLE `%s` ADD COLUMN `steamid64` bigint unsigned NOT NULL DEFAULT 0, ADD KEY `idx_steamid64` (`steamid64`);", g_Cfg.tableName);
		MigrateColumn("steamid64", alter);
		V_snprintf(alter, sizeof(alter), "ALTER TABLE `%s` ADD COLUMN `online` int NOT NULL DEFAULT 0;", g_Cfg.tableName);
		MigrateColumn("online", alter);
		V_snprintf(alter, sizeof(alter), "ALTER TABLE `%s` ADD COLUMN `reset_cooldown` bigint NOT NULL DEFAULT 0;", g_Cfg.tableName);
		MigrateColumn("reset_cooldown", alter);

		char q2[512];
		V_snprintf(q2, sizeof(q2), "UPDATE `%s` SET `online` = 0 WHERE `online` = %i;", g_Cfg.tableName, g_Cfg.serverId);
		DB_Query(q2);

		if (g_Cfg.cleanDbDays > 0)
		{
			V_snprintf(q2, sizeof(q2), "UPDATE `%s` SET `lastconnect` = 0 WHERE `lastconnect` AND `lastconnect` < %lld;",
				g_Cfg.tableName, (long long)(time(nullptr) - (int64_t)g_Cfg.cleanDbDays * 86400));
			DB_Query(q2);
		}

		V_snprintf(q2, sizeof(q2), "SELECT COUNT(`steam`) FROM `%s` WHERE `lastconnect`;", g_Cfg.tableName);
		DB_Query(q2, [](const DBResult& r2) {
			if (r2.ok && r2.RowCount())
				g_iDBCountPlayers = r2.GetInt(0, 0);

			g_bCoreReady = true;
			LR_Log("core is ready (players in db: %i)", g_iDBCountPlayers);
			ApiFireCoreReady();

			// plugin loaded mid-map: pick up players who are already here
			for (int i = 0; i < LR_MAXPLAYERS; i++)
			{
				if (g_Players[i].steam64 && !g_Players[i].loaded)
					LoadPlayer(i, g_Players[i].steam64);
			}
		});
	});
}
