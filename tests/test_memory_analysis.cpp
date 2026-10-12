#include "fake_clock.hpp"
#include "test_framework.hpp"

#include "../src/core/engine.hpp"
#include "../src/core/memory_analysis.hpp"

using namespace memguard;

namespace {

constexpr std::int64_t MiB = 1024 * 1024;

struct Sample
{
	std::int64_t priv;
	std::int64_t heapUsed;
	std::int64_t heapFree;
	std::int64_t objects;
};

Snapshot snap(std::uint64_t minute, const Sample& s)
{
	Snapshot out;
	out.timeMs = minute * 60000ull;
	out.metrics = {
		{ "process.private_bytes", s.priv, MetricKind::Bytes },
		{ "process.heap_in_use_bytes", s.heapUsed, MetricKind::Bytes },
		{ "process.heap_free_bytes", s.heapFree, MetricKind::Bytes },
		{ "tracked.pool.object.active", s.objects, MetricKind::Count },
	};
	return out;
}

// Builds a history with one snapshot per minute; warm-up = 2 minutes.
MemoryAssessment run(const std::vector<Sample>& samples)
{
	SnapshotHistory h(100);
	for (std::size_t i = 0; i < samples.size(); ++i)
	{
		h.push(snap(i, samples[i]));
	}
	const auto& items = h.snapshots();
	const Snapshot* warm = items.size() > 2 ? &items[2] : nullptr;
	GrowthPolicy p;
	p.window = 4;
	return assessMemory(h, items.front(), warm, 2 * 60000ull, p);
}

const MetricAssessment* metric(const MemoryAssessment& a, const char* name)
{
	for (const auto& m : a.metrics)
	{
		if (m.metric == name)
		{
			return &m;
		}
	}
	return nullptr;
}

bool interpretationContains(const MemoryAssessment& a, const char* text)
{
	for (const auto& line : a.interpretation)
	{
		if (line.find(text) != std::string::npos)
		{
			return true;
		}
	}
	return false;
}

} // namespace

TEST(memory_startup_spike_is_not_a_leak)
{
	// +200 MiB while loading, then flat.
	std::vector<Sample> s = { { 50 * MiB, 20 * MiB, 1 * MiB, 0 }, { 200 * MiB, 150 * MiB, 5 * MiB, 500 }, { 250 * MiB, 190 * MiB, 5 * MiB, 500 } };
	for (int i = 0; i < 6; ++i)
	{
		s.push_back({ 250 * MiB, 190 * MiB, 5 * MiB, 500 });
	}
	const MemoryAssessment a = run(s);
	CHECK(a.warmupComplete);
	const MetricAssessment* p = metric(a, "process.private_bytes");
	CHECK(p != nullptr);
	CHECK(p->trend == TrendClass::Stable);
	CHECK_EQ(p->startupDelta, 200 * MiB);
	CHECK_EQ(p->sinceWarmupDelta, 0ll);
	CHECK(interpretationContains(a, "Startup: process memory changed by +200.0 MiB"));
	CHECK(interpretationContains(a, "No metric is rising"));
	for (const auto& m : a.metrics)
	{
		CHECK(m.trend != TrendClass::Rising);
	}
}

TEST(memory_warmup_in_progress)
{
	const MemoryAssessment a = run({ { 50 * MiB, 20 * MiB, 0, 0 }, { 150 * MiB, 100 * MiB, 0, 0 } });
	CHECK(!a.warmupComplete);
	CHECK(metric(a, "process.private_bytes")->trend == TrendClass::Warmup);
	CHECK(interpretationContains(a, "Warm-up in progress"));
}

TEST(memory_grew_then_stable_is_classified_as_plateau)
{
	std::vector<Sample> s = { { 100 * MiB, 50 * MiB, 0, 0 }, { 100 * MiB, 50 * MiB, 0, 0 }, { 100 * MiB, 50 * MiB, 0, 0 },
		{ 160 * MiB, 110 * MiB, 0, 0 } };
	for (int i = 0; i < 6; ++i)
	{
		s.push_back({ 160 * MiB, 110 * MiB, 0, 0 });
	}
	const MemoryAssessment a = run(s);
	CHECK(metric(a, "process.private_bytes")->trend == TrendClass::GrewThenStable);
	CHECK(interpretationContains(a, "flat in the recent snapshots"));
}

