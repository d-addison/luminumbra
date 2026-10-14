<#
.SYNOPSIS
    Validate artifacts emitted by the production far-horizon writer without graphics.
.DESCRIPTION
    Runs a file-only C++ fixture in fresh directories, reads both schemas with the
    gate's reader, and uses the gate's independent oracle and station coverage checks.
    Synthetic captures do not prove the native 90-second readiness requirement.
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$FixtureExe
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version 2.0
$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
. (Join-Path $ScriptDir "capture-artifact-validation.ps1")
. (Join-Path $ScriptDir "far-ladder-oracle.ps1")
$checks = 0

function Assert-Test {
    param([bool]$Condition, [string]$Because)
    if (-not $Condition) { throw "FAILED: $Because" }
    $script:checks++
}

function Assert-Rejected {
    param([scriptblock]$Action, [string]$Fragment, [string]$Because)
    $message = $null
    try { & $Action | Out-Null } catch { $message = $_.Exception.Message }
    Assert-Test ($null -ne $message) "$Because must be refused"
    Assert-Test ($message.Contains($Fragment)) "$Because must name '$Fragment': $message"
}

function Read-WriterFixture {
    param([string]$Directory, [string]$Mode)
    New-Item -ItemType Directory -Path $Directory | Out-Null
    & $script:fixturePath $Directory $Mode | Out-Host
    if ($LASTEXITCODE -ne 0) { throw "Writer fixture ($Mode) exited with code $LASTEXITCODE" }
    return [pscustomobject]@{
        Analysis = Read-JsonArtifact -Path (Join-Path $Directory "farlod-horizon-analysis.json") -Schema "luminumbra.farlod_horizon.v1"
        Oracle = Read-JsonArtifact -Path (Join-Path $Directory "farlod-horizon-ladder-oracle.json") -Schema "luminumbra.farlod_horizon.v2"
    }
}

function Assert-WriterFixture {
    param([object]$Pair)
    $analysis = $Pair.Analysis
    $oracle = $Pair.Oracle
    $result = Assert-FarLadderOracleArtifact -Artifact $oracle -Name "writer oracle"
    Assert-Test ($result.Mode -ceq "legacy") "writer reports mode legacy"
    Assert-Test ($result.LadderChecked -eq $false) "writer reports LadderChecked false"
    Assert-Test ($result.LegacyChecked -eq $true) "writer checks legacy coverage"
    Assert-Test ($null -eq $result.MissingExpected) "legacy writer does not claim ladder residency"
    Assert-Test (@($oracle.ladder.resident_tiles).Count -eq 0) "legacy writer emits no ladder residents"
    Assert-Test ($oracle.ladder.stale_tiles -eq 0) "legacy writer emits zero stale tiles"
    foreach ($station in $oracle.stations) {
        Assert-Test ($station.snapshot.requested_cave_policy -eq 1) "writer preserves requested BoxFiltered"
        Assert-Test ($null -eq $station.snapshot.bound_cave_policy) "writer explicitly emits no bound cave policy"
    }
    Assert-Test ($analysis.world_preset -ceq "mountains" -and $oracle.world_preset -ceq "mountains") "writer preserves the supplied preset in both artifacts"
    Assert-Test ($oracle.camera.world_x -eq 0 -and $oracle.camera.world_z -eq 0) "oracle uses the supplied origin camera"
    Assert-Test ($analysis.aggregates.captured_stations -eq 2 -and $analysis.aggregates.expected_stations -eq 2) "writer emits both supplied stations"
    Assert-Test (@($analysis.stations).Count -eq 2) "v1 has both station rows"
    Assert-Test ($oracle.stations[0].camera.yaw_degrees -eq 12 -and $oracle.stations[0].camera.pitch_degrees -eq -4) "v2 serializes measured camera orientation"
    Assert-Test ($analysis.stations[0].camera.yaw_degrees -eq 0 -and $analysis.stations[0].camera.pitch_degrees -eq 0) "v1 retains historical station label serialization"
    Assert-Test ($analysis.stations[0].camera.world_x -eq 0 -and $analysis.stations[0].camera.world_z -eq 0) "v1 preserves the origin camera"
    Assert-Test ($analysis.stations[1].camera.world_x -eq -512 -and $analysis.stations[1].camera.world_z -eq 1024) "v1 preserves the translated camera"
    foreach ($counter in @("regions_wanted", "regions_resident", "regions_missing")) {
        $v1Count = Get-FarOracleInteger $analysis.farlod.$counter "writer analysis" "farlod.$counter"
        $v2Count = Get-FarOracleInteger $oracle.legacy.$counter "writer oracle" "legacy.$counter"
        Assert-Test ($v1Count -eq $v2Count) "v1 and v2 agree on $counter"
        Assert-Test ($v1Count -eq $analysis.stations[0].farlod.$counter) "selected counters match the supplied origin station's $counter"
    }
    Assert-Test ($analysis.farlod.regions_wanted -eq 132 -and $analysis.farlod.regions_resident -eq 132 -and $analysis.farlod.regions_missing -eq 0) "selected counters match the deterministic complete capture"
}

