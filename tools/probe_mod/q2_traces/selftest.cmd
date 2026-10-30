@echo off
rem Q2 kit self-test without the game: hash vectors, reader vs fake_game end to end, install+cleanup on a scratch fake game folder.
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0selftest.ps1" %*
exit /b %ERRORLEVEL%
