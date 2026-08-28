#include "wallet.h"
#include "lr_core.h"
#include "config.h"

#include <dlfcn.h>

#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>

// libcurl is loaded at runtime (dlopen) so lr_core.so does not require libcurl.so.4
// to be present on the game server. Wallet API is disabled when curl is missing.

using CURL = void;
struct curl_slist;

using curl_write_fn = size_t (*)(char*, size_t, size_t, void*);
using curl_global_init_fn = int (*)(long);
using curl_global_cleanup_fn = void (*)();
using curl_easy_init_fn = CURL* (*)();
using curl_easy_cleanup_fn = void (*)(CURL*);
using curl_easy_setopt_fn = int (*)(CURL*, int, ...);
using curl_easy_perform_fn = int (*)(CURL*);
using curl_easy_getinfo_fn = int (*)(CURL*, int, ...);
using curl_slist_append_fn = curl_slist* (*)(curl_slist*, const char*);
using curl_slist_free_all_fn = void (*)(curl_slist*);

static constexpr int kCurleOk = 0;
static constexpr int kCurlOptUrl = 10002;
static constexpr int kCurlOptPostFields = 10015;
static constexpr int kCurlOptHttpHeader = 10023;
static constexpr int kCurlOptWriteFunction = 20011;
static constexpr int kCurlOptTimeout = 13;
static constexpr int kCurlOptConnectTimeout = 78;
static constexpr int kCurlInfoResponseCode = 2097154;
static constexpr long kCurlGlobalDefault = 0;

static void* s_CurlLib = nullptr;
static curl_global_init_fn p_curl_global_init = nullptr;
static curl_global_cleanup_fn p_curl_global_cleanup = nullptr;
static curl_easy_init_fn p_curl_easy_init = nullptr;
static curl_easy_cleanup_fn p_curl_easy_cleanup = nullptr;
static curl_easy_setopt_fn p_curl_easy_setopt = nullptr;
static curl_easy_perform_fn p_curl_easy_perform = nullptr;
static curl_easy_getinfo_fn p_curl_easy_getinfo = nullptr;
static curl_slist_append_fn p_curl_slist_append = nullptr;
static curl_slist_free_all_fn p_curl_slist_free_all = nullptr;

struct WalletJob
{
	int iSlot = -1;
	uint64_t steam64 = 0;
	int coins = 0;
	char grantKind[32] = {0};
	char idempotencyKey[128] = {0};
};

static std::thread s_Worker;
static std::mutex s_JobMx;
static std::condition_variable s_JobCv;
static std::deque<WalletJob> s_Jobs;
static std::atomic<bool> s_Run{false};

static bool ResolveCurlSymbol(void* sym, const char* name)
{
	if (!sym)
	{
		Warning("[LR] libcurl missing symbol: %s\n", name);
		return false;
	}
	return true;
}

static bool LoadCurlLibrary()
{
	if (s_CurlLib)
		return p_curl_easy_init != nullptr;

	static const char* kLibs[] = {
		"libcurl.so.4",
		"libcurl.so",
		nullptr,
	};

	for (int i = 0; kLibs[i]; i++)
	{
		s_CurlLib = dlopen(kLibs[i], RTLD_LAZY | RTLD_LOCAL);
		if (s_CurlLib)
			break;
	}

	if (!s_CurlLib)
	{
		Warning("[LR] libcurl not found — wallet API disabled (install libcurl4 or set lr_wallet_enabled 0)\n");
		return false;
	}

	p_curl_global_init = (curl_global_init_fn)dlsym(s_CurlLib, "curl_global_init");
	p_curl_global_cleanup = (curl_global_cleanup_fn)dlsym(s_CurlLib, "curl_global_cleanup");
	p_curl_easy_init = (curl_easy_init_fn)dlsym(s_CurlLib, "curl_easy_init");
	p_curl_easy_cleanup = (curl_easy_cleanup_fn)dlsym(s_CurlLib, "curl_easy_cleanup");
	p_curl_easy_setopt = (curl_easy_setopt_fn)dlsym(s_CurlLib, "curl_easy_setopt");
	p_curl_easy_perform = (curl_easy_perform_fn)dlsym(s_CurlLib, "curl_easy_perform");
	p_curl_easy_getinfo = (curl_easy_getinfo_fn)dlsym(s_CurlLib, "curl_easy_getinfo");
	p_curl_slist_append = (curl_slist_append_fn)dlsym(s_CurlLib, "curl_slist_append");
	p_curl_slist_free_all = (curl_slist_free_all_fn)dlsym(s_CurlLib, "curl_slist_free_all");

	if (!ResolveCurlSymbol((void*)p_curl_global_init, "curl_global_init") ||
		!ResolveCurlSymbol((void*)p_curl_global_cleanup, "curl_global_cleanup") ||
		!ResolveCurlSymbol((void*)p_curl_easy_init, "curl_easy_init") ||
		!ResolveCurlSymbol((void*)p_curl_easy_cleanup, "curl_easy_cleanup") ||
		!ResolveCurlSymbol((void*)p_curl_easy_setopt, "curl_easy_setopt") ||
		!ResolveCurlSymbol((void*)p_curl_easy_perform, "curl_easy_perform") ||
		!ResolveCurlSymbol((void*)p_curl_easy_getinfo, "curl_easy_getinfo") ||
		!ResolveCurlSymbol((void*)p_curl_slist_append, "curl_slist_append") ||
		!ResolveCurlSymbol((void*)p_curl_slist_free_all, "curl_slist_free_all"))
	{
		dlclose(s_CurlLib);
		s_CurlLib = nullptr;
		return false;
	}

	return true;
}

