#include "lr_core.h"

#include <map>
#include <vector>

#include <convar.h>
#include <interfaces/interfaces.h>
#include <schemasystem/schemasystem.h>

#include "chat.h"
#include "commands.h"
#include "config.h"
#include "db.h"
#include "events.h"
#include "imultiaddonmanager.h"
#include "players.h"
#include "menu.h"
#include "tab.h"
#include "vtable_finder.h"

LRCorePlugin g_LRPlugin;
PLUGIN_EXPOSE(LRCorePlugin, g_LRPlugin);

IVEngineServer2* g_pEngine = nullptr;
ISource2Server* g_pServer = nullptr;
IServerGameClients* g_pGameClients = nullptr;
IGameEventSystem* g_pGameEventSystem = nullptr;
IGameEventManager2* g_pGameEventManager = nullptr;
INetworkServerService* g_pNetServerService = nullptr;
IGameResourceService* g_pGameResourceService = nullptr;
CGameEntitySystem* g_pGameEntitySystem = nullptr;

// SDK code (entity2) links against this free function
CGameEntitySystem* GameEntitySystem()
{
	return g_pGameEntitySystem;
}

LRSettings g_Cfg;
LRTabSettings g_TabCfg;
DBConfig g_DBConfig;

bool g_bCoreReady = false;
static bool g_bConfigsOk = false;
static void* g_pEventMgrVtbl = nullptr;
static void* g_pEntSysVtbl = nullptr;

#ifdef _WIN32
#define SERVER_LIB "server.dll"
#else
#define SERVER_LIB "/libserver.so"
#endif

// Engine only passes this by reference; KHook needs a complete type for sizeof.
class GameSessionConfiguration_t
{
};

// Fake object whose first word is a vtable pointer — for KHook::AddGlobal
// (equivalent to old SourceHook DVPHOOK on an RTTI-found vtable).
template <typename CLASS, typename RETURN, typename... ARGS>
static void AddGlobalByVtbl(KHook::Virtual<CLASS, RETURN, ARGS...>& hook, void* vtbl)
{
	struct { void* v; } fake{ vtbl };
	hook.AddGlobal(reinterpret_cast<CLASS*>(&fake));
}

template <typename CLASS, typename RETURN, typename... ARGS>
static void RemoveGlobalByVtbl(KHook::Virtual<CLASS, RETURN, ARGS...>& hook, void* vtbl)
{
	if (!vtbl)
		return;
	struct { void* v; } fake{ vtbl };
	hook.RemoveGlobal(reinterpret_cast<CLASS*>(&fake));
}

LRCorePlugin::LRCorePlugin() :
	m_GameFrame(&ISource2Server::GameFrame, this, nullptr, &LRCorePlugin::Hook_GameFrame),
	m_ClientPutInServer(&IServerGameClients::ClientPutInServer, this, nullptr, &LRCorePlugin::Hook_ClientPutInServer),
	m_ClientDisconnect(&IServerGameClients::ClientDisconnect, this, nullptr, &LRCorePlugin::Hook_ClientDisconnect),
	m_DispatchConCommand(&ICvar::DispatchConCommand, this, &LRCorePlugin::Hook_DispatchConCommand, nullptr),
	m_StartupServer(&INetworkServerService::StartupServer, this, nullptr, &LRCorePlugin::Hook_StartupServer),
	m_LoadEventsFromFile(&IGameEventManager2::LoadEventsFromFile, this, &LRCorePlugin::Hook_LoadEventsFromFile, nullptr),
	m_EntitySystemSpawn(&CEntitySystem::Spawn, this, nullptr, &LRCorePlugin::Hook_EntitySystemSpawn)
{
}

void LR_Log(const char* fmt, ...)
{
	char buf[512];
	va_list va;
	va_start(va, fmt);
	V_vsnprintf(buf, sizeof(buf), fmt, va);
	va_end(va);
	ConColorMsg(Color(80, 200, 120, 255), "[LR] %s\n", buf);
}

// ---------------------------------------------------------------------------
// Public API (ILRCoreApi)
// ---------------------------------------------------------------------------

