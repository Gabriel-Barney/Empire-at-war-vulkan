@echo off
rem Q2: undo everything the trace runs installed or started. Safe to run twice. Options: -DryRun, -All, -RunDir <dir>, -Force.
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0cleanup.ps1" %*
exit /b %ERRORLEVEL%
