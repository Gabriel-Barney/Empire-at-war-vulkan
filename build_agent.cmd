@echo off
rem Build a module list in an agent's own tree with MSVC or clang and run ctest (parallel agents, N1 onward).
rem Usage: build_agent.cmd <msvc|clang> [tree] "<module list>|all" [ctest -R regex | all]
rem Tree (optional, WF1 t13, 2026-11-08): left out, it is "wt" (build-wt-<compiler>), the one tree per worktree that
rem CLAUDE.md asks for, so tasks stop leaving a tree per task behind. The second argument is taken as the module list
rem (tree left out) when it contains a ";", is "all", or is the last argument; so a single module with a test regex
rem needs the tree spelled out (build_agent.cmd msvc wt lua lua) or a list ("core;lua" lua).
rem Module list "all": the default ALAMO_ENGINE_MODULES of engine/CMakeLists.txt (configured with -U, like build_msvc.cmd).
rem Tests: no 4th argument = the fast tier (ctest -LE slow, tests without the "slow" label); a regex runs the matching tests,
rem slow ones included; "all" runs every test (the gate scripts always do); "changed" (2026-11-13) runs the fast-tier tests
rem of the engine modules changed against origin/dev (tools\lint\changed_tests.py), the inner loop while you work on one
rem module; "none" builds without running tests. land_on_dev.cmd still needs the whole fast tier on the merged HEAD.
rem Repeat check (2026-11-13, tools\lint\build_state.py): when the last run in this tree FAILED on exactly the same source,
rem modules and tests, it prints that run's summary again and exits 3 without building. ALAMO_RERUN=1 builds anyway.
rem Output: a short summary (tools\lint\build_summary.py); set ALAMO_FULL_LOG=1 to also print the whole log.
rem   e.g. build_agent.cmd clang "core;vfs;data;jobs;galactic;objects;anim;commands;tactical;ai;lua" lua
rem        build_agent.cmd clang n1-lua "core;vfs;data;jobs;galactic;objects;anim;commands;tactical;ai;lua" lua
rem From PowerShell use the stop-parsing token:  & .\build_agent.cmd --% msvc "core;..." lua
rem Tree: build-<tree>-<compiler>\. Log: logs\agent_<tree>_<compiler>.txt (finished when it ends with AGENT_DONE).
rem clang adds -Wconversion -Wshadow like build_clang.cmd.
setlocal EnableDelayedExpansion
cd /d "%~dp0"
if "%~2"=="" (echo usage: build_agent.cmd ^<msvc^|clang^> [tree] ^<module list^|all^> [ctest regex^|all]& exit /b 2)
set "ACOMP=%~1"
set "A2=%~2"
set "TREE_LEFT_OUT="
if "%~3"=="" set TREE_LEFT_OUT=1
if /i "!A2!"=="all" set TREE_LEFT_OUT=1
if not "!A2:;=!"=="!A2!" set TREE_LEFT_OUT=1
if defined TREE_LEFT_OUT (
  set "ATREE=wt"
  set "AMODS=%~2"
  set "ATESTS=%~3"
) else (
  set "ATREE=%~2"
  set "AMODS=%~3"
  set "ATESTS=%~4"
)
set TC=%~dp0toolchain
set B=build-!ATREE!-!ACOMP!
set LOG=logs\agent_!ATREE!_!ACOMP!.txt
if not exist logs mkdir logs
call "%TC%\msvc\setup_x64.bat" >nul
call "%~dp0tools\test_env.cmd"
call "%~dp0tools\cache_env.cmd"
set PATH=%TC%\llvm\bin;%TC%\venv\Scripts;%PATH%
if /i "!ACOMP!"=="clang" (
  set COMPILER="-DCMAKE_CXX_COMPILER=%TC%\llvm\bin\clang++.exe" "-DCMAKE_CXX_FLAGS=-Wconversion -Wshadow"
) else (
  set COMPILER=-DCMAKE_CXX_COMPILER=cl
)
set FILTER=-R "!ATESTS!"
if "!ATESTS!"=="" set FILTER=-LE slow
if /i "!ATESTS!"=="all" set FILTER=
if /i "!ATESTS!"=="changed" set FILTER=--tests-from-file "%~dp0!B!\changed_tests.txt"
if /i "!AMODS!"=="all" (set MODULES=-U ALAMO_ENGINE_MODULES) else set MODULES="-DALAMO_ENGINE_MODULES=!AMODS!"
rem Cheap check first (2026-11-12): a string literal an edit split across lines (C2001, the most common compile error)
rem fails here in a second instead of in a build. Scans the C++ files changed against origin/dev.
python -I tools\lint\check_string_literals.py --changed
if errorlevel 1 (echo build_agent: fix the literal above first; nothing was built.& exit /b 1)
rem Which code this run builds (commit, worktree, source hash) for the build record; refuses an identical failing rerun.
python -I tools\lint\build_state.py begin %B% --modules "!AMODS!" --tests "!ATESTS!" --refuse-repeat
if errorlevel 3 exit /b 3
rem Stamp for tools\land_on_dev.cmd: HEAD, clean or not, modules and test filter; becomes %B%\verified.json on a pass.
python -I tools\lint\verified_build.py begin %B% --modules "!AMODS!" --tests "!ATESTS!"
> %LOG% (
  cmake -S . -B %B% -G Ninja -DCMAKE_BUILD_TYPE=Release "-DCMAKE_MAKE_PROGRAM=%TC%\venv\Scripts\ninja.exe" !COMPILER! !MODULES! 2>&1
  echo configure exit !ERRORLEVEL!
  cmake --build %B% 2>&1
  echo build exit !ERRORLEVEL!
  rem Per-tree temp dir: tests write fixtures under fixed %TEMP% names, so concurrent builds would clobber each other.
  if not exist %B%\tmp mkdir %B%\tmp
  set "TMP=%~dp0%B%\tmp"
  set "TEMP=%~dp0%B%\tmp"
  if /i "!ATESTS!"=="changed" python -I tools\lint\changed_tests.py %B% "%~dp0%B%\changed_tests.txt" 2>&1
  if /i "!ATESTS!"=="none" (
    echo tests: not run, test filter none
  ) else (
    python -I tools\lint\test_slot.py -- ctest --test-dir %B% --output-on-failure --output-junit "%~dp0%B%\ctest_junit.xml" -j 4 !FILTER! 2>&1
    echo ctest exit !ERRORLEVEL!
    python tools\lint\ctest_skips.py "%~dp0%B%\ctest_junit.xml"
  )
)
echo AGENT_DONE>> %LOG%
rem Summary first: it adds the outcome (failure kind, golden drift) to %B%\build_state.json, which the record copies.
python tools\lint\build_summary.py --state %B%\build_state.json %LOG%
set "ARC=!ERRORLEVEL!"
set "ALAMO_BUILD_EXTRA=%~dp0%B%\build_state.json"
python roadmap-store\roadmap_store.py record-build !ACOMP! %B% %LOG%
if "!ARC!"=="0" python -I tools\lint\verified_build.py pass %B%
echo full log: %LOG%
if defined ALAMO_FULL_LOG type %LOG%
exit /b !ARC!
