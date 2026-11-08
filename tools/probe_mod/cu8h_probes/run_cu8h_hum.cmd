@echo off
rem cu8h t02: lightsaber hum / holster probe (land skirmish Dagobah, you Empire vs AI Rebel; Vader, then Luke are
rem spawned for you). Builds Mods\CU8HProbe, launches FoC with it, reads the heroes' state from the game's memory
rem (read only), writes out\<run>\report.txt when the game exits, cleans up. Steps: README.md. Arguments are passed
rem on (--non-interactive, --no-launch, --windowed, --dry-run, --game DIR).
"%~dp0..\..\..\toolchain\venv\Scripts\python.exe" -I "%~dp0cu8h_run.py" --session hum %*
set "RC=%ERRORLEVEL%"
echo %* | find /i "--non-interactive" >nul && exit /b %RC%
echo %* | find /i "--dry-run" >nul && exit /b %RC%
echo.
pause
exit /b %RC%
