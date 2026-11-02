@echo off
rem FOW regrow probe (fowf t06): skirmish land, Polus, you Empire vs AI Rebel, no orders, 3 minutes.
rem Installs Mods\FowRegrowProbe, starts the memory reader (fow_reader.py), launches FoC with MODPATH=Mods\FowRegrowProbe,
rem waits for the game to exit, writes out\<run>\report.txt, cleans up. Steam must be running. Arguments are passed on
rem (--non-interactive, --no-launch, --game DIR, --map NAME, --scan-sec S). With --non-interactive there is no pause.
"%~dp0..\..\..\toolchain\venv\Scripts\python.exe" -I "%~dp0fow_run.py" %*
set "RC=%ERRORLEVEL%"
echo %* | find /i "--non-interactive" >nul && exit /b %RC%
echo.
pause
exit /b %RC%
