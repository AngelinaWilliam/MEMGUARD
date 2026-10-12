cmake_minimum_required(VERSION 3.25)

project(MemGuardX
	VERSION 0.2.0
	DESCRIPTION "Diagnostic, profiling and allocation-tracking component for open.mp servers"
	LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

option(MEMGUARDX_BUILD_COMPONENT "Build the open.mp component (needs the open.mp SDK)" ON)
option(MEMGUARDX_BUILD_TESTS "Build the unit tests" ON)

if(WIN32)
	set(_mgx_default_sdk "C:/Users/PC/open.mp-sdk")
else()
	set(_mgx_default_sdk "${CMAKE_CURRENT_SOURCE_DIR}/../open.mp-sdk")
endif()
set(OMP_SDK_DIR "${_mgx_default_sdk}" CACHE PATH "Path to the open.mp SDK (git clone --recursive https://github.com/openmultiplayer/open.mp-sdk)")

# ----------------------------------------------------------------------------- warnings
function(memguardx_warnings target)
	if(MSVC)
		target_compile_options(${target} PRIVATE /W4 /permissive- /utf-8 /Zc:__cplusplus)
	else()
		target_compile_options(${target} PRIVATE -Wall -Wextra)
	endif()
endfunction()

# ----------------------------------------------------------------------------- diagnostic core
# SDK-independent library: profiler, memory tracker, snapshots, config, logger, engine.
add_library(memguardx_core STATIC
	src/core/clock.cpp
	src/core/config.cpp
	src/core/engine.cpp
	src/core/logger.cpp
	src/core/memory_analysis.cpp
	src/core/memory_tracker.cpp
	src/core/process_memory.cpp
	src/core/profiler.cpp
	src/core/snapshot.cpp
)
target_include_directories(memguardx_core PUBLIC src/core)
memguardx_warnings(memguardx_core)
find_package(Threads REQUIRED)
target_link_libraries(memguardx_core PUBLIC Threads::Threads ${CMAKE_DL_LIBS})
if(WIN32)
	target_link_libraries(memguardx_core PUBLIC psapi)
	target_compile_definitions(memguardx_core PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
endif()

# ----------------------------------------------------------------------------- instrumentation tool
# Source-to-source Pawn instrumenter (inserts MEMGUARD_FUNC into function bodies). Host tool:
# build it for the machine you compile Pawn on; it does not depend on the SDK.
option(MEMGUARDX_BUILD_INSTRUMENTER "Build the memguard_instrument tool" ON)
if(MEMGUARDX_BUILD_INSTRUMENTER OR MEMGUARDX_BUILD_TESTS)
	add_library(memguardx_instrument_lib STATIC src/instrument/pawn_instrumenter.cpp)
	target_include_directories(memguardx_instrument_lib PUBLIC src/instrument)
	memguardx_warnings(memguardx_instrument_lib)
	if(MSVC)
		set_property(TARGET memguardx_instrument_lib PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
	endif()
endif()
if(MEMGUARDX_BUILD_INSTRUMENTER)
	add_executable(memguard_instrument tools/memguard_instrument.cpp)
	target_link_libraries(memguard_instrument PRIVATE memguardx_instrument_lib)
	memguardx_warnings(memguard_instrument)
	if(MSVC)
		set_property(TARGET memguard_instrument PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
	endif()
endif()

# ----------------------------------------------------------------------------- component
if(MEMGUARDX_BUILD_COMPONENT)
	if(NOT EXISTS "${OMP_SDK_DIR}/include/sdk.hpp")
		message(FATAL_ERROR "open.mp SDK not found at '${OMP_SDK_DIR}'. Pass -DOMP_SDK_DIR=<path> or -DMEMGUARDX_BUILD_COMPONENT=OFF.")
	endif()
	if(NOT EXISTS "${OMP_SDK_DIR}/lib/glm/CMakeLists.txt")
		message(FATAL_ERROR "The SDK at '${OMP_SDK_DIR}' has no submodules. Run: git -C \"${OMP_SDK_DIR}\" submodule update --init --recursive")
	endif()

	# The SDK's glm submodule declares cmake_minimum_required(VERSION 3.2). CMake 4.x refuses that
	# unless a policy minimum is given; set it for the SDK subtree only.
	if(CMAKE_VERSION VERSION_GREATER_EQUAL "4.0" AND NOT DEFINED CMAKE_POLICY_VERSION_MINIMUM)
		set(CMAKE_POLICY_VERSION_MINIMUM 3.5)
	endif()
	add_subdirectory("${OMP_SDK_DIR}" "${CMAKE_BINARY_DIR}/omp-sdk" EXCLUDE_FROM_ALL SYSTEM)

	configure_file(src/component/version.hpp.in "${CMAKE_BINARY_DIR}/generated/version.hpp" @ONLY)

	add_library(MemGuardX SHARED
		src/component/memguard_component.cpp
		src/component/pawn_bridge.cpp
	)
	target_include_directories(MemGuardX PRIVATE "${CMAKE_BINARY_DIR}/generated")
	target_include_directories(MemGuardX SYSTEM PRIVATE third_party/pawn-amx third_party/pawn-amx/linux)
	# Same definitions the official openmultiplayer/pawn-template uses for amx.h.
	target_compile_definitions(MemGuardX PRIVATE HAVE_STDINT_H=1 PAWN_CELL_SIZE=32)
	target_link_libraries(MemGuardX PRIVATE OMP-SDK memguardx_core)
	memguardx_warnings(MemGuardX)

	# open.mp loads every shared library in components/; name it MemGuardX.dll / MemGuardX.so.
	set_target_properties(MemGuardX PROPERTIES PREFIX "" OUTPUT_NAME "MemGuardX")
	if(NOT WIN32)
		set_target_properties(MemGuardX PROPERTIES CXX_VISIBILITY_PRESET hidden VISIBILITY_INLINES_HIDDEN ON)
		# The official Linux server links libstdc++ statically, so a 32-bit libstdc++.so.6 may be
		# missing on the server host. ON links it into MemGuardX.so instead (needs a 32-bit
		# libstdc++.a, e.g. from g++-multilib).
		option(MEMGUARDX_STATIC_LIBSTDCXX "Link libstdc++/libgcc statically into MemGuardX.so" OFF)
		if(MEMGUARDX_STATIC_LIBSTDCXX)
			target_link_options(MemGuardX PRIVATE -static-libstdc++ -static-libgcc)
		endif()
	endif()
	if(MSVC)
		# Static CRT: no dependency on a particular VC++ redistributable on the server machine.
		set_property(TARGET MemGuardX memguardx_core PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
	endif()

	if(CMAKE_SIZEOF_VOID_P EQUAL 8)
		message(WARNING
			"MemGuard X: you are building a 64-bit component. Official open.mp server releases are 32-bit (x86), "
			"so this DLL/.so will NOT load into them. Use it for tests only and build the deployable component with "
			"'-A Win32' (MSVC) or -m32 (Linux).")
	endif()
endif()

# ----------------------------------------------------------------------------- tests
if(MEMGUARDX_BUILD_TESTS)
	enable_testing()
	add_executable(memguardx_tests
		tests/test_main.cpp
		tests/test_memory_tracker.cpp
		tests/test_profiler.cpp
		tests/test_engine.cpp
		tests/test_instrumenter.cpp
		tests/test_memory_analysis.cpp
		tests/test_include_api.cpp
	)
	target_link_libraries(memguardx_tests PRIVATE memguardx_core memguardx_instrument_lib)
	target_compile_definitions(memguardx_tests PRIVATE MEMGUARDX_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
	memguardx_warnings(memguardx_tests)
	if(MSVC)
		set_property(TARGET memguardx_tests PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
	endif()
	add_test(NAME memguardx_unit_tests COMMAND memguardx_tests WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")
	if(MEMGUARDX_BUILD_INSTRUMENTER)
		# End-to-end run of the CLI on the project's own Pawn sources (must parse all of them).
		add_test(NAME memguardx_instrument_cli
			COMMAND memguard_instrument --quiet --out "${CMAKE_CURRENT_BINARY_DIR}/instrumented"
				--root "${CMAKE_CURRENT_SOURCE_DIR}/pawn" --exclude Trivial_NoProf "${CMAKE_CURRENT_SOURCE_DIR}/pawn")
		set_tests_properties(memguardx_instrument_cli PROPERTIES PASS_REGULAR_EXPRESSION " 0 file\\(s\\) failed")
	endif()

	if(MEMGUARDX_BUILD_COMPONENT)
		# Loads the built component with LoadLibrary/dlopen and checks the entry point.
		# Only meaningful when the test executable and component have the same bitness.
		add_executable(memguardx_load_test tests/component_load_test.cpp)
		target_include_directories(memguardx_load_test PRIVATE "${CMAKE_BINARY_DIR}/generated")
		target_link_libraries(memguardx_load_test PRIVATE OMP-SDK ${CMAKE_DL_LIBS})
		memguardx_warnings(memguardx_load_test)
		if(MSVC)
			set_property(TARGET memguardx_load_test PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
		endif()
		add_dependencies(memguardx_load_test MemGuardX)
		add_test(NAME memguardx_component_load COMMAND memguardx_load_test "$<TARGET_FILE:MemGuardX>")
	endif()
endif()
