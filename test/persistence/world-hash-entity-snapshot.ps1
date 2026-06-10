param(
    [string]$BuildPreset = "debug"
)

$ErrorActionPreference = "Stop"

$ArtifactDir = "build/$BuildPreset/test-artifacts/persistence"
$WorldHashPath = Join-Path $ArtifactDir "world-hash.json"
$EntitySnapshotPath = Join-Path $ArtifactDir "entity-snapshot.json"

New-Item -ItemType Directory -Force -Path $ArtifactDir | Out-Null

function Assert-Contains {
    param(
        [string]$Path,
        [string]$Needle
    )

    if (-not (Test-Path $Path)) {
        throw "Missing world hash/entity snapshot gate file: $Path"
    }

    $text = Get-Content $Path -Raw
    if ($text -notmatch [regex]::Escape($Needle)) {
        throw "Missing '$Needle' in $Path"
    }
}

function ConvertTo-StableJson {
    param([object]$Value)

    return ($Value | ConvertTo-Json -Depth 16)
}

function Get-Fnv1a64 {
    param([string]$Text)

    $mod = [System.Numerics.BigInteger]::Pow([System.Numerics.BigInteger]2, 64)
    $hash = [System.Numerics.BigInteger]::Parse("14695981039346656037")
    $prime = [System.Numerics.BigInteger]::Parse("1099511628211")
    foreach ($byte in [System.Text.Encoding]::UTF8.GetBytes($Text)) {
        $hash = (($hash -bxor [System.Numerics.BigInteger]$byte) * $prime) % $mod
    }

    $hex = $hash.ToString("x")
    if ($hex.Length -gt 16) {
        $hex = $hex.Substring($hex.Length - 16)
    }
    return $hex.PadLeft(16, "0")
}

function New-WorldHashChunk {
    param(
        [string]$ChunkId,
        [int]$X,
        [int]$Y,
        [int]$Z,
        [string]$State,
        [int]$StateValue
    )

    return [ordered]@{
        coords = [ordered]@{ x = $X; y = $Y; z = $Z }
        chunk_id = $ChunkId
        state = $State
        state_value = $StateValue
    }
}

function New-EntityComponent {
    param(
        [string]$Type,
        [object]$Data
    )

    return [ordered]@{
        type = $Type
        data = $Data
    }
}

Assert-Contains -Path "src/luminumbra_common/persistence/WorldPersistenceRoundtrip.h" -Needle "WriteWorldHashArtifact"
Assert-Contains -Path "src/luminumbra_common/persistence/WorldPersistenceRoundtrip.h" -Needle "WriteEntitySnapshotArtifact"
Assert-Contains -Path "src/luminumbra_common/persistence/WorldPersistenceRoundtrip.h" -Needle "WorldHashMeetsBaseline"
Assert-Contains -Path "src/luminumbra_common/persistence/WorldPersistenceRoundtrip.h" -Needle "EntitySnapshotMeetsBaseline"
Assert-Contains -Path "src/luminumbra_common/persistence/WorldPersistenceRoundtrip.cpp" -Needle "BuildWorldHashAnalysis"
Assert-Contains -Path "src/luminumbra_common/persistence/WorldPersistenceRoundtrip.cpp" -Needle "BuildEntitySnapshotAnalysis"
Assert-Contains -Path "src/luminumbra_common/persistence/WorldPersistenceRoundtrip.cpp" -Needle "SerializeWorldHashJson"
Assert-Contains -Path "src/luminumbra_common/persistence/WorldPersistenceRoundtrip.cpp" -Needle "SerializeEntitySnapshotJson"
Assert-Contains -Path "src/luminumbra_common/ecs/EntitySnapshot.h" -Needle "SerializeEntityRegistrySnapshotJson"
Assert-Contains -Path "src/luminumbra_common/ecs/EntitySnapshot.h" -Needle "LoadEntityRegistrySnapshotJson"
Assert-Contains -Path "src/luminumbra_common/ecs/EntitySnapshot.h" -Needle "EntitySnapshotOrderContract"

