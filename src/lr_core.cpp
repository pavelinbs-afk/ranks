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
static int g_iEventMgrHookId = 0;
static int g_iEntSysHookId = 0;

#ifdef _WIN32
#define SERVER_LIB "server.dll"
#else
#define SERVER_LIB "/libserver.so"
#endif

// engine passes this by reference only; sourcehook needs a complete type
class GameSessionConfiguration_t
{
};

SH_DECL_HOOK3_void(IServerGameDLL, GameFrame, SH_NOATTRIB, 0, bool, bool, bool);
SH_DECL_HOOK4_void(IServerGameClients, ClientPutInServer, SH_NOATTRIB, 0, CPlayerSlot, char const*, int, uint64);
SH_DECL_HOOK5_void(IServerGameClients, ClientDisconnect, SH_NOATTRIB, 0, CPlayerSlot, ENetworkDisconnectionReason, const char*, uint64, const char*);
SH_DECL_HOOK3_void(ICvar, DispatchConCommand, SH_NOATTRIB, 0, ConCommandRef, const CCommandContext&, const CCommand&);
SH_DECL_HOOK3_void(INetworkServerService, StartupServer, SH_NOATTRIB, 0, const GameSessionConfiguration_t&, ISource2WorldSession*, const char*);
SH_DECL_HOOK2(IGameEventManager2, LoadEventsFromFile, SH_NOATTRIB, 0, int, const char*, bool);
SH_DECL_HOOK2_void(CEntitySystem, Spawn, SH_NOATTRIB, 0, int, const EntitySpawnInfo_t*);

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
		return ChangeExp(iSlot, amount, amount >= 0 ? "AdminGive" : "AdminTake", true);
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

	SH_ADD_HOOK(IServerGameDLL, GameFrame, g_pServer, SH_MEMBER(this, &LRCorePlugin::Hook_GameFrame), true);
	SH_ADD_HOOK(IServerGameClients, ClientPutInServer, g_pGameClients, SH_MEMBER(this, &LRCorePlugin::Hook_ClientPutInServer), true);
	SH_ADD_HOOK(IServerGameClients, ClientDisconnect, g_pGameClients, SH_MEMBER(this, &LRCorePlugin::Hook_ClientDisconnect), true);
	SH_ADD_HOOK(ICvar, DispatchConCommand, g_pCVar, SH_MEMBER(this, &LRCorePlugin::Hook_DispatchConCommand), false);
	SH_ADD_HOOK(INetworkServerService, StartupServer, g_pNetServerService, SH_MEMBER(this, &LRCorePlugin::Hook_StartupServer), true);

	// Capture engine singletons without per-update signatures: hook a virtual
	// through the vtable found by RTTI and grab `this` on the first call.
	if (void* pEventMgrVtbl = FindVirtualTable(SERVER_LIB, "CGameEventManager"))
	{
		g_iEventMgrHookId = SH_ADD_DVPHOOK(IGameEventManager2, LoadEventsFromFile,
			reinterpret_cast<IGameEventManager2*>(pEventMgrVtbl),
			SH_MEMBER(this, &LRCorePlugin::Hook_LoadEventsFromFile), false);
	}
	else
	{
		V_strncpy(error, "Failed to locate CGameEventManager vtable", maxlen);
		return false;
	}

	if (void* pEntSysVtbl = FindVirtualTable(SERVER_LIB, "CGameEntitySystem"))
	{
		g_iEntSysHookId = SH_ADD_DVPHOOK(CEntitySystem, Spawn,
			reinterpret_cast<CEntitySystem*>(pEntSysVtbl),
			SH_MEMBER(this, &LRCorePlugin::Hook_EntitySystemSpawn), true);
	}
	else
	{
		V_strncpy(error, "Failed to locate CGameEntitySystem vtable", maxlen);
		return false;
	}

	ConVar_Register(FCVAR_RELEASE | FCVAR_GAMEDLL);

	return true;
}

