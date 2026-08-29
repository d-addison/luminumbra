param(
    [string]$BuildDir = "build/debug",
    [string]$OutputDir = ""
)

$ErrorActionPreference = "Stop"

if ([string]::IsNullOrWhiteSpace($OutputDir)) {
    $OutputDir = Join-Path $BuildDir "test-artifacts/tooling"
}

New-Item -ItemType Directory -Force -Path $OutputDir | Out-Null

function Assert-FileExists {
    param([string]$Path)
    if (-not (Test-Path -LiteralPath $Path)) {
        throw "Missing tooling input artifact: $Path"
    }
}

function Read-Json {
    param([string]$Path)
    Assert-FileExists $Path
    return Get-Content -LiteralPath $Path -Raw | ConvertFrom-Json
}

function Get-RequiredSchema {
    param(
        [object]$Object,
        [string]$Path
    )
    if ([string]::IsNullOrWhiteSpace([string]$Object.schema)) {
        throw "Artifact is missing required schema field: $Path"
    }
    return [string]$Object.schema
}

$artifactRoot = Join-Path $BuildDir "test-artifacts"
$worldgenPath = Join-Path $artifactRoot "worldgen_layers/worldgen_layers.json"
$runtimeVisualPath = Join-Path $artifactRoot "runtime_world_visual/runtime_world_visual.json"
$seamsPath = Join-Path $artifactRoot "runtime_world_visual/lod_seams.json"
$renderPassPath = Join-Path $artifactRoot "render_framework/render_pass_metadata.json"
$shaderHealthPath = Join-Path $artifactRoot "render_framework/shader_health.json"
$captureHooksPath = Join-Path $artifactRoot "render_framework/capture_hooks.json"
$benchmarkSummaryPath = Join-Path $artifactRoot "performance_framework/benchmark_summary.json"
$regressionBudgetPath = Join-Path $artifactRoot "performance_framework/regression_budget.json"

$worldgen = Read-Json $worldgenPath
$runtimeVisual = Read-Json $runtimeVisualPath
$seams = Read-Json $seamsPath
$renderPass = Read-Json $renderPassPath
$shaderHealth = Read-Json $shaderHealthPath
$captureHooks = Read-Json $captureHooksPath
$benchmarkSummary = Read-Json $benchmarkSummaryPath
$regressionBudget = Read-Json $regressionBudgetPath

$worldgenSchema = Get-RequiredSchema -Object $worldgen -Path $worldgenPath
$runtimeVisualSchema = Get-RequiredSchema -Object $runtimeVisual -Path $runtimeVisualPath
$seamsSchema = Get-RequiredSchema -Object $seams -Path $seamsPath
$renderPassSchema = Get-RequiredSchema -Object $renderPass -Path $renderPassPath
$shaderHealthSchema = Get-RequiredSchema -Object $shaderHealth -Path $shaderHealthPath
$captureHooksSchema = Get-RequiredSchema -Object $captureHooks -Path $captureHooksPath
$benchmarkSummarySchema = Get-RequiredSchema -Object $benchmarkSummary -Path $benchmarkSummaryPath
$regressionBudgetSchema = Get-RequiredSchema -Object $regressionBudget -Path $regressionBudgetPath

$topologyDeltas = @()
foreach ($delta in $worldgen.deltas) {
    $topologyDeltas += [ordered]@{
        from = $delta.from
        to = $delta.to
        changed_sdf_samples = [int]$delta.changed_sdf_samples
        sdf_sign_flips = [int]$delta.sdf_sign_flips
        changed_height_samples = [int]$delta.changed_height_samples
    }
}

$worldgenViewer = [ordered]@{
    schema = "luminumbra.tooling.worldgen_viewer.v1"
    consumes = $worldgenPath
    layer_schema = $worldgenSchema
    layer_toggles = @("01_base_terrain", "02_island_mask", "03_caves", "04_caves_lod4", "05_submerged_water")
    topology_deltas = $topologyDeltas
    atlas_html = Join-Path $artifactRoot "worldgen_layers/atlas/worldgen_atlas.html"
}

