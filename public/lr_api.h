// Public Metamod interface of lr_core for other C++ plugins.
// Query via: ismm->MetaFactory(LRCORE_INTERFACE, &ret, NULL) after AllPluginsLoaded.
#pragma once

#include <cstdint>
#include <functional>
#include <ISmmAPI.h>

#define LRCORE_INTERFACE "LRCoreApi002"

// Everything the scoreboard/website/other plugins usually want, in one read.
// Totals are lifetime (DB + current session); `sess*` are this-session deltas.
struct LRPlayerStats
{
	uint64_t steam64;
	char     steamId[24];   // STEAM_1:X:Y
	char     name[128];

	int level;
	int exp;

	int kills;
	int deaths;
	int headshots;
	int assists;
	int shoots;
	int hits;
	int roundWin;
	int roundLose;
	float kd;               // kills / max(deaths, 1)

	int posTop;             // place in the top by exp (1-based, 0 = unknown)
	int posTopTime;         // place in the top by playtime
	int totalPlayers;       // players in the DB (denominator for posTop)

	int64_t playtime;       // lifetime seconds, includes the running session
	int64_t sessionTime;    // seconds since this player connected

	// this-session deltas
	int sessExp;
	int sessKills;
	int sessDeaths;
	int sessHeadshots;
	int sessAssists;
	float sessKD;
	int sessPosTop;         // places gained this session (+ = climbed)
};

class ILRCoreApi
{
public:
	// Core connected to DB, configs loaded, players are being tracked.
	virtual bool IsReady() = 0;

	// true if the player's stats are loaded from the DB.
	virtual bool IsPlayerLoaded(int iSlot) = 0;

	virtual int GetExp(int iSlot) = 0;
	virtual int GetLevel(int iSlot) = 0;
	virtual int GetStat(int iSlot, int statId) = 0; // statId: see LR_Stat below
	virtual uint64_t GetSteam64(int iSlot) = 0;

	// Give (positive) or take (negative) exp. Returns false if player not loaded.
	virtual bool GiveExp(int iSlot, int amount) = 0;

	// --- stats -------------------------------------------------------------
	// One-shot read of everything below. false if the player is not loaded.
	virtual bool GetPlayerStats(int iSlot, LRPlayerStats& out) = 0;

	virtual int GetPosTop(int iSlot) = 0;        // place in top by exp (0 = unknown)
	virtual int GetTotalPlayers() = 0;           // players in the DB
	virtual int GetKills(int iSlot) = 0;
	virtual int GetDeaths(int iSlot) = 0;
	virtual int GetHeadshots(int iSlot) = 0;
	virtual float GetKD(int iSlot) = 0;
	virtual int64_t GetPlaytime(int iSlot) = 0;    // lifetime seconds
	virtual int64_t GetSessionTime(int iSlot) = 0; // seconds since connect

	// Slot of an online player by SteamID64, or -1.
	virtual int FindSlot(uint64_t steam64) = 0;

	// --- events ------------------------------------------------------------
	using FnCoreReady = std::function<void()>;
	using FnLevelChanged = std::function<void(int iSlot, int newLevel, int oldLevel)>;
	using FnExpChanged = std::function<void(int iSlot, int delta, int newExp)>;
	using FnPlayerLoaded = std::function<void(int iSlot, uint64_t steam64)>;

	virtual void HookCoreReady(SourceMM::PluginId id, FnCoreReady fn) = 0;
	virtual void HookLevelChanged(SourceMM::PluginId id, FnLevelChanged fn) = 0;
	virtual void HookExpChanged(SourceMM::PluginId id, FnExpChanged fn) = 0;
	virtual void HookPlayerLoaded(SourceMM::PluginId id, FnPlayerLoaded fn) = 0;
	virtual void UnhookAll(SourceMM::PluginId id) = 0;
};

enum LR_Stat
{
	LR_STAT_KILLS = 0,
	LR_STAT_DEATHS,
	LR_STAT_SHOOTS,
	LR_STAT_HITS,
	LR_STAT_HEADSHOTS,
	LR_STAT_ASSISTS,
	LR_STAT_ROUND_WIN,
	LR_STAT_ROUND_LOSE,
	LR_STAT_PLAYTIME,     // accumulated seconds incl. current session
	LR_STAT_POS_TOP,      // place in top by exp (1-based, 0 = unknown)
	LR_STAT_POS_TOPTIME,
	LR_STAT_SESSION_TIME, // seconds since connect
	LR_STAT_COUNT
};
