# Shared helpers for the Q2 reference-trace scripts (Windows PowerShell 5.1). Dot-source only.
# Nothing here writes outside tools\probe_mod\q2_traces\out\ except Install-Q2Mod (game Mods folder,
# recorded in the run's manifest first) and the game/reader processes the run scripts start.

$ErrorActionPreference = 'Stop'
$Q2Kit = Split-Path -Parent $MyInvocation.MyCommand.Path
$Q2Root = (Resolve-Path (Join-Path $Q2Kit '..\..\..')).Path
$Q2Out = Join-Path $Q2Kit 'out'
$Q2Bin = Join-Path $Q2Kit 'bin'
$Q2Python = Join-Path $Q2Root 'toolchain\venv\Scripts\python.exe'
$Q2FocExeSha256 = '07daeeaec9d1e751383b1db3f5847b6c66f68785440a6c52c87b85f723d018a4'  # docs/re/PE_OVERVIEW.md
$Q2DefaultGameRoot = 'F:\SteamLibrary\steamapps\common\Star Wars Empire at War'       # earlier probes
$Q2TrWorkshopId = '1125571106'   # Thrawn's Revenge: Imperial Civil War (Steam Workshop, app 32470)
$Q2ModName = 'Q2Trace'
$Q2LuaOutputName = 'Q2Trace_Lua.txt'

function Write-Q2 { param([string]$Msg) Write-Host "[q2] $Msg" }

function Write-Q2Json {
    param([string]$Path, $Object)
    $json = $Object | ConvertTo-Json -Depth 8
    [IO.File]::WriteAllText($Path, $json + "`n", (New-Object Text.UTF8Encoding($false)))
}

function Read-Q2Json {
    param([string]$Path)
    if (-not (Test-Path -LiteralPath $Path)) { return $null }
    try { return (Get-Content -LiteralPath $Path -Raw | ConvertFrom-Json) } catch { return $null }
}