$overlay = [ordered]@{
    schema = "luminumbra.tooling.runtime_overlay.v1"
    chunk_fields = @("total_chunks", "ready_chunks", "loading_chunks", "meshing_chunks", "renderable_chunks", "collision_chunks", "lod_distribution", "seam_risk_samples")
    queue_fields = @("generation_job_active", "meshing_job_active", "generation_budget", "meshing_budget", "deferred_generation", "deferred_meshing", "upload_backlog_high_water")
    render_fields = @("pass_name", "draw_count", "shadow_draws", "terrain_draws", "water_draws", "skybox_draws", "estimated_vram_bytes", "resource_registry")
    performance_fields = @("frame_time_ms_p50", "frame_time_ms_p95", "frame_time_ms_p99", "memory_high_water", "job_queue_high_water", "regression_budget_passed")
    shader_fields = @("geometry_shader_ok", "lighting_shader_ok", "terrain_material_fallback_layers", "shader_diagnostics")
    missing_horizon_reasons = @("not_requested", "density_pending", "meshing_failed", "upload_pending", "culled", "shader_failed", "resource_missing")
    consumes = [ordered]@{
        runtime_visual = $runtimeVisualPath
        seams = $seamsPath
        render_passes = $renderPassPath
        shader_health = $shaderHealthPath
        performance_summary = $benchmarkSummaryPath
    }
}

$index = [ordered]@{
    schema = "luminumbra.tooling.index.v1"
    generated_at = (Get-Date).ToString("o")
    artifacts = [ordered]@{
        worldgen_layers = $worldgenPath
        runtime_visual = $runtimeVisualPath
        lod_seams = $seamsPath
        render_pass_metadata = $renderPassPath
        shader_health = $shaderHealthPath
        capture_hooks = $captureHooksPath
        benchmark_summary = $benchmarkSummaryPath
        regression_budget = $regressionBudgetPath
    }
    schemas = [ordered]@{
        worldgen = $worldgenSchema
        runtime_visual = $runtimeVisualSchema
        seams = $seamsSchema
        render_pass = $renderPassSchema
        shader_health = $shaderHealthSchema
        capture_hooks = $captureHooksSchema
        benchmark_summary = $benchmarkSummarySchema
        regression_budget = $regressionBudgetSchema
    }
    status = [ordered]@{
        regression_budget_passed = [bool]$regressionBudget.passed
        scenario_count = @($benchmarkSummary.scenarios).Count
        render_pass_count = @($renderPass.passes).Count
        topology_delta_count = @($worldgen.deltas).Count
    }
}

$worldgenViewerPath = Join-Path $OutputDir "worldgen_layer_viewer.json"
$overlayPath = Join-Path $OutputDir "runtime_overlay_snapshot.json"
$indexPath = Join-Path $OutputDir "tooling_index.json"
$htmlPath = Join-Path $OutputDir "artifact_report.html"

$worldgenViewer | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $worldgenViewerPath
$overlay | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $overlayPath
$index | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $indexPath

$html = @"
<!doctype html>
<html>
<head>
  <meta charset="utf-8">
  <title>Luminumbra Tooling Artifact Report</title>
  <style>
    body { font-family: Segoe UI, Arial, sans-serif; margin: 24px; color: #1b1f23; }
    table { border-collapse: collapse; width: 100%; margin: 16px 0; }
    th, td { border: 1px solid #d0d7de; padding: 6px 8px; text-align: left; }
    th { background: #f6f8fa; }
    code { background: #f6f8fa; padding: 1px 4px; }
  </style>
</head>
<body>
  <h1>Luminumbra Tooling Artifact Report</h1>
  <p>Consumes the same JSON schemas produced by runtime, world, render, and performance gates.</p>
  <table>
    <tr><th>Artifact</th><th>Schema</th><th>Path</th></tr>
    <tr><td>Worldgen Layers</td><td>$worldgenSchema</td><td><code>$worldgenPath</code></td></tr>
    <tr><td>Runtime Visual</td><td>$runtimeVisualSchema</td><td><code>$runtimeVisualPath</code></td></tr>
    <tr><td>LOD Seams</td><td>$seamsSchema</td><td><code>$seamsPath</code></td></tr>
    <tr><td>Render Passes</td><td>$renderPassSchema</td><td><code>$renderPassPath</code></td></tr>
    <tr><td>Shader Health</td><td>$shaderHealthSchema</td><td><code>$shaderHealthPath</code></td></tr>
    <tr><td>Benchmarks</td><td>$benchmarkSummarySchema</td><td><code>$benchmarkSummaryPath</code></td></tr>
  </table>
  <p>Regression budget passed: <strong>$($regressionBudget.passed)</strong></p>
  <p>Generated tooling index: <code>$indexPath</code></p>
</body>
</html>
"@
$html | Set-Content -LiteralPath $htmlPath

Write-Host "Tooling report generated: $htmlPath"
Write-Host "Tooling index generated: $indexPath"
