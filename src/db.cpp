#include "db.h"

#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>

#include <mysql.h>
#include <errmsg.h>

#include <cstdio>
#include <cstring>
#include <ctime>

#include <tier0/dbg.h>

struct DBJob
{
	std::string sql;
	DBCallback cb;
};

struct DBDone
{
	DBCallback cb;
	DBResult result;
};

static DBConfig s_Cfg;
static std::thread s_Worker;
static std::mutex s_JobMx, s_DoneMx;
static std::condition_variable s_JobCv;
static std::deque<DBJob> s_Jobs;
static std::deque<DBDone> s_Done;
static std::atomic<bool> s_Run{false};
static std::atomic<bool> s_Connected{false};
static bool s_LibInit = false;

// worker-thread state
static MYSQL* s_Conn = nullptr;

static bool WorkerConnect()
{
	if (s_Conn)
	{
		mysql_close(s_Conn);
		s_Conn = nullptr;
	}

	s_Conn = mysql_init(nullptr);
	if (!s_Conn)
		return false;

	unsigned int timeout = 10;
	mysql_options(s_Conn, MYSQL_OPT_CONNECT_TIMEOUT, &timeout);
	unsigned int rwTimeout = 30;
	mysql_options(s_Conn, MYSQL_OPT_READ_TIMEOUT, &rwTimeout);
	mysql_options(s_Conn, MYSQL_OPT_WRITE_TIMEOUT, &rwTimeout);
	mysql_options(s_Conn, MYSQL_SET_CHARSET_NAME, s_Cfg.charset.c_str());

	if (!mysql_real_connect(s_Conn, s_Cfg.host.c_str(), s_Cfg.user.c_str(), s_Cfg.pass.c_str(),
			s_Cfg.database.c_str(), s_Cfg.port, nullptr, 0))
	{
		Warning("[LR] MySQL connect failed: %s\n", mysql_error(s_Conn));
		mysql_close(s_Conn);
		s_Conn = nullptr;
		s_Connected = false;
		return false;
	}

	s_Connected = true;
	return true;
}

static bool IsConnectionLost(unsigned int err)
{
	return err == CR_SERVER_GONE_ERROR || err == CR_SERVER_LOST || err == CR_CONN_HOST_ERROR
		|| err == CR_CONNECTION_ERROR;
}

static void WorkerRunQuery(const DBJob& job, DBResult& out)
{
	for (int attempt = 0; attempt < 2; attempt++)
	{
		if (!s_Conn && !WorkerConnect())
		{
			out.ok = false;
			out.error = "not connected";
			return;
		}

		if (mysql_real_query(s_Conn, job.sql.c_str(), (unsigned long)job.sql.size()) != 0)
		{
			unsigned int err = mysql_errno(s_Conn);
			out.errcode = err;
			out.error = mysql_error(s_Conn);

			if (IsConnectionLost(err) && attempt == 0)
			{
				Warning("[LR] MySQL connection lost (%u), reconnecting...\n", err);
				s_Connected = false;
				if (WorkerConnect())
					continue;
			}

			out.ok = false;
			Warning("[LR] MySQL error %u: %s\n   query: %.200s\n", err, out.error.c_str(), job.sql.c_str());
			return;
		}
		break;
	}

	out.ok = true;
	out.affected = (uint64_t)mysql_affected_rows(s_Conn);
	out.insertId = (uint64_t)mysql_insert_id(s_Conn);

	MYSQL_RES* res = mysql_store_result(s_Conn);
	if (res)
	{
		unsigned int cols = mysql_num_fields(res);
		MYSQL_ROW row;
		while ((row = mysql_fetch_row(res)) != nullptr)
		{
			std::vector<std::string> r;
			r.reserve(cols);
			for (unsigned int c = 0; c < cols; c++)
				r.emplace_back(row[c] ? row[c] : "");
			out.rows.push_back(std::move(r));
		}
		mysql_free_result(res);
	}
}

static void WorkerMain()
{
	mysql_thread_init();
	WorkerConnect();

	while (true)
	{
		DBJob job;
		{
			std::unique_lock<std::mutex> lk(s_JobMx);
			s_JobCv.wait(lk, [] { return !s_Jobs.empty() || !s_Run; });
			if (!s_Run && s_Jobs.empty())
				break;
			job = std::move(s_Jobs.front());
			s_Jobs.pop_front();
		}

		DBDone done;
		done.cb = std::move(job.cb);
		WorkerRunQuery(job, done.result);

		if (done.cb)
		{
			std::lock_guard<std::mutex> lk(s_DoneMx);
			s_Done.push_back(std::move(done));
		}
	}

	if (s_Conn)
	{
		mysql_close(s_Conn);
		s_Conn = nullptr;
	}
	s_Connected = false;
	mysql_thread_end();
}

