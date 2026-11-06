@echo off
rem Portability check: build everything with the project-local LLVM clang++ (GNU driver, MSVC ABI, project MSVC
rem headers/libs), -Wall -Wextra -Wpedantic from the CMake files plus -Wconversion -Wshadow, then run ctest.
rem Dir build-clang\, logs logs\clang_configure.txt / logs\clang_build.txt (finished when clang_configure.txt ends with BUILD_DONE).
setlocal EnableDelayedExpansion
cd /d "%~dp0"
if not exist logs mkdir logs
set TC=%~dp0toolchain
if not exist "%TC%\llvm\bin\clang++.exe" (
  echo clang missing: run toolchain\5_portability_compilers.cmd first.> logs\clang_configure.txt
  echo BUILD_DONE>> logs\clang_configure.txt
  exit /b 1
)
call "%TC%\msvc\setup_x64.bat" >nul
set PATH=%TC%\llvm\bin;%TC%\venv\Scripts;%PATH%
set CMAKE=%TC%\venv\Scripts\cmake.exe
set CTEST=%TC%\venv\Scripts\ctest.exe
call "%~dp0tools\test_env.cmd"
call "%~dp0tools\cache_env.cmd"
rem Tests (2026-11-06): -j defaults to the logical core count (ALAMO_CTEST_JOBS overrides; the parallel gate sets it per stage). The
rem nightly-labelled repeats (engine/CMakeLists.txt) are skipped unless ALAMO_TESTS=all (tools\gate_scheduled.cmd sets it).
if not defined ALAMO_CTEST_JOBS set ALAMO_CTEST_JOBS=%NUMBER_OF_PROCESSORS%
set NIGHTLY_FILTER=-LE nightly
if /i "%ALAMO_TESTS%"=="all" set NIGHTLY_FILTER=
> logs\clang_configure.txt (
  "%TC%\llvm\bin\clang++.exe" --version 2>&1
  rem -U drops a cached module list so the full build always gets every module (engine/CMakeLists.txt default).
  "%CMAKE%" -S . -B build-clang -G Ninja -DCMAKE_BUILD_TYPE=Release "-DCMAKE_MAKE_PROGRAM=%TC%\venv\Scripts\ninja.exe" "-DCMAKE_CXX_COMPILER=%TC%\llvm\bin\clang++.exe" "-DCMAKE_CXX_FLAGS=-Wconversion -Wshadow" -U ALAMO_ENGINE_MODULES %* 2>&1
  echo configure exit !ERRORLEVEL!
)
> logs\clang_build.txt (
  "%CMAKE%" --build build-clang 2>&1
  echo build exit !ERRORLEVEL!
  echo === tests
  rem Per-tree temp dir: tests write fixtures under fixed %TEMP% names, so concurrent builds would clobber each other.
  if not exist build-clang\tmp mkdir build-clang\tmp
  set "TMP=%~dp0build-clang\tmp"
  set "TEMP=%~dp0build-clang\tmp"
  "%CTEST%" --test-dir build-clang --output-on-failure --output-junit "%~dp0build-clang\ctest_junit.xml" -j %ALAMO_CTEST_JOBS% %NIGHTLY_FILTER% 2>&1
  echo ctest exit !ERRORLEVEL!
  python tools\lint\ctest_skips.py "%~dp0build-clang\ctest_junit.xml"
)
echo BUILD_DONE>> logs\clang_configure.txt
python tools\lint\build_summary.py logs\clang_configure.txt logs\clang_build.txt
rem The parallel gate (build_gate.cmd) sets ALAMO_DEFER_RECORD and records the stages itself, one after another.
if not defined ALAMO_DEFER_RECORD python roadmap-store\roadmap_store.py record-build clang build-clang logs\clang_configure.txt logs\clang_build.txt
