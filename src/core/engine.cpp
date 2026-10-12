#include "engine.hpp"

#include "process_memory.hpp"

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <ctime>
#include <fstream>

namespace memguard {

namespace {

	void appendf(std::string& out, const char* fmt, ...)
#if defined(__GNUC__) || defined(__clang__)
		__attribute__((format(printf, 2, 3)))
#endif
		;

	void appendf(std::string& out, const char* fmt, ...)
	{
		char buf[1024];
		va_list args;
		va_start(args, fmt);
		const int n = std::vsnprintf(buf, sizeof(buf), fmt, args);
		va_end(args);
		if (n > 0)
		{
			out.append(buf, static_cast<std::size_t>(n) < sizeof(buf) ? static_cast<std::size_t>(n) : sizeof(buf) - 1);
		}
	}

	std::string dur(Nanos ns)
	{
		char buf[32];
		formatDuration(ns, buf, sizeof(buf));
		return buf;
	}

	std::string bytes(std::uint64_t b)
	{
		char buf[32];
		if (b < 1024ull)
		{
			std::snprintf(buf, sizeof(buf), "%llu B", static_cast<unsigned long long>(b));
		}
		else if (b < 1024ull * 1024ull)
		{
			std::snprintf(buf, sizeof(buf), "%.1f KiB", static_cast<double>(b) / 1024.0);
		}
		else if (b < 1024ull * 1024ull * 1024ull)
		{
			std::snprintf(buf, sizeof(buf), "%.2f MiB", static_cast<double>(b) / (1024.0 * 1024.0));
		}
		else
		{
			std::snprintf(buf, sizeof(buf), "%.2f GiB", static_cast<double>(b) / (1024.0 * 1024.0 * 1024.0));
		}
		return buf;
	}

	std::string metricValue(MetricKind kind, std::int64_t v)
	{
		if (kind == MetricKind::Bytes && v >= 0)
		{
			return bytes(static_cast<std::uint64_t>(v));
		}
		return std::to_string(v);
	}

	std::string wallTime(std::uint64_t ms)
	{
		const std::time_t t = static_cast<std::time_t>(ms / 1000);
		std::tm tmv {};
#if defined(_WIN32)
		localtime_s(&tmv, &t);
#else
		localtime_r(&t, &tmv);
#endif
		char buf[32];
		std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tmv);
		return buf;
	}

	ProcessMemory defaultProcessMemory()
	{
		return queryProcessMemory();
	}

	std::vector<std::string> splitWords(std::string_view text)
	{
		std::vector<std::string> words;
		std::size_t i = 0;
		while (i < text.size())
		{
			while (i < text.size() && (text[i] == ' ' || text[i] == '\t'))
			{
				++i;
			}
			const std::size_t start = i;
			while (i < text.size() && text[i] != ' ' && text[i] != '\t')
			{
				++i;
			}
			if (i > start)
			{
				words.emplace_back(text.substr(start, i - start));
			}
		}
		return words;
	}

	bool parseSize(const std::string& s, std::size_t& out)
	{
		if (s.empty() || s.size() > 6)
		{
			return false;
		}
		std::size_t v = 0;
		for (char c : s)
		{
			if (c < '0' || c > '9')
			{
				return false;
			}
			v = v * 10 + static_cast<std::size_t>(c - '0');
		}
		out = v;
		return true;
	}

	constexpr Nanos kSecond = 1000000000ull;

} // namespace

Engine::Engine(ClockFn clock)
	: clock_(clock ? clock : &steadyNowNs)
	, logger_(clock_)
	, profiler_(clock_)
	, tracker_(clock_)
	, processMemory_(&defaultProcessMemory)
	, startedAt_(clock_())
	, startedWallMs_(wallClockMs())
{
	tickZone_ = profiler_.zone("server.tick_interval", ZoneKind::Interval);
	snapshotZone_ = profiler_.zone("memguard.snapshot", ZoneKind::Internal);
	reportZone_ = profiler_.zone("memguard.report", ZoneKind::Internal);
	lastSnapshotAt_ = startedAt_;
	lastReportAt_ = startedAt_;
	applyConfig(config_);
}

