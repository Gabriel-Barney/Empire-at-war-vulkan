@echo off
rem Parity reference capture (q4d): several hands-off retail runs per Q4 parity scenario. Start Steam first, then leave
rem the game alone. Options are passed on, e.g. -Scenario skirmish -Runs 3, -NoLuaProbe, -DryRun, -KeepGoing. README.md.
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0run_capture.ps1" %*
set RC=%ERRORLEVEL%
if not "%PARITY_REFS_NOPAUSE%"=="1" pause
exit /b %RC%
