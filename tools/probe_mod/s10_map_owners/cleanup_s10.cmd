@echo off
rem Removes the S10 probe mod folder and moves any game-written _S10MapOwners.txt into out\. Safe to repeat.
"%~dp0..\..\..\toolchain\venv\Scripts\python.exe" -I "%~dp0cleanup_s10.py" %*
echo.
pause
