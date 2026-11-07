@echo off
rem Pre-merge gate: MSVC build + tests, clang build + tests, then the sanitizer check. Each build records itself on the
rem roadmap (CLAUDE.md). Takes about 10 minutes; run it before handing a change to the integrator.
rem Usage: build_gate.cmd [module list for build_asan]   Exit code 0 only when all three passed with 0 warnings.
rem Summary: logs\gate_summary.txt (also printed). Per-build logs are the usual logs\msvc_*, clang_*, asan_*.
setlocal
cd /d "%~dp0"
if not exist logs mkdir logs
set PATH=%~dp0toolchain\venv\Scripts;%PATH%
rem Old logs go first so a build that never starts cannot pass on the last run's results. The full path is needed
rem because some shells set NoDefaultCurrentDirectoryInExePath, which stops "call build_msvc.cmd" finding the cwd.
del /q logs\msvc_configure.txt logs\msvc_build.txt logs\clang_configure.txt logs\clang_build.txt logs\asan_configure.txt logs\asan_build.txt 2>nul
call "%~dp0build_msvc.cmd"
call "%~dp0build_clang.cmd"
call "%~dp0build_asan.cmd" %1
python tools\lint\gate_summary.py logs > logs\gate_summary.txt
set GATE=%ERRORLEVEL%
type logs\gate_summary.txt
rem Files the release would add to main must pass the tracked-file check too (tools\lint\check_tracked.py).
git rev-parse -q --verify origin/main >nul 2>&1
if errorlevel 1 (
  echo tracked-file check skipped: no origin/main ref, run "git fetch origin" first.
) else (
  python -I tools\lint\check_tracked.py --base origin/main --head HEAD
  if errorlevel 1 set GATE=1
)
if %GATE%==0 (echo GATE PASS) else (echo GATE FAIL)
exit /b %GATE%
