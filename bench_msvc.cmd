@echo off
rem Run Phase 2 benchmarks and the headless host against the real install. Output: logs\bench.txt
rem Build first with build_msvc.cmd. Read-only on the game folder.
setlocal
cd /d "%~dp0"
if not exist logs mkdir logs
set GAME=F:\SteamLibrary\steamapps\common\Star Wars Empire at War\corruption
> logs\bench.txt (
  echo === machine: %NUMBER_OF_PROCESSORS% logical processors, %PROCESSOR_IDENTIFIER%
  echo === headless selfcheck
  build-msvc\bin\alamo_headless.exe --selfcheck 2>&1
  echo === headless: every FoC campaign through the real VFS
  build-msvc\bin\alamo_headless.exe --game "%GAME%" --all --quiet 2>&1
  echo === headless: 400-planet synthetic, 4 AIs
  build-msvc\bin\alamo_headless.exe --synthetic 400 --ais 4 2>&1
  echo === headless: 400-planet synthetic, 4 AIs, background recompute, latency 30
  build-msvc\bin\alamo_headless.exe --synthetic 400 --ais 4 --latency 30 2>&1
  echo === galactic bench
  build-msvc\bin\alamo_galactic_bench.exe 2>&1
  echo === jobs bench
  build-msvc\bin\alamo_jobs_bench.exe 2>&1
)
echo BENCH_DONE>> logs\bench.txt
