/*
 * MemGuard X - Pawn function profiling test.
 *
 * This file contains NO profiling markers. It is compiled twice:
 *   1. as written                        -> reference behaviour ("OUT" lines)
 *   2. after tools/memguard_instrument   -> must print identical "OUT" lines, and the
 *                                           "MGTEST" checks verify the profiler data.
 * See docs/VERIFICATION.md for the exact commands.
 */
#include <open.mp>
#include <memguard>

static gPass, gFail;

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

static Calls(const name[])
{
	MEMGUARD_FUNC("Calls"); return MemGuard_GetZoneCalls(name);
}

// ------------------------------------------------------------------ functions under test

Probe()
{
	MEMGUARD_FUNC("Probe"); return 1;
}

Float:Scale(Float:x)
{
	MEMGUARD_FUNC("Scale"); if (x > 1.0)
	{
		return x * 2.0;
	}
	return x + 0.5;
}

bool:IsBig(v)
{
	MEMGUARD_FUNC("IsBig"); return v > 3;
}

MakeName(id)
{
	MEMGUARD_FUNC("MakeName"); new s[24];
	format(s, sizeof s, "player_%d", id);
	return s;
}

SetRef(&ref, v)
{
	MEMGUARD_FUNC("SetRef"); ref = v * 3;
	for (new i = 0; i < 10; ++i)
	{
		if (i == v)
		{
			return i * 100;   // early return from inside a loop
		}
	}
	return -1;
}

SumArgs(...)
{
	MEMGUARD_FUNC("SumArgs"); new sum = 0;
	for (new i = 0; i < numargs(); ++i)
	{
		sum += getarg(i);
	}
	return sum;
}

Fib(n)
{
	MEMGUARD_FUNC("Fib"); if (n < 2)
	{
		return n;
	}
	return Fib(n - 1) + Fib(n - 2);
}

Pick(v)
{
	MEMGUARD_FUNC("Pick"); switch (v)
	{
		case 1: return 10;
		case 2:
		{
			new k = 5;
			return k * 2 + v;
		}
	}
	return 7;
}

static gVoidCalls;
NoReturnValue()
{
	MEMGUARD_FUNC("NoReturnValue"); ++gVoidCalls;
}

Busy(loops)
{
	MEMGUARD_FUNC("Busy"); new x = 0;
	for (new i = 0; i < loops; ++i)
	{
		x += i % 7;
	}
	return x;
}

Inner()
{
	MEMGUARD_FUNC("Inner"); return Busy(2000);
}

Outer()
{
	MEMGUARD_FUNC("Outer"); return Inner() + Inner() + Busy(1000);
}

forward PublicTarget(a, b);
public PublicTarget(a, b)
{
	MEMGUARD_FUNC("PublicTarget"); if (a > b)
	{
		return a - b;
	}
	return b - a;
}

Trivial(a)
{
	MEMGUARD_FUNC("Trivial"); return a + 1;
}

Trivial_NoProf(a)   // excluded from instrumentation (--exclude Trivial_NoProf)
{
	return a + 1;
}

Faulty(idx)
{
	MEMGUARD_FUNC("Faulty"); new arr[2];
	arr[idx] = 1;     // runtime error for idx >= 2: the function never returns normally
	return arr[0];
}

// ------------------------------------------------------------------ test driver

main()
{
MEMGUARD_FUNC("main"); }

