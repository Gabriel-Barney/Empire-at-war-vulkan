@echo off
rem Pre-merge gate: MSVC build + tests, clang build + tests, then the sanitizer check. Each build records itself on the
rem roadmap (CLAUDE.md). Usually takes 4-5 minutes with a warm cache; run it before handing a change to the integrator.
rem Usage: build_gate.cmd [module list for build_asan]   Exit code 0 only when all three passed with 0 warnings.
rem Summary: logs\gate_summary.txt (also printed). Per-build logs are the usual logs\msvc_*, clang_*, asan_*.
rem Since 2026-11-06 the three stages run at the same time (tools\gate_parallel.ps1: split cores, per-stage console output in
rem logs\gate_<stage>_stdout.txt, roadmap records written at the end). ALAMO_GATE_SERIAL=1 runs them one after another as before.
rem Tests labelled "nightly" (repeat worker counts of the long determinism runs) are skipped; tools\gate_scheduled.cmd sets
rem ALAMO_TESTS=all for the nightly task, and so can you.
setlocal
cd /d "%~dp0"
if not exist logs mkdir logs
set PATH=%~dp0toolchain\venv\Scripts;%PATH%
rem Old logs go first so a build that never starts cannot pass on the last run's results. The full path is needed
rem because some shells set NoDefaultCurrentDirectoryInExePath, which stops "call build_msvc.cmd" finding the cwd.
del /q logs\msvc_configure.txt logs\msvc_build.txt logs\clang_configure.txt logs\clang_build.txt logs\asan_configure.txt logs\asan_build.txt 2>nul
if "%ALAMO_GATE_SERIAL%"=="1" (
  call "%~dp0build_msvc.cmd"
  call "%~dp0build_clang.cmd"
  call "%~dp0build_asan.cmd" %1
) else (
  powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0tools\gate_parallel.ps1" -Root "%~dp0." -AsanModules "%~1"
)
python tools\lint\gate_summary.py logs > logs\gate_summary.txt
set GATE=%ERRORLEVEL%
type logs\gate_summary.txt
if %GATE%==0 (echo GATE PASS) else (echo GATE FAIL)
exit /b %GATE%
