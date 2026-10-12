#include "memory_tracker.hpp"

#include <algorithm>

namespace memguard {

const char* allocResultName(AllocResult r)
{
	switch (r)
	{
	case AllocResult::Tracked:
		return "tracked";
	case AllocResult::DuplicateActive:
		return "duplicate-active";
	case AllocResult::IgnoredNull:
		return "ignored-null";
	case AllocResult::Disabled:
		return "disabled";
	case AllocResult::InvalidCategory:
		return "invalid-category";
	}
	return "?";
}

const char* freeResultName(FreeResult r)
{
	switch (r)
	{
	case FreeResult::Released:
		return "released";
	case FreeResult::UnknownFree:
		return "unknown-free";
	case FreeResult::DoubleFree:
		return "double-free";
	case FreeResult::IgnoredNull:
		return "ignored-null";
	case FreeResult::Disabled:
		return "disabled";
	case FreeResult::InvalidCategory:
		return "invalid-category";
	}
	return "?";
}

MemoryTracker::MemoryTracker(ClockFn clock, std::size_t recentFreeCapacity, std::size_t eventLogCapacity)
	: clock_(clock ? clock : &steadyNowNs)
	, recentCapacity_(recentFreeCapacity)
	, eventCapacity_(eventLogCapacity)
{
}

CategoryId MemoryTracker::category(std::string_view name, bool sizeKnown)
{
	std::lock_guard<std::mutex> lock(mutex_);
	std::string key(name);
	auto it = byName_.find(key);
	if (it != byName_.end())
	{
		return it->second;
	}
	const CategoryId id = static_cast<CategoryId>(categories_.size());
	CategoryStats s;
	s.id = id;
	s.name = key;
	s.sizeKnown = sizeKnown;
	categories_.push_back(std::move(s));
	byName_.emplace(std::move(key), id);
	return id;
}

std::optional<CategoryId> MemoryTracker::findCategory(std::string_view name) const
{
	std::lock_guard<std::mutex> lock(mutex_);
	auto it = byName_.find(std::string(name));
	if (it == byName_.end())
	{
		return std::nullopt;
	}
	return it->second;
}

CategoryStats& MemoryTracker::statsLocked(CategoryId cat)
{
	return categories_[cat];
}

void MemoryTracker::pushEventLocked(TrackerEvent::Kind kind, const Key& key)
{
	if (eventCapacity_ == 0)
	{
		return;
	}
	if (events_.size() >= eventCapacity_)
	{
		events_.pop_front();
	}
	events_.push_back(TrackerEvent { kind, key.cat, key.id, clock_() });
}

void MemoryTracker::rememberFreedLocked(const Key& key)
{
	if (recentCapacity_ == 0)
	{
		return;
	}
	const std::uint64_t gen = ++recentGeneration_;
	recentFreed_[key] = gen;
	recentOrder_.emplace_back(key, gen);
	while (recentOrder_.size() > recentCapacity_)
	{
		const auto oldest = recentOrder_.front();
		recentOrder_.pop_front();
		auto it = recentFreed_.find(oldest.first);
		// Only erase if this FIFO entry is still the current record for that key.
		if (it != recentFreed_.end() && it->second == oldest.second)
		{
			recentFreed_.erase(it);
		}
	}
}

AllocResult MemoryTracker::onAlloc(CategoryId cat, std::uint64_t id, std::uint64_t size)
{
	std::lock_guard<std::mutex> lock(mutex_);
	if (!enabled_)
	{
		return AllocResult::Disabled;
	}
	if (cat >= categories_.size())
	{
		return AllocResult::InvalidCategory;
	}
	CategoryStats& s = statsLocked(cat);
	const Key key { cat, id };
	recentFreed_.erase(key); // re-allocation: a later free is legitimate again
	++s.allocs;
	s.totalBytesAllocated += size;

	auto it = active_.find(key);
	if (it != active_.end())
	{
		// We never saw the free for the previous allocation with this id. Replace the entry so
		// accounting reflects the newest allocation, and record the evidence.
		++s.duplicateAllocs;
		s.activeBytes -= it->second.size;
		s.activeBytes += size;
		it->second = Entry { size, clock_(), ++sequence_ };
		s.peakBytes = std::max(s.peakBytes, s.activeBytes);
		pushEventLocked(TrackerEvent::Kind::DuplicateAlloc, key);
		return AllocResult::DuplicateActive;
	}

	active_.emplace(key, Entry { size, clock_(), ++sequence_ });
	++s.active;
	s.activeBytes += size;
	s.peakActive = std::max(s.peakActive, s.active);
	s.peakBytes = std::max(s.peakBytes, s.activeBytes);
	return AllocResult::Tracked;
}

FreeResult MemoryTracker::onFree(CategoryId cat, std::uint64_t id)
{
	std::lock_guard<std::mutex> lock(mutex_);
	if (!enabled_)
	{
		return FreeResult::Disabled;
	}
	if (cat >= categories_.size())
	{
		return FreeResult::InvalidCategory;
	}
	CategoryStats& s = statsLocked(cat);
	const Key key { cat, id };
	auto it = active_.find(key);
	if (it != active_.end())
	{
		++s.frees;
		--s.active;
		s.activeBytes -= it->second.size;
		active_.erase(it);
		rememberFreedLocked(key);
		return FreeResult::Released;
	}
	if (recentFreed_.find(key) != recentFreed_.end())
	{
		++s.doubleFrees;
		pushEventLocked(TrackerEvent::Kind::DoubleFree, key);
		return FreeResult::DoubleFree;
	}
	++s.unknownFrees;
	pushEventLocked(TrackerEvent::Kind::UnknownFree, key);
	return FreeResult::UnknownFree;
}

AllocResult MemoryTracker::onAllocPtr(CategoryId cat, const void* ptr, std::uint64_t size)
{
	if (!ptr)
	{
		std::lock_guard<std::mutex> lock(mutex_);
		if (!enabled_)
		{
			return AllocResult::Disabled;
		}
		if (cat >= categories_.size())
		{
			return AllocResult::InvalidCategory;
		}
		++statsLocked(cat).nullAllocs;
		return AllocResult::IgnoredNull;
	}
	return onAlloc(cat, static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(ptr)), size);
}

