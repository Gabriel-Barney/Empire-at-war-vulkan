@echo off
rem Packs a test build zip into dist\ (alamo_game, Play.exe and the .cmd launchers, the Command Center launcher, CHANGES.txt).
rem Usage: tools\testbuild\package_testbuild.cmd [--smoke] [--name NAME] [--build-dir build-msvc] [--allow-dirty]
rem   Build first with build_msvc.cmd (Release) and build_launcher.cmd. Edit tools\testbuild\CHANGES.txt for each build. --smoke runs the
rem   Thrawn's Revenge mod check and skirmish smoke through the packed launchers before zipping (needs the game, the
rem   mod and a GPU). Details: tools\testbuild\package_testbuild.py.
setlocal
cd /d "%~dp0..\.."
python -I tools\testbuild\package_testbuild.py %*
