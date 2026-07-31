// Minimal copy of MultiAddonManager's public interface (MultiAddonManager003).
// Soft dependency: queried at runtime via MetaFactory; build does not link MAM.
#pragma once

#include <cstdint>

#define MULTIADDONMANAGER_INTERFACE "MultiAddonManager003"

class IMultiAddonManager
{
public:
	virtual bool AddAddon(const char* pszWorkshopID, bool bRefresh = false) = 0;
	virtual bool RemoveAddon(const char* pszWorkshopID, bool bRefresh = false) = 0;
	virtual bool IsAddonMounted(const char* pszWorkshopID, bool bCheckWorkshopMap = false) = 0;
	virtual bool DownloadAddon(const char* pszWorkshopID, bool bImportant = false, bool bForce = true) = 0;
	virtual void RefreshAddons(bool bReloadMap = false) = 0;
	virtual void ClearAddons() = 0;
	virtual bool HasUGCConnection() = 0;
	virtual void AddClientAddon(const char* pszAddon, uint64 steamID64 = 0, bool bRefresh = false) = 0;
	virtual void RemoveClientAddon(const char* pszAddon, uint64 steamID64 = 0) = 0;
	virtual void ClearClientAddons(uint64 steamID64 = 0) = 0;
};
