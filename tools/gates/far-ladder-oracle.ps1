# Independent far-ladder oracle. Dot-sourcing this file defines functions only:
# it runs no gate, reads no engine source and writes no files. The formulas come
# from the volumetric ladder contract, not from the engine implementation:
#   tier t (1..5): spacing 4 * 2^(t-1) m, brick edge 4 * spacing, tile edge
#   128 * spacing, outer radius 256 * spacing, wanted radius outer + brick for
#   tiers 1..4 and outer for tier 5.
# The legacy F1/F2 scheduler uses 512 m regions and a 3000 m wanted range.

function Test-FarOracleFinite {
    param([double]$Value)
    return -not ([double]::IsNaN($Value) -or [double]::IsInfinity($Value))
}

function Test-FarLadderCameraInDomain {
    param([double]$X, [double]$Z)
    foreach ($value in @($X, $Z)) {
        if (-not (Test-FarOracleFinite $value)) { return $false }
        if ([math]::Abs($value) -gt 8388608.0) { return $false }
    }
    return $true
}

function Assert-FarLadderCameraDomain {
    param([double]$X, [double]$Z)
    if (-not (Test-FarLadderCameraInDomain -X $X -Z $Z)) {
        throw ("camera out of domain: x={0}, z={1}" -f $X, $Z)
    }
}

function Get-FarLadderTierTable {
    $rows = New-Object System.Collections.ArrayList
    for ($tier = 1; $tier -le 5; $tier++) {
        $spacing = [int64]4 * ([int64]1 -shl ($tier - 1))
        $brick = [int64]4 * $spacing
        $tileEdge = [int64]128 * $spacing
        $outer = [int64]256 * $spacing
        $wanted = $outer
        if ($tier -le 4) { $wanted = $outer + $brick }
        [void]$rows.Add([pscustomobject]@{
            Tier = $tier
            Spacing = $spacing
            Brick = $brick
            TileEdge = $tileEdge
            Outer = $outer
            Wanted = $wanted
        })
    }
    return @($rows.ToArray())
}

# The independent #199 arithmetic is shared with file-only stdlib tests. It
# lives in one Python helper; the PowerShell facade never reads engine sources.
$script:FarOracleHelper = Join-Path $PSScriptRoot "far_ladder_oracle.py"
function Invoke-FarOraclePython {
    param([string[]]$Arguments)
    $python = $null
    foreach ($candidate in @("python3", "python", "py")) {
        if (Get-Command $candidate -ErrorAction SilentlyContinue) { $python = $candidate; break }
    }
    if (-not $python) { throw "far oracle requires Python 3 (stdlib only)" }
    $output = & $python -B $script:FarOracleHelper @Arguments
    if ($LASTEXITCODE -ne 0) { throw "far oracle refused artifact/arithmetic (exit $LASTEXITCODE)" }
    return ($output | ConvertFrom-Json)
}
function Get-FarLadderWantedTiles {
    param([double]$X, [double]$Z)
    Assert-FarLadderCameraDomain -X $X -Z $Z
    $culture = [System.Globalization.CultureInfo]::InvariantCulture
    return @(Invoke-FarOraclePython -Arguments @("wanted", "--", $X.ToString("R", $culture), $Z.ToString("R", $culture)))
}
function Get-FarLegacyWantedCount {
    param([double]$X, [double]$Z)
    Assert-FarLadderCameraDomain -X $X -Z $Z
    $culture = [System.Globalization.CultureInfo]::InvariantCulture
    return [int64](Invoke-FarOraclePython -Arguments @("legacy", "--", $X.ToString("R", $culture), $Z.ToString("R", $culture)))
}
function Assert-FarHorizonV2Artifact {
    param([string]$Path, [ValidateSet("legacy", "ladder")][string]$Renderer = "ladder",
          [string[]]$Stations = @("eye_yaw_000", "eye_yaw_090", "eye_yaw_180", "eye_yaw_270", "elevated"))
    # Keep #199's aggregate assertions AND the station validator on this path.
    # Ladder artifacts intentionally carry per-station evidence only.
    if ($Renderer -ceq "legacy") {
        $artifact = Get-Content -LiteralPath $Path -Raw | ConvertFrom-Json
        Assert-FarLadderOracleArtifact -Artifact $artifact -Name "horizon v2 aggregate" | Out-Null
    }
    $arguments = @("validate", $Path, "--renderer", $Renderer)
    foreach ($station in $Stations) { $arguments += @("--station", $station) }
    return Invoke-FarOraclePython -Arguments $arguments
}