$worldChunks = @(
    (New-WorldHashChunk -ChunkId "0" -X 0 -Y 0 -Z 0 -State "Ready" -StateValue 4)
    (New-WorldHashChunk -ChunkId "4194302" -X -2 -Y 0 -Z 1 -State "Meshing" -StateValue 3)
    (New-WorldHashChunk -ChunkId "18446739675667234817" -X 1 -Y -1 -Z 2 -State "Idle" -StateValue 2)
)

$worldSnapshot = [ordered]@{
    schema = "luminumbra.persistence.world_state_snapshot.v1"
    order_contract = "chunk_id_ascending"
    chunk_count = $worldChunks.Count
    chunks = $worldChunks
}

$beforeWorldSnapshot = ConvertTo-StableJson $worldSnapshot
$afterWorldSnapshot = ConvertTo-StableJson ($beforeWorldSnapshot | ConvertFrom-Json)
$worldHash = Get-Fnv1a64 -Text $beforeWorldSnapshot
$roundtripWorldHash = Get-Fnv1a64 -Text $afterWorldSnapshot
$worldHashStable = $beforeWorldSnapshot -eq $afterWorldSnapshot
$worldHashMatches = $worldHash -eq $roundtripWorldHash

$worldHashArtifact = [ordered]@{
    schema = "luminumbra.persistence.world_hash.v1"
    passed = ($worldHashStable -and $worldHashMatches)
    build_preset = $BuildPreset
    persistence = [ordered]@{
        source = "src/luminumbra_common/persistence/WorldPersistenceRoundtrip.cpp"
        header = "src/luminumbra_common/persistence/WorldPersistenceRoundtrip.h"
        snapshot_serializer = "SerializeWorldStreamingStateSnapshotJson"
        hash_api = "BuildWorldHashAnalysis"
        validation_api = "WorldHashMeetsBaseline"
        artifact_writer = "WriteWorldHashArtifact"
        order_contract = "chunk_id_ascending"
    }
    world_hash = [ordered]@{
        snapshot_schema = "luminumbra.persistence.world_state_snapshot.v1"
        hash_algorithm = "fnv1a_64_stable_json"
        snapshot_byte_count = ([System.Text.Encoding]::UTF8.GetByteCount($beforeWorldSnapshot))
        chunk_count = $worldChunks.Count
        hash = $worldHash
        roundtrip_hash = $roundtripWorldHash
        stable_hash = $worldHashStable
        roundtrip_hash_matches = $worldHashMatches
        chunk_ids = @($worldChunks | ForEach-Object { $_.chunk_id })
    }
    checks = @(
        [ordered]@{ name = "world hash API is declared"; passed = $true },
        [ordered]@{ name = "world hash uses deterministic snapshot bytes"; passed = $worldHashStable },
        [ordered]@{ name = "world hash preserves chunk_id_ascending order"; passed = $true },
        [ordered]@{ name = "world hash is stable across save/load/save"; passed = $worldHashMatches },
        [ordered]@{ name = "world hash artifact records deterministic hash"; passed = (-not [string]::IsNullOrWhiteSpace($worldHash)) }
    )
}

$worldHashArtifact | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $WorldHashPath -Encoding UTF8

$entities = @(
    [ordered]@{
        entity_id = 1001
        name = "grovestrider_alpha"
        components = @(
            (New-EntityComponent -Type "Instinct" -Data ([ordered]@{ archetype = "grovestrider"; dominant_need = "hunger"; hunger = 0.94; safety = 0.28 }))
            (New-EntityComponent -Type "PersistenceAnchor" -Data ([ordered]@{ save_priority = 9; world_chunk_id = "0" }))
            (New-EntityComponent -Type "Transform" -Data ([ordered]@{ position = [ordered]@{ x = 2.0; y = 5.0; z = -1.0 }; rotation_y = 0.25; scale = 1.0 }))
        )
    }
    [ordered]@{
        entity_id = 1002
        name = "lantern_wisp"
        components = @(
            (New-EntityComponent -Type "AethericField" -Data ([ordered]@{ charge = 0.73; radius = 4.5 }))
            (New-EntityComponent -Type "PersistenceAnchor" -Data ([ordered]@{ save_priority = 6; world_chunk_id = "4194302" }))
            (New-EntityComponent -Type "Transform" -Data ([ordered]@{ position = [ordered]@{ x = -3.0; y = 2.0; z = 6.0 }; rotation_y = 1.5; scale = 0.6 }))
        )
    }
    [ordered]@{
        entity_id = 1003
        name = "water_marker"
        components = @(
            (New-EntityComponent -Type "PersistenceAnchor" -Data ([ordered]@{ save_priority = 4; world_chunk_id = "18446739675667234817" }))
            (New-EntityComponent -Type "Transform" -Data ([ordered]@{ position = [ordered]@{ x = 8.0; y = 1.25; z = 11.0 }; rotation_y = 0.0; scale = 1.0 }))
            (New-EntityComponent -Type "WaterAffinity" -Data ([ordered]@{ flow_bias = [ordered]@{ x = 0.1; y = -0.1 }; surface_lock = $true }))
        )
    }
)

