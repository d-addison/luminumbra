param(
    [int]$Runs = 3,
    # The release benchmark dir the perf test writes into. The floor is RELEASE
    # only (an absolute frame-ms floor is meaningless against debug timings).
    [string]$BuildPreset = "release",
    # Headroom over the observed p99 so the floor catches a QUIET halving of FPS,
    # not just catastrophic 2-10x blowups (KDD-3). budget_ms = median(p99) * this.
    [double]$HeadroomMultiplier = 1.3,
    # The target GPU class this floor is calibrated for (FR-002). Recorded, NOT
    # pinned to the capture machine.
    [string]$GpuClass = "RTX 5070 Ti",
    [string]$Resolution = "3840x1600",
    # Without -Bless the floor is written status="unblessed" (fail-until-baselined:
    # Test-PerfFloor THROWS). -Bless writes status="blessed" -> the absolute
    # budgets become enforced. Bless ONLY from a confirmed target-GPU release run.
    [switch]$Bless
)

# perf-lane-and-ecology-tick (KDD-3 / T007): capture the RELEASE absolute
# frame-ms floor into tools/gates/baselines/perf-floor-release.json
# (luminumbra.perf_floor.v1). Runs the release perf benchmark $Runs times, takes
# the per-scenario median of p99, multiplies by the headroom, and writes the
# absolute budget_ms per floor-critical scenario. NO machine_id pin (FR-002).
# Validate the resulting floor with:
#   tools/gates/validate-engine-frontier.ps1 -Mode PerfFloor

$ErrorActionPreference = "Stop"

if ($Runs -lt 1) {
    throw "Runs must be at least 1"
}

$FloorPath = "tools/gates/baselines/perf-floor-release.json"
$SummaryPath = "build/$BuildPreset/test-artifacts/performance_framework/benchmark_summary.json"
$Exe = "build/$BuildPreset/bin/initial_world_loading_perf_test.exe"

if (-not (Test-Path $Exe)) {
    throw "Missing release perf test executable: $Exe (build the release preset target initial_world_loading_perf_test first)"
}

# The floor-critical scenarios (FR-001). idle_horizon/pan_camera/streaming_walk
# are pre-existing; forest + ecology are the two NEW scenarios this slice added.
$ScenarioNames = @("idle_horizon", "pan_camera", "streaming_walk", "forest", "ecology")

function Get-Median {
    param([double[]]$Values)
    $sorted = @($Values | Sort-Object)
    $count = $sorted.Count
    if ($count -eq 0) {
        throw "Cannot take the median of an empty sample set"
    }
    if ($count % 2 -eq 1) {
        return [double]$sorted[[int][Math]::Floor($count / 2)]
    }
    return ([double]$sorted[$count / 2 - 1] + [double]$sorted[$count / 2]) / 2.0
}

# samples[scenario] = list of p99 doubles (one per run)
$samples = @{}
foreach ($scenario in $ScenarioNames) {
    $samples[$scenario] = New-Object System.Collections.Generic.List[double]
}

for ($run = 1; $run -le $Runs; $run++) {
    Write-Host "capture-perf-floor: run $run of $Runs"
    & $Exe "--gtest_filter=InitialWorldLoadingPerfTest.PerformanceFrameworkBenchmarkScenariosWriteBudgetArtifacts"
    if ($LASTEXITCODE -ne 0) {
        throw "perf test run $run failed with exit code $LASTEXITCODE"
    }
    if (-not (Test-Path $SummaryPath)) {
        throw "perf test run $run did not produce $SummaryPath"
    }

    $summary = Get-Content $SummaryPath -Raw | ConvertFrom-Json
    if ($summary.schema -ne "luminumbra.performance_framework.benchmark_summary.v1") {
        throw "Unexpected benchmark summary schema '$($summary.schema)'"
    }
    if ($summary.metadata.build_mode -ne "release") {
        throw "perf floor must be captured from a RELEASE build; benchmark build_mode was '$($summary.metadata.build_mode)'"
    }

    foreach ($scenario in $ScenarioNames) {
        $entries = @($summary.scenarios | Where-Object { $_.name -eq $scenario })
        if ($entries.Count -ne 1) {
            throw "benchmark summary must contain exactly one '$scenario' scenario, found $($entries.Count)"
        }
        $p99 = $entries[0].frame_time_ms.p99
        if ($null -eq $p99) {
            throw "benchmark summary scenario '$scenario' is missing frame_time_ms.p99"
        }
        $samples[$scenario].Add([double]$p99)
    }
}

$scenarioBlock = [ordered]@{}
foreach ($scenario in $ScenarioNames) {
    $medianP99 = Get-Median -Values $samples[$scenario].ToArray()
    $budget = [Math]::Round($medianP99 * $HeadroomMultiplier, 4)
    $scenarioBlock[$scenario] = [ordered]@{ budget_ms = $budget }
}

$status = if ($Bless) { "blessed" } else { "unblessed" }

$floor = [ordered]@{
    schema = "luminumbra.perf_floor.v1"
    status = $status
    captured_at = (Get-Date).ToUniversalTime().ToString("yyyy-MM-ddTHH:mm:ssZ")
    gpu_class = $GpuClass
    resolution = $Resolution
    build_mode = "release"
    headroom_multiplier = $HeadroomMultiplier
    note = "perf-lane-and-ecology-tick (FR-002): release-mode ABSOLUTE frame-ms floor. budget_ms = median(p99 over $Runs runs) * headroom_multiplier. NO machine_id pin: this is the target-class floor, not pinned to the capture box. Test-PerfFloor throws until status=='blessed'."
    scenarios = $scenarioBlock
}

# INSTINCT-15: PRESERVE the ecology_tick budgets block. It is owned by
# capture-ecology-tick-budgets.ps1 (independently blessable ecology median-ms/tick
# ceilings); this lane only captures the frame-ms scenarios, and rewriting the floor
# file must not silently drop the ecology budgets that were captured separately.
if (Test-Path $FloorPath) {
    try {
        $existingFloor = Get-Content $FloorPath -Raw | ConvertFrom-Json
        if ($null -ne $existingFloor.ecology_tick) {
            $floor["ecology_tick"] = $existingFloor.ecology_tick
        }
    } catch {
        Write-Host "capture-perf-floor: warning - existing floor file was unreadable; the ecology_tick block (if any) was not preserved"
    }
}

$json = $floor | ConvertTo-Json -Depth 10
$resolvedDir = Resolve-Path (Split-Path $FloorPath -Parent)
$outputPath = Join-Path $resolvedDir (Split-Path $FloorPath -Leaf)
[System.IO.File]::WriteAllText($outputPath, $json + "`n", (New-Object System.Text.UTF8Encoding($false)))

Write-Host "capture-perf-floor: wrote $status floor ($Runs runs, gpu_class '$GpuClass', headroom x$HeadroomMultiplier) to $FloorPath"
if (-not $Bless) {
    Write-Host "capture-perf-floor: floor is UNBLESSED -> Test-PerfFloor stays fail-until-baselined. Re-run with -Bless on confirmed target hardware to enforce the absolute budgets."
}
