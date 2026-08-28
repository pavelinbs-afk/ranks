#pragma once

#include <cstdint>

bool Wallet_Start();
void Wallet_Stop();

// Credits site_wallets on the website (SQLite via backend API). Non-blocking queue.
void Wallet_QueueGrant(int iSlot, uint64_t steam64, int coins, const char* grantKind, const char* idempotencyKey);

void Wallet_ProcessQueue();
