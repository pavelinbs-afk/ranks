#include "menu_layout.h"
#include "menu.h"
#include "lr_core.h"
#include "chat.h"
#include "schema.h"

#include <tier0/dbg.h>
#include <tier0/platform.h>
#include <tier1/bufferstring.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <sys/stat.h>
#include <vector>

#include "in_buttons.h"

static const char* s_MenuSelectorRelPaths[] = {
	"addons/counterstrikesharp/plugins/MenuSelector/player_menus.json",
	"csgo/addons/counterstrikesharp/plugins/MenuSelector/player_menus.json",
	nullptr,
};

static std::string s_PlayerMenusJson;
static char s_LoadedPath[512] = {};
static time_t s_JsonReloadAt = 0;
static time_t s_JsonFileMtime = 0;
static bool s_LoggedLoadOk = false;
static bool s_LoggedLoadFail = false;
static constexpr time_t kJsonReloadIntervalSec = 1;

static SchemaField s_fPawn         {"CBasePlayerController", "m_hPawn"};
static SchemaField s_fMoveServices {"CBasePlayerPawn", "m_pMovementServices"};
static SchemaField s_fButtons      {"CPlayer_MovementServices", "m_nButtons"};
static SchemaField s_fButtonStates {"CInButtonState", "m_pButtonStates"};
static SchemaField s_fVelMod       {"CCSPlayerPawn", "m_flVelocityModifier"};

struct WasdSession
{
	bool active = false;
	char title[128] = {};
	char infoLines[8][128];
	int infoCount = 0;
	LRMenuParsedOption options[16];
	int optionCount = 0;
	int selected = 0;
	uint64_t prevButtons = 0;
	float lastAction = 0.0f;
	float savedVelocityMod = 1.0f;
	bool movementLocked = false;
};

static WasdSession s_Wasd[LR_MAXPLAYERS];

static void NormalizeSlashes(char* path)
{
	for (char* c = path; *c; c++)
	{
		if (*c == '\\')
			*c = '/';
	}
}

static bool LoadFileToString(const char* path, std::string& out)
{
	if (!path || !*path)
		return false;

	FILE* f = fopen(path, "rb");
	if (!f)
		return false;

	if (fseek(f, 0, SEEK_END) != 0)
	{
		fclose(f);
		return false;
	}

	long sz = ftell(f);
	if (sz <= 0 || sz > 512 * 1024)
	{
		fclose(f);
		return false;
	}

	rewind(f);
	out.resize((size_t)sz);
	size_t got = fread(out.data(), 1, (size_t)sz, f);
	fclose(f);
	return got == (size_t)sz;
}

static bool BuildAbsolutePath(const char* rel, char* out, size_t outSize)
{
	if (!rel || !*rel || !outSize)
		return false;

	const char* bases[] = {
		nullptr,
		Plat_GetGameDirectory(),
	};

	if (g_pEngine)
	{
		static char s_GameDir[512];
		CBufferStringN<512> gameDir;
		g_pEngine->GetGameDir(gameDir);
		V_strncpy(s_GameDir, gameDir.Get(), sizeof(s_GameDir));
		if (s_GameDir[0])
			bases[0] = s_GameDir;
	}

	for (const char* base : bases)
	{
		if (!base || !*base)
			continue;

		V_snprintf(out, (int)outSize, "%s/%s", base, rel);
		NormalizeSlashes(out);

		// Collapse duplicate slashes (except leading //).
		char* w = out;
		char* r = out;
		char prev = 0;
		while (*r)
		{
			if (*r == '/' && prev == '/')
			{
				r++;
				continue;
			}
			prev = *r;
			*w++ = *r++;
		}
		*w = '\0';

		struct stat st;
		if (stat(out, &st) == 0 && S_ISREG(st.st_mode))
			return true;
	}

	return false;
}

