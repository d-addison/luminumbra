<#
.SYNOPSIS
    File-only tests for the independent far-ladder oracle. Run in a clean PowerShell process with -NoProfile.
.DESCRIPTION
    Dot-sources far-ladder-oracle.ps1 and checks the tier table, the expected wanted sets,
    and the artifact assertions with negative controls. Never runs the gate, the engine,
    Blender, the network, or graphics APIs.
#>
[CmdletBinding()]
param()

$ErrorActionPreference = "Stop"
Set-StrictMode -Version 2.0
$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
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

function New-OracleArtifact {
    param([string]$Mode)
    $x = [double]100.5
    $z = [double]-37.25
    $tileArrays = New-Object System.Collections.ArrayList
    $byTier = @(0, 0, 0, 0, 0)
    foreach ($key in @(Get-FarLadderWantedTiles -X $x -Z $z)) {
        $parts = $key.Split(",")
        [void]$tileArrays.Add([object[]]@([int]$parts[0], [int]$parts[1], [int]$parts[2]))
        $index = [int]$parts[0] - 1
        $byTier[$index] = $byTier[$index] + 1
    }
    $residentTiles = @()
    if ($Mode -eq "ladder") { $residentTiles = $tileArrays.ToArray() }
    $wanted = Get-FarLegacyWantedCount -X $x -Z $z
    return @{
        schema = "luminumbra.farlod_horizon.v2"
        world_preset = "mountains"
        mode = $Mode
        camera = @{ world_x = $x; world_z = $z }
        legacy = @{
            region_size_m = 512
            f1_outer_range_m = 768
            f2_outer_range_m = 3000
            regions_wanted = $wanted
            regions_resident = $wanted
            regions_missing = 0
        }
        ladder = @{
            wanted_error = "None"
            expected_by_tier = $byTier
            expected_tiles = $tileArrays.ToArray()
            resident_tiles = $residentTiles
            stale_tiles = 0
        }
    }
}

function ConvertTo-OracleRoundTrip {
    param([hashtable]$Artifact)
    return ($Artifact | ConvertTo-Json -Depth 8 | ConvertFrom-Json)
}

function Remove-OracleTile {
    param([object[]]$Tiles, [scriptblock]$Drop)
    $kept = New-Object System.Collections.ArrayList
    foreach ($tile in $Tiles) {
        if (-not (& $Drop $tile)) { [void]$kept.Add($tile) }
    }
    return ,$kept.ToArray()
}

# Tier table: spacing, tile edge, outer radius and wanted radius per tier.
$tiers = @(Get-FarLadderTierTable)
Assert-Test ($tiers.Count -eq 5) "tier table has five tiers"
$expectedSpacing = @(4, 8, 16, 32, 64)
$expectedEdge = @(512, 1024, 2048, 4096, 8192)
$expectedOuter = @(1024, 2048, 4096, 8192, 16384)
$expectedWanted = @(1040, 2080, 4160, 8320, 16384)
for ($i = 0; $i -lt 5; $i++) {
    Assert-Test ([int64]($tiers[$i].Tier) -eq ($i + 1)) "tier $($i + 1) is numbered one-based"
    Assert-Test ([int64]($tiers[$i].Spacing) -eq $expectedSpacing[$i]) "tier $($i + 1) spacing is $($expectedSpacing[$i]) m"
    Assert-Test ([int64]($tiers[$i].TileEdge) -eq $expectedEdge[$i]) "tier $($i + 1) tile edge is $($expectedEdge[$i]) m"
    Assert-Test ([int64]($tiers[$i].Outer) -eq $expectedOuter[$i]) "tier $($i + 1) outer radius is $($expectedOuter[$i]) m"
    Assert-Test ([int64]($tiers[$i].Wanted) -eq $expectedWanted[$i]) "tier $($i + 1) wanted radius is $($expectedWanted[$i]) m"
}

# Origin: exactly 24 tier-1 tiles, including the four boundary-adjacent ones.
$origin = @(Get-FarLadderWantedTiles -X 0 -Z 0)
$originTier1 = @($origin | Where-Object { $_.StartsWith("1,") })
Assert-Test ($originTier1.Count -eq 24) "origin has exactly 24 tier-1 tiles (found $($originTier1.Count))"
foreach ($expectedTile in @("1,-3,-1", "1,-3,0", "1,-1,-3", "1,0,-3")) {
    Assert-Test ($originTier1 -contains $expectedTile) "origin tier-1 set contains $expectedTile"
}

# Per-tier boundary: a camera exactly W metres west of tile (t,0,0) includes it, and
# a camera a quarter metre further west excludes it.
foreach ($row in $tiers) {
    $tier = [int]$row.Tier
    $wanted = [double]$row.Wanted
    $half = [double]$row.TileEdge / 2
    $inside = @(Get-FarLadderWantedTiles -X (-$wanted) -Z $half)
    Assert-Test ($inside -contains "$tier,0,0") "tier $tier tile 0,0 is wanted at exactly W"
    $outside = @(Get-FarLadderWantedTiles -X (-$wanted - 0.25) -Z $half)
    Assert-Test (-not ($outside -contains "$tier,0,0")) "tier $tier tile 0,0 is not wanted at W + 0.25"
}

# Out-of-domain cameras are refused by both independent oracles.
Assert-Rejected { Get-FarLadderWantedTiles -X ([double]::NaN) -Z 0 } "camera out of domain" "NaN camera X"
Assert-Rejected { Get-FarLadderWantedTiles -X 8388609 -Z 0 } "camera out of domain" "camera X beyond 2^23"
Assert-Rejected { Get-FarLadderWantedTiles -X 0 -Z -8388609 } "camera out of domain" "camera Z below -2^23"
Assert-Rejected { Get-FarLegacyWantedCount -X ([double]::NaN) -Z 0 } "camera out of domain" "legacy NaN camera"

