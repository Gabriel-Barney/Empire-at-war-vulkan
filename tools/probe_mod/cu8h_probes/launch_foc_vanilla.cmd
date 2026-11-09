@echo off
rem Starts FoC windowed with no mod (storyb run). From a Claude shell use: Start-Process explorer.exe <this file>
rem (StarWarsG.exe started straight from a Claude shell exits at once). Game folder: Gabriel's Steam install.
cd /d "F:\SteamLibrary\steamapps\common\Star Wars Empire at War\corruption"
start "" StarWarsG.exe WINDOWED SCREENWIDTH=1920 SCREENHEIGHT=1080
