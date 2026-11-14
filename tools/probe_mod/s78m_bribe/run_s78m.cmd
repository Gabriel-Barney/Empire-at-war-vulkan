@echo off
rem s78m t06 bribe probe: backs up the player's profile, builds Mods\S78MBribeProbe from your install, launches FoC with
rem MODPATH=Mods\S78MBribeProbe (or waits for you to with --no-launch), scans the game memory for the probe records,
rem writes out\<run>\report.txt when the game exits, cleans up and restores the profile. Steam must be running.
rem Steps: README.md. Arguments are passed on (--non-interactive, --no-launch, --windowed, --no-manual, --dry-run, --game DIR).
"%~dp0..\..\..\toolchain\venv\Scripts\python.exe" -I "%~dp0s78m_run.py" %*
set "RC=%ERRORLEVEL%"
echo %* | find /i "--non-interactive" >nul && exit /b %RC%
echo %* | find /i "--dry-run" >nul && exit /b %RC%
echo.
pause
exit /b %RC%
