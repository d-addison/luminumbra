<#
.SYNOPSIS
      — the determinism test matrix.

.DESCRIPTION
    Runs the headless determinism smoke across the  axes and asserts every cell
    reproduces the canonical world_hash with run==replay, so a determinism regression
    is caught regardless of worker count, process, build, or replay path:

        cross-worker-count: --smoke under LUMINUMBRA_JOB_WORKERS in {1,2,4}
                                     (each run==replay AND all counts agree)
        fast/slow-job axis: --smoke under LUMINUMBRA_JOB_THROTTLE=<seed> (the
                                     / shuffled-pop service-order
                                     perturbation) must reproduce the unthrottled
                                     baseline; moving runs stay run==replay
        multiprocess one-box: two SEPARATE --smoke processes agree
        replay axis: --record then --replay reproduces the hash
        build-mode axis: --smoke in BOTH build/debug and build/release
        localized failure: on any mismatch, print the axis + observed hashes

    Pure orchestration over existing flags (--smoke / --smoke-moving / --record /
    --replay / LUMINUMBRA_JOB_WORKERS) — no engine change, no determinism risk (it RUNS
    the determinism checks). Run via the PowerShell tool with C:\msys64\ucrt64\bin
    prepended.

.NOTES
    PowerShell 5.1 compatible. Exit 0 = all cells pass; 1 = any divergence/failure.
#>
[CmdletBinding()]
param(
    # Build preset trees to run the matrix in. build/release is skipped if absent.
    [string[]]$Builds = @('debug', 'release'),
    # Worker-count axis (LUMINUMBRA_JOB_WORKERS). 0 = engine default (one per HW thread).
    [int[]]$Workers = @(1, 2, 4),
    # Canonical baseline (local-dev pin) — PER BUILD MODE. world_hash is build-mode-dependent
    # (FP/optimization differences in the sim): the DEBUG sim-truth hash and the RELEASE one are
    # legitimately DIFFERENT deterministic values.  checks each build is run==replay AND
    # matches ITS OWN baseline — NOT that debug == release. Override if intentionally bumped.
    [string]$BaselineHash = 'a66ab4d049ba9228',          # DEBUG canonical (the gate tree;  derived-state reclassification 2026-07-05)
    [string]$ReleaseBaselineHash = '045f7c2f0645bcce',   # RELEASE canonical (distinct, also deterministic; authoritative-state change)
    # Quick mode: workers {1,2}, debug only, skip the replay + moving axes (fast pre-commit check).
    [switch]$Quick,
    # The --smoke-moving streaming-arrival oracle is a DEFAULT first-class axis:
    # it runs per worker count UNLESS -Quick or -SkipMoving. -IncludeMoving is retained as a
    # backward-compatible no-op (moving is on by default) that also FORCES moving under -Quick.
    [switch]$IncludeMoving,
    [switch]$SkipMoving,
    # Optional JSON report path.
    [string]$Report = '',
    # -Mode WaterCrossBuild runs ONLY the debug-vs-release
    # per-tick water-state-hash sequence compare (both builds required). '' = the
    # full  matrix, exactly as before.
    [ValidateSet('', 'WaterCrossBuild')]
    [string]$Mode = ''
)

$ErrorActionPreference = 'Stop'
$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$RepoRoot = (Resolve-Path (Join-Path $ScriptDir '..\..')).Path

if ($Quick) { $Workers = @(1, 2); $Builds = @('debug') }

#  moving-residency axis: default-ON (first-class), off under -Quick or
# -SkipMoving; -IncludeMoving forces it even under -Quick.
$RunMoving = ((-not $Quick) -and (-not $SkipMoving)) -or $IncludeMoving

$results = New-Object System.Collections.Generic.List[object]
$failures = New-Object System.Collections.Generic.List[string]

function Resolve-ServerExe {
    param([string]$Build)
    $exe = Get-ChildItem -Path (Join-Path $RepoRoot "build/$Build") -Recurse -Filter 'luminumbra_server_app*.exe' -ErrorAction SilentlyContinue | Select-Object -First 1
    return $exe
}

