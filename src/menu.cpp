#include "menu.h"
#include "lr_core.h"
#include "chat.h"
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
	Rank,
};

struct TopRow
{
	char name[128];
	int  level = 0;
	int  exp = 0;
	float hours = 0.0f;
};

struct MenuState
{
	MenuScreen screen = MenuScreen::None;
	MenuScreen backScreen = MenuScreen::None;
	int  page = 0;
	float until = 0.0f;
	float nextRefresh = 0.0f;
	uint64_t steam64 = 0;
	std::vector<TopRow> topRows;
	char html[4096];
};

static MenuState s_Menu[LR_MAXPLAYERS];

static const float kMenuDuration = 45.0f;
static const float kMenuRefresh = 0.5f;
static const int kItemsPerPage = 4; // как AdminPlugin (perPage = 4)

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

// Footer как BuildCenterMenuNavFooterHtml в AdminPlugin.
static void AppendNavFooter(std::string& html, int currentPage, int totalPages,
	bool showBack, bool hideBackOnFirstPage = false, const char* closeLabel = "Закрыть")
{
	html += "<br/>";
	const char* sep = " <font color='#555555'>|</font> ";
	bool first = true;

	auto sepBefore = [&]() {
		if (!first)
			html += sep;
		first = false;
	};

	if (showBack && (!hideBackOnFirstPage || currentPage > 0))
	{
		sepBefore();
		html += "<font color='#8888ff'>!7</font> <font color='#ffffff'>Назад</font>";
	}

	if (totalPages > 1 && currentPage + 1 < totalPages)
	{
		sepBefore();
		html += "<font color='#88ccff'>!8</font> <font color='#ffffff'>Вперёд</font>";
	}

	sepBefore();
	html += "<font color='#ff8888'>!9</font> <font color='#ffffff'>";
	html += closeLabel;
	html += "</font><br/>";
}

static void HtmlEscapeShort(const char* in, char* out, size_t outSize)
{
	HtmlEscape(in, out, outSize);
}

static void MenuClose(int iSlot)
{
	if (iSlot < 0 || iSlot >= LR_MAXPLAYERS)
		return;
	s_Menu[iSlot] = MenuState();
}

static void MenuShow(int iSlot, const char* html)
{
	if (iSlot < 0 || iSlot >= LR_MAXPLAYERS || !html)
		return;

	CGlobalVars* gv = GetGlobals();
	float now = gv ? gv->curtime : 0.0f;

	V_strncpy(s_Menu[iSlot].html, html, sizeof(s_Menu[iSlot].html));
	s_Menu[iSlot].until = now + kMenuDuration;
	s_Menu[iSlot].nextRefresh = now;
	s_Menu[iSlot].steam64 = g_Players[iSlot].steam64;

	LRCenterHtml(iSlot, html, kMenuDuration);
}

static void AppendTitle(std::string& html, const char* title)
{
	html += "<font color='#ffd700'>";
	html += title;
	html += "</font><br/>";
}

static void AppendInfoLine(std::string& html, int key, const char* text)
{
	char line[512];
	V_snprintf(line, sizeof(line),
		"<font color='#aaaaaa'>[!%i]</font> <font color='#ffffff'>%s</font><br/>",
		key, text);
	html += line;
}

static void AppendActionLine(std::string& html, int key, const char* text)
{
	char line[512];
	V_snprintf(line, sizeof(line),
		"<font color='#88ff88'>[!%i]</font> <font color='#88ff88'>%s</font><br/>",
		key, text);
	html += line;
}

static void AppendNavFooterSimple(std::string& html, bool showBack)
{
	AppendNavFooter(html, 0, 1, showBack, false);
}

static void ShowRankMenu(int iSlot, int posTop, float kd)
{
	PlayerInfo& p = g_Players[iSlot];
	char safeName[128];
	HtmlEscapeShort(p.name, safeName, sizeof(safeName));

	char l1[160], l2[128], l3[128], l4[128], l5[128], l6[128];
	V_snprintf(l1, sizeof(l1), "Игрок: %s", safeName);
	V_snprintf(l2, sizeof(l2), "Место: %i / %i", posTop, g_iDBCountPlayers);
	V_snprintf(l3, sizeof(l3), "Опыт: %i", p.st.exp);
	V_snprintf(l4, sizeof(l4), "Убийств: %i", p.st.kills);
	V_snprintf(l5, sizeof(l5), "Смертей: %i", p.st.deaths);
	V_snprintf(l6, sizeof(l6), "K/D: %.2f", kd);

	std::string html;
	AppendTitle(html, "Меню рангов | Статистика");
	AppendInfoLine(html, 1, l1);
	AppendInfoLine(html, 2, l2);
	AppendInfoLine(html, 3, l3);
	AppendInfoLine(html, 4, l4);
	AppendInfoLine(html, 5, l5);
	AppendInfoLine(html, 6, l6);
	AppendNavFooterSimple(html, false);

	s_Menu[iSlot].screen = MenuScreen::Rank;
	s_Menu[iSlot].backScreen = MenuScreen::None;
	s_Menu[iSlot].page = 0;
	MenuShow(iSlot, html.c_str());
}

