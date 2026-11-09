@echo off
rem R1 hardpoint probe (roadmap r1 t01): space skirmish Kuat, you EMPIRE vs AI REBEL. Builds Mods\R1HardpointProbe
rem from your install, launches FoC with MODPATH=Mods\R1HardpointProbe, scans the game memory for the probe
rem records, writes out\<run>\report.txt when the game exits, cleans up. Steam must be running. Steps: README.md.
rem Arguments are passed on (--non-interactive, --no-launch, --windowed, --dry-run, --game DIR, --report-only F).
"%~dp0..\..\..\toolchain\venv\Scripts\python.exe" -I "%~dp0r1_run.py" %*
set "RC=%ERRORLEVEL%"
echo %* | find /i "--non-interactive" >nul && exit /b %RC%
echo %* | find /i "--dry-run" >nul && exit /b %RC%
echo.
pause
exit /b %RC%
