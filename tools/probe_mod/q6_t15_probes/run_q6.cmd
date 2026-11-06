@echo off
rem Q6 t15 probes, VANILLA session (probes 1 EliminationLatch, 2 object-script first pump, 3 Perpetual STORY_ELAPSED,
rem 4 load-time story resume, 6 Select All): builds Mods\Q6T15Probe from your install, launches FoC with
rem MODPATH=Mods\Q6T15Probe, scans the game memory for the probe records, writes out\<run>\report.txt when the game
rem exits, cleans up. Steam must be running. Steps: README.md. Arguments are passed on (--non-interactive,
rem --no-launch, --windowed, --game DIR, --scan-sec S). With --non-interactive there is no pause at the end.
"%~dp0..\..\..\toolchain\venv\Scripts\python.exe" -I "%~dp0q6_run.py" --session vanilla %*
set "RC=%ERRORLEVEL%"
echo %* | find /i "--non-interactive" >nul && exit /b %RC%
echo.
pause
exit /b %RC%
