param(
    [int]$Runs = 3,
    # By default the lane writes a PROVISIONAL release baseline
    # (status="provisional", provisional=true): good for verifying the lane
    # end-to-end on a noisy machine, never enforced by the PerfRegression
    # gate. The orchestrator re-runs with -Bless on a quiet machine to write
    # the real blessed baseline.
    [switch]$Bless
)

# T-I3-20: release perf lane.
# 1. Configures + builds the "release" CMake preset (build/release).
# 2. Runs the 9 perf scenarios (initial_world_loading_perf_test) against the
#    release build, $Runs times, via capture-perf-baseline.ps1 -Preset release.
# 3. Writes .forge/artifacts/engine-frontier/perf-baseline-release.json
#    (same luminumbra.perf_baseline.v1 schema as the debug baseline,
#    machine_id pinned to $env:COMPUTERNAME).
# Validate with:
#   .forge/scripts/validate-engine-frontier.ps1 -Mode PerfRegression -Preset release

$ErrorActionPreference = "Stop"

$RepoRoot = Resolve-Path (Join-Path $PSScriptRoot "..\..")
Push-Location $RepoRoot
try {
    Write-Host "release-perf-lane: configuring release preset"
    cmake --preset release
    if ($LASTEXITCODE -ne 0) {
        throw "cmake --preset release failed with exit code $LASTEXITCODE"
    }

    Write-Host "release-perf-lane: building release preset"
    cmake --build --preset release
    if ($LASTEXITCODE -ne 0) {
        # Known flake: gtest test discovery has a 5s timeout that
        # occasionally trips on a loaded machine. Retry the build once;
        # ninja resumes incrementally.
        Write-Host "release-perf-lane: build failed once (gtest discovery flake?); retrying"
        cmake --build --preset release
        if ($LASTEXITCODE -ne 0) {
            throw "cmake --build --preset release failed twice with exit code $LASTEXITCODE"
        }
    }

    $captureScript = Join-Path $PSScriptRoot "capture-perf-baseline.ps1"
    $captureArgs = @{
        Runs = $Runs
        Preset = "release"
    }
    if (-not $Bless) {
        $captureArgs.Provisional = $true
    }

    Write-Host "release-perf-lane: capturing release baseline ($Runs runs, $(if ($Bless) { 'BLESSED' } else { 'provisional' }))"
    & $captureScript @captureArgs

    $baselinePath = ".forge/artifacts/engine-frontier/perf-baseline-release.json"
    if (-not (Test-Path $baselinePath)) {
        throw "release-perf-lane: expected baseline file was not written: $baselinePath"
    }
    Write-Host "release-perf-lane: done -> $baselinePath"
} finally {
    Pop-Location
}