FreeResult MemoryTracker::onFreePtr(CategoryId cat, const void* ptr)
{
	if (!ptr)
	{
		std::lock_guard<std::mutex> lock(mutex_);
		if (!enabled_)
		{
			return FreeResult::Disabled;
		}
		if (cat >= categories_.size())
		{
			return FreeResult::InvalidCategory;
		}
		++statsLocked(cat).nullFrees;
		return FreeResult::IgnoredNull;
	}
	return onFree(cat, static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(ptr)));
}

bool MemoryTracker::isActive(CategoryId cat, std::uint64_t id) const
{
	std::lock_guard<std::mutex> lock(mutex_);
	return active_.find(Key { cat, id }) != active_.end();
}

std::vector<CategoryStats> MemoryTracker::categoryStats() const
{
	std::lock_guard<std::mutex> lock(mutex_);
	return categories_;
}

std::optional<CategoryStats> MemoryTracker::categoryStats(CategoryId cat) const
{
	std::lock_guard<std::mutex> lock(mutex_);
	if (cat >= categories_.size())
	{
		return std::nullopt;
	}
	return categories_[cat];
}

std::vector<ActiveAllocation> MemoryTracker::outstanding(std::optional<CategoryId> cat, std::size_t limit) const
{
	std::vector<ActiveAllocation> out;
	{
		std::lock_guard<std::mutex> lock(mutex_);
		for (const auto& kv : active_)
		{
			if (cat && kv.first.cat != *cat)
			{
				continue;
			}
			out.push_back(ActiveAllocation { kv.first.cat, kv.first.id, kv.second.size, kv.second.at, kv.second.sequence });
		}
	}
	std::sort(out.begin(), out.end(), [](const ActiveAllocation& a, const ActiveAllocation& b) {
		return a.sequence < b.sequence;
	});
	if (out.size() > limit)
	{
		out.resize(limit);
	}
	return out;
}

TrackerSnapshot MemoryTracker::snapshot() const
{
	TrackerSnapshot snap;
	std::lock_guard<std::mutex> lock(mutex_);
	snap.takenAt = clock_();
	snap.sequence = sequence_;
	snap.categories = categories_;
	for (const CategoryStats& s : categories_)
	{
		snap.totalActive += s.active;
		snap.totalActiveBytes += s.activeBytes;
		snap.totalTrackingErrors += s.trackingErrors();
	}
	return snap;
}

std::vector<TrackerEvent> MemoryTracker::recentEvents() const
{
	std::lock_guard<std::mutex> lock(mutex_);
	return std::vector<TrackerEvent>(events_.begin(), events_.end());
}

std::string MemoryTracker::categoryName(CategoryId cat) const
{
	std::lock_guard<std::mutex> lock(mutex_);
	return cat < categories_.size() ? categories_[cat].name : std::string("?");
}

void MemoryTracker::setEnabled(bool enabled)
{
	std::lock_guard<std::mutex> lock(mutex_);
	enabled_ = enabled;
}

bool MemoryTracker::enabled() const
{
	std::lock_guard<std::mutex> lock(mutex_);
	return enabled_;
}

void MemoryTracker::reset()
{
	std::lock_guard<std::mutex> lock(mutex_);
	active_.clear();
	recentFreed_.clear();
	recentOrder_.clear();
	events_.clear();
	for (CategoryStats& s : categories_)
	{
		CategoryStats fresh;
		fresh.id = s.id;
		fresh.name = s.name;
		fresh.sizeKnown = s.sizeKnown;
		s = fresh;
	}
}

std::size_t MemoryTracker::forgetActive(CategoryId cat)
{
	std::lock_guard<std::mutex> lock(mutex_);
	if (cat >= categories_.size())
	{
		return 0;
	}
	std::size_t dropped = 0;
	for (auto it = active_.begin(); it != active_.end();)
	{
		if (it->first.cat == cat)
		{
			it = active_.erase(it);
			++dropped;
		}
		else
		{
			++it;
		}
	}
	categories_[cat].active = 0;
	categories_[cat].activeBytes = 0;
	return dropped;
}

void MemoryTracker::setRecentFreeCapacity(std::size_t capacity)
{
	std::lock_guard<std::mutex> lock(mutex_);
	recentCapacity_ = capacity;
	while (recentOrder_.size() > recentCapacity_)
	{
		const auto oldest = recentOrder_.front();
		recentOrder_.pop_front();
		auto it = recentFreed_.find(oldest.first);
		if (it != recentFreed_.end() && it->second == oldest.second)
		{
			recentFreed_.erase(it);
		}
	}
}

} // namespace memguard
