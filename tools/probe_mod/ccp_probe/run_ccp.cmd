@echo off
rem ccpgu t02 Capture Control Points probe: snapshots the profile, presets game type + map, builds Mods\CcpProbe,
rem starts the memory reader, launches FoC windowed, writes out\<run>\report.txt, removes the mod and restores the
rem profile. Steps: README.md. Arguments: --no-launch, --non-interactive, --map NAME, --no-preset, --game DIR.
"%~dp0..\..\..\toolchain\venv\Scripts\python.exe" -I "%~dp0ccp_run.py" %*
set "RC=%ERRORLEVEL%"
echo %* | find /i "--non-interactive" >nul && exit /b %RC%
echo.
pause
exit /b %RC%
