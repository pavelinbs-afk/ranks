#pragma once

#include <cstddef>
#include <cstdint>

// MenuSelector MenuType: 1 = chat, 2 = HTML/center, 3 = WASD.
enum LRMenuLayout : int
{
	LR_MENU_CHAT = 1,
	LR_MENU_HTML = 2,
	LR_MENU_WASD = 3,
};

struct LRMenuParsedOption
{
	int  digit = 0;
	char label[128] = {};
};

// Reload player_menus.json cache (MenuSelector shared storage).
void MenuLayout_Reload();

// Player preference from MenuSelector (!menu); default LR_MENU_HTML.
LRMenuLayout MenuLayout_GetPlayerType(int iSlot);
LRMenuLayout MenuLayout_GetTypeForSteam64(uint64_t steam64);
void MenuLayout_GetStatus(int& jsonBytes, bool& jsonLoaded, char* pathOut, size_t pathOutSize);

// Parse canonical HTML menu (!N labels) for chat/WASD adapters.
int MenuLayout_ParseOptions(const char* html, LRMenuParsedOption* out, int maxOut);
bool MenuLayout_ExtractTitle(const char* html, char* out, size_t outSize);

void MenuLayout_ShowChatMenu(int iSlot, const char* html);
void MenuLayout_OpenWasd(int iSlot, const char* html, float menuUntil);
void MenuLayout_CloseWasd(int iSlot);
void MenuLayout_PollWasdMenus();
bool MenuLayout_IsWasdActive(int iSlot);
void MenuLayout_GetWasdDebug(int iSlot, int& optionCount, int& infoCount, bool& active);
