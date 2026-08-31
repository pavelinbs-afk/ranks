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

/** Total playtime split into h/m/s (playtime in lr_core is always stored as seconds). */
struct DBPlaytimeNormalized
{
	int64_t totalSec = 0;
	int hours = 0;
	int minutes = 0;
	int seconds = 0;
};

DBPlaytimeNormalized DB_SplitPlaytimeSeconds(int64_t totalSec);

/** Reconstruct total seconds from normalized columns (sec_norm preferred, then h/m/s). */
int64_t DB_PlaytimeFromNormalized(int64_t secNorm, int h, int m, int s, int64_t legacyPlaytime = 0);

/** SQL fragment: player has ever connected (normalized first, legacy fallback). */
constexpr const char* LR_SQL_LASTCONNECT_ACTIVE =
	"(`lastconnect_norm` > 0 OR (`lastconnect` >= 1000000000 AND `lastconnect` <= UNIX_TIMESTAMP() + 86400))";

/** Primary playtime column (total seconds). */
constexpr const char* LR_COL_PLAYTIME_SEC = "`playtime_sec_norm`";

/**
 * Adds playtime_sec_norm/h/m/s + lastconnect_norm/at, backfills all rows from legacy
 * playtime/lastconnect (and playtime_sec if present), then calls onDone on the main thread.
 */
void DB_EnsureNormalizedTimeColumns(const char* tableName, DBCallback onDone = nullptr);