function Get-Q2Sha256 {
    param([string]$Path)
    return (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant()
}

# Steam library folders that contain the game (read-only: registry value + libraryfolders.vdf).
function Find-Q2GameRoot {
    param([string]$Hint)
    $cands = New-Object System.Collections.Generic.List[string]
    if ($Hint) { $cands.Add($Hint) }
    $cands.Add($Q2DefaultGameRoot)
    try {
        $steam = (Get-ItemProperty -Path 'HKCU:\Software\Valve\Steam' -Name SteamPath -ErrorAction Stop).SteamPath
        $vdf = Join-Path $steam 'steamapps\libraryfolders.vdf'
        $libs = @($steam)
        if (Test-Path -LiteralPath $vdf) {
            foreach ($m in [regex]::Matches((Get-Content -LiteralPath $vdf -Raw), '"path"\s+"([^"]+)"')) {
                $libs += $m.Groups[1].Value.Replace('\\', '\')
            }
        }
        foreach ($l in $libs) { $cands.Add((Join-Path $l 'steamapps\common\Star Wars Empire at War')) }
    } catch { }
    foreach ($c in $cands) {
        if ($c -and (Test-Path -LiteralPath (Join-Path $c 'corruption\StarWarsG.exe'))) { return (Resolve-Path -LiteralPath $c).Path }
    }
    return $null
}

function Get-Q2WorkshopDir {
    param([string]$GameRoot, [string]$Id)
    # <library>\steamapps\common\<game> -> <library>\steamapps\workshop\content\32470\<id>
    $steamapps = Split-Path -Parent (Split-Path -Parent $GameRoot)
    return (Join-Path $steamapps "workshop\content\32470\$Id")
}

# Fiscal cycle length in frames: 30 x Fiscal_Cycle_Time_In_Secs (GALACTIC_ECONOMY.md §1.2), read from a
# loose GameConstants.xml of the mod if it ships one, else the vanilla 45 s.
function Get-Q2DayFrames {
    param([string]$ModDir)
    $secs = 45.0
    if ($ModDir) {
        $gc = Get-ChildItem -LiteralPath $ModDir -Recurse -Filter 'GameConstants.xml' -ErrorAction SilentlyContinue | Select-Object -First 1
        if ($gc) {
            $m = [regex]::Match((Get-Content -LiteralPath $gc.FullName -Raw), '<Fiscal_Cycle_Time_In_Secs>\s*([0-9.]+)')
            if ($m.Success) { $secs = [double]::Parse($m.Groups[1].Value, [Globalization.CultureInfo]::InvariantCulture) }
        }
    }
    return [int][Math]::Round(30.0 * $secs, [MidpointRounding]::AwayFromZero)
}

# Copies the Lua probe mod into <game>\corruption\Mods\Q2Trace. Every created file and folder is added
# to the manifest (with its SHA-256) and the manifest is saved BEFORE each copy, so a crash at any point
# leaves a manifest cleanup.ps1 can act on. Refuses to touch an existing Q2Trace folder.
function Install-Q2Mod {
    param([string]$GameRoot, [string]$ManifestPath, $Manifest, [switch]$DryRun)
    $src = Join-Path $Q2Kit "mod\$Q2ModName"
    $modsDir = Join-Path $GameRoot 'corruption\Mods'
    $dst = Join-Path $modsDir $Q2ModName
    if (Test-Path -LiteralPath $dst) { throw "refusing to install: $dst already exists (run cleanup.cmd first)" }
    $dirs = @()
    if (-not (Test-Path -LiteralPath $modsDir)) { $dirs += $modsDir }
    $dirs += $dst
    foreach ($d in (Get-ChildItem -LiteralPath $src -Recurse -Directory | Sort-Object FullName)) {
        $dirs += (Join-Path $dst $d.FullName.Substring($src.Length + 1))
    }
    foreach ($d in $dirs) {
        Write-Q2 "create dir  $d"
        if ($DryRun) { continue }
        $Manifest.dirs_created += $d
        Write-Q2Json $ManifestPath $Manifest
        New-Item -ItemType Directory -Path $d | Out-Null
    }
    foreach ($f in (Get-ChildItem -LiteralPath $src -Recurse -File | Sort-Object FullName)) {
        $to = Join-Path $dst $f.FullName.Substring($src.Length + 1)
        Write-Q2 "copy file   $to"
        if ($DryRun) { continue }
        $Manifest.files_created += [pscustomobject]@{ path = $to; sha256 = (Get-Q2Sha256 $f.FullName) }
        Write-Q2Json $ManifestPath $Manifest
        Copy-Item -LiteralPath $f.FullName -Destination $to
    }
}

# What sits next to the exe that is not part of the Steam install (prior patch chain, DXVK, wrappers). Read-only.
# 'summary' is one line for the trace header and run_status.json.
function Get-Q2GameFolderState {
    param([string]$GameRoot)
    $corr = Join-Path $GameRoot 'corruption'
    $files = @()
    foreach ($n in @('d3dx9_43.dll', 'd3d9.dll', 'dxgi.dll', 'd3d11.dll', 'dinput8.dll', 'version.dll')) {
        $p = Join-Path $corr $n
        if (Test-Path -LiteralPath $p) { $files += [pscustomobject]@{ name = $n; size = (Get-Item -LiteralPath $p).Length; sha256 = (Get-Q2Sha256 $p) } }
    }
    $eaw = @(Get-ChildItem -LiteralPath $corr -File -Filter 'eaw_*' -ErrorAction SilentlyContinue |
             Where-Object { $_.Extension -in '.dll', '.ini', '.json' } | ForEach-Object { $_.Name } | Sort-Object)
    $chain = $(if ($eaw.Count -or ($files | Where-Object { $_.name -eq 'd3dx9_43.dll' })) { 'present' } else { 'absent' })
    $parts = @("patch_chain=$chain")
    foreach ($f in $files) { $parts += "$($f.name):$($f.size):$($f.sha256.Substring(0, 12))" }
    if ($eaw.Count) { $parts += "eaw_files:$($eaw.Count)" }
    return [pscustomobject]@{ patch_chain = $chain; files = $files; eaw_files = $eaw; summary = ($parts -join ',') }
}

# Anything of the Q2 probe left in the game folder: the mod folder, or a Lua output file up to 3 levels down.
function Get-Q2ProbeLeftovers {
    param([string]$GameRoot)
    $left = @()
    $mod = Join-Path $GameRoot "corruption\Mods\$Q2ModName"
    if (Test-Path -LiteralPath $mod) { $left += $mod }
    $left += @(Get-ChildItem -LiteralPath $GameRoot -Recurse -Depth 2 -File -Filter $Q2LuaOutputName -ErrorAction SilentlyContinue |
               ForEach-Object { $_.FullName })
    return $left
}

function New-Q2Manifest {
    param([string]$RunId, [string]$GameRoot, [string]$RunDir)
    return [pscustomobject]@{
        tool = 'q2_traces'; run_id = $RunId; game_root = $GameRoot; run_dir = $RunDir
        started_utc = (Get-Date).ToUniversalTime().ToString('o')
        dirs_created = @(); files_created = @()
        lua_output_name = $Q2LuaOutputName
        reader_pid = 0; game_pid = 0; game_start_utc = ''
        cleaned = $false; cleanup_runs = 0
    }
}
