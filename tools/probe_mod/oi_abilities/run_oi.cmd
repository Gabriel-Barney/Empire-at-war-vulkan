@echo off
rem OI ability probe (ABILITIES_HARDPOINTS 4.12 / 4.13): builds Mods\OIAbilityProbe, launches FoC windowed, records,
rem writes out\<run>\report.txt and cleans up. Steps: README.md. Arguments: --no-launch, --non-interactive, --game DIR.
"%~dp0..\..\..\toolchain\venv\Scripts\python.exe" -I "%~dp0oi_run.py" %*
set "RC=%ERRORLEVEL%"
echo %* | find /i "--non-interactive" >nul && exit /b %RC%
echo.
pause
exit /b %RC%
