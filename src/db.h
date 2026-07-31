// Async MySQL layer: one worker thread owns the connection, queries are
// enqueued from the main thread, callbacks run on the main thread from
// GameFrame (DB_ProcessCallbacks).
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

struct DBConfig
{
	std::string host, user, pass, database, charset = "utf8mb4";
	int port = 3306;
};

class DBResult
{
public:
	bool ok = false;
	std::string error;
	unsigned int errcode = 0;
	uint64_t affected = 0;
	uint64_t insertId = 0;
	std::vector<std::vector<std::string>> rows;

	int RowCount() const { return (int)rows.size(); }
	const char* Get(int r, int c) const
	{
		if (r < 0 || r >= (int)rows.size() || c < 0 || c >= (int)rows[r].size())
			return "";
		return rows[r][c].c_str();
	}
	int GetInt(int r, int c) const { return atoi(Get(r, c)); }
	int64_t GetInt64(int r, int c) const { return strtoll(Get(r, c), nullptr, 10); }
	double GetFloat(int r, int c) const { return atof(Get(r, c)); }
};

using DBCallback = std::function<void(const DBResult&)>;

bool DB_Start(const DBConfig& cfg);
void DB_Stop();

// Thread-safe; cb (optional) runs on the main thread.
void DB_Query(std::string sql, DBCallback cb = nullptr);

// Call every game frame on the main thread.
void DB_ProcessCallbacks();

bool DB_IsConnected();

// utf8/utf8mb4-safe escaping for string literals ('...' contents).
std::string DB_Escape(const char* in);
