@echo off
rem Removes Mods\S78NProbe from the game (only if it carries S78N_PROBE_MARKER.txt) and restores the player's profile
rem from the newest run's snapshot (not when it was already restored; not while the game runs). Safe to repeat; the
rem same cleanup runs by itself at the end of every run. Arguments: --game DIR, --run-dir DIR, --dry-run, --no-profile,
rem --no-pause.
"%~dp0..\..\..\toolchain\venv\Scripts\python.exe" -I "%~dp0s78n_cleanup.py" %*
set "RC=%ERRORLEVEL%"
echo %* | find /i "--no-pause" >nul && exit /b %RC%
echo.
pause
exit /b %RC%
