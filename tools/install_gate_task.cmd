@echo off
rem Registers (or replaces) a per-user nightly Task Scheduler entry that gates origin/dev (tools\gate_scheduled.cmd).
rem No admin needed; it runs while you are logged on. Usage: tools\install_gate_task.cmd [HH:MM]   default 03:00.
rem It copies gate_scheduled.cmd to %LOCALAPPDATA%\AlamoGate\ (so a checkout or merge never changes the script while it
rem runs) and registers that copy with the main checkout of this repository as its argument. Run it from any checkout;
rem rerun it after gate_scheduled.cmd changes.
rem Remove it with: schtasks /Delete /TN AlamoGateNightly /F
setlocal
set T=%~1
if "%T%"=="" set T=03:00
set "GCD="
for /f "delims=" %%G in ('git -C "%~dp0.." rev-parse --path-format=absolute --git-common-dir') do set "GCD=%%G"
if not defined GCD (echo install_gate_task: not inside a checkout of the repository.& exit /b 2)
for %%I in ("%GCD%\..") do set "MAIN=%%~fI"
set "DEST=%LOCALAPPDATA%\AlamoGate"
if not exist "%DEST%" mkdir "%DEST%"
copy /y "%~dp0gate_scheduled.cmd" "%DEST%\gate_scheduled.cmd" >nul
if errorlevel 1 (echo install_gate_task: copy to %DEST% failed.& exit /b 1)
schtasks /Create /F /SC DAILY /ST %T% /TN AlamoGateNightly /TR "\"%DEST%\gate_scheduled.cmd\" \"%MAIN%\""
exit /b %ERRORLEVEL%
