// MemGuard X - observable allocation / resource tracker.
//
// MemGuard X does NOT replace the process allocator. It tracks allocations that are reported
// to it explicitly:
//   * open.mp pool entities (vehicles, objects, text labels, ...) via pool events,
//   * resources a Pawn script reports through MemGuard_TrackAlloc / MemGuard_TrackFree,
//   * C++ code that calls the tracker API directly.
// Every allocation is identified by (category, id). For pointer-style tracking the id is the
// pointer value.
//
// Evidence levels used in reports:
//   * Confirmed tracking errors  - double free, free of an unknown id, duplicate allocation of
//     an id that is still active. These are facts about the event stream we received.
//   * Possible leak              - sustained growth of active counts/bytes across snapshots
//     (see snapshot.hpp). A heuristic, never a proof.
//   * Active allocation          - simply still alive. Not a leak by itself.
#pragma once

#include "clock.hpp"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace memguard {

using CategoryId = std::uint32_t;

enum class AllocResult
{
	Tracked, ///< New active allocation recorded.
	DuplicateActive, ///< Id was already active: recorded as a tracking error, entry replaced.
	IgnoredNull, ///< Null pointer allocation (pointer API only); nothing recorded.
	Disabled, ///< Tracker disabled.
	InvalidCategory
};

enum class FreeResult
{
	Released, ///< Matching active allocation removed.
	UnknownFree, ///< No active allocation and no record of a recent free: never seen.
	DoubleFree, ///< Id was freed before and not re-allocated since (within remembered history).
	IgnoredNull, ///< free(nullptr) semantics: valid no-op (pointer API only).
	Disabled,
	InvalidCategory
};

const char* allocResultName(AllocResult r);
const char* freeResultName(FreeResult r);

struct CategoryStats
{
	CategoryId id = 0;
	std::string name;
	bool sizeKnown = true; ///< False for categories whose sizes are not observable (pool entities).
	std::uint64_t allocs = 0;
	std::uint64_t frees = 0;
	std::uint64_t active = 0;
	std::uint64_t activeBytes = 0;
	std::uint64_t peakActive = 0;
	std::uint64_t peakBytes = 0;
	std::uint64_t totalBytesAllocated = 0;
	std::uint64_t duplicateAllocs = 0;
	std::uint64_t unknownFrees = 0;
	std::uint64_t doubleFrees = 0;
	std::uint64_t nullAllocs = 0;
	std::uint64_t nullFrees = 0;

	std::uint64_t trackingErrors() const { return duplicateAllocs + unknownFrees + doubleFrees; }
};

struct ActiveAllocation
{
	CategoryId category = 0;
	std::uint64_t id = 0;
	std::uint64_t size = 0;
	Nanos allocatedAt = 0; ///< Tracker clock.
	std::uint64_t sequence = 0; ///< Global allocation sequence number (monotonic).
};

struct TrackerEvent
{
	enum class Kind
	{
		DuplicateAlloc,
		UnknownFree,
		DoubleFree
	};
	Kind kind;
	CategoryId category;
	std::uint64_t id;
	Nanos at;
};

struct TrackerSnapshot
{
	Nanos takenAt = 0;
	std::uint64_t sequence = 0; ///< Allocation sequence counter at snapshot time.
	std::vector<CategoryStats> categories;
	std::uint64_t totalActive = 0;
	std::uint64_t totalActiveBytes = 0;
	std::uint64_t totalTrackingErrors = 0;
};

class MemoryTracker
{
public:
	explicit MemoryTracker(ClockFn clock = &steadyNowNs, std::size_t recentFreeCapacity = 4096, std::size_t eventLogCapacity = 64);

	/// Finds or registers a category. `sizeKnown` documents whether byte totals are meaningful.
	CategoryId category(std::string_view name, bool sizeKnown = true);
	std::optional<CategoryId> findCategory(std::string_view name) const;

	AllocResult onAlloc(CategoryId cat, std::uint64_t id, std::uint64_t size);
	FreeResult onFree(CategoryId cat, std::uint64_t id);

	/// Pointer flavour: a null pointer is never tracked (alloc) and is a valid no-op (free).
	AllocResult onAllocPtr(CategoryId cat, const void* ptr, std::uint64_t size);
	FreeResult onFreePtr(CategoryId cat, const void* ptr);

	bool isActive(CategoryId cat, std::uint64_t id) const;

	std::vector<CategoryStats> categoryStats() const;
	std::optional<CategoryStats> categoryStats(CategoryId cat) const;

	/// Active allocations, oldest first. `cat` filters by category; `limit` caps the result.
	std::vector<ActiveAllocation> outstanding(std::optional<CategoryId> cat, std::size_t limit) const;

	/// Consistent snapshot taken under one lock.
	TrackerSnapshot snapshot() const;

	/// The most recent tracking-error events (bounded ring).
	std::vector<TrackerEvent> recentEvents() const;

	std::string categoryName(CategoryId cat) const;

	void setEnabled(bool enabled);
	bool enabled() const;

	/// Drops all active entries, history and counters of every category (registrations kept).
	void reset();

	/// Drops active entries of one category without counting frees (e.g. the source of
	/// events went away). Counters are kept; returns the number of entries dropped.
	std::size_t forgetActive(CategoryId cat);

	void setRecentFreeCapacity(std::size_t capacity);

private:
	struct Key
	{
		CategoryId cat;
		std::uint64_t id;
		bool operator==(const Key& o) const { return cat == o.cat && id == o.id; }
	};
	struct KeyHash
	{
		std::size_t operator()(const Key& k) const
		{
			std::uint64_t h = k.id * 0x9E3779B97F4A7C15ull;
			h ^= (static_cast<std::uint64_t>(k.cat) + 0x632BE59BD9B4E019ull) + (h << 6) + (h >> 2);
			return static_cast<std::size_t>(h ^ (h >> 32));
		}
	};
	struct Entry
	{
		std::uint64_t size;
		Nanos at;
		std::uint64_t sequence;
	};

	void rememberFreedLocked(const Key& key);
	void pushEventLocked(TrackerEvent::Kind kind, const Key& key);
	CategoryStats& statsLocked(CategoryId cat);

	ClockFn clock_;
	mutable std::mutex mutex_;
	bool enabled_ = true;
	std::vector<CategoryStats> categories_;
	std::unordered_map<std::string, CategoryId> byName_;
	std::unordered_map<Key, Entry, KeyHash> active_;

	// Recently freed keys (bounded): key -> generation, plus FIFO for eviction.
	std::size_t recentCapacity_;
	std::unordered_map<Key, std::uint64_t, KeyHash> recentFreed_;
	std::deque<std::pair<Key, std::uint64_t>> recentOrder_;
	std::uint64_t recentGeneration_ = 0;

	std::size_t eventCapacity_;
	std::deque<TrackerEvent> events_;

	std::uint64_t sequence_ = 0;
};

} // namespace memguard
