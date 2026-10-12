# CMake generated Testfile for 
# Source directory: C:/Users/PC/MemGuardX
# Build directory: C:/Users/PC/MemGuardX/build/x86-Release
# 
# This file includes the relevant testing commands required for 
# testing this directory and lists subdirectories to be tested as well.
add_test([=[memguardx_unit_tests]=] "C:/Users/PC/MemGuardX/build/x86-Release/memguardx_tests.exe")
set_tests_properties([=[memguardx_unit_tests]=] PROPERTIES  WORKING_DIRECTORY "C:/Users/PC/MemGuardX/build/x86-Release" _BACKTRACE_TRIPLES "C:/Users/PC/MemGuardX/CMakeLists.txt;147;add_test;C:/Users/PC/MemGuardX/CMakeLists.txt;0;")
add_test([=[memguardx_instrument_cli]=] "C:/Users/PC/MemGuardX/build/x86-Release/memguard_instrument.exe" "--quiet" "--out" "C:/Users/PC/MemGuardX/build/x86-Release/instrumented" "--root" "C:/Users/PC/MemGuardX/pawn" "--exclude" "Trivial_NoProf" "C:/Users/PC/MemGuardX/pawn")
set_tests_properties([=[memguardx_instrument_cli]=] PROPERTIES  PASS_REGULAR_EXPRESSION " 0 file\\(s\\) failed" _BACKTRACE_TRIPLES "C:/Users/PC/MemGuardX/CMakeLists.txt;150;add_test;C:/Users/PC/MemGuardX/CMakeLists.txt;0;")
add_test([=[memguardx_component_load]=] "C:/Users/PC/MemGuardX/build/x86-Release/memguardx_load_test.exe" "C:/Users/PC/MemGuardX/build/x86-Release/MemGuardX.dll")
set_tests_properties([=[memguardx_component_load]=] PROPERTIES  _BACKTRACE_TRIPLES "C:/Users/PC/MemGuardX/CMakeLists.txt;167;add_test;C:/Users/PC/MemGuardX/CMakeLists.txt;0;")
subdirs("omp-sdk")
