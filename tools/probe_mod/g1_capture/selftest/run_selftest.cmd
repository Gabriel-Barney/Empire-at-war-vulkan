@echo off
rem Self-test of the G1 kit against a fake game folder (never touches the real install). About 3 minutes.
setlocal
set PY=%~dp0..\..\..\..\toolchain\venv\Scripts\python.exe
"%PY%" -I "%~dp0selftest.py"
set RC=%ERRORLEVEL%
if not defined G1_NONINTERACTIVE pause
exit /b %RC%