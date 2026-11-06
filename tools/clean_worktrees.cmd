@echo off
rem List (dry run) or remove (--apply) merged worktrees and stale build trees (tools\worktree\worktree_tool.py).
rem Usage: tools\clean_worktrees.cmd [--apply] [--days N] [--keep a,b] [--only a,b] [--pushed] [--trees] [--main] [--no-size]
python "%~dp0worktree\worktree_tool.py" clean %*
exit /b %ERRORLEVEL%
