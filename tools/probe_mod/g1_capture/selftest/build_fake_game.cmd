@echo off
rem Builds selftest\bin\fake_d3d9_game.exe with the project-local MSVC (toolchain\msvc). Log: selftest\bin\build.log.
setlocal
set HERE=%~dp0
set TC=%HERE%..\..\..\..\toolchain
if not exist "%HERE%bin" mkdir "%HERE%bin"
call "%TC%\msvc\setup_x64.bat" >nul
cd /d "%HERE%bin"
cl /nologo /W4 /permissive- /EHsc /O1 /DUNICODE /D_UNICODE /D_CRT_SECURE_NO_WARNINGS "%HERE%fake_d3d9_game.cpp" /Fe:fake_d3d9_game.exe /link d3d9.lib d3dcompiler.lib user32.lib > build.log 2>&1
set RC=%ERRORLEVEL%
type build.log
exit /b %RC%
