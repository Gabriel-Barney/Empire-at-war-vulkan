@echo off
rem Removes Mods\CU9HProbe from the game (only if it carries CU9H_PROBE_MARKER.txt). Safe to repeat; the same
rem cleanup runs by itself at the end of every run. Arguments: --game DIR, --dry-run, --no-pause.
"%~dp0..\..\..\toolchain\venv\Scripts\python.exe" -I "%~dp0cu9h_cleanup.py" %*
set "RC=%ERRORLEVEL%"
echo %* | find /i "--no-pause" >nul && exit /b %RC%
echo.
pause
exit /b %RC%
