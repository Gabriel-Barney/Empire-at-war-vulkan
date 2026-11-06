@echo off
rem PR check without hosted CI: build + test this checkout in its own tree and optionally comment the result on the PR.
rem Usage: tools\pr_check.cmd [PR] [--compiler msvc|clang|both] [--modules "core;..."] [--tests fast|all|regex] [--post] [--tree NAME]
rem Details: tools\pr_check\pr_check.py (docstring). Exit 0 only when every build passed with 0 warnings.
cd /d "%~dp0.."
set PATH=%CD%\toolchain\venv\Scripts;%PATH%
python tools\pr_check\pr_check.py %*
exit /b %ERRORLEVEL%
