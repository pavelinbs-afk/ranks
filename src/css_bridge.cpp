#include "css_bridge.h"
#include "lr_core.h"
#include "chat.h"
#include "commands.h"
#include "db.h"
#include "menu.h"
#include "players.h"

#include <lr_css_api.h>

#include <convar.h>

#include <cstring>
#include <ctime>
#include <string>

static_assert(sizeof(LRCssTopRow) == 144, "LRCssTopRow size mismatch");
static_assert(sizeof(LRCssPlayerSnapshot) == 272, "LRCssPlayerSnapshot size mismatch");
static_assert(sizeof(LRCssRankRow) == 8, "LRCssRankRow size mismatch");

static constexpr int kTopCap = 64;
static LRCssTopRow s_TopRows[kTopCap];
static int s_TopCount = 0;
static bool s_TopReady = false;
static bool s_TopPending = false;
static int  s_TopByTime = 0;
static uint32_t s_TopGen = 0;

// Like customrounds_tp_ready — VIP can ConVar.Find("lr_css_ready").
static CConVar<bool> g_cvLrCssReady(
	"lr_css_ready",
	FCVAR_NONE,
	"1 if lr_css_menu ConCommand is registered",
	true);

static void CopyCString(char* dst, size_t dstSize, const char* src)
{
	if (!dst || dstSize == 0)
		return;
	if (!src)
		src = "";
	std::strncpy(dst, src, dstSize - 1);
	dst[dstSize - 1] = '\0';
}

static int32_t FillSnapshot(int32_t iSlot, LRCssPlayerSnapshot* out)
{
	if (!out || iSlot < 0 || iSlot >= LR_MAXPLAYERS || !g_Players[iSlot].loaded)
		return 0;

	PlayerInfo& p = g_Players[iSlot];
	std::memset(out, 0, sizeof(*out));

	out->steam64 = p.steam64;
	CopyCString(out->steamId, sizeof(out->steamId), p.steamId);
	CopyCString(out->name, sizeof(out->name), p.name);

	out->level = p.level;
	out->exp = p.st.exp;
	out->expNext = ExpForNextLevel(p.level);

	out->kills = p.st.kills;
	out->deaths = p.st.deaths;
	out->headshots = p.st.headshots;
	out->assists = p.st.assists;
	out->shoots = p.st.shoots;
	out->hits = p.st.hits;
	out->roundWin = p.st.roundWin;
	out->roundLose = p.st.roundLose;
	out->kd = StatKD(p.st.kills, p.st.deaths);

	out->posTop = p.posTop;
	out->posTopTime = p.posTopTime;
	out->totalPlayers = g_iDBCountPlayers;

	out->playtime = TotalPlaytime(iSlot);
	out->sessionTime = SessionTime(iSlot);

	out->sessExp = p.sess.exp;
	out->sessKills = p.sess.kills;
	out->sessDeaths = p.sess.deaths;
	out->sessHeadshots = p.sess.headshots;
	out->sessAssists = p.sess.assists;
	out->sessKD = StatKD(p.sess.kills, p.sess.deaths);
	out->sessPosTop = p.sessPosTop;

	out->showResetStats = g_Cfg.showResetStats ? 1 : 0;
	time_t now = time(nullptr);
	out->resetCooldownLeft = (p.resetCooldownUntil > now)
		? (int32_t)(p.resetCooldownUntil - now)
		: 0;
	return 1;
}

static int32_t TopLimit()
{
	int n = g_Cfg.topCount > 0 ? g_Cfg.topCount : 10;
	if (n > kTopCap)
		n = kTopCap;
	return n;
}

