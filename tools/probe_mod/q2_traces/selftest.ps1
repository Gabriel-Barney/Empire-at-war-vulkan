<#
Q2 kit self-test, no game needed. Never touches the real game folder.
  1. q2trace.py hash/CRC vectors (vs engine/core and vs q2reader --selftest)
  2. q2reader against fake_game.exe (skirmish + galactic), validate, to-q1, compare
  3. install + cleanup on scratch fake game folders under out\selftest\: dry run changes nothing, cleanup
     restores the exact tree, a second cleanup is a no-op, a modified probe file is kept unless -Force,
     a Mods folder created by the install is removed again
  4. run_trace.ps1 preflight refuses a folder whose StarWarsG.exe is not FoC 1.121 (exit 20, nothing changed)
Exit 0 = all passed.
#>
. (Join-Path (Split-Path -Parent $MyInvocation.MyCommand.Path) 'q2common.ps1')
$ErrorActionPreference = 'Stop'
$fails = 0
function Check([bool]$Ok, [string]$What) { if ($Ok) { Write-Q2 "PASS $What" } else { Write-Q2 "FAIL $What"; $script:fails++ } }
function Tree([string]$Root) {
    if (-not (Test-Path -LiteralPath $Root)) { return '' }
    $items = Get-ChildItem -LiteralPath $Root -Recurse -Force | Sort-Object FullName | ForEach-Object {
        $rel = $_.FullName.Substring($Root.Length)
        if ($_.PSIsContainer) { "D $rel" } else { "F $rel $(Get-Q2Sha256 $_.FullName)" }
    }
    return ($items -join "`n")
}
$cleanup = Join-Path $Q2Kit 'cleanup.ps1'
function Run-Cleanup([string[]]$CleanArgs) {
    & powershell -NoProfile -ExecutionPolicy Bypass -File $cleanup @CleanArgs | Out-Null
    return $LASTEXITCODE
}

$st = Join-Path $Q2Out 'selftest'
if (Test-Path -LiteralPath $st) { Remove-Item -LiteralPath $st -Recurse -Force }
New-Item -ItemType Directory -Path $st | Out-Null

