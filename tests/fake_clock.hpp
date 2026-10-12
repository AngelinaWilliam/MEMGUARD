// Deterministic clock for tests. Not thread-safe by design (single-threaded timing tests only).
#pragma once

#include "../src/core/clock.hpp"

namespace mgtest {

inline memguard::Nanos& fakeNow()
{
	static memguard::Nanos now = 1000;
	return now;
}

inline memguard::Nanos fakeClock() { return fakeNow(); }
inline void advance(memguard::Nanos ns) { fakeNow() += ns; }

} // namespace mgtest
