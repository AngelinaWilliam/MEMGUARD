# MemGuard X

Diagnostic and performance-monitoring component for open.mp servers (C++20, official open.mp SDK), with a Pawn include and a build-time instrumentation tool.

| feature | how | status |
|---|---|---|
| Native-call profiling | AMX callback hook, automatic | implemented, runtime-verified |
| **Pawn function profiling** | `MEMGUARD_FUNC("name");` in a function, via `memguard.inc` | implemented, runtime-verified (v0.2) |
| **Automatic function instrumentation** | `memguard_instrument` source-to-source tool before compiling | implemented, runtime-verified on test scripts (v0.2) |
| Code-section profiling | `MemGuard_ProfileBegin/End` | implemented, runtime-verified |
| Server tick statistics | core tick event | implemented |
| Resource / allocation tracking | open.mp pool events + `MemGuard_TrackAlloc/Free` | implemented, runtime-verified |
| **Memory phases and assessment** | warm-up period, C-heap statistics, trend classification | implemented, unit-tested, runtime-checked (v0.2) |
| Reports, console commands, config | text report, `memguard ...`, `config.json` | implemented |

Documentation:

- [docs/BUILD_AND_INSTALL.md](docs/BUILD_AND_INSTALL.md) - building on Windows and Linux, installing the component, the include and the tool
- [docs/PAWN_PROFILING.md](docs/PAWN_PROFILING.md) - function profiling, automatic instrumentation, Pawn API reference
- [docs/MEMORY_DIAGNOSTICS.md](docs/MEMORY_DIAGNOSTICS.md) - what is tracked, what is measured, what cannot be seen, how growth is classified
- [docs/DESIGN.md](docs/DESIGN.md) - architecture and internals
- [docs/VERIFICATION.md](docs/VERIFICATION.md) - every test that was run and its actual result
- [CHANGELOG.md](CHANGELOG.md) - changes, including behaviour changes in 0.2.0

## Quick start

1. Build (details in BUILD_AND_INSTALL.md). The open.mp server is **32-bit**, so the component must be built for x86:
   - Windows: `tools\build_windows.bat` -> `build\x86-Release\MemGuardX.dll`
   - Linux: `cmake -S . -B build/linux-x86 -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_FLAGS=-m32 -DMEMGUARDX_BUILD_TESTS=OFF -DMEMGUARDX_BUILD_INSTRUMENTER=OFF -DOMP_SDK_DIR=...` -> `build/linux-x86/MemGuardX.so`
2. Copy the component into the server's `components/` folder and `pawn/memguard.inc` into `qawno/include/`.
3. In your gamemode: `#include <memguard>` after `#include <open.mp>`.
4. Profile functions, either by hand

   ```pawn
   stock SaveAccount(playerid)
   {
            
       // unchanged body, any returns
   }
   ```

   or automatically for every function, without touching your sources:

   ```
   memguard_instrument --out build/instrumented gamemodes/mygm.pwn gamemodes/modules
   pawncc build/instrumented/mygm.pwn -ibuild/instrumented -iqawno/include ...
   ```

5. Read results: `memguard top total 15 function`, `memguard top self 15 function`, `memguard growth`, or `memguard_report.txt`.

## Console commands

Type in the server console (one command per line; `mgx` is an alias). In game, `/rcon memguard ...` uses the same handler (not runtime-tested).

```
memguard help | status | report
memguard top [total|self|calls|max|avg] [n] [native|function|script|internal|interval]
memguard mem [n]                    tracked allocations + n oldest active entries
memguard snapshot                   capture and print a snapshot now
memguard growth                     growth after warm-up + memory phase assessment (alias: assess)
memguard history [n]                compare the last n snapshots
memguard reset [profiler|memory|history|all]
memguard config | get <key> | set <key> <value>     (set = runtime only)
```

## Configuration

Optional `"memguard"` section in `config.json`; see [config/memguard.example.json](config/memguard.example.json) and the key table in BUILD_AND_INSTALL.md. New in 0.2.0: `warmup_seconds` (default 120).

## Project layout

```
CMakeLists.txt
src/core/          SDK-independent diagnostics: profiler, memory tracker, snapshots, memory analysis,
                   config, logger, process/heap memory, engine (reports + commands)
src/component/     open.mp adapter: component, Pawn bridge (natives + native-call hook), pool watchers
src/instrument/    Pawn source instrumenter (library)
tools/             memguard_instrument (CLI), build_windows.bat
tests/             unit tests, include/API sync test, component load test
pawn/              memguard.inc, memguard_test.pwn, tests/function_profiling_test.pwn, examples/
config/            example configuration
third_party/       unmodified AMX headers (zlib licence)
docs/              documentation and evidence/ (real logs and reports from test runs)
```
