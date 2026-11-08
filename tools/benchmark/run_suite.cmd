@echo off
rem BM3: run every benchmark scene several times and report the run-to-run noise on this PC (tools\benchmark\run_suite.py).
rem   tools\benchmark\run_suite.cmd [--repeats 3] [--scenes a,b] [--load] [--build-dir build-msvc] [--out logs\bench_suite] [--list]
rem Close other builds, tests and agents first: a run is flagged NOISY when the CPU was busy before it, and noisy numbers are never a baseline.
rem --load runs the rendered scenes in real time with a cold and then a warm pipeline cache (load time, first-300-frame times, hitches).
rem Results: logs\bench_suite\SCENE_N.json (compare two with python tools\benchmark\compare_results.py A.json B.json) and suite.json.
setlocal
cd /d "%~dp0..\.."
call "%CD%\tools\test_env.cmd"
set "PATH=%CD%\toolchain\venv\Scripts;%PATH%"
set "PYTHONUTF8=1"
python tools\benchmark\run_suite.py %*
exit /b %ERRORLEVEL%
