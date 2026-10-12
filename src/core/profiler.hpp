// MemGuard X - function / operation profiler.
//
// Design notes
//  * Zones (natives, script sections, internal operations) are registered once by name and
//    then addressed by a dense integer id, so the hot path never touches strings or maps.
//  * Per-zone counters are relaxed atomics: recording from several threads is safe and
//    totals are exact; only the *combination* of fields read by stats() is not a single
//    atomic snapshot while writers are active.
//  * Nesting is tracked on a per-thread frame stack so we can report inclusive ("total")
//    and exclusive ("self") time. Frames left open (e.g. a Pawn public aborted by a
//    runtime error) are detected and discarded, never invented.
//  * The clock is injected so tests are deterministic.
#pragma once

#include "clock.hpp"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace memguard {

using ZoneId = std::uint32_t;

enum class ZoneKind : std::uint8_t
{
	Native, ///< A Pawn native function, timed through the AMX callback wrapper.
	Script, ///< A section explicitly instrumented from Pawn (MemGuard_ProfileBegin/End).
	Internal, ///< MemGuard X's own work or C++ code instrumented with ScopedZone.
	Interval, ///< A sampled interval (e.g. time between server ticks); not a nested call.
	Function ///< A whole Pawn function, entered/left through MEMGUARD_FUNC (memguard.inc).
};

const char* zoneKindName(ZoneKind kind);

struct ZoneStats
{
	ZoneId id = 0;
	std::string name;
	ZoneKind kind = ZoneKind::Internal;
	std::uint64_t calls = 0;
	Nanos totalNs = 0; ///< Inclusive time.
	Nanos selfNs = 0; ///< Exclusive time (total minus time in nested zones).
	Nanos maxNs = 0;
	Nanos minNs = 0; ///< 0 when calls == 0.
	std::uint64_t slowCalls = 0; ///< Calls above the configured slow threshold.

	Nanos averageNs() const { return calls ? totalNs / calls : 0; }
};

enum class SortKey
{
	TotalTime,
	SelfTime,
	Calls,
	MaxTime,
	AverageTime
};

struct ProfilerCounters
{
	std::uint64_t abandonedFrames = 0; ///< Frames discarded because they were never closed.
	std::uint64_t unmatchedEnds = 0; ///< End requests with no matching open frame.
	std::uint64_t overflowBegins = 0; ///< Begins rejected because the stack depth limit was hit.
};

class Profiler
{
public:
	static constexpr std::size_t kChunkSize = 256;
	static constexpr std::size_t kMaxChunks = 256; ///< => at most 65536 zones.
	static constexpr std::size_t kMaxDepth = 256;
	static constexpr std::size_t kNoFrame = static_cast<std::size_t>(-1);
	static constexpr ZoneId kInvalidZone = static_cast<ZoneId>(-1);

	explicit Profiler(ClockFn clock = &steadyNowNs);
	~Profiler();
	Profiler(const Profiler&) = delete;
	Profiler& operator=(const Profiler&) = delete;

	/// Finds or registers a zone. Thread-safe. Returns kInvalidZone if the zone table is full.
	ZoneId zone(std::string_view name, ZoneKind kind);
	std::optional<ZoneId> findZone(std::string_view name) const;
	std::size_t zoneCount() const;
	/// Name of a registered zone (empty string if unknown).
	std::string zoneName(ZoneId id) const;

	void setEnabled(bool enabled) { enabled_.store(enabled, std::memory_order_relaxed); }
	bool enabled() const { return enabled_.load(std::memory_order_relaxed); }

	/// Calls longer than this (inclusive time) are counted as slow. 0 disables. Not applied to
	/// Interval zones (an interval is not a call).
	void setSlowThreshold(Nanos ns) { slowThresholdNs_.store(ns, std::memory_order_relaxed); }
	Nanos slowThreshold() const { return slowThresholdNs_.load(std::memory_order_relaxed); }

