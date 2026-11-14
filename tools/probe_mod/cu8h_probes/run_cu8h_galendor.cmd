@echo off
rem cu8h t01, galactic half: installs Mods\CU8HProbe with the campaign "CU8H PROBE: ENDOR GALACTIC" (Jabiim's land
rem map is _MP_LAND_ENDOR), launches FoC with it, waits for the game to exit and cleans up. The trace itself is
rem run_cu8h_trace.cmd. Steps: README.md. Arguments are passed on (--non-interactive, --no-launch, --windowed,
rem --dry-run, --game DIR).
"%~dp0..\..\..\toolchain\venv\Scripts\python.exe" -I "%~dp0cu8h_run.py" --session galendor %*
set "RC=%ERRORLEVEL%"
echo %* | find /i "--non-interactive" >nul && exit /b %RC%
echo %* | find /i "--dry-run" >nul && exit /b %RC%
echo.
pause
exit /b %RC%
