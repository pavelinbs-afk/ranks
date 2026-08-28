#pragma once

#include <cstdarg>

// Loads translations/lr_core.phrases.txt for the configured language,
// replacing {COLOR} tokens with CS2 chat color bytes.
bool LoadPhrases();

// Phrase by key; returns the key itself if missing (so problems are visible).
const char* Phrase(const char* key);

// Chat print with [Статистика] → prefix. Slot -1 = server console.
void LRPrint(int iSlot, const char* fmt, ...);
void LRPrintAll(const char* fmt, ...);

// Phrase-formatted helpers
void LRPrintPhrase(int iSlot, const char* phraseKey, ...);
void LRPrintAllPhrase(const char* phraseKey, ...);

// Raw phrase text (before chat color bytes) — for center HTML.
const char* PhraseRaw(const char* key);

// Center HUD (HTML) with [Статистика] → prefix.
void LRCenterHtml(int iSlot, const char* html, float durationSec = 5.0f);
void LRCenterStop(int iSlot);
void LRWrapCenterHtml(char* out, size_t outSize, const char* body, const char* prefixKey = "Prefix");
void LRCenterBody(int iSlot, const char* htmlBody, float durationSec = 5.0f);
void LRCenterPhrase(int iSlot, const char* phraseKey, ...);
void LRCenterFormat(int iSlot, float durationSec, const char* fmt, ...);
void PhraseFormatHtml(char* out, size_t outSize, const char* phraseKey, ...);

// Escape user-controlled text for center HTML.
void HtmlEscape(const char* in, char* out, size_t outSize);

// Refresh active center panels; call once per GameFrame.
void Center_OnGameFrame();