class LRCoreApi final : public ILRCoreApi
{
public:
	bool IsReady() override { return g_bCoreReady; }
	bool IsPlayerLoaded(int iSlot) override
	{
		return iSlot >= 0 && iSlot < LR_MAXPLAYERS && g_Players[iSlot].loaded;
	}
	int GetExp(int iSlot) override { return IsPlayerLoaded(iSlot) ? g_Players[iSlot].st.exp : 0; }
	int GetLevel(int iSlot) override { return IsPlayerLoaded(iSlot) ? g_Players[iSlot].level : 0; }
	uint64_t GetSteam64(int iSlot) override
	{
		return (iSlot >= 0 && iSlot < LR_MAXPLAYERS) ? g_Players[iSlot].steam64 : 0;
	}
	int GetStat(int iSlot, int statId) override
	{
		if (!IsPlayerLoaded(iSlot))
			return 0;
		PlayerInfo& p = g_Players[iSlot];
		switch (statId)
		{
			case LR_STAT_KILLS: return p.st.kills;
			case LR_STAT_DEATHS: return p.st.deaths;
			case LR_STAT_SHOOTS: return p.st.shoots;
			case LR_STAT_HITS: return p.st.hits;
			case LR_STAT_HEADSHOTS: return p.st.headshots;
			case LR_STAT_ASSISTS: return p.st.assists;
			case LR_STAT_ROUND_WIN: return p.st.roundWin;
			case LR_STAT_ROUND_LOSE: return p.st.roundLose;
			case LR_STAT_PLAYTIME: return (int)TotalPlaytime(iSlot);
			case LR_STAT_POS_TOP: return p.posTop;
			case LR_STAT_POS_TOPTIME: return p.posTopTime;
			case LR_STAT_SESSION_TIME: return (int)SessionTime(iSlot);
		}
		return 0;
	}
	bool GiveExp(int iSlot, int amount) override
	{
		return ChangeExp(iSlot, amount, amount >= 0 ? "AdminGive" : "AdminTake", g_bAdminExpBypass);
	}

	// --- stats -------------------------------------------------------------

	bool GetPlayerStats(int iSlot, LRPlayerStats& out) override
	{
		if (!IsPlayerLoaded(iSlot))
			return false;

		PlayerInfo& p = g_Players[iSlot];
		out = LRPlayerStats{};

		out.steam64 = p.steam64;
		V_strncpy(out.steamId, p.steamId, sizeof(out.steamId));
		V_strncpy(out.name, p.name, sizeof(out.name));

		out.level      = p.level;
		out.exp        = p.st.exp;
		out.kills      = p.st.kills;
		out.deaths     = p.st.deaths;
		out.headshots  = p.st.headshots;
		out.assists    = p.st.assists;
		out.shoots     = p.st.shoots;
		out.hits       = p.st.hits;
		out.roundWin   = p.st.roundWin;
		out.roundLose  = p.st.roundLose;
		out.kd         = StatKD(p.st.kills, p.st.deaths);

		out.posTop       = p.posTop;
		out.posTopTime   = p.posTopTime;
		out.totalPlayers = g_iDBCountPlayers;

		out.playtime    = TotalPlaytime(iSlot);
		out.sessionTime = SessionTime(iSlot);

		out.sessExp       = p.sess.exp;
		out.sessKills     = p.sess.kills;
		out.sessDeaths    = p.sess.deaths;
		out.sessHeadshots = p.sess.headshots;
		out.sessAssists   = p.sess.assists;
		out.sessKD        = StatKD(p.sess.kills, p.sess.deaths);
		out.sessPosTop    = p.sessPosTop;

		return true;
	}

	int GetPosTop(int iSlot) override      { return IsPlayerLoaded(iSlot) ? g_Players[iSlot].posTop : 0; }
	int GetTotalPlayers() override         { return g_iDBCountPlayers; }
	int GetKills(int iSlot) override       { return IsPlayerLoaded(iSlot) ? g_Players[iSlot].st.kills : 0; }
	int GetDeaths(int iSlot) override      { return IsPlayerLoaded(iSlot) ? g_Players[iSlot].st.deaths : 0; }
	int GetHeadshots(int iSlot) override   { return IsPlayerLoaded(iSlot) ? g_Players[iSlot].st.headshots : 0; }
	float GetKD(int iSlot) override
	{
		if (!IsPlayerLoaded(iSlot))
			return 0.0f;
		return StatKD(g_Players[iSlot].st.kills, g_Players[iSlot].st.deaths);
	}
	int64_t GetPlaytime(int iSlot) override    { return IsPlayerLoaded(iSlot) ? TotalPlaytime(iSlot) : 0; }
	int64_t GetSessionTime(int iSlot) override { return IsPlayerLoaded(iSlot) ? SessionTime(iSlot) : 0; }

	int FindSlot(uint64_t steam64) override { return FindSlotBySteam64(steam64); }

