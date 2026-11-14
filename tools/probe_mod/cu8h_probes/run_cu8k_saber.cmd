@echo off
rem cu8k t05 / CU8F 2.7 item 6: the cu8h "luke" session (Luke throws his saber at a target 150 units away while
rem holstered, then at one 120 units away while drawn) with the memory watcher also logging the thrown saber every
rem frame (position, travelled, max travel, flags, distance to Luke) and the throw target; report.txt derives the
rem target radius and the catch / death frames. Arguments go to cu8h_run.py (--no-launch: start the game yourself with
rem launch_foc_cu8hprobe.cmd; --windowed; --dry-run). With --non-interactive: no pause, console in
rem out\run_cu8k_saber.log. Steps: README.md.
setlocal
if not exist "%~dp0out" mkdir "%~dp0out"
echo %* | find /i "--non-interactive" >nul && goto :detached
call :main %*
set "RC=%ERRORLEVEL%"
echo.
pause
exit /b %RC%
:detached
call :main %* > "%~dp0out\run_cu8k_saber.log" 2>&1
exit /b %ERRORLEVEL%
:main
"%~dp0..\..\..\toolchain\venv\Scripts\python.exe" -I "%~dp0cu8h_run.py" --session luke %*
exit /b %ERRORLEVEL%
