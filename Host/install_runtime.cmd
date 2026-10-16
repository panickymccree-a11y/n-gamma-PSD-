@echo off
setlocal
set "INSTALLER=%~dp0build-psd\vc_redist.x64.exe"
if not exist "%INSTALLER%" (
    echo Missing runtime installer: "%INSTALLER%"
    echo Run build_and_test.cmd first.
    exit /b 2
)

"%INSTALLER%" /install /passive /norestart
exit /b %errorlevel%