static void DoRequestTop(int32_t byTime)
{
	s_TopByTime = byTime ? 1 : 0;
	s_TopReady = false;
	s_TopPending = true;
	s_TopCount = 0;
	std::memset(s_TopRows, 0, sizeof(s_TopRows));
	const uint32_t gen = ++s_TopGen;

	const int limit = TopLimit();
	char q[512];
	if (s_TopByTime)
		V_snprintf(q, sizeof(q),
			"SELECT `name`, IFNULL(`playtime_sec_norm`,0) FROM `%s` WHERE %s "
			"ORDER BY `playtime_sec_norm` DESC LIMIT %i;",
			g_Cfg.tableName, LR_SQL_LASTCONNECT_ACTIVE, limit);
	else
		V_snprintf(q, sizeof(q),
			"SELECT `name`, IFNULL(`rank`,0), IFNULL(`value`,0) FROM `%s` WHERE %s "
			"ORDER BY `value` DESC, `rank` DESC LIMIT %i;",
			g_Cfg.tableName, LR_SQL_LASTCONNECT_ACTIVE, limit);

	ConMsg("[LR] CssApi RequestTop byTime=%d limit=%d (sizeof topRow=%zu snap=%zu)\n",
		s_TopByTime, limit, sizeof(LRCssTopRow), sizeof(LRCssPlayerSnapshot));

	DB_Query(q, [gen](const DBResult& r) {
		if (gen != s_TopGen)
			return;

		s_TopCount = 0;
		std::memset(s_TopRows, 0, sizeof(s_TopRows));

		if (!r.ok)
		{
			Warning("[LR] CssApi top query failed: %s\n", r.error.c_str());
			s_TopPending = false;
			s_TopReady = true;
			return;
		}

		const int n = r.RowCount() < kTopCap ? r.RowCount() : kTopCap;
		for (int i = 0; i < n; i++)
		{
			LRCssTopRow& row = s_TopRows[i];
			CopyCString(row.name, sizeof(row.name), r.Get(i, 0));
			if (s_TopByTime)
				row.playtimeSec = r.GetInt64(i, 1);
			else
			{
				row.level = r.GetInt(i, 1);
				row.exp = r.GetInt(i, 2);
			}
			s_TopCount++;
		}

		s_TopPending = false;
		s_TopReady = true;
		ConMsg("[LR] CssApi top ready count=%d\n", s_TopCount);
	});
}

extern "C" int32_t LRCss_Invoke(int32_t op, int64_t a, int64_t b, void* buf, int32_t bufBytes)
{
	(void)b;

	switch (op)
	{
	case LR_CSS_IS_READY:
		return g_bCoreReady ? 1 : 0;

	case LR_CSS_FIND_SLOT:
		return FindSlotBySteam64((uint64_t)a);

	case LR_CSS_GET_SNAPSHOT:
		if (!buf || bufBytes < (int)sizeof(LRCssPlayerSnapshot))
			return 0;
		return FillSnapshot((int32_t)a, reinterpret_cast<LRCssPlayerSnapshot*>(buf));

	case LR_CSS_GET_RANK_COUNT:
		return (int32_t)g_Cfg.ranksExp.size();

	case LR_CSS_GET_RANK:
	{
		if (!buf || bufBytes < (int)sizeof(LRCssRankRow))
			return 0;
		const int level = (int)a;
		if (level < 1 || level > (int)g_Cfg.ranksExp.size())
			return 0;
		auto* out = reinterpret_cast<LRCssRankRow*>(buf);
		std::memset(out, 0, sizeof(*out));
		out->level = level;
		out->expRequired = g_Cfg.ranksExp[level - 1];
		return 1;
	}

	case LR_CSS_GET_TOP_LIMIT:
		return TopLimit();

	case LR_CSS_REQUEST_REFRESH_PLACE:
	{
		const int slot = (int)a;
		if (slot >= 0 && slot < LR_MAXPLAYERS && g_Players[slot].loaded)
			RefreshTopPositions(slot);
		return 1;
	}

	case LR_CSS_REQUEST_TOP:
		// Disabled: VIP↔native top-row marshal segfaults after ready.
		// Use in-process Menu_OpenTop / chat !top instead.
		(void)a;
		Warning("[LR] CssApi RequestTop disabled — use !top / lr_css_menu\n");
		return 0;

	case LR_CSS_IS_TOP_READY:
		return (s_TopReady && !s_TopPending) ? 1 : 0;

	case LR_CSS_GET_TOP_COUNT:
		return s_TopCount;

	case LR_CSS_GET_TOP_ROW:
	{
		if (!buf || bufBytes < (int)sizeof(LRCssTopRow))
			return 0;
		const int index = (int)a;
		if (index < 0 || index >= s_TopCount || index >= kTopCap)
			return 0;
		auto* out = reinterpret_cast<LRCssTopRow*>(buf);
		std::memset(out, 0, sizeof(*out));
		CopyCString(out->name, sizeof(out->name), s_TopRows[index].name);
		out->level = s_TopRows[index].level;
		out->exp = s_TopRows[index].exp;
		out->playtimeSec = s_TopRows[index].playtimeSec;
		return 1;
	}

	case LR_CSS_REQUEST_RESET_STATS:
		Commands_RequestResetStats((int)a);
		return 1;

	case LR_CSS_CENTER_HTML:
	{
		const int slot = (int)a;
		if (slot < 0 || slot >= LR_MAXPLAYERS || !buf || bufBytes < 1)
			return 0;
		const int n = bufBytes > 2047 ? 2047 : bufBytes;
		std::string html(static_cast<const char*>(buf), static_cast<size_t>(n));
		float dur = b > 0 ? (float)b : 120.0f;
		if (dur > 3600.0f)
			dur = 3600.0f;
		LRCenterHtml(slot, html.c_str(), dur);
		return 1;
	}

	case LR_CSS_CENTER_STOP:
	{
		const int slot = (int)a;
		if (slot < 0 || slot >= LR_MAXPLAYERS)
			return 0;
		LRCenterStop(slot);
		return 1;
	}

	default:
		return 0;
	}
}

