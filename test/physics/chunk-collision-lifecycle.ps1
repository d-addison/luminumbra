param(
    [string]$BuildPreset = "debug"
)

$ErrorActionPreference = "Stop"

$WorldPath = "src/luminumbra_common/systems/SHIELD_WorldSystem.cpp"
$ArtifactDir = "build/$BuildPreset/test-artifacts/runtime/chunk-collision-lifecycle"
$ArtifactPath = Join-Path $ArtifactDir "chunk-collision-lifecycle.json"

function Assert-Contains {
    param(
        [string]$Name,
        [string]$Text,
        [string]$Pattern,
        [string]$Evidence
    )

    [pscustomobject]@{
        name = $Name
        passed = [bool]($Text -match $Pattern)
        evidence = $Evidence
    }
}

if (-not (Test-Path $WorldPath)) {
    throw "Missing world system source: $WorldPath"
}

$worldText = Get-Content $WorldPath -Raw
$directArrowAdds = [regex]::Matches($worldText, "->add_chunk_collision\(").Count
$dotAdds = [regex]::Matches($worldText, "\.add_chunk_collision\(").Count

$checks = @(
    Assert-Contains `
        -Name "replace helper removes stale collision before add" `
        -Text $worldText `
        -Pattern "replace_chunk_collision[\s\S]*?remove_chunk_collision\(id\)[\s\S]*?add_chunk_collision\(chunk\)[\s\S]*?has_collision\.store\(true" `
        -Evidence "replace_chunk_collision removes any existing collider before registering a new chunk mesh collider"

    Assert-Contains `
        -Name "runtime update uses lifecycle replacement helper" `
        -Text $worldText `
        -Pattern "replace_chunk_collision\(\*physics_system,\s*\*chunk_ptr\)" `
        -Evidence "SHIELD_WorldSystem::update re-adds chunk collision through replace_chunk_collision"

    Assert-Contains `
        -Name "initial horizon collision uses lifecycle replacement helper" `
        -Text $worldText `
        -Pattern "replace_chunk_collision\(\*physics_system,\s*\*chunk\)" `
        -Evidence "EnsureSurfaceReadyNear re-adds chunk collision through replace_chunk_collision"

    Assert-Contains `
        -Name "chunk unload removes collision before erasing chunk" `
        -Text $worldText `
        -Pattern "(?:void|bool) SHIELD_WorldSystem::update_chunk_activation[\s\S]*?for \(ChunkID id : to_unload\)[\s\S]*?physics_system->remove_chunk_collision\(id\);[\s\S]*?m_streaming_state\.chunks\.erase\(id\);" `
        -Evidence "update_chunk_activation unregisters physics collision before erasing unloaded chunks"

    Assert-Contains `
        -Name "clear world removes collisions before clearing chunks" `
        -Text $worldText `
        -Pattern "void SHIELD_WorldSystem::clear_world[\s\S]*?physics_system->remove_chunk_collision\(id\);[\s\S]*?m_streaming_state\.chunks\.clear\(\);" `
        -Evidence "clear_world unregisters all chunk collisions before clearing streaming chunks"

    Assert-Contains `
        -Name "terrain mesh rebuild invalidates collision flag" `
        -Text $worldText `
        -Pattern "mesh_version\+\+;[\s\S]*?has_collision\.store\(false" `
        -Evidence "terrain mesh replacement increments mesh_version and marks collision stale"

    [pscustomobject]@{
        name = "all collision adds flow through lifecycle replacement"
        passed = ($directArrowAdds -eq 0 -and $dotAdds -eq 1)
        evidence = "source contains $directArrowAdds direct pointer add_chunk_collision calls and $dotAdds helper add_chunk_collision call"
    }
)

$passed = -not @($checks | Where-Object { -not $_.passed })

New-Item -ItemType Directory -Force -Path $ArtifactDir | Out-Null

$analysis = [ordered]@{
    schema = "luminumbra.physics.chunk_collision_lifecycle.v1"
    passed = $passed
    generated_at_utc = (Get-Date).ToUniversalTime().ToString("o")
    build_preset = $BuildPreset
    world_system = [ordered]@{
        source = $WorldPath
        replacement_helper = "replace_chunk_collision"
        direct_add_chunk_collision_calls = $directArrowAdds
        helper_add_chunk_collision_calls = $dotAdds
    }
    checks = $checks
}

$analysis | ConvertTo-Json -Depth 8 | Set-Content -Path $ArtifactPath -Encoding UTF8

if (-not $passed) {
    throw "chunk collision lifecycle gate failed; see $ArtifactPath"
}

Write-Host "chunk collision lifecycle gate passed: $ArtifactPath"
