@echo off
rem Parity reference capture: undo anything the Q2 trace runs left in the game folder (Mods\Q2Trace, a stuck reader or
rem game it started). Safe to run twice. Same as tools\probe_mod\q2_traces\cleanup.cmd -All; options are passed on (-DryRun).
call "%~dp0..\q2_traces\cleanup.cmd" -All %*
exit /b %ERRORLEVEL%