static bool TryLoadPlayerMenusJson(std::string& out, char* pathOut, size_t pathOutSize)
{
	char absPath[512];

	for (int i = 0; s_MenuSelectorRelPaths[i]; i++)
	{
		if (!BuildAbsolutePath(s_MenuSelectorRelPaths[i], absPath, sizeof(absPath)))
			continue;
		if (!LoadFileToString(absPath, out))
			continue;

		if (pathOut && pathOutSize)
			V_strncpy(pathOut, absPath, (int)pathOutSize);
		return true;
	}

	if (pathOut && pathOutSize)
		pathOut[0] = '\0';
	return false;
}

static void ReloadPlayerMenusJsonIfNeeded()
{
	time_t now = time(nullptr);
	if (s_JsonReloadAt && (now - s_JsonReloadAt) < kJsonReloadIntervalSec)
		return;

	char absPath[512];
	struct stat st;
	if (s_LoadedPath[0] && stat(s_LoadedPath, &st) == 0)
	{
		if (s_JsonFileMtime && st.st_mtime == s_JsonFileMtime && !s_PlayerMenusJson.empty())
		{
			s_JsonReloadAt = now;
			return;
		}
	}

	s_JsonReloadAt = now;

	std::string fresh;
	if (!TryLoadPlayerMenusJson(fresh, absPath, sizeof(absPath)))
	{
		if (!s_LoggedLoadFail)
		{
			LR_Log("MenuSelector: player_menus.json not found (default HTML). Expected: addons/counterstrikesharp/plugins/MenuSelector/player_menus.json");
			s_LoggedLoadFail = true;
		}
		return;
	}

	if (stat(absPath, &st) == 0)
		s_JsonFileMtime = st.st_mtime;

	s_PlayerMenusJson = std::move(fresh);
	V_strncpy(s_LoadedPath, absPath, sizeof(s_LoadedPath));
	s_LoggedLoadFail = false;

	if (!s_LoggedLoadOk)
	{
		LR_Log("MenuSelector: loaded %s (%i bytes)", s_LoadedPath, (int)s_PlayerMenusJson.size());
		s_LoggedLoadOk = true;
	}
}

void MenuLayout_Reload()
{
	s_JsonReloadAt = 0;
	s_JsonFileMtime = 0;
	s_LoggedLoadOk = false;
	s_LoggedLoadFail = false;
	ReloadPlayerMenusJsonIfNeeded();
}

void MenuLayout_GetStatus(int& jsonBytes, bool& jsonLoaded, char* pathOut, size_t pathOutSize)
{
	ReloadPlayerMenusJsonIfNeeded();
	jsonBytes = (int)s_PlayerMenusJson.size();
	jsonLoaded = !s_PlayerMenusJson.empty();
	if (pathOut && pathOutSize)
		V_strncpy(pathOut, s_LoadedPath, (int)pathOutSize);
}

static int ParseMenuTypeFromJson(uint64_t steam64)
{
	if (s_PlayerMenusJson.empty() || !steam64)
		return LR_MENU_HTML;

	char key[64];
	V_snprintf(key, sizeof(key), "\"%llu\"", (unsigned long long)steam64);

	const char* pos = s_PlayerMenusJson.c_str();
	while ((pos = strstr(pos, key)) != nullptr)
	{
		const char* colon = strchr(pos + strlen(key), ':');
		if (!colon)
			break;

		colon++;
		while (*colon == ' ' || *colon == '\t')
			colon++;

		int type = atoi(colon);
		if (type == LR_MENU_CHAT || type == LR_MENU_WASD || type == LR_MENU_HTML)
			return type;

		pos += strlen(key);
	}

	return LR_MENU_HTML;
}

LRMenuLayout MenuLayout_GetPlayerType(int iSlot)
{
	ReloadPlayerMenusJsonIfNeeded();

	if (iSlot < 0 || iSlot >= LR_MAXPLAYERS || !g_Players[iSlot].steam64)
		return LR_MENU_HTML;

	return (LRMenuLayout)ParseMenuTypeFromJson(g_Players[iSlot].steam64);
}

LRMenuLayout MenuLayout_GetTypeForSteam64(uint64_t steam64)
{
	ReloadPlayerMenusJsonIfNeeded();
	if (!steam64)
		return LR_MENU_HTML;
	return (LRMenuLayout)ParseMenuTypeFromJson(steam64);
}

