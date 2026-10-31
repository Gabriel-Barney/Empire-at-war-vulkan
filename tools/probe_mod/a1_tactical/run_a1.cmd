@echo off
rem Both A1 probes in one game session (recommended): skirmish land + space, then the campaign invasion.
rem Installs Mods\A1TacticalProbe, starts the memory reader (a1_reader.py), launches FoC with MODPATH=Mods\A1TacticalProbe,
rem waits for the game to exit, writes out\<run>\report.txt, cleans up. Steam must be running. Arguments are passed on
rem (--non-interactive, --no-launch, --game DIR, --scan-sec S). With --non-interactive there is no pause at the end.
"%~dp0..\..\..\toolchain\venv\Scripts\python.exe" -I "%~dp0a1_run.py" --probe both %*
set "RC=%ERRORLEVEL%"
echo %* | find /i "--non-interactive" >nul && exit /b %RC%
echo.
pause
exit /b %RC%
