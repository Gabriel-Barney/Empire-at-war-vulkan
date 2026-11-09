@echo off
rem a2m t07 Rebel slice run (rebel_slice\run_rebel_slice.ps1): hands-off Galactic Conquest Heart of the Maw, human Empire,
rem with the Rebel Advance_Tech_Rebel plan hooked (Mods\Q2RebelSlice) and the Rebel credits / unlocked list watched in
rem memory; the q2 galactic trace runs alongside it. You pick the menus by hand (Galactic Conquest > Heart of the Maw >
rem Empire > Medium); the game closes itself after -Days fiscal cycles (default 12). It reopens itself in its own console
rem window (start), so it does not depend on the shell or the Claude session that started it. Arguments are passed on
rem (-Days N, -NoLaunch, -DryRun, -GameRoot DIR). Result: rebel_slice\out\latest.txt names the run folder.
if /i "%~1"=="--here" goto here
start "A2M t07 rebel slice run" cmd /c ""%~f0" --here %*"
echo Started in its own window. tools\probe_mod\q2_traces\rebel_slice\out\latest.txt names the run folder.
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
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0rebel_slice\run_rebel_slice.ps1" %ARGS%
set "RC=%ERRORLEVEL%"
echo.
echo A2M t07 rebel slice run finished: exit %RC% (0 = records and report). Run folder: tools\probe_mod\q2_traces\rebel_slice\out\latest.txt
pause
exit /b %RC%