void Engine::applyConfig(const Config& config)
{
	config_ = config;
	logger_.setLevel(config_.logLevel);
	logger_.setCooldownSeconds(config_.warnCooldownSec);
	profiler_.setEnabled(config_.enabled);
	profiler_.setSlowThreshold(static_cast<Nanos>(config_.slowNativeUs) * 1000ull);
	tracker_.setEnabled(config_.enabled);
	tracker_.setRecentFreeCapacity(config_.recentFreeCapacity);
	history_.setCapacity(config_.historySize);
}

bool Engine::setConfigValue(std::string_view key, std::string_view value, std::string& error)
{
	Config copy = config_;
	if (!applyConfigValue(copy, key, value, error))
	{
		return false;
	}
	applyConfig(copy);
	return true;
}

void Engine::addMetricProvider(std::string name, MetricProvider provider)
{
	providers_.emplace_back(std::move(name), std::move(provider));
}

void Engine::onTick()
{
	const Nanos now = clock_();
	if (haveLastTick_)
	{
		profiler_.record(tickZone_, now - lastTick_);
	}
	lastTick_ = now;
	haveLastTick_ = true;

	// The server tick runs between script executions, so any profiler frame still open here
	// belongs to Pawn code that never reached its end (runtime error, `exit`, or a section that
	// was begun but not ended in the same callback). Discard it rather than inventing a time.
	if (const std::size_t open = profiler_.openFrames())
	{
		profiler_.discardOpenFrames();
		logger_.logLimited(LogLevel::Debug, "abandoned-frames",
			"[MemGuardX] Discarded " + std::to_string(open) + " profiler frame(s) left open by Pawn code (aborted callback or unbalanced section).");
	}

	if (!config_.enabled)
	{
		return;
	}
	if (config_.snapshotIntervalSec != 0 && now - lastSnapshotAt_ >= config_.snapshotIntervalSec * kSecond)
	{
		takeSnapshot();
	}
	if (config_.reportIntervalSec != 0 && now - lastReportAt_ >= config_.reportIntervalSec * kSecond)
	{
		lastReportAt_ = now;
		std::string error;
		if (!writeReport(error))
		{
			logger_.logLimited(LogLevel::Error, "report-write", "[MemGuardX] Failed to write report: " + error);
		}
		if (config_.reportToConsole)
		{
			const std::string text = buildReport();
			std::size_t pos = 0;
			while (pos < text.size())
			{
				std::size_t nl = text.find('\n', pos);
				if (nl == std::string::npos)
				{
					nl = text.size();
				}
				logger_.log(LogLevel::Info, text.substr(pos, nl - pos));
				pos = nl + 1;
			}
		}
	}
}

