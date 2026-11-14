@echo off
rem bkg3 t03 budget probe: the a2f_budget kit played as EMPIRE, so the Rebel and the Underworld are the AI players whose
rem budgets are recorded (README section 11). Heart of the Maw (Sandbox_Heart_Of_The_Maw_Empire), human EMPIRE, no
rem orders, 5 minutes of galactic time (quit at "5-minute dense phase done", frame 9000). Same steps as run_a2f.cmd
rem (profile backup, build + install Mods\A2fBudgetProbe, launch, scan, report, cleanup, profile restore). It reopens
rem itself in its own console window (start), so the recorder does not depend on the shell or the session that started
rem it. Arguments are passed on to a2f_run.py (--windowed, --no-launch, --no-mod-equations, --game DIR, --scan-sec S,
rem --non-interactive = no pause at the end). Cleanup if a run was interrupted: cleanup_a2f.cmd.
if /i "%~1"=="--here" goto here
start "BKG3 t03 budget probe" cmd /c ""%~f0" --here %*"
echo Started in its own window. tools\probe_mod\a2f_budget\out\latest.txt names the run folder, its run_status.txt the state.
exit /b 0
:here
set "ARGS="
shift /1
:collect
if "%~1"=="" goto run
set "ARGS=%ARGS% %1"
shift /1
goto collect
:run
"%~dp0..\..\..\toolchain\venv\Scripts\python.exe" -I "%~dp0a2f_run.py" --human-empire %ARGS%
set "RC=%ERRORLEVEL%"
echo.
echo BKG3 t03 budget probe (human Empire) finished: exit %RC% (0 = records and report). Run folder: tools\probe_mod\a2f_budget\out\latest.txt
echo %ARGS% | find /i "--non-interactive" >nul && exit /b %RC%
pause
exit /b %RC%
