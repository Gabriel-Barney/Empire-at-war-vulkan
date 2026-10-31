@echo off
rem Removes the TSW planet-destroy probe mod folder (only if it carries TSWP_PROBE_MARKER.txt). Safe to repeat.
"%~dp0..\..\..\toolchain\venv\Scripts\python.exe" -I "%~dp0cleanup_tswp.py" %*
echo.
pause
