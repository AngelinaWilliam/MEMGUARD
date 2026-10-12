#include "fake_clock.hpp"
#include "test_framework.hpp"

#include "../src/core/memory_tracker.hpp"

#include <atomic>
#include <thread>
#include <vector>

using namespace memguard;

TEST(tracker_alloc_and_free_updates_counts_and_bytes)
{
	MemoryTracker t(&mgtest::fakeClock);
	const CategoryId c = t.category("buffers");
	CHECK(t.onAlloc(c, 1, 100) == AllocResult::Tracked);
	CHECK(t.onAlloc(c, 2, 50) == AllocResult::Tracked);
	CHECK(t.onAlloc(c, 3, 25) == AllocResult::Tracked);
	auto s = *t.categoryStats(c);
	CHECK_EQ(s.allocs, 3ull);
	CHECK_EQ(s.active, 3ull);
	CHECK_EQ(s.activeBytes, 175ull);
	CHECK_EQ(s.peakBytes, 175ull);

	CHECK(t.onFree(c, 2) == FreeResult::Released);
	s = *t.categoryStats(c);
	CHECK_EQ(s.frees, 1ull);
	CHECK_EQ(s.active, 2ull);
	CHECK_EQ(s.activeBytes, 125ull);
	CHECK_EQ(s.peakActive, 3ull);
	CHECK_EQ(s.peakBytes, 175ull);
	CHECK_EQ(s.totalBytesAllocated, 175ull);
	CHECK(t.isActive(c, 1));
	CHECK(!t.isActive(c, 2));
}

TEST(tracker_categories_are_independent)
{
	MemoryTracker t(&mgtest::fakeClock);
	const CategoryId a = t.category("a");
	const CategoryId b = t.category("b");
	CHECK(a != b);
	CHECK_EQ(t.category("a"), a); // idempotent registration
	CHECK(t.onAlloc(a, 7, 10) == AllocResult::Tracked);
	CHECK(t.onAlloc(b, 7, 20) == AllocResult::Tracked); // same id, other category: not a duplicate
	CHECK(t.onFree(a, 7) == FreeResult::Released);
	CHECK(t.isActive(b, 7));
	CHECK_EQ(t.categoryStats(b)->activeBytes, 20ull);
}

TEST(tracker_null_pointer_semantics)
{
	MemoryTracker t(&mgtest::fakeClock);
	const CategoryId c = t.category("ptr");
	CHECK(t.onAllocPtr(c, nullptr, 64) == AllocResult::IgnoredNull);
	CHECK(t.onFreePtr(c, nullptr) == FreeResult::IgnoredNull);
	auto s = *t.categoryStats(c);
	CHECK_EQ(s.active, 0ull);
	CHECK_EQ(s.nullAllocs, 1ull);
	CHECK_EQ(s.nullFrees, 1ull);
	CHECK_EQ(s.trackingErrors(), 0ull); // free(NULL) is legal, not an error

	int object = 0;
	CHECK(t.onAllocPtr(c, &object, sizeof(object)) == AllocResult::Tracked);
	CHECK(t.onFreePtr(c, &object) == FreeResult::Released);
	CHECK_EQ(t.categoryStats(c)->active, 0ull);
}

TEST(tracker_unknown_free_is_reported_not_counted_as_release)
{
	MemoryTracker t(&mgtest::fakeClock);
	const CategoryId c = t.category("x");
	CHECK(t.onFree(c, 42) == FreeResult::UnknownFree);
	auto s = *t.categoryStats(c);
	CHECK_EQ(s.frees, 0ull);
	CHECK_EQ(s.unknownFrees, 1ull);
	CHECK_EQ(s.active, 0ull);
	CHECK_EQ(t.recentEvents().size(), static_cast<std::size_t>(1));
}

TEST(tracker_double_free_requires_evidence)
{
	MemoryTracker t(&mgtest::fakeClock);
	const CategoryId c = t.category("x");
	CHECK(t.onAlloc(c, 5, 8) == AllocResult::Tracked);
	CHECK(t.onFree(c, 5) == FreeResult::Released);
	CHECK(t.onFree(c, 5) == FreeResult::DoubleFree);
	CHECK_EQ(t.categoryStats(c)->doubleFrees, 1ull);
	// Re-allocating the id makes a following free legitimate again.
	CHECK(t.onAlloc(c, 5, 8) == AllocResult::Tracked);
	CHECK(t.onFree(c, 5) == FreeResult::Released);
	CHECK_EQ(t.categoryStats(c)->doubleFrees, 1ull);
}

TEST(tracker_double_free_history_is_bounded)
{
	MemoryTracker t(&mgtest::fakeClock, 2);
	const CategoryId c = t.category("x");
	for (std::uint64_t id = 1; id <= 3; ++id)
	{
		t.onAlloc(c, id, 1);
		t.onFree(c, id);
	}
	// id 1 fell out of the 2-entry history: without evidence it is only an unknown free.
	CHECK(t.onFree(c, 1) == FreeResult::UnknownFree);
	CHECK(t.onFree(c, 3) == FreeResult::DoubleFree);
}

