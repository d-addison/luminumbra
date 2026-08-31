<#
.SYNOPSIS
    Opt-in measured-coverage lane (coverage-instrumentation slice).

.DESCRIPTION
    Configures + builds the dedicated `coverage` CMake preset (binaryDir
    build/coverage, instrumented with gcov --coverage), runs the test suite to
    populate .gcda counters, then invokes tools/coverage_summary.py to emit a
    measured coverage_summary.json under
    build/coverage/test-artifacts/coverage/.

    This lane is fully isolated from the determinism-gated build/ and
    build/debug trees: it never writes into them and never records a world_hash
    baseline, so it cannot perturb any determinism gate. It is opt-in (NOT part
    of validate-engine-frontier.ps1's default flow) to keep the determinism path
    fast.

    The validator's coverage gate can then be run against this tree with:
        pwsh -File tools/gates/validate-engine-frontier.ps1 `
             -Mode UiTestBaseline -BuildPreset coverage

.PARAMETER MinimumRequiredPercent
    The honest line-coverage floor written into the summary and asserted by the
    gate. Defaults to the ratcheted floor.

.PARAMETER SkipBuild
    Reuse an already-built build/coverage tree (only re-run tests + summary).
#>
[CmdletBinding()]
param(
    # Honest measured floor (coverage-instrumentation ). First full
    # instrumented run measured ~65% overall line coverage; the floor sits a few
    # points below to absorb run-to-run variance (audio/tools are GPU-/device-/
    # build-time-bound and unexercised headlessly). Ratchet UPWARD toward the
    # aspirational config.yaml coverage_gate: 95 as headless coverage grows.
    [double]$MinimumRequiredPercent = 60.0,
    [switch]$SkipBuild,
    [switch]$SkipTests
)

$ErrorActionPreference = "Stop"

# PATH discipline: the matching gcc/gcov 15.1.0 pair lives in ucrt64; KiCad /
# mingw on PATH would otherwise shadow it (cc1plus exit 127). Prepend it.
$ucrt = "C:\msys64\ucrt64\bin"
if (Test-Path $ucrt) {
    $env:PATH = "$ucrt;$env:PATH"
} else {
    Write-Warning "ucrt64 bin not found at $ucrt; relying on ambient PATH."
}

$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..\..")).Path
Push-Location $repoRoot
try {
    $buildDir = Join-Path $repoRoot "build\coverage"

    if (-not $SkipBuild) {
        Write-Host "== configure coverage preset ==" -ForegroundColor Cyan
        & cmake --preset coverage
        if ($LASTEXITCODE -ne 0) { throw "cmake configure (coverage) failed" }

        Write-Host "== build coverage preset ==" -ForegroundColor Cyan
        & cmake --build --preset coverage
        if ($LASTEXITCODE -ne 0) { throw "cmake build (coverage) failed" }
    }

    if (-not $SkipTests) {
        # Reset stale .gcda so percentages reflect THIS run only (coverage contract).
        Write-Host "== clean stale .gcda ==" -ForegroundColor Cyan
        Get-ChildItem -Path $buildDir -Recurse -Filter *.gcda -ErrorAction SilentlyContinue |
            Remove-Item -Force -ErrorAction SilentlyContinue

        Write-Host "== run instrumented test suite ==" -ForegroundColor Cyan
        # Non-zero ctest exit (a failing/flaky test) must not abort coverage
        # aggregation: partial .gcda still yields a real measurement.
        & ctest --preset coverage --output-on-failure -E "_NOT_BUILT$"
        if ($LASTEXITCODE -ne 0) {
            Write-Warning "ctest reported failures (exit $LASTEXITCODE); aggregating partial coverage anyway."
        }
    }

    Write-Host "== aggregate coverage summary ==" -ForegroundColor Cyan
    $python = (Get-Command python -ErrorAction SilentlyContinue)
    if (-not $python) { throw "python not found on PATH for coverage_summary.py" }

    $outPath = Join-Path $buildDir "test-artifacts\coverage\coverage_summary.json"
    & $python.Source (Join-Path $repoRoot "tools\coverage_summary.py") `
        --build-dir $buildDir `
        --source-root $repoRoot `
        --output $outPath `
        --build-preset coverage `
        --gcov "gcov" `
        --minimum-required-percent $MinimumRequiredPercent
    $summaryExit = $LASTEXITCODE

    Write-Host ""
    Write-Host "coverage summary written to: $outPath" -ForegroundColor Green
    if ($summaryExit -ne 0) {
        Write-Warning "measured line coverage is BELOW the floor ($MinimumRequiredPercent%) -> gate would FAIL."
    }
    exit $summaryExit
}
finally {
    Pop-Location
}
