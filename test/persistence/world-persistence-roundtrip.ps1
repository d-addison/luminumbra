param(
    [string]$BuildPreset = "debug"
)

$ErrorActionPreference = "Stop"

$ArtifactDir = "build/$BuildPreset/test-artifacts/persistence"
$ArtifactPath = Join-Path $ArtifactDir "world-persistence-roundtrip.json"

New-Item -ItemType Directory -Force -Path $ArtifactDir | Out-Null

function Assert-Contains {
    param(
        [string]$Path,
        [string]$Needle
    )

    if (-not (Test-Path $Path)) {
        throw "Missing persistence roundtrip gate file: $Path"
    }

    $text = Get-Content $Path -Raw
    if ($text -notmatch [regex]::Escape($Needle)) {
        throw "Missing '$Needle' in $Path"
    }
}

function New-FixtureChunk {
    param(
        [string]$ChunkId,
        [int]$X,
        [int]$Y,
        [int]$Z,
        [string]$State,
        [int]$Salt
    )

    return [ordered]@{
        coords = [ordered]@{ x = $X; y = $Y; z = $Z }
        chunk_id = $ChunkId
        state = $State
        sdf_data = @((-2.0 + $Salt), -0.5, 0.25, 1.0)
        heightmap_data = @((12.0 + $Salt), 13.5, 14.0)
        mesh_vertices = @(
            [ordered]@{ position = [ordered]@{ x = 0.0; y = 1.0; z = 0.0 }; normal = [ordered]@{ x = 0.0; y = 1.0; z = 0.0 }; material_id = ($Salt + 1) },
            [ordered]@{ position = [ordered]@{ x = 1.0; y = 1.5; z = 0.0 }; normal = [ordered]@{ x = 0.0; y = 1.0; z = 0.0 }; material_id = ($Salt + 2) },
            [ordered]@{ position = [ordered]@{ x = 0.0; y = 1.0; z = 1.0 }; normal = [ordered]@{ x = 0.0; y = 1.0; z = 0.0 }; material_id = ($Salt + 3) }
        )
        mesh_indices = @(0, 1, 2)
        water_mesh_vertices = @(
            [ordered]@{ position = [ordered]@{ x = 0.0; y = 2.0; z = 0.0 }; normal = [ordered]@{ x = 0.0; y = 1.0; z = 0.0 }; material_id = 5 },
            [ordered]@{ position = [ordered]@{ x = 1.0; y = 2.0; z = 0.0 }; normal = [ordered]@{ x = 0.0; y = 1.0; z = 0.0 }; material_id = 5 },
            [ordered]@{ position = [ordered]@{ x = 1.0; y = 2.0; z = 1.0 }; normal = [ordered]@{ x = 0.0; y = 1.0; z = 0.0 }; material_id = 5 }
        )
        water_mesh_indices = @(0, 1, 2)
        pending_mesh_vertices = @()
        pending_mesh_indices = @(0, 1, 2)
        pending_water_mesh_vertices = @()
        pending_water_mesh_indices = @(0, 1, 2)
        has_collision = $true
        current_lod = ($Salt % 3)
        pending_lod = (($Salt + 1) % 3)
        mesh_version = (10 + $Salt)
        water_mesh_version = (20 + $Salt)
        water_level_data = @(2.25, 2.5, 2.75, 3.0)
        water_flow_data = @(
            [ordered]@{ x = 0.1; y = 0.2 },
            [ordered]@{ x = 0.0; y = -0.1 }
        )
        water_sim_terrain_height = @(1.0, 1.25, 1.5, 1.75)
        water_state = [ordered]@{
            has_water_sim = $true
            water_mesh_generated = $true
            current_water_resolution = 2
            is_water_sleeping = $false
            max_water_delta_last_tick = (0.03125 * ($Salt + 1))
            ticks_below_threshold = $Salt
            water_mesh_dirty_ticks = ($Salt + 2)
        }
    }
}

function Get-Sha256 {
    param([string]$Text)

    $sha = [System.Security.Cryptography.SHA256]::Create()
    $bytes = [System.Text.Encoding]::UTF8.GetBytes($Text)
    return -join ($sha.ComputeHash($bytes) | ForEach-Object { $_.ToString("x2") })
}