$fixturePath = (Resolve-Path -LiteralPath $FixtureExe).Path
$root = Join-Path ([System.IO.Path]::GetTempPath()) ("far-ladder-writer-" + [guid]::NewGuid().ToString("N"))
New-Item -ItemType Directory -Path $root | Out-Null
try {
    $completeDir = Join-Path $root "complete"
    $complete = Read-WriterFixture -Directory $completeDir -Mode "complete"
    Assert-WriterFixture -Pair $complete
    Assert-FarLodHorizonStationCoverage -Analysis $complete.Analysis -Name "complete writer capture"
    Assert-Test ($complete.Analysis.stations[1].farlod.regions_missing -eq 0) "positive translated capture is complete"

    # Corrupt retained aggregate fields through the combined NEW gate path.
    foreach ($fault in @("camera", "expected_tiles", "expected_by_tier", "stale_tiles")) {
        $bad = Get-Content -LiteralPath (Join-Path $completeDir "farlod-horizon-ladder-oracle.json") -Raw | ConvertFrom-Json
        $fragment = $fault
        switch ($fault) {
            "camera" { $bad.camera = $null }
            "expected_tiles" { $bad.ladder.expected_tiles = @() }
            "expected_by_tier" { $bad.ladder.expected_by_tier = @() }
            "stale_tiles" { $bad.ladder.stale_tiles = 1; $fragment = "stale tile" }
        }
        $badPath = Join-Path $completeDir ("bad-aggregate-" + $fault + ".json")
        $bad | ConvertTo-Json -Depth 32 | Set-Content -LiteralPath $badPath -Encoding utf8
        Assert-Rejected {
            Assert-FarHorizonV2Artifact -Path $badPath -Renderer "legacy" -Stations @("origin", "translated")
        } $fragment ("new path corrupt aggregate " + $fault)
    }

    # The PS facade uses the same pinned poses and height relation as Python.
    $poses = Get-Content -LiteralPath (Join-Path $completeDir "farlod-horizon-ladder-oracle.json") -Raw | ConvertFrom-Json
    $template = $poses.stations[0] | ConvertTo-Json -Depth 32
    $poses.stations = @()
    $names = @("eye_yaw_000", "eye_yaw_090", "eye_yaw_180", "eye_yaw_270", "elevated")
    $yaws = @(0, 90, 180, 270, 45)
    for ($i = 0; $i -lt 5; $i++) {
        $row = $template | ConvertFrom-Json
        $row.name = $names[$i]
        $row.camera.yaw_degrees = $yaws[$i]
        $row.camera.pitch_degrees = 0
        $row.snapshot.frame_id = $i + 1
        if ($i -eq 4) {
            $row.camera.pitch_degrees = -20
            $row.camera.world_y = [double]$row.camera.world_y + 78.2
            $row.snapshot.camera[1] = [double]$row.snapshot.camera[1] + 78.2
        }
        $poses.stations += $row
    }
    $poses.expected_stations = 5
    $posePath = Join-Path $completeDir "required-poses.json"
    $poses | ConvertTo-Json -Depth 32 | Set-Content -LiteralPath $posePath -Encoding utf8
    Assert-FarHorizonV2Artifact -Path $posePath -Renderer "legacy" | Out-Null
    $wrongHeight = $poses | ConvertTo-Json -Depth 32 | ConvertFrom-Json
    $wrongHeight.stations[4].camera.world_y += 1
    $wrongHeight.stations[4].snapshot.camera[1] += 1
    $wrongHeight | ConvertTo-Json -Depth 32 | Set-Content -LiteralPath $posePath -Encoding utf8
    Assert-Rejected {
        Assert-FarHorizonV2Artifact -Path $posePath -Renderer "legacy"
    } "far oracle refused" "new path wrong elevated height"
    foreach ($row in $poses.stations) {
        $row.camera = $poses.stations[0].camera
        $row.snapshot.camera = $poses.stations[0].snapshot.camera
    }
    $poses | ConvertTo-Json -Depth 32 | Set-Content -LiteralPath $posePath -Encoding utf8
    Assert-Rejected {
        Assert-FarHorizonV2Artifact -Path $posePath -Renderer "legacy"
    } "far oracle refused" "new path identical poses under five names"

    # Corrupt an actual emitted vector, serialize it, and use the same strict
    # reader again. The independent oracle must reject the changed tile key.
    $complete.Oracle.ladder.expected_tiles[0][1] = [int64]$complete.Oracle.ladder.expected_tiles[0][1] + 1000
    $corruptPath = Join-Path $completeDir "corrupted-oracle.json"
    $complete.Oracle | ConvertTo-Json -Depth 16 | Set-Content -LiteralPath $corruptPath -Encoding utf8
    Assert-Rejected {
        $corrupt = Read-JsonArtifact -Path $corruptPath -Schema "luminumbra.farlod_horizon.v2"
        Assert-FarLadderOracleArtifact -Artifact $corrupt -Name "corrupted writer oracle"
    } "expected_tiles" "corrupted emitted expected-tile vector"

    # Also verify every station's wanted count, beyond the selected aggregate.
    $complete.Analysis.stations[1].farlod.regions_wanted = 133
    Assert-Rejected {
        Assert-FarLodHorizonStationCoverage -Analysis $complete.Analysis -Name "incorrect station count"
    } "stations[1] 'translated' regions_wanted" "nonselected station with incorrect wanted count"

    $completeResult = Assert-FarHorizonV2Artifact -Path (Join-Path (Join-Path $root "complete") "farlod-horizon-ladder-oracle.json") -Renderer "legacy" -Stations @("origin", "translated")
    Assert-Test (-not $completeResult.LadderChecked) "new station schema stays explicit legacy evidence"

    # The production writer still selects the complete station for historical
    # v1/v2 aggregates. The gate must reject the other station's missing region.
    $incomplete = Read-WriterFixture -Directory (Join-Path $root "incomplete") -Mode "incomplete"
    Assert-WriterFixture -Pair $incomplete
    Assert-Test ($incomplete.Analysis.stations[0].farlod.regions_missing -eq 0 -and $incomplete.Analysis.stations[1].farlod.regions_missing -eq 1) "negative fixture has one complete and one incomplete station"
    Assert-Rejected {
        Assert-FarLodHorizonStationCoverage -Analysis $incomplete.Analysis -Name "incomplete writer capture"
    } "stations[1] 'translated' regions_missing" "nonselected incomplete station"

    Write-Host "far-ladder-oracle-writer: $checks checks passed (mode legacy; ladder_checked: false; 90-second readiness unproven)"
} finally {
    Remove-Item -LiteralPath $root -Recurse -Force
}
