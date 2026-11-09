<#
a2m t07 Rebel slice run: one hands-off Galactic Conquest of the original game (FoC Steam x64 1.121) with the Rebel
Advance_Tech_Rebel plan hooked (Mods\Q2RebelSlice, make_slice_mod.py) and the Rebel credits / unlocked list watched in
memory (slice_watch.py). The q2 galactic trace runs alongside it unchanged (run_trace.ps1 -Mod custom -NoDrive).

  run_rebel_slice.ps1 [-Days 12] [-NoLaunch] [-DryRun] [-GameRoot DIR] [-WaitSessionMin 20] [-TimeoutMin 30]

Steps: preflight (changes nothing) -> build Mods\Q2RebelSlice in <game>\corruption\Mods -> start q2lua_reader.py and
slice_watch.py (read-only, hidden) -> run_trace.ps1 -Scenario galactic -Mod custom -ModArg MODPATH=Mods\Q2RebelSlice
-NoDrive -Days N -NonInteractive (launches the game WINDOWED 1920x1080 unless -NoLaunch; its reader closes the game
after N fiscal cycles; its cleanup restores the profile) -> wait for both readers -> remove Mods\Q2RebelSlice (always,
also after errors) -> slice_report.py.
Menus by hand: Galactic Conquest > Heart of the Maw > Empire > Medium, defaults; then hands off (README "UI steps").
Output: tools\probe_mod\q2_traces\out\<ts>-rebel-slice\ (lua_records.txt, slice_watch.jsonl, slice_report.txt,
slice_plans.tsv, rebel_slice_status.json; trace_dir in the status names the q2 trace folder). rebel_slice\out\latest.txt
names the folder. Exit: 0 report written, 20 preflight failed (nothing changed), 30 no records, 40 mod folder left.
#>
param(
    [int]$Days = 12,
    [switch]$NoLaunch,
    [switch]$DryRun,
    [string]$GameRoot = '',
    [double]$WaitSessionMin = 20,
    [double]$TimeoutMin = 30
)
$ErrorActionPreference = 'Continue'
$Here = Split-Path -Parent $MyInvocation.MyCommand.Path
. (Join-Path (Split-Path -Parent $Here) 'q2common.ps1')

$ModName = 'Q2RebelSlice'
$runId = (Get-Date).ToString('yyyyMMdd-HHmmss') + '-rebel-slice'
$runDir = Join-Path $Q2Out $runId
$statusPath = Join-Path $runDir 'rebel_slice_status.json'
$status = [ordered]@{ run_id = $runId; state = 'preflight'; exit_code = $null; days = $Days; no_launch = [bool]$NoLaunch
                      mod = "MODPATH=Mods\$ModName"; trace_dir = ''; trace_exit = $null; errors = @(); report = '' }
function Save-Status { if (-not $DryRun) { Write-Q2Json $statusPath ([pscustomobject]$status) } }
function Fail([int]$Code, [string]$Msg) { $status.errors += $Msg; $status.exit_code = $Code; $status.state = 'failed'; Write-Q2 "ERROR: $Msg"; Save-Status; exit $Code }
function Py([string[]]$A) { & $Q2Python -I @A | ForEach-Object { Write-Host $_ }; return $LASTEXITCODE }
function Q([string]$s) { '"' + $s + '"' }

# ---------------------------------------------------------------- preflight (changes nothing)
if (-not $DryRun) { New-Item -ItemType Directory -Force -Path $runDir | Out-Null }
$root = Find-Q2GameRoot $GameRoot
if (-not $root) { Fail 20 'game not found (pass -GameRoot <folder containing corruption\StarWarsG.exe>)' }
if (-not (Test-Path -LiteralPath $Q2Python)) { Fail 20 "project Python missing: $Q2Python (toolchain\2_setup.cmd)" }
if (-not $NoLaunch -and (Get-Process -Name StarWarsG -ErrorAction SilentlyContinue)) { Fail 20 'StarWarsG.exe is already running: close it first (or -NoLaunch)' }
$modDir = Join-Path $root "corruption\Mods\$ModName"
if ((Test-Path -LiteralPath $modDir) -and -not (Test-Path -LiteralPath (Join-Path $modDir 'Q2REBELSLICE_MARKER.txt'))) {
    Fail 20 "$modDir exists and is not this kit's (no marker): move it away first"
}
$traceArgs = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', (Q (Join-Path $Q2Kit 'run_trace.ps1')), '-Scenario', 'galactic',
               '-Mod', 'custom', '-ModArg', "MODPATH=Mods\$ModName", '-NoDrive', '-Days', $Days, '-NonInteractive',
               '-WaitSessionMin', $WaitSessionMin, '-TimeoutMin', $TimeoutMin,
               '-Label', (Q "a2m t07 rebel slice: Heart of the Maw, Empire, Medium, hands-off, $Days days (menus by hand)"))
