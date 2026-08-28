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

// Scale exp delta by online: under 3 → 0, 3-4 → ±1, at/above threshold → full.
int ScaleExpByPlayerCount(int delta);
