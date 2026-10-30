@echo off
rem G1 capture 3/3: apitrace D3D9 trace of a SHORT session (about 10 minutes + analysis).
rem Double-click it. Extra options are passed through (see "g1.py --help"). Logs and captures: out\runs\.
setlocal
set PY=%~dp0..\..\..\toolchain\venv\Scripts\python.exe
if not exist "%PY%" (echo Project Python not found: run toolchain\2_setup.cmd first.& pause & exit /b 1)
"%PY%" -I "%~dp0g1.py" apitrace %*
set RC=%ERRORLEVEL%
echo.
if "%RC%"=="0" (echo Finished OK.) else (echo Finished with exit code %RC% - see out\last_status.json)
if not defined G1_NONINTERACTIVE pause
exit /b %RC%