# MemGuard X - design, coverage and limitations

## Modules

| module | files | depends on SDK? | what it does |
|---|---|---|---|
| Profiler | `src/core/profiler.*` | no | Zones registered once by name, addressed by dense id. Relaxed-atomic counters (calls, total, self, min, max, slow). Per-thread frame stack for nesting and self time. Abandoned frames are discarded and counted, never estimated. |
| Memory tracker | `src/core/memory_tracker.*` | no | `(category, id) -> size/time/sequence` map under one mutex. Counts allocs, frees, active, active bytes, peaks. Bounded "recently freed" history gives double-free evidence. Bounded event log of tracking errors. |
| Memory analysis | `src/core/memory_analysis.*` | no | Phase/warm-up aware classification of every snapshot metric and evidence-based interpretation (see MEMORY_DIAGNOSTICS.md). |
| Instrumenter | `src/instrument/*`, `tools/memguard_instrument.cpp` | no | Pawn tokenizer + function-definition finder; inserts `MEMGUARD_FUNC` without changing line numbers. |
| Snapshots / growth | `src/core/snapshot.*` | no | Snapshot = named integer metrics. Ring buffer history. `detectGrowth` flags sustained increases (min. fraction of rising steps, min. absolute and relative growth). |
| Config | `src/core/config.*` | no | Typed keys with ranges; same validation for `config.json` and `memguard set`. |
| Logger | `src/core/logger.*` | no | Level filter, per-key rate limiting with suppressed-count reporting; sink installed by the adapter. |
| Process memory | `src/core/process_memory.*` | no (Win32/Linux APIs) | `GetProcessMemoryInfo` (working set, private bytes) or `/proc/self/status` (VmRSS, VmData). |
| Engine | `src/core/engine.*` | no | Owns all of the above; tick scheduling, snapshot assembly, report text, console command interpreter. |
| Component adapter | `src/component/memguard_component.cpp` | yes | `IComponent` + `CoreEventHandler` + `PawnEventHandler` + `ConsoleEventHandler`; config, logging sink, pool watchers, metric providers. |
| Pawn bridge | `src/component/pawn_bridge.*` | yes | AMX callback hook (native profiling) and the `MemGuard_*` natives. |
| Pool watchers | `src/component/pool_watchers.hpp` | yes | `PoolEventHandler<T>` per pool -> tracker category `pool.<name>`. |

The core builds as a static library with no SDK dependency and is what the unit tests link against.

## How native profiling works (and its coverage)

The open.mp SDK has no "profile this native" API. What it does expose is the Pawn script (`IPawnScript::GetAMX()`), and in the Pawn abstract machine every native call made by a script (`SYSREQ.C` / `SYSREQ.N`) is dispatched through the function pointer `AMX::callback`. On `onAmxLoad` MemGuard X saves that pointer and installs its own callback, which:

1. resolves the native's name once per script/native index (`IPawnScript::GetNative`) and caches the zone id,
2. records begin/end on the profiler,
3. forwards to the saved callback.

The hook is removed on `onAmxUnload` and on component shutdown, and is re-checked every ~1000 ticks in case another component replaced the callback later.

Coverage, verified at runtime (see VERIFICATION.md): a loop of exactly 1000 `GetMaxPlayers()` calls produced exactly 1000 recorded calls, also after a gamemode restart (GMX).

Not covered / limitations:

- Pawn functions are not timed by this hook. Since 0.2.0 they are timed by `MEMGUARD_FUNC` (memguard.inc), inserted by hand or by `memguard_instrument`; see PAWN_PROFILING.md. The SDK offers no hook around `amx_Exec` for callbacks the server fires, and patching another component's code is not a supported interface, so function profiling is done in the script itself.
- Native times are **inclusive**. A native that runs Pawn code (`CallLocalFunction`, `CallRemoteFunction`, timers fired synchronously) includes that code. MemGuard X's own natives are passed through untimed.
- If some runtime or plugin rewrites `SYSREQ.C` into direct calls (`SYSREQ.D`), those calls would bypass `AMX::callback` and not be counted. The open.mp 1.5.8 runtime does not do this (exact counts verified).
- Cost: measured at startup and printed (`Profiler cost ~N ns per instrumented call`). In the 32-bit Linux test environment it was ~275 ns per call (two clock reads plus atomic counter updates). For servers calling millions of natives per second, set `profile_natives` to false, or `memguard set enable false` at runtime.

## How allocation tracking works (and what it can't see)

MemGuard X does **not** replace `malloc`/`new` or hook the C runtime. Interposing the process allocator in a host that loads many independently built DLLs with their own CRTs is fragile (mismatched heaps, recursion, loader-lock problems) and was judged not justified. Instead it tracks what it can observe reliably:

| source | category | size known? |
|---|---|---|
| open.mp pool events (player, vehicle, object, textlabel, pickup, actor, gangzone, textdraw, menu) | `pool.<name>` | no (count only) |
| `MemGuard_TrackAlloc/TrackFree` from Pawn | `script.<name>` | yes (as reported by the script) |
| C++ API (`MemoryTracker::onAlloc/onFree/onAllocPtr/onFreePtr`) | any | yes |

Additional snapshot gauges: `process.resident_bytes`, `process.private_bytes` (whole process, not attributable), `server.timers`, `server.db_connections`, `server.db_result_sets` (classic leak: missing `db_free_result`), `amx.scriptN.heap_bytes` / `stack_bytes`.

Not visible: memory allocated inside other components/plugins, the AMX's own memory beyond heap/stack usage, per-player objects/textdraws (separate per-player pools), entities created before MemGuard X attached (none in practice: pools are empty at `onInit`).

### Evidence levels

1. **Confirmed tracking error** - double free (id freed before and not re-allocated since, within `recent_free_capacity` remembered frees), free of an unknown id, allocation of an id that is still active. These are facts about the received event stream; for script categories they normally indicate a bookkeeping bug in the script.
2. **Possible leak** - a metric rose in at least 80% of the steps of the last `growth_window` snapshots and grew by at least `growth_min_count` (or `growth_min_mb` for byte metrics) and `growth_min_percent`. Heuristic. Caches warming up, player count rising, etc. also match.
3. **Active allocation** - simply alive. Never reported as a leak.

When a pool component shuts down, its watcher stops recording and its active entries are dropped, so shutdown order cannot produce false "unknown free" errors (this was a real bug found during runtime testing and fixed).

## Threading

open.mp calls components on its main thread. Nevertheless the core is thread-safe: profiler counters are atomics and frame stacks are per thread; tracker, logger and registries use mutexes; logger sinks are invoked outside the lock. Concurrency is covered by tests (8 threads, exact totals) and the test suite runs clean under ThreadSanitizer and AddressSanitizer/UBSan.

## Overhead control

- Hot path (native call): cached zone id lookup, two clock reads, a handful of relaxed atomics. No allocation, no string work, no locks.
- Snapshots and reports run on the main thread at the configured intervals and time themselves (`memguard.snapshot`, `memguard.report` zones in the report). In tests a full report took ~0.8 ms and a snapshot ~0.1-0.3 ms.
- Warnings are rate limited per kind (`warn_cooldown`).
