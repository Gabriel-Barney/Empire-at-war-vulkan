@echo off
rem Q2 skirmish trace (space, 2 players, hands-off). Extra options are passed on, e.g. -Mod tr, -DryRun, -NonInteractive, -Label <map>.
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0run_trace.ps1" -Scenario skirmish %*
exit /b %ERRORLEVEL%
