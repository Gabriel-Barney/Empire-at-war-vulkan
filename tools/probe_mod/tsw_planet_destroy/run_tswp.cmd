@echo off
rem TSW planet-destroy probe: builds + installs Mods\TswPlanetProbe, launches FoC, waits for it to exit, cleans up.
rem Steam must be running. Arguments are passed on (--no-launch, --non-interactive, --rate S, --game DIR).
"%~dp0..\..\..\toolchain\venv\Scripts\python.exe" -I "%~dp0run_tswp.py" %*
set "RC=%ERRORLEVEL%"
echo %* | find /i "--non-interactive" >nul && exit /b %RC%
echo.
pause
exit /b %RC%