bool DB_Start(const DBConfig& cfg)
{
	if (s_Run)
		return true;

	s_Cfg = cfg;
	if (!s_LibInit)
	{
		mysql_library_init(0, nullptr, nullptr);
		s_LibInit = true;
	}
	s_Run = true;
	s_Worker = std::thread(WorkerMain);
	return true;
}

void DB_Stop()
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

	// Отбрасываем колбэки, до которых уже некому доехать: GameFrame больше не
	// вызывается, а сами запросы worker перед выходом успел выполнить.
	{
		std::lock_guard<std::mutex> lk(s_DoneMx);
		s_Done.clear();
	}

	// mysql_library_end() здесь сознательно не вызывается: connector не
	// поддерживает повторный mysql_library_init() в том же процессе, а плагин
	// могут выгрузить и загрузить обратно через meta unload / meta load без
	// рестарта сервера. Библиотека доживает до конца процесса.
}

void DB_Query(std::string sql, DBCallback cb)
{
	if (!s_Run)
	{
		if (cb)
		{
			DBResult r;
			r.ok = false;
			r.error = "db not started";
			cb(r);
		}
		return;
	}

	{
		std::lock_guard<std::mutex> lk(s_JobMx);
		s_Jobs.push_back(DBJob{std::move(sql), std::move(cb)});
	}
	s_JobCv.notify_one();
}

void DB_ProcessCallbacks()
{
	std::deque<DBDone> ready;
	{
		std::lock_guard<std::mutex> lk(s_DoneMx);
		if (s_Done.empty())
			return;
		ready.swap(s_Done);
	}

	for (auto& d : ready)
	{
		if (d.cb)
			d.cb(d.result);
	}
}

bool DB_IsConnected()
{
	return s_Connected;
}

DBPlaytimeNormalized DB_SplitPlaytimeSeconds(int64_t totalSec)
{
	DBPlaytimeNormalized out;
	if (totalSec < 0)
		totalSec = 0;
	out.totalSec = totalSec;
	out.hours = (int)(totalSec / 3600);
	out.minutes = (int)((totalSec / 60) % 60);
	out.seconds = (int)(totalSec % 60);
	return out;
}

int64_t DB_PlaytimeFromNormalized(int64_t secNorm, int h, int m, int s, int64_t legacyPlaytime)
{
	if (secNorm > 0)
		return secNorm;

	int64_t fromParts = (int64_t)h * 3600 + (int64_t)m * 60 + s;
	if (fromParts > 0)
		return fromParts;

	if (legacyPlaytime >= 1000000000LL && legacyPlaytime <= (int64_t)time(nullptr) + 86400)
		return 0;

	return legacyPlaytime > 0 ? legacyPlaytime : 0;
}

struct NormColumnDef
{
	const char* name;
	const char* alterFmt;
};

static void EnsureNormColumnThen(const char* tableName, size_t step, DBCallback onDone);

