#include "menu.h"
#include "menu_layout.h"
#include "lr_core.h"
#include "chat.h"
#include "commands.h"
#include "db.h"
#include "players.h"

#include <string>
#include <vector>

enum class MenuScreen : uint8_t
{
	None = 0,
	Main,
	AllRanks,
	TopExp,
	TopTime,
	Session,
	MyStats,
};

struct TopRow
{
	char name[128];
	int  level = 0;
	int  exp = 0;
	int64_t playtimeSec = 0;
};

struct MenuState
{
	MenuScreen screen = MenuScreen::None;
	MenuScreen backScreen = MenuScreen::None;
	int  page = 0;
	float until = 0.0f;
	float nextRefresh = 0.0f;
	uint64_t steam64 = 0;
	LRMenuLayout layout = LR_MENU_HTML;
	std::vector<TopRow> topRows;
	char html[4096];
};

static MenuState s_Menu[LR_MAXPLAYERS];

static const float kMenuDuration = 120.0f;
static const float kMenuRefresh = 0.02f;
// Center HUD: title + 4 items + nav (5 items clips title/footer).
static const int kListItemsPerPage = 4;
static const int kMyStatsPages = 2;

static int ClampPage(int page, int totalPages)
{
	if (totalPages < 1)
		totalPages = 1;
	if (page >= totalPages)
		page = totalPages - 1;
	if (page < 0)
		page = 0;
	return page;
}

static int TotalPages(int itemCount, int perPage)
{
	if (itemCount < 1)
		return 1;
	return (itemCount + perPage - 1) / perPage;
}

// Same hex + fontSize-m as VIP CenterHtml menus (named colors → plain white on current CS2).
static constexpr const char* kColTitle  = "#cccc33";
static constexpr const char* kColAction = "#88ff88";
static constexpr const char* kColInfo   = "#aaaaaa";
static constexpr const char* kColWhite  = "#ffffff";
static constexpr const char* kColNav    = "#cccc33";
static constexpr const char* kColClose  = "#ff8888";

static void AppendFont(std::string& html, const char* color, const char* text)
{
	html += "<font color='";
	html += color;
	html += "' class='fontSize-m'>";
	html += text;
	html += "</font>";
}

// Навигация в одну строку: !7 | !8 | !9
static void AppendNavFooter(std::string& html, int currentPage, int totalPages,
	bool showBack, bool hideBackOnFirstPage = false, const char* closeLabel = "Закрыть")
{
	bool first = true;

	auto sepBefore = [&]() {
		if (!first)
		{
			html += " ";
			AppendFont(html, kColInfo, "|");
			html += " ";
		}
		first = false;
	};

	if (showBack && (!hideBackOnFirstPage || currentPage > 0))
	{
		sepBefore();
		AppendFont(html, kColNav, "!7 Назад");
	}

	if (totalPages > 1 && currentPage + 1 < totalPages)
	{
		sepBefore();
		AppendFont(html, kColNav, "!8 Вперёд");
	}

	sepBefore();
	html += "<font color='";
	html += kColClose;
	html += "' class='fontSize-m'>!9 ";
	html += closeLabel;
	html += "</font>";
}

static void HtmlEscapeShort(const char* in, char* out, size_t outSize)
{
	HtmlEscape(in, out, outSize);
}

static void MenuBumpTimeout(int iSlot)
{
	if (iSlot < 0 || iSlot >= LR_MAXPLAYERS)
		return;
	MenuState& m = s_Menu[iSlot];
	if (m.screen == MenuScreen::None)
		return;

	CGlobalVars* gv = GetGlobals();
	if (!gv)
		return;

	m.until = gv->curtime + kMenuDuration;
}

static void MenuClose(int iSlot)
{
	if (iSlot < 0 || iSlot >= LR_MAXPLAYERS)
		return;
	MenuLayout_CloseWasd(iSlot);
	s_Menu[iSlot] = MenuState();
	LRCenterStop(iSlot);
}

void Menu_Close(int iSlot)
{
	MenuClose(iSlot);
}

bool Menu_IsWasdActive(int iSlot)
{
	return MenuLayout_IsWasdActive(iSlot);
}