	void HookCoreReady(SourceMM::PluginId id, FnCoreReady fn) override { m_coreReady[id].push_back(fn); }
	void HookLevelChanged(SourceMM::PluginId id, FnLevelChanged fn) override { m_levelChanged[id].push_back(fn); }
	void HookExpChanged(SourceMM::PluginId id, FnExpChanged fn) override { m_expChanged[id].push_back(fn); }
	void HookPlayerLoaded(SourceMM::PluginId id, FnPlayerLoaded fn) override { m_playerLoaded[id].push_back(fn); }
	void UnhookAll(SourceMM::PluginId id) override
	{
		m_coreReady.erase(id);
		m_levelChanged.erase(id);
		m_expChanged.erase(id);
		m_playerLoaded.erase(id);
	}

	std::map<int, std::vector<FnCoreReady>> m_coreReady;
	std::map<int, std::vector<FnLevelChanged>> m_levelChanged;
	std::map<int, std::vector<FnExpChanged>> m_expChanged;
	std::map<int, std::vector<FnPlayerLoaded>> m_playerLoaded;
};

static LRCoreApi g_Api;

void ApiFireCoreReady()
{
	for (auto& [id, fns] : g_Api.m_coreReady)
		for (auto& fn : fns)
			if (fn) fn();
}

void ApiFireLevelChanged(int iSlot, int newLevel, int oldLevel)
{
	for (auto& [id, fns] : g_Api.m_levelChanged)
		for (auto& fn : fns)
			if (fn) fn(iSlot, newLevel, oldLevel);
}

void ApiFireExpChanged(int iSlot, int delta, int newExp)
{
	for (auto& [id, fns] : g_Api.m_expChanged)
		for (auto& fn : fns)
			if (fn) fn(iSlot, delta, newExp);
}

void ApiFirePlayerLoaded(int iSlot, uint64_t steam64)
{
	for (auto& [id, fns] : g_Api.m_playerLoaded)
		for (auto& fn : fns)
			if (fn) fn(iSlot, steam64);
}

// ---------------------------------------------------------------------------
// Plugin
// ---------------------------------------------------------------------------

bool LRCorePlugin::Load(PluginId id, ISmmAPI* ismm, char* error, size_t maxlen, bool late)
{
	PLUGIN_SAVEVARS();

	GET_V_IFACE_CURRENT(GetEngineFactory, g_pCVar, ICvar, CVAR_INTERFACE_VERSION);
	GET_V_IFACE_ANY(GetEngineFactory, g_pSchemaSystem, ISchemaSystem, SCHEMASYSTEM_INTERFACE_VERSION);
	GET_V_IFACE_CURRENT(GetFileSystemFactory, g_pFullFileSystem, IFileSystem, FILESYSTEM_INTERFACE_VERSION);
	GET_V_IFACE_CURRENT(GetEngineFactory, g_pEngine, IVEngineServer2, SOURCE2ENGINETOSERVER_INTERFACE_VERSION);
	GET_V_IFACE_CURRENT(GetServerFactory, g_pServer, ISource2Server, SOURCE2SERVER_INTERFACE_VERSION);
	GET_V_IFACE_ANY(GetServerFactory, g_pGameClients, IServerGameClients, SOURCE2GAMECLIENTS_INTERFACE_VERSION);
	GET_V_IFACE_CURRENT(GetEngineFactory, g_pNetServerService, INetworkServerService, NETWORKSERVERSERVICE_INTERFACE_VERSION);
	GET_V_IFACE_ANY(GetEngineFactory, g_pGameEventSystem, IGameEventSystem, GAMEEVENTSYSTEM_INTERFACE_VERSION);
	GET_V_IFACE_CURRENT(GetEngineFactory, g_pNetworkMessages, INetworkMessages, NETWORKMESSAGES_INTERFACE_VERSION);
	GET_V_IFACE_CURRENT(GetEngineFactory, g_pGameResourceService, IGameResourceService, GAMERESOURCESERVICESERVER_INTERFACE_VERSION);

	g_SMAPI->AddListener(this, this);

	m_GameFrame.Add(g_pServer);
	m_ClientPutInServer.Add(g_pGameClients);
	m_ClientDisconnect.Add(g_pGameClients);
	m_DispatchConCommand.Add(g_pCVar);
	m_StartupServer.Add(g_pNetServerService);

	// Capture engine singletons without per-update signatures: hook a virtual
	// through the vtable found by RTTI and grab `this` on the first call.
	g_pEventMgrVtbl = FindVirtualTable(SERVER_LIB, "CGameEventManager");
	if (!g_pEventMgrVtbl)
	{
		V_strncpy(error, "Failed to locate CGameEventManager vtable", maxlen);
		return false;
	}
	AddGlobalByVtbl(m_LoadEventsFromFile, g_pEventMgrVtbl);

	g_pEntSysVtbl = FindVirtualTable(SERVER_LIB, "CGameEntitySystem");
	if (!g_pEntSysVtbl)
	{
		V_strncpy(error, "Failed to locate CGameEntitySystem vtable", maxlen);
		return false;
	}
	AddGlobalByVtbl(m_EntitySystemSpawn, g_pEntSysVtbl);

	ConVar_Register(FCVAR_RELEASE | FCVAR_GAMEDLL);

	return true;
}

