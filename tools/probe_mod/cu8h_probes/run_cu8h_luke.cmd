@echo off
rem cu8h t02 rerun of the Saber_Throw half: as run_cu8h_hum.cmd without Vader; Luke throws once while holstered and
rem once while drawn; the watcher also reads the thrown-saber id (SAP +0x2e8). Steps: README.md. Arguments are passed
rem on (--non-interactive, --no-launch, --windowed, --dry-run, --game DIR).
"%~dp0..\..\..\toolchain\venv\Scripts\python.exe" -I "%~dp0cu8h_run.py" --session luke %*
set "RC=%ERRORLEVEL%"
echo %* | find /i "--non-interactive" >nul && exit /b %RC%
echo %* | find /i "--dry-run" >nul && exit /b %RC%
echo.
pause
exit /b %RC%
