#include "fake_clock.hpp"
#include "test_framework.hpp"

#include "../src/core/profiler.hpp"

#include <thread>
#include <vector>

using namespace memguard;

TEST(profiler_counts_and_accumulates)
{
	Profiler p(&mgtest::fakeClock);
	const ZoneId z = p.zone("SetPlayerPos", ZoneKind::Native);
	const Nanos durations[] = { 100, 300, 200 };
	for (Nanos d : durations)
	{
		const auto tok = p.begin(z);
		mgtest::advance(d);
		CHECK_EQ(p.end(tok), d);
	}
	const auto s = p.stats()[z];
	CHECK_EQ(s.calls, 3ull);
	CHECK_EQ(s.totalNs, 600ull);
	CHECK_EQ(s.selfNs, 600ull);
	CHECK_EQ(s.maxNs, 300ull);
	CHECK_EQ(s.minNs, 100ull);
	CHECK_EQ(s.averageNs(), 200ull);
}

TEST(profiler_zone_registration_is_idempotent)
{
	Profiler p(&mgtest::fakeClock);
	const ZoneId a = p.zone("a", ZoneKind::Native);
	const ZoneId b = p.zone("b", ZoneKind::Script);
	CHECK(a != b);
	CHECK_EQ(p.zone("a", ZoneKind::Native), a);
	CHECK(p.findZone("b").has_value());
	CHECK(!p.findZone("c").has_value());
	CHECK(p.zoneName(b) == "b");
}

TEST(profiler_nested_self_time)
{
	Profiler p(&mgtest::fakeClock);
	const ZoneId outer = p.zone("outer", ZoneKind::Script);
	const ZoneId inner = p.zone("inner", ZoneKind::Native);
	const auto t1 = p.begin(outer);
	mgtest::advance(10);
	const auto t2 = p.begin(inner);
	mgtest::advance(70);
	p.end(t2);
	mgtest::advance(20);
	p.end(t1);
	const auto all = p.stats();
	CHECK_EQ(all[outer].totalNs, 100ull);
	CHECK_EQ(all[outer].selfNs, 30ull);
	CHECK_EQ(all[inner].totalNs, 70ull);
	CHECK_EQ(all[inner].selfNs, 70ull);
	CHECK_EQ(p.openFrames(), static_cast<std::size_t>(0));
}

TEST(profiler_abandoned_frames_are_discarded_not_invented)
{
	Profiler p(&mgtest::fakeClock);
	const ZoneId native = p.zone("CallLocalFunction", ZoneKind::Native);
	const ZoneId zone = p.zone("leaky_section", ZoneKind::Script);
	const auto outer = p.begin(native);
	p.begin(zone); // the script opened a zone and then aborted without closing it
	mgtest::advance(50);
	p.end(outer);
	const auto all = p.stats();
	CHECK_EQ(all[native].calls, 1ull);
	CHECK_EQ(all[native].totalNs, 50ull);
	CHECK_EQ(all[zone].calls, 0ull); // never recorded
	CHECK_EQ(p.counters().abandonedFrames, 1ull);
}

TEST(profiler_end_zone_by_id)
{
	Profiler p(&mgtest::fakeClock);
	const ZoneId a = p.zone("a", ZoneKind::Script);
	const ZoneId b = p.zone("b", ZoneKind::Script);
	p.begin(a);
	mgtest::advance(5);
	p.begin(b);
	mgtest::advance(5);
	CHECK(p.endZone(b));
	CHECK(p.endZone(a));
	CHECK(!p.endZone(a)); // nothing open anymore
	CHECK_EQ(p.counters().unmatchedEnds, 1ull);
	const auto all = p.stats();
	CHECK_EQ(all[a].totalNs, 10ull);
	CHECK_EQ(all[a].selfNs, 5ull);
	CHECK_EQ(all[b].totalNs, 5ull);
}

TEST(profiler_disabled_records_nothing)
{
	Profiler p(&mgtest::fakeClock);
	const ZoneId z = p.zone("z", ZoneKind::Native);
	p.setEnabled(false);
	const auto tok = p.begin(z);
	CHECK(tok == Profiler::kNoFrame);
	mgtest::advance(10);
	CHECK_EQ(p.end(tok), 0ull);
	p.record(z, 99);
	CHECK_EQ(p.stats()[z].calls, 0ull);
}

