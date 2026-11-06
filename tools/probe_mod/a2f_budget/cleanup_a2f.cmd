@echo off
rem Removes Mods\A2fBudgetProbe from the game (only if it carries A2FB_PROBE_MARKER.txt) and restores the player's profile
rem from the newest run's snapshot (not when it was already restored; not while the game runs). Safe to repeat; the same
rem cleanup runs by itself at the end of every run. Arguments: --game DIR, --run-dir DIR, --dry-run, --no-profile, --no-pause.
"%~dp0..\..\..\toolchain\venv\Scripts\python.exe" -I "%~dp0a2f_cleanup.py" %*
set "RC=%ERRORLEVEL%"
echo %* | find /i "--no-pause" >nul && exit /b %RC%
echo.
pause
exit /b %RC%
