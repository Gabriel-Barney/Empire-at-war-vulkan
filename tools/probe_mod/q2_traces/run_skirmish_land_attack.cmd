@echo off
rem skai t02: long Q2 LAND skirmish trace (hands-off, about 22 min) to see whether and when the retail AI takes the enemy
rem base. In the port the Rebel AI never starts a Destroy_Structure plan: its whole army sits at the 2-player land unit cap
rem of 9, held by scouting and pad plans (docs\re\SKIRMISH_SETUP.md section 10). The trace answers: how many units the AI
rem fields over time (is 9 the real cap), when its units first reach the idle human's base, and which base objects it destroys.
rem Same clicks as run_skirmish_land.cmd: Single Player - Skirmish Battles - game type Land - "Utapau" (_MP_LAND_UTAPAU),
rem you = Empire (slot 1), slot 2 = Rebel Medium AI - Advanced... Default - Start Game. Then touch nothing until it stops.
rem Output: out\<run>\ (send the folder back). Read it with analysis\land_attack.py <run dir>. Cleanup: cleanup.cmd.
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0run_trace.ps1" -Scenario skirmish -NoDrive -LuaProbe -Ticks 36000 -TimeoutMin 30 -Label "Utapau land t02, Empire idle vs Rebel Medium, advanced Default (by hand)" %*
exit /b %ERRORLEVEL%