static void FetchPlaceThenShowMain(int iSlot)
{
	PlayerInfo& p = g_Players[iSlot];
	if (!p.loaded)
	{
		LRCenterPhrase(iSlot, "NotLoaded");
		return;
	}

	uint64_t steam64 = p.steam64;
	char q[512];
	V_snprintf(q, sizeof(q),
		"SELECT (SELECT COUNT(`steam`) FROM `%s` WHERE (`rank` > %i OR (`rank` = %i AND `value` >= %i)) AND `lastconnect`);",
		g_Cfg.tableName, p.level, p.level, p.st.exp);

	DB_Query(q, [iSlot, steam64, gen = PlayerGeneration(iSlot)](const DBResult& r) {
		if (g_Players[iSlot].steam64 != steam64 || !g_Players[iSlot].loaded || PlayerGeneration(iSlot) != gen)
			return;
		if (r.ok && r.RowCount())
			g_Players[iSlot].posTop = r.GetInt(0, 0);

		PlayerInfo& pl = g_Players[iSlot];
		int rankCount = (int)g_Cfg.ranksExp.size();
		if (pl.level < 1) pl.level = 1;
		if (pl.level > rankCount) pl.level = rankCount;

		char rankLine[128], expLine[128], placeLine[128];
		V_snprintf(rankLine, sizeof(rankLine), "Звание: Уровень %i", pl.level);

		if (pl.level < rankCount)
			V_snprintf(expLine, sizeof(expLine), "Опыт: %i / %i", pl.st.exp, ExpForNextLevel(pl.level));
		else
			V_snprintf(expLine, sizeof(expLine), "Опыт: %i", pl.st.exp);

		V_snprintf(placeLine, sizeof(placeLine), "Место: %i из %i", pl.posTop, g_iDBCountPlayers);

		std::string html;
		AppendTitle(html, "Меню рангов");
		AppendInfoLine(html, 1, rankLine);
		AppendInfoLine(html, 2, expLine);
		AppendInfoLine(html, 3, placeLine);
		AppendActionLine(html, 4, "Моя статистика");
		AppendActionLine(html, 5, "TOP игроков");
		AppendNavFooterSimple(html, false);

		s_Menu[iSlot].screen = MenuScreen::Main;
		s_Menu[iSlot].backScreen = MenuScreen::None;
		s_Menu[iSlot].page = 0;
		MenuShow(iSlot, html.c_str());
	});
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

	char l1[128], l2[128], l3[128], l4[128], l5[128], l6[128];
	V_snprintf(l1, sizeof(l1), "Время: %s", timeStr);
	V_snprintf(l2, sizeof(l2), "Опыт: %s", expStr);
	V_snprintf(l3, sizeof(l3), "Убийств: %i", p.sess.kills);
	V_snprintf(l4, sizeof(l4), "Смертей: %i", p.sess.deaths);
	V_snprintf(l5, sizeof(l5), "Хедшотов: %i", p.sess.headshots);
	V_snprintf(l6, sizeof(l6), "K/D: %s | место: %s", kdStr, posStr);

	std::string html;
	AppendTitle(html, "Меню рангов | Моя статистика");
	AppendInfoLine(html, 1, l1);
	AppendInfoLine(html, 2, l2);
	AppendInfoLine(html, 3, l3);
	AppendInfoLine(html, 4, l4);
	AppendInfoLine(html, 5, l5);
	AppendInfoLine(html, 6, l6);
	AppendNavFooterSimple(html, back != MenuScreen::None);

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

	int totalPages = TotalPages(rankCount, kItemsPerPage);
	s_Menu[iSlot].page = ClampPage(s_Menu[iSlot].page, totalPages);
	int page = s_Menu[iSlot].page;
	int start = page * kItemsPerPage;

	std::string html;
	AppendTitle(html, "Все ранги");

	for (int i = 0; i < kItemsPerPage; i++)
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
			V_snprintf(line, sizeof(line), "[%i] Уровень %i", g_Cfg.ranksExp[idx - 1], level);

		if (achieved)
		{
			char grey[512];
			V_snprintf(grey, sizeof(grey),
				"<font color='#888888'>[!%i]</font> <font color='#888888'>%s</font><br/>",
				i + 1, line);
			html += grey;
		}
		else
		{
			AppendInfoLine(html, i + 1, line);
		}
	}

	AppendNavFooter(html, page, totalPages, true, false);

	s_Menu[iSlot].screen = MenuScreen::AllRanks;
	s_Menu[iSlot].backScreen = MenuScreen::Main;
	MenuShow(iSlot, html.c_str());
}

