#pragma once

// Yooma-style menu for !lvl / !session / !top / !toptime (+ !1..!9 nav).
// Layout (HTML / chat / WASD) follows MenuSelector (!menu) preference.
void Menu_OpenLvl(int iSlot);
void Menu_OpenSession(int iSlot);
void Menu_OpenTop(int iSlot, bool byTime);

bool Menu_IsActive(int iSlot);
bool Menu_IsWasdActive(int iSlot);
float Menu_GetUntil(int iSlot);
bool Menu_TryHandleKey(int iSlot, int key); // 1..9

void Menu_Close(int iSlot);

void Menu_OnGameFrame();
void Menu_OnDisconnect(int iSlot);
