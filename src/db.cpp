#include "db.h"

#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>

#include <mysql.h>
#include <errmsg.h>

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