static void MenuShow(int iSlot, const char* html)
{
	if (iSlot < 0 || iSlot >= LR_MAXPLAYERS || !html)
		return;

	CGlobalVars* gv = GetGlobals();
	float now = gv ? gv->curtime : 0.0f;

	LRMenuLayout layout = MenuLayout_GetPlayerType(iSlot);
	s_Menu[iSlot].layout = layout;
	s_Menu[iSlot].until = now + kMenuDuration;
	s_Menu[iSlot].nextRefresh = now;
	s_Menu[iSlot].steam64 = g_Players[iSlot].steam64;

	switch (layout)
	{
		case LR_MENU_CHAT:
			MenuLayout_CloseWasd(iSlot);
			LRCenterStop(iSlot);
			s_Menu[iSlot].html[0] = '\0';
			MenuLayout_ShowChatMenu(iSlot, html);
			return;

		case LR_MENU_WASD:
		{
			char wrapped[4096];
			LRWrapCenterHtml(wrapped, sizeof(wrapped), html, "");
			V_strncpy(s_Menu[iSlot].html, wrapped, sizeof(s_Menu[iSlot].html));
			MenuLayout_OpenWasd(iSlot, html, s_Menu[iSlot].until);
			return;
		}

		default:
			MenuLayout_CloseWasd(iSlot);
			break;
	}

	char wrapped[4096];
	LRWrapCenterHtml(wrapped, sizeof(wrapped), html, "");
	V_strncpy(s_Menu[iSlot].html, wrapped, sizeof(s_Menu[iSlot].html));
	LRCenterHtml(iSlot, wrapped, kMenuDuration);
}

static void AppendTitle(std::string& html, const char* title)
{
	AppendFont(html, kColTitle, title);
	html += "<br>";
}

// Одна <font>-строка + <br>. Без '\n'.
static void AppendPlainLine(std::string& html, const char* color, const char* text)
{
	AppendFont(html, color, text);
	html += "<br>";
}

static void AppendKeyLine(std::string& html, const char* color, int key, const char* text)
{
	char line[512];
	V_snprintf(line, sizeof(line), "!%i %s", key, text);
	AppendFont(html, color, line);
	html += "<br>";
}

static void AppendInfoLine(std::string& html, int key, const char* text)
{
	AppendKeyLine(html, kColInfo, key, text);
}

static void AppendActionLine(std::string& html, int key, const char* text)
{
	AppendKeyLine(html, kColAction, key, text);
}

static void AppendGreyLine(std::string& html, int key, const char* text)
{
	AppendKeyLine(html, kColInfo, key, text);
}

static void AppendWhiteLine(std::string& html, int key, const char* text)
{
	AppendKeyLine(html, kColWhite, key, text);
}

static void ShowMainMenu(int iSlot)
{
	PlayerInfo& pl = g_Players[iSlot];
	if (!pl.loaded)
	{
		LRCenterPhrase(iSlot, "NotLoaded");
		return;
	}

	int rankCount = (int)g_Cfg.ranksExp.size();
	if (pl.level < 1) pl.level = 1;
	if (pl.level > rankCount) pl.level = rankCount;

	MenuState& m = s_Menu[iSlot];
	m.page = 0;

	char stats[128];
	if (pl.level < rankCount)
		V_snprintf(stats, sizeof(stats), "Ур. %i | %i/%i | #%i/%i",
			pl.level, pl.st.exp, ExpForNextLevel(pl.level),
			pl.posTop, g_iDBCountPlayers);
	else
		V_snprintf(stats, sizeof(stats), "Ур. %i | %i | #%i/%i",
			pl.level, pl.st.exp, pl.posTop, g_iDBCountPlayers);

	std::string html;
	AppendTitle(html, "Меню рангов");
	AppendPlainLine(html, kColInfo, stats);
	AppendActionLine(html, 1, "Моя статистика");
	AppendActionLine(html, 2, "TOP 10");
	// !3 и !9 на одной строке — иначе 6-я строка HUD часто обрезается.
	{
		char line[128];
		V_snprintf(line, sizeof(line), "!3 Все ранги");
		AppendFont(html, kColAction, line);
		html += " ";
		AppendFont(html, kColInfo, "|");
		html += " ";
		AppendFont(html, kColClose, "!9 Выход");
	}

	m.screen = MenuScreen::Main;
	m.backScreen = MenuScreen::None;
	MenuShow(iSlot, html.c_str());
}

