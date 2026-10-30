@echo off
rem Builds bin\s10reader.exe and bin\s10_fake_game.exe (test target) with the project-local MSVC (/W4 /WX).
rem Log: logs\s10reader_build.txt. Exit code 0 = both built. run_s10.py calls this when bin\s10reader.exe is missing.
setlocal
set "HERE=%~dp0"
set "ROOT=%HERE%..\..\..\"
if not exist "%ROOT%logs" mkdir "%ROOT%logs"
if not exist "%HERE%bin" mkdir "%HERE%bin"
set "LOG=%ROOT%logs\s10reader_build.txt"
if not exist "%ROOT%toolchain\msvc\setup_x64.bat" (
  echo Toolchain missing: run toolchain\2_setup.cmd first.> "%LOG%"
  exit /b 1
)
call "%ROOT%toolchain\msvc\setup_x64.bat" >nul
set "FLAGS=/nologo /std:c++20 /O2 /W4 /WX /permissive- /EHsc /MT /utf-8 /DNOMINMAX /DWIN32_LEAN_AND_MEAN /D_CRT_SECURE_NO_WARNINGS"
del /q "%HERE%bin\s10reader.exe" "%HERE%bin\s10_fake_game.exe" 2>nul
> "%LOG%" (
  cl %FLAGS% "%HERE%reader\s10reader.cpp" /Fo"%HERE%bin\s10reader.obj" /Fe"%HERE%bin\s10reader.exe" /link user32.lib 2>&1
  cl %FLAGS% "%HERE%reader\s10_fake_game.cpp" /Fo"%HERE%bin\s10_fake_game.obj" /Fe"%HERE%bin\s10_fake_game.exe" /link user32.lib 2>&1
)
del /q "%HERE%bin\*.obj" 2>nul
if not exist "%HERE%bin\s10reader.exe" exit /b 1
if not exist "%HERE%bin\s10_fake_game.exe" exit /b 1
echo BUILD_OK>> "%LOG%"
exit /b 0
