# Deletes stale build trees (build-*) so they do not pile up: every agent run and worktree makes its own tree, 0.1 - 2 GB each.
# Dry run by default; add -Apply to delete.
#   tools\clean_trees.cmd                       list the trees of this checkout not built in for 14 days
#   tools\clean_trees.cmd -Days 3 -Apply        delete those not built in for 3 days
#   tools\clean_trees.cmd -AllWorktrees         look at every git worktree of the repository, not only this checkout
#   tools\clean_trees.cmd -IncludeMain          also consider build-msvc, build-clang, build-asan (kept by default: the gate uses them)
# A tree's age is the time of its last build (its .ninja_log), falling back to the folder time. A tree holding a junction or
# symlink is skipped (removing it could reach outside the tree). Deleting a tree only costs the next build there a rebuild
# (the shared compiler cache makes that fast); logs\ and the sources are untouched.
param(
    [int]$Days = 14,
    [switch]$Apply,
    [switch]$AllWorktrees,
    [switch]$IncludeMain
)
$ErrorActionPreference = 'Stop'
$here = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path

$roots = @($here)
if ($AllWorktrees) {
    $roots = @(git -C $here worktree list --porcelain | Where-Object { $_ -like 'worktree *' } |
        ForEach-Object { $_.Substring(9).Trim() -replace '/', '\' } | Where-Object { Test-Path $_ })
}
$protected = @('build-msvc', 'build-clang', 'build-asan')
$cutoff = (Get-Date).AddDays(-$Days)
$total = 0.0
$count = 0

foreach ($root in $roots) {
    foreach ($tree in Get-ChildItem -Path $root -Directory -Filter 'build-*' -Force -ErrorAction SilentlyContinue) {
        if (-not $IncludeMain -and $protected -contains $tree.Name) { continue }
        $log = Join-Path $tree.FullName '.ninja_log'
        $last = if (Test-Path $log) { (Get-Item $log).LastWriteTime } else { $tree.LastWriteTime }
        if ($last -gt $cutoff) { continue }
        $links = @(Get-ChildItem -Path $tree.FullName -Recurse -Force -Attributes ReparsePoint -ErrorAction SilentlyContinue)
        if ($links.Count -gt 0) {
            Write-Host ("skip   {0}: holds a junction or symlink ({1})" -f $tree.FullName, $links[0].FullName)
            continue
        }
        $mb = (Get-ChildItem -Path $tree.FullName -Recurse -File -Force -ErrorAction SilentlyContinue | Measure-Object Length -Sum).Sum / 1MB
        $total += $mb
        $count++
        if ($Apply) {
            # cmd's rmdir copes with long paths and never follows links.
            cmd /c "rmdir /s /q `"$($tree.FullName)`""
            Write-Host ("delete {0}  {1:N0} MB  (last build {2:yyyy-MM-dd})" -f $tree.FullName, $mb, $last)
        } else {
            Write-Host ("stale  {0}  {1:N0} MB  (last build {2:yyyy-MM-dd})" -f $tree.FullName, $mb, $last)
        }
    }
}
$verb = if ($Apply) { 'deleted' } else { 'would delete (add -Apply)' }
Write-Host ("{0} {1} trees, {2:N0} MB older than {3} days" -f $verb, $count, $total, $Days)
