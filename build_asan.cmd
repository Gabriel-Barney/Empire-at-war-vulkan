@echo off
rem Sanitizer check: build with the project-local LLVM clang++ plus -fsanitize=%ALAMO_SANITIZE% and run ctest.
rem Required for changes to engine/jobs and other threading code (CLAUDE.md). Default is UBSan; see ALAMO_SANITIZE below.
rem TSan has no Windows runtime in LLVM 20, so races are covered by the per-worker-count determinism tests instead.
rem Usage: build_asan.cmd [module list]   default "core;jobs"; e.g. build_asan.cmd "core;vfs;data;jobs;galactic"
rem Dir build-asan\, logs logs\asan_configure.txt / logs\asan_build.txt (finished when asan_configure.txt ends with BUILD_DONE).
setlocal EnableDelayedExpansion
cd /d "%~dp0"
if not exist logs mkdir logs
set TC=%~dp0toolchain
set MODULES=core;jobs
if not "%~1"=="" set MODULES=%~1
if not exist "%TC%\llvm\bin\clang++.exe" (
  echo clang missing: run toolchain\5_portability_compilers.cmd first.> logs\asan_configure.txt
  echo BUILD_DONE>> logs\asan_configure.txt
  exit /b 1
)
call "%TC%\msvc\setup_x64.bat" >nul
set PATH=%TC%\llvm\bin;%TC%\llvm\lib\clang\20\lib\windows;%TC%\venv\Scripts;%PATH%
set CMAKE=%TC%\venv\Scripts\cmake.exe
set CTEST=%TC%\venv\Scripts\ctest.exe
call "%~dp0tools\test_env.cmd"
rem ALAMO_SANITIZE picks the sanitizers (default undefined). With address on, clang 20 (MSVC ABI) crashes in a minimal
rem program that catches a thrown exception, and address,undefined makes UBSan report a false "member call on null
rem pointer" in engine/jobs/tests/test_jobs.cpp:196, so ASan is opt-in: set ALAMO_SANITIZE=address for tests that do not throw.
if not defined ALAMO_SANITIZE set ALAMO_SANITIZE=undefined
set SAN=-fsanitize=%ALAMO_SANITIZE% -fno-sanitize-recover=undefined -fno-omit-frame-pointer -g
> logs\asan_configure.txt (
  "%TC%\llvm\bin\clang++.exe" --version 2>&1
  "%CMAKE%" -S . -B build-asan -G Ninja -DCMAKE_BUILD_TYPE=Release "-DCMAKE_MAKE_PROGRAM=%TC%\venv\Scripts\ninja.exe" "-DCMAKE_CXX_COMPILER=%TC%\llvm\bin\clang++.exe" "-DCMAKE_CXX_FLAGS=-Wconversion -Wshadow !SAN!" "-DCMAKE_EXE_LINKER_FLAGS=!SAN! -Wl,/IGNORE:4217" "-DALAMO_ENGINE_MODULES=!MODULES!" 2>&1
  echo configure exit !ERRORLEVEL!
)
> logs\asan_build.txt (
  "%CMAKE%" --build build-asan 2>&1
  echo build exit !ERRORLEVEL!
  echo === tests
  rem Per-tree temp dir, as in build_clang.cmd.
  if not exist build-asan\tmp mkdir build-asan\tmp
  set "TMP=%~dp0build-asan\tmp"
  set "TEMP=%~dp0build-asan\tmp"
  "%CTEST%" --test-dir build-asan --output-on-failure --output-junit "%~dp0build-asan\ctest_junit.xml" -j 4 2>&1
  echo ctest exit !ERRORLEVEL!
  python tools\lint\ctest_skips.py "%~dp0build-asan\ctest_junit.xml"
)
echo BUILD_DONE>> logs\asan_configure.txt
python roadmap-store\roadmap_store.py record-build clang-asan build-asan logs\asan_configure.txt logs\asan_build.txt
