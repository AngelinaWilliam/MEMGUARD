#include "fake_clock.hpp"
#include "test_framework.hpp"

#include "../src/core/config.hpp"
#include "../src/core/engine.hpp"
#include "../src/core/logger.hpp"
#include "../src/core/snapshot.hpp"

#include <cstdio>
#include <fstream>
#include <sstream>

using namespace memguard;

namespace {

Snapshot makeSnap(std::uint64_t timeMs, std::int64_t value, MetricKind kind = MetricKind::Count)
{
	Snapshot s;
	s.timeMs = timeMs;
	s.metrics.push_back(Metric { "m", value, kind });
	return s;
}

std::uint64_t g_fakeResident = 100ull * 1024 * 1024;
ProcessMemory fakeProcess()
{
	ProcessMemory pm;
	pm.valid = true;
	pm.residentBytes = g_fakeResident;
	pm.privateBytes = g_fakeResident / 2;
	return pm;
}

} // namespace

// ---------------------------------------------------------------- snapshots / growth

TEST(growth_detects_sustained_increase)
{
	SnapshotHistory h(10);
	for (int i = 0; i < 6; ++i)
	{
		h.push(makeSnap(static_cast<std::uint64_t>(i) * 60000, 100 + i * 20));
	}
	GrowthPolicy policy;
	policy.window = 6;
	policy.minDeltaCount = 10;
	policy.minPercent = 10;
	auto f = detectGrowth(h, policy);
	CHECK_EQ(f.size(), static_cast<std::size_t>(1));
	if (!f.empty())
	{
		CHECK_EQ(f[0].first, 100ll);
		CHECK_EQ(f[0].last, 200ll);
		CHECK_EQ(f[0].delta, 100ll);
		CHECK_EQ(f[0].increasingSteps, static_cast<std::size_t>(5));
		CHECK(f[0].perMinute > 19.9 && f[0].perMinute < 20.1);
	}
}

TEST(growth_ignores_stable_and_noisy_series)
{
	SnapshotHistory h(10);
	const std::int64_t noisy[] = { 100, 150, 90, 160, 95, 120 };
	for (int i = 0; i < 6; ++i)
	{
		h.push(makeSnap(static_cast<std::uint64_t>(i) * 1000, noisy[i]));
	}
	CHECK(detectGrowth(h, GrowthPolicy {}).empty());

	SnapshotHistory flat(10);
	for (int i = 0; i < 6; ++i)
	{
		flat.push(makeSnap(static_cast<std::uint64_t>(i) * 1000, 500));
	}
	CHECK(detectGrowth(flat, GrowthPolicy {}).empty());
}

TEST(growth_respects_minimum_delta_and_window)
{
	SnapshotHistory h(10);
	for (int i = 0; i < 6; ++i)
	{
		h.push(makeSnap(static_cast<std::uint64_t>(i) * 1000, 1000 + i)); // +5 total, 0.5%
	}
	CHECK(detectGrowth(h, GrowthPolicy {}).empty());

	SnapshotHistory few(10);
	for (int i = 0; i < 3; ++i)
	{
		few.push(makeSnap(static_cast<std::uint64_t>(i) * 1000, i * 1000));
	}
	CHECK(detectGrowth(few, GrowthPolicy {}).empty()); // fewer snapshots than the window

	SnapshotHistory bytes(10);
	for (int i = 0; i < 6; ++i)
	{
		bytes.push(makeSnap(static_cast<std::uint64_t>(i) * 1000, 100000000 + i * 1000000, MetricKind::Bytes));
	}
	CHECK(detectGrowth(bytes, GrowthPolicy {}).empty()); // +5 MB < 8 MiB minimum for bytes
}

