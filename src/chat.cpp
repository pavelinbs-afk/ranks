#include "lr_core.h"
#include "chat.h"
#include "vtable_finder.h"

#include <cstring>
#include <map>

#include <KeyValues.h>
#include <filesystem.h>
#include <inetchannel.h>
#include <networksystem/inetworkserializer.h>
#include <networksystem/netmessage.h>

#include "usermessages.pb.h"

#define HUD_DEST_CHAT   3
#define HUD_DEST_CENTER 4

// Как AdminPlugin / CounterStrikeSharp PrintToCenterHtml.
static constexpr int   kCenterHudChunkMs     = 20000;
static constexpr float kCenterHudRefreshSec  = 0.02f;

#ifdef _WIN32
#define LR_SERVER_MODULE "server.dll"
#else
#define LR_SERVER_MODULE "/libserver.so"
#endif

using GetLegacyGameEventListenerFn = IGameEventListener2* (*)(CPlayerSlot);

static GetLegacyGameEventListenerFn s_GetLegacyGameEventListener = nullptr;
static bool s_LegacyListenerLookupDone = false;

static void EnsureLegacyGameEventListener()
{
	if (s_LegacyListenerLookupDone)
		return;
	s_LegacyListenerLookupDone = true;

	void* fn = FindFunctionSignature(LR_SERVER_MODULE, ".text",
		"48 8B 05 ? ? ? ? 48 85 C0 74 ? 83 FF ? 77 ? 48 63 FF 48 C1 E7 ? 48 8D 44 38");
	if (!fn)
	{
		Warning("[LR] LegacyGameEventListener signature not found — center HTML may not render\n");
		return;
	}

	s_GetLegacyGameEventListener = reinterpret_cast<GetLegacyGameEventListenerFn>(fn);
}

static std::map<std::string, std::string> s_Phrases;
static std::map<std::string, std::string> s_PhrasesRaw;

struct HtmlColorToken { const char* token; const char* openTag; };
static const HtmlColorToken s_HtmlColors[] = {
	{"{DEFAULT}",  "</font>"},
	{"{DARKRED}",  "<font color='#8b0000'>"},
	{"{TEAM}",     "<font color='#99ccff'>"},
	{"{GREEN}",    "<font color='#88ff88'>"},
	{"{OLIVE}",    "<font color='#808000'>"},
	{"{LIME}",     "<font color='#b8ff88'>"},
	{"{RED}",      "<font color='#ff6666'>"},
	{"{GREY}",     "<font color='#aaaaaa'>"},
	{"{YELLOW}",   "<font color='#cccc33'>"},
	{"{SILVER}",   "<font color='#c0c0c0'>"},
	{"{BLUE}",     "<font color='#6699ff'>"},
	{"{DARKBLUE}", "<font color='#4d79ff'>"},
	{"{PURPLE}",   "<font color='#cc66ff'>"},
	{"{LIGHTRED}", "<font color='#ff9999'>"},
	{"{GOLD}",     "<font color='#ffd700'>"},
};

struct ColorToken { const char* token; const char* byte; };
static const ColorToken s_Colors[] = {
	{"{DEFAULT}",   "\x01"},
	{"{DARKRED}",   "\x02"},
	{"{TEAM}",      "\x03"},
	{"{GREEN}",     "\x04"},
	{"{OLIVE}",     "\x05"},
	{"{LIME}",      "\x06"},
	{"{RED}",       "\x07"},
	{"{GREY}",      "\x08"},
	{"{YELLOW}",    "\x09"},
	{"{SILVER}",    "\x0A"},
	{"{BLUE}",      "\x0B"},
	{"{DARKBLUE}",  "\x0C"},
	{"{PURPLE}",    "\x0E"},
	{"{LIGHTRED}",  "\x0F"},
	{"{GOLD}",      "\x10"},
};

