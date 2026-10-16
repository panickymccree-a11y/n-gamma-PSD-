@echo off
setlocal
set "VSLANG=1033"
set "HOST_ROOT=%~dp0."
set "BUILD_DIR=%HOST_ROOT%\build-psd"

call "%HOST_ROOT%\configure_build_env.cmd"
if errorlevel 1 exit /b 1

call "%QT_ROOT%\bin\qt-cmake.bat" --fresh -S "%HOST_ROOT%" -B "%BUILD_DIR%" -G Ninja -DCMAKE_BUILD_TYPE=Release
if errorlevel 1 exit /b %errorlevel%

cmake.exe --build "%BUILD_DIR%" --clean-first --parallel
if errorlevel 1 exit /b %errorlevel%

"%QT_ROOT%\bin\windeployqt.exe" ^
    --release ^
    --compiler-runtime ^
    --no-translations ^
    "%BUILD_DIR%\mca_studio.exe"
if errorlevel 1 exit /b %errorlevel%

rem The UI tests run without showing windows.  qoffscreen is not discovered by
rem windeployqt from the production executable, so deploy that test platform
rem plugin explicitly before CTest starts.
if not exist "%BUILD_DIR%\platforms" mkdir "%BUILD_DIR%\platforms"
copy /y "%QT_ROOT%\plugins\platforms\qoffscreen.dll" ^
    "%BUILD_DIR%\platforms\qoffscreen.dll" >nul
if errorlevel 1 exit /b %errorlevel%

rem Use the newest installed VC143 runtime for tests.  Do not copy these DLLs
rem app-local: mixing them with the system UCRT can terminate Qt tests during
rem process startup.  windeployqt places the supported vc_redist installer in
rem the deployment directory instead.
if defined VC_RUNTIME if exist "%VC_RUNTIME%\vcruntime140.dll" set "PATH=%VC_RUNTIME%;%PATH%"

set "QT_QPA_PLATFORM=offscreen"
ctest.exe --test-dir "%BUILD_DIR%" --output-on-failure -C Release
if errorlevel 1 exit /b %errorlevel%

set "QT_QPA_PLATFORM="
echo DEPLOY_PASS: "%BUILD_DIR%\mca_studio.exe"
echo RUNTIME_INSTALLER: "%BUILD_DIR%\vc_redist.x64.exe"
exit /b 0
