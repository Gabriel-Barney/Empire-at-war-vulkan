@echo off
rem Regenerate the golden-backed CRC goldens as checkpoint files (ALAMO_CRC 2) from the fast MSVC tree and print a diff summary.
rem   tools\regen_goldens.cmd [--build-dir build-msvc] [--only headless|game|all] [--checkpoint N] [--dry-run] [--list] [--force]
rem Refuses (exit 3) when tracked files have uncommitted edits unless --force. Details: tools\goldens\regen_goldens.py, docs/status/golden-checkpoints.md.
rem The tree must be built (build_msvc.cmd); game data comes from tools\test_env.cmd. From PowerShell:
rem   & .\tools\regen_goldens.cmd --% --only all --dry-run
setlocal
cd /d "%~dp0.."
call "%CD%\toolchain\msvc\setup_x64.bat" >nul
call "%CD%\tools\test_env.cmd"
set "PATH=%CD%\toolchain\venv\Scripts;%PATH%"
set "PYTHONUTF8=1"
if not exist logs mkdir logs
python tools\goldens\regen_goldens.py %* > logs\regen_goldens.txt 2>&1
set RC=%ERRORLEVEL%
type logs\regen_goldens.txt
echo regen_goldens exit %RC% (log: logs\regen_goldens.txt)
exit /b %RC%
