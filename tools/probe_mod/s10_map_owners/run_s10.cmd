@echo off
rem S10 map-owner probe: builds + installs Mods\S10MapOwners, starts the memory reader (bin\s10reader.exe), launches
rem FoC, waits for it to exit, writes the table, cleans up. Steam must be running. Arguments are passed on
rem (--non-interactive, --no-launch, --no-popup, --game DIR). With --non-interactive there is no pause at the end.
"%~dp0..\..\..\toolchain\venv\Scripts\python.exe" -I "%~dp0run_s10.py" %*
set "RC=%ERRORLEVEL%"
echo %* | find /i "--non-interactive" >nul && exit /b %RC%
echo.
pause
exit /b %RC%