TEST(snapshot_history_is_bounded_and_sequenced)
{
	SnapshotHistory h(3);
	for (int i = 0; i < 5; ++i)
	{
		h.push(makeSnap(static_cast<std::uint64_t>(i), i));
	}
	CHECK_EQ(h.size(), static_cast<std::size_t>(3));
	CHECK_EQ(h.snapshots().front().sequence, 3ull);
	CHECK_EQ(h.latest()->sequence, 5ull);
	auto series = h.series("m");
	CHECK_EQ(series.size(), static_cast<std::size_t>(3));
	CHECK_EQ(series[0].second, 2ll);
	auto diff = diffSnapshots(h.snapshots().front(), *h.latest());
	CHECK_EQ(diff.size(), static_cast<std::size_t>(1));
	CHECK_EQ(diff[0].after - diff[0].before, 2ll);
}

// ---------------------------------------------------------------- config

TEST(config_applies_valid_values)
{
	Config c;
	std::string err;
	CHECK(applyConfigValue(c, "report_interval", "120", err));
	CHECK_EQ(c.reportIntervalSec, 120u);
	CHECK(applyConfigValue(c, "profile_natives", "false", err));
	CHECK(!c.profileNatives);
	CHECK(applyConfigValue(c, "log_level", "warning", err));
	CHECK(c.logLevel == LogLevel::Warning);
	CHECK(applyConfigValue(c, "report_file", "logs/mg.txt", err));
	CHECK(c.reportFile == "logs/mg.txt");
	CHECK(configValueAsString(c, "profile_natives") == "false");
	CHECK(configValueAsString(c, "log_level") == "warning");
}

TEST(config_rejects_invalid_values_without_side_effects)
{
	Config c;
	std::string err;
	CHECK(!applyConfigValue(c, "top_n", "0", err)); // below minimum
	CHECK_EQ(c.topN, 15u);
	CHECK(!applyConfigValue(c, "top_n", "abc", err));
	CHECK(!applyConfigValue(c, "top_n", "-3", err));
	CHECK(!applyConfigValue(c, "enable", "maybe", err));
	CHECK(c.enabled);
	CHECK(!applyConfigValue(c, "no_such_key", "1", err));
	CHECK_STR_CONTAINS(err, "unknown key");
	CHECK(!applyConfigValue(c, "log_level", "loud", err));
	CHECK(!applyConfigValue(c, "report_file", "", err));
	// every documented key round-trips its default value
	for (const auto& info : configKeys())
	{
		Config d;
		std::string e;
		CHECK(applyConfigValue(d, info.key, configValueAsString(d, info.key), e));
	}
}

// ---------------------------------------------------------------- logger

TEST(logger_filters_by_level)
{
	Logger log(&mgtest::fakeClock);
	std::vector<std::string> got;
	log.setSink([&](LogLevel, const std::string& m) { got.push_back(m); });
	log.setLevel(LogLevel::Warning);
	log.log(LogLevel::Info, "info");
	log.log(LogLevel::Warning, "warn");
	log.logf(LogLevel::Error, "err %d", 7);
	log.logf(LogLevel::Debug, "dbg %d", 1);
	CHECK_EQ(got.size(), static_cast<std::size_t>(2));
	CHECK(got[1] == "err 7");
	log.setLevel(LogLevel::Off);
	log.log(LogLevel::Error, "x");
	CHECK_EQ(got.size(), static_cast<std::size_t>(2));
}

TEST(logger_rate_limits_by_key)
{
	Logger log(&mgtest::fakeClock);
	std::vector<std::string> got;
	log.setSink([&](LogLevel, const std::string& m) { got.push_back(m); });
	log.setCooldownSeconds(10);
	CHECK(log.logLimited(LogLevel::Warning, "k", "first"));
	CHECK(!log.logLimited(LogLevel::Warning, "k", "second"));
	CHECK(!log.logLimited(LogLevel::Warning, "k", "third"));
	CHECK(log.logLimited(LogLevel::Warning, "other", "independent"));
	mgtest::advance(10ull * 1000000000ull);
	CHECK(log.logLimited(LogLevel::Warning, "k", "later"));
	CHECK_EQ(got.size(), static_cast<std::size_t>(3));
	CHECK_STR_CONTAINS(got[2], "+2 similar suppressed");
	CHECK_EQ(log.suppressedCount(), 2ull);
}

