#include "lr_core.h"
#include "config.h"

#include <KeyValues.h>
#include <filesystem.h>

static const char* TAB_PATH = "addons/lr_core/configs/tab.ini";

void LoadTabConfig()
{
	g_TabCfg = LRTabSettings{};

	KeyValues* kv = new KeyValues("LR_Tab");
	KeyValues::AutoDelete autoDelete(kv);

	if (!kv->LoadFromFile(g_pFullFileSystem, TAB_PATH))
	{
		Warning("[LR] %s not found, TAB ranks disabled\n", TAB_PATH);
		return;
	}

	g_TabCfg.enabled = kv->GetInt("tab_enabled", 1) != 0;
	g_TabCfg.wins    = kv->GetInt("tab_wins", 777);
	g_TabCfg.type    = kv->GetInt("tab_type", 12);

	const char* mode = kv->GetString("tab_mode", "competitive");
	g_TabCfg.mode = (!strcmp(mode, "persona") || !strcmp(mode, "1")) ? 1 : 0;

	V_snprintf(g_TabCfg.workshopId, sizeof(g_TabCfg.workshopId), "%s",
		kv->GetString("workshop_id", ""));
	g_TabCfg.iconsDelay     = kv->GetFloat("tab_icons_delay", 12.0f);
	g_TabCfg.iconsRefresh   = kv->GetFloat("tab_icons_refresh", 20.0f);
	g_TabCfg.revealInterval = kv->GetFloat("tab_reveal_interval", 2.0f);
	if (g_TabCfg.iconsDelay < 0.0f)
		g_TabCfg.iconsDelay = 0.0f;
	if (g_TabCfg.iconsRefresh < 0.0f)
		g_TabCfg.iconsRefresh = 0.0f;
	if (g_TabCfg.revealInterval < 0.0f)
		g_TabCfg.revealInterval = 0.0f;

	g_TabCfg.values.clear();
	KeyValues* values = kv->FindKey("tab_values", false);
	if (values)
	{
		for (KeyValues* v = values->GetFirstSubKey(); v; v = v->GetNextKey())
			g_TabCfg.values.push_back(v->GetInt(nullptr, 0));
	}

	KeyValues* above = kv->FindKey("tab_above", false);
	if (above)
	{
		g_TabCfg.aboveType   = above->GetInt("type", g_TabCfg.type);
		g_TabCfg.aboveValue  = above->GetInt("value", (int)g_TabCfg.values.size());
		g_TabCfg.aboveUseExp = above->GetInt("use_exp", 0) != 0;
	}
	else
	{
		g_TabCfg.aboveType  = g_TabCfg.type;
		g_TabCfg.aboveValue = (int)g_TabCfg.values.size();
	}

	LR_Log("tab.ini loaded enabled=%d values=%d workshop=%s",
		g_TabCfg.enabled ? 1 : 0, (int)g_TabCfg.values.size(),
		g_TabCfg.workshopId[0] ? g_TabCfg.workshopId : "(empty)");
}
