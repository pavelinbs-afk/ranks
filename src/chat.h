#pragma once

#include <cstdarg>

// Loads translations/lr_core.phrases.txt for the configured language,
// replacing {COLOR} tokens with CS2 chat color bytes.
bool LoadPhrases();

// Phrase by key; returns the key itself if missing (so problems are visible).
const char* Phrase(const char* key);

// Chat print with [LR] prefix. Slot -1 = server console.
void LRPrint(int iSlot, const char* fmt, ...);
void LRPrintAll(const char* fmt, ...);

// Phrase-formatted helpers
void LRPrintPhrase(int iSlot, const char* phraseKey, ...);
void LRPrintAllPhrase(const char* phraseKey, ...);
