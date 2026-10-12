// MemGuard X - snapshot history and suspicious-growth detection.
//
// A snapshot is a set of named integer metrics captured at one moment (tracked allocation
// counts, tracked bytes, process memory, open.mp resource counters, AMX heap usage, ...).
// detectGrowth() flags metrics that grew in a sustained way over the last N snapshots.
// A finding means "possible leak - investigate", never "confirmed leak".
#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <string>
#include <vector>

namespace memguard {

enum class MetricKind : std::uint8_t
{
	Count,
	Bytes
};

struct Metric
{
	std::string name;
	std::int64_t value = 0;
	MetricKind kind = MetricKind::Count;
};

struct Snapshot
{
	std::uint64_t sequence = 0; ///< Assigned by SnapshotHistory::push.
	std::uint64_t timeMs = 0; ///< Monotonic milliseconds.
	std::uint64_t wallMs = 0; ///< Wall clock (for display).
	std::vector<Metric> metrics;

	const Metric* find(const std::string& name) const;
};

struct GrowthPolicy
{
	std::size_t window = 6; ///< Number of most recent snapshots examined (>= 2).
	double minIncreasingFraction = 0.8; ///< Fraction of steps that must increase.
	double minPercent = 10.0; ///< Minimum relative growth first->last (ignored if first == 0).
	std::int64_t minDeltaCount = 10; ///< Minimum absolute growth for Count metrics.
	std::int64_t minDeltaBytes = 8ll * 1024 * 1024; ///< Minimum absolute growth for Bytes metrics.
};

struct GrowthFinding
{
	std::string metric;
	MetricKind kind = MetricKind::Count;
	std::int64_t first = 0;
	std::int64_t last = 0;
	std::int64_t delta = 0;
	double percent = 0.0; ///< Negative when first == 0 (undefined percentage).
	double perMinute = 0.0;
	std::size_t increasingSteps = 0;
	std::size_t steps = 0;
	std::uint64_t spanMs = 0;
};

class SnapshotHistory
{
public:
	explicit SnapshotHistory(std::size_t capacity = 120);

	void setCapacity(std::size_t capacity);
	std::size_t capacity() const { return capacity_; }

	/// Stores the snapshot (assigning its sequence number) and returns that number.
	std::uint64_t push(Snapshot snapshot);

	const std::deque<Snapshot>& snapshots() const { return items_; }
	std::size_t size() const { return items_.size(); }
	bool empty() const { return items_.empty(); }
	const Snapshot* latest() const { return items_.empty() ? nullptr : &items_.back(); }
	void clear();

	/// Values of one metric across the stored snapshots, oldest first (missing -> skipped).
	std::vector<std::pair<std::uint64_t, std::int64_t>> series(const std::string& metric) const;

private:
	std::size_t capacity_;
	std::deque<Snapshot> items_;
	std::uint64_t nextSequence_ = 1;
};

/// Examines the last `policy.window` snapshots. Only metrics present in all of them are
/// considered. Returns findings sorted by metric name.
std::vector<GrowthFinding> detectGrowth(const SnapshotHistory& history, const GrowthPolicy& policy);

/// Difference between two snapshots: (metric, before, after) for metrics present in both.
struct MetricDelta
{
	std::string metric;
	MetricKind kind;
	std::int64_t before;
	std::int64_t after;
};
std::vector<MetricDelta> diffSnapshots(const Snapshot& before, const Snapshot& after);

} // namespace memguard
