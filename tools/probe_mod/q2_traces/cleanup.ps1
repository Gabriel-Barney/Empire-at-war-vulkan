<#
Q2 reference traces: put the game folder back exactly as it was. Safe to run any number of times.

  cleanup.ps1                 clean every run under out\ whose manifest is not marked cleaned
  cleanup.ps1 -RunDir <dir>   clean one run
  cleanup.ps1 -All            re-check every manifest, cleaned or not
  -DryRun                     print what would happen, change nothing
  -Force                      also delete probe files whose hash no longer matches (default: keep + report)

For each manifest (out\<run>\manifest.json) it:
  1. stops the q2reader.exe it started, and the StarWarsG.exe it launched (only that exact process:
     same pid AND same start time), first with a window close, then forcibly after 15 s; then waits up to 30 s
     for the Lua record reader (q2lua_reader.py, same pid + start-time check) and stops it if still running;
  2. moves an old-style Lua output (Q2Trace_Lua.txt) into the run dir, if one exists (retail writes none: the
     probe's records come from memory, docs/re/LUA_SCRIPT_OUTPUT.md);
  3. deletes every file it created, after checking the SHA-256 recorded at install time;
  4. removes the folders it created, deepest first, only if empty;
  5. verifies nothing it created is left and marks the manifest cleaned.
Only paths inside <game>\corruption\Mods\Q2Trace (and a Mods folder it created) are ever deleted.
Exit codes: 0 clean, 2 something was left in place (see out\<run>\cleanup.log), 1 error.
#>
param(
    [string]$RunDir,
    [switch]$All,
    [switch]$DryRun,
    [switch]$Force
)
. (Join-Path (Split-Path -Parent $MyInvocation.MyCommand.Path) 'q2common.ps1')

function Test-Q2Inside {
    param([string]$Path, [string]$GameRoot)
    $mods = [IO.Path]::GetFullPath((Join-Path $GameRoot 'corruption\Mods')).TrimEnd('\') + '\'
    $full = [IO.Path]::GetFullPath($Path)
    return ($full.StartsWith($mods + $Q2ModName, [StringComparison]::OrdinalIgnoreCase) -or
            $full.TrimEnd('\').Equals($mods.TrimEnd('\'), [StringComparison]::OrdinalIgnoreCase))
}

function Stop-Q2Process {
    param([int]$ProcId, [string]$Name, [string]$StartUtc, [System.Collections.Generic.List[string]]$Log, [switch]$DryRun)
    if ($ProcId -le 0) { return }
    $p = Get-Process -Id $ProcId -ErrorAction SilentlyContinue
    if (-not $p -or $p.ProcessName -ne $Name) { return }
    if ($StartUtc) {
        try {
            $st = $p.StartTime.ToUniversalTime()
            if ([Math]::Abs(($st - [DateTime]::Parse($StartUtc).ToUniversalTime()).TotalSeconds) -gt 5) { return }
        } catch { return }
    }
    $Log.Add("stop $Name pid $ProcId")
    if ($DryRun) { return }
    try { [void]$p.CloseMainWindow() } catch { }
    if (-not $p.WaitForExit(15000)) { Stop-Process -Id $ProcId -Force -ErrorAction SilentlyContinue }
}

function Invoke-Q2CleanupOne {
    param([string]$ManifestPath)
    $m = Read-Q2Json $ManifestPath
    if (-not $m) { Write-Q2 "unreadable manifest $ManifestPath"; return 1 }
    $log = New-Object System.Collections.Generic.List[string]
    $left = 0
    $log.Add("cleanup of run $($m.run_id) at $((Get-Date).ToUniversalTime().ToString('o'))$(if ($DryRun) {' (dry run)'})")
    # 1. processes
    Stop-Q2Process -ProcId ([int]$m.reader_pid) -Name 'q2reader' -StartUtc '' -Log $log -DryRun:$DryRun
    Stop-Q2Process -ProcId ([int]$m.game_pid) -Name 'StarWarsG' -StartUtc $m.game_start_utc -Log $log -DryRun:$DryRun
    # the Lua record reader (python, no window) ends by itself once the game is gone: give it 30 s to write its files
    $lp = $(if ([int]$m.lua_reader_pid -gt 0) { Get-Process -Id ([int]$m.lua_reader_pid) -ErrorAction SilentlyContinue })
    if ($lp -and $lp.ProcessName -eq 'python' -and $m.lua_reader_start_utc -and
        [Math]::Abs(($lp.StartTime.ToUniversalTime() - [DateTime]::Parse($m.lua_reader_start_utc).ToUniversalTime()).TotalSeconds) -le 5) {
        $log.Add("wait for the lua reader pid $($lp.Id)")
        if (-not $DryRun -and -not $lp.WaitForExit(30000)) {
            $log.Add("stop lua reader pid $($lp.Id)")
            Stop-Process -Id $lp.Id -Force -ErrorAction SilentlyContinue
        }
    }
    # 2. Lua output written by the game (_CustomScriptMessage): look under corruption\ (2 levels).
    $corr = Join-Path $m.game_root 'corruption'
    if (Test-Path -LiteralPath $corr) {
        $since = [DateTime]::Parse($m.started_utc).ToUniversalTime()
        $found = @(Get-ChildItem -LiteralPath $corr -Filter $m.lua_output_name -File -ErrorAction SilentlyContinue)
        $found += @(Get-ChildItem -LiteralPath $corr -Directory -ErrorAction SilentlyContinue | Where-Object { $_.Name -ne 'Mods' } |
                    ForEach-Object { Get-ChildItem -LiteralPath $_.FullName -Filter $m.lua_output_name -File -ErrorAction SilentlyContinue })
        # Where _CustomScriptMessage writes is not documented: also look in the game root and the player's
        # saved-games folder (top level + one level).
        $found += @(Get-ChildItem -LiteralPath $m.game_root -Filter $m.lua_output_name -File -ErrorAction SilentlyContinue)
        $saved = Join-Path $env:USERPROFILE 'Saved Games\Petroglyph\Empire At War - Forces of Corruption'
        if (Test-Path -LiteralPath $saved) {
            $found += @(Get-ChildItem -LiteralPath $saved -Filter $m.lua_output_name -File -ErrorAction SilentlyContinue)
            $found += @(Get-ChildItem -LiteralPath $saved -Directory -ErrorAction SilentlyContinue |
                        ForEach-Object { Get-ChildItem -LiteralPath $_.FullName -Filter $m.lua_output_name -File -ErrorAction SilentlyContinue })
        }
        foreach ($f in $found) {
            if ($f.LastWriteTimeUtc -lt $since) { $log.Add("keep older $($f.FullName) (not from this run)"); continue }
            $dst = Join-Path $m.run_dir ("lua_" + $f.Directory.Name + "_" + $f.Name)
            $log.Add("move $($f.FullName) -> $dst")
            if (-not $DryRun) { Copy-Item -LiteralPath $f.FullName -Destination $dst -Force; Remove-Item -LiteralPath $f.FullName -Force }
        }
    }
    # 3. files
    foreach ($f in @($m.files_created)) {
        if (-not $f) { continue }
        if (-not (Test-Q2Inside $f.path $m.game_root)) { $log.Add("SKIP outside probe folder: $($f.path)"); $left++; continue }
        if (-not (Test-Path -LiteralPath $f.path)) { $log.Add("gone   $($f.path)"); continue }
        $h = Get-Q2Sha256 $f.path
        if ($h -ne $f.sha256 -and -not $Force) { $log.Add("KEPT (hash changed, use -Force) $($f.path)"); $left++; continue }
        $log.Add("delete $($f.path)")
        if (-not $DryRun) { Remove-Item -LiteralPath $f.path -Force }
    }
    # 4. folders, deepest first, only if empty
    foreach ($d in (@($m.dirs_created) | Where-Object { $_ } | Sort-Object { $_.Length } -Descending)) {
        if (-not (Test-Q2Inside $d $m.game_root)) { $log.Add("SKIP outside probe folder: $d"); $left++; continue }
        if (-not (Test-Path -LiteralPath $d)) { $log.Add("gone   $d\"); continue }
        if (@(Get-ChildItem -LiteralPath $d -Force).Count -gt 0) {
            if ($DryRun) { $log.Add("rmdir  $d\ (after its files)"); continue }
            $log.Add("KEPT (not empty) $d\"); $left++; continue
        }
        $log.Add("rmdir  $d\")
        if (-not $DryRun) { Remove-Item -LiteralPath $d -Force }
    }
    # 5. verify
    if (-not $DryRun) {
        foreach ($f in @($m.files_created)) { if ($f -and (Test-Path -LiteralPath $f.path)) { $log.Add("STILL PRESENT $($f.path)"); $left++ } }
        $m.cleanup_runs = [int]$m.cleanup_runs + 1
        $m.cleaned = ($left -eq 0)
        Write-Q2Json $ManifestPath $m
        [IO.File]::AppendAllText((Join-Path $m.run_dir 'cleanup.log'), (($log -join "`n") + "`nresult " + $(if ($left) { "LEFT $left" } else { 'CLEAN' }) + "`n`n"))
    }
    $log | ForEach-Object { Write-Q2 $_ }
    Write-Q2 $(if ($left) { "run $($m.run_id): $left item(s) left in place" } else { "run $($m.run_id): clean" })
    if ($left) { return 2 } else { return 0 }
}

$code = 0
try {
    $manifests = @()
    if ($RunDir) {
        $manifests = @((Join-Path $RunDir 'manifest.json'))
    } elseif (Test-Path -LiteralPath $Q2Out) {
        foreach ($mf in (Get-ChildItem -LiteralPath $Q2Out -Directory | ForEach-Object { Join-Path $_.FullName 'manifest.json' })) {
            if (-not (Test-Path -LiteralPath $mf)) { continue }
            $m = Read-Q2Json $mf
            if ($All -or ($m -and -not $m.cleaned)) { $manifests += $mf }
        }
    }
    if ($manifests.Count -eq 0) { Write-Q2 'nothing to clean' }
    foreach ($mf in $manifests) {
        $r = Invoke-Q2CleanupOne $mf
        if ($r -gt $code) { $code = $r }
    }
} catch {
    Write-Q2 "cleanup error: $_"
    $code = 1
}
exit $code
