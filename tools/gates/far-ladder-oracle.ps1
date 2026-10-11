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
    return ,@($rows.ToArray())
}

# Distance along one axis from a coordinate to the nearest point of the cell
# [index*edge, (index+1)*edge); zero when the coordinate lies inside the cell.
function Get-FarOracleAxisGap {
    param([int64]$Index, [double]$Edge, [double]$Coord)
    $low = [double]$Index * $Edge
    $high = [double]($Index + 1) * $Edge
    return [math]::Max([math]::Max($low - $Coord, 0.0), $Coord - $high)
}

# Returns "tier,tx,tz" strings in canonical order: tier, then tz, then tx.
function Get-FarLadderWantedTiles {
    param([double]$X, [double]$Z)
    Assert-FarLadderCameraDomain -X $X -Z $Z
    $found = New-Object System.Collections.ArrayList
    foreach ($row in @(Get-FarLadderTierTable)) {
        $edge = [double]$row.TileEdge
        $radius = [double]$row.Wanted
        $radiusSquared = $radius * $radius
        $txMin = [int64]([math]::Floor(($X - $radius) / $edge)) - 1
        $txMax = [int64]([math]::Floor(($X + $radius) / $edge)) + 1
        $tzMin = [int64]([math]::Floor(($Z - $radius) / $edge)) - 1
        $tzMax = [int64]([math]::Floor(($Z + $radius) / $edge)) + 1
        for ($tx = $txMin; $tx -le $txMax; $tx++) {
            $gapX = Get-FarOracleAxisGap -Index $tx -Edge $edge -Coord $X
            for ($tz = $tzMin; $tz -le $tzMax; $tz++) {
                $gapZ = Get-FarOracleAxisGap -Index $tz -Edge $edge -Coord $Z
                if ($gapX * $gapX + $gapZ * $gapZ -le $radiusSquared) {
                    [void]$found.Add([pscustomobject]@{
                        Tier = [int64]$row.Tier
                        Tx = $tx
                        Tz = $tz
                    })
                }
            }
        }
    }
    $keys = @($found | Sort-Object -Property Tier, Tz, Tx)
    $keyStrings = New-Object System.Collections.ArrayList
    foreach ($key in $keys) {
        [void]$keyStrings.Add([string]::Format([System.Globalization.CultureInfo]::InvariantCulture,
            "{0},{1},{2}", $key.Tier, $key.Tx, $key.Tz))
    }
    return ,@($keyStrings.ToArray())
}

# Number of 512 m legacy regions (keyed floor(x/512)) whose nearest point lies
# within the 3000 m legacy wanted range of the camera.
function Get-FarLegacyWantedCount {
    param([double]$X, [double]$Z)
    Assert-FarLadderCameraDomain -X $X -Z $Z
    $regionSize = [double]512
    $range = [double]3000
    $rangeSquared = $range * $range
    $centerX = [int64]([math]::Floor($X / $regionSize))
    $centerZ = [int64]([math]::Floor($Z / $regionSize))
    $count = [int64]0
    for ($rx = $centerX - 8; $rx -le $centerX + 8; $rx++) {
        $gapX = Get-FarOracleAxisGap -Index $rx -Edge $regionSize -Coord $X
        for ($rz = $centerZ - 8; $rz -le $centerZ + 8; $rz++) {
            $gapZ = Get-FarOracleAxisGap -Index $rz -Edge $regionSize -Coord $Z
            if ($gapX * $gapX + $gapZ * $gapZ -le $rangeSquared) { $count = $count + 1 }
        }
    }
    return $count
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
    if (($Value -isnot [double]) -and ($Value -isnot [int64]) -and ($Value -isnot [int32])) {
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
    return ,@($keys.ToArray())
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
        if ($staleTiles -ne 0) {
            throw "${Name}: ladder mode has $staleTiles stale tile(s), expected 0"
        }
        $missingExpected = $notResident.Count
    }

    return [pscustomobject]@{
        Mode = $mode
        LadderChecked = $ladderChecked
        LegacyChecked = $true
        ExpectedByTier = [int64[]]$byTierValues.ToArray()
        MissingExpected = $missingExpected
    }
}
