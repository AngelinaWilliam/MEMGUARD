// MemGuard X - whole-process memory figures from the operating system and the C heap.
// These numbers include everything in the server process (all components, plugins, the
// allocator's caches) and cannot be attributed to individual allocations.
#pragma once

#include <cstdint>

namespace memguard {

struct ProcessMemory
{
	bool valid = false;
	std::uint64_t residentBytes = 0; ///< Windows: WorkingSetSize. Linux: VmRSS.
	std::uint64_t privateBytes = 0; ///< Windows: PrivateUsage. Linux: VmData (data+heap+stack mappings).
	std::uint64_t peakResidentBytes = 0; ///< Windows: PeakWorkingSetSize. Linux: VmHWM.

	/// C heap statistics. Distinguish memory that is allocated and live ("in use") from memory
	/// the allocator keeps after it was freed ("free retained") - growth of the latter is
	/// allocator caching, not live data.
	bool heapValid = false;
	const char* heapSource = "unavailable"; ///< Which API produced the heap numbers.
	std::uint64_t heapInUseBytes = 0; ///< glibc: uordblks + hblkhd. Windows: HeapSummary cbAllocated.
	std::uint64_t heapFreeBytes = 0; ///< glibc: fordblks. Windows: cbCommitted - cbAllocated.
	std::uint64_t heapMappedBytes = 0; ///< glibc: hblkhd (large mmap'd blocks, part of in-use). Windows: 0.
};

ProcessMemory queryProcessMemory();

} // namespace memguard
