@echo off
rem Packs a test build zip into dist\ (alamo_game, the FoC and Thrawn's Revenge launchers, CHANGES.txt).
rem Usage: tools\testbuild\package_testbuild.cmd [--smoke] [--name NAME] [--build-dir build-msvc] [--allow-dirty]
rem   Build first with build_msvc.cmd (Release). Edit tools\testbuild\CHANGES.txt for each build. --smoke runs the
rem   Thrawn's Revenge mod check and skirmish smoke through the packed launchers before zipping (needs the game, the
rem   mod and a GPU). Details: tools\testbuild\package_testbuild.py.
setlocal
cd /d "%~dp0..\.."
python -I tools\testbuild\package_testbuild.py %*