static void ReplaceColors(std::string& s)
{
	for (const auto& c : s_Colors)
	{
		size_t pos = 0;
		size_t tokenLen = strlen(c.token);
		while ((pos = s.find(c.token, pos)) != std::string::npos)
			s.replace(pos, tokenLen, c.byte);
	}
}

// ---------------------------------------------------------------------------
// Format validation
//
// Phrases come from a file admins edit and are handed straight to vsnprintf as
// format strings, with the argument list fixed by the call site. A "%s" where
// the code passes an int makes vsnprintf dereference an integer as a pointer
// and takes the server down — and lr_reload re-reads the file on a live server,
// so a typo is a live crash, not a startup one.
//
// Every phrase used as a format string therefore declares what its call site
// actually passes, and a phrase whose conversions do not match is refused at
// load time and replaced with its own key (which contains no '%').
//
//   'i' = int   'f' = double (floats promote through varargs)   's' = const char*
// ---------------------------------------------------------------------------

struct PhraseSpec { const char* key; const char* args; };

// ChangeExp() passes (const char* signedDelta, const char* coinSuffix, int newExp) to exp phrases.
#define LR_EXP_ARGS "ssi"

static const PhraseSpec s_Specs[] = {
	{"Kill",            LR_EXP_ARGS},
	{"MyDeath",         LR_EXP_ARGS},
	{"Suicide",         LR_EXP_ARGS},
	{"TeamKill",        LR_EXP_ARGS},
	{"AssisterKill",    LR_EXP_ARGS},
	{"HeadShotKill",    LR_EXP_ARGS},
	{"RoundWin",        LR_EXP_ARGS},
	{"RoundLose",       LR_EXP_ARGS},
	{"RoundMVP",        LR_EXP_ARGS},
	{"BombPlanted",     LR_EXP_ARGS},
	{"BombDefused",     LR_EXP_ARGS},
	{"BombDropped",     LR_EXP_ARGS},
	{"BombPickup",      LR_EXP_ARGS},
	{"HostageKilled",   LR_EXP_ARGS},
	{"HostageRescued",  LR_EXP_ARGS},
	{"TimeExp",         LR_EXP_ARGS},
	{"AdminGive",       LR_EXP_ARGS},
	{"AdminTake",       LR_EXP_ARGS},
	{"DoubleKill",      LR_EXP_ARGS},
	{"TripleKill",      LR_EXP_ARGS},
	{"Domination",      LR_EXP_ARGS},
	{"Rampage",         LR_EXP_ARGS},
	{"MegaKill",        LR_EXP_ARGS},
	{"Ownage",          LR_EXP_ARGS},
	{"UltraKill",       LR_EXP_ARGS},
	{"KillingSpree",    LR_EXP_ARGS},
	{"MonsterKill",     LR_EXP_ARGS},
	{"Unstoppable",     LR_EXP_ARGS},
	{"GodLike",         LR_EXP_ARGS},

	{"CoinInterval",    "s"},
	{"CoinMvp",         "s"},
	{"CoinMultiKill",   "s"},

	{"LevelUp",         "i"},        // level number
	{"LevelDown",       "i"},
	{"LevelUpAll",      "si"},       // player name, new level
	{"LevelDownAll",    "si"},       // player name, new level

	{"RankPlayer",      "siiiiif"},  // name, pos, total, exp, kills, deaths, kd
	{"MyLevel",         "siis"},     // rank name, level, level count, exp text
	{"SessionStats",    "ssiiifs"},  // time, exp, kills, deaths, hs, kd, pos

	{"TopTitle",        ""},
	{"TopTimeTitle",    ""},
	{"TopLine",         "isii"},     // place, name, level, exp
	{"TopTimeLine",     "isf"},      // place, name, hours
	{"NoData",          ""},

	{"RoundStartMessageRanks", ""},
	{"SpawnHintRank",          ""},
	{"SpawnHintCommands",      ""},
	{"RoundStartCheckCount",   "ii"},  // humans, required
	{"RoundExpResultGive",     "i"},
	{"RoundExpResultTake",     "i"},
	{"RoundExpResultNothing",  ""},
	{"RoundExpResultAll",      "i"},

	{"ResetStatsDone",     ""},
	{"ResetStatsCooldown", "ii"},    // hours, minutes
	{"ResetStatsConfirm",  ""},
	{"ResetStatsCancelled",""},
	{"NotLoaded",          ""},
};

