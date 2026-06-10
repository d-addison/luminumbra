param(
    [ValidateSet("CodexOnly", "Panels", "Files", "Sections", "Build", "UnitTests", "MaterialVisual", "RenderHealth", "ShaderInventory", "FrontierDisabled", "All")]
    [string]$Mode = "All",

    [string]$BuildPreset = "debug"
)

$ErrorActionPreference = "Stop"

$SpecPath = ".forge/specs/ENGINE-FRONTIER-2026-06-09.md"
$ArtifactDir = ".forge/artifacts/engine-frontier"
$FrontierDisabledPath = "$ArtifactDir/frontier-disabled.md"
$WorkflowPath = ".forge/workflows/engine-frontier.yaml"
$DispatchPath = ".forge/tasks/engine-frontier/dispatch.json"
$RunnerPath = ".forge/scripts/run-codex-engine-frontier.ps1"
$ExecuteWrapperPath = ".forge/scripts/run-engine-frontier-execute.ps1"

$PanelFiles = @(
    "$ArtifactDir/panel-1-world-streaming.md",
    "$ArtifactDir/panel-2-rendering-visual-loop.md",
    "$ArtifactDir/panel-3-physics-player.md",
    "$ArtifactDir/panel-4-audio.md",
    "$ArtifactDir/panel-5-simulation-ai-scripting.md",
    "$ArtifactDir/panel-6-persistence-gpu-network.md",
    "$ArtifactDir/panel-7-ui-tooling-tests.md"
)

function Assert-FileExists {
    param([string]$Path)
    if (-not (Test-Path $Path)) {
        throw "Missing engine-frontier file: $Path"
    }
}

function Assert-Contains {
    param(
        [string]$Path,
        [string]$Needle
    )
    Assert-FileExists $Path
    $text = Get-Content $Path -Raw
    if ($text -notmatch [regex]::Escape($Needle)) {
        throw "Missing '$Needle' in $Path"
    }
}

function Test-CodexOnly {
    Assert-FileExists ".forge/config.yaml"
    Assert-FileExists $WorkflowPath
    Assert-FileExists $RunnerPath

    $config = Get-Content ".forge/config.yaml" -Raw
    if ($config -notmatch "spawner_override:\s*codex") {
        throw "Forge config must set dispatch.spawner_override to codex"
    }
    if ($config -notmatch "allowed_spawners:\s*\[codex\]") {
        throw "Forge config must restrict dispatch.allowed_spawners to [codex]"
    }

    $workflow = Get-Content $WorkflowPath -Raw
    $runner = Get-Content $RunnerPath -Raw
    if ($workflow -notmatch "#runtime:codex") {
        throw "engine-frontier workflow must carry #runtime:codex tag"
    }
    if ($workflow -notmatch "run-codex-engine-frontier\.ps1") {
        throw "engine-frontier workflow must invoke the Codex CLI panel helper"
    }
    if ($workflow -notmatch "--spawner codex") {
        throw "engine-frontier workflow must pass --spawner codex for contract dispatch dry-runs"
    }
    if ($runner -notmatch "codex\s+exec") {
        throw "engine-frontier Codex helper must run panels through codex exec"
    }
    if (($workflow + "`n" + $runner) -match "forge\s+dispatch\s+spawners|all-spawner") {
        throw "engine-frontier workflow must not run all-spawner probes; use Codex-only checks"
    }

    $routingText = $workflow + "`n" + $runner
    if (Test-Path $ExecuteWrapperPath) {
        $routingText += "`n" + (Get-Content $ExecuteWrapperPath -Raw)
    }
    # dispatch.json is generated mid-workflow by the Revision panel; only check it once present.
    if (Test-Path $DispatchPath) {
        $dispatch = Get-Content $DispatchPath -Raw
        if ($dispatch -notmatch "Codex-only") {
            throw "engine-frontier dispatch graph must declare Codex-only execution in prompts or description"
        }
        $routingText += "`n" + $dispatch
    }
    if ($routingText -match "(?i)\b(claude|gemini|cursor|antigravity)\b") {
        throw "engine-frontier routing files must not reference non-Codex agents"
    }
}

function Test-Panels {
    foreach ($panel in $PanelFiles) {
        Assert-FileExists $panel
        foreach ($needle in @(
            "Subsystem State",
            "Findings",
            "Must-Fix",
            "Deepening",
            "Frontier Proposals",
            "Proposed Gates",
            "References"
        )) {
            Assert-Contains -Path $panel -Needle $needle
        }
    }

    $found = @(Get-ChildItem $ArtifactDir -Filter "panel-*.md" -ErrorAction SilentlyContinue)
    if ($found.Count -ne 7) {
        throw "Expected exactly 7 engine-frontier panel files, found $($found.Count)"
    }
}

