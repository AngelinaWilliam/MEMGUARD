// MemGuard X - time source abstraction.
// The diagnostic core never calls a clock directly; it receives a ClockFn so that
// unit tests can drive time deterministically.
#pragma once

#include <cstddef>
#include <cstdint>

namespace memguard {

using Nanos = std::uint64_t;
using ClockFn = Nanos (*)();

/// Monotonic clock in nanoseconds (std::chrono::steady_clock).
Nanos steadyNowNs();

/// Wall-clock time as milliseconds since the Unix epoch (for report timestamps only).
std::uint64_t wallClockMs();

/// Formats a duration in nanoseconds as a short human-readable string ("12.3 us", "4.56 ms").
void formatDuration(Nanos ns, char* out, std::size_t outSize);

} // namespace memguard
