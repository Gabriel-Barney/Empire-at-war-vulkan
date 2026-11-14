@echo off
rem Starts FoC windowed with MODPATH=Mods\CU8HProbe (the kashyyyk / jetpack / luke sessions; start the kit with
rem --no-launch first). From a Claude shell use: Start-Process explorer.exe <this file>.
cd /d "F:\SteamLibrary\steamapps\common\Star Wars Empire at War\corruption"
start "" StarWarsG.exe WINDOWED SCREENWIDTH=1920 SCREENHEIGHT=1080 MODPATH=Mods\CU8HProbe