static void UnloadCurlLibrary()
{
	if (!s_CurlLib)
		return;

	dlclose(s_CurlLib);
	s_CurlLib = nullptr;
	p_curl_global_init = nullptr;
	p_curl_global_cleanup = nullptr;
	p_curl_easy_init = nullptr;
	p_curl_easy_cleanup = nullptr;
	p_curl_easy_setopt = nullptr;
	p_curl_easy_perform = nullptr;
	p_curl_easy_getinfo = nullptr;
	p_curl_slist_append = nullptr;
	p_curl_slist_free_all = nullptr;
}

static size_t CurlDiscard(char* ptr, size_t size, size_t nmemb, void*)
{
	return size * nmemb;
}

static bool PostWalletGrant(const WalletJob& job)
{
	if (!g_Cfg.walletEnabled || !g_Cfg.walletApiUrl[0] || !g_Cfg.walletApiSecret[0])
		return false;
	if (job.coins <= 0 || !job.steam64 || !p_curl_easy_init)
		return false;

	char body[512];
	V_snprintf(body, sizeof(body),
		"{\"steamId\":\"%llu\",\"coins\":%i,\"grantKind\":\"%s\",\"idempotencyKey\":\"%s\"}",
		(unsigned long long)job.steam64, job.coins, job.grantKind, job.idempotencyKey);

	CURL* curl = p_curl_easy_init();
	if (!curl)
		return false;

	char auth[256];
	V_snprintf(auth, sizeof(auth), "Authorization: Bearer %s", g_Cfg.walletApiSecret);

	curl_slist* headers = nullptr;
	headers = p_curl_slist_append(headers, "Content-Type: application/json");
	headers = p_curl_slist_append(headers, auth);

	p_curl_easy_setopt(curl, kCurlOptUrl, g_Cfg.walletApiUrl);
	p_curl_easy_setopt(curl, kCurlOptHttpHeader, headers);
	p_curl_easy_setopt(curl, kCurlOptPostFields, body);
	p_curl_easy_setopt(curl, kCurlOptWriteFunction, (curl_write_fn)CurlDiscard);
	p_curl_easy_setopt(curl, kCurlOptTimeout, 8L);
	p_curl_easy_setopt(curl, kCurlOptConnectTimeout, 4L);

	int rc = p_curl_easy_perform(curl);
	long code = 0;
	p_curl_easy_getinfo(curl, kCurlInfoResponseCode, &code);

	p_curl_slist_free_all(headers);
	p_curl_easy_cleanup(curl);

	if (rc != kCurleOk || code < 200 || code >= 300)
	{
		Warning("[LR] wallet grant failed steam=%llu coins=%i http=%ld curl=%i\n",
			(unsigned long long)job.steam64, job.coins, code, rc);
		return false;
	}
	return true;
}

static void WorkerMain()
{
	while (true)
	{
		WalletJob job;
		{
			std::unique_lock<std::mutex> lk(s_JobMx);
			s_JobCv.wait(lk, [] { return !s_Run || !s_Jobs.empty(); });
			if (!s_Run && s_Jobs.empty())
				break;
			job = s_Jobs.front();
			s_Jobs.pop_front();
		}
		PostWalletGrant(job);
	}
}

bool Wallet_Start()
{
	if (s_Run)
		return true;

	if (!LoadCurlLibrary())
		return false;

	if (p_curl_global_init(kCurlGlobalDefault) != 0)
	{
		Warning("[LR] curl_global_init failed — wallet API disabled\n");
		UnloadCurlLibrary();
		return false;
	}

	s_Run = true;
	s_Worker = std::thread(WorkerMain);
	return true;
}

void Wallet_Stop()
{
	if (!s_Run)
		return;

	{
		std::lock_guard<std::mutex> lk(s_JobMx);
		s_Run = false;
	}
	s_JobCv.notify_all();
	if (s_Worker.joinable())
		s_Worker.join();

	s_Jobs.clear();

	if (p_curl_global_cleanup)
		p_curl_global_cleanup();
	UnloadCurlLibrary();
}

void Wallet_QueueGrant(int iSlot, uint64_t steam64, int coins, const char* grantKind, const char* idempotencyKey)
{
	if (!s_Run || coins <= 0 || !steam64 || !grantKind || !idempotencyKey)
		return;

	WalletJob job;
	job.iSlot = iSlot;
	job.steam64 = steam64;
	job.coins = coins;
	V_strncpy(job.grantKind, grantKind, sizeof(job.grantKind));
	V_strncpy(job.idempotencyKey, idempotencyKey, sizeof(job.idempotencyKey));

	{
		std::lock_guard<std::mutex> lk(s_JobMx);
		if (s_Jobs.size() > 256)
			s_Jobs.pop_front();
		s_Jobs.push_back(job);
	}
	s_JobCv.notify_one();
}

void Wallet_ProcessQueue()
{
	// Worker thread handles HTTP; nothing on game thread.
}
