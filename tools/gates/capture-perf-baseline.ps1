param(
    [int]$Runs = 3,
    [string]$BuildPreset = "",
    # T-I3-20: preset-aware lane selection. -Preset picks BOTH the build dir
    # and the baseline file: debug -> perf-baseline.json + build/debug,
    # release -> perf-baseline-release.json + build/release. Default (empty)
    # preserves the historical behavior exactly: build dir from -BuildPreset
    # (default debug) and the debug baseline file, so existing callers are
    # unchanged. An explicit -BuildPreset still overrides the build dir.
    [ValidateSet("", "debug", "release")]
    [string]$Preset = "",
    # T-I3-20: write status="provisional" + provisional=true instead of
    # "blessed" (used by run-release-perf-lane.ps1 on noisy machines; the
    # orchestrator re-runs without this switch to bless for real).
    [switch]$Provisional
)

# Captures a blessed perf baseline for the PerfRegression gate
# (tools/gates/validate-engine-frontier.ps1 -Mode PerfRegression).
# Runs the perf benchmark $Runs times and writes the per-scenario MEDIAN of
# p50/p95/p99/max/mem into tools/gates/baselines/perf-baseline.json
# (or perf-baseline-release.json for -Preset release) with
# machine_id=$env:COMPUTERNAME and status="blessed". Any previously
# blessed values are preserved in a "previous" block.

$ErrorActionPreference = "Stop"

if ($Runs -lt 1) {
    throw "Runs must be at least 1"
}

if ([string]::IsNullOrWhiteSpace($BuildPreset)) {
    if ([string]::IsNullOrWhiteSpace($Preset)) {
        $BuildPreset = "debug"
    } else {
        $BuildPreset = $Preset
    }
}

if ($Preset -eq "release") {
    $BaselinePath = "tools/gates/baselines/perf-baseline-release.json"
} else {
    $BaselinePath = "tools/gates/baselines/perf-baseline.json"
}
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

# T-I3-22: GPU provenance. Perf timings are GPU/driver-sensitive, so the
# baseline records which adapter produced them. The headless perf test reports
# gpu="unknown" (no GL context), so we query the OS video controller directly.
# Best-effort: on failure or a non-Windows host the fields fall back to
# "unknown" and the gate treats them as absent (warn, never fail).
function Get-GpuProvenance {
    $provenance = [ordered]@{
        gpu_vendor = "unknown"
        gpu_renderer = "unknown"
        driver_version = "unknown"
        source = "unavailable"
    }
    try {
        $controller = $null
        try {
            $controller = Get-CimInstance -ClassName Win32_VideoController -ErrorAction Stop |
                Where-Object { $_.Name -and $_.AdapterRAM -ne $null } |
                Sort-Object -Property AdapterRAM -Descending |
                Select-Object -First 1
        } catch {
            $controller = $null
        }
        if ($null -ne $controller) {
            $provenance.gpu_renderer = [string]$controller.Name
            if ($controller.AdapterCompatibility) {
                $provenance.gpu_vendor = [string]$controller.AdapterCompatibility
            }
            if ($controller.DriverVersion) {
                $provenance.driver_version = [string]$controller.DriverVersion
            }
            $provenance.source = "win32_videocontroller"
            return $provenance
        }
        # Fallback to wmic when CIM is unavailable.
        $wmic = & wmic path win32_VideoController get name,driverversion /format:csv 2>$null
        if ($LASTEXITCODE -eq 0 -and $wmic) {
            $row = @($wmic | Where-Object { $_ -match "," -and $_ -notmatch "^Node,|DriverVersion" }) |
                Select-Object -First 1
            if ($row) {
                $parts = $row.Split(",")
                if ($parts.Count -ge 3) {
                    $provenance.driver_version = $parts[1].Trim()
                    $provenance.gpu_renderer = $parts[2].Trim()
                    $provenance.source = "wmic"
                }
            }
        }
    } catch {
        # leave defaults
    }
    return $provenance
}

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

$gpuProvenance = Get-GpuProvenance

$baseline = [ordered]@{
    schema = "luminumbra.perf_baseline.v1"
    captured_at = (Get-Date).ToUniversalTime().ToString("yyyy-MM-ddTHH:mm:ssZ")
    machine_id = $env:COMPUTERNAME
    runs_aggregated = $Runs
    # T-I3-22 (additive, schema-compatible): GPU/driver provenance. Old
    # baselines without this block are still valid; Test-PerfRegression warns
    # (never fails) when the recording machine's GPU/driver differs.
    gpu = $gpuProvenance
    status = $(if ($Provisional) { "provisional" } else { "blessed" })
    scenarios = $scenarioBlock
}
if ($Provisional) {
    $baseline.provisional = $true
}
if ($null -ne $previous) {
    $baseline.previous = $previous
}

$json = $baseline | ConvertTo-Json -Depth 10
$resolvedDir = Resolve-Path (Split-Path $BaselinePath -Parent)
$outputPath = Join-Path $resolvedDir (Split-Path $BaselinePath -Leaf)
[System.IO.File]::WriteAllText($outputPath, $json + "`n", (New-Object System.Text.UTF8Encoding($false)))

$statusLabel = if ($Provisional) { "provisional" } else { "blessed" }
Write-Host "capture-perf-baseline: wrote $statusLabel baseline ($Runs runs, machine $env:COMPUTERNAME, build preset $BuildPreset) to $BaselinePath"