const Snapshot& Engine::takeSnapshot()
{
	ScopedZone zone(profiler_, snapshotZone_);
	Snapshot snap;
	snap.timeMs = clock_() / 1000000ull;
	snap.wallMs = wallClockMs();

	const TrackerSnapshot ts = tracker_.snapshot();
	for (const CategoryStats& c : ts.categories)
	{
		snap.metrics.push_back(Metric { "tracked." + c.name + ".active", static_cast<std::int64_t>(c.active), MetricKind::Count });
		if (c.sizeKnown)
		{
			snap.metrics.push_back(Metric { "tracked." + c.name + ".bytes", static_cast<std::int64_t>(c.activeBytes), MetricKind::Bytes });
		}
	}
	if (processMemory_)
	{
		const ProcessMemory pm = processMemory_();
		lastProcessMemory_ = pm;
		if (pm.valid)
		{
			snap.metrics.push_back(Metric { "process.resident_bytes", static_cast<std::int64_t>(pm.residentBytes), MetricKind::Bytes });
			snap.metrics.push_back(Metric { "process.private_bytes", static_cast<std::int64_t>(pm.privateBytes), MetricKind::Bytes });
		}
		if (pm.heapValid)
		{
			snap.metrics.push_back(Metric { "process.heap_in_use_bytes", static_cast<std::int64_t>(pm.heapInUseBytes), MetricKind::Bytes });
			snap.metrics.push_back(Metric { "process.heap_free_bytes", static_cast<std::int64_t>(pm.heapFreeBytes), MetricKind::Bytes });
		}
	}
	for (auto& p : providers_)
	{
		p.second(snap.metrics);
	}

	lastSnapshotAt_ = clock_();
	++snapshotsTaken_;
	history_.push(std::move(snap));
	const Snapshot& stored = history_.snapshots().back();
	if (!phaseStart_)
	{
		phaseStart_ = stored;
	}
	if (!warmupEnd_ && stored.timeMs - phaseStart_->timeMs >= static_cast<std::uint64_t>(config_.warmupSec) * 1000ull)
	{
		warmupEnd_ = stored;
		logger_.logf(LogLevel::Info, "[MemGuardX] Warm-up after %s finished; memory growth from now on is checked for leaks.", phaseLabel_.c_str());
	}

	GrowthPolicy policy;
	policy.window = config_.growthWindow;
	policy.minPercent = static_cast<double>(config_.growthMinPercent);
	policy.minDeltaCount = static_cast<std::int64_t>(config_.growthMinCount);
	policy.minDeltaBytes = static_cast<std::int64_t>(config_.growthMinMB) * 1024 * 1024;
	// Only snapshots after the warm-up period count: startup growth is expected.
	lastFindings_.clear();
	if (warmupEnd_)
	{
		lastFindings_ = detectGrowthSince(history_, warmupEnd_->timeMs, policy);
	}
	for (const GrowthFinding& f : lastFindings_)
	{
		std::string msg = "[MemGuardX] Possible leak (sustained growth after warm-up): '" + f.metric + "' grew from " + metricValue(f.kind, f.first) + " to " + metricValue(f.kind, f.last) + " over the last " + std::to_string(f.steps + 1) + " snapshots";
		char extra[96];
		std::snprintf(extra, sizeof(extra), " (%.1f per minute, increased in %u/%u steps)", f.perMinute, static_cast<unsigned>(f.increasingSteps), static_cast<unsigned>(f.steps));
		msg += extra;
		logger_.logLimited(LogLevel::Warning, "growth:" + f.metric, msg);
	}
	return history_.snapshots().back();
}

void Engine::markPhase(std::string label)
{
	phaseLabel_ = std::move(label);
	phaseStart_.reset();
	warmupEnd_.reset();
	lastFindings_.clear();
	phaseStartedAt_ = clock_();
	logger_.logf(LogLevel::Info, "[MemGuardX] New memory phase: %s (warm-up %u s).", phaseLabel_.c_str(), static_cast<unsigned>(config_.warmupSec));
}

std::optional<MemoryAssessment> Engine::assessMemoryNow() const
{
	if (!phaseStart_ || history_.empty())
	{
		return std::nullopt;
	}
	GrowthPolicy policy;
	policy.window = config_.growthWindow;
	policy.minPercent = static_cast<double>(config_.growthMinPercent);
	policy.minDeltaCount = static_cast<std::int64_t>(config_.growthMinCount);
	policy.minDeltaBytes = static_cast<std::int64_t>(config_.growthMinMB) * 1024 * 1024;
	return assessMemory(history_, *phaseStart_, warmupEnd_ ? &*warmupEnd_ : nullptr, static_cast<std::uint64_t>(config_.warmupSec) * 1000ull, policy);
}

std::string Engine::formatMemoryAssessment() const
{
	std::string out;
	const auto a = assessMemoryNow();
	appendf(out, "Memory assessment (phase: %s, warm-up %u s)\n", phaseLabel_.c_str(), static_cast<unsigned>(config_.warmupSec));
	if (!a)
	{
		appendf(out, "  no snapshot in this phase yet\n");
		return out;
	}
	appendf(out, "  %-38s %14s %14s %14s %12s %12s  %s\n", "metric", "phase start", "warm-up end", "now", "startup", "since w-up", "trend");
	for (const MetricAssessment& m : a->metrics)
	{
		auto signedValue = [&](std::int64_t v) {
			return std::string(v > 0 ? "+" : (v < 0 ? "-" : "")) + metricValue(m.kind, v < 0 ? -v : v);
		};
		appendf(out, "  %-38s %14s %14s %14s %12s %12s  %s\n", m.metric.c_str(), metricValue(m.kind, m.atPhaseStart).c_str(),
			m.atWarmupEnd ? metricValue(m.kind, *m.atWarmupEnd).c_str() : "-", metricValue(m.kind, m.now).c_str(),
			signedValue(m.startupDelta).c_str(), m.atWarmupEnd ? signedValue(m.sinceWarmupDelta).c_str() : "-", trendClassName(m.trend));
	}
	appendf(out, "  Interpretation:\n");
	for (const std::string& line : a->interpretation)
	{
		appendf(out, "  - %s\n", line.c_str());
	}
	return out;
}

