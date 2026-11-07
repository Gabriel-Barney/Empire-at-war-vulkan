@echo off
rem modc t09: play a second mod in the port and in the original, side by side. Steps and what to look at: README.md.
rem Usage: run_t09.cmd port   <workshop id> [more alamo_game arguments]   the port (alamo_game), log in out\
rem        run_t09.cmd retail <workshop id>                               the original (StarWarsG.exe), for comparison
rem        run_t09.cmd check  <workshop id>                               the load report only (alamo_mod_check), no window
rem GAME is the FoC "corruption" folder; ALAMO_BIN the folder with alamo_game.exe / alamo_mod_check.exe (default: the
rem MSVC build of this checkout, build-msvc\bin, else build-wt-msvc\bin). Nothing is written into the game or mod folders.
setlocal
if not defined GAME set "GAME=F:\SteamLibrary\steamapps\common\Star Wars Empire at War\corruption"
if not exist "%GAME%\Data" (echo Forces of Corruption not found at "%GAME%". Set GAME first.& exit /b 1)
set "MODE=%~1"
set "ID=%~2"
if "%ID%"=="" (echo usage: run_t09.cmd port^|retail^|check ^<workshop id^>& exit /b 2)
shift
shift
set "ROOT=%~dp0..\..\.."
if not defined ALAMO_BIN if exist "%ROOT%\build-msvc\bin\alamo_game.exe" set "ALAMO_BIN=%ROOT%\build-msvc\bin"
if not defined ALAMO_BIN if exist "%ROOT%\build-wt-msvc\bin\alamo_game.exe" set "ALAMO_BIN=%ROOT%\build-wt-msvc\bin"
set "OUT=%~dp0out"
if not exist "%OUT%" mkdir "%OUT%"
for /f %%T in ('powershell -NoProfile -Command "Get-Date -Format yyyyMMdd-HHmmss"') do set "STAMP=%%T"
if /i "%MODE%"=="retail" (
  echo Starting the original: StarWarsG.exe STEAMMOD=%ID%  ^(Steam must be running^)
  start "" /D "%GAME%" "%GAME%\StarWarsG.exe" STEAMMOD=%ID%
  exit /b 0
)
if not defined ALAMO_BIN (echo alamo_game.exe not found: build first ^(build_msvc.cmd^) or set ALAMO_BIN.& exit /b 1)
if /i "%MODE%"=="check" (
  "%ALAMO_BIN%\alamo_mod_check.exe" --game "%GAME%" STEAMMOD=%ID% --out "%OUT%\%STAMP%-%ID%-report.txt" --json "%OUT%\%STAMP%-%ID%-report.json"
  echo Exit code %errorlevel%. Report: out\%STAMP%-%ID%-report.txt
  exit /b 0
)
if /i not "%MODE%"=="port" (echo unknown mode %MODE%: port, retail or check& exit /b 2)
echo Starting the port: alamo_game STEAMMOD=%ID%  ^(log: out\%STAMP%-%ID%-alamo_game.txt^)
"%ALAMO_BIN%\alamo_game.exe" --game "%GAME%" --cache-dir "%OUT%\cache" --saves-dir "%OUT%\saves-%ID%" STEAMMOD=%ID% %1 %2 %3 %4 %5 %6 1> "%OUT%\%STAMP%-%ID%-alamo_game.txt" 2>&1
echo alamo_game exit code %errorlevel%. Log: out\%STAMP%-%ID%-alamo_game.txt
exit /b 0