TEST(tracker_duplicate_alloc_replaces_entry)
{
	MemoryTracker t(&mgtest::fakeClock);
	const CategoryId c = t.category("x");
	CHECK(t.onAlloc(c, 9, 100) == AllocResult::Tracked);
	CHECK(t.onAlloc(c, 9, 30) == AllocResult::DuplicateActive);
	auto s = *t.categoryStats(c);
	CHECK_EQ(s.active, 1ull);
	CHECK_EQ(s.activeBytes, 30ull);
	CHECK_EQ(s.duplicateAllocs, 1ull);
	CHECK(t.onFree(c, 9) == FreeResult::Released);
	CHECK_EQ(t.categoryStats(c)->activeBytes, 0ull);
}

TEST(tracker_outstanding_sorted_oldest_first_and_limited)
{
	MemoryTracker t(&mgtest::fakeClock);
	const CategoryId c = t.category("x");
	const CategoryId d = t.category("y");
	t.onAlloc(c, 30, 1);
	mgtest::advance(10);
	t.onAlloc(d, 10, 1);
	mgtest::advance(10);
	t.onAlloc(c, 20, 1);
	auto all = t.outstanding(std::nullopt, 10);
	CHECK_EQ(all.size(), static_cast<std::size_t>(3));
	CHECK_EQ(all[0].id, 30ull);
	CHECK_EQ(all[1].id, 10ull);
	CHECK_EQ(all[2].id, 20ull);
	auto onlyC = t.outstanding(c, 1);
	CHECK_EQ(onlyC.size(), static_cast<std::size_t>(1));
	CHECK_EQ(onlyC[0].id, 30ull);
}

TEST(tracker_snapshot_is_consistent)
{
	MemoryTracker t(&mgtest::fakeClock);
	const CategoryId a = t.category("a");
	const CategoryId b = t.category("b", false);
	t.onAlloc(a, 1, 10);
	t.onAlloc(a, 2, 20);
	t.onAlloc(b, 1, 0);
	t.onFree(a, 1);
	t.onFree(b, 99);
	const TrackerSnapshot s = t.snapshot();
	std::uint64_t active = 0;
	std::uint64_t bytes = 0;
	std::uint64_t errors = 0;
	for (const auto& c : s.categories)
	{
		active += c.active;
		bytes += c.activeBytes;
		errors += c.trackingErrors();
		CHECK_EQ(c.allocs - c.frees - c.duplicateAllocs, c.active); // accounting identity
	}
	CHECK_EQ(s.totalActive, active);
	CHECK_EQ(s.totalActiveBytes, bytes);
	CHECK_EQ(s.totalActive, 2ull);
	CHECK_EQ(s.totalActiveBytes, 20ull);
	CHECK_EQ(s.totalTrackingErrors, errors);
	CHECK_EQ(s.totalTrackingErrors, 1ull);
	CHECK(!s.categories[1].sizeKnown);
	CHECK_EQ(s.sequence, 3ull);
}

TEST(tracker_disabled_records_nothing)
{
	MemoryTracker t(&mgtest::fakeClock);
	const CategoryId c = t.category("x");
	t.setEnabled(false);
	CHECK(t.onAlloc(c, 1, 1) == AllocResult::Disabled);
	CHECK(t.onFree(c, 1) == FreeResult::Disabled);
	CHECK(t.onAlloc(c + 100, 1, 1) == AllocResult::Disabled);
	t.setEnabled(true);
	CHECK(t.onAlloc(c + 100, 1, 1) == AllocResult::InvalidCategory);
	CHECK_EQ(t.categoryStats(c)->allocs, 0ull);
}

TEST(tracker_reset_and_forget)
{
	MemoryTracker t(&mgtest::fakeClock);
	const CategoryId c = t.category("x");
	t.onAlloc(c, 1, 5);
	t.onAlloc(c, 2, 5);
	CHECK_EQ(t.forgetActive(c), static_cast<std::size_t>(2));
	CHECK_EQ(t.categoryStats(c)->active, 0ull);
	CHECK_EQ(t.categoryStats(c)->allocs, 2ull); // history kept
	t.reset();
	CHECK_EQ(t.categoryStats(c)->allocs, 0ull);
	CHECK_EQ(t.category("x"), c); // registration kept
}

TEST(tracker_concurrent_alloc_free_is_exact)
{
	MemoryTracker t;
	const CategoryId c = t.category("mt");
	constexpr int kThreads = 8;
	constexpr int kPerThread = 5000;
	std::vector<std::thread> threads;
	for (int th = 0; th < kThreads; ++th)
	{
		threads.emplace_back([&, th] {
			for (int i = 0; i < kPerThread; ++i)
			{
				const std::uint64_t id = static_cast<std::uint64_t>(th) * 1000000ull + static_cast<std::uint64_t>(i);
				t.onAlloc(c, id, 4);
				if (i % 2 == 0)
				{
					t.onFree(c, id);
				}
			}
		});
	}
	for (auto& th : threads)
	{
		th.join();
	}
	const auto s = *t.categoryStats(c);
	CHECK_EQ(s.allocs, static_cast<std::uint64_t>(kThreads) * kPerThread);
	CHECK_EQ(s.frees, static_cast<std::uint64_t>(kThreads) * kPerThread / 2);
	CHECK_EQ(s.active, static_cast<std::uint64_t>(kThreads) * kPerThread / 2);
	CHECK_EQ(s.activeBytes, static_cast<std::uint64_t>(kThreads) * kPerThread / 2 * 4);
	CHECK_EQ(s.trackingErrors(), 0ull);
}
