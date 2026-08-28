#include "wallet.h"
#include "lr_core.h"
#include "config.h"

#include <curl/curl.h>

#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>

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

static size_t CurlDiscard(char* ptr, size_t size, size_t nmemb, void*)
{
	return size * nmemb;
}

static bool PostWalletGrant(const WalletJob& job)
{
	if (!g_Cfg.walletEnabled || !g_Cfg.walletApiUrl[0] || !g_Cfg.walletApiSecret[0])
		return false;
	if (job.coins <= 0 || !job.steam64)
		return false;

	char body[512];
	V_snprintf(body, sizeof(body),
		"{\"steamId\":\"%llu\",\"coins\":%i,\"grantKind\":\"%s\",\"idempotencyKey\":\"%s\"}",
		(unsigned long long)job.steam64, job.coins, job.grantKind, job.idempotencyKey);

	CURL* curl = curl_easy_init();
	if (!curl)
		return false;

	char auth[256];
	V_snprintf(auth, sizeof(auth), "Authorization: Bearer %s", g_Cfg.walletApiSecret);

	struct curl_slist* headers = nullptr;
	headers = curl_slist_append(headers, "Content-Type: application/json");
	headers = curl_slist_append(headers, auth);

	curl_easy_setopt(curl, CURLOPT_URL, g_Cfg.walletApiUrl);
	curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
	curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body);
	curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, CurlDiscard);
	curl_easy_setopt(curl, CURLOPT_TIMEOUT, 8L);
	curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 4L);

	CURLcode rc = curl_easy_perform(curl);
	long code = 0;
	curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &code);

	curl_slist_free_all(headers);
	curl_easy_cleanup(curl);

	if (rc != CURLE_OK || code < 200 || code >= 300)
	{
		Warning("[LR] wallet grant failed steam=%llu coins=%i http=%ld curl=%i\n",
			(unsigned long long)job.steam64, job.coins, code, (int)rc);
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

	if (curl_global_init(CURL_GLOBAL_DEFAULT) != 0)
	{
		Warning("[LR] curl_global_init failed — wallet API disabled\n");
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
	curl_global_cleanup();
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