// Retried from AllPluginsLoaded and every StartupServer until mounted.
static bool g_bWorkshopAddonOk = false;
static bool g_bWorkshopAddonLoggedEmpty = false;

static void Tab_RegisterWorkshopAddon(bool bFromMapStart)
{
	if (!g_TabCfg.workshopId[0])
	{
		if (!g_bWorkshopAddonLoggedEmpty)
		{
			LR_Log("tab.ini workshop_id is empty — skillgroup50+ will not be delivered "
				"(set workshop_id and mm_extra_addons)");
			g_bWorkshopAddonLoggedEmpty = true;
		}
		return;
	}

	if (g_bWorkshopAddonOk)
		return;

	IMultiAddonManager* pMam = (IMultiAddonManager*)g_SMAPI->MetaFactory(
		MULTIADDONMANAGER_INTERFACE, nullptr, nullptr);
	if (!pMam)
	{
		LR_Log("workshop_id=%s set, but MultiAddonManager is not loaded — "
			"clients will not receive skillgroup SVGs", g_TabCfg.workshopId);
		return;
	}

	// Server-mounted list (shows up as Refreshing addons (ID)). Client-only
	// addons leave Refreshing addons () empty and race Panorama FILEOPEN.
	const bool added = pMam->AddAddon(g_TabCfg.workshopId, /*bRefresh=*/true);
	const bool mounted = pMam->IsAddonMounted(g_TabCfg.workshopId);
	const bool ugc = pMam->HasUGCConnection();

	if (mounted)
	{
		g_bWorkshopAddonOk = true;
		LR_Log("workshop addon %s mounted via MultiAddonManager (AddAddon=%s)",
			g_TabCfg.workshopId, added ? "ok" : "false");
		return;
	}

	if (ugc)
	{
		pMam->DownloadAddon(g_TabCfg.workshopId, /*bImportant=*/true, /*bForce=*/false);
		LR_Log("requested MultiAddonManager download+mount of %s "
			"(AddAddon=%s, map may reload when ready). Also set mm_extra_addons \"%s\"",
			g_TabCfg.workshopId, added ? "ok" : "false", g_TabCfg.workshopId);
	}
	else if (bFromMapStart)
	{
		LR_Log("waiting for MultiAddonManager UGC (Steam) to mount %s — "
			"put mm_extra_addons \"%s\" in multiaddonmanager.cfg",
			g_TabCfg.workshopId, g_TabCfg.workshopId);
	}
	else
	{
		LR_Log("MultiAddonManager has no UGC yet for %s; will retry on map start. "
			"Ensure mm_extra_addons \"%s\"",
			g_TabCfg.workshopId, g_TabCfg.workshopId);
	}
}

void LRCorePlugin::AllPluginsLoaded()
{
	g_bConfigsOk = LoadAllConfigs();
	if (!g_bConfigsOk)
	{
		LR_Log("FATAL: config load failed, plugin is idle");
		return;
	}

	if (!LoadDatabaseConfig(g_DBConfig))
	{
		LR_Log("FATAL: database config invalid, plugin is idle");
		g_bConfigsOk = false;
		return;
	}

	g_bWorkshopAddonOk = false;
	g_bWorkshopAddonLoggedEmpty = false;
	Tab_RegisterWorkshopAddon(/*bFromMapStart=*/false);

	DB_Start(g_DBConfig);
	DB_Bootstrap();
}

bool LRCorePlugin::Unload(char* error, size_t maxlen)
{
	// flush stats of everyone online
	for (int i = 0; i < LR_MAXPLAYERS; i++)
	{
		if (g_Players[i].loaded)
			SavePlayer(i, true);
	}

	Events_Unregister();

	m_GameFrame.Remove(g_pServer);
	m_ClientPutInServer.Remove(g_pGameClients);
	m_ClientDisconnect.Remove(g_pGameClients);
	m_DispatchConCommand.Remove(g_pCVar);
	m_StartupServer.Remove(g_pNetServerService);
	RemoveGlobalByVtbl(m_LoadEventsFromFile, g_pEventMgrVtbl);
	RemoveGlobalByVtbl(m_EntitySystemSpawn, g_pEntSysVtbl);
	g_pEventMgrVtbl = nullptr;
	g_pEntSysVtbl = nullptr;

	DB_Stop(); // drains queued saves before exiting

	ConVar_Unregister();
	return true;
}

