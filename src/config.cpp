#include "lr_core.h"
#include "config.h"
#include "chat.h"

#include <KeyValues.h>
#include <filesystem.h>

static const char* SETTINGS_PATH = "addons/lr_core/configs/settings.ini";
static const char* TAB_PATH      = "addons/lr_core/configs/tab.ini";
static const char* DB_PATH       = "addons/lr_core/configs/database.ini";

static void LoadExpSection(KeyValues* kv)
{
	g_Cfg.expHeadshot      = kv->GetInt("lr_headshot", 1);
	g_Cfg.expAssist        = kv->GetInt("lr_assist", 1);
	g_Cfg.expSuicide       = kv->GetInt("lr_suicide", 0);
	g_Cfg.expTeamkill      = kv->GetInt("lr_teamkill", 0);
	g_Cfg.expRoundWin      = kv->GetInt("lr_winround", 2);
	g_Cfg.expRoundLose     = kv->GetInt("lr_loseround", 2);
	g_Cfg.expMvp           = kv->GetInt("lr_mvpround", 1);
	g_Cfg.expBombPlant     = kv->GetInt("lr_bombplanted", 2);
	g_Cfg.expBombDefuse    = kv->GetInt("lr_bombdefused", 2);
	g_Cfg.expBombDrop      = kv->GetInt("lr_bombdropped", 1);
	g_Cfg.expBombPickup    = kv->GetInt("lr_bombpickup", 1);
	g_Cfg.expHostageKill   = kv->GetInt("lr_hostagekilled", 0);
	g_Cfg.expHostageRescue = kv->GetInt("lr_hostagerescued", 2);
}

bool LoadSettings()
{
	KeyValues* kv = new KeyValues("LR_Settings");
	KeyValues::AutoDelete autoDelete(kv);

	if (!kv->LoadFromFile(g_pFullFileSystem, SETTINGS_PATH))
	{
		Warning("[LR] Failed to load %s\n", SETTINGS_PATH);
		return false;
	}

	KeyValues* main = kv->FindKey("MainSettings", false);
	if (!main)
	{
		Warning("[LR] MainSettings section missing in %s\n", SETTINGS_PATH);
		return false;
	}

	V_snprintf(g_Cfg.tableName, sizeof(g_Cfg.tableName), "%s", main->GetString("lr_table", "lvl_base"));
	V_snprintf(g_Cfg.language, sizeof(g_Cfg.language), "%s", main->GetString("lr_language", "ru"));
	g_Cfg.serverId         = main->GetInt("lr_server_id", 1);
	g_Cfg.typeStatistics   = main->GetInt("lr_type_statistics", 0);
	g_Cfg.minPlayers       = main->GetInt("lr_minplayers_count", 4);
	g_Cfg.showResetStats   = main->GetInt("lr_show_resetmystats", 1) != 0;
	g_Cfg.resetCooldown    = main->GetInt("lr_resetmystats_cooldown", 86400);
	g_Cfg.showUsualMessage = main->GetInt("lr_show_usualmessage", 1);
	g_Cfg.showSpawnMessage = main->GetInt("lr_show_spawnmessage", 1) != 0;
	g_Cfg.showLevelUp      = main->GetInt("lr_show_levelup_message", 1) != 0;
	g_Cfg.showLevelDown    = main->GetInt("lr_show_leveldown_message", 1) != 0;
	g_Cfg.showAllLevelUp   = main->GetInt("lr_show_all_levelup_message", 0) != 0;
	g_Cfg.showAllLevelDown = main->GetInt("lr_show_all_leveldown_message", 0) != 0;
	g_Cfg.showRankMessage  = main->GetInt("lr_show_rankmessage", 1) != 0;
	g_Cfg.topCount         = main->GetInt("lr_top_count", 10);
	g_Cfg.startPoints      = main->GetInt("lr_start_points", 0);
	g_Cfg.giveExpRoundEnd  = main->GetInt("lr_giveexp_roundend", 1) != 0;
	g_Cfg.blockWarmup      = main->GetInt("lr_block_warmup", 1) != 0;
	g_Cfg.allAgainstAll    = main->GetInt("lr_allagainst_all", 0) != 0;
	g_Cfg.cleanDbDays      = main->GetInt("lr_cleandb_days", 30);
	g_Cfg.saveMode         = main->GetInt("lr_db_savedataplayer_mode", 1);
	g_Cfg.timeExpAmount    = main->GetInt("lr_time_exp_amount", 0);
	g_Cfg.timeExpInterval  = main->GetInt("lr_time_exp_interval", 300);

	// Ranks
	KeyValues* ranks = kv->FindKey("Ranks", false);
	if (!ranks)
	{
		Warning("[LR] Ranks section missing in %s\n", SETTINGS_PATH);
		return false;
	}
	g_Cfg.ranksExp.clear();
	g_Cfg.ranksKey.clear();
	FOR_EACH_VALUE(ranks, v)
	{
		g_Cfg.ranksKey.emplace_back(v->GetName());
		g_Cfg.ranksExp.emplace_back(v->GetInt(nullptr));
	}
	if (g_Cfg.ranksExp.empty())
	{
		Warning("[LR] Ranks section is empty in %s\n", SETTINGS_PATH);
		return false;
	}

	// Exp sections
	switch (g_Cfg.typeStatistics)
	{
		case 0:
		{
			KeyValues* s = kv->FindKey("Funded_System", false);
			if (!s)
			{
				Warning("[LR] Funded_System section missing\n");
				return false;
			}
			g_Cfg.expKill    = s->GetInt("lr_kill", 5);
			g_Cfg.expKillBot = s->GetInt("lr_kill_is_bot", 2);
			g_Cfg.expDeath   = s->GetInt("lr_death", 3);
			g_Cfg.expDeathBot= s->GetInt("lr_death_is_bot", 2);
			LoadExpSection(s);
			break;
		}
		case 1:
		{
			KeyValues* s = kv->FindKey("Rating_Extended", false);
			if (!s)
			{
				Warning("[LR] Rating_Extended section missing\n");
				return false;
			}
			float coeff = s->GetFloat("lr_killcoeff", 1.0f);
			if (coeff < 0.5f) coeff = 0.5f;
			if (coeff > 1.5f) coeff = 1.5f;
			g_Cfg.killCoeffPct = int(coeff * 100.0f);
			LoadExpSection(s);
			break;
		}
		default:
		{
			KeyValues* s = kv->FindKey("Rating_Simple", false);
			if (!s)
			{
				Warning("[LR] Rating_Simple section missing\n");
				return false;
			}
			LoadExpSection(s);
			break;
		}
	}

	// Killstreak bonuses
	memset(g_Cfg.bonus, 0, sizeof(g_Cfg.bonus));
	if (g_Cfg.typeStatistics != 2)
	{
		KeyValues* b = kv->FindKey("Special_Bonuses", false);
		if (b)
		{
			char key[32];
			for (int i = 0; i < 11; i++)
			{
				V_snprintf(key, sizeof(key), "lr_bonus_%i", i + 1);
				g_Cfg.bonus[i] = b->GetInt(key, 0);
			}
		}
	}

	return true;
}

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

	// "competitive" (колонка соревновательного ранга справа, дефолт) или
	// "persona" (бейдж уровня профиля слева от ника). Дефолт именно
	// competitive: persona клиент берёт из Steam, подделать её нельзя.
	const char* mode = kv->GetString("tab_mode", "competitive");
	g_TabCfg.mode = (!strcmp(mode, "persona") || !strcmp(mode, "1")) ? 1 : 0;

	KeyValues* levels = kv->FindKey("Levels", false);
	if (levels)
	{
		// values indexed by level, missing levels fall back to the level number
		FOR_EACH_VALUE(levels, v)
		{
			int level = atoi(v->GetName());
			if (level < 1 || level > 512)
				continue;
			if ((int)g_TabCfg.values.size() < level)
				g_TabCfg.values.resize(level, 0);
			g_TabCfg.values[level - 1] = v->GetInt(nullptr);
		}
	}

	KeyValues* above = kv->FindKey("AboveMax", false);
	if (above)
	{
		g_TabCfg.aboveType   = above->GetInt("tab_type", g_TabCfg.type);
		g_TabCfg.aboveValue  = above->GetInt("value", 0);
		g_TabCfg.aboveUseExp = above->GetInt("use_exp", 0) != 0;
	}
	else
	{
		g_TabCfg.aboveType  = g_TabCfg.type;
		g_TabCfg.aboveValue = (int)g_TabCfg.values.size();
	}
}

