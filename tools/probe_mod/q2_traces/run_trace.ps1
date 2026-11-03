<#
Q2 reference traces: one recorded run of the original game (FoC Steam x64 1.121).

  run_trace.ps1 -Scenario galactic|skirmish [options]

  -Scenario galactic   Galactic Conquest "Heart of the Maw", Empire, hands-off; stops after -Days fiscal cycles
  -Scenario skirmish   space skirmish, hands-off; stops after -Ticks logic ticks
  -Mod vanilla|tr      vanilla FoC (default) or Thrawn's Revenge (STEAMMOD=1125571106); -ModArg overrides
  -LuaProbe            also install the Lua probe (vanilla only; mod folder Mods\Q2Trace): galactic = story plot script,
                       skirmish = the game's GameScoring.lua with a read-only tactical probe appended. Its records are
                       collected by q2lua_reader.py (read-only memory scan, docs/re/LUA_SCRIPT_OUTPUT.md) into
                       lua_records.txt / lua_perception.tsv / lua_summary.json; run_status.json -> lua_canary / lua
  -Days 10 -Ticks 5400 -KeyEvery 300
  -WaitSessionMin 20   time allowed for menus + loading before the trace starts
  -TimeoutMin 30       hard limit for the traced part
  -NonInteractive      no prompts, no "press a key"; result in out\<run>\run_status.json and the exit code.
                       Also drives the menus (q2_ui.py) up to the start of the galaxy / battle, unless -NoDrive
  -Drive / -NoDrive    force the menu driver on / off (default: on with -NonInteractive)
  -DryRun              print every action, change nothing, start nothing
  -NoLaunch            do not start the game (attach to one started by hand, e.g. through Steam)
  -GameRoot <dir>      game folder (default: detected from Steam)
  -Label <text>        free text stored in the trace header (e.g. the skirmish map picked)

The reader closes the game when the trace is complete (or on timeout). Cleanup (cleanup.ps1) ALWAYS runs at
the end, also after errors or Ctrl+C; if this window itself is killed, run cleanup.cmd.

Exit codes: 0 complete trace, 10 partial trace (stopped early: see reason), 20 preflight failed (nothing
changed), 30 no trace (reader failed / no session), 40 cleanup left something (see cleanup.log).
#>
param(
    [ValidateSet('galactic', 'skirmish')][string]$Scenario = 'galactic',
    [ValidateSet('vanilla', 'tr', 'custom')][string]$Mod = 'vanilla',
    [string]$ModArg = '',
    [switch]$LuaProbe,
    [int]$Days = 10,
    [int]$Ticks = 5400,
    [int]$KeyEvery = 300,
    [double]$WaitSessionMin = 20,
    [double]$TimeoutMin = 30,
    [switch]$NonInteractive,
    [switch]$DryRun,
    [switch]$NoLaunch,
    [switch]$Drive,
    [switch]$NoDrive,
    [switch]$Windowed = $true,
    [string]$GameRoot = '',
    [string]$Label = ''
)
. (Join-Path (Split-Path -Parent $MyInvocation.MyCommand.Path) 'q2common.ps1')

$runId = (Get-Date).ToString('yyyyMMdd-HHmmss') + "-$Scenario-$Mod" + $(if ($LuaProbe) { '-lua' } else { '' })
$runDir = Join-Path $Q2Out $runId
$status = [ordered]@{ run_id = $runId; scenario = $Scenario; mod = $Mod; lua_probe = [bool]$LuaProbe; dry_run = [bool]$DryRun
                      state = 'preflight'; exit_code = $null; reason = ''; errors = @(); trace = ''; q1 = ''; summary = '' }
$statusPath = Join-Path $runDir 'run_status.json'
function Save-Status { if (-not $DryRun) { Write-Q2Json $statusPath ([pscustomobject]$status) } }
function Fail([int]$Code, [string]$Msg) { $status.errors += $Msg; $status.exit_code = $Code; $status.state = 'failed'; Write-Q2 "ERROR: $Msg"; Save-Status; exit $Code }

