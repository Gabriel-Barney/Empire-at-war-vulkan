@echo off
rem a2k t05 saboteur probe: the short a2f_budget run for docs\re\A2K_T08_RE_ANSWERS.md section 5 (Q5; H1 / H2 of Q4.2).
rem Heart of the Maw (Sandbox_Heart_Of_The_Maw_Rebel), human REBEL, no orders, story on, frames 0..1400. Same steps as
rem run_a2f.cmd (profile backup, build + install Mods\A2fBudgetProbe with --saboteur, launch, scan, report, cleanup,
rem profile restore). It reopens itself in its own console window (start), so the recorder does not depend on the shell
rem or the Claude session that started it. Arguments are passed on to a2f_run.py (--windowed, --no-launch,
rem --no-sab-desire, --no-mod-equations, --game DIR, --scan-sec S, --non-interactive = no pause at the end).
if /i "%~1"=="--here" goto here
start "A2K t05 saboteur probe" cmd /c ""%~f0" --here %*"
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
"%~dp0..\..\..\toolchain\venv\Scripts\python.exe" -I "%~dp0a2f_run.py" --saboteur %ARGS%
set "RC=%ERRORLEVEL%"
echo.
echo A2K t05 saboteur probe finished: exit %RC% (0 = records and report). Run folder: tools\probe_mod\a2f_budget\out\latest.txt
echo %ARGS% | find /i "--non-interactive" >nul && exit /b %RC%
pause
exit /b %RC%