static const char* SkipHtmlTag(const char* p)
{
	if (!p || *p != '<')
		return p;
	while (*p && *p != '>')
		p++;
	return (*p == '>') ? p + 1 : p;
}

static bool IsDigitChar(char c, int* outDigit)
{
	if (c < '1' || c > '9')
		return false;
	*outDigit = c - '0';
	return true;
}

static bool ExtractOptionLabel(const char* start, char* label, size_t labelSize)
{
	if (!start || !label || labelSize == 0)
		return false;

	label[0] = '\0';

	const char* p = start;
	while (*p == ' ' || *p == '\t')
		p++;

	// lr_core: "<font>!N</font> <font color='...'>Label</font>"
	if (*p == '<')
	{
		const char* gt = strchr(p, '>');
		if (!gt)
			return false;
		p = gt + 1;
	}

	size_t li = 0;
	for (; *p && li + 1 < labelSize; p++)
	{
		if (*p == '<' || *p == '\n' || *p == '\r')
			break;
		label[li++] = *p;
	}
	label[li] = '\0';

	while (li > 0 && (label[li - 1] == ' ' || label[li - 1] == '\t'))
		label[--li] = '\0';

	return li > 0;
}

static bool TryParseOptionAt(const char* hit, int* digit, char* label, size_t labelSize)
{
	if (!hit || !digit || !label)
		return false;

	if (!IsDigitChar(*hit, digit))
		return false;

	const char* afterDigit = hit + 1;
	if (*afterDigit == ']')
		afterDigit++;

	if (strncmp(afterDigit, "</font>", 7) != 0)
		return false;

	return ExtractOptionLabel(afterDigit + 7, label, labelSize);
}

static bool IsGreyInfoLine(const char* hit, const char* htmlStart)
{
	if (!hit || hit < htmlStart + 2)
		return false;

	// Цвет только у <font> этого !N, а не у строк выше (иначе !3 «Моя статистика» → info).
	const char* closeGt = hit - 1;
	if (closeGt < htmlStart || *closeGt != '>')
		return false;

	const char* fontOpen = closeGt;
	while (fontOpen > htmlStart)
	{
		if (fontOpen[0] == '<' && (fontOpen[1] == 'f' || fontOpen[1] == 'F'))
			break;
		fontOpen--;
	}

	if (fontOpen <= htmlStart || fontOpen[0] != '<')
		return false;

	char tag[96];
	size_t tagLen = (size_t)(closeGt - fontOpen);
	if (tagLen >= sizeof(tag))
		tagLen = sizeof(tag) - 1;
	memcpy(tag, fontOpen, tagLen);
	tag[tagLen] = '\0';

	return strstr(tag, "#aaaaaa") != nullptr;
}

static void ParseWasdMenuContent(const char* html, WasdSession& ws)
{
	ws.optionCount = 0;
	ws.infoCount = 0;
	if (!html)
		return;

	bool seen[10] = {};

	for (const char* p = html; *p; )
	{
		const char* bang = strstr(p, ">!");
		const char* bracket = strstr(p, ">[");
		const char* hit = nullptr;
		if (bang && (!bracket || bang < bracket))
			hit = bang + 2;
		else if (bracket)
			hit = bracket + 1;
		else
			break;

		int digit = 0;
		char label[128];
		if (!TryParseOptionAt(hit, &digit, label, sizeof(label)))
		{
			p = hit + 1;
			continue;
		}

		const char* afterDigit = hit + 1;
		if (*afterDigit == ']')
			afterDigit++;
		const char* labelStart = afterDigit + 7;

		if (seen[digit])
		{
			p = labelStart;
			continue;
		}
		seen[digit] = true;

		if (IsGreyInfoLine(hit, html))
		{
			if (ws.infoCount < (int)sizeof(ws.infoLines) / (int)sizeof(ws.infoLines[0]))
				V_strncpy(ws.infoLines[ws.infoCount++], label, sizeof(ws.infoLines[0]));
		}
		else if (ws.optionCount < 16)
		{
			ws.options[ws.optionCount].digit = digit;
			V_strncpy(ws.options[ws.optionCount].label, label, sizeof(ws.options[ws.optionCount].label));
			ws.optionCount++;
		}

		p = labelStart;
	}
}

