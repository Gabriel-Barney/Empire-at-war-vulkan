@echo off
rem s78n t08 probe (tractor-beam footprint range + Combat_Bonus aura on an allied player): backs up the player's profile,
rem builds Mods\S78NProbe from your install, launches FoC with MODPATH=Mods\S78NProbe (or waits for you to with
rem --no-launch), scans the game memory for the probe records, writes out\<run>\report.txt when the game exits, cleans
rem up and restores the profile. Steam must be running. Steps: README.md. Arguments are passed on (--non-interactive,
rem --no-launch, --windowed, --dry-run, --no-profile, --game DIR, --report-only RECORDS.log).
"%~dp0..\..\..\toolchain\venv\Scripts\python.exe" -I "%~dp0s78n_run.py" %*
set "RC=%ERRORLEVEL%"
echo %* | find /i "--non-interactive" >nul && exit /b %RC%
echo %* | find /i "--dry-run" >nul && exit /b %RC%
echo.
pause
exit /b %RC%
