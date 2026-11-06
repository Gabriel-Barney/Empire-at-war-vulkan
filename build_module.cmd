@echo off
rem Build one engine module (plus core) with the project-local MSVC toolchain and run its tests.
rem Usage: build_module.cmd <module> [module list]   e.g. build_module.cmd rhi
rem The module list defaults to "core;<module>"; modules with dependencies need them listed, e.g.
rem   build_module.cmd render "core;vfs;data;jobs;galactic;assets;rhi;shaderxlat;materials;terrain;render"
rem Uses build-<module>\ so module agents can build in parallel. Logs: logs\module_<module>.txt
rem Runs the module's fast tier (ctest -LE slow); set ALAMO_TESTS=all to include the slow tests.
rem Output: a short summary (tools\lint\build_summary.py); set ALAMO_FULL_LOG=1 to also print the whole log.
setlocal EnableDelayedExpansion
cd /d "%~dp0"
if "%~1"=="" (echo usage: build_module.cmd ^<module^> [module list]& exit /b 2)
set MODULES=core;%~1
if not "%~2"=="" set MODULES=%~2
if not exist logs mkdir logs
set SLOWFILTER=-LE slow
if /i "%ALAMO_TESTS%"=="all" set SLOWFILTER=
set LOG=logs\module_%~1.txt
call toolchain\msvc\setup_x64.bat >nul
call "%~dp0tools\test_env.cmd"
call "%~dp0tools\cache_env.cmd"
rem Tests: -j 4 unless ALAMO_CTEST_JOBS is set (several agents share the machine).
if not defined ALAMO_CTEST_JOBS set ALAMO_CTEST_JOBS=4
set PATH=%~dp0toolchain\venv\Scripts;%PATH%
> %LOG% (
  cmake -S . -B build-%~1 -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_COMPILER=cl "-DALAMO_ENGINE_MODULES=!MODULES!" 2>&1
  echo configure exit !ERRORLEVEL!
  cmake --build build-%~1 2>&1
  echo build exit !ERRORLEVEL!
  rem Per-tree temp dir: tests write fixtures under fixed %TEMP% names, so concurrent builds would clobber each other.
  if not exist build-%~1\tmp mkdir build-%~1\tmp
  set "TMP=%~dp0build-%~1\tmp"
  set "TEMP=%~dp0build-%~1\tmp"
  ctest --test-dir build-%~1 --output-on-failure -j %ALAMO_CTEST_JOBS% !SLOWFILTER! -L "^%~1$" --no-tests=error 2>&1
  echo ctest exit !ERRORLEVEL!
)
echo MODULE_DONE>> %LOG%
rem Inner-loop builds are not recorded (see build_agent.cmd); set ALAMO_RECORD_BUILD=1 to record this run on the roadmap.
if "%ALAMO_RECORD_BUILD%"=="1" python roadmap-store\roadmap_store.py record-build msvc build-%~1 %LOG%
python tools\lint\build_summary.py %LOG%
echo full log: %LOG%
if defined ALAMO_FULL_LOG type %LOG%
