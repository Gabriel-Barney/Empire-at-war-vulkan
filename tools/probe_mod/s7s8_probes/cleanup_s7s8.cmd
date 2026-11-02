@echo off
rem Removes Mods\S7S8Probe from the game (only if it carries S7S8_PROBE_MARKER.txt). Safe to repeat; the same
rem cleanup runs by itself at the end of every run. Arguments: --game DIR, --dry-run, --no-pause.
"%~dp0..\..\..\toolchain\venv\Scripts\python.exe" -I "%~dp0s7s8_cleanup.py" %*
set "RC=%ERRORLEVEL%"
echo %* | find /i "--no-pause" >nul && exit /b %RC%
echo.
pause
exit /b %RC%