// nullptr = never used as a format string, only ever passed as a %s argument,
// so a literal '%' inside it is harmless and must not be rejected.
static const char* ExpectedArgs(const char* key)
{
	if (!strncmp(key, "rank_", 5) || !strcmp(key, "Prefix") || !strcmp(key, "You"))
		return nullptr;

	for (const auto& s : s_Specs)
	{
		if (!strcmp(s.key, key))
			return s.args;
	}

	// Unregistered phrase: treat it as taking no arguments. If someone adds a
	// call site and forgets the table, this fails loudly instead of crashing.
	return "";
}

// Builds the argument signature of a format string, or fails on anything we
// refuse to pass to vsnprintf.
static bool ParseFormatSig(const char* fmt, std::string& out, std::string& why)
{
	out.clear();

	for (const char* p = fmt; *p; )
	{
		if (*p++ != '%')
			continue;
		if (*p == '%') { p++; continue; } // literal percent

		while (*p == '-' || *p == '+' || *p == ' ' || *p == '#' || *p == '0')
			p++;

		// '*' pulls an extra int off the stack that no call site provides
		if (*p == '*') { why = "'*' width is not supported"; return false; }
		while (*p >= '0' && *p <= '9')
			p++;

		if (*p == '.')
		{
			p++;
			if (*p == '*') { why = "'*' precision is not supported"; return false; }
			while (*p >= '0' && *p <= '9')
				p++;
		}

		// length modifiers change the argument width and would misread the stack
		if (*p && strchr("lhzjtL", *p))
		{
			why = "length modifiers are not supported";
			return false;
		}

		switch (*p)
		{
			case 'd': case 'i': case 'u': case 'c':
			case 'x': case 'X': case 'o':
				out += 'i';
				break;
			case 'f': case 'F': case 'e': case 'E': case 'g': case 'G':
				out += 'f';
				break;
			case 's':
				out += 's';
				break;
			case '\0':
				why = "truncated conversion at end of string";
				return false;
			default:
				why = std::string("unsupported conversion '%") + *p + "'";
				return false;
		}
		p++;
	}

	return true;
}

bool LoadPhrases()
{
	KeyValues* kv = new KeyValues("Phrases");
	KeyValues::AutoDelete autoDelete(kv);

	const char* path = "addons/lr_core/translations/lr_core.phrases.txt";
	if (!kv->LoadFromFile(g_pFullFileSystem, path))
	{
		Warning("[LR] Failed to load %s\n", path);
		return false;
	}

	s_Phrases.clear();
	s_PhrasesRaw.clear();
	int rejected = 0;

	for (KeyValues* k = kv->GetFirstTrueSubKey(); k; k = k->GetNextTrueSubKey())
	{
		const char* key = k->GetName();
		std::string value = k->GetString(g_Cfg.language, k->GetString("en", key));
		s_PhrasesRaw[key] = value;

		if (const char* expected = ExpectedArgs(key))
		{
			std::string sig, why;
			if (!ParseFormatSig(value.c_str(), sig, why))
			{
				Warning("[LR] phrase \"%s\" refused: %s\n", key, why.c_str());
				value = key;
				rejected++;
			}
			else if (sig != expected)
			{
				Warning("[LR] phrase \"%s\" refused: call site passes [%s], text wants [%s]\n",
					key, expected, sig.c_str());
				value = key;
				rejected++;
			}
		}

		ReplaceColors(value);
		s_Phrases[key] = std::move(value);
	}

	if (rejected)
	{
		// Not fatal: falling back to the key keeps the server up and readable,
		// which matters more than exactness when this fires from lr_reload.
		Warning("[LR] %i phrase(s) replaced with their key, fix %s\n", rejected, path);
	}

	return true;
}

