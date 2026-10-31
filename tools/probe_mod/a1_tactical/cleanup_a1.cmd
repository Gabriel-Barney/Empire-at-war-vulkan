@echo off
rem Removes Mods\A1TacticalProbe from the game (only if it carries A1_PROBE_MARKER.txt). Safe to repeat; the same
rem cleanup runs by itself at the end of every run. Arguments: --game DIR, --dry-run, --no-pause.
"%~dp0..\..\..\toolchain\venv\Scripts\python.exe" -I "%~dp0a1_cleanup.py" %*
set "RC=%ERRORLEVEL%"
echo %* | find /i "--no-pause" >nul && exit /b %RC%
echo.
pause
exit /b %RC%
