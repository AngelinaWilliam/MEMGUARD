# Changelog

## 0.2.0

New

- Pawn function profiling: `MEMGUARD_FUNC("name");` (memguard.inc) measures whole functions - calls, total, self, average, max - closing the measurement at every return through a Pawn destructor (`operator~`). New natives `MemGuard_FuncEnter`, `MemGuard_FuncLeave`, `MemGuard_GetZoneStats`, `MemGuard_GetVersion`. New zone kind `function`: report tables "Top function by total/self time/call count", `memguard top ... function`.
- `memguard_instrument`: source-to-source tool that inserts `MEMGUARD_FUNC` into every function of an instrumented copy of the sources, preserving line numbers.
- `MEMGUARD_DISABLE`: compile scripts that use memguard.inc for servers without the component.
- Memory phases: `warmup_seconds` (default 120), phase restart on gamemode reload, per-metric classification (warm-up / stable / grew-now-flat / RISING / decreasing), interpretation lines, "What MemGuard X can and cannot see" report section.
- C-heap statistics: `process.heap_in_use_bytes`, `process.heap_free_bytes` (glibc mallinfo2/mallinfo; Windows HeapSummary).
- Tests: instrumenter, memory analysis, profiler frame serials, and a test that keeps memguard.inc in sync with the registered natives.
- CMake 4.x support (policy minimum for the SDK's glm submodule); optional `MEMGUARDX_STATIC_LIBSTDCXX`.

Behaviour changes (please note)

- **Growth warnings start after warm-up.** With the default `warmup_seconds = 120`, "Possible leak" warnings are not raised during the first two minutes after start and after a gamemode reload. Set `warmup_seconds` to 0 for the 0.1.0 behaviour.
- **Profiler frames still open at a server tick are discarded** (counted as `abandoned`). This affects `MemGuard_ProfileBegin` sections that were begun in one callback and ended in a later one; such sections were never measured correctly (other callbacks' time was mixed in). Begin and end a section in the same callback.
- The "Top script" table is still fed only by explicit sections; function profiling has its own tables. Empty tables now say why.
- Growth warning text changed to "Possible leak (sustained growth after warm-up): ...".
- Component version 0.2.0 (UID unchanged). memguard.inc keeps every 0.1.0 native and constant unchanged.

## 0.1.0

First release: component, native-call profiler, pool and script allocation tracking, snapshots, growth detection, reports, console commands.
