@echo off
rem Unattended gate run (PAF t01): build_gate.cmd (MSVC + clang + sanitizers) on this checkout, one line per run in logs\gate_history.txt.
rem Skips the run when tracked files have uncommitted edits, so a half-finished agent change never fails the gate.
rem Started by the "AlamoGateNightly" scheduled task (tools\install_gate_task.cmd) or by hand. Exit code = the gate's (0 = all pass, 0 warnings).
setlocal EnableDelayedExpansion
cd /d "%~dp0.."
if not exist logs mkdir logs
set DIRTY=
for /f "delims=" %%L in ('git status --porcelain -uno') do set DIRTY=1
for /f %%H in ('git rev-parse --short HEAD') do set SHA=%%H
if defined DIRTY (
  echo %DATE% %TIME% !SHA! skipped: uncommitted changes>> logs\gate_history.txt
  exit /b 3
)
call "%~dp0..uild_gate.cmd"
set RC=!ERRORLEVEL!
echo %DATE% %TIME% !SHA! gate exit !RC!>> logs\gate_history.txt
exit /b !RC!