	/// Opens a frame on the calling thread's stack. Returns a token for end(), or kNoFrame
	/// when profiling is disabled, the id is invalid or the depth limit is reached.
	std::size_t begin(ZoneId id);

	/// Closes the frame identified by `token` and records it. Frames opened above it and not
	/// closed are discarded (counted in abandonedFrames). Returns the inclusive duration,
	/// or 0 if the token did not match an open frame.
	Nanos end(std::size_t token);

	/// Closes the most recent open frame of zone `id` on this thread (used by Pawn
	/// instrumentation where begin and end happen in separate native calls).
	bool endZone(ZoneId id);

	/// Like begin(), but returns a frame serial number (1..2^31-1, never 0) that is unique
	/// among open frames, or 0 if no frame was opened. Used by Pawn function profiling,
	/// where the "end" arrives in a later native call and must never close the wrong frame.
	std::uint32_t beginSerial(ZoneId id);

	/// Closes the open frame with this serial (and discards frames opened above it that were
	/// never closed). Returns false if no open frame on this thread has that serial.
	bool endSerial(std::uint32_t serial);

	/// Number of open frames on the calling thread.
	std::size_t openFrames() const;

	/// Discards every open frame on the calling thread (e.g. after a script unload).
	void discardOpenFrames();

	/// Records one sample directly (inclusive == exclusive == duration).
	void record(ZoneId id, Nanos duration);

	std::vector<ZoneStats> stats() const;
	std::optional<ZoneStats> zoneStats(ZoneId id) const;
	std::vector<ZoneStats> sorted(SortKey key, std::size_t limit, std::optional<ZoneKind> kind = std::nullopt) const;
	ProfilerCounters counters() const;

	/// Resets all counters. Zone registrations are kept so ids stay valid.
	void reset();

	Nanos now() const { return clock_(); }

	/// Measures the cost of one begin()/end() pair (median of several batches) using the real
	/// clock. Used to tell administrators how much overhead profiling adds per native call.
	static Nanos measureOverheadNs();

private:
	struct Slot
	{
		std::string name;
		ZoneKind kind = ZoneKind::Internal;
		std::atomic<std::uint64_t> calls { 0 };
		std::atomic<std::uint64_t> totalNs { 0 };
		std::atomic<std::uint64_t> selfNs { 0 };
		std::atomic<std::uint64_t> maxNs { 0 };
		std::atomic<std::uint64_t> minNs { ~0ull };
		std::atomic<std::uint64_t> slowCalls { 0 };
	};

	struct Frame
	{
		ZoneId id;
		Nanos start;
		Nanos child;
		std::uint32_t serial;
	};

	Slot* slot(ZoneId id) const;
	void commit(Slot& s, Nanos total, Nanos self);
	std::vector<Frame>& frames() const;

	ClockFn clock_;
	std::uint64_t instanceId_;
	std::atomic<bool> enabled_ { true };
	std::atomic<Nanos> slowThresholdNs_ { 0 };

	mutable std::mutex registryMutex_;
	std::unordered_map<std::string, ZoneId> byName_;
	std::array<std::atomic<Slot*>, kMaxChunks> chunks_ {};
	std::atomic<std::uint32_t> count_ { 0 };

	std::atomic<std::uint64_t> abandoned_ { 0 };
	std::atomic<std::uint64_t> unmatched_ { 0 };
	std::atomic<std::uint64_t> overflow_ { 0 };
	std::atomic<std::uint32_t> nextSerial_ { 1 };
};

/// RAII helper for C++ code paths.
class ScopedZone
{
public:
	ScopedZone(Profiler& profiler, ZoneId id)
		: profiler_(profiler)
		, token_(profiler.begin(id))
	{
	}
	~ScopedZone() { profiler_.end(token_); }
	ScopedZone(const ScopedZone&) = delete;
	ScopedZone& operator=(const ScopedZone&) = delete;

private:
	Profiler& profiler_;
	std::size_t token_;
};

} // namespace memguard
