#include "lr_core.h"

#include <cstdarg>

#include <convar.h>
#include <interfaces/interfaces.h>
#include <schemasystem/schemasystem.h>

#include "commands.h"
#include "config.h"
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
INetworkServerService* g_pNetServerService = nullptr;
IGameResourceService* g_pGameResourceService = nullptr;
CGameEntitySystem* g_pGameEntitySystem = nullptr;

CGameEntitySystem* GameEntitySystem()
{
	return g_pGameEntitySystem;
}

LRTabSettings g_TabCfg;

bool g_bCoreReady = false;
static bool g_bConfigsOk = false;
static void* g_pEntSysVtbl = nullptr;

#ifdef _WIN32
#define SERVER_LIB "server.dll"
#else
#define SERVER_LIB "/libserver.so"
#endif

class GameSessionConfiguration_t
{
};

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
	m_StartupServer(&INetworkServerService::StartupServer, this, nullptr, &LRCorePlugin::Hook_StartupServer),
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
	m_StartupServer.Add(g_pNetServerService);

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

static bool g_bWorkshopAddonOk = false;
static bool g_bWorkshopAddonLoggedEmpty = false;

static void Tab_RegisterWorkshopAddon(bool bFromMapStart)
{
	if (!g_TabCfg.workshopId[0])
	{
		if (!g_bWorkshopAddonLoggedEmpty)
		{
			LR_Log("tab.ini workshop_id empty — skipping MAM AddAddon");
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
		LR_Log("workshop_id=%s set, but MultiAddonManager is not loaded", g_TabCfg.workshopId);
		return;
	}

	const bool added = pMam->AddAddon(g_TabCfg.workshopId, /*bRefresh=*/true);
	const bool mounted = pMam->IsAddonMounted(g_TabCfg.workshopId);
	const bool ugc = pMam->HasUGCConnection();

	if (mounted)
	{
		g_bWorkshopAddonOk = true;
		LR_Log("workshop addon %s mounted (AddAddon=%s)",
			g_TabCfg.workshopId, added ? "ok" : "false");
		return;
	}

	if (ugc)
	{
		pMam->DownloadAddon(g_TabCfg.workshopId, /*bImportant=*/true, /*bForce=*/false);
		LR_Log("requested MAM download of %s — also set mm_extra_addons \"%s\"",
			g_TabCfg.workshopId, g_TabCfg.workshopId);
	}
	else if (bFromMapStart)
	{
		LR_Log("waiting for MAM UGC for %s", g_TabCfg.workshopId);
	}
}

void LRCorePlugin::AllPluginsLoaded()
{
	LoadTabConfig();
	g_bConfigsOk = true;
	g_bCoreReady = true;

	g_bWorkshopAddonOk = false;
	g_bWorkshopAddonLoggedEmpty = false;
	Tab_RegisterWorkshopAddon(/*bFromMapStart=*/false);

	LR_Log("TAB-only mode ready (XP/DB/menus/wallet → LevelsRanks C#)");
}

bool LRCorePlugin::Unload(char* error, size_t maxlen)
{
	(void)error;
	(void)maxlen;

	m_GameFrame.Remove(g_pServer);
	m_ClientPutInServer.Remove(g_pGameClients);
	m_ClientDisconnect.Remove(g_pGameClients);
	m_StartupServer.Remove(g_pNetServerService);
	RemoveGlobalByVtbl(m_EntitySystemSpawn, g_pEntSysVtbl);
	g_pEntSysVtbl = nullptr;

	for (int i = 0; i < LR_MAXPLAYERS; i++)
		g_Players[i].Reset();

	ConVar_Unregister();
	return true;
}

void* LRCorePlugin::OnMetamodQuery(const char* iface, int* ret)
{
	(void)iface;
	if (ret)
		*ret = META_IFACE_FAILED;
	return nullptr;
}

KHook::Return<void> LRCorePlugin::Hook_GameFrame(ISource2Server*, bool simulating, bool bFirstTick, bool bLastTick)
{
	(void)simulating;
	(void)bFirstTick;
	(void)bLastTick;

	if (g_bConfigsOk)
		Tab_OnGameFrame();

	return { KHook::Action::Ignore };
}

KHook::Return<void> LRCorePlugin::Hook_ClientPutInServer(IServerGameClients*, CPlayerSlot slot, char const* pszName, int type, uint64 xuid)
{
	(void)type;
	int iSlot = slot.Get();
	if (iSlot >= 0 && iSlot < LR_MAXPLAYERS)
	{
		g_Players[iSlot].Reset();
		if (xuid)
		{
			g_Players[iSlot].steam64 = xuid;
			Steam64ToSteamId(xuid, g_Players[iSlot].steamId, sizeof(g_Players[iSlot].steamId));
			g_Players[iSlot].level = 1;
			g_Players[iSlot].st.exp = 0;
			g_Players[iSlot].loaded = true;
			if (pszName)
				V_snprintf(g_Players[iSlot].name, sizeof(g_Players[iSlot].name), "%s", pszName);
			Tab_OnPlayerLoaded(iSlot);
		}
	}
	return { KHook::Action::Ignore };
}

KHook::Return<void> LRCorePlugin::Hook_ClientDisconnect(IServerGameClients*, CPlayerSlot slot, ENetworkDisconnectionReason reason, const char* pszName, uint64 xuid, const char* pszNetworkID)
{
	(void)reason;
	(void)pszName;
	(void)xuid;
	(void)pszNetworkID;
	int iSlot = slot.Get();
	if (iSlot >= 0 && iSlot < LR_MAXPLAYERS)
		g_Players[iSlot].Reset();
	return { KHook::Action::Ignore };
}

KHook::Return<void> LRCorePlugin::Hook_StartupServer(INetworkServerService*, const GameSessionConfiguration_t& config, ISource2WorldSession*, const char*)
{
	(void)config;
	Tab_RegisterWorkshopAddon(/*bFromMapStart=*/true);
	return { KHook::Action::Ignore };
}

KHook::Return<void> LRCorePlugin::Hook_EntitySystemSpawn(CEntitySystem* pThis, int nCount, const EntitySpawnInfo_t* pInfo)
{
	(void)nCount;
	(void)pInfo;
	CGameEntitySystem* pSys = reinterpret_cast<CGameEntitySystem*>(pThis);
	if (pSys && g_pGameEntitySystem != pSys)
	{
		g_pGameEntitySystem = pSys;
		LR_Log("captured game entity system");
	}
	return { KHook::Action::Ignore };
}
