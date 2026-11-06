@echo off
rem modc t13 / t14 loader probe: builds Mods\ModcLoaderProbe from your install, launches FoC with
rem MODPATH=Mods\ModcLoaderProbe, waits until the game exits, reads which canary files the probe scripts deleted,
rem writes out\<run>\report.txt, cleans up. Steam must be running. Steps: README.md. Arguments are passed on
rem (--non-interactive, --no-launch, --windowed, --digit-listed yes/no, --game DIR).
"%~dp0..\..\..\toolchain\venv\Scripts\python.exe" -I "%~dp0modc_run.py" %*
set "RC=%ERRORLEVEL%"
echo %* | find /i "--non-interactive" >nul && exit /b %RC%
echo.
pause
exit /b %RC%
