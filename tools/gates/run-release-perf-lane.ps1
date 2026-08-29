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
# 3. Writes tools/gates/baselines/perf-baseline-release.json
#    (same luminumbra.perf_baseline.v1 schema as the debug baseline,
#    machine_id pinned to $env:COMPUTERNAME).
# Validate with:
#   tools/gates/validate-engine-frontier.ps1 -Mode PerfRegression -Preset release

$ErrorActionPreference = "Stop"

# T-I4-19 closeout: run a native command with stderr NOT treated as a
# terminating error, then throw only on a non-zero exit code. Windows
# PowerShell 5.1 wraps every native stderr line as a NativeCommandError, and
# under $ErrorActionPreference='Stop' that aborts the lane on a BENIGN cmake
# warning (e.g. the vendor `cmake_minimum_required` deprecation) before the
# explicit $LASTEXITCODE check can run. Exit code remains the source of truth.
function Invoke-Native {
    param(
        [Parameter(Mandatory)][string]$What,
        [Parameter(Mandatory)][scriptblock]$Call
    )
    $prev = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    try { & $Call } finally { $ErrorActionPreference = $prev }
    if ($LASTEXITCODE -ne 0) {
        throw "$What failed with exit code $LASTEXITCODE"
    }
}

$RepoRoot = Resolve-Path (Join-Path $PSScriptRoot "..\..")
Push-Location $RepoRoot
try {
    Write-Host "release-perf-lane: configuring release preset"
    Invoke-Native "cmake --preset release" { cmake --preset release }

    Write-Host "release-perf-lane: building release preset"
    # Known flake: gtest test discovery has a 5s timeout that occasionally
    # trips on a loaded machine. Retry the build once (ninja resumes
    # incrementally). stderr is non-terminating here for the same reason as
    # Invoke-Native; the retry decision and the final throw both key off
    # $LASTEXITCODE, never off a stderr line.
    $ErrorActionPreference = 'Continue'
    cmake --build --preset release
    if ($LASTEXITCODE -ne 0) {
        Write-Host "release-perf-lane: build failed once (gtest discovery flake?); retrying"
        cmake --build --preset release
    }
    $buildExit = $LASTEXITCODE
    $ErrorActionPreference = 'Stop'
    if ($buildExit -ne 0) {
        throw "cmake --build --preset release failed twice with exit code $buildExit"
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

    $baselinePath = "tools/gates/baselines/perf-baseline-release.json"
    if (-not (Test-Path $baselinePath)) {
        throw "release-perf-lane: expected baseline file was not written: $baselinePath"
    }
    Write-Host "release-perf-lane: done -> $baselinePath"
} finally {
    Pop-Location
}
