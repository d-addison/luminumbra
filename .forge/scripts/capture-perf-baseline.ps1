param(
    [int]$Runs = 3,
    [string]$BuildPreset = "debug"
)

# Captures a blessed perf baseline for the PerfRegression gate
# (.forge/scripts/validate-engine-frontier.ps1 -Mode PerfRegression).
# Runs the perf benchmark $Runs times and writes the per-scenario MEDIAN of
# p50/p95/p99/max/mem into .forge/artifacts/engine-frontier/perf-baseline.json
# with machine_id=$env:COMPUTERNAME and status="blessed". Any previously
# blessed values are preserved in a "previous" block.

$ErrorActionPreference = "Stop"

if ($Runs -lt 1) {
    throw "Runs must be at least 1"
}

$BaselinePath = ".forge/artifacts/engine-frontier/perf-baseline.json"
$SummaryPath = "build/$BuildPreset/test-artifacts/performance_framework/benchmark_summary.json"
$Exe = "build/$BuildPreset/bin/initial_world_loading_perf_test.exe"

if (-not (Test-Path $Exe)) {
    throw "Missing perf test executable: $Exe (build target initial_world_loading_perf_test first)"
}

$ScenarioNames = @(
    "boot", "create_world", "enter_spawn", "idle_horizon", "pan_camera",
    "streaming_walk", "chunk_churn", "shader_warmup", "shutdown"
)
$MetricNames = @("p50_ms", "p95_ms", "p99_ms", "max_ms", "mem_high_water_mb")

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

# samples[scenario][metric] = list of doubles, one per run
$samples = @{}
foreach ($scenario in $ScenarioNames) {
    $samples[$scenario] = @{}
    foreach ($metric in $MetricNames) {
        $samples[$scenario][$metric] = New-Object System.Collections.Generic.List[double]
    }
}

for ($run = 1; $run -le $Runs; $run++) {
    Write-Host "capture-perf-baseline: run $run of $Runs"
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

    foreach ($scenario in $ScenarioNames) {
        $entries = @($summary.scenarios | Where-Object { $_.name -eq $scenario })
        if ($entries.Count -ne 1) {
            throw "benchmark summary must contain exactly one '$scenario' scenario, found $($entries.Count)"
        }
        $metrics = $entries[0].regression_metrics
        if ($null -eq $metrics) {
            throw "benchmark summary scenario '$scenario' is missing the regression_metrics block"
        }
        foreach ($metric in $MetricNames) {
            $samples[$scenario][$metric].Add([double]$metrics.$metric)
        }
    }
}

$previous = $null
if (Test-Path $BaselinePath) {
    $existing = Get-Content $BaselinePath -Raw | ConvertFrom-Json
    if ($existing.schema -eq "luminumbra.perf_baseline.v1" -and $existing.status -eq "blessed") {
        $previous = [ordered]@{
            captured_at = $existing.captured_at
            machine_id = $existing.machine_id
            runs_aggregated = $existing.runs_aggregated
            scenarios = $existing.scenarios
        }
    }
}

$scenarioBlock = [ordered]@{}
foreach ($scenario in $ScenarioNames) {
    $entry = [ordered]@{}
    foreach ($metric in $MetricNames) {
        $entry[$metric] = [Math]::Round((Get-Median -Values $samples[$scenario][$metric].ToArray()), 6)
    }
    $scenarioBlock[$scenario] = $entry
}

$baseline = [ordered]@{
    schema = "luminumbra.perf_baseline.v1"
    captured_at = (Get-Date).ToUniversalTime().ToString("yyyy-MM-ddTHH:mm:ssZ")
    machine_id = $env:COMPUTERNAME
    runs_aggregated = $Runs
    status = "blessed"
    scenarios = $scenarioBlock
}
if ($null -ne $previous) {
    $baseline.previous = $previous
}

$json = $baseline | ConvertTo-Json -Depth 10
$resolvedDir = Resolve-Path (Split-Path $BaselinePath -Parent)
$outputPath = Join-Path $resolvedDir (Split-Path $BaselinePath -Leaf)
[System.IO.File]::WriteAllText($outputPath, $json + "`n", (New-Object System.Text.UTF8Encoding($false)))

Write-Host "capture-perf-baseline: wrote blessed baseline ($Runs runs, machine $env:COMPUTERNAME) to $BaselinePath"
