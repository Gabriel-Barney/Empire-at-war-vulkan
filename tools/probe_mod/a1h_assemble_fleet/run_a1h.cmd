@echo off
rem a1h t09 probe: Assemble_Fleet of ground units on a planet surface. Installs Mods\A1hAssembleProbe, launches FoC
rem with MODPATH=Mods\A1hAssembleProbe, scans the game memory for the story records, writes out\<run>\report.txt when
rem the game exits, cleans up. Steam must be running. Arguments are passed on (--non-interactive, --no-launch,
rem --windowed, --game DIR, --scan-sec S). With --non-interactive there is no pause at the end.
"%~dp0..\..\..\toolchain\venv\Scripts\python.exe" -I "%~dp0a1h_run.py" %*
set "RC=%ERRORLEVEL%"
echo %* | find /i "--non-interactive" >nul && exit /b %RC%
echo.
pause
exit /b %RC%
