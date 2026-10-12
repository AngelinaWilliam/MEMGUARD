@echo off
rem MemGuard X - Windows build helper (MSVC).
rem
rem   tools\build_windows.bat            -> x86 (Win32) Release: the DLL the open.mp server can load
rem   tools\build_windows.bat x64        -> x64 Release: unit tests + load test only (server is 32-bit)
rem   tools\build_windows.bat x86 Debug  -> x86 Debug
rem
rem Optional: set OMP_SDK_DIR before running to use another SDK location.
setlocal EnableExtensions

set "ARCH=%~1"
if "%ARCH%"=="" set "ARCH=x86"
set "CONFIG=%~2"
if "%CONFIG%"=="" set "CONFIG=Release"
if "%OMP_SDK_DIR%"=="" set "OMP_SDK_DIR=C:/Users/PC/open.mp-sdk"

set "ROOT=%~dp0.."
set "BUILD=%ROOT%\build\%ARCH%-%CONFIG%"

set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (
    echo [MemGuardX] vswhere.exe not found. Run this from a "Native Tools Command Prompt" instead.
    goto :configure
)
for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -prerelease -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSPATH=%%i"
if "%VSPATH%"=="" (
    echo [MemGuardX] No Visual Studio with the C++ x86/x64 tools was found.
    exit /b 1
)
call "%VSPATH%\VC\Auxiliary\Build\vcvarsall.bat" %ARCH% >nul || exit /b 1

:configure
set "GEN=NMake Makefiles"
where ninja >nul 2>nul && set "GEN=Ninja"
echo [MemGuardX] Arch=%ARCH% Config=%CONFIG% Generator="%GEN%" SDK=%OMP_SDK_DIR%

cmake -S "%ROOT%" -B "%BUILD%" -G "%GEN%" -DCMAKE_BUILD_TYPE=%CONFIG% "-DOMP_SDK_DIR=%OMP_SDK_DIR%" || exit /b 1
cmake --build "%BUILD%" || exit /b 1
ctest --test-dir "%BUILD%" --output-on-failure || exit /b 1

echo.
echo [MemGuardX] Build and tests OK. Component: %BUILD%\MemGuardX.dll
endlocal
