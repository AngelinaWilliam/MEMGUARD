#include "profiler.hpp"

#include <algorithm>

namespace memguard {

const char* zoneKindName(ZoneKind kind)
{
	switch (kind)
	{
	case ZoneKind::Native:
		return "native";
	case ZoneKind::Script:
		return "script";
	case ZoneKind::Internal:
		return "internal";
	case ZoneKind::Interval:
		return "interval";
	case ZoneKind::Function:
		return "function";
	}
	return "?";
}

namespace {

	// Instance ids are never reused, so a per-thread stack that belonged to a destroyed
	// profiler is simply never looked up again (see Profiler::frames()).
	std::atomic<std::uint64_t> g_nextInstanceId { 1 };

	void atomicMax(std::atomic<std::uint64_t>& target, std::uint64_t value)
	{
		std::uint64_t cur = target.load(std::memory_order_relaxed);
		while (value > cur && !target.compare_exchange_weak(cur, value, std::memory_order_relaxed))
		{
		}
	}

	void atomicMin(std::atomic<std::uint64_t>& target, std::uint64_t value)
	{
		std::uint64_t cur = target.load(std::memory_order_relaxed);
		while (value < cur && !target.compare_exchange_weak(cur, value, std::memory_order_relaxed))
		{
		}
	}

} // namespace

Profiler::Profiler(ClockFn clock)
	: clock_(clock ? clock : &steadyNowNs)
	, instanceId_(g_nextInstanceId.fetch_add(1, std::memory_order_relaxed))
{
	for (auto& c : chunks_)
	{
		c.store(nullptr, std::memory_order_relaxed);
	}
}

Profiler::~Profiler()
{
	for (auto& c : chunks_)
	{
		delete[] c.load(std::memory_order_relaxed);
	}
}

Profiler::Slot* Profiler::slot(ZoneId id) const
{
	if (id >= count_.load(std::memory_order_acquire))
	{
		return nullptr;
	}
	Slot* chunk = chunks_[id / kChunkSize].load(std::memory_order_acquire);
	return chunk ? &chunk[id % kChunkSize] : nullptr;
}

ZoneId Profiler::zone(std::string_view name, ZoneKind kind)
{
	std::lock_guard<std::mutex> lock(registryMutex_);
	std::string key(name);
	auto it = byName_.find(key);
	if (it != byName_.end())
	{
		return it->second;
	}
	const std::uint32_t id = count_.load(std::memory_order_relaxed);
	const std::size_t chunkIndex = id / kChunkSize;
	if (chunkIndex >= kMaxChunks)
	{
		return kInvalidZone;
	}
	Slot* chunk = chunks_[chunkIndex].load(std::memory_order_relaxed);
	if (!chunk)
	{
		chunk = new Slot[kChunkSize];
		chunks_[chunkIndex].store(chunk, std::memory_order_release);
	}
	Slot& s = chunk[id % kChunkSize];
	s.name = key;
	s.kind = kind;
	byName_.emplace(std::move(key), id);
	// Publishing the new count makes the fully initialised slot visible to readers.
	count_.store(id + 1, std::memory_order_release);
	return id;
}

std::optional<ZoneId> Profiler::findZone(std::string_view name) const
{
	std::lock_guard<std::mutex> lock(registryMutex_);
	auto it = byName_.find(std::string(name));
	if (it == byName_.end())
	{
		return std::nullopt;
	}
	return it->second;
}

std::size_t Profiler::zoneCount() const
{
	return count_.load(std::memory_order_acquire);
}

std::string Profiler::zoneName(ZoneId id) const
{
	const Slot* s = slot(id);
	return s ? s->name : std::string();
}

std::vector<Profiler::Frame>& Profiler::frames() const
{
	// One small vector of (instance id, stack) per thread; the last lookup is cached.
	struct Entry
	{
		std::uint64_t id;
		std::unique_ptr<std::vector<Frame>> stack;
	};
	thread_local std::vector<Entry> entries;
	thread_local std::uint64_t cachedId = 0;
	thread_local std::vector<Frame>* cached = nullptr;

	if (cachedId == instanceId_ && cached)
	{
		return *cached;
	}
	for (Entry& e : entries)
	{
		if (e.id == instanceId_)
		{
			cachedId = instanceId_;
			cached = e.stack.get();
			return *cached;
		}
	}
	entries.push_back(Entry { instanceId_, std::make_unique<std::vector<Frame>>() });
	entries.back().stack->reserve(32);
	cachedId = instanceId_;
	cached = entries.back().stack.get();
	return *cached;
}

std::size_t Profiler::begin(ZoneId id)
{
	if (!enabled() || id >= count_.load(std::memory_order_acquire))
	{
		return kNoFrame;
	}
	std::vector<Frame>& stack = frames();
	if (stack.size() >= kMaxDepth)
	{
		overflow_.fetch_add(1, std::memory_order_relaxed);
		return kNoFrame;
	}
	const std::size_t token = stack.size();
	stack.push_back(Frame { id, clock_(), 0, 0 });
	return token;
}

std::uint32_t Profiler::beginSerial(ZoneId id)
{
	const std::size_t token = begin(id);
	if (token == kNoFrame)
	{
		return 0;
	}
	// Serials are kept within 31 bits so they stay positive when handed to Pawn as a cell.
	std::uint32_t serial = nextSerial_.fetch_add(1, std::memory_order_relaxed) & 0x7FFFFFFFu;
	if (serial == 0)
	{
		serial = nextSerial_.fetch_add(1, std::memory_order_relaxed) & 0x7FFFFFFFu; // skip 0 on wrap-around
	}
	frames()[token].serial = serial;
	return serial;
}

bool Profiler::endSerial(std::uint32_t serial)
{
	if (serial == 0)
	{
		return false;
	}
	std::vector<Frame>& stack = frames();
	for (std::size_t i = stack.size(); i > 0; --i)
	{
		if (stack[i - 1].serial == serial)
		{
			end(i - 1);
			return true;
		}
	}
	unmatched_.fetch_add(1, std::memory_order_relaxed);
	return false;
}

void Profiler::commit(Slot& s, Nanos total, Nanos self)
{
	s.calls.fetch_add(1, std::memory_order_relaxed);
	s.totalNs.fetch_add(total, std::memory_order_relaxed);
	s.selfNs.fetch_add(self, std::memory_order_relaxed);
	atomicMax(s.maxNs, total);
	atomicMin(s.minNs, total);
	const Nanos slow = slowThresholdNs_.load(std::memory_order_relaxed);
	if (slow != 0 && total > slow && s.kind != ZoneKind::Interval)
	{
		s.slowCalls.fetch_add(1, std::memory_order_relaxed);
	}
}

Nanos Profiler::end(std::size_t token)
{
	if (token == kNoFrame)
	{
		return 0;
	}
	const Nanos now = clock_();
	std::vector<Frame>& stack = frames();
	if (token >= stack.size())
	{
		unmatched_.fetch_add(1, std::memory_order_relaxed);
		return 0;
	}
	while (stack.size() > token + 1)
	{
		stack.pop_back();
		abandoned_.fetch_add(1, std::memory_order_relaxed);
	}
	const Frame frame = stack.back();
	stack.pop_back();
	const Nanos total = now >= frame.start ? now - frame.start : 0;
	const Nanos self = total >= frame.child ? total - frame.child : 0;
	if (Slot* s = slot(frame.id))
	{
		commit(*s, total, self);
	}
	if (!stack.empty())
	{
		stack.back().child += total;
	}
	return total;
}

bool Profiler::endZone(ZoneId id)
{
	std::vector<Frame>& stack = frames();
	for (std::size_t i = stack.size(); i > 0; --i)
	{
		if (stack[i - 1].id == id)
		{
			end(i - 1);
			return true;
		}
	}
	unmatched_.fetch_add(1, std::memory_order_relaxed);
	return false;
}

std::size_t Profiler::openFrames() const
{
	return frames().size();
}

void Profiler::discardOpenFrames()
{
	std::vector<Frame>& stack = frames();
	abandoned_.fetch_add(stack.size(), std::memory_order_relaxed);
	stack.clear();
}

void Profiler::record(ZoneId id, Nanos duration)
{
	if (!enabled())
	{
		return;
	}
	if (Slot* s = slot(id))
	{
		commit(*s, duration, duration);
	}
}

std::vector<ZoneStats> Profiler::stats() const
{
	std::vector<ZoneStats> out;
	const std::uint32_t n = count_.load(std::memory_order_acquire);
	out.reserve(n);
	for (std::uint32_t id = 0; id < n; ++id)
	{
		const Slot* s = slot(id);
		if (!s)
		{
			continue;
		}
		ZoneStats z;
		z.id = id;
		z.name = s->name;
		z.kind = s->kind;
		z.calls = s->calls.load(std::memory_order_relaxed);
		z.totalNs = s->totalNs.load(std::memory_order_relaxed);
		z.selfNs = s->selfNs.load(std::memory_order_relaxed);
		z.maxNs = s->maxNs.load(std::memory_order_relaxed);
		const std::uint64_t mn = s->minNs.load(std::memory_order_relaxed);
		z.minNs = (z.calls == 0 || mn == ~0ull) ? 0 : mn;
		z.slowCalls = s->slowCalls.load(std::memory_order_relaxed);
		out.push_back(std::move(z));
	}
	return out;
}

std::optional<ZoneStats> Profiler::zoneStats(ZoneId id) const
{
	const Slot* s = slot(id);
	if (!s)
	{
		return std::nullopt;
	}
	ZoneStats z;
	z.id = id;
	z.name = s->name;
	z.kind = s->kind;
	z.calls = s->calls.load(std::memory_order_relaxed);
	z.totalNs = s->totalNs.load(std::memory_order_relaxed);
	z.selfNs = s->selfNs.load(std::memory_order_relaxed);
	z.maxNs = s->maxNs.load(std::memory_order_relaxed);
	const std::uint64_t mn = s->minNs.load(std::memory_order_relaxed);
	z.minNs = (z.calls == 0 || mn == ~0ull) ? 0 : mn;
	z.slowCalls = s->slowCalls.load(std::memory_order_relaxed);
	return z;
}

std::vector<ZoneStats> Profiler::sorted(SortKey key, std::size_t limit, std::optional<ZoneKind> kind) const
{
	std::vector<ZoneStats> all = stats();
	std::vector<ZoneStats> filtered;
	filtered.reserve(all.size());
	for (ZoneStats& z : all)
	{
		if (z.calls == 0)
		{
			continue;
		}
		if (kind && z.kind != *kind)
		{
			continue;
		}
		filtered.push_back(std::move(z));
	}
	auto metric = [key](const ZoneStats& z) -> std::uint64_t {
		switch (key)
		{
		case SortKey::TotalTime:
			return z.totalNs;
		case SortKey::SelfTime:
			return z.selfNs;
		case SortKey::Calls:
			return z.calls;
		case SortKey::MaxTime:
			return z.maxNs;
		case SortKey::AverageTime:
			return z.averageNs();
		}
		return 0;
	};
	std::stable_sort(filtered.begin(), filtered.end(), [&](const ZoneStats& a, const ZoneStats& b) {
		const std::uint64_t ma = metric(a);
		const std::uint64_t mb = metric(b);
		if (ma != mb)
		{
			return ma > mb;
		}
		return a.name < b.name;
	});
	if (filtered.size() > limit)
	{
		filtered.resize(limit);
	}
	return filtered;
}

ProfilerCounters Profiler::counters() const
{
	ProfilerCounters c;
	c.abandonedFrames = abandoned_.load(std::memory_order_relaxed);
	c.unmatchedEnds = unmatched_.load(std::memory_order_relaxed);
	c.overflowBegins = overflow_.load(std::memory_order_relaxed);
	return c;
}

void Profiler::reset()
{
	const std::uint32_t n = count_.load(std::memory_order_acquire);
	for (std::uint32_t id = 0; id < n; ++id)
	{
		if (Slot* s = slot(id))
		{
			s->calls.store(0, std::memory_order_relaxed);
			s->totalNs.store(0, std::memory_order_relaxed);
			s->selfNs.store(0, std::memory_order_relaxed);
			s->maxNs.store(0, std::memory_order_relaxed);
			s->minNs.store(~0ull, std::memory_order_relaxed);
			s->slowCalls.store(0, std::memory_order_relaxed);
		}
	}
	abandoned_.store(0, std::memory_order_relaxed);
	unmatched_.store(0, std::memory_order_relaxed);
	overflow_.store(0, std::memory_order_relaxed);
}

Nanos Profiler::measureOverheadNs()
{
	Profiler p;
	const ZoneId id = p.zone("overhead-probe", ZoneKind::Internal);
	constexpr int kBatch = 2000;
	constexpr int kRuns = 7;
	Nanos results[kRuns];
	for (int r = 0; r < kRuns; ++r)
	{
		const Nanos t0 = steadyNowNs();
		for (int i = 0; i < kBatch; ++i)
		{
			p.end(p.begin(id));
		}
		const Nanos t1 = steadyNowNs();
		results[r] = (t1 - t0) / kBatch;
	}
	std::sort(results, results + kRuns);
	return results[kRuns / 2];
}

} // namespace memguard
