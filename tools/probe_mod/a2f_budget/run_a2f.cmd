@echo off
rem a2f t07 budget probe (AI budget reset flag +0x74, pre-Init pools / template state, Heart of the Maw): backs up the
rem player's profile, builds Mods\A2fBudgetProbe, launches FoC, records the budget through the probe Lua and a read-only
rem memory watcher, writes out\<run>\report.txt, cleans up and restores the profile. Steam must be running. Steps: README.md.
rem Arguments are passed on (--non-interactive, --no-launch, --windowed, --no-mod-equations, --no-percept, --game DIR, --scan-sec S).
"%~dp0..\..\..\toolchain\venv\Scripts\python.exe" -I "%~dp0a2f_run.py" %*
set "RC=%ERRORLEVEL%"
echo %* | find /i "--non-interactive" >nul && exit /b %RC%
echo.
pause
exit /b %RC%
