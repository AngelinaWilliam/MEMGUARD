/*
 * MemGuard X runtime verification gamemode.
 * Compile with the open.mp compiler (qawno) and run it as the main script. It prints
 * "MGTEST PASS ..." / "MGTEST FAIL ..." lines and a final "MGTEST SUMMARY" line.
 * It also leaks objects on purpose (LeakTimer) so the growth detector has something to find.
 */
#include <open.mp>
#include "memguard"

static gPass = 0;
static gFail = 0;
static gLeakTicks = 0;

static Check(bool:ok, const what[], got, expected)
{
	MEMGUARD_FUNC("Check"); if (ok)
	{
		++gPass;
		printf("MGTEST PASS %s (got %d)", what, got);
	}
	else
	{
		++gFail;
		printf("MGTEST FAIL %s (got %d, expected %d)", what, got, expected);
	}
}

main()
{
MEMGUARD_FUNC("main"); }

forward LeakTimer();
public LeakTimer()
{
	// Intentional leak: 15 objects per second that are never destroyed.
	MEMGUARD_FUNC("LeakTimer"); for (new i = 0; i < 15; ++i)
	{
		CreateObject(1337, float(i), float(gLeakTicks), 3.0, 0.0, 0.0, 0.0);
	}
	++gLeakTicks;
	if (gLeakTicks == 8)
	{
		MemGuard_WriteReport();
		printf("MGTEST leak phase finished, objects created: %d, tracked active objects: %d", gLeakTicks * 15, MemGuard_GetActiveCount("pool.object"));
		Check(MemGuard_GetActiveCount("pool.object") == gLeakTicks * 15, "pool.object active count matches leaked objects", MemGuard_GetActiveCount("pool.object"), gLeakTicks * 15);
		printf("MGTEST SUMMARY pass=%d fail=%d", gPass, gFail);
	}
	return 1;
}

public OnGameModeInit()
{
	// 1. Native-call profiling: exactly 1000 calls of one native.
	MEMGUARD_FUNC("OnGameModeInit"); new before = MemGuard_GetZoneCalls("GetMaxPlayers");
	new dummy = 0;
	for (new i = 0; i < 1000; ++i)
	{
		dummy += GetMaxPlayers();
	}
	new calls = MemGuard_GetZoneCalls("GetMaxPlayers") - before;
	Check(calls == 1000, "native GetMaxPlayers profiled exactly 1000 times", calls, 1000);
	#pragma unused dummy

	// 2. Script zones (statistics survive a gamemode restart, so compare deltas).
	new zoneBefore = MemGuard_GetZoneCalls("test_section");
	for (new rep = 0; rep < 5; ++rep)
	{
		new zone = MemGuard_ProfileBegin("test_section");
		new s[64];
		for (new i = 0; i < 200; ++i)
		{
			format(s, sizeof s, "iteration %d", i);
		}
		Check(MemGuard_ProfileEnd(zone) == 1, "MemGuard_ProfileEnd closes the zone", 1, 1);
	}
	calls = MemGuard_GetZoneCalls("test_section") - zoneBefore;
	Check(calls == 5, "script zone test_section recorded 5 times", calls, 5);
	calls = MemGuard_GetZoneCalls("format");
	Check(calls >= 1000, "native format profiled inside the zone (>= 1000)", calls, 1000);
	Check(MemGuard_ProfileEnd(12345) == 0, "ending a zone that is not open returns 0", 0, 0);

	// 3. Explicit allocation tracking (a fresh category per run, because tracked entries
	//    intentionally survive a gamemode restart).
	new inventory[32];
	format(inventory, sizeof inventory, "inventory_%d", GetTickCount());
	Check(MemGuard_TrackAlloc(inventory, 1, 100) == MG_ALLOC_TRACKED, "TrackAlloc id 1", 1, 1);
	Check(MemGuard_TrackAlloc(inventory, 2, 50) == MG_ALLOC_TRACKED, "TrackAlloc id 2", 1, 1);
	new r = MemGuard_TrackAlloc(inventory, 2, 50);
	Check(r == MG_ALLOC_DUPLICATE, "TrackAlloc of an active id reports duplicate", r, MG_ALLOC_DUPLICATE);
	r = MemGuard_TrackFree(inventory, 1);
	Check(r == MG_FREE_RELEASED, "TrackFree id 1 released", r, MG_FREE_RELEASED);
	r = MemGuard_TrackFree(inventory, 1);
	Check(r == MG_FREE_DOUBLE, "second TrackFree id 1 is a double free", r, MG_FREE_DOUBLE);
	r = MemGuard_TrackFree(inventory, 77);
	Check(r == MG_FREE_UNKNOWN, "TrackFree of unknown id 77", r, MG_FREE_UNKNOWN);
	r = MemGuard_GetActiveCount(inventory);
	Check(r == 1, "inventory has 1 active entry", r, 1);

	// 4. Pool entity tracking.
	new vehiclesBefore = MemGuard_GetActiveCount("pool.vehicle");
	new v1 = CreateVehicle(411, 0.0, 0.0, 3.0, 0.0, -1, -1, 60);
	new v2 = CreateVehicle(411, 5.0, 0.0, 3.0, 0.0, -1, -1, 60);
	new v3 = CreateVehicle(411, 10.0, 0.0, 3.0, 0.0, -1, -1, 60);
	r = MemGuard_GetActiveCount("pool.vehicle") - vehiclesBefore;
	Check(r == 3, "3 vehicles tracked after CreateVehicle", r, 3);
	DestroyVehicle(v2);
	r = MemGuard_GetActiveCount("pool.vehicle") - vehiclesBefore;
	Check(r == 2, "2 vehicles tracked after DestroyVehicle", r, 2);
	#pragma unused v1, v3

	// 5. Snapshot and report.
	r = MemGuard_Snapshot();
	Check(r >= 1, "MemGuard_Snapshot returns a sequence number", r, 1);
	r = MemGuard_WriteReport();
	Check(r == 1, "MemGuard_WriteReport succeeded", r, 1);

	SetTimer("LeakTimer", 1000, true);
	return 1;
}