# ---------------------------------------------------------------- preflight (changes nothing)
if (-not $DryRun) { New-Item -ItemType Directory -Force -Path $runDir | Out-Null }
$root = Find-Q2GameRoot $GameRoot
if (-not $root) { Fail 20 "game not found (pass -GameRoot <folder containing corruption\StarWarsG.exe>)" }
$exe = Join-Path $root 'corruption\StarWarsG.exe'
$sha = Get-Q2Sha256 $exe
if ($sha -ne $Q2FocExeSha256) { Fail 20 "corruption\StarWarsG.exe is not the FoC 1.121 build the reader knows (sha256 $sha)" }
if (Get-Process -Name StarWarsG -ErrorAction SilentlyContinue) {
    if (-not $NoLaunch) { Fail 20 'StarWarsG.exe is already running: close it first (or use -NoLaunch to attach to it)' }
}
if (-not $NoLaunch -and -not (Get-Process -Name steam -ErrorAction SilentlyContinue)) { Fail 20 'Steam is not running: start Steam first' }
if ($LuaProbe -and $Mod -ne 'vanilla') { Fail 20 '-LuaProbe is for vanilla runs only' }
$useDriver = (($NonInteractive -or $Drive) -and -not $NoDrive -and -not $NoLaunch)
if ($useDriver -and $Mod -ne 'vanilla') { Fail 20 'the menu driver knows the vanilla menus only: use -NoDrive with -Mod' }
$folderBefore = Get-Q2GameFolderState $root
if (-not (Test-Path -LiteralPath $Q2Python)) { Fail 20 "project Python missing: $Q2Python (toolchain\2_setup.cmd)" }
$reader = Join-Path $Q2Bin 'q2reader.exe'
if (-not (Test-Path -LiteralPath $reader)) {
    Write-Q2 'building q2reader.exe (build_reader.cmd)'
    if (-not $DryRun) { & cmd /c "`"$(Join-Path $Q2Kit 'build_reader.cmd')`"" | Out-Null }
    if (-not $DryRun -and -not (Test-Path -LiteralPath $reader)) { Fail 20 'q2reader.exe build failed: see logs\q2reader_build.txt' }
}
$modDir = $null
$gameArgs = @()
switch ($Mod) {
    'vanilla' { }
    'tr' {
        $modDir = Get-Q2WorkshopDir $root $Q2TrWorkshopId
        if (-not (Test-Path -LiteralPath $modDir)) { Fail 20 "Thrawn's Revenge not found at $modDir (subscribe in the Workshop, or use -Mod custom -ModArg MODPATH=...)" }
        $gameArgs += "STEAMMOD=$Q2TrWorkshopId"
    }
    'custom' {
        if (-not $ModArg) { Fail 20 '-Mod custom needs -ModArg, e.g. MODPATH=Mods\ThrawnsRevenge' }
        $gameArgs += $ModArg
        if ($ModArg -match '^MODPATH=(.+)$') { $modDir = Join-Path (Join-Path $root 'corruption') $Matches[1] }
    }
}
if ($LuaProbe) { $gameArgs = @("MODPATH=Mods\$Q2ModName") + $gameArgs }
if ($Windowed) { $gameArgs += @('WINDOWED', 'SCREENWIDTH=1920', 'SCREENHEIGHT=1080') }  # the driver's 1920x1080 client space
if ($useDriver -and -not $Label) {
    $Label = $(if ($Scenario -eq 'galactic') { 'Heart of the Maw, Empire, Medium, advanced options Default (driven)' }
               else { '(2) Battle Over Geonosis, Empire vs Medium AI Rebellion, advanced options Default (driven)' })
}
$dayFrames = Get-Q2DayFrames $modDir
$trace = Join-Path $runDir 'trace.q2t'
$readerStatus = Join-Path $runDir 'reader_status.json'
$waitSec = [int]($WaitSessionMin * 60)
$timeoutSec = [int](($WaitSessionMin + $TimeoutMin) * 60)
$readerArgs = @('--out', $trace, '--status', $readerStatus, '--dump', (Join-Path $runDir 'playerlist_dump.txt'), '--scenario', $Scenario, '--key-every', $KeyEvery,
                '--queues', (Join-Path $runDir 'production.q2q'),
                '--wait-process-sec', $(if ($NoLaunch) { $waitSec } else { 180 }), '--wait-session-sec', $waitSec,
                '--timeout-sec', $timeoutSec, '--close-game',
                '--meta', "game=foc", '--meta', "mod=$Mod", '--meta', "mod_args=$($gameArgs -join ' ')",
                '--meta', "exe_sha256=$sha", '--meta', "lua_probe=$([int][bool]$LuaProbe)",
                '--meta', "game_folder=$($folderBefore.summary)", '--meta', "menu_driver=$([int][bool]$useDriver)")
if ($Scenario -eq 'galactic') {
    $readerArgs += @('--days', $Days, '--day-frames', $dayFrames, '--meta', 'campaign=Sandbox_Heart_Of_The_Maw_Empire')
} else {
    $readerArgs += @('--ticks', $Ticks, '--meta', 'skirmish=space 2 players, human Empire idle vs AI Rebel Medium')
}
if ($Label) { $readerArgs += @('--meta', "label=$Label") }
Write-Q2 "run $runId"
Write-Q2 "game    $root (sha256 ok)"
Write-Q2 "args    StarWarsG.exe $($gameArgs -join ' ')"
Write-Q2 "reader  q2reader.exe $($readerArgs -join ' ')"
$luaReaderArgs = @('-I', ('"' + (Join-Path $Q2Kit 'q2lua_reader.py') + '"'), 'run', '--run-dir', ('"' + $runDir + '"'),
                   '--wait-process-sec', $(if ($NoLaunch) { $waitSec } else { 180 }))
if ($LuaProbe) { Write-Q2 "lua     python $($luaReaderArgs -join ' ') (below-normal priority)" }
Write-Q2 "output  $runDir"
Write-Q2 "folder  $($folderBefore.summary)"
Write-Q2 "driver  $(if ($useDriver) { 'q2_ui.py drive ' + $Scenario } else { 'off (drive the menus by hand)' })"
$status.game_folder_before = $folderBefore
if ($DryRun) {
    if ($LuaProbe) { $dm = New-Q2Manifest $runId $root $runDir; Install-Q2Mod -GameRoot $root -ManifestPath '' -Manifest $dm -DryRun }
    Write-Q2 "dry run: would start the reader, launch the game, wait up to $([int]($WaitSessionMin + $TimeoutMin)) min, then run cleanup.ps1 -RunDir $runDir"
    exit 0
}

# ---------------------------------------------------------------- run (cleanup always follows)
$manifestPath = Join-Path $runDir 'manifest.json'
$manifest = New-Q2Manifest $runId $root $runDir
Write-Q2Json $manifestPath $manifest
$readerProc = $null
$readerExit = $null
$driverProc = $null
$uiStatus = Join-Path $runDir 'ui_status.json'
& $Q2Python -I (Join-Path $Q2Kit 'q2_profile.py') snapshot $runDir
if ($LASTEXITCODE -ne 0) { Fail 20 'profile snapshot failed (nothing changed yet)' }
try {
    if ($LuaProbe) { Install-Q2Mod -GameRoot $root -ManifestPath $manifestPath -Manifest $manifest }
    $status.state = 'running'; Save-Status
    $quoted = $readerArgs | ForEach-Object { $s = [string]$_; if ($s -match '[\s"]') { '"' + $s.Replace('"', '\"') + '"' } else { $s } }
    $readerProc = Start-Process -FilePath $reader -ArgumentList ($quoted -join ' ') -PassThru -WindowStyle Hidden
    $manifest.reader_pid = $readerProc.Id; Write-Q2Json $manifestPath $manifest
    if ($LuaProbe) {
        # Lua records: retail _CustomScriptMessage writes no file, so the probe keeps them in the game's memory
        $luaProc = Start-Process -FilePath $Q2Python -ArgumentList ($luaReaderArgs -join ' ') -PassThru -WindowStyle Hidden
        try { $luaProc.PriorityClass = [Diagnostics.ProcessPriorityClass]::BelowNormal } catch { }
        $manifest.lua_reader_pid = $luaProc.Id
        $manifest.lua_reader_start_utc = $luaProc.StartTime.ToUniversalTime().ToString('o')
        Write-Q2Json $manifestPath $manifest
        Write-Q2 "lua reader started (pid $($luaProc.Id))"
    }
    if (-not $NoLaunch) {
        $game = Start-Process -FilePath $exe -WorkingDirectory (Split-Path -Parent $exe) -ArgumentList ($gameArgs -join ' ') -PassThru
        $manifest.game_pid = $game.Id
        $manifest.game_start_utc = $game.StartTime.ToUniversalTime().ToString('o')
        Write-Q2Json $manifestPath $manifest
        Write-Q2 "game started (pid $($game.Id))"
        if ($useDriver) {
            $driverArgs = @('-I', ('"' + (Join-Path $Q2Kit 'q2_ui.py') + '"'), 'drive', $Scenario, '--shots', ('"' + (Join-Path $runDir 'shots') + '"'),
                            '--status', ('"' + $uiStatus + '"'), '--pid', $game.Id)
            $driverProc = Start-Process -FilePath $Q2Python -ArgumentList ($driverArgs -join ' ') -PassThru -WindowStyle Hidden
            Write-Q2 "menu driver started (pid $($driverProc.Id))"
        }
    }
    $steps = Join-Path $Q2Kit 'README.md'
    Write-Q2 "now drive the menus: README.md section 'UI steps' ($Scenario). Do NOT click inside the battle/galaxy once it runs."
    if (-not $NonInteractive) { Write-Host "`n  Follow the '$Scenario' UI steps in $steps. This window waits on its own.`n" }
    $deadline = (Get-Date).AddSeconds($timeoutSec + 120)
    $driverDone = $false
    while (-not $readerProc.HasExited) {
        Start-Sleep -Seconds 5
        if ($driverProc -and -not $driverDone -and $driverProc.HasExited) {
            $driverDone = $true
            $status.ui = Read-Q2Json $uiStatus
            $status.ui_exit = $driverProc.ExitCode
            if ($driverProc.ExitCode -ne 0) {
                $status.errors += "menu driver failed (exit $($driverProc.ExitCode)): $(if ($status.ui) { $status.ui.error })"
                Write-Q2 'menu driver failed: stopping the reader; cleanup closes the game'
                Stop-Process -Id $readerProc.Id -Force -ErrorAction SilentlyContinue
                break
            }
            Write-Q2 'menu driver reached the start and released the window'
        }
        $rs = Read-Q2Json $readerStatus
        if ($rs) {
            $status.reader = $rs; Save-Status
            Write-Q2 ("reader: {0} frames={1} mode_frame={2} cycles={3} pause={4}" -f $rs.state, $rs.frames_sampled, $rs.mode_frame, $rs.cycles_done, $rs.galactic_pause)
        }
        if ((Get-Date) -gt $deadline) { Write-Q2 'overall timeout: stopping the reader'; Stop-Process -Id $readerProc.Id -Force -ErrorAction SilentlyContinue; break }
    }
    $readerProc.WaitForExit()
    $readerExit = $readerProc.ExitCode
} catch {
    $status.errors += "run error: $_"
    Write-Q2 "ERROR: $_"
} finally {
    Write-Q2 'cleanup (always)'
    if ($driverProc -and -not $driverProc.HasExited) { Stop-Process -Id $driverProc.Id -Force -ErrorAction SilentlyContinue }
    if ($LuaProbe -and (Test-Path -LiteralPath (Join-Path $root "corruption\Mods\$Q2ModName"))) {
        $status.lua_canary = Get-Q2CanaryState $root
        Write-Q2 "lua canary: galactic $($status.lua_canary.galactic); scoring $($status.lua_canary.scoring)"
    }
    & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $Q2Kit 'cleanup.ps1') -RunDir $runDir
    $cleanExit = $LASTEXITCODE
    $status.cleanup_exit = $cleanExit
    $left = @(Get-Q2ProbeLeftovers $root)
    $status.probe_left_in_game_folder = $left
    if ($left.Count) { Write-Q2 "PROBE LEFT IN GAME FOLDER: $($left -join ', ')"; $status.cleanup_exit = 41 }
    & $Q2Python -I (Join-Path $Q2Kit 'q2_profile.py') restore $runDir
    $status.profile_restore_exit = $LASTEXITCODE
    $status.profile_restore = Read-Q2Json (Join-Path $runDir 'profile_restore.json')
    $status.game_folder_after = Get-Q2GameFolderState $root
    if ($status.game_folder_after.summary -ne $folderBefore.summary) { $status.errors += "game folder changed during the run (not by this kit): $($status.game_folder_after.summary)" }
    if (-not $status.ui -and (Test-Path -LiteralPath $uiStatus)) { $status.ui = Read-Q2Json $uiStatus }
}

# ---------------------------------------------------------------- post-process
$rs = Read-Q2Json $readerStatus
$status.reader = $rs
$status.reader_exit = $readerExit
$status.reason = $(if ($rs) { $rs.reason } else { 'reader_status_missing' })
if ((Test-Path -LiteralPath $trace) -and (Get-Item -LiteralPath $trace).Length -gt 0) {
    $status.trace = $trace
    $summary = Join-Path $runDir 'summary.json'
    $q1 = Join-Path $runDir 'reference.crc'
    & $Q2Python -I (Join-Path $Q2Kit 'q2trace.py') validate $trace --json $summary
    $status.validate_exit = $LASTEXITCODE
    & $Q2Python -I (Join-Path $Q2Kit 'q2trace.py') to-q1 $trace $q1
    $status.summary = $summary; $status.q1 = $q1
}
if ($LuaProbe) {
    # rebuilds the outputs from lua_reader.log, also when cleanup had to stop the reader before it wrote them
    & $Q2Python -I (Join-Path $Q2Kit 'q2lua_reader.py') summarize --run-dir $runDir | Out-Null
    $ls = Read-Q2Json (Join-Path $runDir 'lua_summary.json')
    if ($ls) {
        $status.lua = [ordered]@{ records = $ls.records; galactic = $ls.scripts.G.verdict; scoring = $ls.scripts.T.verdict
                                  plans = $ls.scripts.P.verdict; errors = @($ls.errors).Count }
        Write-Q2 "lua: galactic $($ls.scripts.G.verdict); scoring $($ls.scripts.T.verdict); plans $($ls.scripts.P.verdict)"
    }
}
$code = 30
if ($status.trace) { $code = $(if ($readerExit -eq 0) { 0 } else { 10 }) }
if ($status.cleanup_exit -ne 0 -or $status.profile_restore_exit -ne 0) { $code = 40 }
$status.exit_code = $code
$status.state = 'done'
Save-Status
Write-Q2 "done: exit $code reason=$($status.reason) -> $statusPath"
if (-not $NonInteractive) { Write-Host "`nSend back the folder $runDir (zip it).`n" }
exit $code
