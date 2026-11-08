@echo off
rem cu8g probes, session maps-hit: t08 second launch: the same campaign with a faction-patched _MP_LAND_ALDERAAN copy.
rem Builds Mods\CU8GProbe from your install, launches FoC with MODPATH=Mods\CU8GProbe, scans the game memory for
rem the probe records, writes out\<run>\report.txt when the game exits, cleans up. Steam must be running. Steps:
rem README.md. Arguments are passed on (--non-interactive, --no-launch, --windowed, --dry-run, --game DIR, --map-loose).
"%~dp0..\..\..\toolchain\venv\Scripts\python.exe" -I "%~dp0cu8g_run.py" --session maps-hit %*
set "RC=%ERRORLEVEL%"
echo %* | find /i "--non-interactive" >nul && exit /b %RC%
echo %* | find /i "--dry-run" >nul && exit /b %RC%
echo.
pause
exit /b %RC%
