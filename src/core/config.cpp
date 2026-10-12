#include "config.hpp"

#include <charconv>
#include <cstring>

namespace memguard {

namespace {

	bool parseBool(std::string_view v, bool& out)
	{
		if (v == "1" || v == "true" || v == "on" || v == "yes" || v == "TRUE" || v == "True")
		{
			out = true;
			return true;
		}
		if (v == "0" || v == "false" || v == "off" || v == "no" || v == "FALSE" || v == "False")
		{
			out = false;
			return true;
		}
		return false;
	}

	bool parseUint(std::string_view v, std::uint32_t minV, std::uint32_t maxV, std::uint32_t& out, std::string& error)
	{
		while (!v.empty() && (v.front() == ' ' || v.front() == '\t'))
		{
			v.remove_prefix(1);
		}
		while (!v.empty() && (v.back() == ' ' || v.back() == '\t'))
		{
			v.remove_suffix(1);
		}
		if (v.empty())
		{
			error = "expected an integer";
			return false;
		}
		unsigned long long parsed = 0;
		const char* first = v.data();
		const char* last = v.data() + v.size();
		auto res = std::from_chars(first, last, parsed);
		if (res.ec != std::errc() || res.ptr != last)
		{
			error = "expected a non-negative integer, got '" + std::string(v) + "'";
			return false;
		}
		if (parsed < minV || parsed > maxV)
		{
			error = "value " + std::to_string(parsed) + " out of range [" + std::to_string(minV) + ", " + std::to_string(maxV) + "]";
			return false;
		}
		out = static_cast<std::uint32_t>(parsed);
		return true;
	}

	struct IntField
	{
		const char* key;
		std::uint32_t Config::*member;
		std::uint32_t minV;
		std::uint32_t maxV;
	};

	struct BoolField
	{
		const char* key;
		bool Config::*member;
	};

	const IntField intFields[] = {
		{ "report_interval", &Config::reportIntervalSec, 0, 86400 },
		{ "snapshot_interval", &Config::snapshotIntervalSec, 0, 86400 },
		{ "top_n", &Config::topN, 1, 1000 },
		{ "history_size", &Config::historySize, 2, 100000 },
		{ "growth_window", &Config::growthWindow, 2, 10000 },
		{ "warmup_seconds", &Config::warmupSec, 0, 86400 },
		{ "growth_min_percent", &Config::growthMinPercent, 0, 100000 },
		{ "growth_min_count", &Config::growthMinCount, 1, 100000000 },
		{ "growth_min_mb", &Config::growthMinMB, 1, 1000000 },
		{ "slow_native_us", &Config::slowNativeUs, 0, 600000000 },
		{ "warn_cooldown", &Config::warnCooldownSec, 0, 86400 },
		{ "outstanding_limit", &Config::outstandingLimit, 0, 100000 },
		{ "recent_free_capacity", &Config::recentFreeCapacity, 0, 10000000 },
	};

	const BoolField boolFields[] = {
		{ "enable", &Config::enabled },
		{ "profile_natives", &Config::profileNatives },
		{ "track_pools", &Config::trackPools },
		{ "report_append", &Config::reportAppend },
		{ "report_to_console", &Config::reportToConsole },
	};

} // namespace

const std::vector<ConfigKeyInfo>& configKeys()
{
	static const std::vector<ConfigKeyInfo> keys = {
		{ "enable", ConfigValueType::Bool, "Master switch for all recording" },
		{ "profile_natives", ConfigValueType::Bool, "Time every Pawn native call (AMX callback wrapper)" },
		{ "track_pools", ConfigValueType::Bool, "Track open.mp pool entity creation/destruction" },
		{ "log_level", ConfigValueType::String, "debug | info | warning | error | off" },
		{ "report_interval", ConfigValueType::Int, "Seconds between periodic reports (0 = off)" },
		{ "snapshot_interval", ConfigValueType::Int, "Seconds between memory snapshots (0 = off)" },
		{ "report_file", ConfigValueType::String, "Report output file (relative to server directory)" },
		{ "report_append", ConfigValueType::Bool, "Append reports instead of overwriting" },
		{ "report_to_console", ConfigValueType::Bool, "Also print periodic reports to the server log" },
		{ "top_n", ConfigValueType::Int, "Rows per profiler table" },
		{ "history_size", ConfigValueType::Int, "Snapshots kept in memory" },
		{ "warmup_seconds", ConfigValueType::Int, "Startup period: growth inside it is reported as startup, not as a possible leak" },
		{ "growth_window", ConfigValueType::Int, "Snapshots used for growth detection (after warm-up)" },
		{ "growth_min_percent", ConfigValueType::Int, "Minimum relative growth to flag (%)" },
		{ "growth_min_count", ConfigValueType::Int, "Minimum absolute growth for count metrics" },
		{ "growth_min_mb", ConfigValueType::Int, "Minimum absolute growth for byte metrics (MiB)" },
		{ "slow_native_us", ConfigValueType::Int, "Warn when one native call exceeds this many microseconds (0 = off)" },
		{ "warn_cooldown", ConfigValueType::Int, "Seconds between repeated warnings of one kind" },
		{ "outstanding_limit", ConfigValueType::Int, "Outstanding allocations listed per report" },
		{ "recent_free_capacity", ConfigValueType::Int, "Freed keys remembered for double-free evidence" },
	};
	return keys;
}

bool applyConfigValue(Config& config, std::string_view key, std::string_view value, std::string& error)
{
	for (const IntField& f : intFields)
	{
		if (key == f.key)
		{
			std::uint32_t parsed = 0;
			if (!parseUint(value, f.minV, f.maxV, parsed, error))
			{
				error = std::string(key) + ": " + error;
				return false;
			}
			config.*(f.member) = parsed;
			return true;
		}
	}
	for (const BoolField& f : boolFields)
	{
		if (key == f.key)
		{
			bool parsed = false;
			if (!parseBool(value, parsed))
			{
				error = std::string(key) + ": expected true/false/1/0, got '" + std::string(value) + "'";
				return false;
			}
			config.*(f.member) = parsed;
			return true;
		}
	}
	if (key == "log_level")
	{
		LogLevel level;
		if (!parseLogLevel(value, level))
		{
			error = "log_level: expected debug|info|warning|error|off, got '" + std::string(value) + "'";
			return false;
		}
		config.logLevel = level;
		return true;
	}
	if (key == "report_file")
	{
		if (value.empty() || value.size() > 260)
		{
			error = "report_file: must be 1..260 characters";
			return false;
		}
		config.reportFile = std::string(value);
		return true;
	}
	error = "unknown key '" + std::string(key) + "'";
	return false;
}

std::string configValueAsString(const Config& config, std::string_view key)
{
	for (const IntField& f : intFields)
	{
		if (key == f.key)
		{
			return std::to_string(config.*(f.member));
		}
	}
	for (const BoolField& f : boolFields)
	{
		if (key == f.key)
		{
			return (config.*(f.member)) ? "true" : "false";
		}
	}
	if (key == "log_level")
	{
		return logLevelName(config.logLevel);
	}
	if (key == "report_file")
	{
		return config.reportFile;
	}
	return std::string();
}

std::string describeConfig(const Config& config)
{
	std::string out;
	for (const ConfigKeyInfo& info : configKeys())
	{
		out += "  memguard.";
		out += info.key;
		out += " = ";
		out += configValueAsString(config, info.key);
		out += '\n';
	}
	return out;
}

} // namespace memguard
