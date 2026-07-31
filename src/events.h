#pragma once

// Registers the IGameEventListener2 once the event manager is captured and
// event descriptors are loaded. Safe to call every frame (throttled inside).
void Events_TryRegister();
void Events_Unregister();

// Map change: drop cached gamerules, re-register listeners.
void Events_OnStartupServer();

// Recomputes g_bAllowStatistic (min players / warmup).
void CheckAllowStatistic(bool roundStart = false);
