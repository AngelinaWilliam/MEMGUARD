/*
 * memguard.inc - Pawn API for the MemGuard X open.mp component.
 *
 *     #include <open.mp>
 *     #include <memguard>
 *
 * Every native declared here is registered by MemGuard X (src/component/pawn_bridge.cpp).
 * A script that uses them needs the component in components/; without it open.mp refuses to
 * run the script ("Function not registered: MemGuard_..."). To build the same source without
 * the component, define MEMGUARD_DISABLE before the include: every call then becomes a no-op
 * returning 0 and MEMGUARD_FUNC expands to nothing.
 *
 * Sections:
 *   1. Pawn function profiling   - MEMGUARD_FUNC (one line per function, or automatic with
 *                                  tools/memguard_instrument)
 *   2. Code-section profiling    - MemGuard_ProfileBegin/End, MemGuard_ZoneId/ZoneBegin
 *   3. Profiler queries          - MemGuard_GetZoneCalls, MemGuard_GetZoneStats
 *   4. Memory / resource tracking - MemGuard_TrackAlloc/TrackFree, MemGuard_GetActiveCount
 *   5. Diagnostics               - MemGuard_Snapshot, MemGuard_WriteReport, MemGuard_GetVersion
 * 
 * it's for all guys if you change it upload it
 *
 * Every Pawn *native* call is profiled by the component automatically; nothing here is needed
 * for that.
 */
#if defined _memguard_included
	#endinput
#endif
#define _memguard_included

/* Version of the API described by this file (MemGuard_GetVersion() returns the component's). */
#define MEMGUARD_INC_VERSION        (0x000200)   // 0.2.0 = (major << 16) | (minor << 8) | patch

/* Return values of MemGuard_TrackAlloc */
#define MG_ALLOC_IGNORED      (0)   // tracker disabled or invalid arguments
#define MG_ALLOC_TRACKED      (1)
#define MG_ALLOC_DUPLICATE    (2)   // id was still active: tracking error recorded

/* Return values of MemGuard_TrackFree */
#define MG_FREE_IGNORED       (0)
#define MG_FREE_RELEASED      (1)
#define MG_FREE_UNKNOWN       (-1)  // id was never tracked
#define MG_FREE_DOUBLE        (-2)  // id was already freed (double free)

#if !defined MEMGUARD_DISABLE

/* ------------------------------------------------------------------------------------------
 * 1. Pawn function profiling
 *
 *     stock SaveAccount(playerid)
 *     {
 *         MEMGUARD_FUNC("SaveAccount");
 *         ... unchanged body, any number of return statements ...
 *     }
 *
 * MEMGUARD_FUNC declares a local variable of tag MGFunc. Pawn calls operator~ (below) for it
 * whenever the function leaves that scope - at every `return`, and at the closing brace - so
 * the measurement ends exactly when the function returns, whatever it returns (cell, Float:,
 * bool:, arrays/strings) and however deeply it nests or recurses. The function's return value
 * is not touched. Statistics: calls, total (inclusive) time, self time (excluding natives and
 * other profiled functions it calls), average and maximum, shown in "Top function" report
 * tables and `memguard top ... function`.
 *
 * Put it as the first statement of the function. Not measured: code that is aborted by a
 * runtime error or `exit` (such frames are discarded at the next server tick).
 * ---------------------------------------------------------------------------------------- */

native MemGuard_FuncEnter(&cache, const name[]);   // returns a frame handle (> 0) or 0
native MemGuard_FuncLeave(frame);                  // returns 1 if the frame was closed

stock operator~(const MGFunc:frames[], count)
{
	for (new i = 0; i < count; ++i)
	{
		if (_:frames[i] > 0)
		{
			MemGuard_FuncLeave(_:frames[i]);
		}
	}
}

#define MEMGUARD_FUNC(%0); static __mgx_zone = -1; new MGFunc:__mgx_frame = MGFunc:MemGuard_FuncEnter(__mgx_zone, %0);

/* ------------------------------------------------------------------------------------------
 * 2. Code-section profiling (part of a function, or several functions)
 *
 *     new zone = MemGuard_ProfileBegin("LoadHouses");
 *     ...
 *     MemGuard_ProfileEnd(zone);
 *
 * Begin and End must run in the same callback execution: a section still open at the next
 * server tick is discarded (counted as abandoned in `memguard status`).
 * ---------------------------------------------------------------------------------------- */

