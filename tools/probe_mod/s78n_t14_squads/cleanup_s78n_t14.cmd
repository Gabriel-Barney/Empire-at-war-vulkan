@echo off
rem Removes Mods\S78NT14Probe from the game (only if it carries S78NT14_PROBE_MARKER.txt) and restores the player's
rem profile from the newest run's snapshot (not when it was already restored; not while the game runs). Safe to
rem repeat; the same cleanup runs by itself at the end of every run. Arguments: --game DIR, --run-dir DIR, --dry-run,
rem --no-profile, --no-pause.
"%~dp0..\..\..\toolchain\venv\Scripts\python.exe" -I "%~dp0s78n_t14_cleanup.py" %*
set "RC=%ERRORLEVEL%"
echo %* | find /i "--no-pause" >nul && exit /b %RC%
echo.
pause
exit /b %RC%
