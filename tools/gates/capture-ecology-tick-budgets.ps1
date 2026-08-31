param(
    [int]$Runs = 3,
    # The build tree whose ecology_tick_perf_test.exe is measured. Budgets are only
    # meaningful (and only blessable) from RELEASE timings.
    [string]$BuildPreset = "release",
    # Headroom over the observed median-of-medians so the ceiling catches a QUIET
    # regression (e.g. an O(N^2) path sneaking back in) without flaking on ordinary
    # run-to-run scheduler noise. budget_median_ms = median(median_ms over runs) * this.
    [double]$HeadroomMultiplier = 1.5,
    # -Bless marks the reviewed summary as enforceable. Without it, the capture
    # remains record-only.
    [switch]$Bless
)

# capture + (optionally) bless the ECOLOGY-TICK perf budgets.
#
# Runs the EcologyTickPerf gtest (N in {256,1k,4k}, 300 headless ticks per roster,
# the kinematic PopulatedWorldReplay roster shape) $Runs times, takes the per-N
# MEDIAN of the run medians, applies the headroom multiplier, and writes the
# compact reviewed summary at tools/gates/baselines/ecology-tick-release.json.
# Validate with:
#   tools/gates/validate-engine-frontier.ps1 -Mode EcologyTickPerf -BuildPreset release
#
# STANDING RULE: there is NO ecology budget CAP in the sim — the whole
# roster ticks every tick, and these budgets price that full-roster cost. Any future
# per-tick ecology work cap MUST be a deterministic rotating id-sorted window (the
# WaterSystem MAX_WATER_SIMS_PER_TICK pattern), NEVER a time-based/adaptive cutoff.

$ErrorActionPreference = "Stop"

if ($Runs -lt 1) {
    throw "Runs must be at least 1"
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

$RepoRoot = Resolve-Path (Join-Path $PSScriptRoot "..\..")
Push-Location $RepoRoot
try {
    $FloorPath = "tools/gates/baselines/ecology-tick-release.json"
    $Exe = "build/$BuildPreset/bin/ecology_tick_perf_test.exe"
    $ArtifactPath = "build/$BuildPreset/test-artifacts/sim/ecology_tick_perf.json"

    if (-not (Test-Path $Exe)) {
        throw "Missing ecology perf test executable: $Exe (build the '$BuildPreset' preset target ecology_tick_perf_test first)"
    }

    # medianSamples[n] / p99Samples[n] = one value per run
    $medianSamples = @{}
    $p99Samples = @{}
    $rosterOrder = @()
    $buildMode = $null
    $ticks = 0

    for ($run = 1; $run -le $Runs; $run++) {
        Write-Host "capture-ecology-tick-budgets: run $run of $Runs"
        if (Test-Path $ArtifactPath) {
            Remove-Item $ArtifactPath
        }
        & $Exe "--gtest_filter=EcologyTickPerf.MeasuresMedianAndP99AcrossRosterSizes"
        if ($LASTEXITCODE -ne 0) {
            throw "ecology perf test run $run failed with exit code $LASTEXITCODE"
        }
        if (-not (Test-Path $ArtifactPath)) {
            throw "ecology perf test run $run did not produce $ArtifactPath"
        }

        $artifact = Get-Content $ArtifactPath -Raw | ConvertFrom-Json
        if ($artifact.schema -ne "luminumbra.ecology_tick_perf.v1") {
            throw "Unexpected ecology perf artifact schema '$($artifact.schema)'"
        }
        if ($null -eq $buildMode) {
            $buildMode = [string]$artifact.build_mode
            $ticks = [int]$artifact.ticks
        } elseif ([string]$artifact.build_mode -ne $buildMode) {
            throw "ecology perf runs disagree on build_mode ('$buildMode' vs '$($artifact.build_mode)')"
        }

        foreach ($r in @($artifact.results)) {
            $n = [int]$r.n
            if ([int]$r.spawned -ne $n) {
                throw "run ${run}: roster N=$n spawned $($r.spawned) creatures (non-vacuous measurement required)"
            }
            if ([double]$r.median_ms -le 0.0) {
                throw "run ${run}: roster N=$n reported a non-positive median ms/tick"
            }
            if (-not $medianSamples.ContainsKey($n)) {
                $medianSamples[$n] = New-Object System.Collections.Generic.List[double]
                $p99Samples[$n] = New-Object System.Collections.Generic.List[double]
                $rosterOrder += $n
            }
            $medianSamples[$n].Add([double]$r.median_ms)
            $p99Samples[$n].Add([double]$r.p99_ms)
        }
    }

    if ($rosterOrder.Count -lt 1) {
        throw "no roster results were collected"
    }
    if ($Bless -and $buildMode -ne "release") {
        throw "refusing to BLESS ecology budgets from a '$buildMode' build: budgets are release-mode ceilings (rerun against the release preset, or omit -Bless for a record-only capture)"
    }

    $budgetBlock = [ordered]@{}
    foreach ($n in $rosterOrder) {
        $observedMedian = Get-Median -Values $medianSamples[$n].ToArray()
        $observedP99 = Get-Median -Values $p99Samples[$n].ToArray()
        $budgetBlock["$n"] = [ordered]@{
            budget_median_ms   = [Math]::Round($observedMedian * $HeadroomMultiplier, 4)
            observed_median_ms = [Math]::Round($observedMedian, 4)
            observed_p99_ms    = [Math]::Round($observedP99, 4)
        }
        Write-Host ("capture-ecology-tick-budgets: N={0,5} observed median {1,8:N3} ms/tick (p99 {2,8:N3} ms) -> budget {3,8:N3} ms" -f `
            $n, $observedMedian, $observedP99, $budgetBlock["$n"].budget_median_ms)
    }

    $status = if ($Bless) { "reviewed" } else { "observation" }
    $floor = [ordered]@{
        schema = "luminumbra.ecology_tick_baseline.v1"
        status = $status
        captured_at = (Get-Date).ToUniversalTime().ToString("yyyy-MM-ddTHH:mm:ssZ")
        build_mode = $buildMode
        runs = $Runs
        ticks = $ticks
        headroom_multiplier = $HeadroomMultiplier
        note = "Reviewed full-roster median-ms/tick ceilings. Work caps use a deterministic stable-id window, never a wall-clock cutoff."
        budgets = $budgetBlock
    }

    $json = $floor | ConvertTo-Json -Depth 10
    $resolvedDir = Resolve-Path (Split-Path $FloorPath -Parent)
    $outputPath = Join-Path $resolvedDir (Split-Path $FloorPath -Leaf)
    [System.IO.File]::WriteAllText($outputPath, $json + "`n", (New-Object System.Text.UTF8Encoding($false)))

    Write-Host "capture-ecology-tick-budgets: wrote $status summary ($Runs runs, build_mode '$buildMode', headroom x$HeadroomMultiplier) into $FloorPath"
    if (-not $Bless) {
        Write-Host "capture-ecology-tick-budgets: observation is record-only. Review it and re-run with -Bless to enforce the median ceilings."
    }
} finally {
    Pop-Location
}