TEST(memory_live_heap_growth_without_tracked_growth)
{
	std::vector<Sample> s;
	for (int i = 0; i < 10; ++i)
	{
		s.push_back({ (100 + 20 * i) * MiB, (50 + 20 * i) * MiB, 2 * MiB, 100 });
	}
	const MemoryAssessment a = run(s);
	CHECK(metric(a, "process.heap_in_use_bytes")->trend == TrendClass::Rising);
	CHECK(metric(a, "tracked.pool.object.active")->trend == TrendClass::Stable);
	CHECK(interpretationContains(a, "no tracked resource is rising"));
	CHECK(interpretationContains(a, "Possible leak"));
}

TEST(memory_allocator_retention_is_not_live_growth)
{
	std::vector<Sample> s;
	for (int i = 0; i < 10; ++i)
	{
		s.push_back({ (100 + 15 * i) * MiB, 50 * MiB, (5 + 15 * i) * MiB, 100 });
	}
	const MemoryAssessment a = run(s);
	CHECK(metric(a, "process.private_bytes")->trend == TrendClass::Rising);
	CHECK(metric(a, "process.heap_in_use_bytes")->trend == TrendClass::Stable);
	CHECK(interpretationContains(a, "freed memory kept by the allocator"));
}

TEST(memory_tracked_pool_growth_is_named)
{
	std::vector<Sample> s;
	for (int i = 0; i < 10; ++i)
	{
		s.push_back({ 100 * MiB, 50 * MiB, 1 * MiB, 100 + 50 * i });
	}
	const MemoryAssessment a = run(s);
	CHECK(metric(a, "tracked.pool.object.active")->trend == TrendClass::Rising);
	CHECK(interpretationContains(a, "Tracked resources still rising: tracked.pool.object.active"));
}

TEST(memory_decreasing)
{
	std::vector<Sample> s = { { 300 * MiB, 200 * MiB, 0, 0 }, { 300 * MiB, 200 * MiB, 0, 0 }, { 300 * MiB, 200 * MiB, 0, 0 } };
	for (int i = 0; i < 6; ++i)
	{
		s.push_back({ 200 * MiB, 100 * MiB, 0, 0 });
	}
	const MemoryAssessment a = run(s);
	CHECK(metric(a, "process.private_bytes")->trend == TrendClass::Decreasing);
}

namespace {
std::uint64_t g_proc = 100;
ProcessMemory fakeProc()
{
	ProcessMemory pm;
	pm.valid = true;
	pm.residentBytes = g_proc * MiB;
	pm.privateBytes = g_proc * MiB;
	pm.heapValid = true;
	pm.heapSource = "fake";
	pm.heapInUseBytes = g_proc * MiB / 2;
	return pm;
}
} // namespace

TEST(engine_no_growth_warning_during_warmup)
{
	Engine e(&mgtest::fakeClock);
	e.setProcessMemorySource(&fakeProc);
	std::vector<std::string> warnings;
	e.logger().setSink([&](LogLevel l, const std::string& m) {
		if (l == LogLevel::Warning)
			warnings.push_back(m);
	});
	std::string err;
	CHECK(e.setConfigValue("warmup_seconds", "300", err));
	CHECK(e.setConfigValue("growth_window", "4", err));
	g_proc = 100;
	// strong growth during the first 5 minutes only
	for (int i = 0; i < 5; ++i)
	{
		g_proc += 100;
		e.takeSnapshot();
		mgtest::advance(60ull * 1000000000ull);
	}
	CHECK(warnings.empty());
	for (int i = 0; i < 6; ++i)
	{
		e.takeSnapshot(); // flat afterwards
		mgtest::advance(60ull * 1000000000ull);
	}
	CHECK(warnings.empty());
	CHECK(e.lastGrowthFindings().empty());
	const auto a = e.assessMemoryNow();
	CHECK(a && a->warmupComplete);
	CHECK_STR_CONTAINS(e.formatMemoryAssessment(), "Startup: process memory changed by");
	CHECK_STR_CONTAINS(e.buildReport(), "What MemGuard X can and cannot see");
	// sustained growth after warm-up is reported
	for (int i = 0; i < 5; ++i)
	{
		g_proc += 50;
		e.takeSnapshot();
		mgtest::advance(60ull * 1000000000ull);
	}
	CHECK(!warnings.empty());
	if (!warnings.empty())
	{
		CHECK_STR_CONTAINS(warnings[0], "sustained growth after warm-up");
	}
	// a gamemode restart starts a new warm-up
	e.markPhase("gamemode restart");
	CHECK(e.lastGrowthFindings().empty());
	e.takeSnapshot();
	CHECK(!e.assessMemoryNow()->warmupComplete);
}
