@echo off
rem Build all C++ tools with the project-local MSVC toolchain. Logs go to logs\.
setlocal EnableDelayedExpansion
cd /d "%~dp0"
if not exist logs mkdir logs
if not exist toolchain\msvc\setup_x64.bat (
  echo Toolchain missing: run toolchain\2_setup.cmd first.> logs\msvc_configure.txt
  echo BUILD_DONE>> logs\msvc_configure.txt
  exit /b 1
)
call toolchain\msvc\setup_x64.bat >nul
set PATH=%~dp0toolchain\venv\Scripts;%PATH%
call "%~dp0tools\test_env.cmd"
call "%~dp0tools\cache_env.cmd"
rem Stamp for tools\land_on_dev.cmd (tools\lint\verified_build.py): becomes build-msvc\verified.json on a pass.
python -I tools\lint\verified_build.py begin build-msvc --modules all --tests all
> logs\msvc_configure.txt (
  where cl.exe 2>&1
  cl.exe 2>&1
  rem -U drops a cached module list so the full build always gets every module (engine/CMakeLists.txt default).
  cmake -S . -B build-msvc -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_COMPILER=cl -U ALAMO_ENGINE_MODULES 2>&1
  echo configure exit !ERRORLEVEL!
)
> logs\msvc_build.txt (
  cmake --build build-msvc 2>&1
  echo build exit !ERRORLEVEL!
  echo === tests
  rem Per-tree temp dir: tests write fixtures under fixed %TEMP% names, so concurrent builds would clobber each other.
  if not exist build-msvc\tmp mkdir build-msvc\tmp
  set "TMP=%~dp0build-msvc\tmp"
  set "TEMP=%~dp0build-msvc\tmp"
  ctest --test-dir build-msvc --output-on-failure --output-junit "%~dp0build-msvc\ctest_junit.xml" -j 8 2>&1
  echo ctest exit !ERRORLEVEL!
  python tools\lint\ctest_skips.py "%~dp0build-msvc\ctest_junit.xml"
)
echo BUILD_DONE>> logs\msvc_configure.txt
python tools\lint\build_summary.py logs\msvc_configure.txt logs\msvc_build.txt
if not errorlevel 1 python -I tools\lint\verified_build.py pass build-msvc
python roadmap-store\roadmap_store.py record-build msvc build-msvc logs\msvc_configure.txt logs\msvc_build.txt
