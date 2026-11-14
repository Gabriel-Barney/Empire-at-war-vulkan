@echo off
rem SP5 t07: the services stress scene, four times land_battle's forces (docs/SIM_PARALLEL_DESIGN.md section 7: "a large-scene
rem stress (four times the fleets)"), without the tactical AI (its land population cap would keep the extra
rem companies in reserve; the stand-in deploys them all). Its save is ~4x land_battle's and over the repository's file-size limit, so it is
rem made on demand into logs\bench_stress\ (ignored) instead of being checked in, then run like any bench scene.
rem   tools\benchmark\stress_land4.cmd [workers] [more alamo_headless --bench options]
rem e.g. tools\benchmark\stress_land4.cmd 0 --runs 3          (W0)
rem      tools\benchmark\stress_land4.cmd 7 --runs 3          (W7)
rem Result: logs\bench_stress\land_stress4_w<workers>.json; compare the svc.* / tac.* zones (services) between worker counts
rem within one interleaved run, and record the machine load next to the numbers (other lanes' builds skew them).
setlocal
cd /d "%~dp0..\.."
call "%CD%\tools\test_env.cmd"
set "W=%~1"
if "%W%"=="" set "W=0"
set "EXE="
for %%B in (build-wt-msvc build-msvc) do if not defined EXE if exist "%%B\bin\alamo_headless.exe" set "EXE=%%B\bin\alamo_headless.exe"
if not defined EXE (echo stress_land4: no alamo_headless.exe; build first& exit /b 2)
set "SC=logs\bench_stress\land_stress4.abench"
if not exist logs\bench_stress mkdir logs\bench_stress
if not exist "%SC%" (
  echo stress_land4: making %SC% ^(once^)
  "%EXE%" --bench-make "%SC%" --save-session skirmish --campaign foc-land-army4 --bench-save-at 300 --ticks 1200 --bench-every 16 --bench-desc "SP5 services stress: FoC _MP_LAND_UTAPAU skirmish, Empire vs Rebel with 24 times the foc-land company lists (four times land_battle), saved at tick 300, then 40 s of battle"
  if errorlevel 1 (echo stress_land4: --bench-make failed& exit /b 1)
)
set "REST="
:collect
shift
if "%~1"=="" goto run
set REST=%REST% %1
goto collect
:run
"%EXE%" --bench "%SC%" --workers %W% --bench-json "logs\bench_stress\land_stress4_w%W%.json" %REST%
set RC=%ERRORLEVEL%
echo stress_land4 exit %RC% (result: logs\bench_stress\land_stress4_w%W%.json)
exit /b %RC%
