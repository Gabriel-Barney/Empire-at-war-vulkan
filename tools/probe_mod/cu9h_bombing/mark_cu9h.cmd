@echo off
rem cu9h probe: prints the bombing-run state of the running game (phase, button, dial, ready_in, targeting, blob
rem colour); with a label (q1_..., q2_..., q3_..., no spaces) it also records it in out\<latest run>\marks.log.
"%~dp0..\..\..\toolchain\venv\Scripts\python.exe" -I "%~dp0cu9h_mark.py" %*