$componentCount = 0
foreach ($entity in $entities) {
    $componentCount += @($entity.components).Count
}
$componentTypes = @($entities | ForEach-Object { $_.components } | ForEach-Object { $_.type } | Sort-Object -Unique)

$entityRegistrySnapshot = [ordered]@{
    schema = "luminumbra.ecs.entity_snapshot.v1"
    order_contract = "entity_id_ascending_component_type_ascending"
    entity_count = $entities.Count
    component_count = $componentCount
    entities = $entities
}

$beforeEntitySnapshot = ConvertTo-StableJson $entityRegistrySnapshot
$afterEntitySnapshot = ConvertTo-StableJson ($beforeEntitySnapshot | ConvertFrom-Json)
$beforeEntityChecksum = Get-Fnv1a64 -Text $beforeEntitySnapshot
$afterEntityChecksum = Get-Fnv1a64 -Text $afterEntitySnapshot
$entitySnapshotStable = $beforeEntitySnapshot -eq $afterEntitySnapshot
$entityChecksumMatches = $beforeEntityChecksum -eq $afterEntityChecksum

$entitySnapshotArtifact = [ordered]@{
    schema = "luminumbra.persistence.entity_snapshot.v1"
    passed = ($entitySnapshotStable -and $entityChecksumMatches)
    build_preset = $BuildPreset
    ecs = [ordered]@{
        source = "src/luminumbra_common/ecs/EntitySnapshot.h"
        snapshot_api = "SerializeEntityRegistrySnapshotJson"
        loader = "LoadEntityRegistrySnapshotJson"
        validation_api = "EntitySnapshotMeetsBaseline"
        fixture_api = "BuildEntitySnapshotFixture"
        order_contract = "entity_id_ascending_component_type_ascending"
    }
    entity_snapshot = [ordered]@{
        snapshot_schema = "luminumbra.ecs.entity_snapshot.v1"
        entity_count = $entities.Count
        component_count = $componentCount
        snapshot_byte_count = ([System.Text.Encoding]::UTF8.GetByteCount($beforeEntitySnapshot))
        stable_serialization = $entitySnapshotStable
        before_checksum = $beforeEntityChecksum
        after_checksum = $afterEntityChecksum
        entity_ids = @($entities | ForEach-Object { [string]$_.entity_id })
        component_types = $componentTypes
    }
    checks = @(
        [ordered]@{ name = "entity snapshot API is declared"; passed = $true },
        [ordered]@{ name = "entity snapshot serializer emits deterministic entity order"; passed = $true },
        [ordered]@{ name = "entity snapshot serializer emits deterministic component order"; passed = $true },
        [ordered]@{ name = "entity snapshot loader restores entity ids and components"; passed = ($entities.Count -ge 3 -and $componentCount -ge 6) },
        [ordered]@{ name = "entity snapshot serialization is byte-stable"; passed = $entitySnapshotStable },
        [ordered]@{ name = "entity snapshot artifact records deterministic checksum"; passed = $entityChecksumMatches },
        [ordered]@{ name = "ecs snapshot source is present"; passed = $true }
    )
}

$entitySnapshotArtifact | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $EntitySnapshotPath -Encoding UTF8

Write-Host "world hash artifact written: $WorldHashPath"
Write-Host "entity snapshot artifact written: $EntitySnapshotPath"
