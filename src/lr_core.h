#pragma once

#include <ISmmPlugin.h>
#include <tier0/dbg.h>
#include <tier1/strtools.h>
#include <eiface.h>
#include <iserver.h>
#include <igameevents.h>
#include <igameeventsystem.h>
#include <icvar.h>
#include <entity2/entitysystem.h>
#include <networksystem/inetworkmessages.h>

#include <cstdint>
#include <ctime>
#include <string>
#include <vector>

#include "lr_api.h"

#define LR_MAXPLAYERS 64

class LRCorePlugin final : public ISmmPlugin, public IMetamodListener
{
public:
	bool Load(PluginId id, ISmmAPI* ismm, char* error, size_t maxlen, bool late) override;
	bool Unload(char* error, size_t maxlen) override;
	void AllPluginsLoaded() override;
	void* OnMetamodQuery(const char* iface, int* ret) override;

	const char* GetAuthor() override		{ return "lr_core"; }
	const char* GetName() override			{ return "[LR] Core"; }
	const char* GetDescription() override	{ return "Levels Ranks core (standalone)"; }
	const char* GetURL() override			{ return ""; }
	const char* GetLicense() override		{ return "GPL"; }
	const char* GetVersion() override		{ return "1.0.0"; }
	const char* GetDate() override			{ return __DATE__; }
	const char* GetLogTag() override		{ return "LR"; }

public: // hooks
	void Hook_GameFrame(bool simulating, bool bFirstTick, bool bLastTick);
	void Hook_ClientPutInServer(CPlayerSlot slot, char const* pszName, int type, uint64 xuid);
	void Hook_ClientDisconnect(CPlayerSlot slot, ENetworkDisconnectionReason reason, const char* pszName, uint64 xuid, const char* pszNetworkID);
	void Hook_DispatchConCommand(ConCommandRef cmd, const CCommandContext& ctx, const CCommand& args);
	void Hook_StartupServer(const GameSessionConfiguration_t& config, ISource2WorldSession*, const char*);
	int  Hook_LoadEventsFromFile(const char* filename, bool bSearchAll);
	void Hook_EntitySystemSpawn(int nCount, const EntitySpawnInfo_t* pInfo);
};

// ---------------------------------------------------------------------------
// Settings
// ---------------------------------------------------------------------------

struct LRSettings
{
	char tableName[64];
	char language[16];
	int  serverId;
	int  typeStatistics;   // 0 funded, 1 rating extended, 2 rating simple
	int  minPlayers;
	bool showResetStats;
	int  resetCooldown;
	int  showUsualMessage; // 0/1/2
	bool showSpawnMessage;
	bool showLevelUp, showLevelDown, showAllLevelUp, showAllLevelDown;
	bool showRankMessage;
	int  topCount;
	int  startPoints;
	bool giveExpRoundEnd;
	bool blockWarmup;
	bool allAgainstAll;
	int  cleanDbDays;
	int  saveMode;

	// exp amounts (positive numbers; sign applied at call sites)
	int expKill, expKillBot, expDeath, expDeathBot;
	int killCoeffPct; // 50..150
	int expHeadshot, expAssist, expSuicide, expTeamkill;
	int expRoundWin, expRoundLose, expMvp;
	int expBombPlant, expBombDefuse, expBombDrop, expBombPickup;
	int expHostageKill, expHostageRescue;
	int bonus[11];

	// periodic exp for time spent in game (independent of player count)
	int timeExpAmount;     // exp per interval (0 = feature off)
	int timeExpInterval;   // seconds between grants

	// ranks (level thresholds, ascending; level i+1 requires ranksExp[i])
	std::vector<int> ranksExp;
	std::vector<std::string> ranksKey; // phrase keys ("rank_1", ...)
};

struct LRTabSettings
{
	bool enabled;
	int  mode;                 // 0 = competitive rank column (default), 1 = persona level badge
	int  wins;
	int  type;                 // rank type for mapped levels (competitive mode)
	std::vector<int> values;   // values[level-1] = ranking value / persona badge level
	int  aboveType;
	int  aboveValue;
	bool aboveUseExp;

	// Workshop skillgroup icons (MultiAddonManager)
	char workshopId[32];       // e.g. "3772685382"; empty = do not touch MAM
	float iconsDelay;          // seconds after load before writing custom skillgroup IDs
	float iconsRefresh;        // seconds of forced re-dirty after first apply (0 = off)
	float revealInterval;      // periodic ServerRankRevealAll to all players (0 = off)
};

// ---------------------------------------------------------------------------
// Per-player state
// ---------------------------------------------------------------------------

struct PlayerStats
{
	int exp = 0;
	int kills = 0, deaths = 0, shoots = 0, hits = 0;
	int headshots = 0, assists = 0, roundWin = 0, roundLose = 0;
};