# Each helper either returns a scalar or throws; none returns a bare array.
function Assert-FarOracleObject {
    param([object]$Value, [string]$Name, [string]$Path, [string[]]$Fields)
    if (-not ($Value -is [pscustomobject])) { throw "${Name}: ${Path} must be a JSON object" }
    $names = @($Value.PSObject.Properties.Name)
    foreach ($field in $Fields) {
        if ($names -cnotcontains $field) { throw "${Name}: ${Path}.${field} is missing" }
    }
}

function Assert-FarOracleArray {
    param([object]$Value, [string]$Name, [string]$Path)
    if (-not ($Value -is [System.Array])) { throw "${Name}: ${Path} must be an array" }
}

function Get-FarOracleInteger {
    param([object]$Value, [string]$Name, [string]$Path)
    if (($Value -is [int64]) -or ($Value -is [int32])) { return [int64]$Value }
    throw "${Name}: ${Path} must be an integer"
}

function Get-FarOracleCoordinate {
    param([object]$Value, [string]$Name, [string]$Path)
    if ($null -eq $Value) { throw "${Name}: ${Path} is non-finite (null)" }
    if (($Value -isnot [double]) -and ($Value -isnot [decimal]) -and ($Value -isnot [int64]) -and ($Value -isnot [int32])) {
        throw "${Name}: ${Path} must be a number"
    }
    $number = [double]$Value
    if (-not (Test-FarOracleFinite $number)) { throw "${Name}: ${Path} is non-finite" }
    return $number
}

function ConvertTo-FarOracleTileKey {
    param([object]$Entry, [string]$Name, [string]$Path)
    if (($Entry -isnot [System.Array]) -or (@($Entry).Count -ne 3)) {
        throw "${Name}: ${Path} must be a 3-element integer array"
    }
    $items = @($Entry)
    $tier = Get-FarOracleInteger $items[0] $Name $Path
    $tx = Get-FarOracleInteger $items[1] $Name $Path
    $tz = Get-FarOracleInteger $items[2] $Name $Path
    return [string]::Format([System.Globalization.CultureInfo]::InvariantCulture,
        "{0},{1},{2}", $tier, $tx, $tz)
}

function Get-FarOracleTileKeys {
    param([object]$Entries, [string]$Name, [string]$Path)
    $keys = New-Object System.Collections.ArrayList
    $seen = @{}
    $index = 0
    foreach ($entry in @($Entries)) {
        $key = ConvertTo-FarOracleTileKey $entry $Name "${Path}[${index}]"
        if ($seen.ContainsKey($key)) { throw "${Name}: ${Path} has duplicate tile '${key}'" }
        $seen[$key] = $true
        [void]$keys.Add($key)
        $index++
    }
    return @($keys.ToArray())
}