TEST(profiler_invalid_zone_is_ignored)
{
	Profiler p(&mgtest::fakeClock);
	CHECK(p.begin(12345) == Profiler::kNoFrame);
	p.record(12345, 1);
	CHECK_EQ(p.zoneCount(), static_cast<std::size_t>(0));
}

TEST(profiler_depth_limit)
{
	Profiler p(&mgtest::fakeClock);
	const ZoneId z = p.zone("deep", ZoneKind::Script);
	for (std::size_t i = 0; i < Profiler::kMaxDepth + 3; ++i)
	{
		p.begin(z);
	}
	CHECK_EQ(p.openFrames(), Profiler::kMaxDepth);
	CHECK_EQ(p.counters().overflowBegins, 3ull);
	p.discardOpenFrames();
	CHECK_EQ(p.openFrames(), static_cast<std::size_t>(0));
}

TEST(profiler_sorting_by_time_and_calls)
{
	Profiler p(&mgtest::fakeClock);
	const ZoneId frequent = p.zone("frequent_cheap", ZoneKind::Native);
	const ZoneId rare = p.zone("rare_expensive", ZoneKind::Native);
	const ZoneId script = p.zone("script_zone", ZoneKind::Script);
	for (int i = 0; i < 100; ++i)
	{
		p.record(frequent, 1);
	}
	p.record(rare, 1000);
	p.record(rare, 3000);
	p.record(script, 5);

	auto byTime = p.sorted(SortKey::TotalTime, 10, ZoneKind::Native);
	CHECK_EQ(byTime.size(), static_cast<std::size_t>(2));
	CHECK(byTime[0].name == "rare_expensive");
	auto byCalls = p.sorted(SortKey::Calls, 10, ZoneKind::Native);
	CHECK(byCalls[0].name == "frequent_cheap");
	auto byMax = p.sorted(SortKey::MaxTime, 1);
	CHECK_EQ(byMax.size(), static_cast<std::size_t>(1));
	CHECK(byMax[0].name == "rare_expensive");
	CHECK_EQ(byMax[0].maxNs, 3000ull);
	auto avg = p.sorted(SortKey::AverageTime, 10);
	CHECK(avg[0].name == "rare_expensive");
	CHECK_EQ(avg[0].averageNs(), 2000ull);
}

TEST(profiler_slow_threshold_counts)
{
	Profiler p(&mgtest::fakeClock);
	const ZoneId z = p.zone("z", ZoneKind::Native);
	p.setSlowThreshold(100);
	p.record(z, 50);
	p.record(z, 101);
	p.record(z, 100);
	CHECK_EQ(p.stats()[z].slowCalls, 1ull);
	const ZoneId interval = p.zone("tick", ZoneKind::Interval);
	p.record(interval, 1000);
	CHECK_EQ(p.stats()[interval].slowCalls, 0ull);
}

TEST(profiler_reset_keeps_zones)
{
	Profiler p(&mgtest::fakeClock);
	const ZoneId z = p.zone("z", ZoneKind::Native);
	p.record(z, 5);
	p.reset();
	CHECK_EQ(p.stats()[z].calls, 0ull);
	CHECK_EQ(p.stats()[z].minNs, 0ull);
	CHECK_EQ(p.zone("z", ZoneKind::Native), z);
}

TEST(profiler_concurrent_recording_is_exact)
{
	Profiler p; // real clock; only counts and sums of fixed samples are checked
	const ZoneId z = p.zone("shared", ZoneKind::Internal);
	constexpr int kThreads = 8;
	constexpr int kPerThread = 20000;
	std::vector<std::thread> threads;
	for (int t = 0; t < kThreads; ++t)
	{
		threads.emplace_back([&] {
			const ZoneId local = p.zone("shared", ZoneKind::Internal); // concurrent lookup
			for (int i = 0; i < kPerThread; ++i)
			{
				p.record(local, 3);
				p.end(p.begin(local)); // per-thread stacks must not interfere
			}
		});
	}
	for (auto& t : threads)
	{
		t.join();
	}
	const auto s = p.stats()[z];
	CHECK_EQ(s.calls, static_cast<std::uint64_t>(kThreads) * kPerThread * 2);
	CHECK(s.totalNs >= static_cast<std::uint64_t>(kThreads) * kPerThread * 3);
	CHECK_EQ(p.counters().abandonedFrames, 0ull);
	CHECK_EQ(p.counters().unmatchedEnds, 0ull);
}