std::string Engine::formatVisibility() const
{
	std::string out;
	appendf(out, "What MemGuard X can and cannot see\n");
	appendf(out, "  exact     : open.mp pool entities (counts only, no byte sizes), script-reported allocations\n"
				 "              (MemGuard_TrackAlloc: counts and the sizes the script reports), timers, DB handles,\n"
				 "              AMX heap/stack usage of each script\n");
	appendf(out, "  measured  : process resident/private memory%s; C heap in use / free-retained (%s).\n"
				 "              Whole-process numbers: they include every component and plugin and cannot be\n"
				 "              attributed to individual allocations.\n",
		lastProcessMemory_.valid ? "" : " (unavailable)", lastProcessMemory_.heapSource);
	appendf(out, "  not seen  : allocations made inside other components/plugins or the server, per-player\n"
				 "              objects/textdraws, memory of data structures inside the AMX beyond heap/stack usage,\n"
				 "              allocator internals. A leak there shows up only as process/heap growth.\n");
	appendf(out, "  rule      : an active allocation is never reported as leaked; only sustained growth after warm-up is\n"
				 "              flagged as a possible leak, and only double/unknown frees are confirmed tracking errors.\n");
	return out;
}

std::string Engine::formatStatus() const
{
	std::string out;
	const Nanos up = clock_() - startedAt_;
	appendf(out, "MemGuard X status\n");
	if (!hostDescription_.empty())
	{
		appendf(out, "  host: %s\n", hostDescription_.c_str());
	}
	appendf(out, "  started: %s, uptime %s\n", wallTime(startedWallMs_).c_str(), dur(up).c_str());
	appendf(out, "  recording: %s, native profiling: %s, pool tracking: %s, log level: %s\n",
		config_.enabled ? "on" : "off", config_.profileNatives ? "on" : "off", config_.trackPools ? "on" : "off",
		logLevelName(config_.logLevel));
	appendf(out, "  zones registered: %u, snapshots taken: %llu (kept %u), reports written: %llu\n",
		static_cast<unsigned>(profiler_.zoneCount()), static_cast<unsigned long long>(snapshotsTaken_),
		static_cast<unsigned>(history_.size()), static_cast<unsigned long long>(reportsWritten_));
	const ProfilerCounters pc = profiler_.counters();
	appendf(out, "  profiler frame anomalies: abandoned=%llu unmatched-end=%llu depth-overflow=%llu\n",
		static_cast<unsigned long long>(pc.abandonedFrames), static_cast<unsigned long long>(pc.unmatchedEnds),
		static_cast<unsigned long long>(pc.overflowBegins));
	if (profilerOverheadNs_)
	{
		appendf(out, "  measured profiler cost per instrumented call: ~%s\n", dur(profilerOverheadNs_).c_str());
	}
	appendf(out, "  log messages: %llu emitted, %llu rate-limited\n",
		static_cast<unsigned long long>(logger_.emittedCount()), static_cast<unsigned long long>(logger_.suppressedCount()));
	return out;
}

