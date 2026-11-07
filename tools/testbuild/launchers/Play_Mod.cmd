@echo off
rem Alamo engine test build: play any installed mod.
rem   Double-click: a menu of the mods in the Steam workshop folder and in the game's Mods folder.
rem   Drag a mod folder onto this file: plays that folder (it needs a Data folder inside).
rem   From a console: Play_Mod.cmd <mod folder | workshop id | menu answer such as 3, C3 or 7+1>
rem Each mod gets its own folder under mod_runs: shader cache, saves, log.txt and mod_check.txt.
rem The game folder is found on its own (Steam); if not, you are asked once and it is saved in game_path.txt.
setlocal
cd /d "%~dp0"
if not exist "%~dp0mod_runs" mkdir "%~dp0mod_runs"
set "CHOICE_FILE=%~dp0mod_runs\choice.cmd"
if exist "%CHOICE_FILE%" del "%CHOICE_FILE%"
if "%~1"=="" (
  powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0launcher.ps1" -Mode pick -Out "%CHOICE_FILE%"
) else (
  powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0launcher.ps1" -Mode pick -Out "%CHOICE_FILE%" -Pick "%~1"
)
if errorlevel 1 (
  pause
  exit /b 1
)
if not exist "%CHOICE_FILE%" exit /b 1
call "%CHOICE_FILE%"
if "%ACTION%"=="quit" exit /b 0
rem The base game uses the same cache, log and saves (LOCALAPPDATA\alamo\Saves) as Play.cmd.
set "RUN=%~dp0"
set "LOGNAME=alamo_game_log.txt"
set "CACHE=%~dp0cache"
set "SAVES="
if "%MODKEY%"=="base" goto run_dirs_set
set "RUN=%~dp0mod_runs\%MODKEY%\"
set "LOGNAME=mod_runs\%MODKEY%\log.txt"
set "CACHE=%~dp0mod_runs\%MODKEY%\cache"
set SAVES=--saves-dir "%~dp0mod_runs\%MODKEY%\saves"
:run_dirs_set
if not exist "%RUN%" mkdir "%RUN%"
if "%ACTION%"=="check" goto check

echo Playing %MODNAME%
echo   game: %GAME%
echo   mod arguments: %MODARGS%
echo   log: %LOGNAME%
echo Close the game window to come back here. The first start of a mod builds its shader cache, so it is slower.
"%~dp0alamo_game.exe" --game "%GAME%" --cache-dir "%CACHE%" %SAVES% %MODARGS% %ALAMO_ARGS% 1> "%~dp0%LOGNAME%" 2>&1
if errorlevel 1 (
  echo alamo_game exited with an error. Please send %LOGNAME%.
  pause
  exit /b 1
)
exit /b 0

:check
if "%MODKEY%"=="base" (set "REPORT=mod_check_base.txt") else (set "REPORT=mod_runs\%MODKEY%\mod_check.txt")
echo Checking %MODNAME% (loads it the way the game does, without a window) ...
"%~dp0alamo_mod_check.exe" --game "%GAME%" %MODARGS% 1> "%~dp0%REPORT%" 2>&1
echo Exit code %errorlevel%. Report: %REPORT%
if not defined ALAMO_NO_NOTEPAD start "" notepad "%~dp0%REPORT%"
exit /b 0