int MenuLayout_ParseOptions(const char* html, LRMenuParsedOption* out, int maxOut)
{
	if (!html || !out || maxOut <= 0)
		return 0;

	int count = 0;
	bool seen[10] = {};

	for (const char* p = html; *p; )
	{
		const char* bang = strstr(p, ">!");
		const char* bracket = strstr(p, ">[");
		const char* hit = nullptr;
		// HTML menus use ">!N</font>" (AdminPlugin) or ">[N]</font>".
		if (bang && (!bracket || bang < bracket))
			hit = bang + 2;
		else if (bracket)
			hit = bracket + 1;
		else
			break;

		int digit = 0;
		char label[128];
		if (!TryParseOptionAt(hit, &digit, label, sizeof(label)))
		{
			p = hit + 1;
			continue;
		}

		const char* afterDigit = hit + 1;
		if (*afterDigit == ']')
			afterDigit++;
		const char* labelStart = afterDigit + 7;

		if (seen[digit])
		{
			p = labelStart;
			continue;
		}

		seen[digit] = true;
		out[count].digit = digit;
		V_strncpy(out[count].label, label, sizeof(out[count].label));
		count++;
		if (count >= maxOut)
			break;

		p = labelStart;
	}

	return count;
}

bool MenuLayout_ExtractTitle(const char* html, char* out, size_t outSize)
{
	if (!out || outSize == 0)
		return false;
	out[0] = '\0';
	if (!html)
		return false;

	const char* font = strstr(html, "<font");
	if (!font)
		return false;

	const char* gt = strchr(font, '>');
	if (!gt)
		return false;

	const char* start = gt + 1;
	if (*start == '!' || *start == '[' || (unsigned char)*start == 0xE2)
		return false;

	size_t j = 0;
	for (const char* c = start; *c && j + 1 < outSize; )
	{
		if (*c == '<')
			break;
		out[j++] = *c++;
	}
	out[j] = '\0';

	while (j > 0 && (out[j - 1] == ' ' || out[j - 1] == '\t'))
		out[--j] = '\0';

	return j > 0;
}

void MenuLayout_ShowChatMenu(int iSlot, const char* html)
{
	if (iSlot < 0 || iSlot >= LR_MAXPLAYERS || !html)
		return;

	char title[128];
	if (!MenuLayout_ExtractTitle(html, title, sizeof(title)))
		V_strncpy(title, "Меню", sizeof(title));

	LRPrint(iSlot, "%s", title);

	LRMenuParsedOption opts[16];
	int n = MenuLayout_ParseOptions(html, opts, 16);
	bool hasNine = false;

	for (int i = 0; i < n; i++)
	{
		if (opts[i].digit == 9)
			hasNine = true;
		LRPrint(iSlot, "!%i %s", opts[i].digit, opts[i].label);
	}

	if (!hasNine)
		LRPrint(iSlot, "!9 Выход");
}

static uint64_t ReadPlayerButtons(int iSlot)
{
	CEntityInstance* pController = GetControllerBySlot(iSlot);
	if (!pController || !g_pGameEntitySystem)
		return 0;

	CEntityHandle hPawn = s_fPawn.Get<CEntityHandle>(pController);
	CEntityInstance* pPawn = g_pGameEntitySystem->GetEntityInstance(hPawn);
	if (!pPawn)
		return 0;

	void* pMoveServices = s_fMoveServices.Get<void*>(pPawn);
	if (!pMoveServices)
		return 0;

	int32_t offButtons = s_fButtons.Offset();
	int32_t offStates = s_fButtonStates.Offset();
	if (offButtons < 0 || offStates < 0)
		return 0;

	return *reinterpret_cast<uint64_t*>(
		reinterpret_cast<uintptr_t>(pMoveServices) + offButtons + offStates);
}