std::string Engine::formatProfiler(SortKey key, std::size_t limit, std::optional<ZoneKind> kind) const
{
	const char* title = "total time";
	switch (key)
	{
	case SortKey::TotalTime:
		title = "total (inclusive) time";
		break;
	case SortKey::SelfTime:
		title = "self (exclusive) time";
		break;
	case SortKey::Calls:
		title = "call count";
		break;
	case SortKey::MaxTime:
		title = "maximum single-call time";
		break;
	case SortKey::AverageTime:
		title = "average time per call";
		break;
	}
	std::string out;
	appendf(out, "Top %u %s by %s\n", static_cast<unsigned>(limit), kind ? zoneKindName(*kind) : "zones", title);
	const std::vector<ZoneStats> rows = profiler_.sorted(key, limit, kind);
	if (rows.empty())
	{
		if (kind && *kind == ZoneKind::Function)
		{
			appendf(out, "  (no samples recorded: no Pawn function is instrumented. Add MEMGUARD_FUNC(\"name\"); to functions\n"
						 "   or build the script with tools/memguard_instrument - see docs/PAWN_PROFILING.md)\n");
		}
		else if (kind && *kind == ZoneKind::Script)
		{
			appendf(out, "  (no samples recorded: no MemGuard_ProfileBegin/End section was executed)\n");
		}
		else
		{
			appendf(out, "  (no samples recorded)\n");
		}
		return out;
	}
	appendf(out, "  %-36s %-8s %12s %12s %12s %12s %12s %8s\n", "name", "kind", "calls", "total", "self", "avg", "max", "slow");
	for (const ZoneStats& z : rows)
	{
		std::string name = z.name;
		if (name.size() > 36)
		{
			name = name.substr(0, 33) + "...";
		}
		appendf(out, "  %-36s %-8s %12llu %12s %12s %12s %12s %8llu\n", name.c_str(), zoneKindName(z.kind),
			static_cast<unsigned long long>(z.calls), dur(z.totalNs).c_str(), dur(z.selfNs).c_str(),
			dur(z.averageNs()).c_str(), dur(z.maxNs).c_str(), static_cast<unsigned long long>(z.slowCalls));
	}
	return out;
}

std::string Engine::formatMemory(std::size_t outstandingLimit) const
{
	std::string out;
	const TrackerSnapshot ts = tracker_.snapshot();
	appendf(out, "Tracked allocations (only what MemGuard X observes; not the whole process)\n");
	appendf(out, "  %-24s %10s %10s %10s %12s %10s %12s %8s %8s %8s\n", "category", "allocs", "frees", "active", "active size",
		"peak", "peak size", "dup", "unknown", "double");
	for (const CategoryStats& c : ts.categories)
	{
		const std::string activeSize = c.sizeKnown ? bytes(c.activeBytes) : std::string("n/a");
		const std::string peakSize = c.sizeKnown ? bytes(c.peakBytes) : std::string("n/a");
		appendf(out, "  %-24s %10llu %10llu %10llu %12s %10llu %12s %8llu %8llu %8llu\n", c.name.c_str(),
			static_cast<unsigned long long>(c.allocs), static_cast<unsigned long long>(c.frees),
			static_cast<unsigned long long>(c.active), activeSize.c_str(), static_cast<unsigned long long>(c.peakActive),
			peakSize.c_str(), static_cast<unsigned long long>(c.duplicateAllocs),
			static_cast<unsigned long long>(c.unknownFrees), static_cast<unsigned long long>(c.doubleFrees));
	}
	appendf(out, "  total active: %llu, total active (known sizes): %s, confirmed tracking errors: %llu\n",
		static_cast<unsigned long long>(ts.totalActive), bytes(ts.totalActiveBytes).c_str(),
		static_cast<unsigned long long>(ts.totalTrackingErrors));

	const std::vector<TrackerEvent> events = tracker_.recentEvents();
	if (!events.empty())
	{
		appendf(out, "Recent confirmed tracking errors (newest last)\n");
		const std::size_t from = events.size() > 10 ? events.size() - 10 : 0;
		for (std::size_t i = from; i < events.size(); ++i)
		{
			const TrackerEvent& e = events[i];
			const char* kind = e.kind == TrackerEvent::Kind::DoubleFree ? "double free" : (e.kind == TrackerEvent::Kind::UnknownFree ? "free of unknown id" : "allocation of an id that was still active");
			appendf(out, "  %s: category '%s' id %llu (%s after start)\n", kind, tracker_.categoryName(e.category).c_str(),
				static_cast<unsigned long long>(e.id), dur(e.at - startedAt_).c_str());
		}
	}

	if (outstandingLimit > 0)
	{
		const std::vector<ActiveAllocation> oldest = tracker_.outstanding(std::nullopt, outstandingLimit);
		appendf(out, "Oldest active tracked allocations (active != leaked)\n");
		if (oldest.empty())
		{
			appendf(out, "  (none)\n");
		}
		const Nanos now = clock_();
		for (const ActiveAllocation& a : oldest)
		{
			const auto stats = tracker_.categoryStats(a.category);
			const bool sizeKnown = stats ? stats->sizeKnown : true;
			appendf(out, "  #%llu %-20s id=%-12llu size=%-10s age=%s\n", static_cast<unsigned long long>(a.sequence),
				tracker_.categoryName(a.category).c_str(), static_cast<unsigned long long>(a.id),
				sizeKnown ? bytes(a.size).c_str() : "n/a", dur(now - a.allocatedAt).c_str());
		}
	}
	return out;
}

