@echo off
rem cu8k t05, MOVEMENT 4.7.6 items 1 and 3: installs Mods\CU8HProbe with the campaign "CU8K PROBE: KASHYYYK SPACE"
rem (human Rebel fleet at Corellia: Calamari Cruiser, Nebulon-B, two X-wing squadrons; Empire star base at Kashyyyk),
rem waits for the game, then runs two debugger traces one after the other: 1 the Kashyyyk space battle load (create
rem out\arm right before Begin; stops at mode frame 240), 2 one group move of the fleet (create out\arm right before
rem the right-click; stops 600 mode frames later). out\trace_status.txt says which trace waits. Cleans up when the game exits.
rem Arguments go to cu8h_run.py (--no-launch: start the game yourself with launch_foc_cu8hprobe.cmd; --windowed;
rem --dry-run). With --non-interactive: no pause, console in out\run_cu8k_kashyyyk.log. Steps: README.md.
setlocal
if not exist "%~dp0out" mkdir "%~dp0out"
echo %* | find /i "--non-interactive" >nul && goto :detached
call :main %*
set "RC=%ERRORLEVEL%"
echo.
pause
exit /b %RC%
:detached
call :main %* > "%~dp0out\run_cu8k_kashyyyk.log" 2>&1
exit /b %ERRORLEVEL%
:main
"%~dp0..\..\..\toolchain\venv\Scripts\python.exe" -I "%~dp0cu8h_run.py" --session kashyyyk %*
exit /b %ERRORLEVEL%
