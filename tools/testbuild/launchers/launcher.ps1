# Alamo engine test build: finds the game and the installed mods, and shows the mod menu.
# Called by Play.cmd, Play_Mod.cmd and tr_paths.cmd; it never starts the game itself. It writes a small .cmd file
# (-Out) that sets GAME, ACTION, MODARGS, MODKEY and MODNAME, and the calling .cmd starts alamo_game with those.
#   -Mode game   find the Forces of Corruption folder only (asks for it once if it cannot be found)
#   -Mode pick   find the game, then pick a mod: from -Pick (a mod folder, a workshop id or a menu answer) or the menu
# Windows PowerShell 5.1; keep this file ASCII.
param(
    [ValidateSet('game', 'pick')][string]$Mode = 'pick',
    [Parameter(Mandatory = $true)][string]$Out,
    [string]$Pick = ''
)
$ErrorActionPreference = 'Stop'
$Here = Split-Path -Parent $MyInvocation.MyCommand.Path
$Runs = Join-Path $Here 'mod_runs'
$WorkshopApp = '32470'   # Forces of Corruption's Steam app id: workshop items live in steamapps\workshop\content\32470

# Mods without a modinfo.json that this test build knows by name (launcher only; the engine names nothing).
$KnownNames = @{
    '1770851727' = 'EAW-Remake'
    '2858787558' = 'Star Wars: Age of Legends (New Jedi Order)'
}

function Write-Choice([hashtable]$v) {
    $lines = @('@echo off')
    foreach ($k in 'GAME', 'ACTION', 'MODARGS', 'MODKEY', 'MODNAME') {
        $val = ''
        if ($v.ContainsKey($k)) { $val = [string]$v[$k] }
        $lines += ('set "{0}={1}"' -f $k, $val)
    }
    # cmd reads a batch file in the console code page; a UTF-8 BOM would break its first line.
    $enc = [Console]::OutputEncoding
    if ($enc.CodePage -eq 65001) { $enc = New-Object System.Text.UTF8Encoding($false) }
    [IO.File]::WriteAllText($Out, (($lines -join "`r`n") + "`r`n"), $enc)
}

function Safe-Text([string]$s) {
    # Text that goes into a cmd "set" line and an echo: drop the characters cmd treats specially.
    return ($s -replace '[&|<>^%!"]', '').Trim()
}

