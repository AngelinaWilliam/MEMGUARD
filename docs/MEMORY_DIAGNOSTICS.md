# Memory diagnostics in MemGuard X

Memory tracking (what exists) and profiling (how long things take) are separate modules; nothing in this document depends on function profiling.

## What MemGuard X can and cannot see

| visibility | data | source |
|---|---|---|
| **exact** | open.mp pool entities: players, vehicles, objects, text labels, pickups, actors, gang zones, text draws, menus - creation/destruction **counts** | pool events |
| **exact** | script-reported allocations (`MemGuard_TrackAlloc/Free`): counts, and the byte sizes the script reports | the script |
| **exact** | timers, SQLite connections and result sets, AMX heap/stack in use per script | component APIs |
| **measured, not attributable** | process resident and private memory | `/proc/self/status` (Linux), `GetProcessMemoryInfo` (Windows) |
| **measured, not attributable** | C heap: bytes **in use** (live allocations) and bytes **free but retained** by the allocator | glibc `mallinfo2`/`mallinfo` (Linux), `HeapSummary` on the process heap (Windows) |
| **not observable** | which component, plugin or server subsystem owns a given allocation; byte sizes of pool entities; per-player objects/textdraws; data inside the AMX beyond heap/stack use | - |

MemGuard X does not hook `malloc`/`new`. A leak inside another component or plugin is therefore visible only as process/heap growth, never as a named allocation.

On Windows the heap numbers cover the process default heap, which the Visual C++ runtime (UCRT) uses for `malloc`/`new` in every module built with it. Other heaps (custom allocators, `VirtualAlloc`) are not included. The Windows heap path was compiled but has not been run on Windows yet (see VERIFICATION.md).

## Phases and warm-up (new in 0.2.0)

A server allocates most of its memory while it starts: components initialise, the gamemode loads data, pools fill, caches are built. MemGuard X therefore splits time into **phases**:

- the first phase starts with the first snapshot (when the server is ready);
- a new phase starts when the gamemode is loaded again after a restart (`gmx`);
- the first `warmup_seconds` (default 120) of each phase are **warm-up**. Growth during warm-up is reported as *startup* and never produces a "possible leak" warning.

Each snapshot metric is then classified:

| class | meaning |
|---|---|
| `warm-up` | still inside the warm-up period |
| `stable` | no significant change since the end of warm-up |
| `grew, now flat` | grew after warm-up but the last `growth_window` snapshots are flat - typical for caches reaching their size or more players online; not a leak signal by itself |
| `RISING` | rose in at least 80% of the steps of the last `growth_window` post-warm-up snapshots, by at least `growth_min_count` / `growth_min_mb` and `growth_min_percent` - **possible leak, investigate** |
| `decreasing` | went down since the end of warm-up |

"Significant" uses the same thresholds as growth detection (`growth_min_percent`, `growth_min_count`, `growth_min_mb`).

## Interpretation lines

The report section "Memory assessment" (and `memguard growth`) prints a table (phase start, warm-up end, now, startup change, change since warm-up, trend) followed by conclusions such as:

- *Startup: process memory changed by +X MiB between phase start and the end of warm-up ... not counted as a leak.*
- *Tracked resources still rising: tracked.pool.object.active ...* - exact counts; check that the script destroys them.
- *Live C-heap memory (in use) is rising while no tracked resource is rising* - growth from code MemGuard X cannot attribute. Possible leak.
- *Process memory is rising, but live heap memory is not: freed memory kept by the allocator* - allocator caching/fragmentation, not live data.
- *Process memory is rising outside the C heap* - large mappings, thread stacks, other allocators.
- *Process memory grew after warm-up but is flat in the recent snapshots* - consistent with a cache or working set reaching a steady size.

### Your observation: big increase at startup, later stable

That pattern is now classified as startup growth plus `stable` (or `grew, now flat` if part of it happened after warm-up), and it produces **no** leak warning. If it still looks suspicious, compare `process.heap_in_use_bytes` and `process.heap_free_bytes` after warm-up: flat in-use memory with higher free-retained memory means the allocator kept freed memory, not that something is still allocated. Increase `warmup_seconds` if your gamemode loads data for longer than two minutes.

## Evidence levels (unchanged)

1. **Confirmed tracking error** - double free, free of an unknown id, allocation of an id that is still active (from the event stream MemGuard X received).
2. **Possible leak** - a `RISING` metric after warm-up. Heuristic.
3. **Active allocation** - alive. Never reported as leaked.

## Configuration

| key | default | |
|---|---|---|
| `snapshot_interval` | 60 | seconds between snapshots |
| `warmup_seconds` | 120 | warm-up after server start / gamemode reload |
| `growth_window` | 6 | post-warm-up snapshots examined for a rising trend |
| `growth_min_percent` | 10 | minimum relative growth |
| `growth_min_count` | 10 | minimum growth of count metrics |
| `growth_min_mb` | 8 | minimum growth of byte metrics (MiB) |
| `history_size` | 120 | snapshots kept |
