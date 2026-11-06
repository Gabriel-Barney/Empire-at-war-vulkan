@echo off
rem Land the current branch on the long-lived dev branch without touching any other checkout.
rem Gabriel, 2026-11-06: no branch per task / group / session (CLAUDE.md "Branches", docs/DECISIONS.md "Branching").
rem
rem Usage (from any worktree, branch committed):  tools\land_on_dev.cmd [--create]
rem   1. refuses on main, on detached HEAD and with uncommitted tracked edits
rem   2. fetches origin; with --create makes origin/dev from origin/main when dev does not exist yet
rem   3. merges origin/dev INTO THIS BRANCH (conflicts are resolved here, so dev never holds a broken merge)
rem   4. pushes HEAD to origin dev as a plain fast-forward (never --force; a rejected push means dev moved: rerun)
rem   5. deletes this branch on origin when it is not dev
rem Run the build / tests the change needs (docs/BUILD.md) BEFORE this; dev must stay green.
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