const char* Phrase(const char* key)
{
	auto it = s_Phrases.find(key);
	return it != s_Phrases.end() ? it->second.c_str() : key;
}

const char* PhraseRaw(const char* key)
{
	auto it = s_PhrasesRaw.find(key);
	return it != s_PhrasesRaw.end() ? it->second.c_str() : key;
}

static void ReplaceHtmlColors(std::string& s)
{
	for (const auto& c : s_HtmlColors)
	{
		size_t pos = 0;
		size_t tokenLen = strlen(c.token);
		while ((pos = s.find(c.token, pos)) != std::string::npos)
			s.replace(pos, tokenLen, c.openTag);
	}
}

void HtmlEscape(const char* in, char* out, size_t outSize)
{
	if (!outSize)
		return;
	out[0] = '\0';
	if (!in)
		return;

	size_t j = 0;
	for (const char* c = in; *c && j + 1 < outSize; c++)
	{
		switch (*c)
		{
			case '&':
				if (j + 5 >= outSize) return;
				memcpy(out + j, "&amp;", 5); j += 5;
				break;
			case '<':
				if (j + 4 >= outSize) return;
				memcpy(out + j, "&lt;", 4); j += 4;
				break;
			case '>':
				if (j + 4 >= outSize) return;
				memcpy(out + j, "&gt;", 4); j += 4;
				break;
			case '"':
				if (j + 6 >= outSize) return;
				memcpy(out + j, "&quot;", 6); j += 6;
				break;
			default:
				out[j++] = *c;
				break;
		}
	}
	out[j] = '\0';
}

static void FormatPhraseHtmlBody(char* out, size_t outSize, const char* phraseKey, va_list va)
{
	std::string tmpl = PhraseRaw(phraseKey);
	ReplaceHtmlColors(tmpl);
	va_list copy;
	va_copy(copy, va);
	V_vsnprintf(out, outSize, tmpl.c_str(), copy);
	va_end(copy);
}

static void WrapCenterHtml(char* out, size_t outSize, const char* body, const char* prefixKey)
{
	std::string prefix = PhraseRaw(prefixKey);
	ReplaceHtmlColors(prefix);
	V_snprintf(out, outSize, "%s<br/>%s", prefix.c_str(), body);
}

void LRWrapCenterHtml(char* out, size_t outSize, const char* body, const char* prefixKey)
{
	if (!prefixKey)
		prefixKey = "Prefix";
	WrapCenterHtml(out, outSize, body ? body : "", prefixKey);
}

static void FireCenterHtml(int iSlot, const char* html, int durationMs = kCenterHudChunkMs)
{
	if (iSlot < 0 || iSlot >= LR_MAXPLAYERS || !html)
		return;

	EnsureLegacyGameEventListener();

	if (g_pGameEventManager && s_GetLegacyGameEventListener)
	{
		IGameEventListener2* pListener = s_GetLegacyGameEventListener(CPlayerSlot(iSlot));
		if (!pListener)
			goto fallback_plain;

		IGameEvent* ev = g_pGameEventManager->CreateEvent("show_survival_respawn_status", true);
		if (!ev)
			goto fallback_plain;

		ev->SetString("loc_token", html);
		ev->SetInt("duration", durationMs);

		if (CEntityInstance* pController = GetControllerBySlot(iSlot))
			ev->SetPlayer("userid", pController);

		pListener->FireGameEvent(ev);
		g_pGameEventManager->FreeEvent(ev);
		return;
	}

fallback_plain:
	// Fallback: plain center text (strip simple tags).
	if (!g_pNetworkMessages || !g_pGameEventSystem)
		return;

	char plain[1024];
	const char* src = html;
	char* dst = plain;
	size_t left = sizeof(plain) - 1;
	while (*src && left > 0)
	{
		if (*src == '<')
		{
			while (*src && *src != '>')
				src++;
			if (*src == '>')
				src++;
			continue;
		}
		*dst++ = *src++;
		left--;
	}
	*dst = '\0';

	INetworkMessageInternal* pMsg = g_pNetworkMessages->FindNetworkMessagePartial("TextMsg");
	if (!pMsg)
		return;

	auto* data = pMsg->AllocateMessage()->ToPB<CUserMessageTextMsg>();
	data->set_dest(HUD_DEST_CENTER);
	data->add_param(plain);

	uint64 clients = 1ull << iSlot;
	g_pGameEventSystem->PostEventAbstract(CSplitScreenSlot(-1), false, LR_MAXPLAYERS, &clients,
		pMsg, data, 0, NetChannelBufType_t::BUF_RELIABLE);
	delete data;
}