function Assert-FarLadderOracleArtifact {
    param([object]$Artifact, [string]$Name)

    Assert-FarOracleObject $Artifact $Name "artifact" @("schema", "camera", "mode", "legacy", "ladder")
    $schema = $Artifact.schema
    if (($schema -isnot [string]) -or ($schema -cne "luminumbra.farlod_horizon.v2")) {
        throw "${Name}: schema '${schema}' is not luminumbra.farlod_horizon.v2"
    }

    Assert-FarOracleObject $Artifact.camera $Name "camera" @("world_x", "world_z")
    $cameraX = Get-FarOracleCoordinate $Artifact.camera.world_x $Name "camera.world_x"
    $cameraZ = Get-FarOracleCoordinate $Artifact.camera.world_z $Name "camera.world_z"
    if (-not (Test-FarLadderCameraInDomain -X $cameraX -Z $cameraZ)) {
        throw "${Name}: camera out of domain (x=${cameraX}, z=${cameraZ})"
    }

    $mode = $Artifact.mode
    if (($mode -isnot [string]) -or (@("legacy", "ladder") -cnotcontains $mode)) {
        throw "${Name}: mode '${mode}' is not legacy or ladder"
    }

    Assert-FarOracleObject $Artifact.legacy $Name "legacy" @(
        "region_size_m", "f1_outer_range_m", "f2_outer_range_m",
        "regions_wanted", "regions_resident", "regions_missing")
    $legacy = $Artifact.legacy
    [void](Get-FarOracleInteger $legacy.region_size_m $Name "legacy.region_size_m")
    [void](Get-FarOracleInteger $legacy.f1_outer_range_m $Name "legacy.f1_outer_range_m")
    [void](Get-FarOracleInteger $legacy.f2_outer_range_m $Name "legacy.f2_outer_range_m")
    [void](Get-FarOracleInteger $legacy.regions_resident $Name "legacy.regions_resident")
    $legacyWanted = Get-FarOracleInteger $legacy.regions_wanted $Name "legacy.regions_wanted"
    $legacyMissing = Get-FarOracleInteger $legacy.regions_missing $Name "legacy.regions_missing"

    Assert-FarOracleObject $Artifact.ladder $Name "ladder" @(
        "wanted_error", "expected_by_tier", "expected_tiles", "resident_tiles", "stale_tiles")
    $ladder = $Artifact.ladder
    if ($ladder.wanted_error -isnot [string]) { throw "${Name}: ladder.wanted_error must be a string" }
    if ($ladder.wanted_error -cne "None") {
        throw "${Name}: ladder.wanted_error is '$($ladder.wanted_error)', expected None"
    }
    Assert-FarOracleArray $ladder.expected_by_tier $Name "ladder.expected_by_tier"
    $byTier = @($ladder.expected_by_tier)
    if ($byTier.Count -ne 5) {
        throw "${Name}: ladder.expected_by_tier has $($byTier.Count) entries, expected 5"
    }
    $byTierValues = New-Object System.Collections.ArrayList
    for ($i = 0; $i -lt 5; $i++) {
        [void]$byTierValues.Add((Get-FarOracleInteger $byTier[$i] $Name "ladder.expected_by_tier[${i}]"))
    }
    Assert-FarOracleArray $ladder.expected_tiles $Name "ladder.expected_tiles"
    Assert-FarOracleArray $ladder.resident_tiles $Name "ladder.resident_tiles"
    $staleTiles = Get-FarOracleInteger $ladder.stale_tiles $Name "ladder.stale_tiles"

    $tileKeys = @(Get-FarOracleTileKeys -Entries $ladder.expected_tiles -Name $Name -Path "ladder.expected_tiles")
    $expectedKeys = @(Get-FarLadderWantedTiles -X $cameraX -Z $cameraZ)
    $actualSet = @{}
    foreach ($key in $tileKeys) { $actualSet[$key] = $true }
    $expectedSet = @{}
    foreach ($key in $expectedKeys) { $expectedSet[$key] = $true }
    $missingTiles = @($expectedKeys | Where-Object { -not $actualSet.ContainsKey($_) })
    $extraTiles = @($tileKeys | Where-Object { -not $expectedSet.ContainsKey($_) })
    if (($missingTiles.Count -gt 0) -or ($extraTiles.Count -gt 0)) {
        $firstMissing = "none"
        if ($missingTiles.Count -gt 0) { $firstMissing = $missingTiles[0] }
        $firstExtra = "none"
        if ($extraTiles.Count -gt 0) { $firstExtra = $extraTiles[0] }
        throw ("${Name}: ladder.expected_tiles differs from the independent oracle: " +
            "$($missingTiles.Count) missing (first $firstMissing), " +
            "$($extraTiles.Count) extra (first $firstExtra)")
    }

    $counts = @(0, 0, 0, 0, 0)
    foreach ($key in $tileKeys) {
        $tierIndex = [int]($key.Split(",")[0]) - 1
        $counts[$tierIndex] = $counts[$tierIndex] + 1
    }
    for ($i = 0; $i -lt 5; $i++) {
        if ($byTierValues[$i] -ne $counts[$i]) {
            throw "${Name}: ladder.expected_by_tier[${i}] is $($byTierValues[$i]) but expected_tiles has $($counts[$i]) tier-$($i + 1) tiles"
        }
    }

    $legacyExpected = Get-FarLegacyWantedCount -X $cameraX -Z $cameraZ
    if ($legacyWanted -ne $legacyExpected) {
        throw "${Name}: legacy.regions_wanted is $legacyWanted, the independent legacy oracle expects $legacyExpected"
    }
    if ($legacyMissing -ne 0) {
        throw "${Name}: legacy.regions_missing is $legacyMissing, expected 0"
    }

    $ladderChecked = ($mode -ceq "ladder")
    $missingExpected = $null
    if ($ladderChecked) {
        $residentKeys = @(Get-FarOracleTileKeys -Entries $ladder.resident_tiles -Name $Name -Path "ladder.resident_tiles")
        $residentSet = @{}
        foreach ($key in $residentKeys) { $residentSet[$key] = $true }
        $notResident = @($expectedKeys | Where-Object { -not $residentSet.ContainsKey($_) })
        if ($notResident.Count -gt 0) {
            throw "${Name}: ladder mode: missing_expected $($notResident.Count) expected tile(s) absent from ladder.resident_tiles (first missing $($notResident[0]))"
        }
        $extraResident = @($residentKeys | Where-Object { -not $expectedSet.ContainsKey($_) })
        if ($extraResident.Count -gt 0) {
            throw "${Name}: ladder mode: $($extraResident.Count) extra tile(s) in ladder.resident_tiles outside the independent wanted set (first extra $($extraResident[0]))"
        }
        if ($staleTiles -ne 0) {
            throw "${Name}: ladder mode has $staleTiles stale tile(s), expected 0"
        }
        $missingExpected = $notResident.Count
    }

    # Validate range extras before generations so stale reports cannot hide extra keys.
    if ($staleTiles -ne 0) { throw "${Name}: aggregate has $staleTiles stale tile(s), expected 0" }

    return [pscustomobject]@{
        Mode = $mode
        LadderChecked = $ladderChecked
        LegacyChecked = $true
        ExpectedByTier = [int64[]]$byTierValues.ToArray()
        MissingExpected = $missingExpected
    }
}

