@echo off
rem Builds q2reader.exe and the fake_game.exe test target with the project-local MSVC (toolchain\msvc).
rem Output: tools\probe_mod\q2_traces\bin\. Log: logs\q2reader_build.txt. Exit code 0 = both built.
setlocal
set "HERE=%~dp0"
set "ROOT=%HERE%..\..\..\"
if not exist "%ROOT%logs" mkdir "%ROOT%logs"
if not exist "%HERE%bin" mkdir "%HERE%bin"
set "LOG=%ROOT%logs\q2reader_build.txt"
if not exist "%ROOT%toolchain\msvc\setup_x64.bat" (
  echo Toolchain missing: run toolchain\2_setup.cmd first.> "%LOG%"
  exit /b 1
)
call "%ROOT%toolchain\msvc\setup_x64.bat" >nul
set "FLAGS=/nologo /std:c++20 /O2 /W4 /WX /permissive- /EHsc /MT /utf-8 /DNOMINMAX /DWIN32_LEAN_AND_MEAN /D_CRT_SECURE_NO_WARNINGS"
del /q "%HERE%bin\q2reader.exe" "%HERE%bin\fake_game.exe" 2>nul
> "%LOG%" (
  cl %FLAGS% "%HERE%reader\q2reader.cpp" /Fo"%HERE%bin\q2reader.obj" /Fe"%HERE%bin\q2reader.exe" /link user32.lib winmm.lib 2>&1
)
if not exist "%HERE%bin\q2reader.exe" exit /b 1
>> "%LOG%" (
  cl %FLAGS% "%HERE%reader\fake_game.cpp" /Fo"%HERE%bin\fake_game.obj" /Fe"%HERE%bin\fake_game.exe" 2>&1
)
if not exist "%HERE%bin\fake_game.exe" exit /b 1
del /q "%HERE%bin\*.obj" 2>nul
echo BUILD_OK>> "%LOG%"
exit /b 0