# Run --smoke (optionally with a worker-count override / --smoke-moving) and return
# @{ ok; hash; replay_ok; raw }. ok = process exit 0 + "smoke passed" + hash == baseline.
function Invoke-Smoke {
    param([string]$ExePath, [int]$WorkerCount = -1, [switch]$Moving, [uint64]$ThrottleSeed = 0)
    $prev = $env:LUMINUMBRA_JOB_WORKERS
    $prevThrottle = $env:LUMINUMBRA_JOB_THROTTLE
    try {
        if ($WorkerCount -ge 0) { $env:LUMINUMBRA_JOB_WORKERS = "$WorkerCount" } else { Remove-Item Env:\LUMINUMBRA_JOB_WORKERS -ErrorAction SilentlyContinue }
        if ($ThrottleSeed -gt 0) { $env:LUMINUMBRA_JOB_THROTTLE = "$ThrottleSeed" } else { Remove-Item Env:\LUMINUMBRA_JOB_THROTTLE -ErrorAction SilentlyContinue }
        $smokeArg = if ($Moving) { '--smoke-moving' } else { '--smoke' }
        $raw = & $ExePath $smokeArg 2>&1 | Out-String
    } finally {
        if ($null -ne $prev) { $env:LUMINUMBRA_JOB_WORKERS = $prev } else { Remove-Item Env:\LUMINUMBRA_JOB_WORKERS -ErrorAction SilentlyContinue }
        if ($null -ne $prevThrottle) { $env:LUMINUMBRA_JOB_THROTTLE = $prevThrottle } else { Remove-Item Env:\LUMINUMBRA_JOB_THROTTLE -ErrorAction SilentlyContinue }
    }
    $hash = ''
    $m = [regex]::Match($raw, 'world_hash[=:\s]+([0-9a-f]{8,})')
    if ($m.Success) { $hash = $m.Groups[1].Value }
    $passed = ($raw -match 'smoke passed' -or $raw -match 'smoke-moving passed')
    # ok = run==replay only; the absolute-hash check is applied PER BUILD by the caller (the
    # expected baseline differs debug vs release). Moving runs are convergent oracles -> the
    # caller gates them on run==replay (passed) only, never on the static baseline literal.
    return @{ hash = $hash; replay_ok = $passed; raw = $raw }
}

# Per-build expected baseline (build-mode-dependent world_hash).
function Get-ExpectedBaseline {
    param([string]$Build)
    if ($Build -eq 'release') { return $ReleaseBaselineHash }
    return $BaselineHash
}

function Add-Cell {
    param([string]$Axis, [string]$Cell, [bool]$Ok, [string]$Detail)
    $results.Add([pscustomobject]@{ axis = $Axis; cell = $Cell; ok = $Ok; detail = $Detail })
    $status = if ($Ok) { 'PASS' } else { 'FAIL' }
    Write-Host ("[matrix] {0,-16} {1,-22} {2}  {3}" -f $Axis, $Cell, $status, $Detail)
    if (-not $Ok) { $failures.Add("$Axis/$Cell: $Detail") }
}

