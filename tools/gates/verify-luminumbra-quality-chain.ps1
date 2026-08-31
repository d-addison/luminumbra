[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$Baseline,
    [Parameter(Mandatory = $true)][string]$Measurement,
    [Parameter(Mandatory = $true)][string]$Predecessor,
    [Parameter(Mandatory = $true)][string]$Verdict,
    [Parameter(Mandatory = $true)][ValidateSet("visual", "audio", "ui", "game_feel")][string]$ExpectedSurface,
    [Parameter(Mandatory = $true)][ValidateRange(1, 3)][int]$ExpectedIteration,
    [Parameter(Mandatory = $true)][string]$NegativeFixture
)

$ErrorActionPreference = "Stop"

function Read-JsonFile([string]$Path, [string]$Description) {
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) { throw "$Description is missing: $Path" }
    try { return Get-Content -LiteralPath $Path -Raw | ConvertFrom-Json } catch { throw "$Description is invalid JSON: $Path ($($_.Exception.Message))" }
}

function Get-Sha256([string]$Path) {
    return (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant()
}

function Assert-Equal($Actual, $Expected, [string]$Description) {
    if ($Actual -ne $Expected) { throw "$Description (actual='$Actual', expected='$Expected')" }
}

function Assert-Hash([string]$Value, [string]$Description) {
    if ($Value -notmatch '^[0-9a-fA-F]{64}$') { throw "$Description is not SHA-256: $Value" }
}

function Assert-StringSet($Actual, $Expected, [string]$Description) {
    $actualValues = @($Actual | ForEach-Object { [string]$_ } | Sort-Object -Unique)
    $expectedValues = @($Expected | ForEach-Object { [string]$_ } | Sort-Object -Unique)
    if (($actualValues -join "`n") -ne ($expectedValues -join "`n")) { throw "$Description differs" }
}

function Test-VerdictObject($Object, $BaselineObject, $MeasurementObject) {
    Assert-Equal $Object.schema "luminumbra.quality_loop_verdict.v1" "verdict schema"
    Assert-Equal $Object.surface $ExpectedSurface "verdict surface"
    Assert-Equal ([int]$Object.iteration) $ExpectedIteration "verdict iteration"
    foreach ($field in @("baseline_sha256", "measurement_sha256", "predecessor_verdict_sha256")) { Assert-Hash ([string]$Object.$field) "verdict $field" }
    Assert-Equal ([string]$Object.baseline_sha256).ToLowerInvariant() (Get-Sha256 $Baseline) "verdict baseline hash"
    Assert-Equal ([string]$Object.measurement_sha256).ToLowerInvariant() (Get-Sha256 $Measurement) "verdict measurement hash"
    Assert-Equal ([string]$Object.predecessor_verdict_sha256).ToLowerInvariant() (Get-Sha256 $Predecessor) "verdict predecessor hash"
    if (@("pass", "fail", "noop") -notcontains [string]$Object.gate_result) { throw "verdict gate_result is invalid" }
    if ($Object.verified_noop -isnot [bool]) { throw "verdict verified_noop must be a JSON boolean" }
    if ([string]$Object.gate_result -eq "noop") {
        if (-not [bool]$Object.verified_noop) { throw "noop verdict must set verified_noop=true" }
    } elseif ([bool]$Object.verified_noop) { throw "non-noop verdict cannot set verified_noop=true" }
    if ([string]$Object.gate_result -eq "fail" -and @($Object.failed_gate_ids).Count -eq 0) { throw "failed verdict lacks failed_gate_ids" }
    if ([string]$Object.gate_result -ne "fail" -and @($Object.failed_gate_ids).Count -ne 0) { throw "passing/noop verdict carries failed_gate_ids" }
    if (@($Object.source_finding_ids).Count -eq 0) { throw "verdict lacks source_finding_ids" }
    Assert-StringSet $Object.source_finding_ids $BaselineObject.source_finding_ids "verdict/baseline source findings"
    Assert-StringSet $Object.source_finding_ids $MeasurementObject.source_finding_ids "verdict/measurement source findings"
    Assert-Equal $Object.gate_result $MeasurementObject.gate_result "verdict/measurement gate result"
    Assert-StringSet $Object.failed_gate_ids $MeasurementObject.failed_gate_ids "verdict/measurement failed gates"
}

$baselineObject = Read-JsonFile $Baseline "quality baseline"
$measurementObject = Read-JsonFile $Measurement "quality measurement"
$predecessorObject = Read-JsonFile $Predecessor "quality predecessor"
$verdictObject = Read-JsonFile $Verdict "quality verdict"

Assert-Equal $baselineObject.schema "luminumbra.quality_loop_baseline.v1" "baseline schema"
Assert-Equal $baselineObject.surface $ExpectedSurface "baseline surface"
if (@($baselineObject.source_finding_ids).Count -eq 0) { throw "baseline lacks source_finding_ids" }
Assert-Equal $measurementObject.schema "luminumbra.quality_loop_measurement.v1" "measurement schema"
Assert-Equal $measurementObject.surface $ExpectedSurface "measurement surface"
Assert-Equal ([int]$measurementObject.iteration) $ExpectedIteration "measurement iteration"
Assert-Equal ([string]$measurementObject.baseline_sha256).ToLowerInvariant() (Get-Sha256 $Baseline) "measurement baseline hash"
Assert-Equal ([string]$measurementObject.predecessor_verdict_sha256).ToLowerInvariant() (Get-Sha256 $Predecessor) "measurement predecessor hash"
if (@("pass", "fail", "noop") -notcontains [string]$measurementObject.gate_result) { throw "measurement gate_result is invalid" }
if (@($measurementObject.source_finding_ids).Count -eq 0) { throw "measurement lacks source_finding_ids" }
if (@($measurementObject.metrics.PSObject.Properties).Count -eq 0) { throw "measurement lacks metrics" }
if ([string]$measurementObject.gate_result -eq "fail" -and @($measurementObject.failed_gate_ids).Count -eq 0) { throw "failed measurement lacks failed_gate_ids" }
if ([string]$measurementObject.gate_result -ne "fail" -and @($measurementObject.failed_gate_ids).Count -ne 0) { throw "passing/noop measurement carries failed_gate_ids" }
Assert-StringSet $measurementObject.source_finding_ids $baselineObject.source_finding_ids "measurement/baseline source findings"
if ($ExpectedIteration -eq 1) {
    Assert-Equal (Get-Sha256 $Predecessor) (Get-Sha256 $Baseline) "iteration-one predecessor/baseline hash"
} else {
    Assert-Equal $predecessorObject.schema "luminumbra.quality_loop_verdict.v1" "predecessor verdict schema"
    Assert-Equal $predecessorObject.surface $ExpectedSurface "predecessor verdict surface"
    Assert-Equal ([int]$predecessorObject.iteration) ($ExpectedIteration - 1) "predecessor verdict iteration"
    Assert-StringSet $predecessorObject.source_finding_ids $baselineObject.source_finding_ids "predecessor/baseline source findings"
}
Test-VerdictObject $verdictObject $baselineObject $measurementObject

$negativeObject = Read-JsonFile $NegativeFixture "quality-chain negative fixture"
Assert-Equal $negativeObject.schema "luminumbra.quality_loop_negative_fixture.v1" "negative fixture schema"
Assert-Equal $negativeObject.mutation "corrupt_measurement_sha256" "negative fixture mutation"
if ($null -eq $negativeObject.mutated_verdict) { throw "negative fixture lacks mutated_verdict" }
$mutatedVerdict = $negativeObject.mutated_verdict
foreach ($field in @("schema", "surface", "iteration", "baseline_sha256", "predecessor_verdict_sha256", "gate_result", "verified_noop")) { Assert-Equal $mutatedVerdict.$field $verdictObject.$field "negative fixture changed non-target field $field" }
Assert-StringSet $mutatedVerdict.failed_gate_ids $verdictObject.failed_gate_ids "negative fixture changed failed_gate_ids"
Assert-StringSet $mutatedVerdict.source_finding_ids $verdictObject.source_finding_ids "negative fixture changed source_finding_ids"
Assert-Hash ([string]$mutatedVerdict.measurement_sha256) "negative fixture corrupted measurement hash"
if ([string]$mutatedVerdict.measurement_sha256 -eq [string]$verdictObject.measurement_sha256) { throw "negative fixture did not corrupt measurement_sha256" }
$negativeRejected = $false
try { Test-VerdictObject $mutatedVerdict $baselineObject $measurementObject } catch { $negativeRejected = $true }
if (-not $negativeRejected) { throw "quality-chain negative fixture was accepted" }

Write-Output "quality chain verified; corrupt-SHA negative fixture rejected"
