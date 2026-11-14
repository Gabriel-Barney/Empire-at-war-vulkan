@echo off
rem cu8k t05, MOVEMENT 4.7.6 item 4: land skirmish Dagobah, you EMPIRE vs AI REBEL; the probe spawns three Dark
rem Troopers for you and runs three JET_PACK trials (moving, standing, with a position). The probe Lua arms and stops
rem the debugger trace itself (records DBGARM / DBGSTOP). Cleans up when the game exits. Arguments go to cu8h_run.py
rem (--no-launch: start the game yourself with launch_foc_cu8hprobe.cmd; --windowed; --dry-run). With
rem --non-interactive: no pause, console in out\run_cu8k_jetpack.log. Steps: README.md.
setlocal
if not exist "%~dp0out" mkdir "%~dp0out"
echo %* | find /i "--non-interactive" >nul && goto :detached
call :main %*
set "RC=%ERRORLEVEL%"
echo.
pause
exit /b %RC%
:detached
call :main %* > "%~dp0out\run_cu8k_jetpack.log" 2>&1
exit /b %ERRORLEVEL%
:main
"%~dp0..\..\..\toolchain\venv\Scripts\python.exe" -I "%~dp0cu8h_run.py" --session jetpack %*
exit /b %ERRORLEVEL%