function Test-Files {
    foreach ($file in @(
        $SpecPath,
        "$ArtifactDir/research.md",
        "$ArtifactDir/critique.md",
        "$ArtifactDir/ultimate-plan.md",
        "$ArtifactDir/handoff.md",
        $FrontierDisabledPath,
        $DispatchPath,
        $WorkflowPath,
        $RunnerPath,
        $ExecuteWrapperPath,
        ".forge/scripts/validate-engine-frontier.ps1"
    ) + $PanelFiles) {
        Assert-FileExists $file
    }
}

function Test-Sections {
    $checks = [ordered]@{
        $SpecPath = @("Objective", "Scope", "Acceptance Criteria", "Verification Commands", "No-Deferral Rules")
        "$ArtifactDir/research.md" = @("Consensus", "Emphasis", "World", "Rendering", "Physics", "Audio", "Simulation", "Persistence", "UI")
        "$ArtifactDir/critique.md" = @("Finding", "Mitigation", "Verdict")
        "$ArtifactDir/ultimate-plan.md" = @("Wave 1", "Success Definition", "Material")
        "$ArtifactDir/handoff.md" = @("Current Status", "Immediate Next Step", "No-Deferral Rules", "Success Definition")
        $FrontierDisabledPath = @("Status", "Gate", "Disabled by Default", "Allowed Activation", "Verification")
    }

    foreach ($path in $checks.Keys) {
        foreach ($needle in $checks[$path]) {
            Assert-Contains -Path $path -Needle $needle
        }
    }
}

function Test-Build {
    # Full preset build: the engine-frontier dispatch touches every subsystem,
    # and ctest registers placeholder entries for any test executable that was
    # not built.
    & cmake --build --preset $BuildPreset
    if ($LASTEXITCODE -ne 0) {
        exit $LASTEXITCODE
    }
}

function Test-UnitTests {
    # Placeholder entries named *_NOT_BUILT are registered at configure time
    # for missing test executables; exclude them so only real tests gate.
    & ctest --preset $BuildPreset --output-on-failure -E "_NOT_BUILT$"
    if ($LASTEXITCODE -ne 0) {
        exit $LASTEXITCODE
    }
}

function Test-MaterialVisual {
    $visualDir = "build/$BuildPreset/test-artifacts/runtime/material-visual"
    $analysisPath = Join-Path $visualDir "material-visual-analysis.json"

    if (-not (Test-Path $analysisPath)) {
        throw "material visual gate not yet implemented - missing $analysisPath (produced by task T-EF-1-material-visual-gate)"
    }

    $analysis = Get-Content $analysisPath -Raw | ConvertFrom-Json
    if ($analysis.schema -ne "luminumbra.material_visual_analysis.v1") {
        throw "Unexpected material visual analysis schema '$($analysis.schema)'"
    }
    if ([int64]$analysis.gl_debug.errors -ne 0) {
        throw "Material visual run emitted GL debug errors: $($analysis.gl_debug.errors)"
    }
    if ($null -eq $analysis.materials -or @($analysis.materials).Count -lt 1) {
        throw "Material visual analysis is missing the materials ROI array"
    }

    $sandEntries = @($analysis.materials | Where-Object { $_.name -eq "Sand" })
    if ($sandEntries.Count -lt 1) {
        throw "Material visual analysis has no Sand ROI entry; sand vs grey fallback is the primary gate target"
    }

    foreach ($entry in $analysis.materials) {
        if ($null -eq $entry.pixels -or $null -eq $entry.thresholds) {
            throw "Material ROI entry '$($entry.name)' is missing pixels or thresholds"
        }
        if ([int64]$entry.pixels.classified_pixels -lt [int64]$entry.thresholds.min_classified_pixels) {
            throw "Material ROI '$($entry.name)' has too few classified pixels: $($entry.pixels.classified_pixels) < $($entry.thresholds.min_classified_pixels)"
        }
        if ([int64]$entry.pixels.grey_fallback_pixels -gt [int64]$entry.thresholds.max_grey_fallback_pixels) {
            throw "Material ROI '$($entry.name)' shows grey fallback pixels above threshold: $($entry.pixels.grey_fallback_pixels) > $($entry.thresholds.max_grey_fallback_pixels)"
        }
    }

    if (-not $analysis.passed) {
        throw "Material visual analysis reported failure"
    }

    Assert-FileExists (Join-Path $visualDir $analysis.screenshot)
    Assert-FileExists (Join-Path $visualDir $analysis.heatmap_screenshot)
}

