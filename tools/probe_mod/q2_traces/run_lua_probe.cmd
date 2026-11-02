@echo off
rem Q2 Lua probe (q2f t06): galactic trace of Heart of the Maw with the Lua probe installed. Extra options are passed on, e.g. -NonInteractive, -DryRun.
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0run_trace.ps1" -Scenario galactic -LuaProbe %*
exit /b %ERRORLEVEL%