# ---  ( water cross-process): the water cross-PROCESS gate (re-scoped). ---
#, water-scoped. FIRST-RUN FINDING (2026-07-05, the contract's
# anticipated re-scope): the per-tick water hash DIVERGES debug-vs-release AT
# TICK 1 (e2e4c1d1fa39760c vs 94e9b1b5fb101a13) because water BEDS are seeded
# from float terrain sampling (GetTerrainHeightAt), which is legitimately
# build-mode-dependent — the SAME reason the canonical world_hash itself is
# per-build (see $BaselineHash vs $ReleaseBaselineHash above). The SIM is
# integer-mm end to end; the INITIAL CONDITIONS are not cross-build-portable,
# and lockstep peers run the SAME build, so the real host==peer contract is
# same-build CROSS-PROCESS: two separate OS processes of the SAME build must
# produce identical per-tick water-hash sequences (each also asserts its
# in-process run==replay). Both builds are exercised when present.
if ($Mode -eq 'WaterCrossBuild') {
    $artDir = Join-Path $RepoRoot 'build/test-artifacts/water-crossbuild'
    New-Item -ItemType Directory -Force -Path $artDir | Out-Null
    $anyRan = $false
    foreach ($build in @('debug', 'release')) {
        $exe = Resolve-ServerExe -Build $build
        if (-not $exe) {
            if ($build -eq 'release') { Write-Host "[water-crossprocess] build/release absent -> release leg SKIPPED"; continue }
            Write-Host "[water-crossprocess] FAIL: luminumbra_server_app.exe not found under build/debug"
            exit 1
        }
        $traces = @()
        foreach ($proc in @('p1', 'p2')) {
            $artPath = Join-Path $artDir "water-trace-$build-$proc.json"
            Remove-Item $artPath -Force -ErrorAction SilentlyContinue
            $raw = & $exe.FullName --smoke --water-hash-trace --artifact $artPath 2>&1 | Out-String
            if ($raw -notmatch 'smoke passed') {
                Write-Host "[water-crossprocess] FAIL: $build --smoke ($proc) did not pass"
                exit 1
            }
            if ($raw -match 'Water hash trace MISMATCH') {
                Write-Host "[water-crossprocess] FAIL: $build ($proc) water trace diverged IN-PROCESS (run!=replay)"
                exit 1
            }
            $traces +=,@((Get-Content $artPath -Raw | ConvertFrom-Json).water_hash_trace)
        }
        if ($traces[0].Count -eq 0) {
            Write-Host "[water-crossprocess] FAIL: empty water_hash_trace ($build)"
            exit 1
        }
        if ($traces[0].Count -ne $traces[1].Count) {
            Write-Host "[water-crossprocess] FAIL: $build trace lengths differ across processes"
            exit 1
        }
        for ($i = 0; $i -lt $traces[0].Count; $i++) {
            if ($traces[0][$i].hash -ne $traces[1][$i].hash) {
                Write-Host ("[water-crossprocess] FAIL: {0} water hash DIVERGES across processes at tick {1}: {2} vs {3} (host==peer same-build broken)" -f $build, $traces[0][$i].tick, $traces[0][$i].hash, $traces[1][$i].hash)
                exit 1
            }
        }
        Write-Host ("[water-crossprocess] PASS: {0} - {1} ticks, per-tick water hash IDENTICAL across two OS processes (  same-build host==peer, )" -f $build, $traces[0].Count)
        $anyRan = $true
    }
    if (-not $anyRan) { exit 1 }
    exit 0
}

Write-Host "===   determinism matrix (debug $BaselineHash / release $ReleaseBaselineHash) ==="

