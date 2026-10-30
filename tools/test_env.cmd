@echo off
rem Called by the build scripts: gives the data-driven tests their game data unless the caller already set it.
rem Defaults are Gabriel's Steam install (CLAUDE.md); a missing folder leaves the variable unset and the tests skip.
rem Use "call tools\test_env.cmd" (no setlocal here, so the variables reach the caller).
if not defined ALAMO_FOC_DATA (
  if exist "F:\SteamLibrary\steamapps\common\Star Wars Empire at War\corruption\Data\XML" (
    set "ALAMO_FOC_DATA=F:\SteamLibrary\steamapps\common\Star Wars Empire at War\corruption"
  )
)
if not defined ALAMO_EAW_DATA (
  if exist "F:\SteamLibrary\steamapps\common\Star Wars Empire at War\GameData\Data\XML" (
    set "ALAMO_EAW_DATA=F:\SteamLibrary\steamapps\common\Star Wars Empire at War\GameData"
  )
)
if not defined ALAMO_FOC_DATA echo test_env: ALAMO_FOC_DATA not set and default path missing; data tests will skip.
if not defined ALAMO_EAW_DATA echo test_env: ALAMO_EAW_DATA not set and default path missing; data tests will skip.
exit /b 0