static void AppendNavFooterSimple(std::string& html, bool showBack, const char* closeLabel = "Выход")
{
	AppendNavFooter(html, 0, 1, showBack, false, closeLabel);
}

static void FetchPlaceThenShowMain(int iSlot)
{
	PlayerInfo& p = g_Players[iSlot];
	if (!p.loaded)
	{
		LRCenterPhrase(iSlot, "NotLoaded");
		return;
	}

	MenuBumpTimeout(iSlot);

	uint64_t steam64 = p.steam64;
	char q[512];
	V_snprintf(q, sizeof(q),
		"SELECT (SELECT COUNT(`steam`) FROM `%s` WHERE (`rank` > %i OR (`rank` = %i AND `value` >= %i)) AND %s);",
		g_Cfg.tableName, p.level, p.level, p.st.exp, LR_SQL_LASTCONNECT_ACTIVE);

	DB_Query(q, [iSlot, steam64, gen = PlayerGeneration(iSlot)](const DBResult& r) {
		if (g_Players[iSlot].steam64 != steam64 || !g_Players[iSlot].loaded || PlayerGeneration(iSlot) != gen)
			return;
		if (r.ok && r.RowCount())
			g_Players[iSlot].posTop = r.GetInt(0, 0);

		ShowMainMenu(iSlot);
	});
}

static void ShowMyStatsMenu(int iSlot, MenuScreen back)
{
	PlayerInfo& p = g_Players[iSlot];
	if (!p.loaded)
	{
		LRCenterPhrase(iSlot, "NotLoaded");
		return;
	}

	MenuState& m = s_Menu[iSlot];
	m.page = ClampPage(m.page, kMyStatsPages);
	int page = m.page;

	std::string html;
	AppendTitle(html, "Моя статистика");

	if (page == 0)
	{
		char timeStr[64];
		FormatPlaytimeLong(TotalPlaytime(iSlot), timeStr, sizeof(timeStr));

		char l2[160], l3[128];
		V_snprintf(l2, sizeof(l2), "Сыграно: %s", timeStr);
		V_snprintf(l3, sizeof(l3), "K: %i | D: %i | A: %i",
			p.st.kills, p.st.deaths, p.st.assists);

		AppendActionLine(html, 1, "Данные за сессию");
		AppendInfoLine(html, 2, l2);
		AppendInfoLine(html, 3, l3);
		AppendNavFooter(html, page, kMyStatsPages, back != MenuScreen::None, false, "Выход");
	}
	else
	{
		int hsPct = p.st.kills ? (p.st.headshots * 100 / p.st.kills) : 0;
		float kd = StatKD(p.st.kills, p.st.deaths);
		int accPct = p.st.shoots ? (p.st.hits * 100 / p.st.shoots) : 0;
		int rounds = p.st.roundWin + p.st.roundLose;
		int wrPct = rounds ? (p.st.roundWin * 100 / rounds) : 0;

		char l1[128], l2[96];
		V_snprintf(l1, sizeof(l1), "HS: %i (%i%%) | KDR: %.2f", p.st.headshots, hsPct, kd);
		V_snprintf(l2, sizeof(l2), "Точность: %i%% | WR: %i%%", accPct, wrPct);

		AppendInfoLine(html, 1, l1);
		AppendInfoLine(html, 2, l2);
		if (g_Cfg.showResetStats)
			AppendActionLine(html, 4, "Сбросить статистику");
		AppendNavFooter(html, page, kMyStatsPages, true, false, "Выход");
	}

	m.screen = MenuScreen::MyStats;
	m.backScreen = back;
	MenuShow(iSlot, html.c_str());
}

