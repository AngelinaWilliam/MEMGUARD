#include "clock.hpp"

#include <chrono>
#include <cstdio>

namespace memguard {

Nanos steadyNowNs()
{
	using namespace std::chrono;
	return static_cast<Nanos>(duration_cast<nanoseconds>(steady_clock::now().time_since_epoch()).count());
}

std::uint64_t wallClockMs()
{
	using namespace std::chrono;
	return static_cast<std::uint64_t>(duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count());
}

void formatDuration(Nanos ns, char* out, std::size_t outSize)
{
	if (outSize == 0)
	{
		return;
	}
	if (ns < 1000ull)
	{
		std::snprintf(out, outSize, "%llu ns", static_cast<unsigned long long>(ns));
	}
	else if (ns < 1000000ull)
	{
		std::snprintf(out, outSize, "%.2f us", static_cast<double>(ns) / 1e3);
	}
	else if (ns < 1000000000ull)
	{
		std::snprintf(out, outSize, "%.2f ms", static_cast<double>(ns) / 1e6);
	}
	else
	{
		std::snprintf(out, outSize, "%.3f s", static_cast<double>(ns) / 1e9);
	}
}

} // namespace memguard