# 1-2. hashes and the reader end to end
if (-not (Test-Path -LiteralPath (Join-Path $Q2Bin 'fake_game.exe'))) { & cmd /c "`"$(Join-Path $Q2Kit 'build_reader.cmd')`"" | Out-Null }
& $Q2Python -I (Join-Path $Q2Kit 'q2trace.py') selftest-fake --bin $Q2Bin --out (Join-Path $st 'fake')
Check ($LASTEXITCODE -eq 0) 'reader + q2trace.py against fake_game (selftest-fake)'

# 3. install / cleanup on a fake game folder that already has a Mods folder
$fake = Join-Path $st 'fakegame'
New-Item -ItemType Directory -Path (Join-Path $fake 'corruption\Mods\OtherMod\Data') -Force | Out-Null
New-Item -ItemType Directory -Path (Join-Path $fake 'GameData') -Force | Out-Null
[IO.File]::WriteAllText((Join-Path $fake 'corruption\StarWarsG.exe'), 'not the real exe')
[IO.File]::WriteAllText((Join-Path $fake 'corruption\Mods\OtherMod\Data\keep.txt'), 'someone else''s mod')
$before = Tree $fake
$run1 = Join-Path $st 'run1'
New-Item -ItemType Directory -Path $run1 | Out-Null
$mp = Join-Path $run1 'manifest.json'
$m = New-Q2Manifest 'selftest-run1' $fake $run1
Write-Q2Json $mp $m
Install-Q2Mod -GameRoot $fake -ManifestPath $mp -Manifest $m -DryRun
Check ((Tree $fake) -eq $before) 'install dry run changes nothing'
Install-Q2Mod -GameRoot $fake -ManifestPath $mp -Manifest $m
Check (Test-Path -LiteralPath (Join-Path $fake 'corruption\Mods\Q2Trace\Data\Scripts\Story\Q2_Trace_Galactic.lua')) 'install copies the probe'
$threw = $false
try { Install-Q2Mod -GameRoot $fake -ManifestPath $mp -Manifest $m } catch { $threw = $true }
Check $threw 'second install into an existing Q2Trace folder is refused'
Start-Sleep -Milliseconds 1100
[IO.File]::WriteAllText((Join-Path $fake 'corruption\Q2Trace_Lua.txt'), "Q2LUA 1 start`n")
$installed = Tree $fake
Check ((Run-Cleanup @('-RunDir', $run1, '-DryRun')) -eq 0) 'cleanup dry run exit 0'
Check ((Tree $fake) -eq $installed) 'cleanup dry run changes nothing'
Check ((Run-Cleanup @('-RunDir', $run1)) -eq 0) 'cleanup exit 0'
Check ((Tree $fake) -eq $before) 'cleanup restores the exact tree'
Check (Test-Path -LiteralPath (Join-Path $run1 'lua_corruption_Q2Trace_Lua.txt')) 'Lua output moved into the run folder'
Check ((Run-Cleanup @('-RunDir', $run1)) -eq 0) 'second cleanup exit 0'
Check ((Tree $fake) -eq $before) 'second cleanup changes nothing'
Check ((Read-Q2Json $mp).cleaned -eq $true) 'manifest marked cleaned'

# modified probe file: kept without -Force, removed with it
$run2 = Join-Path $st 'run2'
New-Item -ItemType Directory -Path $run2 | Out-Null
$mp2 = Join-Path $run2 'manifest.json'
$m2 = New-Q2Manifest 'selftest-run2' $fake $run2
Write-Q2Json $mp2 $m2
Install-Q2Mod -GameRoot $fake -ManifestPath $mp2 -Manifest $m2
Add-Content -LiteralPath (Join-Path $fake 'corruption\Mods\Q2Trace\Data\XML\Story_Plots_Sandbox_12U_Empire.xml') '<!-- edited -->'
Check ((Run-Cleanup @('-RunDir', $run2)) -eq 2) 'modified file: cleanup exit 2'
Check (Test-Path -LiteralPath (Join-Path $fake 'corruption\Mods\Q2Trace\Data\XML\Story_Plots_Sandbox_12U_Empire.xml')) 'modified file kept'
Check ((Run-Cleanup @('-RunDir', $run2, '-Force')) -eq 0) 'cleanup -Force exit 0'
Check ((Tree $fake) -eq $before) 'cleanup -Force restores the exact tree'

# fake game folder without a Mods folder: the install creates it, cleanup removes it
$fake2 = Join-Path $st 'fakegame2'
New-Item -ItemType Directory -Path (Join-Path $fake2 'corruption') -Force | Out-Null
[IO.File]::WriteAllText((Join-Path $fake2 'corruption\StarWarsG.exe'), 'not the real exe')
$before2 = Tree $fake2
$run3 = Join-Path $st 'run3'
New-Item -ItemType Directory -Path $run3 | Out-Null
$mp3 = Join-Path $run3 'manifest.json'
$m3 = New-Q2Manifest 'selftest-run3' $fake2 $run3
Write-Q2Json $mp3 $m3
Install-Q2Mod -GameRoot $fake2 -ManifestPath $mp3 -Manifest $m3
Check ((Run-Cleanup @('-RunDir', $run3)) -eq 0) 'no-Mods case: cleanup exit 0'
Check ((Tree $fake2) -eq $before2) 'no-Mods case: Mods folder removed again'

# 4. preflight refuses an unknown exe and changes nothing
$before = Tree $fake
& powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $Q2Kit 'run_trace.ps1') -Scenario galactic -LuaProbe -GameRoot $fake -NonInteractive -DryRun | Out-Null
Check ($LASTEXITCODE -eq 20) 'run_trace preflight rejects a non-1.121 exe (exit 20)'
Check ((Tree $fake) -eq $before) 'rejected run changed nothing'

if ($fails) { Write-Q2 "SELFTEST FAILED ($fails)"; exit 1 }
Write-Q2 'SELFTEST OK'
exit 0
