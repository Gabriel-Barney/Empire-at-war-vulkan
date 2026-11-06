# Runs the three gate stages (build_msvc.cmd, build_clang.cmd, build_asan.cmd) at the same time instead of one after another.
# Called by build_gate.cmd (set ALAMO_GATE_SERIAL=1 there for the old one-at-a-time order). Why: a stage's test phase is mostly
# idle cores (the longest test runs on one core for about a minute), and the other stages' compiles fill them.
#
# Each stage keeps its own build tree, logs\<stage>_*.txt and per-tree temp folder, so nothing is shared except the compiler
# cache. The cores and memory are split so three compiles do not swap (32 GB: about 26 compiler processes at once):
#   build jobs (CMAKE_BUILD_PARALLEL_LEVEL): msvc 60 %, clang 60 %, asan 40 % of the logical cores
#   test jobs (ALAMO_CTEST_JOBS):            msvc 40 %, clang 40 %, asan 20 %
# Each stage's console output goes to logs\gate_<stage>_stdout.txt and is printed here in order when all three are done.
# Roadmap build records are deferred (ALAMO_DEFER_RECORD) and written one after another at the end: two records at once
# would fight over the store lock.
# Usage: gate_parallel.ps1 <project root> [asan module list]
param(
    [Parameter(Mandatory = $true)][string]$Root,
    [string]$AsanModules = ''
)
$ErrorActionPreference = 'Stop'
$Root = (Resolve-Path $Root).Path
$n = [Environment]::ProcessorCount
function Share([double]$f, [int]$min) { [Math]::Max($min, [int][Math]::Round($n * $f)) }

$stages = @(
    @{ Name = 'msvc';  Script = 'build_msvc.cmd';  Args = '';            Build = (Share 0.6 4); Test = (Share 0.4 2) },
    @{ Name = 'clang'; Script = 'build_clang.cmd'; Args = '';            Build = (Share 0.6 4); Test = (Share 0.4 2) },
    @{ Name = 'asan';  Script = 'build_asan.cmd';  Args = $AsanModules;  Build = (Share 0.4 3); Test = (Share 0.2 2) }
)

# Tests of these modules write PNGs to <project>\logs\ under fixed names (about 25 test files), so MSVC and clang running them at the
# same time overwrite each other (alamo_render_galactic_tests crashed 0xc0000409 once, passes alone). The clang stage skips them in
# parallel mode; they are not sim code, so clang tests are not required for them (CLAUDE.md). ALAMO_GATE_SERIAL=1 runs everything.
$clangSkips = 'render|ui|audio|video'

foreach ($s in $stages) {
    $s.Out = Join-Path $Root "logs\gate_$($s.Name)_stdout.txt"
    $line = "set CMAKE_BUILD_PARALLEL_LEVEL=$($s.Build)&& set ALAMO_CTEST_JOBS=$($s.Test)&& set ALAMO_DEFER_RECORD=1&& " +
            "call `"$Root\$($s.Script)`" $(if ($s.Args) { '"' + $s.Args + '"' }) > `"$($s.Out)`" 2>&1"
    # cmd /s strips the outer quotes, so the inner quotes around paths with spaces survive.
    if ($s.Name -eq 'clang') { $env:ALAMO_EXCLUDE_LABELS = $clangSkips } else { Remove-Item Env:\ALAMO_EXCLUDE_LABELS -ErrorAction SilentlyContinue }
    $s.Proc = Start-Process -FilePath "$env:ComSpec" -ArgumentList "/s /c `"$line`"" -WorkingDirectory $Root -PassThru -WindowStyle Hidden
    Remove-Item Env:\ALAMO_EXCLUDE_LABELS -ErrorAction SilentlyContinue
    Write-Host "started $($s.Name): build -j$($s.Build), tests -j$($s.Test)"
}
Wait-Process -Id ($stages | ForEach-Object { $_.Proc.Id })

foreach ($s in $stages) {
    Write-Host "===== $($s.Name) (exit $($s.Proc.ExitCode)) ====="
    if (Test-Path $s.Out) { Get-Content $s.Out }
}

# Deferred roadmap records, same arguments the stage scripts use.
Push-Location $Root
try {
    $py = 'python'
    & $py roadmap-store\roadmap_store.py record-build msvc build-msvc logs\msvc_configure.txt logs\msvc_build.txt
    & $py roadmap-store\roadmap_store.py record-build clang build-clang logs\clang_configure.txt logs\clang_build.txt
    & $py roadmap-store\roadmap_store.py record-build clang-asan build-asan logs\asan_configure.txt logs\asan_build.txt
} finally {
    Pop-Location
}