// ---------------------------------------------------------------- engine

TEST(engine_tick_interval_and_periodic_snapshot)
{
	Engine e(&mgtest::fakeClock);
	e.setProcessMemorySource(&fakeProcess);
	Config c;
	c.snapshotIntervalSec = 1;
	c.reportIntervalSec = 0;
	e.applyConfig(c);
	for (int i = 0; i < 25; ++i)
	{
		mgtest::advance(100ull * 1000000ull); // 100 ms per tick
		e.onTick();
	}
	const auto tick = e.profiler().stats()[e.tickZone()];
	CHECK_EQ(tick.calls, 24ull);
	CHECK_EQ(tick.totalNs, 24ull * 100ull * 1000000ull);
	CHECK_EQ(e.history().size(), static_cast<std::size_t>(2)); // at 1.0 s and 2.0 s
	CHECK(e.history().latest()->find("process.resident_bytes") != nullptr);
}

TEST(engine_snapshot_metrics_and_growth_warning)
{
	Engine e(&mgtest::fakeClock);
	e.setProcessMemorySource(&fakeProcess);
	std::vector<std::string> warnings;
	e.logger().setSink([&](LogLevel l, const std::string& m) {
		if (l == LogLevel::Warning)
			warnings.push_back(m);
	});
	const CategoryId objects = e.tracker().category("object", false);
	const CategoryId buffers = e.tracker().category("buffer", true);
	std::int64_t extra = 0;
	e.addMetricProvider("test", [&](std::vector<Metric>& out) { out.push_back(Metric { "test.gauge", extra, MetricKind::Count }); });
	std::string err;
	CHECK(e.setConfigValue("warmup_seconds", "0", err)); // this test is about growth detection itself
	std::uint64_t nextId = 0;
	for (int s = 0; s < 6; ++s)
	{
		for (int i = 0; i < 20; ++i)
		{
			e.tracker().onAlloc(objects, nextId++, 0); // leaking 20 objects per interval
		}
		e.tracker().onAlloc(buffers, 1000 + s, 16);
		e.tracker().onFree(buffers, 1000 + s); // balanced
		mgtest::advance(60ull * 1000000000ull);
		e.takeSnapshot();
	}
	const Snapshot* last = e.history().latest();
	CHECK(last != nullptr);
	CHECK(last->find("tracked.object.active") != nullptr);
	CHECK(last->find("tracked.object.bytes") == nullptr); // size not observable
	CHECK(last->find("tracked.buffer.bytes") != nullptr);
	CHECK_EQ(last->find("tracked.object.active")->value, 120ll);
	CHECK_EQ(last->find("test.gauge")->value, 0ll);
	CHECK_EQ(e.lastGrowthFindings().size(), static_cast<std::size_t>(1));
	CHECK(e.lastGrowthFindings()[0].metric == "tracked.object.active");
	CHECK_EQ(warnings.size(), static_cast<std::size_t>(1));
	if (!warnings.empty())
	{
		CHECK_STR_CONTAINS(warnings[0], "Possible leak");
	}
	CHECK_STR_CONTAINS(e.formatGrowth(), "POSSIBLE LEAK tracked.object.active");
}

