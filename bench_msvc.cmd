@echo off
rem Run Phase 2 benchmarks and the headless host against the real install. Output: logs\bench.txt
rem Build first with build_msvc.cmd. Read-only on the game folder.
setlocal
cd /d "%~dp0"
if not exist logs mkdir logs
rem ALAMO_FOC_DATA: tools\test_env.cmd defaults it to the Steam install when that folder exists.
call "%~dp0tools\test_env.cmd"
> logs\bench.txt (
  echo === machine: %NUMBER_OF_PROCESSORS% logical processors, %PROCESSOR_IDENTIFIER%
  echo === headless selfcheck
  build-msvc\bin\alamo_headless.exe --selfcheck 2>&1
  echo === headless: every FoC campaign through the real VFS
  if defined ALAMO_FOC_DATA (
    build-msvc\bin\alamo_headless.exe --game "%ALAMO_FOC_DATA%" --all --quiet 2>&1
  ) else (
    echo skipped: ALAMO_FOC_DATA not set and the default install folder is missing
  )
  echo === headless: 400-planet synthetic, 4 AIs
  build-msvc\bin\alamo_headless.exe --synthetic 400 --ais 4 2>&1
  echo === headless: 400-planet synthetic, 4 AIs, background recompute, latency 30
  build-msvc\bin\alamo_headless.exe --synthetic 400 --ais 4 --latency 30 2>&1
  echo === tick profile: summary lines, full reports in logs\bench_profile_*.txt ^(OPT1; wall-clock only, CRCs unchanged^)
)
rem OPT1 tick profiles: one run per scenario at the default worker count. Needs ALAMO_FOC_DATA (set above).
set HL=build-msvc\bin\alamo_headless.exe
%HL% --economy --galactic-ai --ticks 18000 --quiet --profile --profile-out logs\bench_profile_galactic_hotm.txt >nul 2>&1
%HL% --synthetic 400 --ais 4 --profile --profile-out logs\bench_profile_synthetic400.txt >nul 2>&1
%HL% --skirmish-ai foc-land-campaign --ticks 20000 --quiet --profile --profile-out logs\bench_profile_land.txt >nul 2>&1
%HL% --skirmish-ai foc-station --seed 3 --ticks 20000 --quiet --profile --profile-out logs\bench_profile_space.txt >nul 2>&1
>> logs\bench.txt (
  for %%S in (galactic_hotm synthetic400 land space) do (
    echo --- %%S
    if exist logs\bench_profile_%%S.txt (findstr /b /c:"=== tick profile" /c:"tick ms:" logs\bench_profile_%%S.txt) else (echo no profile written)
  )
  echo === galactic bench
  build-msvc\bin\alamo_galactic_bench.exe 2>&1
  echo === jobs bench
  build-msvc\bin\alamo_jobs_bench.exe 2>&1
)
echo BENCH_DONE>> logs\bench.txt
