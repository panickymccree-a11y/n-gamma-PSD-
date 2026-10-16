@echo off
setlocal
call "%~dp0configure_build_env.cmd"
if errorlevel 1 exit /b 1
set "VSLANG=1033"
cmake -S "%~dp0." -B "%~dp0build-psd" -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH="%QT_ROOT%"
if errorlevel 1 exit /b %errorlevel%
cmake --build "%~dp0build-psd" --parallel 8
if errorlevel 1 exit /b %errorlevel%
set "QT_QPA_PLATFORM=offscreen"
ctest --test-dir "%~dp0build-psd" --output-on-failure -C Release
exit /b %errorlevel%