static void ApplyWasdMovementLock(int iSlot)
{
	CEntityInstance* pController = GetControllerBySlot(iSlot);
	if (!pController || !g_pGameEntitySystem)
		return;

	CEntityHandle hPawn = s_fPawn.Get<CEntityHandle>(pController);
	CEntityInstance* pPawn = g_pGameEntitySystem->GetEntityInstance(hPawn);
	if (!pPawn)
		return;

	float mod = s_fVelMod.Get<float>(pPawn);
	if (mod > 0.0001f)
		Schema_SetNetworked(pPawn, "CCSPlayerPawn", "m_flVelocityModifier", 0.0f);
}

static void RestoreWasdMovement(int iSlot, WasdSession& ws)
{
	if (!ws.movementLocked)
		return;

	ws.movementLocked = false;

	CEntityInstance* pController = GetControllerBySlot(iSlot);
	if (!pController || !g_pGameEntitySystem)
		return;

	CEntityHandle hPawn = s_fPawn.Get<CEntityHandle>(pController);
	CEntityInstance* pPawn = g_pGameEntitySystem->GetEntityInstance(hPawn);
	if (!pPawn)
		return;

	float restore = ws.savedVelocityMod > 0.001f ? ws.savedVelocityMod : 1.0f;
	Schema_SetNetworked(pPawn, "CCSPlayerPawn", "m_flVelocityModifier", restore);
}

static void BuildWasdHud(const WasdSession& ws, char* out, size_t outSize)
{
	if (!out || outSize == 0)
		return;

	out[0] = '\0';
	V_snprintf(out, outSize,
		"<font color='#cccc33'>%s</font><br/>",
		ws.title[0] ? ws.title : "Меню");

	size_t used = strlen(out);
	for (int i = 0; i < ws.infoCount && used + 1 < outSize; i++)
	{
		char line[256];
		V_snprintf(line, sizeof(line), "<font color='#dddddd'>%s</font><br/>", ws.infoLines[i]);
		V_strncat(out, line, (int)outSize);
		used = strlen(out);
	}

	for (int i = 0; i < ws.optionCount && used + 1 < outSize; i++)
	{
		const LRMenuParsedOption& opt = ws.options[i];
		char line[256];
		if (i == ws.selected)
			V_snprintf(line, sizeof(line), "<font color='#88ff88'>&#9654; %s</font><br/>", opt.label);
		else
			V_snprintf(line, sizeof(line), "<font color='#aaaaaa'>  %s</font><br/>", opt.label);

		V_strncat(out, line, (int)outSize);
		used = strlen(out);
	}

	V_strncat(out, "<font color='#888888'>[W/S] листать · [E] выбрать · [R] выход</font>", (int)outSize);
}

static void RefreshWasdHud(int iSlot, WasdSession& ws)
{
	if (ws.optionCount <= 0)
		return;

	if (ws.selected < 0)
		ws.selected = 0;
	if (ws.selected >= ws.optionCount)
		ws.selected = ws.optionCount - 1;

	char html[4096];
	BuildWasdHud(ws, html, sizeof(html));

	CGlobalVars* gv = GetGlobals();
	float remain = Menu_GetUntil(iSlot) - (gv ? gv->curtime : 0.0f);
	if (remain < 0.1f)
		remain = 0.1f;

	LRCenterHtml(iSlot, html, remain);
}

