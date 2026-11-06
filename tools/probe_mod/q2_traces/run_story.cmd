@echo off
rem story t04: Q2 trace of a STORY MISSION (hands-off), the reference for the port's story campaign check (roadmap story t04).
rem Records the first tactical mission of the FoC Underworld story campaign: Kessel UM01 (STORY_UNDERWORLD_M01_LAND.XML,
rem the linked land battle LINK_TACTICAL starts at once). The trace starts when the battle mode is up (mode stack 2) and
rem ends when the battle ends or after 18000 ticks (10 min of game time); the mission scripts run on their own.
rem Rebuilds q2reader.exe first (the story scenario is new). The menus are picked by hand (-NoDrive):
rem   Single Player - Campaigns (Forces of Corruption) - Underworld - Start (difficulty Normal). Esc through the movie.
rem   When the Kessel battle has loaded, do NOT touch mouse or keyboard until the game closes by itself.
rem Output: out\<run>\trace.q2t (send the folder back). Cleanup afterwards if the window was closed early: cleanup.cmd.
call "%~dp0build_reader.cmd"
if errorlevel 1 (echo q2reader build failed: see logs\q2reader_build.txt& exit /b 20)
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0run_trace.ps1" -Scenario story -NoDrive -Ticks 18000 -WaitSessionMin 30 -Label "Underworld campaign, Kessel UM01 land mission, Normal, hands-off (by hand)" %*
exit /b %ERRORLEVEL%
