@echo off
rem storyb t07 / t08: synced-draw trace (cu8h_dbg.py, a debugger) of the Underworld story campaign start: the seed and
rem the galactic start, the Kessel UM01 land battle's map load (retail 919 + 111 draws at battle frames 1-2, t07) and
rem 10 minutes of that battle (about 2.6 draws per frame from frame 256 on, t08), with the call site of every draw.
rem Vanilla game, no mod. The debugger attaches to the running StarWarsG.exe: start the game yourself, before or
rem after this script (launch_foc_vanilla.cmd). Arm: create out\arm right before Start Game on the campaign
rem difficulty screen. It stops by itself at battle mode frame 18010, when out\stop appears or when the game exits.
rem Writes out\<yyyymmdd-hhmmss>-trace-storyb-kessel\ (trace.log, report.txt, sites.txt). With --non-interactive:
rem no pause, console in out\run_storyb_kessel.log. Other arguments go to cu8h_trace.py. Steps: README.md.
setlocal
if not exist "%~dp0out" mkdir "%~dp0out"
echo %* | find /i "--non-interactive" >nul && goto :detached
call :main %*
set "RC=%ERRORLEVEL%"
echo.
pause
exit /b %RC%
:detached
call :main %* > "%~dp0out\run_storyb_kessel.log" 2>&1
exit /b %ERRORLEVEL%
:main
"%~dp0..\..\..\toolchain\venv\Scripts\python.exe" -I "%~dp0cu8h_trace.py" --label storyb-kessel --arm-file "%~dp0out\arm" --stop-file "%~dp0out\stop" --stop-mode-frame 18010 --wait-process-sec 1800 --sites %*
exit /b %ERRORLEVEL%
