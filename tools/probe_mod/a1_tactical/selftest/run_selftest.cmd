@echo off
rem Offline selftest of both A1 probes (no game needed): Lua 5.0.2 harness, fake game -> a1_reader.py -> a1_parse.py.
rem Builds tools\probe_mod\out\tools\lua50.exe on first use (common\build_lua50.cmd). Ends with SELFTEST OK / FAILED.
"%~dp0..\..\..\..\toolchain\venv\Scripts\python.exe" -I "%~dp0selftest.py" %*