static void ShowSessionMenu(int iSlot, MenuScreen back)
{
	PlayerInfo& p = g_Players[iSlot];
	if (!p.loaded)
	{
		LRCenterPhrase(iSlot, "NotLoaded");
		return;
	}

	char expStr[16], posStr[16], timeStr[32], kdStr[16];
	V_snprintf(expStr, sizeof(expStr), "%s%i", p.sess.exp > 0 ? "+" : "", p.sess.exp);
	V_snprintf(posStr, sizeof(posStr), "%s%i", p.sessPosTop > 0 ? "+" : "", p.sessPosTop);
	FormatDuration(SessionTime(iSlot), timeStr, sizeof(timeStr));
	V_snprintf(kdStr, sizeof(kdStr), "%.2f", StatKD(p.sess.kills, p.sess.deaths));

	char l1[160], l2[160], l3[128];
	V_snprintf(l1, sizeof(l1), "Время: %s | Exp: %s | #%s", timeStr, expStr, posStr);
	V_snprintf(l2, sizeof(l2), "K/D: %s | K: %i | D: %i", kdStr, p.sess.kills, p.sess.deaths);
	V_snprintf(l3, sizeof(l3), "Хедшотов: %i", p.sess.headshots);

	std::string html;
	AppendTitle(html, back == MenuScreen::MyStats ? "Сессия" : "Данные за сессию");
	AppendInfoLine(html, 1, l1);
	AppendInfoLine(html, 2, l2);
	AppendInfoLine(html, 3, l3);
	AppendNavFooterSimple(html, back != MenuScreen::None, "Выход");

	s_Menu[iSlot].screen = MenuScreen::Session;
	s_Menu[iSlot].backScreen = back;
	s_Menu[iSlot].page = 0;
	MenuShow(iSlot, html.c_str());
}

static void ShowAllRanksMenu(int iSlot)
{
	PlayerInfo& p = g_Players[iSlot];
	int rankCount = (int)g_Cfg.ranksExp.size();
	if (rankCount < 1)
		return;

	int totalPages = TotalPages(rankCount, kListItemsPerPage);
	s_Menu[iSlot].page = ClampPage(s_Menu[iSlot].page, totalPages);
	int page = s_Menu[iSlot].page;
	int start = page * kListItemsPerPage;

	std::string html;
	AppendTitle(html, "Все ранги");

	for (int i = 0; i < kListItemsPerPage; i++)
	{
		int idx = start + i;
		if (idx >= rankCount)
			break;

		int level = idx + 1;
		char line[256];
		bool achieved = level <= p.level;

		if (level == 1)
			V_snprintf(line, sizeof(line), "Уровень %i", level);
		else
			V_snprintf(line, sizeof(line), "[%i] Уровень %i", g_Cfg.ranksExp[idx], level);

		// Достигнутый — светло-серый, ещё не взятый — белый.
		if (achieved)
			AppendGreyLine(html, i + 1, line);
		else
			AppendWhiteLine(html, i + 1, line);
	}

	AppendNavFooter(html, page, totalPages, true, false, "Выход");

	s_Menu[iSlot].screen = MenuScreen::AllRanks;
	s_Menu[iSlot].backScreen = MenuScreen::Main;
	MenuShow(iSlot, html.c_str());
}

static void ShowTopMenu(int iSlot)
{
	MenuState& m = s_Menu[iSlot];
	bool byTime = m.screen == MenuScreen::TopTime;
	int total = (int)m.topRows.size();
	int totalPages = TotalPages(total, kListItemsPerPage);
	m.page = ClampPage(m.page, totalPages);
	int start = m.page * kListItemsPerPage;

	std::string html;
	if (byTime)
		AppendTitle(html, "TOP 10 | активность");
	else
		AppendTitle(html, "TOP 10 | опыт");

	for (int i = 0; i < kListItemsPerPage; i++)
	{
		int idx = start + i;
		if (idx >= total)
			break;

		const TopRow& row = m.topRows[idx];
		char safeName[128];
		HtmlEscapeShort(row.name, safeName, sizeof(safeName));

		// AppendInfoLine prefixes !1..!4 (page keys); place is #N in the body.
		char line[512];
		if (byTime)
		{
			char timeStr[32];
			FormatDuration(row.playtimeSec, timeStr, sizeof(timeStr));
			V_snprintf(line, sizeof(line), "#%i — %s — %s",
				idx + 1, timeStr, safeName);
		}
		else
		{
			V_snprintf(line, sizeof(line), "#%i — %i — %s",
				idx + 1, row.exp, safeName);
		}
		AppendInfoLine(html, i + 1, line);
	}

	AppendNavFooter(html, m.page, totalPages, m.backScreen != MenuScreen::None, false, "Выход");
	MenuShow(iSlot, html.c_str());
}

