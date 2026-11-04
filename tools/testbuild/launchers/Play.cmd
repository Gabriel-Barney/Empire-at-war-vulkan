@echo off
rem Alamo engine test build: Forces of Corruption. Set GAME to your "corruption" folder if it differs.
set "GAME=F:\SteamLibrary\steamapps\common\Star Wars Empire at War\corruption"
if not exist "%GAME%\Data" (
  echo Game data not found at "%GAME%". Edit GAME in Play.cmd.
  pause
  exit /b 1
)
cd /d "%~dp0"
"%~dp0alamo_game.exe" --game "%GAME%" --cache-dir "%~dp0cache" %* 1> "%~dp0alamo_game_log.txt" 2>&1
if errorlevel 1 (
  echo alamo_game exited with an error. See alamo_game_log.txt.
  pause
)
