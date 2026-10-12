#include "process_memory.hpp"

#include <cstddef>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <psapi.h>
#elif defined(__linux__)
#include <cstdio>
#include <cstring>
#include <dlfcn.h>
#endif

namespace memguard {

namespace {

#if defined(_WIN32)
	// Layout of HEAP_SUMMARY (heapapi.h). Resolved at run time so the component still loads on
	// systems where kernel32 does not export HeapSummary.
	struct MgHeapSummary
	{
		DWORD cb;
		SIZE_T cbAllocated;
		SIZE_T cbCommitted;
		SIZE_T cbReserved;
		SIZE_T cbMaxReserve;
	};
	using HeapSummaryFn = BOOL(WINAPI*)(HANDLE, DWORD, MgHeapSummary*);

	void queryHeap(ProcessMemory& pm)
	{
		static HeapSummaryFn fn = reinterpret_cast<HeapSummaryFn>(
			reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "HeapSummary")));
		if (!fn)
		{
			return;
		}
		MgHeapSummary s {};
		s.cb = sizeof(s);
		if (fn(GetProcessHeap(), 0, &s))
		{
			pm.heapValid = true;
			pm.heapSource = "HeapSummary(process heap)";
			pm.heapInUseBytes = s.cbAllocated;
			pm.heapFreeBytes = s.cbCommitted > s.cbAllocated ? s.cbCommitted - s.cbAllocated : 0;
			pm.heapMappedBytes = 0;
		}
	}
#elif defined(__linux__)
	// Same layout as glibc's struct mallinfo2 (size_t fields) and struct mallinfo (int fields).
	// Looked up with dlsym so the component neither requires glibc >= 2.33 (mallinfo2) nor
	// fails on a C library without mallinfo.
	template <typename T>
	struct MallinfoT
	{
		T arena, ordblks, smblks, hblks, hblkhd, usmblks, fsmblks, uordblks, fordblks, keepcost;
	};
	using Mallinfo2Fn = MallinfoT<std::size_t> (*)();
	using MallinfoFn = MallinfoT<int> (*)();

	void queryHeap(ProcessMemory& pm)
	{
		static Mallinfo2Fn mi2 = reinterpret_cast<Mallinfo2Fn>(dlsym(RTLD_DEFAULT, "mallinfo2"));
		static MallinfoFn mi = reinterpret_cast<MallinfoFn>(dlsym(RTLD_DEFAULT, "mallinfo"));
		if (mi2)
		{
			const auto m = mi2();
			pm.heapValid = true;
			pm.heapSource = "glibc mallinfo2";
			pm.heapInUseBytes = m.uordblks + m.hblkhd;
			pm.heapFreeBytes = m.fordblks;
			pm.heapMappedBytes = m.hblkhd;
		}
		else if (mi)
		{
			const auto m = mi();
			// int fields wrap above 2 GiB; a 32-bit server cannot exceed that anyway.
			pm.heapValid = true;
			pm.heapSource = "glibc mallinfo";
			pm.heapInUseBytes = static_cast<unsigned int>(m.uordblks) + static_cast<unsigned int>(m.hblkhd);
			pm.heapFreeBytes = static_cast<unsigned int>(m.fordblks);
			pm.heapMappedBytes = static_cast<unsigned int>(m.hblkhd);
		}
	}
#else
	void queryHeap(ProcessMemory&) { }
#endif

} // namespace

ProcessMemory queryProcessMemory()
{
	ProcessMemory pm;
#if defined(_WIN32)
	PROCESS_MEMORY_COUNTERS_EX counters {};
	counters.cb = sizeof(counters);
	if (GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters), sizeof(counters)))
	{
		pm.valid = true;
		pm.residentBytes = counters.WorkingSetSize;
		pm.privateBytes = counters.PrivateUsage;
		pm.peakResidentBytes = counters.PeakWorkingSetSize;
	}
#elif defined(__linux__)
	FILE* f = std::fopen("/proc/self/status", "r");
	if (f)
	{
		char line[256];
		bool any = false;
		while (std::fgets(line, sizeof(line), f))
		{
			unsigned long long kb = 0;
			if (std::sscanf(line, "VmRSS: %llu kB", &kb) == 1)
			{
				pm.residentBytes = kb * 1024ull;
				any = true;
			}
			else if (std::sscanf(line, "VmData: %llu kB", &kb) == 1)
			{
				pm.privateBytes = kb * 1024ull;
				any = true;
			}
			else if (std::sscanf(line, "VmHWM: %llu kB", &kb) == 1)
			{
				pm.peakResidentBytes = kb * 1024ull;
				any = true;
			}
		}
		std::fclose(f);
		pm.valid = any;
	}
#endif
	queryHeap(pm);
	return pm;
}

} // namespace memguard