static void FetchTopThenShow(int iSlot, bool byTime, MenuScreen back)
{
	MenuBumpTimeout(iSlot);

	char q[512];
	if (byTime)
		V_snprintf(q, sizeof(q),
			"SELECT `name`, `playtime_sec_norm` FROM `%s` WHERE %s ORDER BY `playtime_sec_norm` DESC LIMIT %i;",
			g_Cfg.tableName, LR_SQL_LASTCONNECT_ACTIVE, g_Cfg.topCount);
	else
		V_snprintf(q, sizeof(q),
			"SELECT `name`, `rank`, `value` FROM `%s` WHERE %s ORDER BY `value` DESC, `rank` DESC LIMIT %i;",
			g_Cfg.tableName, LR_SQL_LASTCONNECT_ACTIVE, g_Cfg.topCount);

	uint64_t steam64 = g_Players[iSlot].steam64;
	DB_Query(q, [iSlot, steam64, byTime, back, gen = PlayerGeneration(iSlot)](const DBResult& r) {
		if (g_Players[iSlot].steam64 != steam64 || PlayerGeneration(iSlot) != gen)
			return;

		s_Menu[iSlot].topRows.clear();
		if (r.ok)
		{
			for (int i = 0; i < r.RowCount(); i++)
			{
				TopRow row;
				V_strncpy(row.name, r.Get(i, 0), sizeof(row.name));
				if (byTime)
					row.playtimeSec = r.GetInt64(i, 1);
				else
				{
					row.level = r.GetInt(i, 1);
					row.exp = r.GetInt(i, 2);
				}
				s_Menu[iSlot].topRows.push_back(row);
			}
		}

		if (s_Menu[iSlot].topRows.empty())
		{
			LRCenterPhrase(iSlot, "NoData");
			MenuClose(iSlot);
			return;
		}

		s_Menu[iSlot].screen = byTime ? MenuScreen::TopTime : MenuScreen::TopExp;
		s_Menu[iSlot].backScreen = back;
		s_Menu[iSlot].page = 0;
		ShowTopMenu(iSlot);
	});
}

void Menu_OpenLvl(int iSlot)
{
	s_Menu[iSlot].page = 0;
	FetchPlaceThenShowMain(iSlot);
}

void Menu_OpenSession(int iSlot)
{
	ShowSessionMenu(iSlot, MenuScreen::None);
}

void Menu_OpenTop(int iSlot, bool byTime)
{
	FetchTopThenShow(iSlot, byTime, MenuScreen::None);
}

bool Menu_IsActive(int iSlot)
{
	if (iSlot < 0 || iSlot >= LR_MAXPLAYERS)
		return false;
	return s_Menu[iSlot].screen != MenuScreen::None;
}

float Menu_GetUntil(int iSlot)
{
	if (iSlot < 0 || iSlot >= LR_MAXPLAYERS)
		return 0.0f;
	return s_Menu[iSlot].until;
}

