@echo off
rem Alamo engine test build: Thrawn's Revenge (Steam workshop 1125571106) on Forces of Corruption.
rem Paths are in tr_paths.cmd. Saves go to saves_tr next to this file, the log to alamo_game_tr_log.txt.
setlocal
cd /d "%~dp0"
call "%~dp0tr_paths.cmd" || (pause & exit /b 1)
echo Thrawn's Revenge: %TR%
"%~dp0alamo_game.exe" --game "%GAME%" --cache-dir "%~dp0cache_tr" --saves-dir "%~dp0saves_tr" "MODPATH=%TR%" %* 1> "%~dp0alamo_game_tr_log.txt" 2>&1
if errorlevel 1 (
  echo alamo_game exited with an error. See alamo_game_tr_log.txt.
  pause
)
