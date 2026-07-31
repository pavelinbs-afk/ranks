#pragma once

#include "db.h"

// settings.ini + Ranks; false = fatal (bad config)
bool LoadSettings();
// tab.ini; missing file just disables the TAB feature
void LoadTabConfig();
// database.ini
bool LoadDatabaseConfig(DBConfig& out);
// everything incl. phrases (for lr_reload)
bool LoadAllConfigs();
