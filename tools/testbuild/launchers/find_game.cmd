@echo off
rem Shared by the launchers: sets GAME to the Forces of Corruption "corruption" folder (launcher.ps1 -Mode game).
rem Order: game_path.txt next to this file, ALAMO_FOC_DATA, the Steam library folders, then it asks once.
set "GAME="
set "FIND_GAME_OUT=%TEMP%\alamo_find_game_%RANDOM%.cmd"
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0launcher.ps1" -Mode game -Out "%FIND_GAME_OUT%"
if errorlevel 1 exit /b 1
call "%FIND_GAME_OUT%"
del "%FIND_GAME_OUT%" 2>nul
set "FIND_GAME_OUT="
if not defined GAME exit /b 1
exit /b 0
