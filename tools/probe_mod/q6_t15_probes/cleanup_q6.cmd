@echo off
rem Removes Mods\Q6T15Probe and Mods\Q6T15ProbeTR from the game (only folders that carry Q6T15_PROBE_MARKER.txt). Safe
rem to repeat; the same cleanup runs by itself at the end of every run. Arguments: --game DIR, --dry-run, --no-pause.
"%~dp0..\..\..\toolchain\venv\Scripts\python.exe" -I "%~dp0q6_cleanup.py" %*
set "RC=%ERRORLEVEL%"
echo %* | find /i "--no-pause" >nul && exit /b %RC%
echo.
pause
exit /b %RC%
