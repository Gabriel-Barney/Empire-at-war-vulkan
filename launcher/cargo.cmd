@echo off
rem Runs cargo from the project-local Rust toolchain (toolchain\rust, set up by toolchain\12_rust.cmd)
rem with the project-local MSVC linker (toolchain\msvc). Nothing is installed system-wide.
rem Usage (from launcher\):  cargo.cmd build --release      cargo.cmd test --release      cargo.cmd run
setlocal
set "ROOT=%~dp0..\"
if not exist "%ROOT%toolchain\rust\cargo\bin\cargo.exe" (
  echo Rust toolchain missing: run toolchain\12_rust.cmd ^(or toolchain\2_setup.cmd^) first.
  exit /b 1
)
if not exist "%ROOT%toolchain\msvc\setup_x64.bat" (
  echo MSVC toolchain missing: run toolchain\2_setup.cmd first.
  exit /b 1
)
call "%ROOT%toolchain\msvc\setup_x64.bat" >nul
set "RUSTUP_HOME=%ROOT%toolchain\rust\rustup"
set "CARGO_HOME=%ROOT%toolchain\rust\cargo"
set "PATH=%CARGO_HOME%\bin;%PATH%"
cd /d "%~dp0"
"%CARGO_HOME%\bin\cargo.exe" %*
exit /b %ERRORLEVEL%