function Test-RenderHealth {
    $renderDir = "build/$BuildPreset/test-artifacts/render"
    $analysisPath = Join-Path $renderDir "render-health-analysis.json"

    if (-not (Test-Path $analysisPath)) {
        throw "render health gate not yet implemented - missing $analysisPath (produced by task T-EF-5-render-health-gate)"
    }

    $analysis = Get-Content $analysisPath -Raw | ConvertFrom-Json
    if ($analysis.schema -ne "luminumbra.render_health_analysis.v1") {
        throw "Unexpected render health analysis schema '$($analysis.schema)'"
    }
    if (-not $analysis.passed) {
        throw "Render health analysis reported failure"
    }
    if ($analysis.startup.health_snapshot_api -ne "get_render_health_snapshot") {
        throw "Render health analysis must be backed by RenderPipeline::get_render_health_snapshot"
    }
    if (-not $analysis.startup.health_api_present) {
        throw "Render health analysis reports missing RenderPipeline health API"
    }
    if ([int64]$analysis.gl_debug.errors -ne 0) {
        throw "Render health run emitted GL debug errors: $($analysis.gl_debug.errors)"
    }

    $programs = @($analysis.shader_health.programs)
    if ($programs.Count -lt 8) {
        throw "Render health analysis is missing shader program health entries"
    }
    foreach ($program in $programs) {
        if (-not $program.ok) {
            throw "Render shader health failed for '$($program.name)'"
        }
    }

    $requiredPasses = @("shadow", "gbuffer", "ssao", "ssao_blur", "lighting", "water", "skybox", "final_blit")
    $actualPasses = @($analysis.render_pass_metadata.required_passes)
    foreach ($requiredPass in $requiredPasses) {
        if ($actualPasses -notcontains $requiredPass) {
            throw "Render health analysis is missing render pass metadata for '$requiredPass'"
        }
    }
    if (-not $analysis.render_pass_metadata.present) {
        throw "Render health analysis reports missing render pass metadata"
    }

    if (-not $analysis.resource_registry.present) {
        throw "Render health analysis reports missing resource registry"
    }
    if (-not $analysis.resource_registry.debug_labels) {
        throw "Render health analysis must require debug labels for render resources"
    }
    if (-not $analysis.resource_registry.shutdown_requires_empty_registry) {
        throw "Render health analysis must require an empty registry after shutdown"
    }
    if (-not $analysis.resource_registry.empty_after_shutdown) {
        throw "Render health analysis reports leaked resources after shutdown"
    }

    if (-not $analysis.terrain_materials.present) {
        throw "Render health analysis reports missing terrain material diagnostics"
    }
    if (-not $analysis.terrain_materials.texture_array_required -or -not $analysis.terrain_materials.material_lut_required) {
        throw "Render health analysis must require terrain texture array and material LUT"
    }
    if ([int64]$analysis.terrain_materials.max_fallback_layers -ne 0) {
        throw "Render health analysis must require zero terrain texture fallback layers"
    }
}