function Get-SteamRoots {
    $roots = New-Object System.Collections.Generic.List[string]
    if ($env:ALAMO_STEAM_ROOT) {
        foreach ($r in $env:ALAMO_STEAM_ROOT.Split(';')) { if ($r) { $roots.Add($r) } }
        return $roots
    }
    foreach ($key in 'HKCU:\Software\Valve\Steam', 'HKLM:\SOFTWARE\WOW6432Node\Valve\Steam', 'HKLM:\SOFTWARE\Valve\Steam') {
        try {
            $p = Get-ItemProperty -LiteralPath $key -ErrorAction Stop
            foreach ($name in 'SteamPath', 'InstallPath') {
                $val = $p.$name
                if ($val) { $roots.Add(($val -replace '/', '\')) }
            }
        } catch { }
    }
    $roots.Add('C:\Program Files (x86)\Steam')
    return $roots
}

function Get-SteamLibraries {
    # Steam library folders: each Steam client folder and every "path" in its libraryfolders.vdf.
    $libs = New-Object System.Collections.Generic.List[string]
    foreach ($root in (Get-SteamRoots)) {
        if (-not (Test-Path -LiteralPath $root)) { continue }
        $libs.Add($root)
        foreach ($vdf in (Join-Path $root 'steamapps\libraryfolders.vdf'), (Join-Path $root 'config\libraryfolders.vdf')) {
            if (-not (Test-Path -LiteralPath $vdf)) { continue }
            foreach ($m in [regex]::Matches((Get-Content -LiteralPath $vdf -Raw), '"(?:path|\d+)"\s+"([^"]+)"')) {
                $libs.Add(($m.Groups[1].Value -replace '\\\\', '\'))
            }
        }
    }
    $seen = @{}
    $result = @()
    foreach ($l in $libs) {
        try { $full = [IO.Path]::GetFullPath($l).TrimEnd('\') } catch { continue }
        if (-not $seen.ContainsKey($full.ToLowerInvariant())) { $seen[$full.ToLowerInvariant()] = 1; $result += $full }
    }
    return $result
}

function Test-Game([string]$dir) {
    return ($dir -and (Test-Path -LiteralPath (Join-Path $dir 'Data')) -and
            ((Test-Path -LiteralPath (Join-Path $dir 'Data\MegaFiles.xml')) -or (Test-Path -LiteralPath (Join-Path $dir 'StarWarsG.exe'))))
}

function Normalize-GameDir([string]$dir) {
    # Accepts the "corruption" folder itself, or the "Star Wars Empire at War" folder above it.
    $dir = $dir.Trim().Trim('"').TrimEnd('\')
    if (Test-Game $dir) { return $dir }
    $sub = Join-Path $dir 'corruption'
    if (Test-Game $sub) { return $sub }
    return ''
}

function Find-Game {
    $saved = Join-Path $Here 'game_path.txt'
    if (Test-Path -LiteralPath $saved) {
        $g = Normalize-GameDir ((Get-Content -LiteralPath $saved -TotalCount 1) + '')
        if ($g) { return $g }
        Write-Host ('game_path.txt does not point at Forces of Corruption; looking again.')
    }
    if ($env:ALAMO_FOC_DATA) {
        $g = Normalize-GameDir $env:ALAMO_FOC_DATA
        if ($g) { return $g }
    }
    foreach ($lib in (Get-SteamLibraries)) {
        $g = Normalize-GameDir (Join-Path $lib 'steamapps\common\Star Wars Empire at War')
        if ($g) { return $g }
    }
    foreach ($g in 'F:\SteamLibrary\steamapps\common\Star Wars Empire at War\corruption',
                   'C:\GOG Games\Star Wars - Empire at War\corruption',
                   'C:\Program Files (x86)\GOG Galaxy\Games\Star Wars - Empire at War\corruption') {
        if (Test-Game $g) { return $g }
    }
    Write-Host ''
    Write-Host 'Forces of Corruption was not found.'
    Write-Host 'Paste the path of its "corruption" folder (or of the "Star Wars Empire at War" folder) and press Enter.'
    for ($i = 0; $i -lt 3; $i++) {
        $answer = Read-Host 'Game folder'
        if (-not $answer) { break }
        $g = Normalize-GameDir $answer
        if ($g) {
            Set-Content -LiteralPath $saved -Value $g -Encoding UTF8
            Write-Host ('Saved in game_path.txt: ' + $g)
            return $g
        }
        Write-Host 'That folder has no Data folder of Forces of Corruption. Try again (Enter alone to stop).'
    }
    return ''
}

function Read-ModInfo([string]$dir) {
    $f = Join-Path $dir 'modinfo.json'
    if (-not (Test-Path -LiteralPath $f)) { return $null }
    try { return ((Get-Content -LiteralPath $f -Raw -Encoding UTF8) | ConvertFrom-Json) } catch { return $null }
}

function New-Mod([string]$kind, [string]$id, [string]$path) {
    $info = Read-ModInfo $path
    $name = ''
    $version = ''
    $text = ''
    if ($info) {
        if ($info.name) { $name = [string]$info.name }
        if ($info.version) { $version = [string]$info.version }
        # Where a submod names the mods it needs (not steamdata: a copy of a mod carries the original's id there).
        foreach ($field in 'name', 'summary', 'description', 'dependencies') {
            if ($info.$field) { $text += ' ' + (($info.$field | ConvertTo-Json -Depth 6 -Compress) + '') }
        }
    }
    if (-not $name -and $KnownNames.ContainsKey($id)) { $name = $KnownNames[$id] }
    if (-not $name) { $name = $id }
    if ($version -and $name -notlike ('*' + $version + '*')) { $name = $name + ' ' + $version }
    if ($kind -eq 'workshop') {
        $where = 'workshop ' + $id
        $arg = '"STEAMMOD=' + $id + '"'
        $key = 'ws' + $id
    } else {
        $where = 'Mods\' + $id
        $arg = '"MODPATH=' + $path + '"'
        $key = 'local_' + ($id -replace '[^A-Za-z0-9_.-]', '_')
    }
    if ($kind -eq 'folder') {
        $where = $path
        $key = 'folder_' + ((Split-Path -Leaf $path) -replace '[^A-Za-z0-9_.-]', '_')
    }
    return [pscustomobject]@{ Kind = $kind; Id = $id; Path = $path; Name = (Safe-Text $name); Where = $where
                              Arg = $arg; Key = $key; InfoText = $text; Parents = @() }
}

function Find-Mods([string]$game) {
    $mods = @()
    $seen = @{}
    # The library holding the game first, then every other Steam library (first one with the item wins).
    $libs = @()
    $p = $game
    while ($p) {
        if ((Split-Path -Leaf $p) -ieq 'steamapps') { $libs += (Split-Path -Parent $p); break }
        $parent = Split-Path -Parent $p
        if ($parent -eq $p) { break }
        $p = $parent
    }
    $libs += (Get-SteamLibraries)
    $items = @()
    foreach ($lib in $libs) {
        $content = Join-Path $lib ('steamapps\workshop\content\' + $WorkshopApp)
        if (-not (Test-Path -LiteralPath $content)) { continue }
        foreach ($d in (Get-ChildItem -LiteralPath $content -Directory -ErrorAction SilentlyContinue)) {
            if ($d.Name -notmatch '^\d+$' -or $seen.ContainsKey($d.Name)) { continue }
            if (-not (Test-Path -LiteralPath (Join-Path $d.FullName 'Data'))) { continue }
            $seen[$d.Name] = 1
            $items += $d
        }
    }
    foreach ($d in ($items | Sort-Object { [long]$_.Name })) { $mods += (New-Mod 'workshop' $d.Name $d.FullName) }
    $local = Join-Path $game 'Mods'
    if (Test-Path -LiteralPath $local) {
        foreach ($d in (Get-ChildItem -LiteralPath $local -Directory -ErrorAction SilentlyContinue | Sort-Object Name)) {
            if (Test-Path -LiteralPath (Join-Path $d.FullName 'Data')) { $mods += (New-Mod 'local' $d.Name $d.FullName) }
        }
    }
    # A local mod whose modinfo.json names installed workshop items (e.g. "Requires Thrawn's Revenge 1125571106") is a
    # submod: it is played on top of them, in the order it names them.
    foreach ($m in $mods) {
        if ($m.Kind -eq 'workshop' -or -not $m.InfoText) { continue }
        $parents = @()
        foreach ($match in [regex]::Matches($m.InfoText, '\d{6,}')) {
            $id = $match.Value
            if ($id -eq $m.Id) { continue }
            foreach ($w in $mods) {
                if ($w.Kind -eq 'workshop' -and $w.Id -eq $id -and ($parents -notcontains $w)) { $parents += $w }
            }
        }
        $m.Parents = $parents
    }
    return $mods
}

function Get-Label($m, $all) {
    $label = $m.Name
    if ($label.Length -gt 46) { $label = $label.Substring(0, 43) + '...' }
    if ($m.Parents.Count -gt 0) {
        $nums = @()
        foreach ($p in $m.Parents) { $nums += ([array]::IndexOf($all, $p) + 1) }
        $label += '  (on top of ' + ($nums -join ' + ') + ')'
    }
    return $label
}

function Resolve-Selection([object[]]$chosen) {
    # $chosen: mods in load order (first wins). A submod's parents follow it unless they are already chosen.
    $order = @()
    foreach ($m in $chosen) {
        if ($order -notcontains $m) { $order += $m }
        if ($chosen.Count -eq 1) { foreach ($p in $m.Parents) { if ($order -notcontains $p) { $order += $p } } }
    }
    $sel = @{ MODARGS = (($order | ForEach-Object { $_.Arg }) -join ' ')
              MODKEY = (($order | ForEach-Object { $_.Key }) -join '+')
              MODNAME = (($order | ForEach-Object { $_.Name }) -join ' on top of ') }
    return $sel
}

function Parse-Answer([string]$answer, $mods) {
    # Returns @{ ACTION; MODARGS; MODKEY; MODNAME } or $null if the answer is not understood.
    $a = $answer.Trim().Trim('"')
    if ($a -eq '') { return $null }
    if ($a -match '^(?i)q$') { return @{ ACTION = 'quit' } }
    $action = 'play'
    if ($a -match '^(?i)c\s*(.+)$') { $action = 'check'; $a = $Matches[1].Trim() }
    if ($a -eq '0') {
        return @{ ACTION = $action; MODARGS = ''; MODKEY = 'base'; MODNAME = 'Forces of Corruption (no mod)' }
    }
    if ($a -match '^(?i)f$') {
        $a = (Read-Host 'Mod folder (the one that holds the Data folder)').Trim().Trim('"')
        if (-not $a) { return $null }
    }
    # A folder anywhere on disk (dragged onto Play_Mod.cmd, pasted, or typed).
    if ($a -match '[\\/:]' -and (Test-Path -LiteralPath $a -PathType Container)) {
        $dir = (Resolve-Path -LiteralPath $a).ProviderPath.TrimEnd('\')
        if ((Split-Path -Leaf $dir) -ieq 'Data') { $dir = Split-Path -Parent $dir }
        if (-not (Test-Path -LiteralPath (Join-Path $dir 'Data'))) {
            Write-Host ('"' + $dir + '" has no Data folder, so it is not a mod folder.')
            return $null
        }
        $known = $mods | Where-Object { $_.Path -ieq $dir } | Select-Object -First 1
        if (-not $known) { $known = New-Mod 'folder' (Split-Path -Leaf $dir) $dir; $known.Name = Safe-Text (Split-Path -Leaf $dir) }
        $sel = Resolve-Selection @($known)
        $sel.ACTION = $action
        return $sel
    }
    # "5", "5+1" (mod 5 on top of mod 1), or a workshop id.
    $chosen = @()
    foreach ($part in ($a -split '\+')) {
        $part = $part.Trim()
        if ($part -notmatch '^\d+$') { return $null }
        $n = [long]$part
        if ($n -ge 1 -and $n -le $mods.Count) { $chosen += $mods[$n - 1]; continue }
        $byId = $mods | Where-Object { $_.Kind -eq 'workshop' -and $_.Id -eq $part } | Select-Object -First 1
        if ($byId) { $chosen += $byId; continue }
        if ($part.Length -ge 6) {   # a workshop item this script did not find: let the engine look it up
            $chosen += [pscustomobject]@{ Kind = 'workshop'; Id = $part; Path = ''; Name = ('workshop ' + $part)
                                          Where = ''; Arg = ('"STEAMMOD=' + $part + '"'); Key = ('ws' + $part); Parents = @() }
            continue
        }
        return $null
    }
    if ($chosen.Count -eq 0) { return $null }
    $sel = Resolve-Selection $chosen
    $sel.ACTION = $action
    return $sel
}

# ---------------------------------------------------------------------------------------------------------------------
$game = Find-Game
if (-not $game) {
    Write-Host 'No game folder; nothing started. Put the path in game_path.txt next to Play.cmd and try again.'
    exit 1
}
if ($Mode -eq 'game') {
    Write-Choice @{ GAME = $game; ACTION = 'play' }
    exit 0
}

$mods = @(Find-Mods $game)
if (-not (Test-Path -LiteralPath $Runs)) { New-Item -ItemType Directory -Path $Runs | Out-Null }
$lastFile = Join-Path $Runs 'last_pick.txt'   # MODARGS, MODKEY, MODNAME of the last mod played

function Save-Last($sel) {
    $lines = @($sel.MODARGS, $sel.MODKEY, $sel.MODNAME)
    Set-Content -LiteralPath $lastFile -Value $lines -Encoding UTF8
}

function Read-Last {
    if (-not (Test-Path -LiteralPath $lastFile)) { return $null }
    $lines = @(Get-Content -LiteralPath $lastFile -Encoding UTF8)
    if ($lines.Count -lt 3 -or -not $lines[1]) { return $null }
    return @{ ACTION = 'play'; MODARGS = $lines[0]; MODKEY = $lines[1]; MODNAME = $lines[2] }
}

if ($Pick) {
    $sel = Parse-Answer $Pick $mods
    if (-not $sel) { Write-Host ('Not a mod folder, mod number or workshop id: ' + $Pick); exit 1 }
} else {
    $last = Read-Last
    while ($true) {
        Clear-Host
        Write-Host 'Alamo engine test build: pick a mod'
        Write-Host ('Game: ' + $game)
        Write-Host ''
        Write-Host ('   0  Forces of Corruption (no mod)')
        $width = 20
        foreach ($m in $mods) { $l = (Get-Label $m $mods).Length; if ($l -gt $width) { $width = $l } }
        if ($width -gt 62) { $width = 62 }
        for ($i = 0; $i -lt $mods.Count; $i++) {
            $label = Get-Label $mods[$i] $mods
            Write-Host ('{0,4}  {1}  {2}' -f ($i + 1), $label.PadRight($width), $mods[$i].Where)
        }
        if ($mods.Count -eq 0) {
            Write-Host ''
            Write-Host '      No mods found: none in the Steam workshop folder (subscribe to one on Steam) and none'
            Write-Host ('      in ' + (Join-Path $game 'Mods') + '.')
        }
        Write-Host ''
        Write-Host 'Type a number and press Enter to play it.'
        if ($last) { Write-Host ('  Enter alone   play ' + $last.MODNAME + ' again (your last pick)') }
        Write-Host '  C<number>     check a mod without playing: loads it and writes a report (e.g. C1)'
        Write-Host '  <a>+<b>       play mod a on top of mod b, e.g. a submod on the mod it needs (e.g. 7+1)'
        Write-Host '  F             a mod folder somewhere else (you can also drag a mod folder onto Play_Mod.cmd)'
        Write-Host '  Q             quit'
        $answer = Read-Host 'Your pick'
        if (-not $answer) {
            if ($last) { $sel = $last; break }
            continue
        }
        $sel = Parse-Answer $answer $mods
        if ($sel) { break }
        Write-Host ('Not understood: ' + $answer)
        Start-Sleep -Seconds 2
    }
}
if ($sel.ACTION -eq 'play') { Save-Last $sel }
$sel.GAME = $game
Write-Choice $sel
exit 0
