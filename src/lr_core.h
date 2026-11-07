#pragma once

#include <ISmmPlugin.h>
#include <tier0/dbg.h>
#include <tier1/strtools.h>
#include <eiface.h>
#include <iserver.h>
#include <igameevents.h>
#include <igameeventsystem.h>
#include <icvar.h>
#include <entity2/entitysystem.h>
#include <networksystem/inetworkmessages.h>

#include <cstdint>
#include <vector>

#define LR_MAXPLAYERS 64

class LRCorePlugin final : public ISmmPlugin, public IMetamodListener
{
public:
	LRCorePlugin();

	bool Load(PluginId id, ISmmAPI* ismm, char* error, size_t maxlen, bool late) override;
	bool Unload(char* error, size_t maxlen) override;
	void AllPluginsLoaded() override;
	void* OnMetamodQuery(const char* iface, int* ret) override;

	const char* GetAuthor() override		{ return "2k_x & pRfect"; }
	const char* GetName() override			{ return "[LR] Core TAB"; }
	const char* GetDescription() override	{ return "Levels Ranks TAB icons only (XP in LevelsRanks C#)"; }
	const char* GetURL() override			{ return ""; }
	const char* GetLicense() override		{ return "GPL"; }
	const char* GetVersion() override		{ return "3.0.1-tab"; }
	const char* GetDate() override			{ return __DATE__; }
	const char* GetLogTag() override		{ return "LR"; }

public:
	KHook::Return<void> Hook_GameFrame(ISource2Server*, bool simulating, bool bFirstTick, bool bLastTick);
	KHook::Return<void> Hook_ClientPutInServer(IServerGameClients*, CPlayerSlot slot, char const* pszName, int type, uint64 xuid);
	KHook::Return<void> Hook_ClientDisconnect(IServerGameClients*, CPlayerSlot slot, ENetworkDisconnectionReason reason, const char* pszName, uint64 xuid, const char* pszNetworkID);
	KHook::Return<void> Hook_StartupServer(INetworkServerService*, const GameSessionConfiguration_t& config, ISource2WorldSession*, const char*);
	KHook::Return<void> Hook_EntitySystemSpawn(CEntitySystem* pThis, int nCount, const EntitySpawnInfo_t* pInfo);

protected:
	KHook::Virtual<ISource2Server, void, bool, bool, bool> m_GameFrame;
	KHook::Virtual<IServerGameClients, void, CPlayerSlot, char const*, int, uint64> m_ClientPutInServer;
	KHook::Virtual<IServerGameClients, void, CPlayerSlot, ENetworkDisconnectionReason, const char*, uint64, const char*> m_ClientDisconnect;
	KHook::Virtual<INetworkServerService, void, const GameSessionConfiguration_t&, ISource2WorldSession*, const char*> m_StartupServer;
	KHook::Virtual<CEntitySystem, void, int, const EntitySpawnInfo_t*> m_EntitySystemSpawn;
};

struct LRTabSettings
{
	bool enabled = false;
	int  mode = 0;                 // 0 = competitive, 1 = persona
	int  wins = 777;
	int  type = 12;
	std::vector<int> values;
	int  aboveType = 12;
	int  aboveValue = 21;
	bool aboveUseExp = false;

	char workshopId[32] = {};
	float iconsDelay = 12.0f;
	float iconsRefresh = 20.0f;
	float revealInterval = 2.0f;
};

struct PlayerStats
{
	int exp = 0;
};

struct PlayerInfo
{
	bool  loaded = false;
	uint64_t steam64 = 0;
	char  steamId[24] = {};
	char  name[128] = {};

	int   level = 1;
	PlayerStats st;

	uint64_t oldButtons = 0;
	bool revealSent = false;
	int  tabOverride = 0;

	float tabIconsAt = 0.0f;
	float tabRefreshUntil = 0.0f;
	bool  tabIconsApplied = false;

	void Reset() { *this = PlayerInfo(); }
};

extern LRCorePlugin g_LRPlugin;

extern IVEngineServer2* g_pEngine;
extern ISource2Server* g_pServer;
extern IServerGameClients* g_pGameClients;
extern IGameEventSystem* g_pGameEventSystem;
extern INetworkServerService* g_pNetServerService;
extern CGameEntitySystem* g_pGameEntitySystem;

extern LRTabSettings g_TabCfg;
extern PlayerInfo g_Players[LR_MAXPLAYERS];
extern bool g_bCoreReady;
extern bool g_bCustomRoundActive;

inline CGlobalVars* GetGlobals()
{
	return g_pEngine ? g_pEngine->GetServerGlobals() : nullptr;
}

void LR_Log(const char* fmt, ...);

CEntityInstance* GetControllerBySlot(int iSlot);
int FindSlotBySteam64(uint64_t steam64);
void Steam64ToSteamId(uint64_t steam64, char* out, int outSize);