struct PlayerInfo
{
	bool  loaded = false;      // stats fetched from DB
	bool  haveBomb = false;
	uint64_t steam64 = 0;
	char  steamId[24] = {0};   // STEAM_1:X:Y
	char  name[128] = {0};

	int   level = 0;           // 1-based rank index, 0 = none yet
	PlayerStats st;            // persistent totals
	PlayerStats sess;          // this-session deltas
	int64_t dbPlaytime = 0;    // seconds accumulated in DB
	time_t  sessionStart = 0;  // baseline for playtime accumulation; SavePlayer moves it
	time_t  connectTime = 0;   // set once on connect, never moved: the session clock

	int posTop = 0, posTopTime = 0;
	int sessPosTop = 0, sessPosTopTime = 0;

	int killStreak = 0;
	int roundExp = 0;
	time_t timeExpAt = 0;      // next periodic time-exp grant (0 = arm on first frame)
	time_t resetCooldownUntil = 0;

	uint64_t oldButtons = 0;   // scoreboard button edge detection
	bool revealSent = false;   // ServerRankRevealAll sent after load
	int  tabOverride = 0;      // lr_tab_test preview badge (0 = off)

	// Workshop SVG icons: wait for MultiAddonManager reconnect/mount, then
	// force-dirty fields for a short window so Panorama retries the resource.
	float tabIconsAt = 0.0f;       // curtime when custom skillgroup IDs may be written
	float tabRefreshUntil = 0.0f;  // curtime until which we keep force-dirtying
	bool  tabIconsApplied = false;

	void Reset() { *this = PlayerInfo(); }
};

// ---------------------------------------------------------------------------
// Globals
// ---------------------------------------------------------------------------

extern LRCorePlugin g_LRPlugin;

extern IVEngineServer2* g_pEngine;
extern ISource2Server* g_pServer;
extern IServerGameClients* g_pGameClients;
extern IGameEventSystem* g_pGameEventSystem;
extern IGameEventManager2* g_pGameEventManager;
extern INetworkServerService* g_pNetServerService;
extern CGameEntitySystem* g_pGameEntitySystem;

struct DBConfig;
extern DBConfig g_DBConfig;

extern LRSettings g_Cfg;
extern LRTabSettings g_TabCfg;
extern PlayerInfo g_Players[LR_MAXPLAYERS];

extern bool g_bCoreReady;        // DB connected + table ready
extern bool g_bAllowStatistic;   // enough players / not warmup
extern int  g_iDBCountPlayers;

inline CGlobalVars* GetGlobals()
{
	return g_pEngine ? g_pEngine->GetServerGlobals() : nullptr;
}

// entity index for player slot N is N+1
CEntityInstance* GetControllerBySlot(int iSlot);

// Bumped by every LoadPlayer. A DB callback captures the value it started with
// and bails if it no longer matches: comparing steam64 alone is not enough,
// because a player can disconnect and reconnect into the same slot while their
// own query is still in flight, and then the stale result would be applied.
uint32_t PlayerGeneration(int iSlot);

// player is a real, fully authenticated human with loaded stats
bool IsPlayerReady(int iSlot);

int LevelFromExp(int exp);
void CheckRank(int iSlot, bool bNotify = true);

// Total playtime in seconds: what is banked in the DB plus the time accrued
// since the last save (sessionStart moves on every save, connectTime does not).
int64_t TotalPlaytime(int iSlot);

// Seconds since the player connected (0 if not tracked yet).
int64_t SessionTime(int iSlot);

float StatKD(int kills, int deaths);

// "5ч 07м" / "12м 30с" — human duration for chat.
void FormatDuration(int64_t seconds, char* out, int outSize);

// Central exp mutation: applies floors, session stats, rank check, chat message.
// Returns true if the change was applied (player ready + stats allowed or bypassed).
bool ChangeExp(int iSlot, int delta, const char* phraseKey, bool bypassRestrictions = false);

void SavePlayer(int iSlot, bool disconnect = false);

// flushCurrent: write the in-memory stats out before wiping them. Wanted when
// the same player is being re-loaded over a live session (map change), and
// explicitly *not* wanted after lr_reset, where the old values must not come
// back into the table that was just cleared.
void LoadPlayer(int iSlot, uint64_t steam64, bool flushCurrent = true);
void ResetPlayerStats(int iSlot);

// Periodic exp for time in game; call once per game frame.
void GiveTimeExp();

// API hook broadcast (implemented in lr_core.cpp)
void ApiFireCoreReady();
void ApiFireLevelChanged(int iSlot, int newLevel, int oldLevel);
void ApiFireExpChanged(int iSlot, int delta, int newExp);
void ApiFirePlayerLoaded(int iSlot, uint64_t steam64);

void LR_Log(const char* fmt, ...);