# Positive controls: a legacy artifact and a ladder artifact both pass.
$legacyArtifact = ConvertTo-OracleRoundTrip (New-OracleArtifact -Mode "legacy")
$legacyResult = Assert-FarLadderOracleArtifact -Artifact $legacyArtifact -Name "legacy oracle"
Assert-Test ($legacyResult.LadderChecked -eq $false) "legacy artifact reports LadderChecked false"
Assert-Test ($legacyResult.LegacyChecked -eq $true) "legacy artifact reports LegacyChecked true"
Assert-Test ($legacyResult.Mode -eq "legacy") "legacy artifact reports mode legacy"

$ladderArtifact = ConvertTo-OracleRoundTrip (New-OracleArtifact -Mode "ladder")
$ladderResult = Assert-FarLadderOracleArtifact -Artifact $ladderArtifact -Name "ladder oracle"
Assert-Test ($ladderResult.LadderChecked -eq $true) "ladder artifact reports LadderChecked true"
Assert-Test ($ladderResult.MissingExpected -eq 0) "ladder artifact has no missing expected tiles"

# Negative controls on otherwise valid ladder artifacts.
$omitted = New-OracleArtifact -Mode "ladder"
$omitted.ladder.expected_tiles = Remove-OracleTile $omitted.ladder.expected_tiles { param($t) [int]$t[0] -eq 1 -and [int]$t[1] -eq 0 -and [int]$t[2] -eq -1 }
Assert-Rejected { Assert-FarLadderOracleArtifact -Artifact (ConvertTo-OracleRoundTrip $omitted) -Name "omitted tile" } "expected_tiles" "omitted expected tile"

$missingTier = New-OracleArtifact -Mode "legacy"
$missingTier.ladder.expected_tiles = Remove-OracleTile $missingTier.ladder.expected_tiles { param($t) [int]$t[0] -eq 5 }
$missingTier.ladder.expected_by_tier[4] = 0
Assert-Rejected { Assert-FarLadderOracleArtifact -Artifact (ConvertTo-OracleRoundTrip $missingTier) -Name "missing tier" } "expected_tiles" "dropped tier-5 tiles"

$stale = New-OracleArtifact -Mode "ladder"
$stale.ladder.stale_tiles = 1
Assert-Rejected { Assert-FarLadderOracleArtifact -Artifact (ConvertTo-OracleRoundTrip $stale) -Name "stale generation" } "stale" "ladder mode with a stale tile"

$notResident = New-OracleArtifact -Mode "ladder"
$notResident.ladder.resident_tiles = Remove-OracleTile $notResident.ladder.resident_tiles { param($t) [int]$t[0] -eq 2 -and [int]$t[1] -eq 0 -and [int]$t[2] -eq 0 }
Assert-Rejected { Assert-FarLadderOracleArtifact -Artifact (ConvertTo-OracleRoundTrip $notResident) -Name "absent resident tile" } "resident_tiles" "ladder mode with one expected tile absent from resident_tiles"

# Malformed artifacts.
$wrongSchema = New-OracleArtifact -Mode "legacy"
$wrongSchema.schema = "luminumbra.farlod_horizon.v1"
Assert-Rejected { Assert-FarLadderOracleArtifact -Artifact (ConvertTo-OracleRoundTrip $wrongSchema) -Name "wrong schema" } "schema" "wrong schema string"

$noCamera = New-OracleArtifact -Mode "legacy"
$noCamera.Remove("camera")
Assert-Rejected { Assert-FarLadderOracleArtifact -Artifact (ConvertTo-OracleRoundTrip $noCamera) -Name "absent camera" } "camera" "absent camera key"

$bogusMode = New-OracleArtifact -Mode "legacy"
$bogusMode.mode = "bogus"
Assert-Rejected { Assert-FarLadderOracleArtifact -Artifact (ConvertTo-OracleRoundTrip $bogusMode) -Name "bogus mode" } "mode" "mode 'bogus'"

$shortTile = New-OracleArtifact -Mode "legacy"
$shortTile.ladder.expected_tiles[0] = [object[]]@(1, 0)
Assert-Rejected { Assert-FarLadderOracleArtifact -Artifact (ConvertTo-OracleRoundTrip $shortTile) -Name "two-element tile" } "expected_tiles" "two-element expected_tiles entry"

$offByOne = New-OracleArtifact -Mode "legacy"
$offByOne.legacy.regions_wanted = $offByOne.legacy.regions_wanted + 1
Assert-Rejected { Assert-FarLadderOracleArtifact -Artifact (ConvertTo-OracleRoundTrip $offByOne) -Name "regions_wanted off by one" } "regions_wanted" "legacy.regions_wanted off by one"

$missingOne = New-OracleArtifact -Mode "legacy"
$missingOne.legacy.regions_missing = 1
Assert-Rejected { Assert-FarLadderOracleArtifact -Artifact (ConvertTo-OracleRoundTrip $missingOne) -Name "regions_missing 1" } "regions_missing" "legacy.regions_missing 1"

$duplicate = New-OracleArtifact -Mode "legacy"
$tileList = New-Object System.Collections.ArrayList
$tileList.AddRange([object[]]$duplicate.ladder.expected_tiles)
[void]$tileList.Add($tileList[0])
$duplicate.ladder.expected_tiles = $tileList.ToArray()
Assert-Rejected { Assert-FarLadderOracleArtifact -Artifact (ConvertTo-OracleRoundTrip $duplicate) -Name "duplicate tile" } "duplicate" "duplicate expected tile"

Write-Host "far-ladder-oracle: $checks checks passed"