static LRCssApiEntry g_CssEntry = {
	LRCORE_CSS_ABI,
	&LRCss_Invoke,
};

void* CssBridge_GetApiTable()
{
	return &g_CssEntry;
}

void CssBridge_OnGameFrame()
{
}

// ---------------------------------------------------------------------------
// CustomRounds-style: VIP → Server.ExecuteCommand("lr_css_menu <slot> <op>")
// Menus / HUD stay entirely in C++ (no POD marshal, no PrintToCenterHtml).
// ---------------------------------------------------------------------------

static void Cmd_LrCssMenu(const CCommand& args)
{
	if (args.ArgC() < 3)
	{
		Msg("Usage: lr_css_menu <slot> <lvl|top|toptime|session|close|resetstats>\n");
		return;
	}

	const int slot = atoi(args.Arg(1));
	if (slot < 0 || slot >= LR_MAXPLAYERS)
	{
		Msg("[LR] lr_css_menu: bad slot %d\n", slot);
		return;
	}

	const char* op = args.Arg(2);
	if (!op || !*op)
		return;

	if (!V_stricmp(op, "close"))
	{
		Menu_Close(slot);
		ConMsg("[LR] lr_css_menu slot=%d close\n", slot);
		return;
	}

	if (!g_Players[slot].loaded)
	{
		LRPrintPhrase(slot, "NotLoaded");
		return;
	}

	if (!V_stricmp(op, "lvl") || !V_stricmp(op, "level"))
	{
		Menu_OpenLvl(slot);
		ConMsg("[LR] lr_css_menu slot=%d lvl\n", slot);
	}
	else if (!V_stricmp(op, "top"))
	{
		Menu_OpenTop(slot, false);
		ConMsg("[LR] lr_css_menu slot=%d top\n", slot);
	}
	else if (!V_stricmp(op, "toptime"))
	{
		Menu_OpenTop(slot, true);
		ConMsg("[LR] lr_css_menu slot=%d toptime\n", slot);
	}
	else if (!V_stricmp(op, "session"))
	{
		Menu_OpenSession(slot);
		ConMsg("[LR] lr_css_menu slot=%d session\n", slot);
	}
	else if (!V_stricmp(op, "resetstats"))
	{
		Commands_RequestResetStats(slot);
		ConMsg("[LR] lr_css_menu slot=%d resetstats\n", slot);
	}
	else
	{
		Msg("Usage: lr_css_menu <slot> <lvl|top|toptime|session|close|resetstats>\n");
	}
}

void CssBridge_RegisterCommands()
{
	(void)g_cvLrCssReady;
	new ConCommand("lr_css_menu", Cmd_LrCssMenu,
		"lr_css_menu <slot> <lvl|top|toptime|session|close|resetstats>",
		FCVAR_GAMEDLL);
}
