param(
    [string]$BuildDir = "build/debug"
)

$ErrorActionPreference = "Stop"

$requiredPaths = @(
    "$BuildDir/test-artifacts/runtime_world_visual/runtime_world_visual.json",
    "$BuildDir/test-artifacts/runtime_world_visual/lod_seams.json",
    "$BuildDir/test-artifacts/runtime_world_visual/physics_water_budget.json",
    "$BuildDir/test-artifacts/runtime_world_visual/spawn_horizon.ppm",
    "$BuildDir/test-artifacts/performance/initial_world_loading.json",
    "$BuildDir/test-artifacts/performance/streaming_budget.json",
    "$BuildDir/test-artifacts/performance/meshing_throughput.json",
    "$BuildDir/test-artifacts/render_perf/pass_counts.json"
)

foreach ($path in $requiredPaths) {
    if (-not (Test-Path $path)) {
        throw "Missing required blocker artifact: $path"
    }
}

$visual = Get-Content "$BuildDir/test-artifacts/runtime_world_visual/runtime_world_visual.json" -Raw | ConvertFrom-Json
if ([int]$visual.horizon.mesh_chunks -lt 600) {
    throw "runtime visual mesh chunk threshold failed: $($visual.horizon.mesh_chunks)"
}
if ([int]$visual.horizon.collision_chunks -ne 81) {
    throw "near collision budget threshold failed: $($visual.horizon.collision_chunks)"
}
if ([int]$visual.image.foreground_pixels -lt 4608) {
    throw "runtime visual foreground threshold failed: $($visual.image.foreground_pixels)"
}
if ([int]$visual.image.occupied_tiles -lt 45) {
    throw "runtime visual occupied tile threshold failed: $($visual.image.occupied_tiles)"
}

$quadrantsWithMesh = 0
foreach ($count in $visual.horizon.quadrant_mesh_chunks) {
    if ([int]$count -gt 0) {
        $quadrantsWithMesh++
    }
}
if ($quadrantsWithMesh -ne 4) {
    throw "runtime visual quadrant coverage failed: $quadrantsWithMesh"
}

$seams = Get-Content "$BuildDir/test-artifacts/runtime_world_visual/lod_seams.json" -Raw | ConvertFrom-Json
if ([int]$seams.metrics.density_mismatches -ne 0) {
    throw "LOD seam density continuity failed: $($seams.metrics.density_mismatches)"
}
if ([int]$seams.metrics.mixed_lod_pairs -le 0) {
    throw "LOD seam detector did not report mixed LOD pairs"
}
if ($null -ne $seams.metrics.mixed_lod_pairs_without_transition_skirt -and [int]$seams.metrics.mixed_lod_pairs_without_transition_skirt -ne 0) {
    throw "LOD seam transition pair coverage failed: $($seams.metrics.mixed_lod_pairs_without_transition_skirt)"
}
if ([double]$seams.metrics.max_density_delta -gt 0.05) {
    throw "LOD seam max density delta exceeded threshold: $($seams.metrics.max_density_delta)"
}
if ($null -ne $seams.metrics.unmitigated_transition_risk_samples -and [int]$seams.metrics.unmitigated_transition_risk_samples -ne 0) {
    throw "LOD seam transition geometry coverage failed: $($seams.metrics.unmitigated_transition_risk_samples)"
}

$physics = Get-Content "$BuildDir/test-artifacts/runtime_world_visual/physics_water_budget.json" -Raw | ConvertFrom-Json
if ([int]$physics.horizon.collision_chunks -ne 81) {
    throw "physics/water collision budget artifact failed: $($physics.horizon.collision_chunks)"
}

Write-Host "Major blocker artifacts OK: $BuildDir"
