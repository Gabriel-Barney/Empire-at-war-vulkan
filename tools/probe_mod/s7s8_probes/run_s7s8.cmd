@echo off
rem S7/S8 probe (LOB_SOLVER 9, TACTICAL_COMBAT gate details, AI_TACTICAL 7.3.3, FIGHTER_FLYING 4.10): builds
rem Mods\S7S8Probe, launches FoC windowed, records a LAND (Geonosis) and a SPACE (Kuat) skirmish, writes
rem out\<run>\report.txt and cleans up. Steps: README.md. Arguments: --no-launch, --non-interactive, --game DIR.
"%~dp0..\..\..\toolchain\venv\Scripts\python.exe" -I "%~dp0s7s8_run.py" %*
set "RC=%ERRORLEVEL%"
echo %* | find /i "--non-interactive" >nul && exit /b %RC%
echo.
pause
exit /b %RC%