std::string Engine::formatGrowth() const
{
	std::string out;
	appendf(out, "Growth analysis after warm-up (window %u snapshots, min +%u%%, min +%u count / +%u MiB)\n",
		static_cast<unsigned>(config_.growthWindow), static_cast<unsigned>(config_.growthMinPercent),
		static_cast<unsigned>(config_.growthMinCount), static_cast<unsigned>(config_.growthMinMB));
	if (!warmupEnd_)
	{
		appendf(out, "  warm-up (%u s after %s) not finished yet: growth is not evaluated\n", static_cast<unsigned>(config_.warmupSec), phaseLabel_.c_str());
		return out;
	}
	std::size_t post = 0;
	for (const Snapshot& snap : history_.snapshots())
	{
		post += snap.timeMs >= warmupEnd_->timeMs ? 1 : 0;
	}
	if (post < config_.growthWindow)
	{
		appendf(out, "  not enough snapshots after warm-up yet (%u of %u)\n", static_cast<unsigned>(post), static_cast<unsigned>(config_.growthWindow));
		return out;
	}
	if (lastFindings_.empty())
	{
		appendf(out, "  no sustained growth detected\n");
		return out;
	}
	for (const GrowthFinding& f : lastFindings_)
	{
		std::string pct = f.percent < 0 ? std::string("from zero") : std::to_string(static_cast<long long>(f.percent)) + "%";
		appendf(out, "  POSSIBLE LEAK %-34s %s -> %s (+%s, %s, %.1f/min, rising in %u/%u steps)\n", f.metric.c_str(),
			metricValue(f.kind, f.first).c_str(), metricValue(f.kind, f.last).c_str(), metricValue(f.kind, f.delta).c_str(),
			pct.c_str(), f.perMinute, static_cast<unsigned>(f.increasingSteps), static_cast<unsigned>(f.steps));
	}
	return out;
}

std::string Engine::formatHistory(std::size_t rows) const
{
	std::string out;
	const auto& items = history_.snapshots();
	appendf(out, "Snapshot history (%u stored)\n", static_cast<unsigned>(items.size()));
	if (items.empty())
	{
		appendf(out, "  (no snapshots yet)\n");
		return out;
	}
	const std::size_t from = items.size() > rows ? items.size() - rows : 0;
	const Snapshot& first = items[from];
	const Snapshot& last = items.back();
	appendf(out, "  comparing snapshot #%llu (%s) with #%llu (%s)\n", static_cast<unsigned long long>(first.sequence),
		wallTime(first.wallMs).c_str(), static_cast<unsigned long long>(last.sequence), wallTime(last.wallMs).c_str());
	appendf(out, "  %-40s %16s %16s %16s\n", "metric", "before", "after", "change");
	for (const MetricDelta& d : diffSnapshots(first, last))
	{
		const std::int64_t change = d.after - d.before;
		std::string changeText = (change > 0 ? "+" : (change < 0 ? "-" : "")) + metricValue(d.kind, change < 0 ? -change : change);
		appendf(out, "  %-40s %16s %16s %16s\n", d.metric.c_str(), metricValue(d.kind, d.before).c_str(),
			metricValue(d.kind, d.after).c_str(), changeText.c_str());
	}
	return out;
}