TEST(profiler_concurrent_registration_unique_ids)
{
	Profiler p;
	constexpr int kThreads = 6;
	std::vector<std::vector<ZoneId>> ids(kThreads);
	std::vector<std::thread> threads;
	for (int t = 0; t < kThreads; ++t)
	{
		threads.emplace_back([&, t] {
			for (int i = 0; i < 300; ++i)
			{
				ids[t].push_back(p.zone("zone_" + std::to_string(i), ZoneKind::Native));
			}
		});
	}
	for (auto& t : threads)
	{
		t.join();
	}
	CHECK_EQ(p.zoneCount(), static_cast<std::size_t>(300));
	for (int t = 1; t < kThreads; ++t)
	{
		CHECK(ids[t] == ids[0]);
	}
}

TEST(profiler_overhead_measurement_returns_value)
{
	// No timing threshold: only checks the measurement runs and yields a finite number.
	const Nanos ns = Profiler::measureOverheadNs();
	CHECK(ns < 1000000000ull);
}

TEST(profiler_serial_frames_close_only_their_own_frame)
{
	Profiler p(&mgtest::fakeClock);
	const ZoneId f = p.zone("Outer", ZoneKind::Function);
	const ZoneId g = p.zone("Inner", ZoneKind::Function);
	const std::uint32_t s1 = p.beginSerial(f);
	mgtest::advance(10);
	const std::uint32_t s2 = p.beginSerial(g);
	CHECK(s1 != 0 && s2 != 0 && s1 != s2);
	mgtest::advance(30);
	CHECK(p.endSerial(s2));
	CHECK(!p.endSerial(s2)); // already closed: never closes another frame
	mgtest::advance(5);
	CHECK(p.endSerial(s1));
	const auto outer = *p.zoneStats(f);
	const auto inner = *p.zoneStats(g);
	CHECK_EQ(outer.calls, 1ull);
	CHECK_EQ(outer.totalNs, 45ull);
	CHECK_EQ(outer.selfNs, 15ull);
	CHECK_EQ(inner.totalNs, 30ull);
	CHECK_EQ(p.counters().unmatchedEnds, 1ull);
	CHECK(!p.endSerial(0));
}

TEST(profiler_serial_after_discard_is_rejected)
{
	Profiler p(&mgtest::fakeClock);
	const ZoneId f = p.zone("Aborted", ZoneKind::Function);
	const std::uint32_t s = p.beginSerial(f);
	p.discardOpenFrames(); // e.g. tick boundary after a runtime error
	const std::uint32_t s2 = p.beginSerial(f);
	CHECK(!p.endSerial(s)); // stale handle must not close the new frame
	CHECK(p.endSerial(s2));
	CHECK_EQ(p.zoneStats(f)->calls, 1ull);
	CHECK_EQ(p.counters().abandonedFrames, 1ull);
}

TEST(profiler_recursion_counts_every_call)
{
	Profiler p(&mgtest::fakeClock);
	const ZoneId f = p.zone("Fib", ZoneKind::Function);
	// depth-3 recursion: each level spends 1 ns itself
	std::uint32_t s[3];
	for (int i = 0; i < 3; ++i)
	{
		s[i] = p.beginSerial(f);
		mgtest::advance(1);
	}
	for (int i = 2; i >= 0; --i)
	{
		CHECK(p.endSerial(s[i]));
	}
	const auto z = *p.zoneStats(f);
	CHECK_EQ(z.calls, 3ull);
	CHECK_EQ(z.selfNs, 3ull); // self time is exact
	CHECK_EQ(z.totalNs, 6ull); // inclusive time counts nested recursive calls again (3+2+1)
}
