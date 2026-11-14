@echo off
rem Build and test the Command Center launcher (launcher\, Rust) with the project-local toolchain.
rem Separate from the CMake engine build: build_msvc / build_clang / build_gate do not build it.
rem Output: launcher\target\release\command-center.exe. Log: logs\launcher_build.txt
setlocal EnableDelayedExpansion
cd /d "%~dp0"
if not exist logs mkdir logs
> logs\launcher_build.txt (
  call launcher\cargo.cmd build --release --locked 2>&1
  echo build exit !ERRORLEVEL!
  echo === tests
  call launcher\cargo.cmd test --release --locked 2>&1
  echo test exit !ERRORLEVEL!
)
echo BUILD_DONE>> logs\launcher_build.txt
findstr /r /c:"^build exit" /c:"^test exit" /c:"^test result:" logs\launcher_build.txt
findstr /r /c:"^build exit [1-9]" /c:"^test exit [1-9]" logs\launcher_build.txt >nul && exit /b 1
exit /b 0
