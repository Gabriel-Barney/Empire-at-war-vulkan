@echo off
rem Shared by the Thrawn's Revenge launchers: finds GAME (the FoC "corruption" folder) and TR (the mod folder).
rem Edit GAME / TR here if yours are elsewhere. TR is found on its own in the Steam workshop folder next to the game.
set "GAME=F:\SteamLibrary\steamapps\common\Star Wars Empire at War\corruption"
set "TR="
if not exist "%GAME%\Data" (
  echo Forces of Corruption not found at "%GAME%". Edit GAME in tr_paths.cmd.
  exit /b 1
)
for %%D in ("%GAME%\..\..\..\workshop\content\32470\1125571106") do if exist "%%~fD\Data" set "TR=%%~fD"
if not defined TR for %%D in ("%GAME%\Mods\ThrawnsRevenge" "%GAME%\Mods\Thrawns_Revenge") do if exist "%%~fD\Data" set "TR=%%~fD"
if not defined TR (
  echo Thrawn's Revenge not found. Subscribe to it on the Steam workshop ^(item 1125571106^) or set TR in tr_paths.cmd.
  exit /b 1
)
exit /b 0
