@echo off
rem Restores the game folder from the kit's manifest and re-enables the prior patch chain. Safe to run any number of times.
rem Double-click it. Extra options are passed through (see "g1.py --help"). Logs and captures: out\runs\.
setlocal
set PY=%~dp0..\..\..\toolchain\venv\Scripts\python.exe
if not exist "%PY%" (echo Project Python not found: run toolchain\2_setup.cmd first.& pause & exit /b 1)
"%PY%" -I "%~dp0g1.py" cleanup %*
set RC=%ERRORLEVEL%
echo.
if "%RC%"=="0" (echo Finished OK.) else (echo Finished with exit code %RC% - see out\last_status.json)
if not defined G1_NONINTERACTIVE pause
exit /b %RC%