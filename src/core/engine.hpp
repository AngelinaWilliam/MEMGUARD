// MemGuard X - diagnostic engine.
// Owns the profiler, memory tracker, snapshot history, logger and configuration, schedules
// periodic work, produces reports and interprets console commands. It has no dependency on
// the open.mp SDK; the component adapter feeds it events and metric providers.
#pragma once

#include "config.hpp"
#include "logger.hpp"
#include "memory_analysis.hpp"
#include "process_memory.hpp"
#include "memory_tracker.hpp"
#include "profiler.hpp"
#include "snapshot.hpp"

#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace memguard {

class Engine
{
public:
	using MetricProvider = std::function<void(std::vector<Metric>&)>;
	using ProcessMemoryFn = ProcessMemory (*)();

	explicit Engine(ClockFn clock = &steadyNowNs);

	Profiler& profiler() { return profiler_; }
	MemoryTracker& tracker() { return tracker_; }
	SnapshotHistory& history() { return history_; }
	Logger& logger() { return logger_; }
	const Config& config() const { return config_; }

	/// Replaces the configuration and propagates it to every module.
	void applyConfig(const Config& config);

	/// Sets one key at runtime (same validation as config.json). Returns false on error.
	bool setConfigValue(std::string_view key, std::string_view value, std::string& error);

	void setHostDescription(std::string text) { hostDescription_ = std::move(text); }
	void addMetricProvider(std::string name, MetricProvider provider);

	/// Overrides the process-memory source (tests use a fake; default queries the OS).
	void setProcessMemorySource(ProcessMemoryFn fn) { processMemory_ = fn; }

	/// Called once per server tick. Records the tick interval and runs due snapshots/reports.
	void onTick();

	/// Captures a snapshot now, stores it and runs growth detection.
	const Snapshot& takeSnapshot();
	const std::vector<GrowthFinding>& lastGrowthFindings() const { return lastFindings_; }

	/// Starts a new memory phase (server start is the first). Growth during the following
	/// warm-up period is reported as startup. Called on gamemode restart.
	void markPhase(std::string label);
	const std::string& phaseLabel() const { return phaseLabel_; }

	/// Phase/warm-up aware classification of every snapshot metric (needs >= 1 snapshot).
	std::optional<MemoryAssessment> assessMemoryNow() const;

	/// Full text report.
	std::string buildReport() const;
	/// Writes buildReport() to config().reportFile. Returns false and sets `error` on failure.
	bool writeReport(std::string& error);
	std::uint64_t reportsWritten() const { return reportsWritten_; }

	/// Executes a console command line (everything after "memguard"). Returns the reply text.
	std::string handleCommand(std::string_view args);

	/// Called by the native-call wrapper when a call exceeded the slow threshold.
	void noteSlowCall(ZoneId zone, Nanos duration);

	// Sections used by buildReport() and by commands.
	std::string formatStatus() const;
	std::string formatProfiler(SortKey key, std::size_t limit, std::optional<ZoneKind> kind) const;
	std::string formatMemory(std::size_t outstandingLimit) const;
	std::string formatGrowth() const;
	std::string formatHistory(std::size_t rows) const;
	std::string formatMemoryAssessment() const;
	std::string formatVisibility() const;

	ZoneId tickZone() const { return tickZone_; }
	void setMeasuredProfilerOverhead(Nanos ns) { profilerOverheadNs_ = ns; }

private:
	ClockFn clock_;
	Config config_;
	Logger logger_;
	Profiler profiler_;
	MemoryTracker tracker_;
	SnapshotHistory history_;
	std::vector<std::pair<std::string, MetricProvider>> providers_;
	ProcessMemoryFn processMemory_;
	std::vector<GrowthFinding> lastFindings_;
	std::optional<Snapshot> phaseStart_;
	std::optional<Snapshot> warmupEnd_;
	std::string phaseLabel_ = "server start";
	Nanos phaseStartedAt_ = 0;
	ProcessMemory lastProcessMemory_;
	std::string hostDescription_;

	Nanos startedAt_;
	std::uint64_t startedWallMs_;
	Nanos lastTick_ = 0;
	bool haveLastTick_ = false;
	Nanos lastSnapshotAt_ = 0;
	Nanos lastReportAt_ = 0;
	std::uint64_t reportsWritten_ = 0;
	std::uint64_t snapshotsTaken_ = 0;
	Nanos profilerOverheadNs_ = 0;

	ZoneId tickZone_;
	ZoneId snapshotZone_;
	ZoneId reportZone_;
};

} // namespace memguard
