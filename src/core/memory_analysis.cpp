#include "memory_analysis.hpp"

#include <cstdio>

namespace memguard {

const char* trendClassName(TrendClass c)
{
	switch (c)
	{
	case TrendClass::Warmup:
		return "warm-up";
	case TrendClass::Stable:
		return "stable";
	case TrendClass::GrewThenStable:
		return "grew, now flat";
	case TrendClass::Rising:
		return "RISING";
	case TrendClass::Decreasing:
		return "decreasing";
	}
	return "?";
}

std::vector<GrowthFinding> detectGrowthSince(const SnapshotHistory& history, std::uint64_t fromMs, const GrowthPolicy& policy)
{
	SnapshotHistory filtered(history.capacity());
	for (const Snapshot& s : history.snapshots())
	{
		if (s.timeMs >= fromMs)
		{
			filtered.push(s);
		}
	}
	return detectGrowth(filtered, policy);
}

namespace {

	std::string mib(std::int64_t bytes)
	{
		char buf[48];
		std::snprintf(buf, sizeof(buf), "%s%.1f MiB", bytes < 0 ? "-" : "+", static_cast<double>(bytes < 0 ? -bytes : bytes) / (1024.0 * 1024.0));
		return buf;
	}

	bool significant(std::int64_t delta, std::int64_t base, MetricKind kind, const GrowthPolicy& policy)
	{
		const std::int64_t minDelta = kind == MetricKind::Bytes ? policy.minDeltaBytes : policy.minDeltaCount;
		const std::int64_t magnitude = delta < 0 ? -delta : delta;
		if (magnitude < minDelta || magnitude == 0)
		{
			return false;
		}
		if (base > 0)
		{
			return 100.0 * static_cast<double>(magnitude) / static_cast<double>(base) >= policy.minPercent;
		}
		return true;
	}