static void Tab_RegisterWorkshopAddon()
{
	if (!g_TabCfg.workshopId[0])
		return;

	IMultiAddonManager* pMam = (IMultiAddonManager*)g_SMAPI->MetaFactory(
		MULTIADDONMANAGER_INTERFACE, nullptr, nullptr);
	if (!pMam)
	{
		LR_Log("workshop_id=%s set, but MultiAddonManager is not loaded — "
			"clients will not receive skillgroup SVGs", g_TabCfg.workshopId);
		return;
	}

	// Prefer mm_extra_addons (server-mounted): FaceitLevels / working setups use
	// this path. Client-only addons alone leave Refreshing addons () empty and
	// race with Panorama looking up skillgroup{N}.vsvg_c.
	pMam->AddAddon(g_TabCfg.workshopId, false);
	if (!pMam->IsAddonMounted(g_TabCfg.workshopId))
	{
		if (pMam->HasUGCConnection())
		{
			pMam->DownloadAddon(g_TabCfg.workshopId, /*bImportant=*/true, /*bForce=*/false);
			LR_Log("requested MultiAddonManager download+mount of %s (map will reload when ready)",
				g_TabCfg.workshopId);
		}
		else
		{
			LR_Log("MultiAddonManager has no UGC connection yet; ensure mm_extra_addons \"%s\" "
				"and run mm_download_addon %s after GC connects",
				g_TabCfg.workshopId, g_TabCfg.workshopId);
		}
	}
	else
	{
		LR_Log("workshop addon %s already mounted via MultiAddonManager", g_TabCfg.workshopId);
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

	Tab_RegisterWorkshopAddon();

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

	SH_REMOVE_HOOK(IServerGameDLL, GameFrame, g_pServer, SH_MEMBER(this, &LRCorePlugin::Hook_GameFrame), true);
	SH_REMOVE_HOOK(IServerGameClients, ClientPutInServer, g_pGameClients, SH_MEMBER(this, &LRCorePlugin::Hook_ClientPutInServer), true);
	SH_REMOVE_HOOK(IServerGameClients, ClientDisconnect, g_pGameClients, SH_MEMBER(this, &LRCorePlugin::Hook_ClientDisconnect), true);
	SH_REMOVE_HOOK(ICvar, DispatchConCommand, g_pCVar, SH_MEMBER(this, &LRCorePlugin::Hook_DispatchConCommand), false);
	SH_REMOVE_HOOK(INetworkServerService, StartupServer, g_pNetServerService, SH_MEMBER(this, &LRCorePlugin::Hook_StartupServer), true);
	if (g_iEventMgrHookId)
		SH_REMOVE_HOOK_ID(g_iEventMgrHookId);
	if (g_iEntSysHookId)
		SH_REMOVE_HOOK_ID(g_iEntSysHookId);

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

void LRCorePlugin::Hook_GameFrame(bool simulating, bool bFirstTick, bool bLastTick)
{
	DB_ProcessCallbacks();

	if (!g_bConfigsOk)
		return;

	Events_TryRegister();
	Commands_ProcessQueue(); // chat commands deferred by the say hook
	GiveTimeExp();
	Tab_OnGameFrame();
}

void LRCorePlugin::Hook_ClientPutInServer(CPlayerSlot slot, char const* pszName, int type, uint64 xuid)
{
	int iSlot = slot.Get();
	if (iSlot < 0 || iSlot >= LR_MAXPLAYERS)
		return;

	if (!xuid) // bot
	{
		g_Players[iSlot].Reset();
		return;
	}

	LoadPlayer(iSlot, xuid);
	if (pszName)
		V_snprintf(g_Players[iSlot].name, sizeof(g_Players[iSlot].name), "%s", pszName);

	CheckAllowStatistic();
}

void LRCorePlugin::Hook_ClientDisconnect(CPlayerSlot slot, ENetworkDisconnectionReason reason, const char* pszName, uint64 xuid, const char* pszNetworkID)
{
	int iSlot = slot.Get();
	if (iSlot < 0 || iSlot >= LR_MAXPLAYERS)
		return;

	SavePlayer(iSlot, true);
}

void LRCorePlugin::Hook_DispatchConCommand(ConCommandRef cmd, const CCommandContext& ctx, const CCommand& args)
{
	int iSlot = ctx.GetPlayerSlot().Get();
	if (iSlot >= 0 && args.ArgC() > 1)
	{
		const char* name = args[0];
		if (!V_stricmp(name, "say") || !V_stricmp(name, "say_team"))
		{
			const char* text = args[1];
			if (Commands_IsChatCommand(text))
			{
				// Run it next frame: this hook fires before the engine echoes
				// the player's own line, so printing here would put the answer
				// above the "!session" they typed.
				Commands_QueueChat(iSlot, text);

				if (text[0] == '/')
					RETURN_META(MRES_SUPERCEDE); // silent command
			}
		}
	}
	RETURN_META(MRES_IGNORED);
}

void LRCorePlugin::Hook_StartupServer(const GameSessionConfiguration_t& config, ISource2WorldSession*, const char*)
{
	Events_OnStartupServer();
}

int LRCorePlugin::Hook_LoadEventsFromFile(const char* filename, bool bSearchAll)
{
	if (!g_pGameEventManager)
	{
		g_pGameEventManager = META_IFACEPTR(IGameEventManager2);
		LR_Log("captured game event manager");
	}
	RETURN_META_VALUE(MRES_IGNORED, 0);
}

void LRCorePlugin::Hook_EntitySystemSpawn(int nCount, const EntitySpawnInfo_t* pInfo)
{
	CGameEntitySystem* pSys = reinterpret_cast<CGameEntitySystem*>(META_IFACEPTR(CEntitySystem));
	if (g_pGameEntitySystem != pSys)
	{
		g_pGameEntitySystem = pSys;
		LR_Log("captured game entity system");
	}
	RETURN_META(MRES_IGNORED);
}