/// Starts a section. Returns the zone id (>= 0) or -1 on error.
native MemGuard_ProfileBegin(const name[]);

/// Ends the most recent open section with this zone id. Returns 1 on success, 0 if not open.
native MemGuard_ProfileEnd(zone);

/// Registers (or looks up) a zone without starting it. Returns the zone id or -1.
native MemGuard_ZoneId(const name[]);

/// Starts a section by id (faster than MemGuard_ProfileBegin in hot code). Returns 1 on success.
native MemGuard_ZoneBegin(zone);

/* ------------------------------------------------------------------------------------------
 * 3. Profiler queries
 * ---------------------------------------------------------------------------------------- */

/// Recorded calls of a zone: a native ("SetPlayerPos"), a profiled function or a section.
/// Returns 0 if the zone does not exist.
native MemGuard_GetZoneCalls(const name[]);

/// Statistics of a zone; times in microseconds (clamped to cellmax). Returns 1 if the zone
/// exists, 0 otherwise (all outputs set to 0).
native MemGuard_GetZoneStats(const name[], &calls, &total_us, &self_us, &max_us);

/* ------------------------------------------------------------------------------------------
 * 4. Memory / resource tracking
 *
 * Tracks what the script reports - ids it allocates and frees (house cache slots, dynamic
 * arrays, DB result handles...). Category names are prefixed with "script.". The component
 * also tracks open.mp pool entities by itself ("pool.vehicle", "pool.object", ...).
 * An active entry is NOT a leak; see docs/MEMORY_DIAGNOSTICS.md.
 * ---------------------------------------------------------------------------------------- */

/// Records an allocation. Returns MG_ALLOC_TRACKED, MG_ALLOC_DUPLICATE or MG_ALLOC_IGNORED.
native MemGuard_TrackAlloc(const category[], id, size = 0);

/// Records a release. Returns MG_FREE_RELEASED, MG_FREE_UNKNOWN, MG_FREE_DOUBLE or MG_FREE_IGNORED.
native MemGuard_TrackFree(const category[], id);

/// Active entries of a script category ("house_cache") or a built-in one ("pool.vehicle").
/// Returns 0 for an unknown category, -1 on invalid arguments.
native MemGuard_GetActiveCount(const category[]);

/* ------------------------------------------------------------------------------------------
 * 5. Diagnostics
 * ---------------------------------------------------------------------------------------- */

/// Takes a memory snapshot now. Returns the snapshot sequence number.
native MemGuard_Snapshot();

/// Writes the full report file now. Returns 1 on success, 0 on failure (see server log).
native MemGuard_WriteReport();

/// Component version as (major << 16) | (minor << 8) | patch.
native MemGuard_GetVersion();

#else // MEMGUARD_DISABLE: same API, no component required, no code generated for MEMGUARD_FUNC.

#define MEMGUARD_FUNC(%0); {}

stock MemGuard_ProfileBegin(const name[])
{
	#pragma unused name
	return -1;
}
stock MemGuard_ProfileEnd(zone)
{
	#pragma unused zone
	return 0;
}
stock MemGuard_ZoneId(const name[])
{
	#pragma unused name
	return -1;
}
stock MemGuard_ZoneBegin(zone)
{
	#pragma unused zone
	return 0;
}
stock MemGuard_GetZoneCalls(const name[])
{
	#pragma unused name
	return 0;
}
stock MemGuard_TrackAlloc(const category[], id, size = 0)
{
	#pragma unused category, id, size
	return MG_ALLOC_IGNORED;
}
stock MemGuard_TrackFree(const category[], id)
{
	#pragma unused category, id
	return MG_FREE_IGNORED;
}
stock MemGuard_GetActiveCount(const category[])
{
	#pragma unused category
	return 0;
}
stock MemGuard_GetZoneStats(const name[], &calls, &total_us, &self_us, &max_us)
{
	#pragma unused name
	calls = total_us = self_us = max_us = 0;
	return 0;
}
stock MemGuard_Snapshot() { return 0; }
stock MemGuard_WriteReport() { return 0; }
stock MemGuard_GetVersion() { return 0; }

#endif
