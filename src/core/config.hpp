// MemGuard X - configuration model and validation.
// Values arrive as text (from config.json via the component adapter, or from the
// `memguard set <key> <value>` console command) and are validated here.
#pragma once

#include "logger.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace memguard {

struct Config
{
	bool enabled = true; ///< Master switch. When false the component loads but records nothing.
	bool profileNatives = true; ///< Wrap the AMX native-call callback to time every native call.
	bool trackPools = true; ///< Track creation/destruction of open.mp pool entities.
	LogLevel logLevel = LogLevel::Info;

	std::uint32_t reportIntervalSec = 300; ///< 0 = no periodic report (commands still work).
	std::uint32_t snapshotIntervalSec = 60; ///< 0 = no periodic snapshots.
	std::string reportFile = "memguard_report.txt"; ///< Relative to the server's working directory.
	bool reportAppend = false; ///< Append each report instead of overwriting the file.
	bool reportToConsole = false; ///< Also print periodic reports to the server log.

	std::uint32_t topN = 15; ///< Rows per profiler table.
	std::uint32_t historySize = 120; ///< Number of snapshots kept in memory.
	std::uint32_t warmupSec = 120; ///< Startup period after server start / gamemode restart. Growth during
								   ///< it is reported as startup, never as a possible leak.
	std::uint32_t growthWindow = 6; ///< Snapshots considered by growth detection.
	std::uint32_t growthMinPercent = 10; ///< Minimum relative growth over the window.
	std::uint32_t growthMinCount = 10; ///< Minimum absolute growth for count metrics.
	std::uint32_t growthMinMB = 8; ///< Minimum absolute growth for byte metrics (MiB).

	std::uint32_t slowNativeUs = 5000; ///< Warn when a single native call exceeds this (0 = off).
	std::uint32_t warnCooldownSec = 60; ///< Rate limit for repeated warnings of the same kind.
	std::uint32_t outstandingLimit = 20; ///< Max outstanding allocations listed per report.
	std::uint32_t recentFreeCapacity = 4096; ///< Freed keys remembered for double-free evidence.
};

enum class ConfigValueType
{
	Bool,
	Int,
	String
};

struct ConfigKeyInfo
{
	const char* key;
	ConfigValueType type;
	const char* description;
};

/// All recognised keys (without the "memguard." prefix used in config.json).
const std::vector<ConfigKeyInfo>& configKeys();

/// Applies one key/value pair. Returns false and fills `error` if the key is unknown or the
/// value is invalid / out of range; `config` is left unchanged in that case.
bool applyConfigValue(Config& config, std::string_view key, std::string_view value, std::string& error);

/// Returns the current value of `key` as text, or an empty string for an unknown key.
std::string configValueAsString(const Config& config, std::string_view key);

/// Multi-line "key = value" dump used by the status report.
std::string describeConfig(const Config& config);

} // namespace memguard