	const MetricAssessment* find(const MemoryAssessment& a, const std::string& name)
	{
		for (const MetricAssessment& m : a.metrics)
		{
			if (m.metric == name)
			{
				return &m;
			}
		}
		return nullptr;
	}

} // namespace

MemoryAssessment assessMemory(const SnapshotHistory& history, const Snapshot& phaseStart, const Snapshot* warmupEnd,
	std::uint64_t warmupMs, const GrowthPolicy& policy)
{
	MemoryAssessment out;
	out.phaseStartMs = phaseStart.timeMs;
	out.warmupEndMs = phaseStart.timeMs + warmupMs;
	out.warmupComplete = warmupEnd != nullptr;
	const Snapshot* latest = history.latest();
	if (!latest)
	{
		return out;
	}
	for (const Snapshot& s : history.snapshots())
	{
		if (warmupEnd && s.timeMs >= warmupEnd->timeMs)
		{
			++out.postWarmupSnapshots;
		}
	}

	std::vector<GrowthFinding> recent;
	if (warmupEnd)
	{
		recent = detectGrowthSince(history, warmupEnd->timeMs, policy);
	}

	for (const Metric& m : latest->metrics)
	{
		MetricAssessment a;
		a.metric = m.name;
		a.kind = m.kind;
		a.now = m.value;
		const Metric* start = phaseStart.find(m.name);
		a.atPhaseStart = start ? start->value : m.value;
		if (!warmupEnd)
		{
			a.trend = TrendClass::Warmup;
			a.startupDelta = a.now - a.atPhaseStart;
		}
		else
		{
			const Metric* w = warmupEnd->find(m.name);
			const std::int64_t base = w ? w->value : a.atPhaseStart;
			a.atWarmupEnd = base;
			a.startupDelta = base - a.atPhaseStart;
			a.sinceWarmupDelta = a.now - base;
			for (const GrowthFinding& f : recent)
			{
				if (f.metric == m.name)
				{
					a.recentGrowth = f;
				}
			}
			if (a.recentGrowth)
			{
				a.trend = TrendClass::Rising;
			}
			else if (a.sinceWarmupDelta > 0 && significant(a.sinceWarmupDelta, base, m.kind, policy))
			{
				a.trend = TrendClass::GrewThenStable;
			}
			else if (a.sinceWarmupDelta < 0 && significant(a.sinceWarmupDelta, base, m.kind, policy))
			{
				a.trend = TrendClass::Decreasing;
			}
			else
			{
				a.trend = TrendClass::Stable;
			}
		}
		out.metrics.push_back(std::move(a));
	}

	// ------------------------------------------------------------- interpretation
	const MetricAssessment* priv = find(out, "process.private_bytes");
	const MetricAssessment* rss = find(out, "process.resident_bytes");
	const MetricAssessment* heapUsed = find(out, "process.heap_in_use_bytes");
	const MetricAssessment* heapFree = find(out, "process.heap_free_bytes");
	const MetricAssessment* proc = priv ? priv : rss;

	if (!out.warmupComplete)
	{
		char buf[200];
		std::snprintf(buf, sizeof(buf), "Warm-up in progress (%llu s left): changes now are classified as startup, not as growth.",
			static_cast<unsigned long long>(out.warmupEndMs > latest->timeMs ? (out.warmupEndMs - latest->timeMs) / 1000 : 0));
		out.interpretation.emplace_back(buf);
		if (proc)
		{
			out.interpretation.push_back("Process memory so far in this phase: " + mib(proc->startupDelta) + ".");
		}
		return out;
	}

	if (proc && proc->startupDelta > 0 && significant(proc->startupDelta, proc->atPhaseStart, MetricKind::Bytes, policy))
	{
		out.interpretation.push_back("Startup: process memory changed by " + mib(proc->startupDelta)
			+ " between phase start and the end of warm-up. Loading scripts, data and caches explains this kind of growth; it is not counted as a leak.");
	}

	std::vector<std::string> risingTracked;
	for (const MetricAssessment& a : out.metrics)
	{
		if (a.trend == TrendClass::Rising && a.metric.rfind("tracked.", 0) == 0)
		{
			risingTracked.push_back(a.metric);
		}
	}
	if (!risingTracked.empty())
	{
		std::string list;
		for (const std::string& n : risingTracked)
		{
			list += (list.empty() ? "" : ", ") + n;
		}
		out.interpretation.push_back("Tracked resources still rising: " + list
			+ ". These are exact counts of objects MemGuard X observes; check that the script destroys/frees them.");
	}

	const bool heapRising = heapUsed && heapUsed->trend == TrendClass::Rising;
	const bool procRising = proc && proc->trend == TrendClass::Rising;
	if (heapRising)
	{
		if (risingTracked.empty())
		{
			out.interpretation.push_back("Live C-heap memory (in use) is rising while no tracked resource is rising: the allocations come from code "
										 "MemGuard X cannot attribute (another component or plugin, the server itself, or untracked script data). "
										 "Possible leak - investigate.");
		}
		else
		{
			out.interpretation.push_back("Live C-heap memory is rising together with tracked resources; the tracked growth is a likely contributor, "
										 "but byte sizes of pool entities are not observable, so the share cannot be computed.");
		}
	}
	else if (procRising)
	{
		if (heapUsed && heapFree && heapFree->trend == TrendClass::Rising)
		{
			out.interpretation.push_back("Process memory is rising, but live heap memory is not: the growth is freed memory kept by the allocator "
										 "(caching/fragmentation). This is not live data.");
		}
		else if (heapUsed)
		{
			out.interpretation.push_back("Process memory is rising outside the C heap (large mappings, thread stacks, other allocators). "
										 "MemGuard X cannot see what owns it. Possible leak - investigate.");
		}
		else
		{
			out.interpretation.push_back("Process memory is rising; heap statistics are unavailable on this system, so live allocations and allocator "
										 "caching cannot be told apart. Possible leak - investigate.");
		}
	}
	if (proc && proc->trend == TrendClass::GrewThenStable)
	{
		out.interpretation.push_back("Process memory grew after warm-up (" + mib(proc->sinceWarmupDelta)
			+ ") but is flat in the recent snapshots: consistent with a cache or working set reaching a steady size, not with a continuous leak.");
	}

	bool anyRising = false;
	for (const MetricAssessment& a : out.metrics)
	{
		anyRising = anyRising || a.trend == TrendClass::Rising;
	}
	if (!anyRising)
	{
		out.interpretation.push_back(out.postWarmupSnapshots < policy.window
				? "Not enough snapshots after warm-up yet to judge recent trends."
				: "No metric is rising in the recent snapshots.");
	}
	return out;
}

} // namespace memguard
