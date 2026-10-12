/*
 * MemGuard X - example gamemode.
 *
 * Shows the three things a script can do with memguard.inc:
 *   1. profile whole functions        MEMGUARD_FUNC("name");   (or tools/memguard_instrument)
 *   2. profile a part of a function   MemGuard_ProfileBegin / MemGuard_ProfileEnd
 *   3. report its own allocations     MemGuard_TrackAlloc / MemGuard_TrackFree
 * Native calls (SetPlayerPos, format, ...) are profiled by the component without any code.
 *
 * Look at the results with the console commands
 *     memguard top total 10 function
 *     memguard top self 10 function
 *     memguard mem
 * or in memguard_report.txt.
 */
#include <open.mp>
#include <memguard>

#define MAX_HOUSES          (200)

enum E_HOUSE
{
	bool:hUsed,
	Float:hX,
	Float:hY,
	Float:hZ,
	hPrice
};
static gHouses[MAX_HOUSES][E_HOUSE];

main()
{
	print("MemGuard X example gamemode loaded");
}

// --------------------------------------------------------------------- 1. whole functions

stock CreateHouse(Float:x, Float:y, Float:z, price)
{
	MEMGUARD_FUNC("CreateHouse");
	for (new i = 0; i < MAX_HOUSES; ++i)
	{
		if (!gHouses[i][hUsed])
		{
			gHouses[i][hUsed] = true;
			gHouses[i][hX] = x;
			gHouses[i][hY] = y;
			gHouses[i][hZ] = z;
			gHouses[i][hPrice] = price;
			// 3. tell MemGuard X that this slot is now in use (size: one house record)
			MemGuard_TrackAlloc("house", i, _:E_HOUSE * 4);
			return i;                       // early return: the profile still ends here
		}
	}
	return -1;
}

stock DeleteHouse(houseid)
{
	MEMGUARD_FUNC("DeleteHouse");
	if (houseid < 0 || houseid >= MAX_HOUSES || !gHouses[houseid][hUsed])
	{
		return 0;
	}
	gHouses[houseid][hUsed] = false;
	MemGuard_TrackFree("house", houseid);
	return 1;
}

stock Float:DistanceToNearestHouse(Float:x, Float:y)
{
	MEMGUARD_FUNC("DistanceToNearestHouse");
	new Float:best = 99999.0;
	for (new i = 0; i < MAX_HOUSES; ++i)
	{
		if (gHouses[i][hUsed])
		{
			new Float:d = VectorSize(gHouses[i][hX] - x, gHouses[i][hY] - y, 0.0);
			if (d < best)
			{
				best = d;
			}
		}
	}
	return best;                            // Float: return values are untouched
}

// --------------------------------------------------------------------- 2. sections

public OnGameModeInit()
{
	MEMGUARD_FUNC("OnGameModeInit");
	SetGameModeText("MemGuard X example");

	new section = MemGuard_ProfileBegin("LoadHouses");
	for (new i = 0; i < 150; ++i)
	{
		CreateHouse(float(i * 10), float(i * 5), 3.0, 1000 + i);
	}
	MemGuard_ProfileEnd(section);

	SetTimer("HouseMaintenance", 1000, true);
	return 1;
}

forward HouseMaintenance();
public HouseMaintenance()
{
	MEMGUARD_FUNC("HouseMaintenance");
	static tick;
	++tick;
	new Float:d = DistanceToNearestHouse(123.0, 456.0);
	if (tick % 5 == 0)
	{
		DeleteHouse(tick);
		new calls, total_us, self_us, max_us;
		if (MemGuard_GetZoneStats("DistanceToNearestHouse", calls, total_us, self_us, max_us))
		{
			printf("[example] DistanceToNearestHouse: %d calls, avg %d us, max %d us (nearest %.1f), houses active: %d",
				calls, total_us / calls, max_us, d, MemGuard_GetActiveCount("house"));
		}
	}
	return 1;
}

public OnPlayerConnect(playerid)
{
	MEMGUARD_FUNC("OnPlayerConnect");
	SendClientMessage(playerid, -1, "This server runs MemGuard X.");
	return 1;
}
