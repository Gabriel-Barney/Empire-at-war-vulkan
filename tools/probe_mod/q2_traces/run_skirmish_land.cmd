@echo off
rem skai t04: Q2 LAND skirmish trace (hands-off) to settle the land skirmish [M] items of docs\re\SKIRMISH_SETUP.md section 10:
rem which pads start built, which Land_Skirmish_AI_Default_Forces appear where, when the AI is charged credits (pad builds,
rem production) and when a neutral Skirmish_Mineral_Processor_Pad changes owner.
rem The menu driver only knows the space Geonosis scene, so pick the map by hand (-NoDrive):
rem   Single Player - Skirmish Battles - game type Land - "Utapau" (_MP_LAND_UTAPAU, 2 players) - you = Empire (slot 1),
rem   slot 2 = Rebel Medium AI - Advanced... Default - Start Game. Then do not touch anything until the trace stops.
rem Output: out\<run>\trace.q2t (send the folder back). Cleanup afterwards: cleanup.cmd.
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0run_trace.ps1" -Scenario skirmish -NoDrive -Ticks 9000 -Label "Utapau land, Empire idle vs Rebel Medium, advanced Default (by hand)" %*
exit /b %ERRORLEVEL%