struct PendingCenter
{
	int   iSlot;
	float until;
	float nextRefresh;
	char  html[2048];
};

static std::vector<PendingCenter> s_PendingCenter;

static void RemovePendingCenter(int iSlot)
{
	for (size_t i = 0; i < s_PendingCenter.size(); )
	{
		if (s_PendingCenter[i].iSlot == iSlot)
		{
			s_PendingCenter[i] = s_PendingCenter.back();
			s_PendingCenter.pop_back();
		}
		else
			i++;
	}
}

void LRCenterStop(int iSlot)
{
	if (iSlot < 0 || iSlot >= LR_MAXPLAYERS)
		return;

	RemovePendingCenter(iSlot);
	FireCenterHtml(iSlot, "", 1);
}

void LRCenterHtml(int iSlot, const char* html, float durationSec)
{
	if (iSlot < 0 || iSlot >= LR_MAXPLAYERS)
	{
		ConMsg("[LR center] %s\n", html ? html : "");
		return;
	}
	if (!html || !*html)
		return;

	FireCenterHtml(iSlot, html);

	if (durationSec <= 0.0f)
		return;

	CGlobalVars* gv = GetGlobals();
	float now = gv ? gv->curtime : 0.0f;

	for (PendingCenter& pc : s_PendingCenter)
	{
		if (pc.iSlot == iSlot)
		{
			pc.until = now + durationSec;
			pc.nextRefresh = now + kCenterHudRefreshSec;
			V_strncpy(pc.html, html, sizeof(pc.html));
			return;
		}
	}

	PendingCenter pc;
	pc.iSlot = iSlot;
	pc.until = now + durationSec;
	pc.nextRefresh = now + kCenterHudRefreshSec;
	V_strncpy(pc.html, html, sizeof(pc.html));
	s_PendingCenter.push_back(pc);
}

void Center_OnGameFrame()
{
	if (s_PendingCenter.empty())
		return;

	CGlobalVars* gv = GetGlobals();
	float now = gv ? gv->curtime : 0.0f;

	for (size_t i = 0; i < s_PendingCenter.size(); )
	{
		PendingCenter& pc = s_PendingCenter[i];
		if (now >= pc.until || pc.iSlot < 0 || pc.iSlot >= LR_MAXPLAYERS
			|| !g_Players[pc.iSlot].steam64)
		{
			s_PendingCenter[i] = s_PendingCenter.back();
			s_PendingCenter.pop_back();
			continue;
		}

		if (now >= pc.nextRefresh)
		{
			FireCenterHtml(pc.iSlot, pc.html);
			pc.nextRefresh = now + kCenterHudRefreshSec;
		}
		i++;
	}
}

void LRCenterPhrase(int iSlot, const char* phraseKey, ...)
{
	char body[1024];
	va_list va;
	va_start(va, phraseKey);
	FormatPhraseHtmlBody(body, sizeof(body), phraseKey, va);
	va_end(va);

	char html[1200];
	WrapCenterHtml(html, sizeof(html), body, "Prefix");
	LRCenterHtml(iSlot, html, 5.0f);
}

void PhraseFormatHtml(char* out, size_t outSize, const char* phraseKey, ...)
{
	va_list va;
	va_start(va, phraseKey);
	FormatPhraseHtmlBody(out, outSize, phraseKey, va);
	va_end(va);
}

