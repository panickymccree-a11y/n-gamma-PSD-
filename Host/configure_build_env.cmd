@echo off
rem Shared local toolchain setup for Host build scripts.
if exist "%~dp0local_env.cmd" call "%~dp0local_env.cmd"

if not defined QT_ROOT (
    echo Set QT_ROOT to the Qt 6.8 MSVC 2022 kit directory.
    echo See local_env.example.cmd and Docs\BUILD.md.
    exit /b 2
)
if not exist "%QT_ROOT%\bin\qt-cmake.bat" (
    echo Cannot find "%QT_ROOT%\bin\qt-cmake.bat".
    exit /b 2
)

if not defined VSCMD_VER if defined VSDEVCMD (
    if not exist "%VSDEVCMD%" (
        echo Cannot find VSDEVCMD: "%VSDEVCMD%".
        exit /b 2
    )
    call "%VSDEVCMD%" -arch=x64 -host_arch=x64
    if errorlevel 1 exit /b 1
)
if not defined VSCMD_VER (
    echo Run this script from an x64 Visual Studio Developer Prompt or set VSDEVCMD in local_env.cmd.
    exit /b 2
)

if defined CMAKE_BIN set "PATH=%CMAKE_BIN%;%PATH%"
if defined NINJA_BIN set "PATH=%NINJA_BIN%;%PATH%"
set "PATH=%QT_ROOT%\bin;%PATH%"
set "CMAKE_PREFIX_PATH=%QT_ROOT%"
where cmake.exe >nul 2>nul
if errorlevel 1 (
    echo CMake was not found on PATH. Set CMAKE_BIN in local_env.cmd.
    exit /b 2
)
where ninja.exe >nul 2>nul
if errorlevel 1 (
    echo Ninja was not found on PATH. Set NINJA_BIN in local_env.cmd.
    exit /b 2
)
exit /b 0
