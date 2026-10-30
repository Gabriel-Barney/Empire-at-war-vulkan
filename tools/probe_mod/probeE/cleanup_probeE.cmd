@echo off
rem Removes only the Probe E files; none share a name with retail files.
set GAME=F:\SteamLibrary\steamapps\common\Star Wars Empire at War
> "%~dp0cleanup.log" (
  del /q "%GAME%\GameData\Data\XML\Probe_E1.xml" 2>&1
  del /q "%GAME%\GameData\Data\XML\Probe_E2.xml" 2>&1
  rmdir /s /q "%GAME%\corruption\Mods\ProbeE" 2>&1
  rmdir /s /q "%GAME%\corruption\Mods\ProbeE_FB" 2>&1
  dir /b "%GAME%\GameData\Data\XML" 2>&1
  dir /b "%GAME%\corruption\Mods" 2>&1
  echo CLEANUP_DONE
)
