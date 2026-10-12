# MemGuard X - verification record

Only what was actually run is listed as verified. Environment for 0.2.0: Linux cloud workspace (GCC 13.3, clang 18, llvm-mingw clang 23 for Windows cross-compilation, CMake 3.28 and 4.4.4) and the official **open.mp 1.5.8.3079 Linux x86 (32-bit)** server with its bundled Pawn compiler 3.10.11. No shell on the Windows PC was available, so nothing in 0.2.0 has been compiled with MSVC yet.

## 0.2.0

### Reproduction of the reported problem

| step | result |
|---|---|
| 0.1.0 component + stock `gungame` gamemode, `memguard report` | report shows `Top 15 script by total (inclusive) time / (no samples recorded)` - reproduced |
| cause (source audit) | script zones were created only by `MemGuard_ProfileBegin/ZoneBegin`; no code path measured Pawn functions |

### Automated tests (`ctest`)

| test | toolchain | result |
|---|---|---|
| `memguardx_unit_tests` - 63 tests (tracker, profiler incl. frame serials/recursion/discard, engine, config, logger, growth, **memory phase analysis (8)**, **instrumenter (11)**, **include/API sync (1)**) | GCC 13.3 x64 | **63/63 pass** |
| same under AddressSanitizer + UBSan | GCC 13.3 | 63/63, no reports |
| same under ThreadSanitizer | GCC 13.3 | 63/63, no reports |
| `memguardx_component_load` (dlopen, entry point, UID, version 0.2.0, config defaults incl. new key, free) | GCC 13.3 x64 | pass |
| `memguardx_instrument_cli` (CLI over all files in `pawn/`) | GCC 13.3 x64 | pass, 0 files failed |
| full configure/build/ctest with **CMake 4.4.4** | GCC 13.3 | **was failing** (glm: "Compatibility with CMake < 3.5 has been removed") -> fixed -> all pass |
| include/API sync test detects drift | mutation: changed `MemGuard_TrackFree` parameter count in memguard.inc | test failed as expected, then restored |
| Windows cross-build (component, tool, tests) | llvm-mingw i686 | builds; DLL exports only `ComponentEntryPoint`; Windows heap code (`HeapSummary`) compiles. Not executed (no Windows runtime). |

### Runtime tests in the real server (32-bit Linux)

`pawn/tests/function_profiling_test.pwn` contains no profiling markers. It was compiled (a) as written and (b) after `memguard_instrument --exclude Trivial_NoProf` (21 of 22 functions instrumented). Both compile without warnings.

| check | result |
|---|---|
| functional output (`OUT` lines: Float/bool/string returns, reference param, varargs, recursion, switch returns, void function, nested calls, `CallLocalFunction`) plain vs instrumented | **identical** (only the `instrumented=` flag differs) |
| `MGTEST` checks in the instrumented build | **15/15 pass**, and **15/15 again after `gmx`** |
| recursion Fib(10) | exactly 177 recorded calls |
| 100 calls mixing early returns in a loop and the final return | 100 recorded calls |
| returns inside `switch` cases / function without `return` | counts exact |
| nested: Outer x50 calling Inner x2 | 50 / 100 calls, Outer inclusive >= Inner inclusive, Outer self < Outer inclusive |
| public via `CallLocalFunction` | recorded |
| excluded function | no zone created |
| runtime error inside a profiled function (array index out of bounds, called from a timer) | callback aborted by the server; the 2 open frames were discarded at the next tick (`abandoned=2`), the aborted call is not recorded, later calls are counted exactly |
| `MemGuard_GetVersion()` == `MEMGUARD_INC_VERSION` | 0x000200 |
| cost per profiled call (100 000 calls of a trivial function) | 56-63 ms instrumented vs 1-2 ms plain, about 0.55 us per call |
| instrumented copy compiled with `MEMGUARD_DISABLE=1`, server **without** MemGuard X | runs; output identical to the plain build; no "Function not registered" errors |
| plain script that uses MemGuard natives, server without MemGuard X | server refuses to run it (`Function not registered: MemGuard_...`, `File or function is not found`) - documented, hence `MEMGUARD_DISABLE` |
| multi-file project (`mygm.pwn` + `modules/util.inc` via relative include) instrumented and compiled from the output folder | works; included function profiled |
| `pawn/examples/profiling_example.pwn` | compiles without warnings; function, section and `script.house` tracking tables populated |
| regression: `pawn/memguard_test.pwn` (0.1.0 test) with the 0.2.0 component and include | **21/21 pass**, again after `gmx` |
| memory assessment in the real server (object leak test, `warmup_seconds` 3) | `tracked.pool.object.active` classified RISING with a "sustained growth after warm-up" warning; process/heap memory (+1.4 MiB after warm-up) below threshold -> stable; startup change reported separately; heap source `glibc mallinfo2` |
| new memory phase after `gmx` | logged when the gamemode is loaded again |

Logs and reports from these runs: `docs/evidence/function_profiling_log_linux_x86.txt`, `function_profiling_reference_run.txt`, `sample_report_v0.2_function_profiling.txt`, `runtime_log_linux_x86.txt`, `sample_report_linux_x86.txt`.

### Bugs found while testing 0.2.0 and fixed

1. Inserting `MEMGUARD_FUNC` right after `{` caused pawncc warning 217 (loose indentation) in every function; it is now inserted in front of the first statement on its own line.
2. With `MEMGUARD_DISABLE`, an empty macro expansion caused the same warning; it now expands to `{}`.
3. `operator~` produced warning 214; parameter is now `const`.
4. The memory phase restarted at `reset()`, before open.mp reloads the gamemode (about 12 s later), so the new gamemode's loading fell outside warm-up; the phase now restarts when the script is loaded.
5. The instrumenter instrumented the `MEMGUARD_DISABLE` stubs inside memguard.inc; MemGuard X's own functions and memguard.inc are now skipped.
6. CMake 4.x could not configure the project (SDK submodule glm).

### Not verified

- **MSVC build of 0.2.0** and loading it into the Windows server. (0.1.0 was built by you with MSVC: `build\x86-Release\Testing\Temporary\LastTest.log` shows 40/40 unit tests and the load test passing.)
- Windows heap statistics (`HeapSummary`) at run time.
- GCC 9.4 (your WSL Ubuntu 20.04 toolchain) for 0.2.0.
- `MEMGUARDX_STATIC_LIBSTDCXX=ON`.
- `/rcon memguard` from a player; YSI-style macro-generated functions; very large real gamemodes through `memguard_instrument`.

## 0.1.0 (previous round, unchanged)

Built and tested on Linux (40 unit tests, ASan/TSan), cross-built for Win32, loaded into open.mp 1.5.8.3079 (32-bit Linux), runtime test 21/21 before and after `gmx`, console commands checked. Built by the user with MSVC x86: 40/40 unit tests + load test pass.
