@echo off
rem cu8h t01: traces the synced draws of one battle load with cu8h_dbg.py (a debugger: hardware write watchpoint on
rem the LCG state; the game is only read). Start the game yourself (for a galactic load: run_cu8h_galendor.cmd
rem first, in another window). Press Enter here right before you click Begin. Writes out\<run>\report.txt.
rem Arguments are passed on (--label NAME, --n-env N, --stop-mode-frame N, --arm-now, --non-interactive). README.md.
"%~dp0..\..\..\toolchain\venv\Scripts\python.exe" -I "%~dp0cu8h_trace.py" %*
set "RC=%ERRORLEVEL%"
echo %* | find /i "--non-interactive" >nul && exit /b %RC%
echo %* | find /i "--dry-run" >nul && exit /b %RC%
echo.
pause
exit /b %RC%