std::string Engine::buildReport() const
{
	std::string out;
	appendf(out, "==================== MemGuard X report - %s ====================\n", wallTime(wallClockMs()).c_str());
	out += formatStatus();
	out += '\n';
	out += formatProfiler(SortKey::TotalTime, config_.topN, ZoneKind::Native);
	out += '\n';
	out += formatProfiler(SortKey::Calls, config_.topN, ZoneKind::Native);
	out += '\n';
	out += formatProfiler(SortKey::MaxTime, config_.topN, ZoneKind::Native);
	out += '\n';
	out += formatProfiler(SortKey::TotalTime, config_.topN, ZoneKind::Function);
	out += '\n';
	out += formatProfiler(SortKey::SelfTime, config_.topN, ZoneKind::Function);
	out += '\n';
	out += formatProfiler(SortKey::Calls, config_.topN, ZoneKind::Function);
	out += '\n';
	out += formatProfiler(SortKey::TotalTime, config_.topN, ZoneKind::Script);
	out += '\n';
	out += formatProfiler(SortKey::TotalTime, config_.topN, ZoneKind::Interval);
	out += '\n';
	out += formatProfiler(SortKey::TotalTime, config_.topN, ZoneKind::Internal);
	out += '\n';
	out += formatMemory(config_.outstandingLimit);
	out += '\n';
	out += formatHistory(config_.growthWindow);
	out += '\n';
	out += formatGrowth();
	out += '\n';
	out += formatMemoryAssessment();
	out += '\n';
	out += formatVisibility();
	out += '\n';
	out += "Notes: 'active' tracked allocations are not leaks by themselves. 'POSSIBLE LEAK' means sustained growth\n"
		   "across snapshots and needs investigation. Only double/unknown frees and duplicate allocations are confirmed\n"
		   "tracking errors. Native times are inclusive of anything the native itself executes (e.g. CallLocalFunction).\n";
	return out;
}

bool Engine::writeReport(std::string& error)
{
	ScopedZone zone(profiler_, reportZone_);
	const std::string text = buildReport();
	std::ofstream file(config_.reportFile, config_.reportAppend ? (std::ios::out | std::ios::app) : (std::ios::out | std::ios::trunc));
	if (!file)
	{
		error = "cannot open '" + config_.reportFile + "' for writing";
		return false;
	}
	file << text << '\n';
	file.flush();
	if (!file)
	{
		error = "write to '" + config_.reportFile + "' failed";
		return false;
	}
	++reportsWritten_;
	logger_.logf(LogLevel::Debug, "[MemGuardX] Report written to %s (%u bytes)", config_.reportFile.c_str(), static_cast<unsigned>(text.size()));
	return true;
}

void Engine::noteSlowCall(ZoneId zone, Nanos duration)
{
	const std::string name = profiler_.zoneName(zone);
	logger_.logLimited(LogLevel::Warning, "slow:" + name,
		"[MemGuardX] Slow call: " + name + " took " + dur(duration) + " (threshold " + std::to_string(config_.slowNativeUs) + " us)");
}

