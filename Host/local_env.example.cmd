@echo off
rem Copy this file to local_env.cmd and edit the paths for this computer.
rem local_env.cmd is ignored by Git.
set "QT_ROOT=C:\Qt\6.8.3\msvc2022_64"
set "CMAKE_BIN=C:\Qt\Tools\CMake_64\bin"
set "NINJA_BIN=C:\Qt\Tools\Ninja"
rem Optional when not already running in an x64 Visual Studio Developer Prompt:
rem set "VSDEVCMD=C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\Tools\VsDevCmd.bat"
rem Optional if the test executables need the matching VC runtime directory:
rem set "VC_RUNTIME=C:\Path\To\Microsoft.VC143.CRT"
