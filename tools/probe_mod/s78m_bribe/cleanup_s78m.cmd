@echo off
rem Removes Mods\S78MBribeProbe from the game (only if it carries S78M_PROBE_MARKER.txt) and restores the player's
rem profile from the newest run's snapshot (not when it was already restored; not while the game runs). Safe to repeat;
rem the same cleanup runs by itself at the end of every run. Arguments: --game DIR, --run-dir DIR, --dry-run,
rem --no-profile, --no-pause.
"%~dp0..\..\..\toolchain\venv\Scripts\python.exe" -I "%~dp0s78m_cleanup.py" %*
set "RC=%ERRORLEVEL%"
echo %* | find /i "--no-pause" >nul && exit /b %RC%
echo.
pause
exit /b %RC%
