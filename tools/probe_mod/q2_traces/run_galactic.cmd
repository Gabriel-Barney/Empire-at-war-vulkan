@echo off
rem Q2 galactic trace (Heart of the Maw, Empire, hands-off). Extra options are passed on, e.g. -LuaProbe, -Mod tr, -DryRun, -NonInteractive.
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0run_trace.ps1" -Scenario galactic %*
exit /b %ERRORLEVEL%