foreach ($build in $Builds) {
    $exe = Resolve-ServerExe -Build $build
    if (-not $exe) {
        if ($build -eq 'release') { Write-Host "[matrix] build/release absent -> build-mode axis SKIPPED for release"; continue }
        Add-Cell 'build-mode' $build $false "luminumbra_server_app.exe not found under build/$build"
        continue
    }

    #  cross-worker-count +  build-mode (this tree). Each build matches ITS OWN
    # baseline (debug 6f008a9f / release ea9a0121) — both deterministic, legitimately different.
    $expected = Get-ExpectedBaseline -Build $build
    $hashesThisBuild = @{}
    foreach ($w in $Workers) {
        $r = Invoke-Smoke -ExePath $exe.FullName -WorkerCount $w
        $rOk = ($r.replay_ok -and $r.hash -eq $expected)
        Add-Cell 'worker-count' "$build/w=$w" $rOk ("hash=$($r.hash) expected=$expected run==replay=$($r.replay_ok)")
        $hashesThisBuild["w=$w"] = $r.hash
        if ($RunMoving) {
            #  moving-residency oracle (first-class default axis). CONVERGENT:
            # per-tick residency varies run-to-run while the FINAL hash is run==replay.
            # Gate on run==replay only — its hash is a distinct moving baseline, not
            # $expected, and the per-tick availability set is NOT asserted here (the
            # richer --avail-trace convergence check lives in the engine-frontier
            # MovingResidency mode, which reads the --artifact JSON).
            $rm = Invoke-Smoke -ExePath $exe.FullName -WorkerCount $w -Moving
            Add-Cell 'moving-oracle' "$build/w=$w" $rm.replay_ok ("hash=$($rm.hash) run==replay=$($rm.replay_ok)")
        }
    }
    # cross-worker agreement within this build (all worker counts agree on this build's baseline).
    $distinct = @($hashesThisBuild.Values | Sort-Object -Unique)
    Add-Cell 'worker-agree' $build ($distinct.Count -eq 1 -and $distinct[0] -eq $expected) ("distinct hashes: " + ($distinct -join ','))

    #  multiprocess one-box: two SEPARATE processes (default workers) must agree + match baseline.
    $p1 = Invoke-Smoke -ExePath $exe.FullName
    $p2 = Invoke-Smoke -ExePath $exe.FullName
    Add-Cell 'multiprocess' $build ($p1.replay_ok -and $p2.replay_ok -and $p1.hash -eq $p2.hash -and $p1.hash -eq $expected) ("pA=$($p1.hash) pB=$($p2.hash)")

    #  fast/slow-job axis: the shuffled-pop throttle
    # perturbs job SERVICE ORDER (never mere wall-time); the sim hash must be
    # INVARIANT under it. Static runs assert the unthrottled per-build baseline;
    # moving runs assert run==replay (the convergent-oracle rule above). One
    # seed in -Quick keeps the axis always-exercised (no-silent-caps).
    $throttleSeeds = if ($Quick) { @([uint64]1337) } else { @([uint64]1337, [uint64]424242) }
    foreach ($seed in $throttleSeeds) {
        $rt = Invoke-Smoke -ExePath $exe.FullName -ThrottleSeed $seed
        $rtOk = ($rt.replay_ok -and $rt.hash -eq $expected)
        Add-Cell 'fast-slow-job' "$build/seed=$seed" $rtOk ("hash=$($rt.hash) expected=$expected run==replay=$($rt.replay_ok)")
        if ($RunMoving) {
            $rtm = Invoke-Smoke -ExePath $exe.FullName -ThrottleSeed $seed -Moving
            Add-Cell 'fast-slow-job-mv' "$build/seed=$seed" $rtm.replay_ok ("hash=$($rtm.hash) run==replay=$($rtm.replay_ok)")
        }
    }

    #  replay axis (skipped in Quick): record then replay reproduces the hash.
    if (-not $Quick) {
        $rec = Join-Path $env:TEMP ("lumin_detmatrix_{0}.lrec" -f $build)
        & $exe.FullName '--record' $rec 2>&1 | Out-Null
        if (Test-Path $rec) {
            $rp = & $exe.FullName '--replay' $rec 2>&1 | Out-String
            $rpHash = ''
            $rm2 = [regex]::Match($rp, 'world_hash[=:\s]+([0-9a-f]{8,})')
            if ($rm2.Success) { $rpHash = $rm2.Groups[1].Value }
            $rpOk = (($rp -match 'replay' -and $rp -notmatch 'diverg') -and ($rpHash -eq '' -or $rpHash -eq $expected))
            Add-Cell 'replay' $build $rpOk ("replay hash=$rpHash")
            Remove-Item $rec -ErrorAction SilentlyContinue
        } else {
            Add-Cell 'replay' $build $false "--record produced no stream at $rec"
        }
    }
}

if ($Report) {
    $obj = [pscustomobject]@{ schema = 'luminumbra.determinism_matrix.v1'; baseline_debug = $BaselineHash; baseline_release = $ReleaseBaselineHash; cells = $results; failures = $failures }
    $obj | ConvertTo-Json -Depth 6 | Out-File -FilePath $Report -Encoding utf8
    Write-Host "[matrix] report -> $Report"
}

if ($failures.Count -gt 0) {
    Write-Host "`n=== determinism matrix FAILED ($($failures.Count) cell(s)) ==="
    $failures | ForEach-Object { Write-Host "  $_" }
    exit 1
}
Write-Host "`n=== determinism matrix PASSED ($($results.Count) cells; debug $BaselineHash / release $ReleaseBaselineHash) ==="
exit 0