public OnGameModeInit()
{
	// functional output: must be identical with and without instrumentation
	MEMGUARD_FUNC("OnGameModeInit"); printf("OUT Scale %.2f %.2f", Scale(3.0), Scale(0.5));
	printf("OUT IsBig %d %d", IsBig(5), IsBig(1));
	new name[24];
	name = MakeName(7);
	printf("OUT MakeName %s", name);
	new r;
	new v = SetRef(r, 3);
	printf("OUT SetRef %d %d", v, r);
	printf("OUT SetRef2 %d", SetRef(r, 50));
	printf("OUT SumArgs %d", SumArgs(1, 2, 3, 4));
	printf("OUT Fib %d", Fib(10));
	printf("OUT Pick %d %d %d", Pick(1), Pick(2), Pick(9));
	NoReturnValue();
	NoReturnValue();
	printf("OUT Void %d", gVoidCalls);
	printf("OUT Outer %d", Outer());
	printf("OUT Public %d %d", CallLocalFunction("PublicTarget", "ii", 3, 10), CallLocalFunction("PublicTarget", "ii", 10, 3));

	Probe();
	new bool:instrumented = Calls("Probe") > 0;
	printf("OUT instrumented=%d", instrumented ? 1 : 0);
	if (!instrumented)
	{
		printf("MGTEST SUMMARY not instrumented - functional reference run only");
		return 1;
	}

	Check(MemGuard_GetVersion() == MEMGUARD_INC_VERSION, "component version matches memguard.inc", MemGuard_GetVersion(), MEMGUARD_INC_VERSION);

	// call-count checks use deltas so they also hold after a gamemode restart
	new before = Calls("Fib");
	Fib(10);
	Check(Calls("Fib") - before == 177, "Fib(10) recursion recorded 177 calls", Calls("Fib") - before, 177);

	before = Calls("SetRef");
	for (new i = 0; i < 100; ++i)
	{
		SetRef(r, i % 12);   // mixes early returns (i%12 < 10) and the final return
	}
	Check(Calls("SetRef") - before == 100, "SetRef: every early/late return closes its frame", Calls("SetRef") - before, 100);

	before = Calls("Pick");
	for (new i = 0; i < 30; ++i)
	{
		Pick(i % 3);
	}
	Check(Calls("Pick") - before == 30, "Pick: returns inside switch cases", Calls("Pick") - before, 30);

	before = Calls("NoReturnValue");
	NoReturnValue();
	Check(Calls("NoReturnValue") - before == 1, "function without return statement", Calls("NoReturnValue") - before, 1);

	new beforeInner = Calls("Inner"), beforeOuter = Calls("Outer");
	for (new i = 0; i < 50; ++i)
	{
		Outer();
	}
	Check(Calls("Outer") - beforeOuter == 50, "Outer called 50 times", Calls("Outer") - beforeOuter, 50);
	Check(Calls("Inner") - beforeInner == 100, "nested Inner called 100 times", Calls("Inner") - beforeInner, 100);

	new calls, total, self, maxv;
	Check(MemGuard_GetZoneStats("Outer", calls, total, self, maxv) == 1, "MemGuard_GetZoneStats finds Outer", 1, 1);
	new oTotal = total, oSelf = self;
	MemGuard_GetZoneStats("Inner", calls, total, self, maxv);
	new iTotal = total;
	printf("MGTEST INFO Outer total=%dus self=%dus, Inner total=%dus", oTotal, oSelf, iTotal);
	Check(oTotal >= iTotal, "Outer inclusive time >= Inner inclusive time", oTotal, iTotal);
	Check(oSelf < oTotal, "Outer self time < inclusive time (nested time excluded)", oSelf, oTotal);
	new found = MemGuard_GetZoneStats("NoSuchZone", calls, total, self, maxv);
	Check(found == 0 && calls == 0, "unknown zone returns 0 and zeroed outputs", calls, 0);

	before = Calls("PublicTarget");
	CallLocalFunction("PublicTarget", "ii", 1, 2);
	Check(Calls("PublicTarget") - before == 1, "public called through CallLocalFunction is profiled", Calls("PublicTarget") - before, 1);

	Check(Calls("Trivial_NoProf") == 0, "excluded function has no zone", Calls("Trivial_NoProf"), 0);

	// rough cost of instrumentation per call, measured here (not asserted)
	new t0 = GetTickCount(), acc = 0;
	for (new i = 0; i < 100000; ++i)
	{
		acc += Trivial(i);
	}
	new t1 = GetTickCount();
	for (new i = 0; i < 100000; ++i)
	{
		acc += Trivial_NoProf(i);
	}
	new t2 = GetTickCount();
	printf("MGTEST INFO 100000 calls: instrumented %d ms, uninstrumented %d ms (acc %d)", t1 - t0, t2 - t1, acc);

	SetTimer("FaultyTimer", 500, false);
	SetTimer("AfterFaultTimer", 1500, false);
	return 1;
}

forward FaultyTimer();
public FaultyTimer()
{
	MEMGUARD_FUNC("FaultyTimer"); print("MGTEST INFO calling Faulty(5): a runtime error is expected next");
	Faulty(5);
	print("MGTEST FAIL this line must not be reached");
	return 1;
}

forward AfterFaultTimer();
public AfterFaultTimer()
{
	MEMGUARD_FUNC("AfterFaultTimer"); new before = Calls("SetRef"), r;
	for (new i = 0; i < 10; ++i)
	{
		SetRef(r, i);
	}
	Check(Calls("SetRef") - before == 10, "profiling still correct after an aborted callback", Calls("SetRef") - before, 10);
	Check(Calls("Faulty") == 0, "aborted function is not recorded as a completed call", Calls("Faulty"), 0);
	printf("MGTEST SUMMARY pass=%d fail=%d", gPass, gFail);
	return 1;
}