bool LoadDatabaseConfig(DBConfig& out)
{
	KeyValues* kv = new KeyValues("Database");
	KeyValues::AutoDelete autoDelete(kv);

	if (!kv->LoadFromFile(g_pFullFileSystem, DB_PATH))
	{
		Warning("[LR] Failed to load %s\n", DB_PATH);
		return false;
	}

	out.host     = kv->GetString("host", "127.0.0.1");
	out.port     = kv->GetInt("port", 3306);
	out.user     = kv->GetString("user", "");
	out.pass     = kv->GetString("pass", "");
	out.database = kv->GetString("database", "");
	out.charset  = kv->GetString("charset", "utf8mb4");

	// DB_Escape backslash-escapes bytes, which is only sound on charsets whose
	// multibyte sequences never contain an ASCII byte. On gbk/big5/sjis/cp932
	// and friends a trailing 0x5C can swallow the escape, and player names come
	// straight from a client convar — that is an injection hole. Refuse rather
	// than run with an escaper that does not match the connection charset.
	static const char* kSafeCharsets[] = {"utf8mb4", "utf8mb3", "utf8", "latin1", "ascii", "binary"};
	bool charsetOk = false;
	for (const char* c : kSafeCharsets)
	{
		if (!V_stricmp(out.charset.c_str(), c))
		{
			charsetOk = true;
			break;
		}
	}
	if (!charsetOk)
	{
		Warning("[LR] charset '%s' is not supported (escaping is only safe for utf8mb4/utf8/latin1/ascii/binary)\n",
			out.charset.c_str());
		return false;
	}

	return !out.user.empty() && !out.database.empty();
}

bool LoadAllConfigs()
{
	if (!LoadSettings())
		return false;
	LoadTabConfig();
	if (!LoadPhrases())
		return false;
	return true;
}
