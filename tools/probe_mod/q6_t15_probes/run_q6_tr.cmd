@echo off
rem Q6 t15 probes, THRAWN'S REVENGE session (probe 5 TR land end time, probe 1 TR GCMenu by eye): builds
rem Mods\Q6T15ProbeTR (TR's LandTacticalFreeStore.lua + a read-only recorder), launches FoC with
rem MODPATH=Mods\Q6T15ProbeTR MODPATH=<TR workshop folder>, scans the game memory for the probe records, writes
rem out\<run>\report.txt when the game exits, cleans up. TR's own folder is only read. Steps: README.md. Arguments are
rem passed on (--non-interactive, --no-launch, --windowed, --game DIR, --tr DIR, --scan-sec S).
"%~dp0..\..\..\toolchain\venv\Scripts\python.exe" -I "%~dp0q6_run.py" --session tr %*
set "RC=%ERRORLEVEL%"
echo %* | find /i "--non-interactive" >nul && exit /b %RC%
echo.
pause
exit /b %RC%
