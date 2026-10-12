// MemGuard X - memory phase analysis.
//
// Separates what happened during startup (component load until the end of a warm-up period)
// from what happened afterwards, and classifies each metric:
//
//   Warmup          still inside the warm-up period; growth here is expected (scripts load
//                   data, pools fill, caches are built)
//   Stable          no significant change since the end of warm-up
//   GrewThenStable  grew after warm-up, but the recent snapshots are flat (a cache filling up
//                   to its size, more players, a one-off load) - not a leak signal by itself
//   Rising          still rising in the recent snapshots: possible leak, investigate
//   Decreasing      went down since warm-up
//
// It also combines process, C-heap and tracked metrics into short interpretations (allocator
// retention vs. live heap growth vs. tracked pool growth). Everything is phrased as evidence,
// never as a confirmed leak.
#pragma once

#include "snapshot.hpp"

#include <optional>
#include <string>
#include <vector>

namespace memguard {

enum class TrendClass
{
	Warmup,
	Stable,
	GrewThenStable,
	Rising,
	Decreasing
};

const char* trendClassName(TrendClass c);

struct MetricAssessment
{
	std::string metric;
	MetricKind kind = MetricKind::Count;
	std::int64_t atPhaseStart = 0;
	std::optional<std::int64_t> atWarmupEnd;
	std::int64_t now = 0;
	std::int64_t startupDelta = 0; ///< warmup end (or now, during warm-up) minus phase start
	std::int64_t sinceWarmupDelta = 0; ///< now minus warmup end (0 during warm-up)
	TrendClass trend = TrendClass::Stable;
	std::optional<GrowthFinding> recentGrowth; ///< set when trend == Rising
};

struct MemoryAssessment
{
	bool warmupComplete = false;
	std::uint64_t phaseStartMs = 0;
	std::uint64_t warmupEndMs = 0; ///< phaseStartMs + warm-up length
	std::size_t postWarmupSnapshots = 0;
	std::vector<MetricAssessment> metrics;
	std::vector<std::string> interpretation; ///< Human-readable conclusions (evidence-based).
};

/// `phaseStart` is the first snapshot of the current phase (server start or last gamemode
/// restart); `warmupEnd` the first snapshot taken at least `warmupMs` after it (if any yet).
/// Recent-trend detection uses only snapshots in `history` taken at or after warm-up end.
MemoryAssessment assessMemory(const SnapshotHistory& history, const Snapshot& phaseStart, const Snapshot* warmupEnd,
	std::uint64_t warmupMs, const GrowthPolicy& policy);

/// Growth detection restricted to snapshots with timeMs >= fromMs.
std::vector<GrowthFinding> detectGrowthSince(const SnapshotHistory& history, std::uint64_t fromMs, const GrowthPolicy& policy);

} // namespace memguard
