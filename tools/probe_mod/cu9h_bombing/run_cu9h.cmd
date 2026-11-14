@echo off
rem cu9h t02 bombing-run probe (HUD_GAPS sec. 9.9): installs Mods\CU9HProbe (campaign "CU9H PROBE: BOMBING RUN"),
rem launches FoC with it unless --no-launch, watches the bombing-run state (reader + watchpoint debugger) until the
rem game exits, writes out\<run>\report.txt and cleans up. Steps: README.md. Arguments are passed on
rem (--non-interactive, --no-launch, --windowed, --dry-run, --target tatooine or jabiim, --interval-sec N, --game DIR).
"%~dp0..\..\..\toolchain\venv\Scripts\python.exe" -I "%~dp0cu9h_run.py" %*
set "RC=%ERRORLEVEL%"
echo %* | find /i "--non-interactive" >nul && exit /b %RC%
echo %* | find /i "--dry-run" >nul && exit /b %RC%
echo.
pause
exit /b %RC%
