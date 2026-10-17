// CounterStrikeSharp VIP ↔ lr_core: one Invoke entry (no fn-pointer table).
// MetaFactory("LRCoreCssApi002") → &LRCssApiEntry
#pragma once

#include <cstdint>

#define LRCORE_CSS_INTERFACE "LRCoreCssApi002"
#define LRCORE_CSS_ABI       2

#pragma pack(push, 1)
struct LRCssPlayerSnapshot
{
	uint64_t steam64;
	char     steamId[24];
	char     name[128];

	int32_t level;
	int32_t exp;
	int32_t expNext;

	int32_t kills;
	int32_t deaths;
	int32_t headshots;
	int32_t assists;
	int32_t shoots;
	int32_t hits;
	int32_t roundWin;
	int32_t roundLose;
	float   kd;

	int32_t posTop;
	int32_t posTopTime;
	int32_t totalPlayers;

	int64_t playtime;
	int64_t sessionTime;

	int32_t sessExp;
	int32_t sessKills;
	int32_t sessDeaths;
	int32_t sessHeadshots;
	int32_t sessAssists;
	float   sessKD;
	int32_t sessPosTop;

	int32_t showResetStats;
	int32_t resetCooldownLeft;
};

struct LRCssTopRow
{
	char    name[128];
	int32_t level;
	int32_t exp;
	int64_t playtimeSec;
};

struct LRCssRankRow
{
	int32_t level;
	int32_t expRequired;
};
#pragma pack(pop)

// op codes for Invoke(op, a, b, buf, bufBytes)
enum LRCssOp : int32_t
{
	LR_CSS_IS_READY = 1,
	LR_CSS_FIND_SLOT = 2,          // a=steam64 → slot
	LR_CSS_GET_SNAPSHOT = 3,       // a=slot, buf=LRCssPlayerSnapshot*
	LR_CSS_GET_RANK_COUNT = 4,
	LR_CSS_GET_RANK = 5,           // a=level, buf=LRCssRankRow*
	LR_CSS_GET_TOP_LIMIT = 6,
	LR_CSS_REQUEST_REFRESH_PLACE = 7, // a=slot
	LR_CSS_REQUEST_TOP = 8,        // a=byTime(0/1)
	LR_CSS_IS_TOP_READY = 9,
	LR_CSS_GET_TOP_COUNT = 10,
	LR_CSS_GET_TOP_ROW = 11,       // a=index, buf=LRCssTopRow*
	LR_CSS_REQUEST_RESET_STATS = 12, // a=slot
	LR_CSS_CENTER_HTML = 13,       // a=slot, b=durationSec, buf=UTF-8 html (bufBytes)
	LR_CSS_CENTER_STOP = 14,       // a=slot
};

using LRCssInvokeFn = int32_t (*)(int32_t op, int64_t a, int64_t b, void* buf, int32_t bufBytes);

struct LRCssApiEntry
{
	int32_t abiVersion; // LRCORE_CSS_ABI
	LRCssInvokeFn Invoke;
};
