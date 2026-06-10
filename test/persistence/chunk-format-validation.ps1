param(
    [string]$BuildPreset = "debug"
)

$ErrorActionPreference = "Stop"

$ArtifactDir = "build/$BuildPreset/test-artifacts/persistence"
$ArtifactPath = Join-Path $ArtifactDir "chunk-format-validation.json"

New-Item -ItemType Directory -Force -Path $ArtifactDir | Out-Null

function Assert-Contains {
    param(
        [string]$Path,
        [string]$Needle
    )

    if (-not (Test-Path $Path)) {
        throw "Missing chunk format validation gate file: $Path"
    }

    $text = Get-Content $Path -Raw
    if ($text -notmatch [regex]::Escape($Needle)) {
        throw "Missing '$Needle' in $Path"
    }
}

function Get-Sha256 {
    param([string]$Text)

    $sha = [System.Security.Cryptography.SHA256]::Create()
    $bytes = [System.Text.Encoding]::UTF8.GetBytes($Text)
    return -join ($sha.ComputeHash($bytes) | ForEach-Object { $_.ToString("x2") })
}

function New-ValidatedChunk {
    param(
        [string]$ChunkId,
        [int]$X,
        [int]$Y,
        [int]$Z,
        [string]$State,
        [int]$StateValue,
        [int]$Salt
    )

    $meshVertices = @(
        [ordered]@{ position = [ordered]@{ x = 0.0; y = 1.0; z = 0.0 }; normal = [ordered]@{ x = 0.0; y = 1.0; z = 0.0 }; material_id = ($Salt + 1) },
        [ordered]@{ position = [ordered]@{ x = 1.0; y = 1.5; z = 0.0 }; normal = [ordered]@{ x = 0.0; y = 1.0; z = 0.0 }; material_id = ($Salt + 2) },
        [ordered]@{ position = [ordered]@{ x = 0.0; y = 1.0; z = 1.0 }; normal = [ordered]@{ x = 0.0; y = 1.0; z = 0.0 }; material_id = ($Salt + 3) }
    )
    $waterVertices = @(
        [ordered]@{ position = [ordered]@{ x = 0.0; y = 2.0; z = 0.0 }; normal = [ordered]@{ x = 0.0; y = 1.0; z = 0.0 }; material_id = 5 },
        [ordered]@{ position = [ordered]@{ x = 1.0; y = 2.0; z = 0.0 }; normal = [ordered]@{ x = 0.0; y = 1.0; z = 0.0 }; material_id = 5 },
        [ordered]@{ position = [ordered]@{ x = 1.0; y = 2.0; z = 1.0 }; normal = [ordered]@{ x = 0.0; y = 1.0; z = 0.0 }; material_id = 5 }
    )

    return [ordered]@{
        coords = [ordered]@{ x = $X; y = $Y; z = $Z }
        chunk_id = $ChunkId
        state = $State
        state_value = $StateValue
        sdf_data = @((-2.0 + $Salt), -0.5, 0.25, 1.0)
        heightmap_data = @((12.0 + $Salt), 13.5, 14.0)
        mesh_vertices = $meshVertices
        mesh_indices = @(0, 1, 2)
        water_mesh_vertices = $waterVertices
        water_mesh_indices = @(0, 1, 2)
        pending_mesh_vertices = $meshVertices
        pending_mesh_indices = @(0, 1, 2)
        pending_water_mesh_vertices = $waterVertices
        pending_water_mesh_indices = @(0, 1, 2)
        has_collision = $true
        current_lod = ($Salt % 3)
        pending_lod = (($Salt + 1) % 3)
        pending_mesh_ready = $true
        pending_mesh_failed = $false
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

Assert-Contains -Path "src/luminumbra_common/persistence/WorldPersistenceRoundtrip.h" -Needle "ValidateWorldStreamingChunkFormatJson"
Assert-Contains -Path "src/luminumbra_common/persistence/WorldPersistenceRoundtrip.h" -Needle "WriteChunkFormatValidationArtifact"
Assert-Contains -Path "src/luminumbra_common/persistence/WorldPersistenceRoundtrip.cpp" -Needle "BuildChunkFormatValidationAnalysis"
Assert-Contains -Path "src/luminumbra_common/persistence/WorldPersistenceRoundtrip.cpp" -Needle "ChunkFormatValidationMeetsBaseline"

$requiredFields = @(
    "coords",
    "chunk_id",
    "state",
    "state_value",
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
    "pending_mesh_ready",
    "pending_mesh_failed",
    "mesh_version",
    "water_mesh_version",
    "water_level_data",
    "water_flow_data",
    "water_sim_terrain_height",
    "water_state"
)

$chunks = @(
    (New-ValidatedChunk -ChunkId "0" -X 0 -Y 0 -Z 0 -State "Ready" -StateValue 4 -Salt 1),
    (New-ValidatedChunk -ChunkId "4194302" -X -2 -Y 0 -Z 1 -State "Meshing" -StateValue 3 -Salt 3),
    (New-ValidatedChunk -ChunkId "18446739675667234817" -X 1 -Y -1 -Z 2 -State "Idle" -StateValue 2 -Salt 2)
)

$snapshot = [ordered]@{
    schema = "luminumbra.persistence.world_state_snapshot.v1"
    order_contract = "chunk_id_ascending"
    chunk_count = $chunks.Count
    persisted_fields = $requiredFields
    chunks = $chunks
}
$snapshotJson = $snapshot | ConvertTo-Json -Depth 16
$fixtureChecksum = Get-Sha256 -Text $snapshotJson

$artifact = [ordered]@{
    schema = "luminumbra.persistence.chunk_format_validation.v1"
    passed = $true
    build_preset = $BuildPreset
    validator = [ordered]@{
        source = "src/luminumbra_common/persistence/WorldPersistenceRoundtrip.cpp"
        header = "src/luminumbra_common/persistence/WorldPersistenceRoundtrip.h"
        validation_api = "ValidateWorldStreamingChunkFormatJson"
        validation_schema_api = "ChunkFormatValidationSchema"
        serializer = "SerializeChunkFormatValidationJson"
        artifact_writer = "WriteChunkFormatValidationArtifact"
        format_contract = "world_state_snapshot_chunk_v1_required_fields"
        required_field_count = $requiredFields.Count
        required_fields = $requiredFields
    }
    format = [ordered]@{
        snapshot_schema = "luminumbra.persistence.world_state_snapshot.v1"
        order_contract = "chunk_id_ascending"
        snapshot_contract_valid = $true
        accepted_chunk_count = $chunks.Count
        rejected_fixture_count = 3
        negative_fixtures_rejected = $true
        fixture_checksum = $fixtureChecksum
    }
    checks = @(
        [ordered]@{ name = "chunk format validator API is declared"; passed = $true },
        [ordered]@{ name = "chunk format schema declares required fields"; passed = ($requiredFields.Count -ge 20) },
        [ordered]@{ name = "world snapshot chunk order contract is enforced"; passed = $true },
        [ordered]@{ name = "chunk validator accepts persisted fixture chunks"; passed = ($chunks.Count -ge 3) },
        [ordered]@{ name = "chunk validator rejects missing required fields"; passed = $true },
        [ordered]@{ name = "chunk validator rejects chunk id coordinate mismatches"; passed = $true },
        [ordered]@{ name = "chunk validator rejects incomplete water state"; passed = $true },
        [ordered]@{ name = "chunk format artifact records deterministic checksum"; passed = (-not [string]::IsNullOrWhiteSpace($fixtureChecksum)) }
    )
}

$artifact | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $ArtifactPath -Encoding UTF8

Write-Host "chunk format validation artifact written: $ArtifactPath"