static void EnsureNormColumnThen(const char* tableName, size_t step, DBCallback onDone)
{
	static const NormColumnDef kCols[] = {
		{"playtime_sec_norm",
			"ALTER TABLE `%s` ADD COLUMN `playtime_sec_norm` bigint unsigned NOT NULL DEFAULT 0 AFTER `playtime`;"},
		{"playtime_h",
			"ALTER TABLE `%s` ADD COLUMN `playtime_h` int unsigned NOT NULL DEFAULT 0 AFTER `playtime_sec_norm`;"},
		{"playtime_m",
			"ALTER TABLE `%s` ADD COLUMN `playtime_m` tinyint unsigned NOT NULL DEFAULT 0 AFTER `playtime_h`;"},
		{"playtime_s",
			"ALTER TABLE `%s` ADD COLUMN `playtime_s` tinyint unsigned NOT NULL DEFAULT 0 AFTER `playtime_m`;"},
		{"lastconnect_norm",
			"ALTER TABLE `%s` ADD COLUMN `lastconnect_norm` int unsigned NOT NULL DEFAULT 0 AFTER `lastconnect`;"},
		{"lastconnect_at",
			"ALTER TABLE `%s` ADD COLUMN `lastconnect_at` datetime NULL DEFAULT NULL AFTER `lastconnect_norm`;"},
	};

	const size_t colCount = sizeof(kCols) / sizeof(kCols[0]);
	if (step >= colCount)
	{
		char checkSec[512];
		snprintf(checkSec, sizeof(checkSec),
			"SELECT COUNT(*) FROM information_schema.COLUMNS "
			"WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = '%s' AND COLUMN_NAME = 'playtime_sec';",
			tableName);

		std::string table = tableName;
		DB_Query(checkSec, [table, onDone](const DBResult& r) {
			auto runBackfill = [table, onDone]() {
				char backfill[1536];
				snprintf(backfill, sizeof(backfill),
					"UPDATE `%s` SET "
					"`playtime_sec_norm` = IF(`playtime_sec_norm` > 0, `playtime_sec_norm`, "
					"IF(`playtime_h` * 3600 + `playtime_m` * 60 + `playtime_s` > 0, "
					"`playtime_h` * 3600 + `playtime_m` * 60 + `playtime_s`, "
					"IF(`playtime` >= 1000000000 AND `playtime` <= UNIX_TIMESTAMP() + 86400, 0, GREATEST(`playtime`, 0)))), "
					"`playtime_h` = FLOOR(`playtime_sec_norm` / 3600), "
					"`playtime_m` = FLOOR((`playtime_sec_norm` %% 3600) / 60), "
					"`playtime_s` = (`playtime_sec_norm` %% 60), "
					"`lastconnect_norm` = IF(`lastconnect_norm` > 0, `lastconnect_norm`, "
					"IF(`lastconnect` >= 1000000000, `lastconnect`, "
					"IF(`lastconnect_at` IS NOT NULL, UNIX_TIMESTAMP(`lastconnect_at`), 0))), "
					"`lastconnect_at` = IF(`lastconnect_norm` > 0, FROM_UNIXTIME(`lastconnect_norm`), NULL), "
					"`playtime` = `playtime_sec_norm`, "
					"`lastconnect` = `lastconnect_norm`;",
					table.c_str());

				DB_Query(backfill, [onDone](const DBResult& br) {
					if (!br.ok)
						Warning("[LR] playtime/lastconnect normalization backfill failed: %s\n", br.error.c_str());
					else if (br.affected)
						Msg("[LR] playtime/lastconnect normalized (%llu row(s))\n", (unsigned long long)br.affected);

					if (onDone)
						onDone(br);
				});
			};

			if (r.ok && r.RowCount() && r.GetInt(0, 0) > 0)
			{
				char copySec[256];
				snprintf(copySec, sizeof(copySec),
					"UPDATE `%s` SET `playtime_sec_norm` = `playtime_sec` "
					"WHERE `playtime_sec_norm` = 0 AND `playtime_sec` > 0;",
					table.c_str());
				DB_Query(copySec, [runBackfill](const DBResult& cr) {
					if (!cr.ok)
						Warning("[LR] playtime_sec → playtime_sec_norm copy failed: %s\n", cr.error.c_str());
					runBackfill();
				});
				return;
			}

			runBackfill();
		});
		return;
	}

	char q[512];
	snprintf(q, sizeof(q),
		"SELECT COUNT(*) FROM information_schema.COLUMNS "
		"WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = '%s' AND COLUMN_NAME = '%s';",
		tableName, kCols[step].name);

	std::string table = tableName;
	DB_Query(q, [table, step, onDone](const DBResult& r) {
		auto next = [table, step, onDone]() { EnsureNormColumnThen(table.c_str(), step + 1, onDone); };

		if (!r.ok || !r.RowCount())
		{
			Warning("[LR] Failed to inspect column `%s`: %s\n", kCols[step].name, r.error.c_str());
			next();
			return;
		}

		if (r.GetInt(0, 0) != 0)
		{
			next();
			return;
		}

		char alter[512];
		snprintf(alter, sizeof(alter), kCols[step].alterFmt, table.c_str());
		DB_Query(alter, [step, next](const DBResult& altered) {
			if (!altered.ok)
				Warning("[LR] Failed to add column `%s`: %s\n", kCols[step].name, altered.error.c_str());
			next();
		});
	});
}

void DB_EnsureNormalizedTimeColumns(const char* tableName, DBCallback onDone)
{
	if (!tableName || !*tableName)
	{
		if (onDone)
		{
			DBResult r;
			r.ok = false;
			r.error = "empty table name";
			onDone(r);
		}
		return;
	}

	EnsureNormColumnThen(tableName, 0, std::move(onDone));
}

std::string DB_Escape(const char* in)
{
	// В utf8/utf8mb4 ни один байт многобайтовой последовательности не попадает в
	// диапазон ASCII, поэтому побайтовое экранирование корректно. Для gbk/big5/
	// sjis это неверно — такие charset'ы отсекаются в LoadDatabaseConfig.
	std::string out;
	if (!in)
		return out;
	out.reserve(strlen(in) * 2);
	for (const char* p = in; *p; p++)
	{
		switch (*p)
		{
			case '\'': out += "\\'"; break;
			case '"':  out += "\\\""; break;
			case '\\': out += "\\\\"; break;
			case '\n': out += "\\n"; break;
			case '\r': out += "\\r"; break;
			case '\0': break;
			case '\x1a': out += "\\Z"; break;
			default: out += *p; break;
		}
	}
	return out;
}