if ($NoLaunch) { $traceArgs += '-NoLaunch' }
if ($GameRoot) { $traceArgs += @('-GameRoot', (Q $GameRoot)) }
$waitSec = [int](($WaitSessionMin + 5) * 60)
$luaArgs = @('-I', (Q (Join-Path $Q2Kit 'q2lua_reader.py')), 'run', '--run-dir', (Q $runDir), '--wait-process-sec', $waitSec)
$watchArgs = @('-I', (Q (Join-Path $Here 'slice_watch.py')), 'run', '--run-dir', (Q $runDir), '--wait-process-sec', $waitSec)
Write-Q2 "run     $runId"
Write-Q2 "game    $root"
Write-Q2 "mod     $modDir (make_slice_mod.py; removed at the end)"
Write-Q2 "lua     python $($luaArgs -join ' ')"
Write-Q2 "watch   python $($watchArgs -join ' ')"
Write-Q2 "trace   powershell $($traceArgs -join ' ')"
Write-Q2 "output  $runDir"
if ($DryRun) { Write-Q2 'dry run: nothing built, started or changed'; exit 0 }
$latest = Join-Path $Here 'out\latest.txt'
New-Item -ItemType Directory -Force -Path (Split-Path -Parent $latest) | Out-Null
Set-Content -LiteralPath $latest -Value $runDir -Encoding ascii

# ---------------------------------------------------------------- run (the mod is always removed)
$t0 = Get-Date
$luaProc = $null; $watchProc = $null
try {
    if ((Py @((Join-Path $Here 'make_slice_mod.py'), '--out', $modDir)) -ne 0) { throw 'make_slice_mod.py --out failed' }
    $status.state = 'running'; Save-Status
    $luaProc = Start-Process -FilePath $Q2Python -ArgumentList ($luaArgs -join ' ') -PassThru -WindowStyle Hidden
    try { $luaProc.PriorityClass = [Diagnostics.ProcessPriorityClass]::BelowNormal } catch { }
    $watchProc = Start-Process -FilePath $Q2Python -ArgumentList ($watchArgs -join ' ') -PassThru -WindowStyle Hidden
    Write-Q2 "lua reader pid $($luaProc.Id), slice watcher pid $($watchProc.Id)"
    Write-Host ''
    Write-Host '  In the game: Galactic Conquest > Heart of the Maw > Empire > Medium (defaults) > Start. Close the'
    Write-Host '  objectives popup, then hands off. Answer a battle prompt with Auto-Resolve within 60 s.'
    Write-Host "  The game closes itself after $Days fiscal cycles (about $([int]($Days * 45 / 60) + 1) min of galaxy time)."
    Write-Host "  Live: $runDir\slice_live.json"
    Write-Host ''
    $tp = Start-Process -FilePath 'powershell.exe' -ArgumentList ($traceArgs -join ' ') -PassThru -NoNewWindow -Wait
    $status.trace_exit = $tp.ExitCode
    $td = Get-ChildItem -LiteralPath $Q2Out -Directory -Filter '*-galactic-custom' | Where-Object { $_.CreationTime -ge $t0.AddSeconds(-5) } |
          Sort-Object CreationTime | Select-Object -Last 1
    if ($td) { $status.trace_dir = $td.FullName }
    Write-Q2 "run_trace.ps1 exit $($tp.ExitCode) ($(if ($td) { $td.FullName } else { 'no trace folder' }))"
    foreach ($p in @($luaProc, $watchProc)) { if ($p -and -not $p.WaitForExit(90000)) { $status.errors += "reader pid $($p.Id) still running: stopped"; Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue } }
} catch {
    $status.errors += "run error: $_"
    Write-Q2 "ERROR: $_"
} finally {
    if (Get-Process -Name StarWarsG -ErrorAction SilentlyContinue) { Write-Q2 'the game still runs: the mod folder is removed after it exits (waiting up to 5 min)'; Wait-Process -Name StarWarsG -Timeout 300 -ErrorAction SilentlyContinue }
    foreach ($p in @($luaProc, $watchProc)) { if ($p -and -not $p.HasExited) { Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue } }
    & $Q2Python -I (Join-Path $Here 'make_slice_mod.py') --remove $modDir
    $status.mod_left = [bool](Test-Path -LiteralPath $modDir)
    if ($status.mod_left) { Write-Q2 "MOD LEFT IN GAME FOLDER: $modDir (delete it by hand)" }
}

# ---------------------------------------------------------------- report
$recs = Join-Path $runDir 'lua_records.txt'
if (Test-Path -LiteralPath $recs) {
    $ra = @((Join-Path $Here 'slice_report.py'), '--run-dir', $runDir)
    if ($status.trace_dir) { $ra += @('--trace-dir', $status.trace_dir) }
    $null = Py $ra
    $status.report = Join-Path $runDir 'slice_report.txt'
    if (Test-Path -LiteralPath $status.report) { Get-Content -LiteralPath $status.report | Write-Host }
}
$status.exit_code = $(if ($status.mod_left) { 40 } elseif (-not $status.report -or -not (Select-String -LiteralPath $recs -Pattern ' PSTART ' -Quiet)) { 30 } else { 0 })
$status.state = 'done'
Save-Status
Write-Q2 "done: exit $($status.exit_code), folder $runDir"
exit $status.exit_code
