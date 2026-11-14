<#
Parity reference capture (roadmap group q4d): several hands-off runs of the original game per Q4 parity scenario, so
the parity harness can compare distributions (tools/parity/README.md, "Distribution mode"; docs/DECISIONS.md "Parity
tiers", tier 2). Each run is one ordinary Q2 trace (tools\probe_mod\q2_traces\run_trace.ps1 -NonInteractive: it drives
the menus, records, closes the game and runs its own cleanup); this script only repeats it and lists the results.

  run_capture.ps1 [-Scenario galactic|skirmish|both] [-Runs 6] [-NoLuaProbe] [-DryRun] [-PauseSec 20] [-KeepGoing]

  -Scenario both    galactic = Heart of the Maw, Empire, 10 fiscal cycles (hotm_galactic, about 12 min a run);
                    skirmish = (2) Battle Over Geonosis, Empire vs Medium AI Rebel, 5400 ticks (geonosis_skirmish,
                    about 6 min a run). both = all galactic runs, then all skirmish runs
  -Runs 6           runs per scenario (6 + 6: about 1 h 50 min)
  -NoLuaProbe       plain traces (default: with the Lua probe, as the scenarios' newest reference runs)
  -DryRun           print what each run would do (run_trace.ps1 -DryRun), start nothing, change nothing
  -PauseSec 20      wait between runs, for the game process and Steam to settle
  -KeepGoing        go on after a failed run (default: stop the batch at the first run that is not complete)

Output: out\<stamp>\batch.json (each run: id, scenario, exit code, reason) and out\<stamp>\add_refs.cmd, which adds the
complete runs (exit 0) to tools\parity\scenarios.json with `parity_harness.py add-refs`. The traces themselves are in
tools\probe_mod\q2_traces\out\<run>\ as for any Q2 run. Exit codes: 0 every run complete, 10 some run partial or
failed, 20 preflight failed (nothing started), 40 cleanup left something in the game folder (run cleanup.cmd).
#>
param(
    [ValidateSet('galactic', 'skirmish', 'both')][string]$Scenario = 'both',
    [ValidateRange(1, 50)][int]$Runs = 6,
    [switch]$NoLuaProbe,
    [switch]$DryRun,
    [int]$PauseSec = 20,
    [switch]$KeepGoing
)
$ErrorActionPreference = 'Stop'
$Kit = Split-Path -Parent $MyInvocation.MyCommand.Path
$Root = (Resolve-Path (Join-Path $Kit '..\..\..')).Path
$Q2Kit = Join-Path $Root 'tools\probe_mod\q2_traces'
$Q2Out = Join-Path $Q2Kit 'out'
$Python = Join-Path $Root 'toolchain\venv\Scripts\python.exe'
$Harness = Join-Path $Root 'tools\parity\parity_harness.py'
$ScenarioIds = @{ galactic = 'hotm_galactic'; skirmish = 'geonosis_skirmish' }

function Say([string]$Msg) { Write-Host "[parity_refs] $Msg" }

$stamp = (Get-Date).ToString('yyyyMMdd-HHmmss')
$batchDir = Join-Path $Kit "out\$stamp"
$kinds = $(if ($Scenario -eq 'both') { @('galactic', 'skirmish') } else { @($Scenario) })

# ---- preflight (changes nothing)
foreach ($p in @($Python, $Harness, (Join-Path $Q2Kit 'run_trace.ps1'), (Join-Path $Q2Kit 'bin\q2reader.exe'))) {
    if (-not (Test-Path -LiteralPath $p)) {
        $msg = "$p is missing$(if ($p -like '*q2reader.exe') { ' (double-click tools\probe_mod\q2_traces\build_reader.cmd once)' })"
        if ($DryRun) { Say "WARNING (dry run): $msg" } else { Say "ERROR: $msg"; exit 20 }
    }
}
if (-not $DryRun) {
    if (Get-Process -Name StarWarsG -ErrorAction SilentlyContinue) { Say 'ERROR: the game (StarWarsG.exe) is already running; close it first'; exit 20 }
    if (-not (Get-Process -Name steam -ErrorAction SilentlyContinue)) { Say 'ERROR: start Steam first'; exit 20 }
}

$minutes = 0
foreach ($k in $kinds) { $minutes += $Runs * $(if ($k -eq 'galactic') { 12 } else { 6 }) }
Say "$Runs run(s) of $($kinds -join ' and ') with$(if ($NoLuaProbe) { 'out' }) the Lua probe, about $minutes min. Do not touch the game window."
if (-not $DryRun) { New-Item -ItemType Directory -Force -Path $batchDir | Out-Null }
$batch = [ordered]@{ stamp = $stamp; scenario = $Scenario; runs_per_scenario = $Runs; lua_probe = -not $NoLuaProbe
                     dry_run = [bool]$DryRun; runs = @() }
function Save-Batch { if (-not $DryRun) { $batch | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath (Join-Path $batchDir 'batch.json') -Encoding utf8 } }
Save-Batch

$code = 0
:outer foreach ($k in $kinds) {
    for ($i = 1; $i -le $Runs; $i++) {
        Say "== $k run $i of $Runs"
        $before = @(Get-ChildItem -LiteralPath $Q2Out -Directory -ErrorAction SilentlyContinue | ForEach-Object Name)
        $trArgs = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', (Join-Path $Q2Kit 'run_trace.ps1'), '-Scenario', $k, '-NonInteractive')
        if (-not $NoLuaProbe) { $trArgs += '-LuaProbe' }
        if ($DryRun) { $trArgs += '-DryRun' }
        & powershell.exe @trArgs
        $exit = $LASTEXITCODE
        $new = @(Get-ChildItem -LiteralPath $Q2Out -Directory -ErrorAction SilentlyContinue |
                 Where-Object { $before -notcontains $_.Name -and $_.Name -like "*-$k-*" } | ForEach-Object Name)
        $run = $(if ($new.Count) { $new[-1] } else { '' })
        $reason = ''
        if ($run) {
            try { $st = Get-Content -LiteralPath (Join-Path $Q2Out "$run\run_status.json") -Raw | ConvertFrom-Json; $reason = $st.reason } catch { }
        }
        $batch.runs += [ordered]@{ scenario = $ScenarioIds[$k]; run = $run; exit_code = $exit; reason = $reason }
        Save-Batch
        Say "   run '$run' exit $exit $reason"
        if ($exit -eq 40) { $code = 40; break outer }
        if ($exit -ne 0) {
            $code = [Math]::Max($code, 10)
            if (-not $KeepGoing) { Say 'stopping the batch (a run was not complete; -KeepGoing to go on)'; break outer }
        }
        if (-not $DryRun) {
            # run_trace.ps1 already cleaned up; make sure the game is gone before the next launch.
            for ($w = 0; $w -lt 60 -and (Get-Process -Name StarWarsG -ErrorAction SilentlyContinue); $w++) { Start-Sleep -Seconds 1 }
            if (Get-Process -Name StarWarsG -ErrorAction SilentlyContinue) { Say 'ERROR: the game is still running 60 s after the run; stopping'; $code = [Math]::Max($code, 10); break outer }
            Start-Sleep -Seconds $PauseSec
        }
    }
}

# ---- the command that adds the complete runs to tools\parity\scenarios.json
$lines = @('@echo off', 'rem Adds this batch''s complete runs to tools\parity\scenarios.json (written by run_capture.ps1).', 'setlocal')
foreach ($sid in ($batch.runs | Where-Object { $_.exit_code -eq 0 -and $_.run } | ForEach-Object { $_.scenario } | Select-Object -Unique)) {
    $ids = @($batch.runs | Where-Object { $_.exit_code -eq 0 -and $_.run -and $_.scenario -eq $sid } | ForEach-Object { $_.run })
    $lines += "`"$Python`" -I `"$Harness`" add-refs --scenario $sid --note `"parity_refs batch $stamp`" $($ids -join ' ')"
    $lines += 'if errorlevel 1 exit /b 1'
}
$lines += 'exit /b 0'
if (-not $DryRun) {
    [IO.File]::WriteAllText((Join-Path $batchDir 'add_refs.cmd'), (($lines -join "`r`n") + "`r`n"), (New-Object Text.ASCIIEncoding))
    Say "batch: $(Join-Path $batchDir 'batch.json')"
    Say "add the complete runs to the harness: $(Join-Path $batchDir 'add_refs.cmd') (or leave it to Claude)"
}
$ok = @($batch.runs | Where-Object { $_.exit_code -eq 0 }).Count
Say "done: $ok of $($batch.runs.Count) run(s) complete, exit $code"
exit $code
