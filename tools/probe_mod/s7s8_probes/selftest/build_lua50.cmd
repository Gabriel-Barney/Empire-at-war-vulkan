@echo off
rem Builds a stock Lua 5.0.2 interpreter (toolchain\lua-5.0.2, project-local MSVC) for s7s8_selftest.py:
rem out\tools\lua50.exe. Log: out\tools\lua50_build.txt. Exit code 0 = built. /Od: MSVC 14.51 /O2 miscompiles
rem ltable.c computesizes (engine/lua/CMakeLists.txt), so stock 5.0.2 built at /O2 crashes at start.
setlocal
set "HERE=%~dp0"
set "ROOT=%HERE%..\..\..\..\"
set "LUA=%ROOT%toolchain\lua-5.0.2"
set "OUT=%HERE%..\out\tools"
if not exist "%OUT%" mkdir "%OUT%"
set "LOG=%OUT%\lua50_build.txt"
if not exist "%ROOT%toolchain\msvc\setup_x64.bat" (
  echo Toolchain missing: run toolchain\2_setup.cmd first.> "%LOG%"
  exit /b 1
)
if not exist "%LUA%\src\lapi.c" (
  echo Lua 5.0.2 sources missing: run toolchain\7_lua.cmd first.> "%LOG%"
  exit /b 1
)
call "%ROOT%toolchain\msvc\setup_x64.bat" >nul
del /q "%OUT%\lua50.exe" 2>nul
pushd "%OUT%"
> "%LOG%" cl /nologo /Od /W0 /MT /D_CRT_SECURE_NO_WARNINGS /I"%LUA%\include" /I"%LUA%\src" "%LUA%\src\lapi.c" "%LUA%\src\lcode.c" "%LUA%\src\ldebug.c" "%LUA%\src\ldo.c" "%LUA%\src\ldump.c" "%LUA%\src\lfunc.c" "%LUA%\src\lgc.c" "%LUA%\src\llex.c" "%LUA%\src\lmem.c" "%LUA%\src\lobject.c" "%LUA%\src\lopcodes.c" "%LUA%\src\lparser.c" "%LUA%\src\lstate.c" "%LUA%\src\lstring.c" "%LUA%\src\ltable.c" "%LUA%\src\ltm.c" "%LUA%\src\lundump.c" "%LUA%\src\lvm.c" "%LUA%\src\lzio.c" "%LUA%\src\lib\lauxlib.c" "%LUA%\src\lib\lbaselib.c" "%LUA%\src\lib\ldblib.c" "%LUA%\src\lib\liolib.c" "%LUA%\src\lib\lmathlib.c" "%LUA%\src\lib\loadlib.c" "%LUA%\src\lib\lstrlib.c" "%LUA%\src\lib\ltablib.c" "%LUA%\src\lua\lua.c" /Fe"%OUT%\lua50.exe" 2>&1
del /q *.obj 2>nul
popd
if not exist "%OUT%\lua50.exe" exit /b 1
echo BUILD_OK>> "%LOG%"
exit /b 0
