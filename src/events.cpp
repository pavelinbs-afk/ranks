#include "lr_core.h"
#include "events.h"
#include "chat.h"
#include "schema.h"

bool g_bAllowStatistic = false;

static bool s_bRegistered = false;
static int s_iRetryThrottle = 0;
static void* s_pGameRules = nullptr;

static const char* s_EventNames[] = {
	"player_death",
	"player_hurt",
	"weapon_fire",
	"round_start",
	"round_end",
	"round_mvp",
	"bomb_planted",
	"bomb_defused",
	"bomb_dropped",
	"bomb_pickup",
	"hostage_killed",
	"hostage_rescued",
};

// ---------------------------------------------------------------------------
// helpers
// ---------------------------------------------------------------------------

static int EventSlot(IGameEvent* event, const char* key)
{
	CPlayerSlot slot = event->GetPlayerSlot(key);
	int i = slot.Get();
	return (i >= 0 && i < LR_MAXPLAYERS) ? i : -1;
}

// A miss is never cached: cs_gamerules may not have spawned yet on the first
// call of a map, and remembering the nullptr would make IsWarmup() return false
// for the rest of it — silently disabling lr_block_warmup. Callers are
// round_start and client connect, so re-scanning on a miss is cheap.
static void* FindGameRules()
{
	if (s_pGameRules || !g_pGameEntitySystem)
		return s_pGameRules;

	for (int i = 0; i < 512; i++)
	{
		CEntityInstance* pEnt = g_pGameEntitySystem->GetEntityInstance(CEntityIndex(i));
		if (pEnt && !strcmp(pEnt->GetClassname(), "cs_gamerules"))
		{
			s_pGameRules = Schema_Get<void*>(pEnt, "CCSGameRulesProxy", "m_pGameRules");
			break;
		}
	}
	return s_pGameRules;
}

static bool IsWarmup()
{
	void* pRules = FindGameRules();
	if (!pRules)
		return false;
	return Schema_Get<bool>(pRules, "CCSGameRules", "m_bWarmupPeriod");
}

static int GetTeam(int iSlot)
{
	CEntityInstance* pController = GetControllerBySlot(iSlot);
	if (!pController)
		return 0;
	return Schema_Get<uint8_t>(pController, "CBaseEntity", "m_iTeamNum");
}

static bool IsPawnAlive(int iSlot)
{
	CEntityInstance* pController = GetControllerBySlot(iSlot);
	if (!pController || !g_pGameEntitySystem)
		return false;
	CEntityHandle hPawn = Schema_Get<CEntityHandle>(pController, "CBasePlayerController", "m_hPawn");
	CEntityInstance* pPawn = g_pGameEntitySystem->GetEntityInstance(hPawn);
	if (!pPawn)
		return false;
	return Schema_Get<int32_t>(pPawn, "CBaseEntity", "m_iHealth") > 0;
}

void CheckAllowStatistic(bool /*roundStart*/)
{
	g_bAllowStatistic = !(g_Cfg.blockWarmup && IsWarmup())
		&& !(g_Cfg.blockCustomRound && g_bCustomRoundActive);
}

int CountHumansOnTeams()
{
	int humans = 0;
	for (int i = 0; i < LR_MAXPLAYERS; i++)
	{
		if (g_Players[i].steam64 && GetControllerBySlot(i) && GetTeam(i) > 1)
			humans++;
	}
	return humans;
}

int ScaleExpByPlayerCount(int delta)
{
	if (delta == 0)
		return 0;

	int humans = CountHumansOnTeams();
	if (humans < g_Cfg.minPlayers)
		return 0;

	return delta;
}

static void TryPayKillStreak(int iSlot)
{
	if (iSlot < 0 || !g_Players[iSlot].loaded)
		return;

	int streak = g_Players[iSlot].killStreak;
	if (streak < 2)
		return;

	static const char* phrases[] = {
		"DoubleKill", "TripleKill", "Domination", "Rampage", "MegaKill",
		"Ownage", "UltraKill", "KillingSpree", "MonsterKill", "Unstoppable", "GodLike",
	};
	int idx = streak - 2;
	if (idx > 10)
		idx = 10;
	if (g_Cfg.bonus[idx] != 0 || g_Cfg.coinsMultikill > 0)
		ChangeExp(iSlot, g_Cfg.bonus[idx], phrases[idx], false, g_Cfg.coinsMultikill, "lr_multikill");
}

// ---------------------------------------------------------------------------
// handlers
// ---------------------------------------------------------------------------