static void ShowTopMenu(int iSlot)
{
	MenuState& m = s_Menu[iSlot];
	bool byTime = m.screen == MenuScreen::TopTime;
	int total = (int)m.topRows.size();
	int totalPages = TotalPages(total, kItemsPerPage);
	m.page = ClampPage(m.page, totalPages);
	int start = m.page * kItemsPerPage;

	std::string html;
	if (byTime)
		AppendTitle(html, "Меню рангов | TOP 10 | по активности");
	else
		AppendTitle(html, "Меню рангов | TOP 10 | по очкам опыта");

	for (int i = 0; i < kItemsPerPage; i++)
	{
		int idx = start + i;
		if (idx >= total)
			break;

		const TopRow& row = m.topRows[idx];
		char safeName[128];
		HtmlEscapeShort(row.name, safeName, sizeof(safeName));

		char line[512];
		if (byTime)
		{
			V_snprintf(line, sizeof(line), "%i - ( %.1f ч. ) - %s",
				idx + 1, row.hours, safeName);
		}
		else
		{
			V_snprintf(line, sizeof(line), "%i - ( %i ) - %s",
				idx + 1, row.exp, safeName);
		}
		AppendInfoLine(html, i + 1, line);
	}

	AppendNavFooter(html, m.page, totalPages, m.backScreen != MenuScreen::None, false);
	MenuShow(iSlot, html.c_str());
}

static void FetchTopThenShow(int iSlot, bool byTime, MenuScreen back)
{
	char q[512];
	if (byTime)
		V_snprintf(q, sizeof(q),
			"SELECT `name`, `playtime` / 3600.0 FROM `%s` WHERE `lastconnect` ORDER BY `playtime` DESC LIMIT %i;",
			g_Cfg.tableName, g_Cfg.topCount);
	else
		V_snprintf(q, sizeof(q),
			"SELECT `name`, `rank`, `value` FROM `%s` WHERE `lastconnect` ORDER BY `rank` DESC, `value` DESC LIMIT %i;",
			g_Cfg.tableName, g_Cfg.topCount);

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
					row.hours = (float)r.GetFloat(i, 1);
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

void Menu_OpenRank(int iSlot)
{
	PlayerInfo& p = g_Players[iSlot];
	if (!p.loaded)
	{
		LRCenterPhrase(iSlot, "NotLoaded");
		return;
	}

	uint64_t steam64 = p.steam64;
	char q[512];
	V_snprintf(q, sizeof(q),
		"SELECT (SELECT COUNT(`steam`) FROM `%s` WHERE (`rank` > %i OR (`rank` = %i AND `value` >= %i)) AND `lastconnect`);",
		g_Cfg.tableName, p.level, p.level, p.st.exp);

	DB_Query(q, [iSlot, steam64, gen = PlayerGeneration(iSlot)](const DBResult& r) {
		if (g_Players[iSlot].steam64 != steam64 || !g_Players[iSlot].loaded || PlayerGeneration(iSlot) != gen)
			return;

		int posTop = g_Players[iSlot].posTop;
		if (r.ok && r.RowCount())
		{
			posTop = r.GetInt(0, 0);
			g_Players[iSlot].posTop = posTop;
		}

		PlayerInfo& pl = g_Players[iSlot];
		float kd = pl.st.kills / (pl.st.deaths ? float(pl.st.deaths) : 1.0f);
		ShowRankMenu(iSlot, posTop, kd);
	});
}

bool Menu_IsActive(int iSlot)
{
	if (iSlot < 0 || iSlot >= LR_MAXPLAYERS)
		return false;
	return s_Menu[iSlot].screen != MenuScreen::None;
}

bool Menu_TryHandleKey(int iSlot, int key)
{
	if (iSlot < 0 || iSlot >= LR_MAXPLAYERS || key < 1 || key > 9)
		return false;

	MenuState& m = s_Menu[iSlot];
	if (m.screen == MenuScreen::None || g_Players[iSlot].steam64 != m.steam64)
		return false;

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
				FetchPlaceThenShowMain(iSlot);
			}
			else
			{
				MenuClose(iSlot);
			}
		}
		else if (m.backScreen == MenuScreen::Main)
		{
			FetchPlaceThenShowMain(iSlot);
		}
		else
		{
			MenuClose(iSlot);
		}
		return true;
	}

	if (key == 8)
	{
		if (m.screen == MenuScreen::AllRanks)
		{
			int rankCount = (int)g_Cfg.ranksExp.size();
			int totalPages = TotalPages(rankCount, kItemsPerPage);
			if (m.page + 1 < totalPages)
			{
				m.page++;
				ShowAllRanksMenu(iSlot);
			}
		}
		else if (m.screen == MenuScreen::TopExp || m.screen == MenuScreen::TopTime)
		{
			int totalPages = TotalPages((int)m.topRows.size(), kItemsPerPage);
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
			if (key == 4)
				ShowSessionMenu(iSlot, MenuScreen::Main);
			else if (key == 5)
				FetchTopThenShow(iSlot, false, MenuScreen::Main);
			else if (key == 1)
			{
				m.page = 0;
				ShowAllRanksMenu(iSlot);
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
