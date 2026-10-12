# Building and installing MemGuard X

There are three separate deliverables. Build each for the machine that uses it:

| part | runs on | architecture | built from |
|---|---|---|---|
| `MemGuardX.dll` / `MemGuardX.so` (component) | the **game server** | **32-bit x86** (official open.mp server releases are 32-bit only) | `CMakeLists.txt`, target `MemGuardX` |
| `memguard.inc` (Pawn include) | the Pawn compiler | - (source file) | `pawn/memguard.inc`, nothing to build |
| `memguard_instrument` (optional tool) | the machine where you **compile Pawn** | any (native 64-bit is fine) | target `memguard_instrument` |

All need the open.mp SDK with submodules (`git clone --recursive https://github.com/openmultiplayer/open.mp-sdk`). The tool and the unit tests do not need the SDK if you pass `-DMEMGUARDX_BUILD_COMPONENT=OFF`.

CMake 3.25 or newer. CMake 4.x works (0.2.0 sets `CMAKE_POLICY_VERSION_MINIMUM` for the SDK's glm submodule, which otherwise stops CMake 4 with "Compatibility with CMake < 3.5 has been removed").

## 1. Windows component (MSVC)

```bat
cd C:\Users\PC\MemGuardX
tools\build_windows.bat              :: x86 Release: MemGuardX.dll + memguard_instrument.exe + tests (all run)
tools\build_windows.bat x64          :: x64: tests and a 64-bit memguard_instrument.exe only
```

Output: `build\x86-Release\MemGuardX.dll` (static CRT, no VC++ redistributable needed). An x64 `MemGuardX.dll` cannot be loaded by the 32-bit server; CMake warns when you configure one.

## 2. Linux component (32-bit)

On a 64-bit Debian/Ubuntu machine (or WSL):

```bash
sudo apt install g++-multilib cmake ninja-build      # 32-bit compiler support
cmake -S . -B build/linux-x86 -G Ninja -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_CXX_FLAGS=-m32 \
      -DMEMGUARDX_BUILD_TESTS=OFF -DMEMGUARDX_BUILD_INSTRUMENTER=OFF \
      -DOMP_SDK_DIR=/path/to/open.mp-sdk                # WSL: /mnt/c/Users/PC/open.mp-sdk
cmake --build build/linux-x86
file build/linux-x86/MemGuardX.so                     # must say: ELF 32-bit LSB shared object, Intel 80386
```

Run time dependencies of `MemGuardX.so` (check with `readelf -d MemGuardX.so | grep NEEDED`): `libstdc++.so.6`, `libgcc_s.so.1`, `libc.so.6`, `libdl.so.2` (glibc < 2.34), `ld-linux.so.2` - all **32-bit**. The open.mp server links its own C++ runtime statically, so a server host may have no 32-bit `libstdc++`. Either install it:

```bash
sudo dpkg --add-architecture i386 && sudo apt update && sudo apt install libstdc++6:i386
```

or build with `-DMEMGUARDX_STATIC_LIBSTDCXX=ON` (links libstdc++/libgcc into the .so; needs the 32-bit `libstdc++.a` from g++-multilib; this option has not been tested yet).

The `.so` you built earlier with GCC 9.4 (Ubuntu 20.04) requires at most `GLIBC_2.7` and `GLIBCXX_3.4.21`, so it runs on practically any current distribution.

Test builds on Linux: configure a second, native build directory without `-m32` (`-DMEMGUARDX_BUILD_COMPONENT=ON` is fine; the load test then loads the 64-bit build) and run `ctest`.

## 3. The tool `memguard_instrument`

Built by default together with the tests (`MEMGUARDX_BUILD_INSTRUMENTER=ON`). Windows: `build\x86-Release\memguard_instrument.exe` (or the x64 build). Linux: build natively:

```bash
cmake -S . -B build/host -DCMAKE_BUILD_TYPE=Release -DMEMGUARDX_BUILD_COMPONENT=OFF
cmake --build build/host --target memguard_instrument
```

Usage: docs/PAWN_PROFILING.md.

## 4. Installing on the server

1. Stop the server.
2. Copy the 32-bit component into `components/` (Windows `MemGuardX.dll`, Linux `MemGuardX.so`).
3. Copy `pawn/memguard.inc` into `qawno/include/` (the include folder of the compiler you use).
4. Optional: merge the `"memguard"` block from `config/memguard.example.json` into `config.json`.
5. Start the server and look for:
   ```
   Loading component MemGuardX.so
       Successfully loaded component MemGuard X (0.2.0.0) with UID 4208a60f1c13f660
   [Info] [MemGuardX] MemGuard X 0.2.0 loaded (open.mp ..., 32-bit process). ...
   [Info] [MemGuardX] Attached to Pawn script id 1 (... natives), native profiling on
   ```

## 5. Using the include in a gamemode

```pawn
#include <open.mp>
#include <memguard>
```

- To profile functions: `MEMGUARD_FUNC("Name");` as the first statement, or compile an instrumented copy (PAWN_PROFILING.md).
- The script then requires the component. To compile the same source for a server without it: `pawncc ... MEMGUARD_DISABLE=1`.

## 6. Configuration keys (`config.json`, section `"memguard"`)

| key | default | meaning |
|---|---|---|
| `enable` | true | master switch for all recording |
| `profile_natives` | true | time every Pawn native call |
| `track_pools` | true | track open.mp pool entities (startup only) |
| `log_level` | info | debug / info / warning / error / off |
| `report_interval` | 300 | seconds between report files (0 = off) |
| `snapshot_interval` | 60 | seconds between snapshots (0 = off) |
| `report_file` | memguard_report.txt | relative to the server folder |
| `report_append` | false | append instead of overwrite |
| `report_to_console` | false | also print periodic reports to the log |
| `top_n` | 15 | rows per profiler table |
| `history_size` | 120 | snapshots kept in memory |
| `warmup_seconds` | 120 | **new in 0.2.0**: startup period, see MEMORY_DIAGNOSTICS.md |
| `growth_window` | 6 | post-warm-up snapshots examined |
| `growth_min_percent` | 10 | minimum relative growth to flag |
| `growth_min_count` | 10 | minimum absolute growth for counts |
| `growth_min_mb` | 8 | minimum absolute growth for byte metrics (MiB) |
| `slow_native_us` | 5000 | count/warn calls longer than this (0 = off) |
| `warn_cooldown` | 60 | seconds between repeated warnings of one kind |
| `outstanding_limit` | 20 | oldest active allocations listed per report |
| `recent_free_capacity` | 4096 | freed ids remembered as double-free evidence |

Existing `config.json` files keep working; the new key gets its default.
