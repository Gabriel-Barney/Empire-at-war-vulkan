@echo off
rem Screenshot check (PAF t09): compares the PNGs the game smoke tests write (build-msvc\smoke_out and smoke_out\screens) with the
rem downscaled baselines in tests\visual_baselines (320 px wide, see tools\visual\png_diff.py). Exit 0 = all match.
rem Usage: tools\visual\visual_check.cmd [run|update]
rem   (none)  compare the PNGs of the last build_msvc.cmd run (it runs the smoke tests)
rem   run     run the smoke tests first (needs the game data and a GPU), then compare
rem   update  store the current PNGs as the new baselines (after an intended visual change; commit tests\visual_baselines)
rem Mismatches get a diff image in logs\visual_diff\<set>\ (new image dimmed, differing pixels red); summary in logs\visual_check.txt.
setlocal EnableDelayedExpansion
cd /d "%~dp0..\.."
if not exist logs mkdir logs
set TC=%CD%\toolchain
set PD=python -I tools\visual\png_diff.py
set SCALE=320
rem Shots whose content depends on wall-clock timing (differ between two runs of the same build): not compared. The save /
rem load smoke's shots list saves with their wall-clock save time. The video options shots list this PC's display modes
rem (resolution / refresh rate rows), which change with the monitor setup.
rem They have no baselines, and update skips them too.
set IGN=--ignore game_campaign_galactic_after.png --ignore game_campaign_video_options.png --ignore "game_smoke_saveload_*.png" --ignore game_campaign_screen_video.png
if /i "%~1"=="update" (
  %PD% update tests\visual_baselines\smoke build-msvc\smoke_out --scale %SCALE% %IGN%
  %PD% update tests\visual_baselines\smoke_screens build-msvc\smoke_out\screens --scale %SCALE% %IGN%
  exit /b !ERRORLEVEL!
)
if /i "%~1"=="run" (
  call tools\test_env.cmd
  if not exist build-msvc\tmp mkdir build-msvc\tmp
  set "TMP=%CD%\build-msvc\tmp"
  set "TEMP=%CD%\build-msvc\tmp"
  "%TC%\venv\Scripts\ctest.exe" --test-dir build-msvc -R "alamo_game_smoke_(skirmish|campaign|land|screens)$" -j 4 --output-on-failure
  if errorlevel 1 exit /b 1
)
set RC=0
> logs\visual_check.txt (
  %PD% compare tests\visual_baselines\smoke build-msvc\smoke_out --scale %SCALE% --diff-dir logs\visual_diff\smoke --only "game_*.png" %IGN%
  if errorlevel 1 set RC=1
  %PD% compare tests\visual_baselines\smoke_screens build-msvc\smoke_out\screens --scale %SCALE% --diff-dir logs\visual_diff\smoke_screens --only "game_*.png" %IGN%
  if errorlevel 1 set RC=1
)
type logs\visual_check.txt | findstr /v /b "ok "
exit /b %RC%
