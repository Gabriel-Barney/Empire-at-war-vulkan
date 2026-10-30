@echo off
rem Probe E launch: FALLBACKPATH chain vs replace. Steam must be running.
cd /d "F:\SteamLibrary\steamapps\common\Star Wars Empire at War\corruption"
start "" StarWarsG.exe MODPATH=Mods\ProbeE FALLBACKPATH=Mods\ProbeE_FB