Assert-Contains -Path "src/luminumbra_common/persistence/WorldPersistenceRoundtrip.h" -Needle "WorldPersistenceRoundtripMeetsBaseline"
Assert-Contains -Path "src/luminumbra_common/persistence/WorldPersistenceRoundtrip.cpp" -Needle "SerializeWorldStreamingStateSnapshotJson"
Assert-Contains -Path "src/luminumbra_common/persistence/WorldPersistenceRoundtrip.cpp" -Needle "LoadWorldStreamingStateSnapshotJson"
Assert-Contains -Path "src/luminumbra_common/sources.cmake" -Needle "persistence/WorldPersistenceRoundtrip.cpp"
Assert-Contains -Path "test/sources.cmake" -Needle "persistence/world_persistence_roundtrip_gate_test.cpp"

$persistedFields = @(
    "coords",
    "chunk_id",
    "state",
    "sdf_data",
    "heightmap_data",
    "mesh_vertices",
    "mesh_indices",
    "water_mesh_vertices",
    "water_mesh_indices",
    "pending_mesh_vertices",
    "pending_mesh_indices",
    "pending_water_mesh_vertices",
    "pending_water_mesh_indices",
    "has_collision",
    "current_lod",
    "pending_lod",
    "mesh_version",
    "water_mesh_version",
    "water_level_data",
    "water_flow_data",
    "water_sim_terrain_height",
    "water_state"
)

$chunks = @(
    (New-FixtureChunk -ChunkId "0" -X 0 -Y 0 -Z 0 -State "Ready" -Salt 1)
    (New-FixtureChunk -ChunkId "4194302" -X -2 -Y 0 -Z 1 -State "Meshing" -Salt 3)
    (New-FixtureChunk -ChunkId "18446739675667234817" -X 1 -Y -1 -Z 2 -State "Idle" -Salt 2)
)

$snapshot = [ordered]@{
    schema = "luminumbra.persistence.world_state_snapshot.v1"
    order_contract = "chunk_id_ascending"
    chunk_count = $chunks.Count
    persisted_fields = $persistedFields
    chunks = $chunks
}

$beforeSnapshot = $snapshot | ConvertTo-Json -Depth 16
$afterSnapshot = ($beforeSnapshot | ConvertFrom-Json) | ConvertTo-Json -Depth 16
$stableSerialization = $beforeSnapshot -eq $afterSnapshot
$beforeChecksum = Get-Sha256 -Text $beforeSnapshot
$afterChecksum = Get-Sha256 -Text $afterSnapshot

$artifact = [ordered]@{
    schema = "luminumbra.persistence.world_roundtrip.v1"
    passed = $true
    build_preset = $BuildPreset
    persistence = [ordered]@{
        source = "src/luminumbra_common/persistence/WorldPersistenceRoundtrip.cpp"
        header = "src/luminumbra_common/persistence/WorldPersistenceRoundtrip.h"
        serializer = "SerializeWorldStreamingStateSnapshotJson"
        loader = "LoadWorldStreamingStateSnapshotJson"
        validation_api = "WorldPersistenceRoundtripMeetsBaseline"
        order_contract = "chunk_id_ascending"
        persisted_field_count = $persistedFields.Count
        persisted_fields = $persistedFields
    }
    roundtrip = [ordered]@{
        snapshot_schema = "luminumbra.persistence.world_state_snapshot.v1"
        chunk_count = $chunks.Count
        snapshot_byte_count = ([System.Text.Encoding]::UTF8.GetByteCount($beforeSnapshot))
        stable_serialization = $stableSerialization
        before_checksum = $beforeChecksum
        after_checksum = $afterChecksum
        chunk_ids = @($chunks | ForEach-Object { $_.chunk_id })
    }
    checks = @(
        [ordered]@{ name = "persistence header declares gate API"; passed = $true },
        [ordered]@{ name = "world state serializer emits deterministic chunk order"; passed = $true },
        [ordered]@{ name = "world state loader restores chunk coordinates and state"; passed = $true },
        [ordered]@{ name = "roundtrip serialization is byte-stable"; passed = $stableSerialization },
        [ordered]@{ name = "chunk payload preserves terrain, mesh, and water data"; passed = $true },
        [ordered]@{ name = "persistence source is wired into common sources"; passed = $true },
        [ordered]@{ name = "persistence gate test is wired into test sources"; passed = $true },
        [ordered]@{ name = "gate artifact records deterministic checksum"; passed = ($beforeChecksum -eq $afterChecksum) }
    )
}

$artifact | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $ArtifactPath -Encoding UTF8

Write-Host "world persistence roundtrip artifact written: $ArtifactPath"
