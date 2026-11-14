@echo off
rem Removes Mods\CcpProbe from the game (only if it carries CCP_PROBE_MARKER.txt) and restores the profile saved by
rem the newest run (out\latest.txt). Safe to repeat; the same cleanup runs at the end of every run.
rem Arguments: --game DIR, --dry-run, --run-dir DIR, --no-profile, --no-pause.
"%~dp0..\..\..\toolchain\venv\Scripts\python.exe" -I "%~dp0ccp_cleanup.py" %*
set "RC=%ERRORLEVEL%"
echo %* | find /i "--no-pause" >nul && exit /b %RC%
echo.
pause
exit /b %RC%