# V1 preserves its historical selected-station aggregate. Qualification must
# instead check every captured station, using that station's own camera XZ.
# This checks serialization and coverage, not readiness within 90 seconds.
function Assert-FarLodHorizonStationCoverage {
    param([object]$Analysis, [string]$Name)

    Assert-FarOracleObject $Analysis $Name "analysis" @("schema", "stations", "aggregates")
    if ($Analysis.schema -cne "luminumbra.farlod_horizon.v1") {
        throw "${Name}: analysis.schema is not luminumbra.farlod_horizon.v1"
    }
    Assert-FarOracleArray $Analysis.stations $Name "stations"
    Assert-FarOracleObject $Analysis.aggregates $Name "aggregates" @("captured_stations", "expected_stations")
    $captured = Get-FarOracleInteger $Analysis.aggregates.captured_stations $Name "aggregates.captured_stations"
    $expected = Get-FarOracleInteger $Analysis.aggregates.expected_stations $Name "aggregates.expected_stations"
    $stations = @($Analysis.stations)
    if (($stations.Count -eq 0) -or ($stations.Count -ne $captured) -or ($captured -ne $expected)) {
        throw "${Name}: stations has $($stations.Count) rows, captured_stations $captured, expected_stations $expected; expected a complete nonempty capture"
    }

    for ($i = 0; $i -lt $stations.Count; $i++) {
        $station = $stations[$i]
        $path = "stations[${i}]"
        Assert-FarOracleObject $station $Name $path @("name", "camera", "farlod")
        Assert-FarOracleObject $station.camera $Name "${path}.camera" @("world_x", "world_z")
        $cameraX = Get-FarOracleCoordinate $station.camera.world_x $Name "${path}.camera.world_x"
        $cameraZ = Get-FarOracleCoordinate $station.camera.world_z $Name "${path}.camera.world_z"
        $wanted = Get-FarLegacyWantedCount -X $cameraX -Z $cameraZ
        Assert-FarOracleObject $station.farlod $Name "${path}.farlod" @("regions_wanted", "regions_missing")
        $reportedWanted = Get-FarOracleInteger $station.farlod.regions_wanted $Name "${path}.farlod.regions_wanted"
        $missing = Get-FarOracleInteger $station.farlod.regions_missing $Name "${path}.farlod.regions_missing"
        if ($reportedWanted -ne $wanted) {
            throw "${Name}: ${path} '$($station.name)' regions_wanted is $reportedWanted, the independent legacy oracle expects $wanted"
        }
        if ($missing -ne 0) {
            throw "${Name}: ${path} '$($station.name)' regions_missing is $missing, expected 0"
        }
    }
}