function Test-ShaderInventory {
    $renderDir = "build/$BuildPreset/test-artifacts/render"
    $inventoryPath = Join-Path $renderDir "shader-inventory.json"
    $suiteHealthPath = Join-Path $renderDir "shader-suite-health.json"

    foreach ($path in @($inventoryPath, $suiteHealthPath)) {
        if (-not (Test-Path $path)) {
            throw "render shader inventory gate not yet implemented - missing $path (produced by task T-EF-10-render-shader-inventory)"
        }
    }

    $inventory = Get-Content $inventoryPath -Raw | ConvertFrom-Json
    if ($inventory.schema -ne "luminumbra.render.shader_inventory.v1") {
        throw "Unexpected shader inventory schema '$($inventory.schema)'"
    }

    $sources = @($inventory.sources)
    if ($sources.Count -lt 20) {
        throw "Shader inventory is missing source entries: found $($sources.Count)"
    }
    if ([int64]$inventory.source_count -ne $sources.Count) {
        throw "Shader inventory source_count does not match sources array"
    }
    if ([int64]$inventory.compiled_source_count -ne $sources.Count) {
        throw "Shader inventory must compile every listed shader source"
    }

    foreach ($source in $sources) {
        if ([string]::IsNullOrWhiteSpace($source.file)) {
            throw "Shader inventory contains a source entry without a file"
        }
        if ([string]::IsNullOrWhiteSpace($source.stage)) {
            throw "Shader inventory source '$($source.file)' is missing a stage"
        }
        if (-not $source.compiled) {
            throw "Shader inventory source '$($source.file)' did not compile"
        }
        if ([int64]$source.bytes -le 0) {
            throw "Shader inventory source '$($source.file)' has no byte size"
        }
    }

    foreach ($stage in @("vertex", "fragment", "geometry")) {
        if ([int64]$inventory.stage_counts.$stage -lt 1) {
            throw "Shader inventory is missing $stage shader coverage"
        }
    }

    $requiredPrograms = @(
        "basic",
        "g_buffer",
        "instanced_mesh_gbuffer",
        "lighting_pass",
        "skybox",
        "shadow_map",
        "ssao",
        "ssao_blur",
        "water",
        "rml_ui",
        "loading_hologram",
        "loading_visual",
        "volumetric_lighting",
        "magical_particles"
    )

    $inventoryPrograms = @($inventory.pipeline_programs)
    if ([int64]$inventory.pipeline_program_count -ne $inventoryPrograms.Count) {
        throw "Shader inventory pipeline_program_count does not match pipeline_programs array"
    }
    foreach ($requiredProgram in $requiredPrograms) {
        $matches = @($inventoryPrograms | Where-Object { $_.name -eq $requiredProgram })
        if ($matches.Count -ne 1) {
            throw "Shader inventory is missing pipeline program '$requiredProgram'"
        }
        if (@($matches[0].stages).Count -lt 2) {
            throw "Shader inventory program '$requiredProgram' must list at least vertex and fragment stages"
        }
    }

    $magicalParticles = @($inventoryPrograms | Where-Object { $_.name -eq "magical_particles" })
    if (@($magicalParticles[0].stages | Where-Object { $_.stage -eq "geometry" }).Count -ne 1) {
        throw "Shader inventory must record the magical_particles geometry stage"
    }

    $suiteHealth = Get-Content $suiteHealthPath -Raw | ConvertFrom-Json
    if ($suiteHealth.schema -ne "luminumbra.render.shader_suite_health.v1") {
        throw "Unexpected shader suite health schema '$($suiteHealth.schema)'"
    }
    if (-not $suiteHealth.passed) {
        throw "Shader suite health reported failure"
    }
    if ([int64]$suiteHealth.gl_debug.errors -ne 0) {
        throw "Shader suite health emitted GL debug errors: $($suiteHealth.gl_debug.errors)"
    }

    $healthPrograms = @($suiteHealth.programs)
    if ([int64]$suiteHealth.expected_program_count -ne $requiredPrograms.Count) {
        throw "Shader suite health expected_program_count must cover the required render pipeline programs"
    }
    if ([int64]$suiteHealth.linked_program_count -ne $healthPrograms.Count) {
        throw "Shader suite health linked_program_count does not match programs array"
    }
    foreach ($requiredProgram in $requiredPrograms) {
        $matches = @($healthPrograms | Where-Object { $_.name -eq $requiredProgram })
        if ($matches.Count -ne 1) {
            throw "Shader suite health is missing program '$requiredProgram'"
        }
        if (-not $matches[0].compiled -or -not $matches[0].linked -or -not $matches[0].ok) {
            throw "Shader suite health failed for '$requiredProgram'"
        }
    }
}

function Test-FrontierDisabled {
    Assert-FileExists $FrontierDisabledPath

    foreach ($needle in @(
        "Gate: disabled by default",
        "Default state: documentation and validation only",
        "frontier runtime behavior requires explicit opt-in",
        "Activation must be documented",
        "validate-engine-frontier.ps1 -Mode FrontierDisabled"
    )) {
        Assert-Contains -Path $FrontierDisabledPath -Needle $needle
    }

    $text = Get-Content $FrontierDisabledPath -Raw
    if ($text -match "(?i)\benabled by default\b") {
        throw "frontier-disabled gate must not declare frontier behavior enabled by default"
    }
}

switch ($Mode) {
    "CodexOnly" { Test-CodexOnly }
    "Panels" { Test-Panels }
    "Files" { Test-Files }
    "Sections" { Test-Sections }
    "Build" { Test-Build }
    "UnitTests" { Test-UnitTests }
    "MaterialVisual" { Test-MaterialVisual }
    "RenderHealth" { Test-RenderHealth }
    "ShaderInventory" { Test-ShaderInventory }
    "FrontierDisabled" { Test-FrontierDisabled }
    "All" {
        Test-CodexOnly
        Test-Files
        Test-Sections
        Test-Panels
        Test-RenderHealth
        Test-ShaderInventory
        Test-FrontierDisabled
    }
}

Write-Host "engine-frontier validation passed: $Mode"
