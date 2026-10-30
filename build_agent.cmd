@echo off
rem Build a module list in an agent's own tree with MSVC or clang and run ctest (parallel agents, N1 onward).
rem Usage: build_agent.cmd <msvc|clang> <tree> "<module list>" [ctest -R regex]
rem   e.g. build_agent.cmd clang n1-lua "core;vfs;data;jobs;galactic;objects;anim;commands;tactical;ai;lua" lua
rem From PowerShell use the stop-parsing token:  & .\build_agent.cmd --% msvc n1-lua "core;..." lua
rem Tree: build-<tree>-<compiler>\. Log: logs\agent_<tree>_<compiler>.txt (finished when it ends with AGENT_DONE).
rem clang adds -Wconversion -Wshadow like build_clang.cmd. Without a regex every test in the tree runs.
setlocal EnableDelayedExpansion
cd /d "%~dp0"
if "%~3"=="" (echo usage: build_agent.cmd ^<msvc^|clang^> ^<tree^> "^<module list^>" [ctest regex]& exit /b 2)
set TC=%~dp0toolchain
set B=build-%~2-%~1
set LOG=logs\agent_%~2_%~1.txt
if not exist logs mkdir logs
call "%TC%\msvc\setup_x64.bat" >nul
set PATH=%TC%\llvm\bin;%TC%\venv\Scripts;%PATH%
if /i "%~1"=="clang" (
  set COMPILER="-DCMAKE_CXX_COMPILER=%TC%\llvm\bin\clang++.exe" "-DCMAKE_CXX_FLAGS=-Wconversion -Wshadow"
) else (
  set COMPILER=-DCMAKE_CXX_COMPILER=cl
)
set FILTER=
if not "%~4"=="" set FILTER=-R "%~4"
> %LOG% (
  cmake -S . -B %B% -G Ninja -DCMAKE_BUILD_TYPE=Release "-DCMAKE_MAKE_PROGRAM=%TC%\venv\Scripts\ninja.exe" !COMPILER! "-DALAMO_ENGINE_MODULES=%~3" 2>&1
  echo configure exit !ERRORLEVEL!
  cmake --build %B% 2>&1
  echo build exit !ERRORLEVEL!
  rem Per-tree temp dir: tests write fixtures under fixed %TEMP% names, so concurrent builds would clobber each other.
  if not exist %B%\tmp mkdir %B%\tmp
  set "TMP=%~dp0%B%\tmp"
  set "TEMP=%~dp0%B%\tmp"
  ctest --test-dir %B% --output-on-failure -j 4 !FILTER! 2>&1
  echo ctest exit !ERRORLEVEL!
)
echo AGENT_DONE>> %LOG%
python roadmap-store\roadmap_store.py record-build %~1 %B% %LOG%
type %LOG%