void MenuLayout_OpenWasd(int iSlot, const char* html, float /*menuUntil*/)
{
	if (iSlot < 0 || iSlot >= LR_MAXPLAYERS || !html)
		return;

	MenuLayout_CloseWasd(iSlot);

	WasdSession ws;
	if (!MenuLayout_ExtractTitle(html, ws.title, sizeof(ws.title)))
		V_strncpy(ws.title, "Меню", sizeof(ws.title));

	ParseWasdMenuContent(html, ws);
	if (ws.optionCount <= 0)
	{
		CGlobalVars* gv = GetGlobals();
		float remain = Menu_GetUntil(iSlot) - (gv ? gv->curtime : 0.0f);
		if (remain < 0.1f)
			remain = 5.0f;
		LR_Log("MenuLayout WASD slot %d: 0 options parsed, HTML fallback", iSlot);
		LRCenterHtml(iSlot, html, remain);
		return;
	}

	bool hasNine = false;
	for (int i = 0; i < ws.optionCount; i++)
	{
		if (ws.options[i].digit == 9)
			hasNine = true;
	}

	if (!hasNine && ws.optionCount < 16)
	{
		ws.options[ws.optionCount].digit = 9;
		V_strncpy(ws.options[ws.optionCount].label, "Выход", sizeof(ws.options[ws.optionCount].label));
		ws.optionCount++;
	}

	CEntityInstance* pController = GetControllerBySlot(iSlot);
	if (pController && g_pGameEntitySystem)
	{
		CEntityHandle hPawn = s_fPawn.Get<CEntityHandle>(pController);
		CEntityInstance* pPawn = g_pGameEntitySystem->GetEntityInstance(hPawn);
		if (pPawn)
		{
			float mod = s_fVelMod.Get<float>(pPawn);
			ws.savedVelocityMod = mod > 0.001f ? mod : 1.0f;
		}
	}

	ws.prevButtons = ReadPlayerButtons(iSlot);
	ws.active = true;
	ws.movementLocked = true;
	ws.selected = 0;

	CGlobalVars* gv = GetGlobals();
	ws.lastAction = gv ? gv->curtime : 0.0f;

	s_Wasd[iSlot] = ws;
	ApplyWasdMovementLock(iSlot);
	RefreshWasdHud(iSlot, s_Wasd[iSlot]);
}

void MenuLayout_CloseWasd(int iSlot)
{
	if (iSlot < 0 || iSlot >= LR_MAXPLAYERS)
		return;

	WasdSession& ws = s_Wasd[iSlot];
	if (ws.active)
		RestoreWasdMovement(iSlot, ws);
	s_Wasd[iSlot] = WasdSession{};
}

bool MenuLayout_IsWasdActive(int iSlot)
{
	if (iSlot < 0 || iSlot >= LR_MAXPLAYERS)
		return false;
	return s_Wasd[iSlot].active;
}

void MenuLayout_GetWasdDebug(int iSlot, int& optionCount, int& infoCount, bool& active)
{
	optionCount = 0;
	infoCount = 0;
	active = false;
	if (iSlot < 0 || iSlot >= LR_MAXPLAYERS)
		return;
	optionCount = s_Wasd[iSlot].optionCount;
	infoCount = s_Wasd[iSlot].infoCount;
	active = s_Wasd[iSlot].active;
}

void MenuLayout_PollWasdMenus()
{
	CGlobalVars* gv = GetGlobals();
	float now = gv ? gv->curtime : 0.0f;

	for (int iSlot = 0; iSlot < LR_MAXPLAYERS; iSlot++)
	{
		WasdSession& ws = s_Wasd[iSlot];
		if (!ws.active)
			continue;

		if (!Menu_IsActive(iSlot) || now >= Menu_GetUntil(iSlot)
			|| !g_Players[iSlot].steam64)
		{
			MenuLayout_CloseWasd(iSlot);
			continue;
		}

		if (ws.movementLocked)
			ApplyWasdMovementLock(iSlot);

		uint64_t buttons = ReadPlayerButtons(iSlot);
		uint64_t prev = ws.prevButtons;
		ws.prevButtons = buttons;

		if (now - ws.lastAction < 0.12f)
			continue;

		auto pressed = [&](uint64_t btn) -> bool {
			return (buttons & btn) != 0 && (prev & btn) == 0;
		};

		if (pressed(IN_FORWARD))
		{
			ws.lastAction = now;
			ws.selected = (ws.selected - 1 + ws.optionCount) % ws.optionCount;
			RefreshWasdHud(iSlot, ws);
			continue;
		}

		if (pressed(IN_BACK))
		{
			ws.lastAction = now;
			ws.selected = (ws.selected + 1) % ws.optionCount;
			RefreshWasdHud(iSlot, ws);
			continue;
		}

		if (pressed(IN_USE))
		{
			ws.lastAction = now;
			if (ws.optionCount > 0)
				Menu_TryHandleKey(iSlot, ws.options[ws.selected].digit);
			continue;
		}

		if (pressed(IN_RELOAD))
		{
			ws.lastAction = now;
			Menu_Close(iSlot);
		}
	}
}
