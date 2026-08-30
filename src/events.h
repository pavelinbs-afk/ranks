#pragma once

// Registers the IGameEventListener2 once the event manager is captured and
// event descriptors are loaded. Safe to call every frame (throttled inside).
void Events_TryRegister();
void Events_Unregister();

// Map change: drop cached gamerules, re-register listeners.
void Events_OnStartupServer();

// Recomputes g_bAllowStatistic (warmup / VIP custom round).
void CheckAllowStatistic(bool roundStart = false);

// Humans on T/CT (not spec).
int CountHumansOnTeams();

// Scale exp delta by online: under lr_minplayers_count → 0.
int ScaleExpByPlayerCount(int delta);

// Round-start snapshot (for player-count messages).
bool Events_WasRoundExpAllowed();
int Events_RoundStartHumans();
