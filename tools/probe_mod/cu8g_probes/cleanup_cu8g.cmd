@echo off
rem Removes Mods\CU8GProbe from the game (only if it carries CU8G_PROBE_MARKER.txt) and the loose
rem corruption\Data\Art\Maps\_MP_LAND_CU8G_ALDERAAN.TED of maps-hit --map-loose (only with its marker and the same
rem content). Safe to repeat; the same cleanup runs by itself at the end of every run. Arguments: --game DIR,
rem --dry-run, --no-pause.
"%~dp0..\..\..\toolchain\venv\Scripts\python.exe" -I "%~dp0cu8g_cleanup.py" %*
set "RC=%ERRORLEVEL%"
echo %* | find /i "--no-pause" >nul && exit /b %RC%
echo.
pause
exit /b %RC%
