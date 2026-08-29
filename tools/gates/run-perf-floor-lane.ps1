param(
    [int]$Runs = 3,
    [double]$HeadroomMultiplier = 1.3,
    [string]$GpuClass = "RTX 5070 Ti",
    [string]$Resolution = "3840x1600",
    # Without -Bless the lane writes an UNBLESSED floor (fail-until-baselined:
    # Test-PerfFloor throws). Re-run with -Bless on confirmed target hardware
    # (the RTX 5070 Ti class) to write the enforced absolute-budget floor.
    [switch]$Bless
)

# perf-lane-and-ecology-tick (KDD-3 / T008): the RELEASE-mode absolute frame-ms
# floor lane.
# 1. Configures + builds the "release" CMake preset (build/release).
# 2. Runs the floor-critical perf scenarios (idle_horizon/pan_camera/
#    streaming_walk + forest + ecology) against the RELEASE build, $Runs times,
#    via capture-perf-floor.ps1.
# 3. Writes tools/gates/baselines/perf-floor-release.json
#    (luminumbra.perf_floor.v1) with ABSOLUTE budget_ms ceilings, NO machine_id
#    pin. UNBLESSED by default (fail-until-baselined, OQ-002); -Bless on the
#    target GPU writes the enforced floor.
# Validate with:
#   tools/gates/validate-engine-frontier.ps1 -Mode PerfFloor

$ErrorActionPreference = "Stop"

# Windows PowerShell 5.1 wraps every native stderr line as a terminating
# NativeCommandError under $ErrorActionPreference='Stop', which aborts the lane
# on a BENIGN cmake warning before the explicit $LASTEXITCODE check. Exit code
# remains the source of truth.
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
    Write-Host "perf-floor-lane: configuring release preset"
    Invoke-Native "cmake --preset release" { cmake --preset release }

    Write-Host "perf-floor-lane: building release preset (initial_world_loading_perf_test)"
    $ErrorActionPreference = 'Continue'
    cmake --build --preset release --target initial_world_loading_perf_test
    if ($LASTEXITCODE -ne 0) {
        Write-Host "perf-floor-lane: build failed once (gtest discovery flake?); retrying"
        cmake --build --preset release --target initial_world_loading_perf_test
    }
    $buildExit = $LASTEXITCODE
    $ErrorActionPreference = 'Stop'
    if ($buildExit -ne 0) {
        throw "cmake --build --preset release failed twice with exit code $buildExit"
    }

    $captureScript = Join-Path $PSScriptRoot "capture-perf-floor.ps1"
    $captureArgs = @{
        Runs = $Runs
        BuildPreset = "release"
        HeadroomMultiplier = $HeadroomMultiplier
        GpuClass = $GpuClass
        Resolution = $Resolution
    }
    if ($Bless) {
        $captureArgs.Bless = $true
    }

    Write-Host "perf-floor-lane: capturing release floor ($Runs runs, $(if ($Bless) { 'BLESSED' } else { 'unblessed' }))"
    & $captureScript @captureArgs

    $floorPath = "tools/gates/baselines/perf-floor-release.json"
    if (-not (Test-Path $floorPath)) {
        throw "perf-floor-lane: expected floor file was not written: $floorPath"
    }
    Write-Host "perf-floor-lane: done -> $floorPath"
} finally {
    Pop-Location
}