static void OnPlayerDeath(IGameEvent* event)
{
	int victim = EventSlot(event, "userid");
	int attacker = EventSlot(event, "attacker");
	int assister = EventSlot(event, "assister");

	if (assister >= 0 && ChangeExp(assister, g_Cfg.expAssist, "AssisterKill"))
	{
		g_Players[assister].st.assists++;
		g_Players[assister].sess.assists++;
	}

	if (attacker >= 0 && victim >= 0)
	{
		if (attacker == victim)
		{
			if (g_Players[victim].loaded)
				ChangeExp(victim, -g_Cfg.expSuicide, "Suicide");
		}
		else if (!g_Cfg.allAgainstAll && GetTeam(victim) == GetTeam(attacker))
		{
			ChangeExp(attacker, -g_Cfg.expTeamkill, "TeamKill");
		}
		else
		{
			bool victimFake = !g_Players[victim].loaded;
			bool attackerFake = !g_Players[attacker].loaded;
			int expAttacker = victimFake ? g_Cfg.expKillBot : g_Cfg.expKill;
			int expVictim = attackerFake ? g_Cfg.expDeathBot : g_Cfg.expDeath;

			bool changedA = ChangeExp(attacker, expAttacker, "Kill");
			bool changedV = ChangeExp(victim, -expVictim, "MyDeath");

			if (changedA || changedV)
			{
				if (!attackerFake)
				{
					if (event->GetBool("headshot") && ChangeExp(attacker, g_Cfg.expHeadshot, "HeadShotKill"))
					{
						g_Players[attacker].st.headshots++;
						g_Players[attacker].sess.headshots++;
					}
					g_Players[attacker].st.kills++;
					g_Players[attacker].sess.kills++;
					g_Players[attacker].killStreak++;
					TryPayKillStreak(attacker);
				}
				if (!victimFake)
				{
					g_Players[victim].st.deaths++;
					g_Players[victim].sess.deaths++;
				}
			}
		}
	}

	if (victim >= 0 && g_Players[victim].loaded)
		g_Players[victim].killStreak = 0;
}

static void OnRoundEnd(IGameEvent* event)
{
	int winTeam = event->GetInt("winner");

	if (winTeam > 1)
	{
		for (int i = 0; i < LR_MAXPLAYERS; i++)
		{
			if (!g_Players[i].steam64 || !GetControllerBySlot(i))
				continue;

			int team = GetTeam(i);
			if (team > 1)
			{
				bool lose = team != winTeam;
				bool counted = lose
					? ChangeExp(i, -g_Cfg.expRoundLose, "RoundLose")
					: ChangeExp(i, g_Cfg.expRoundWin, "RoundWin");
				if (counted)
				{
					if (lose) { g_Players[i].st.roundLose++; g_Players[i].sess.roundLose++; }
					else      { g_Players[i].st.roundWin++;  g_Players[i].sess.roundWin++; }
				}
			}

			if (IsPawnAlive(i))
				g_Players[i].killStreak = 0;

			if (g_Players[i].loaded)
			{
				if (g_Cfg.showUsualMessage)
				{
					int re = g_Players[i].roundExp;
					int rc = g_Players[i].roundCoins;
					if (re > 0)
						LRPrintPhrase(i, "RoundExpResultGive", re);
					else if (re < 0)
						LRPrintPhrase(i, "RoundExpResultTake", -re);
					if (rc > 0)
						LRPrintPhrase(i, "RoundCoinResultGive", rc);
					if (re != 0)
						LRPrintPhrase(i, "RoundExpResultAll", g_Players[i].st.exp);
					if (rc > 0)
						LRPrintPhrase(i, "RoundCoinResultAll", g_Players[i].coins);
				}
				g_Players[i].roundExp = 0;
				g_Players[i].roundCoins = 0;
			}
		}
	}

	if (!g_Cfg.giveExpRoundEnd)
		g_bAllowStatistic = false;
}

static void OnBombEvent(const char* name, IGameEvent* event)
{
	int iSlot = EventSlot(event, "userid");
	if (iSlot < 0)
		return;

	// bomb_planted / bomb_defused / bomb_dropped / bomb_pickup
	switch (name[5])
	{
		case 'd': // defused / dropped
			if (name[6] == 'e')
			{
				ChangeExp(iSlot, g_Cfg.expBombDefuse, "BombDefused");
			}
			else if (g_Players[iSlot].haveBomb && ChangeExp(iSlot, -g_Cfg.expBombDrop, "BombDropped"))
			{
				g_Players[iSlot].haveBomb = false;
			}
			break;
		case 'p': // planted / pickup
			if (name[6] == 'l')
			{
				if (ChangeExp(iSlot, g_Cfg.expBombPlant, "BombPlanted"))
					g_Players[iSlot].haveBomb = false;
			}
			else if (!g_Players[iSlot].haveBomb && ChangeExp(iSlot, g_Cfg.expBombPickup, "BombPickup"))
			{
				g_Players[iSlot].haveBomb = true;
			}
			break;
	}
}

