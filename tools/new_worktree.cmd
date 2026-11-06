@echo off
rem Create a git worktree for an agent with junctions to Data, extracted, toolchain and roadmap-store (tools\worktree\worktree_tool.py).
rem Usage: tools\new_worktree.cmd <name> [--base main] [--fetch] [--no-sync] [--modules "core;x"]
python "%~dp0worktree\worktree_tool.py" new %*
exit /b %ERRORLEVEL%
