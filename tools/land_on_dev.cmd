@echo off
rem Land the current branch on the long-lived dev branch without touching any other checkout.
rem Gabriel, 2026-11-06: no branch per task / group / session (CLAUDE.md "Branches", docs/DECISIONS.md "Branching").
rem
rem Usage (from any worktree, branch committed):  tools\land_on_dev.cmd [--create]
rem   1. refuses on main, on detached HEAD and with uncommitted tracked edits
rem   2. fetches origin; with --create makes origin/dev from origin/main when dev does not exist yet
rem   3. merges origin/dev INTO THIS BRANCH (conflicts are resolved here, so dev never holds a broken merge)
rem   4. refuses generated or oversized new files (tools\lint\check_tracked.py)
rem   5. refuses a merged HEAD that has no passing MSVC build of every module with the fast tier (tools\lint\verified_build.py;
rem      runs build_agent.cmd msvc wt all itself when there is none; docs / *.md / .claude-only changes need none), then
rem      pushes HEAD to origin dev as a plain fast-forward (never --force; a rejected push means dev moved: rerun)
rem   6. deletes this branch on origin when it is not dev
rem Run the build / tests the change needs (docs/BUILD.md) BEFORE this; dev must stay green. Step 5 covers only the
rem MSVC fast tier: clang, ASan and slow tests are still yours to run when the change needs them.
rem The local branch and its worktree are left alone (git cannot delete a checked-out branch); the app or
rem "git branch -d <name>" from another checkout removes them once the work is on dev.
setlocal
set "CREATE=0"
if /i "%~1"=="--create" set "CREATE=1"

for /f "delims=" %%b in ('git rev-parse --abbrev-ref HEAD') do set "BR=%%b"
if "%BR%"=="" goto :nobranch
if "%BR%"=="HEAD" goto :nobranch
if /i "%BR%"=="main" (
  echo land_on_dev: refusing to run on main.
  exit /b 2
)

for /f "delims=" %%s in ('git status --porcelain -uno') do (
  echo land_on_dev: uncommitted tracked changes, commit first:
  git status --short -uno
  exit /b 2
)

git fetch origin
if errorlevel 1 (
  echo land_on_dev: git fetch failed, nothing changed.
  exit /b 1
)

git ls-remote --exit-code --heads origin dev >nul 2>&1
if errorlevel 1 (
  if "%CREATE%"=="1" (
    echo land_on_dev: dev does not exist on origin, creating it from origin/main.
    git push origin refs/remotes/origin/main:refs/heads/dev
    if errorlevel 1 exit /b 1
    git fetch origin
  ) else (
    echo land_on_dev: origin has no dev branch. Run once with --create to make it from origin/main.
    exit /b 2
  )
)

if /i "%BR%"=="dev" goto :pushit

git merge --no-edit origin/dev
if errorlevel 1 (
  echo.
  echo land_on_dev: merge conflict with origin/dev. Nothing was pushed.
  echo Resolve here, commit, rebuild the affected modules, then rerun this script.
  exit /b 1
)

:pushit
rem Refuse generated or oversized files this landing would add (tools\lint\check_tracked.py; exceptions in
rem tools\lint\tracked_allow.txt). Nothing is pushed when it fails.
set "PY=python"
if exist "%~dp0..\toolchain\venv\Scripts\python.exe" set "PY=%~dp0..\toolchain\venv\Scripts\python.exe"
"%PY%" -I "%~dp0lint\check_tracked.py" --base origin/dev --head HEAD
if errorlevel 1 (
  echo.
  echo land_on_dev: new files failed the tracked-file check above. Nothing was pushed.
  exit /b 1
)

rem dev must stay green (2026-11-12): push only a commit that was built and tested AS MERGED. Two branches that each pass
rem can still break dev together, so a pass before the merge with origin/dev does not count. tools\lint\verified_build.py
rem accepts a build-wt-msvc or build-msvc stamp for exactly HEAD (every module, fast tier or all tests; build_agent.cmd and
rem build_msvc.cmd write it), or a change to docs / *.md / .claude only. Otherwise this runs the fast tier itself.
"%PY%" -I "%~dp0lint\verified_build.py" land-check --base origin/dev --head HEAD --tree build-wt-msvc --tree build-msvc
if errorlevel 1 (
  echo land_on_dev: building and testing the merged HEAD: build_agent.cmd msvc wt all, the fast tier. This takes a while.
  call "%~dp0..\build_agent.cmd" msvc wt all
  cd /d "%~dp0.."
  "%PY%" -I "%~dp0lint\verified_build.py" land-check --base origin/dev --head HEAD --tree build-wt-msvc
  if errorlevel 1 (
    echo.
    echo land_on_dev: the merged HEAD did not pass the MSVC build and fast tier above. Nothing was pushed.
    echo Fix it here, commit, then rerun this script. If the failure is already on origin/dev, say so to Gabriel and fix dev first.
    exit /b 1
  )
)

git push origin HEAD:refs/heads/dev
if errorlevel 1 (
  echo.
  echo land_on_dev: push rejected. Nothing was pushed and your branch is intact. If git said non-fast-forward, dev moved since the fetch: rerun this script. Otherwise read the git message above.
  exit /b 1
)

if /i not "%BR%"=="dev" (
  git ls-remote --exit-code --heads origin "%BR%" >nul 2>&1
  if not errorlevel 1 git push origin --delete "%BR%"
)

echo.
echo land_on_dev: %BR% is on origin/dev.
if /i not "%BR%"=="dev" echo Local branch %BR% is now redundant: remove it with "git branch -d %BR%" from another checkout once this worktree is closed.
exit /b 0

:nobranch
echo land_on_dev: detached HEAD, check out a branch first.
exit /b 2
