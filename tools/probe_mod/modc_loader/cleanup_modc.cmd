@echo off
rem Removes Mods\ModcLoaderProbe from the game (only if it carries MODC_LOADER_PROBE_MARKER.txt). Safe to repeat;
rem the same cleanup runs by itself at the end of every run. Arguments: --game DIR, --dry-run, --no-pause.
"%~dp0..\..\..\toolchain\venv\Scripts\python.exe" -I "%~dp0modc_cleanup.py" %*
set "RC=%ERRORLEVEL%"
echo %* | find /i "--no-pause" >nul && exit /b %RC%
echo.
pause
exit /b %RC%
