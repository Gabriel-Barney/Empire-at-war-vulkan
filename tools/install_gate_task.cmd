@echo off
rem Registers (or replaces) a per-user nightly Task Scheduler entry that runs tools\gate_scheduled.cmd on this checkout.
rem No admin needed; it runs while you are logged on. Usage: tools\install_gate_task.cmd [HH:MM]   default 03:00.
rem Remove it with: schtasks /Delete /TN AlamoGateNightly /F
setlocal
set T=%~1
if "%T%"=="" set T=03:00
schtasks /Create /F /SC DAILY /ST %T% /TN AlamoGateNightly /TR "\"%~dp0gate_scheduled.cmd\""
exit /b %ERRORLEVEL%