void LRCenterBody(int iSlot, const char* htmlBody, float durationSec)
{
	char html[4096];
	WrapCenterHtml(html, sizeof(html), htmlBody ? htmlBody : "", "Prefix");
	LRCenterHtml(iSlot, html, durationSec);
}

void LRCenterFormat(int iSlot, float durationSec, const char* fmt, ...)
{
	char body[1024];
	va_list va;
	va_start(va, fmt);
	V_vsnprintf(body, sizeof(body), fmt, va);
	va_end(va);

	char html[1200];
	WrapCenterHtml(html, sizeof(html), body, "Prefix");
	LRCenterHtml(iSlot, html, durationSec);
}

static void SendChatToMask(uint64_t mask, const char* text)
{
	if (!g_pNetworkMessages || !g_pGameEventSystem || !mask)
		return;

	INetworkMessageInternal* pMsg = g_pNetworkMessages->FindNetworkMessagePartial("TextMsg");
	if (!pMsg)
		return;

	auto* data = pMsg->AllocateMessage()->ToPB<CUserMessageTextMsg>();
	data->set_dest(HUD_DEST_CHAT);
	data->add_param(text);

	uint64 clients = mask;
	g_pGameEventSystem->PostEventAbstract(CSplitScreenSlot(-1), false, LR_MAXPLAYERS, &clients,
		pMsg, data, 0, NetChannelBufType_t::BUF_RELIABLE);

	delete data;
}

static void FormatFinal(char* out, size_t outSize, const char* body)
{
	V_snprintf(out, outSize, " %s %s", Phrase("Prefix"), body);
}

void LRPrint(int iSlot, const char* fmt, ...)
{
	char body[512];
	va_list va;
	va_start(va, fmt);
	V_vsnprintf(body, sizeof(body), fmt, va);
	va_end(va);

	if (iSlot < 0 || iSlot >= LR_MAXPLAYERS)
	{
		ConMsg("[LR] %s\n", body);
		return;
	}

	char final_[600];
	FormatFinal(final_, sizeof(final_), body);
	SendChatToMask(1ull << iSlot, final_);
}

void LRPrintAll(const char* fmt, ...)
{
	char body[512];
	va_list va;
	va_start(va, fmt);
	V_vsnprintf(body, sizeof(body), fmt, va);
	va_end(va);

	uint64_t mask = 0;
	for (int i = 0; i < LR_MAXPLAYERS; i++)
	{
		CEntityInstance* pController = GetControllerBySlot(i);
		if (pController && g_Players[i].steam64)
			mask |= 1ull << i;
	}

	char final_[600];
	FormatFinal(final_, sizeof(final_), body);
	SendChatToMask(mask, final_);
}

void LRPrintPhrase(int iSlot, const char* phraseKey, ...)
{
	char body[512];
	va_list va;
	va_start(va, phraseKey);
	V_vsnprintf(body, sizeof(body), Phrase(phraseKey), va);
	va_end(va);

	if (iSlot < 0 || iSlot >= LR_MAXPLAYERS)
	{
		ConMsg("[LR] %s\n", body);
		return;
	}

	char final_[600];
	FormatFinal(final_, sizeof(final_), body);
	SendChatToMask(1ull << iSlot, final_);
}

void LRPrintAllPhrase(const char* phraseKey, ...)
{
	char body[512];
	va_list va;
	va_start(va, phraseKey);
	V_vsnprintf(body, sizeof(body), Phrase(phraseKey), va);
	va_end(va);

	uint64_t mask = 0;
	for (int i = 0; i < LR_MAXPLAYERS; i++)
	{
		CEntityInstance* pController = GetControllerBySlot(i);
		if (pController && g_Players[i].steam64)
			mask |= 1ull << i;
	}

	char final_[600];
	FormatFinal(final_, sizeof(final_), body);
	SendChatToMask(mask, final_);
}