std::string Engine::handleCommand(std::string_view args)
{
	const std::vector<std::string> w = splitWords(args);
	const std::string cmd = w.empty() ? std::string("help") : w[0];

	if (cmd == "help")
	{
		return "MemGuard X commands (prefix: memguard)\n"
			   "  status                         component status\n"
			   "  report                         write the full report file now\n"
			   "  top [total|self|calls|max|avg] [n] [native|function|script|internal|interval]\n"
			   "  mem [n]                        tracked allocations + n oldest active entries\n"
			   "  snapshot                       take a memory snapshot now\n"
			   "  growth                         growth analysis + memory phase assessment\n"
			   "  history [n]                    compare the last n snapshots\n"
			   "  reset [profiler|memory|history|all]\n"
			   "  get <key> | set <key> <value> | config\n";
	}
	if (cmd == "status")
	{
		return formatStatus();
	}
	if (cmd == "report")
	{
		std::string error;
		if (!writeReport(error))
		{
			return "Report failed: " + error + "\n";
		}
		return "Report written to " + config_.reportFile + "\n";
	}
	if (cmd == "top")
	{
		SortKey key = SortKey::TotalTime;
		std::size_t n = config_.topN;
		std::optional<ZoneKind> kind;
		for (std::size_t i = 1; i < w.size(); ++i)
		{
			const std::string& a = w[i];
			std::size_t parsed = 0;
			if (a == "total")
				key = SortKey::TotalTime;
			else if (a == "self")
				key = SortKey::SelfTime;
			else if (a == "calls")
				key = SortKey::Calls;
			else if (a == "max")
				key = SortKey::MaxTime;
			else if (a == "avg")
				key = SortKey::AverageTime;
			else if (a == "native")
				kind = ZoneKind::Native;
			else if (a == "script")
				kind = ZoneKind::Script;
			else if (a == "internal")
				kind = ZoneKind::Internal;
			else if (a == "interval")
				kind = ZoneKind::Interval;
			else if (a == "function")
				kind = ZoneKind::Function;
			else if (parseSize(a, parsed) && parsed > 0)
				n = parsed;
			else
				return "Unknown argument '" + a + "'. Try: memguard help\n";
		}
		return formatProfiler(key, n, kind);
	}
	if (cmd == "mem")
	{
		std::size_t n = config_.outstandingLimit;
		if (w.size() > 1 && !parseSize(w[1], n))
		{
			return "Usage: memguard mem [n]\n";
		}
		return formatMemory(n);
	}
	if (cmd == "snapshot")
	{
		const Snapshot& s = takeSnapshot();
		std::string out;
		appendf(out, "Snapshot #%llu captured with %u metrics\n", static_cast<unsigned long long>(s.sequence), static_cast<unsigned>(s.metrics.size()));
		for (const Metric& m : s.metrics)
		{
			appendf(out, "  %-40s %s\n", m.name.c_str(), metricValue(m.kind, m.value).c_str());
		}
		return out;
	}
	if (cmd == "growth" || cmd == "assess")
	{
		return formatGrowth() + formatMemoryAssessment();
	}
	if (cmd == "history")
	{
		std::size_t n = config_.growthWindow;
		if (w.size() > 1 && !parseSize(w[1], n))
		{
			return "Usage: memguard history [n]\n";
		}
		return formatHistory(n < 2 ? 2 : n);
	}
	if (cmd == "reset")
	{
		const std::string what = w.size() > 1 ? w[1] : std::string("profiler");
		if (what == "profiler" || what == "all")
		{
			profiler_.reset();
			haveLastTick_ = false;
		}
		if (what == "memory" || what == "all")
		{
			tracker_.reset();
		}
		if (what == "history" || what == "all")
		{
			history_.clear();
			lastFindings_.clear();
			phaseStart_.reset();
			warmupEnd_.reset();
		}
		if (what != "profiler" && what != "memory" && what != "history" && what != "all")
		{
			return "Usage: memguard reset [profiler|memory|history|all]\n";
		}
		return "Reset " + what + ".\n";
	}
	if (cmd == "config")
	{
		return "Current configuration\n" + describeConfig(config_);
	}
	if (cmd == "get")
	{
		if (w.size() < 2)
		{
			return "Usage: memguard get <key>\n";
		}
		for (const ConfigKeyInfo& info : configKeys())
		{
			if (w[1] == info.key)
			{
				return "memguard." + w[1] + " = " + configValueAsString(config_, w[1]) + "\n";
			}
		}
		return "Unknown key '" + w[1] + "'\n";
	}
	if (cmd == "set")
	{
		if (w.size() < 3)
		{
			return "Usage: memguard set <key> <value>\n";
		}
		std::string value = w[2];
		for (std::size_t i = 3; i < w.size(); ++i)
		{
			value += " " + w[i];
		}
		std::string error;
		if (!setConfigValue(w[1], value, error))
		{
			return "Rejected: " + error + "\n";
		}
		return "memguard." + w[1] + " = " + configValueAsString(config_, w[1]) + " (runtime only, not saved to config.json)\n";
	}
	return "Unknown command '" + cmd + "'. Try: memguard help\n";
}

} // namespace memguard