static void OnHitOrShot(const char* name, IGameEvent* event)
{
	if (!g_bAllowStatistic)
		return;

	if (name[0] == 'w') // weapon_fire
	{
		int iSlot = EventSlot(event, "userid");
		if (iSlot >= 0 && g_Players[iSlot].loaded)
		{
			g_Players[iSlot].st.shoots++;
			g_Players[iSlot].sess.shoots++;
		}
	}
	else // player_hurt
	{
		int victim = EventSlot(event, "userid");
		int attacker = EventSlot(event, "attacker");
		if (attacker >= 0 && attacker != victim && g_Players[attacker].loaded)
		{
			g_Players[attacker].st.hits++;
			g_Players[attacker].sess.hits++;
		}
	}
}

// ---------------------------------------------------------------------------
// listener
// ---------------------------------------------------------------------------

class LREventListener : public IGameEventListener2
{
public:
	void FireGameEvent(IGameEvent* event) override
	{
		const char* name = event->GetName();

		switch (name[0])
		{
			case 'p':
				if (!strcmp(name, "player_death"))
					OnPlayerDeath(event);
				else if (!strcmp(name, "player_hurt"))
					OnHitOrShot(name, event);
				break;

			case 'w':
				OnHitOrShot(name, event);
				break;

			case 'r':
				if (!strcmp(name, "round_start"))
				{
					for (int i = 0; i < LR_MAXPLAYERS; i++)
					{
						g_Players[i].killStreak = 0;
						g_Players[i].roundExp = 0;
					}
					CheckAllowStatistic(true);
				}
				else if (!strcmp(name, "round_end"))
					OnRoundEnd(event);
				else if (!strcmp(name, "round_mvp"))
				{
					int iSlot = EventSlot(event, "userid");
					if (iSlot >= 0)
					{
						ChangeExp(iSlot, g_Cfg.expMvp, "RoundMVP", false, g_Cfg.coinsMvp, "lr_mvp");
					}
				}
				break;

			case 'b':
				OnBombEvent(name, event);
				break;

			case 'h':
				if (!strcmp(name, "hostage_killed"))
				{
					int iSlot = EventSlot(event, "userid");
					if (iSlot >= 0)
						ChangeExp(iSlot, -g_Cfg.expHostageKill, "HostageKilled");
				}
				else if (!strcmp(name, "hostage_rescued"))
				{
					int iSlot = EventSlot(event, "userid");
					if (iSlot >= 0)
						ChangeExp(iSlot, g_Cfg.expHostageRescue, "HostageRescued");
				}
				break;
		}
	}
};

static LREventListener s_Listener;

void Events_TryRegister()
{
	if (s_bRegistered || !g_pGameEventManager)
		return;
	if (s_iRetryThrottle++ % 64 != 0)
		return;

	bool allOk = true;
	for (const char* name : s_EventNames)
	{
		if (g_pGameEventManager->FindListener(&s_Listener, name))
			continue;
		if (!g_pGameEventManager->AddListener(&s_Listener, name, true))
			allOk = false;
	}

	if (allOk)
	{
		s_bRegistered = true;
		LR_Log("game events hooked");
	}
}

void Events_Unregister()
{
	if (g_pGameEventManager)
		g_pGameEventManager->RemoveListener(&s_Listener);
	s_bRegistered = false;
}

void Events_OnStartupServer()
{
	s_pGameRules = nullptr;
	s_bRegistered = false;
	s_iRetryThrottle = 0;
	g_bAllowStatistic = false;
	g_bCustomRoundActive = false;

	// Controllers are recreated on a map change, so the scoreboard rank has to
	// be pushed and revealed again. Without this the TAB icon stays blank until
	// the player happens to open the scoreboard.
	CGlobalVars* gv = GetGlobals();
	float now = gv ? gv->curtime : 0.0f;
	for (int i = 0; i < LR_MAXPLAYERS; i++)
	{
		g_Players[i].revealSent = false;
		g_Players[i].oldButtons = 0;
		g_Players[i].tabIconsApplied = false;
		// Re-arm the mount delay: clients remount addons across changelevel.
		if (g_Players[i].loaded)
		{
			g_Players[i].tabIconsAt = now + g_TabCfg.iconsDelay;
			g_Players[i].tabRefreshUntil = g_Players[i].tabIconsAt + g_TabCfg.iconsRefresh;
		}
	}
}