void* LRCorePlugin::OnMetamodQuery(const char* iface, int* ret)
{
	if (!strcmp(iface, LRCORE_INTERFACE))
	{
		if (ret)
			*ret = META_IFACE_OK;
		return &g_Api;
	}
	if (ret)
		*ret = META_IFACE_FAILED;
	return nullptr;
}

// ---------------------------------------------------------------------------
// Hooks
// ---------------------------------------------------------------------------

KHook::Return<void> LRCorePlugin::Hook_GameFrame(ISource2Server*, bool simulating, bool bFirstTick, bool bLastTick)
{
	DB_ProcessCallbacks();

	if (g_bConfigsOk)
	{
		Events_TryRegister();
		Commands_ProcessQueue();
		Menu_OnGameFrame();
		Center_OnGameFrame();
		TickActivePlaytime();
		PulseOnlinePresence();
		GiveTimeExp();
		Tab_OnGameFrame();
	}

	return { KHook::Action::Ignore };
}

KHook::Return<void> LRCorePlugin::Hook_ClientPutInServer(IServerGameClients*, CPlayerSlot slot, char const* pszName, int type, uint64 xuid)
{
	int iSlot = slot.Get();
	if (iSlot >= 0 && iSlot < LR_MAXPLAYERS)
	{
		if (!xuid) // bot
		{
			g_Players[iSlot].Reset();
		}
		else
		{
			LoadPlayer(iSlot, xuid);
			if (pszName)
				V_snprintf(g_Players[iSlot].name, sizeof(g_Players[iSlot].name), "%s", pszName);

			CheckAllowStatistic();
		}
	}

	return { KHook::Action::Ignore };
}

KHook::Return<void> LRCorePlugin::Hook_ClientDisconnect(IServerGameClients*, CPlayerSlot slot, ENetworkDisconnectionReason reason, const char* pszName, uint64 xuid, const char* pszNetworkID)
{
	int iSlot = slot.Get();
	if (iSlot >= 0 && iSlot < LR_MAXPLAYERS)
	{
		ClearPlayerOnlinePresence(iSlot);
		SavePlayer(iSlot, true);
		Menu_OnDisconnect(iSlot);
		Commands_OnDisconnect(iSlot);
	}

	return { KHook::Action::Ignore };
}

KHook::Return<void> LRCorePlugin::Hook_DispatchConCommand(ICvar*, ConCommandRef cmd, const CCommandContext& ctx, const CCommand& args)
{
	int iSlot = ctx.GetPlayerSlot().Get();
	if (iSlot >= 0 && args.ArgC() > 1)
	{
		const char* name = args[0];
		if (!V_stricmp(name, "say") || !V_stricmp(name, "say_team"))
		{
			const char* text = args[1];
			if (Commands_IsAwaitingResetConfirm(iSlot))
				Commands_QueuePendingSay(iSlot, text);
			else if (Commands_IsChatCommand(iSlot, text))
			{
				// Run it next frame: this hook fires before the engine echoes
				// the player's own line, so printing here would put the answer
				// above the "!session" they typed.
				Commands_QueueChat(iSlot, text);

				if (text[0] == '/')
					return { KHook::Action::Supersede }; // silent command
			}
		}
	}
	return { KHook::Action::Ignore };
}

KHook::Return<void> LRCorePlugin::Hook_StartupServer(INetworkServerService*, const GameSessionConfiguration_t& config, ISource2WorldSession*, const char*)
{
	// Steam/UGC is often ready only after the first map — retry ranks workshop mount.
	Tab_RegisterWorkshopAddon(/*bFromMapStart=*/true);
	Events_OnStartupServer();
	return { KHook::Action::Ignore };
}

KHook::Return<int> LRCorePlugin::Hook_LoadEventsFromFile(IGameEventManager2* pThis, const char* filename, bool bSearchAll)
{
	if (!g_pGameEventManager && pThis)
	{
		g_pGameEventManager = pThis;
		LR_Log("captured game event manager");
	}
	return { KHook::Action::Ignore };
}

KHook::Return<void> LRCorePlugin::Hook_EntitySystemSpawn(CEntitySystem* pThis, int nCount, const EntitySpawnInfo_t* pInfo)
{
	CGameEntitySystem* pSys = reinterpret_cast<CGameEntitySystem*>(pThis);
	if (pSys && g_pGameEntitySystem != pSys)
	{
		g_pGameEntitySystem = pSys;
		LR_Log("captured game entity system");
	}
	return { KHook::Action::Ignore };
}