TEST(engine_report_contains_all_sections_and_is_written)
{
	Engine e(&mgtest::fakeClock);
	e.setProcessMemorySource(&fakeProcess);
	Config c;
	c.reportFile = "memguard_test_report.txt";
	e.applyConfig(c);
	const ZoneId z = e.profiler().zone("SendClientMessage", ZoneKind::Native);
	e.profiler().record(z, 1500);
	const CategoryId cat = e.tracker().category("pawn.buffer");
	e.tracker().onAlloc(cat, 1, 64);
	e.tracker().onFree(cat, 2);
	e.takeSnapshot();
	const std::string report = e.buildReport();
	CHECK_STR_CONTAINS(report, "MemGuard X status");
	CHECK_STR_CONTAINS(report, "SendClientMessage");
	CHECK_STR_CONTAINS(report, "pawn.buffer");
	CHECK_STR_CONTAINS(report, "free of unknown id");
	CHECK_STR_CONTAINS(report, "Snapshot history");
	CHECK_STR_CONTAINS(report, "Growth analysis");
	std::string err;
	CHECK(e.writeReport(err));
	std::ifstream in("memguard_test_report.txt");
	std::stringstream ss;
	ss << in.rdbuf();
	CHECK_STR_CONTAINS(ss.str(), "SendClientMessage");
	in.close();
	std::remove("memguard_test_report.txt");
	CHECK_EQ(e.reportsWritten(), 1ull);
}

TEST(engine_report_write_failure_is_reported)
{
	Engine e(&mgtest::fakeClock);
	std::string err;
	CHECK(e.setConfigValue("report_file", "no_such_directory_mgx/sub/report.txt", err));
	CHECK(!e.writeReport(err));
	CHECK_STR_CONTAINS(err, "cannot open");
}

TEST(engine_commands)
{
	Engine e(&mgtest::fakeClock);
	e.setProcessMemorySource(&fakeProcess);
	const ZoneId a = e.profiler().zone("GetPlayerPos", ZoneKind::Native);
	const ZoneId b = e.profiler().zone("format", ZoneKind::Native);
	e.profiler().record(a, 10);
	e.profiler().record(a, 10);
	e.profiler().record(b, 1000);
	CHECK_STR_CONTAINS(e.handleCommand("help"), "memguard");
	CHECK_STR_CONTAINS(e.handleCommand("status"), "uptime");
	const std::string byCalls = e.handleCommand("top calls 1 native");
	CHECK_STR_CONTAINS(byCalls, "GetPlayerPos");
	CHECK(byCalls.find("format") == std::string::npos);
	CHECK_STR_CONTAINS(e.handleCommand("top total 1"), "format");
	CHECK_STR_CONTAINS(e.handleCommand("top bogus"), "Unknown argument");
	CHECK_STR_CONTAINS(e.handleCommand("snapshot"), "Snapshot #1");
	CHECK_STR_CONTAINS(e.handleCommand("set top_n 3"), "= 3");
	CHECK_EQ(e.config().topN, 3u);
	CHECK_STR_CONTAINS(e.handleCommand("set top_n 0"), "Rejected");
	CHECK_EQ(e.config().topN, 3u);
	CHECK_STR_CONTAINS(e.handleCommand("get log_level"), "info");
	CHECK_STR_CONTAINS(e.handleCommand("set log_level debug"), "debug");
	CHECK(e.logger().level() == LogLevel::Debug);
	CHECK_STR_CONTAINS(e.handleCommand("reset profiler"), "Reset profiler");
	CHECK_EQ(e.profiler().stats()[a].calls, 0ull);
	CHECK_STR_CONTAINS(e.handleCommand("frobnicate"), "Unknown command");
	CHECK_STR_CONTAINS(e.handleCommand("config"), "memguard.report_interval");
	CHECK_STR_CONTAINS(e.handleCommand("mem 5"), "Tracked allocations");
	CHECK_STR_CONTAINS(e.handleCommand("history"), "Snapshot history");
}

TEST(engine_disable_stops_recording)
{
	Engine e(&mgtest::fakeClock);
	std::string err;
	CHECK(e.setConfigValue("enable", "0", err));
	const ZoneId z = e.profiler().zone("x", ZoneKind::Native);
	e.profiler().record(z, 5);
	const CategoryId c = e.tracker().category("c");
	CHECK(e.tracker().onAlloc(c, 1, 1) == AllocResult::Disabled);
	CHECK_EQ(e.profiler().stats()[z].calls, 0ull);
}
