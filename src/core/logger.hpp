// MemGuard X - leveled, rate-limited logging.
// The logger itself does not know about open.mp; the component installs a sink that
// forwards to ICore::logLn. Tests install a capturing sink.
#pragma once

#include "clock.hpp"

#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>

namespace memguard {

enum class LogLevel : int
{
	Debug = 0,
	Info = 1,
	Warning = 2,
	Error = 3,
	Off = 4
};

const char* logLevelName(LogLevel level);

/// Parses "debug"/"info"/"warning"/"warn"/"error"/"off" or "0".."4". Returns false if invalid.
bool parseLogLevel(std::string_view text, LogLevel& out);

class Logger
{
public:
	using Sink = std::function<void(LogLevel, const std::string&)>;

	explicit Logger(ClockFn clock = &steadyNowNs);

	void setSink(Sink sink);
	void setLevel(LogLevel level);
	LogLevel level() const;
	bool enabled(LogLevel level) const;

	/// Sets the minimum time between two messages that share the same rate-limit key.
	void setCooldownSeconds(std::uint32_t seconds);

	void log(LogLevel level, const std::string& message);
	void logf(LogLevel level, const char* fmt, ...)
#if defined(__GNUC__) || defined(__clang__)
		__attribute__((format(printf, 3, 4)))
#endif
		;

	/// Logs at most once per cooldown period for `key`. Suppressed messages are counted and the
	/// count is appended to the next message that gets through. Returns true if emitted.
	bool logLimited(LogLevel level, const std::string& key, const std::string& message);

	std::uint64_t emittedCount() const;
	std::uint64_t suppressedCount() const;

private:
	struct LimitState
	{
		Nanos lastEmit = 0;
		bool everEmitted = false;
		std::uint64_t suppressed = 0;
	};

	ClockFn clock_;
	mutable std::mutex mutex_;
	Sink sink_;
	LogLevel level_ = LogLevel::Info;
	Nanos cooldownNs_ = 60ull * 1000000000ull;
	std::unordered_map<std::string, LimitState> limits_;
	std::uint64_t emitted_ = 0;
	std::uint64_t suppressed_ = 0;
};

} // namespace memguard
