#include "snapshot.hpp"

#include <algorithm>
#include <map>

namespace memguard {

const Metric* Snapshot::find(const std::string& name) const
{
	for (const Metric& m : metrics)
	{
		if (m.name == name)
		{
			return &m;
		}
	}
	return nullptr;
}

SnapshotHistory::SnapshotHistory(std::size_t capacity)
	: capacity_(capacity < 2 ? 2 : capacity)
{
}

void SnapshotHistory::setCapacity(std::size_t capacity)
{
	capacity_ = capacity < 2 ? 2 : capacity;
	while (items_.size() > capacity_)
	{
		items_.pop_front();
	}
}

std::uint64_t SnapshotHistory::push(Snapshot snapshot)
{
	snapshot.sequence = nextSequence_++;
	items_.push_back(std::move(snapshot));
	while (items_.size() > capacity_)
	{
		items_.pop_front();
	}
	return items_.back().sequence;
}

void SnapshotHistory::clear()
{
	items_.clear();
}

std::vector<std::pair<std::uint64_t, std::int64_t>> SnapshotHistory::series(const std::string& metric) const
{
	std::vector<std::pair<std::uint64_t, std::int64_t>> out;
	for (const Snapshot& s : items_)
	{
		if (const Metric* m = s.find(metric))
		{
			out.emplace_back(s.timeMs, m->value);
		}
	}
	return out;
}

std::vector<GrowthFinding> detectGrowth(const SnapshotHistory& history, const GrowthPolicy& policy)
{
	std::vector<GrowthFinding> findings;
	const std::size_t window = policy.window < 2 ? 2 : policy.window;
	const auto& items = history.snapshots();
	if (items.size() < window)
	{
		return findings;
	}
	const std::size_t start = items.size() - window;

	// Collect metric values present in every snapshot of the window.
	std::map<std::string, std::pair<MetricKind, std::vector<std::int64_t>>> values;
	for (const Metric& m : items[start].metrics)
	{
		values[m.name] = { m.kind, { m.value } };
	}
	for (std::size_t i = start + 1; i < items.size(); ++i)
	{
		for (auto it = values.begin(); it != values.end();)
		{
			const Metric* m = items[i].find(it->first);
			if (!m)
			{
				it = values.erase(it);
				continue;
			}
			it->second.second.push_back(m->value);
			++it;
		}
	}

	const std::uint64_t spanMs = items.back().timeMs >= items[start].timeMs ? items.back().timeMs - items[start].timeMs : 0;
	for (const auto& kv : values)
	{
		const std::vector<std::int64_t>& v = kv.second.second;
		const MetricKind kind = kv.second.first;
		std::size_t increasing = 0;
		for (std::size_t i = 1; i < v.size(); ++i)
		{
			if (v[i] > v[i - 1])
			{
				++increasing;
			}
		}
		const std::size_t steps = v.size() - 1;
		const std::int64_t first = v.front();
		const std::int64_t last = v.back();
		const std::int64_t delta = last - first;
		const std::int64_t minDelta = kind == MetricKind::Bytes ? policy.minDeltaBytes : policy.minDeltaCount;
		if (delta <= 0 || delta < minDelta)
		{
			continue;
		}
		if (static_cast<double>(increasing) < policy.minIncreasingFraction * static_cast<double>(steps))
		{
			continue;
		}
		double percent = -1.0;
		if (first > 0)
		{
			percent = 100.0 * static_cast<double>(delta) / static_cast<double>(first);
			if (percent < policy.minPercent)
			{
				continue;
			}
		}
		GrowthFinding f;
		f.metric = kv.first;
		f.kind = kind;
		f.first = first;
		f.last = last;
		f.delta = delta;
		f.percent = percent;
		f.increasingSteps = increasing;
		f.steps = steps;
		f.spanMs = spanMs;
		f.perMinute = spanMs ? static_cast<double>(delta) * 60000.0 / static_cast<double>(spanMs) : 0.0;
		findings.push_back(std::move(f));
	}
	return findings;
}

std::vector<MetricDelta> diffSnapshots(const Snapshot& before, const Snapshot& after)
{
	std::vector<MetricDelta> out;
	for (const Metric& a : after.metrics)
	{
		if (const Metric* b = before.find(a.name))
		{
			out.push_back(MetricDelta { a.name, a.kind, b->value, a.value });
		}
	}
	return out;
}

} // namespace memguard
