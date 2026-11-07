@echo off
rem Alamo engine test build: Forces of Corruption, no mod. For mods use Play_Mod.cmd.
rem The game folder is found on its own (Steam); if not, you are asked once and it is saved in game_path.txt.
setlocal
cd /d "%~dp0"
call "%~dp0find_game.cmd" || (pause & exit /b 1)
"%~dp0alamo_game.exe" --game "%GAME%" --cache-dir "%~dp0cache" %* 1> "%~dp0alamo_game_log.txt" 2>&1
if errorlevel 1 (
  echo alamo_game exited with an error. See alamo_game_log.txt.
  pause
)
