@echo off
rem Loads Thrawn's Revenge the way the game does, without a window, and writes a report: mod_check_tr.txt.
setlocal
cd /d "%~dp0"
call "%~dp0tr_paths.cmd" || (pause & exit /b 1)
echo Checking Thrawn's Revenge at %TR% ...
"%~dp0alamo_mod_check.exe" --game "%GAME%" "MODPATH=%TR%" 1> "%~dp0mod_check_tr.txt" 2>&1
echo Exit code %errorlevel%. Report: mod_check_tr.txt
if not defined ALAMO_NO_NOTEPAD start "" notepad "%~dp0mod_check_tr.txt"
