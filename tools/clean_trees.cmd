@echo off
rem Delete stale build trees (dry run unless -Apply). Options and rules: see the top of tools\clean_trees.ps1.
rem   tools\clean_trees.cmd [-Days N] [-Apply] [-AllWorktrees] [-IncludeMain]
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0clean_trees.ps1" %*
