@echo off
rem story t04 / storyb t09: Q2 trace of a STORY MISSION (fully hands-off), the reference for the port's story campaign check.
rem Records the first tactical mission of the FoC Underworld story campaign: Kessel UM01 (STORY_UNDERWORLD_M01_LAND.XML,
rem the linked land battle LINK_TACTICAL starts at once). The trace starts when the battle mode is up (mode stack 2) and
rem ends when the battle ends or after 18000 ticks (10 min of game time); the mission scripts run on their own.
rem Rebuilds q2reader.exe first. The menus are picked by hand (-NoDrive):
rem   Single Player - Campaigns (Forces of Corruption) - Underworld - Medium - Start. Esc is allowed only BEFORE the
rem   battle (intro movies, opening crawl). From the battle's load screen on: no key, no click, NOT EVEN Esc (an Esc in
rem   the battle runs the mission's Story_Handle_Esc and changes the sim), until the game closes by itself.
rem No time limit while the game runs (-TimeoutMin 0): a paused game (FoC pauses when its window loses focus) only
rem delays the run; bring the game to the front again. summary.json is written on every exit path (partial if needed).
rem It reopens itself in its own console window, so it does not depend on the shell that started it. Arguments are
rem passed on. From a Claude shell use -NoLaunch: the run writes out\launch_game.cmd; start it with cmd /c <file>. It runs
rem ..\common\launch_foc.py (CreateProcess under the debug API, detached at once; a plain or explorer.exe start dies).
rem Output: out\<yyyymmdd-hhmmss>-story-vanilla\ (trace.q2t, summary.json, run_status.json "state": "done", run_trace.log).
rem Cleanup afterwards if the window was closed early: cleanup.cmd.
if /i "%~1"=="--here" goto here
start "Q2 story run (run_story.cmd)" cmd /c ""%~f0" --here %*"
echo Started in its own window. Run folder: newest tools\probe_mod\q2_traces\out\*-story-vanilla (run_status.json "state": "done" at the end).
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
call "%~dp0build_reader.cmd"
if errorlevel 1 (echo q2reader build failed: see logs\q2reader_build.txt& pause& exit /b 20)
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0run_trace.ps1" -Scenario story -NoDrive -Ticks 18000 -WaitSessionMin 30 -TimeoutMin 0 -Label "Underworld campaign, Kessel UM01 land mission, Medium, fully hands-off (no Esc in the battle)" %ARGS%
set "RC=%ERRORLEVEL%"
echo.
echo Q2 story run finished: exit %RC% (0 = complete trace, 10 = partial). Run folder: newest tools\probe_mod\q2_traces\out\*-story-vanilla
pause
exit /b %RC%