bool Menu_TryHandleKey(int iSlot, int key)
{
	if (iSlot < 0 || iSlot >= LR_MAXPLAYERS || key < 1 || key > 9)
		return false;

	MenuState& m = s_Menu[iSlot];
	if (m.screen == MenuScreen::None || g_Players[iSlot].steam64 != m.steam64)
		return false;

	// Любое нажатие !1..!8 продлевает таймер (раньше сбрасывался только при смене экрана).
	if (key != 9)
		MenuBumpTimeout(iSlot);

	if (key == 9)
	{
		MenuClose(iSlot);
		return true;
	}

	if (key == 7)
	{
		if (m.screen == MenuScreen::AllRanks || m.screen == MenuScreen::TopExp || m.screen == MenuScreen::TopTime)
		{
			if (m.page > 0)
			{
				m.page--;
				if (m.screen == MenuScreen::AllRanks)
					ShowAllRanksMenu(iSlot);
				else
					ShowTopMenu(iSlot);
			}
			else if (m.backScreen == MenuScreen::Main)
			{
				m.page = 0;
				ShowMainMenu(iSlot);
			}
			else
			{
				MenuClose(iSlot);
			}
		}
		else if (m.screen == MenuScreen::MyStats)
		{
			if (m.page > 0)
			{
				m.page--;
				ShowMyStatsMenu(iSlot, m.backScreen);
			}
			else if (m.backScreen == MenuScreen::Main)
			{
				m.page = 0;
				ShowMainMenu(iSlot);
			}
			else
			{
				MenuClose(iSlot);
			}
		}
		else if (m.backScreen == MenuScreen::Main)
		{
			m.page = 0;
			ShowMainMenu(iSlot);
		}
		else if (m.backScreen == MenuScreen::MyStats)
		{
			m.page = 0;
			ShowMyStatsMenu(iSlot, MenuScreen::Main);
		}
		else
		{
			MenuClose(iSlot);
		}
		return true;
	}

	if (key == 8)
	{
		if (m.screen == MenuScreen::MyStats)
		{
			if (m.page + 1 < kMyStatsPages)
			{
				m.page++;
				ShowMyStatsMenu(iSlot, m.backScreen);
			}
			return true;
		}

		if (m.screen == MenuScreen::AllRanks)
		{
			int rankCount = (int)g_Cfg.ranksExp.size();
			int totalPages = TotalPages(rankCount, kListItemsPerPage);
			if (m.page + 1 < totalPages)
			{
				m.page++;
				ShowAllRanksMenu(iSlot);
			}
		}
		else if (m.screen == MenuScreen::TopExp || m.screen == MenuScreen::TopTime)
		{
			int totalPages = TotalPages((int)m.topRows.size(), kListItemsPerPage);
			if (m.page + 1 < totalPages)
			{
				m.page++;
				ShowTopMenu(iSlot);
			}
		}
		return true;
	}

	switch (m.screen)
	{
		case MenuScreen::Main:
			if (key == 1)
			{
				m.page = 0;
				ShowMyStatsMenu(iSlot, MenuScreen::Main);
			}
			else if (key == 2)
				FetchTopThenShow(iSlot, false, MenuScreen::Main);
			else if (key == 3)
			{
				m.page = 0;
				ShowAllRanksMenu(iSlot);
			}
			break;

		case MenuScreen::MyStats:
			if (m.page == 0 && key == 1)
				ShowSessionMenu(iSlot, MenuScreen::MyStats);
			else if (m.page == 1 && key == 4 && g_Cfg.showResetStats)
			{
				MenuClose(iSlot);
				Commands_RequestResetStats(iSlot);
			}
			break;

		case MenuScreen::AllRanks:
			break;

		case MenuScreen::TopExp:
		case MenuScreen::TopTime:
			break;

		case MenuScreen::Session:
			break;

		default:
			break;
	}

	return true;
}

void Menu_OnGameFrame()
{
	MenuLayout_PollWasdMenus();

	CGlobalVars* gv = GetGlobals();
	if (!gv)
		return;

	float now = gv->curtime;
	for (int i = 0; i < LR_MAXPLAYERS; i++)
	{
		MenuState& m = s_Menu[i];
		if (m.screen == MenuScreen::None)
			continue;

		if (now >= m.until || !g_Players[i].steam64 || g_Players[i].steam64 != m.steam64)
		{
			MenuClose(i);
			continue;
		}

		if (m.layout != LR_MENU_HTML)
			continue;

		if (now >= m.nextRefresh)
		{
			LRCenterHtml(i, m.html, m.until - now);
			m.nextRefresh = now + kMenuRefresh;
		}
	}
}

void Menu_OnDisconnect(int iSlot)
{
	MenuClose(iSlot);
}
