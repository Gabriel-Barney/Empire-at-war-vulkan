@echo off
rem G1 hop timing: list the fleet hops of a Q2 trace (read only). Usage: analyze.cmd [trace.q2t] [--from A --to B] [--csv]
rem Default trace: the retail Heart of the Maw run of 2026-10-27.
setlocal
set ROOT=%~dp0..\..\..
set TRACE=%~1
if "%TRACE%"=="" (
  set TRACE=%ROOT%\tools\probe_mod\q2_traces\out\20261027-101057-galactic-vanilla\trace.q2t
  "%ROOT%\toolchain\venv\Scripts\python.exe" -I "%~dp0analyze_hops.py" "%TRACE%"
) else (
  "%ROOT%\toolchain\venv\Scripts\python.exe" -I "%~dp0analyze_hops.py" %*
)
exit /b %ERRORLEVEL%
