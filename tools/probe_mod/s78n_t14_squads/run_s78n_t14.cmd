@echo off
rem s78n t14 probe (land-squad formation: member lag, start delays, Rows / Dartboard slots; FIGHTER_FLYING 7-7.4):
rem backs up the player's profile, builds Mods\S78NT14Probe from your install, launches FoC with
rem MODPATH=Mods\S78NT14Probe (or waits for you to with --no-launch), scans the game memory for the probe records,
rem writes out\<run>\report.txt when the game exits, cleans up and restores the profile. Steam must be running.
rem Steps: README.md. Arguments are passed on (--non-interactive, --no-launch, --windowed, --dry-run, --no-profile,
rem --game DIR, --map NAME, --reps N, --report-only RECORDS.log).
"%~dp0..\..\..\toolchain\venv\Scripts\python.exe" -I "%~dp0s78n_t14_run.py" %*
set "RC=%ERRORLEVEL%"
echo %* | find /i "--non-interactive" >nul && exit /b %RC%
echo %* | find /i "--dry-run" >nul && exit /b %RC%
echo.
pause
exit /b %RC%
