@echo off
rem BM1: run a benchmark scenario in sim-only mode (1 warm-up + 3 timed runs) and write its JSON result to logs\.
rem   tools\benchmark\run_bench.cmd [name | path\to\scenario.abench] [more alamo_headless --bench options]
rem e.g. tools\benchmark\run_bench.cmd space_battle --bench-baseline logs\bench_space_battle_before.json
rem Default scenario: tests\benchmark\space_battle.abench; result: logs\bench_<name>.json. Close other builds and tests first:
rem the result is flagged "noisy" when other processes take more than 10% of the CPU during a timed run.
rem Exit 3 = "scenario changed, not comparable": regenerate with tools\benchmark\regen_scenario.cmd.
setlocal
cd /d "%~dp0..\.."
call "%CD%\tools\test_env.cmd"
set "SC=%~1"
if "%SC%"=="" set "SC=space_battle"
if /i not "%SC:~-7%"==".abench" set "SC=tests\benchmark\%SC%.abench"
for %%F in ("%SC%") do set "NAME=%%~nF"
set "EXE="
for %%B in (build-msvc build-wt-msvc) do if not defined EXE if exist "%%B\bin\alamo_headless.exe" set "EXE=%%B\bin\alamo_headless.exe"
if not defined EXE (echo run_bench: no alamo_headless.exe; build first ^(build_msvc.cmd^)& exit /b 2)
if not exist logs mkdir logs
set "REST="
:collect
shift
if "%~1"=="" goto run
set REST=%REST% %1
goto collect
:run
"%EXE%" --bench "%SC%" --bench-json "logs\bench_%NAME%.json" %REST%
set RC=%ERRORLEVEL%
echo run_bench exit %RC% (result: logs\bench_%NAME%.json)
exit /b %RC%
