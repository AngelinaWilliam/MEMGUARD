#include "logger.hpp"

#include <cstdarg>
#include <cstdio>
#include <vector>

namespace memguard {

const char* logLevelName(LogLevel level)
{
	switch (level)
	{
	case LogLevel::Debug:
		return "debug";
	case LogLevel::Info:
		return "info";
	case LogLevel::Warning:
		return "warning";
	case LogLevel::Error:
		return "error";
	case LogLevel::Off:
		return "off";
	}
	return "unknown";
}

static bool equalsIgnoreCase(std::string_view a, std::string_view b)
{
	if (a.size() != b.size())
	{
		return false;
	}
	for (std::size_t i = 0; i < a.size(); ++i)
	{
		char ca = a[i];
		char cb = b[i];
		if (ca >= 'A' && ca <= 'Z')
		{
			ca = static_cast<char>(ca - 'A' + 'a');
		}
		if (cb >= 'A' && cb <= 'Z')
		{
			cb = static_cast<char>(cb - 'A' + 'a');
		}
		if (ca != cb)
		{
			return false;
		}
	}
	return true;
}

bool parseLogLevel(std::string_view text, LogLevel& out)
{
	struct Entry
	{
		const char* name;
		LogLevel level;
	};
	static const Entry entries[] = {
		{ "debug", LogLevel::Debug }, { "0", LogLevel::Debug },
		{ "info", LogLevel::Info }, { "1", LogLevel::Info },
		{ "warning", LogLevel::Warning }, { "warn", LogLevel::Warning }, { "2", LogLevel::Warning },
		{ "error", LogLevel::Error }, { "3", LogLevel::Error },
		{ "off", LogLevel::Off }, { "none", LogLevel::Off }, { "4", LogLevel::Off },
	};
	for (const Entry& e : entries)
	{
		if (equalsIgnoreCase(text, e.name))
		{
			out = e.level;
			return true;
		}
	}
	return false;
}

Logger::Logger(ClockFn clock)
	: clock_(clock ? clock : &steadyNowNs)
{
}

void Logger::setSink(Sink sink)
{
	std::lock_guard<std::mutex> lock(mutex_);
	sink_ = std::move(sink);
}

void Logger::setLevel(LogLevel level)
{
	std::lock_guard<std::mutex> lock(mutex_);
	level_ = level;
}

LogLevel Logger::level() const
{
	std::lock_guard<std::mutex> lock(mutex_);
	return level_;
}

bool Logger::enabled(LogLevel level) const
{
	std::lock_guard<std::mutex> lock(mutex_);
	return level != LogLevel::Off && static_cast<int>(level) >= static_cast<int>(level_);
}

void Logger::setCooldownSeconds(std::uint32_t seconds)
{
	std::lock_guard<std::mutex> lock(mutex_);
	cooldownNs_ = static_cast<Nanos>(seconds) * 1000000000ull;
}

void Logger::log(LogLevel level, const std::string& message)
{
	Sink sink;
	{
		std::lock_guard<std::mutex> lock(mutex_);
		if (level == LogLevel::Off || static_cast<int>(level) < static_cast<int>(level_))
		{
			return;
		}
		++emitted_;
		sink = sink_;
	}
	// The sink is called outside the lock so a sink that logs again cannot deadlock.
	if (sink)
	{
		sink(level, message);
	}
}

void Logger::logf(LogLevel level, const char* fmt, ...)
{
	if (!enabled(level))
	{
		return;
	}
	char stackBuf[512];
	va_list args;
	va_start(args, fmt);
	va_list copy;
	va_copy(copy, args);
	const int needed = std::vsnprintf(stackBuf, sizeof(stackBuf), fmt, args);
	va_end(args);
	if (needed < 0)
	{
		va_end(copy);
		return;
	}
	if (static_cast<std::size_t>(needed) < sizeof(stackBuf))
	{
		va_end(copy);
		log(level, std::string(stackBuf, static_cast<std::size_t>(needed)));
		return;
	}
	std::vector<char> heapBuf(static_cast<std::size_t>(needed) + 1);
	std::vsnprintf(heapBuf.data(), heapBuf.size(), fmt, copy);
	va_end(copy);
	log(level, std::string(heapBuf.data(), static_cast<std::size_t>(needed)));
}

bool Logger::logLimited(LogLevel level, const std::string& key, const std::string& message)
{
	std::string text;
	{
		std::lock_guard<std::mutex> lock(mutex_);
		if (level == LogLevel::Off || static_cast<int>(level) < static_cast<int>(level_))
		{
			return false;
		}
		const Nanos now = clock_();
		LimitState& state = limits_[key];
		if (state.everEmitted && now - state.lastEmit < cooldownNs_)
		{
			++state.suppressed;
			++suppressed_;
			return false;
		}
		text = message;
		if (state.suppressed > 0)
		{
			text += " (+" + std::to_string(state.suppressed) + " similar suppressed)";
		}
		state.suppressed = 0;
		state.lastEmit = now;
		state.everEmitted = true;
	}
	log(level, text);
	return true;
}

std::uint64_t Logger::emittedCount() const
{
	std::lock_guard<std::mutex> lock(mutex_);
	return emitted_;
}

std::uint64_t Logger::suppressedCount() const
{
	std::lock_guard<std::mutex> lock(mutex_);
	return suppressed_;
}

} // namespace memguard
