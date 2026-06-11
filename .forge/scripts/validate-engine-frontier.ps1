param(
    [ValidateSet("CodexOnly", "Panels", "Files", "Sections", "Build", "UnitTests", "MaterialVisual", "RenderHealth", "ShaderInventory", "GpuSdfCallbackSafetyGate", "GpuSdfComputeParityGate", "GpuSdfRuntimeToggleGate", "ChunkCollisionLifecycle", "PhysicsReplay", "AudioNullTelemetry", "AudioHandleApplication", "UiTestBaseline", "SimulationEventBusOrderGate", "LuaApiManifestGate", "AethericDiffusionGate", "InstinctPlannerGate", "PersistenceRoundtripGate", "PersistenceRuntimeRoundtrip", "ChunkFormatValidationGate", "WorldHashEntitySnapshotGate", "NetworkLoopbackAuthorityGate", "NetworkStateHash", "PerfRegression", "FrontierDisabled", "SkyboxVisual", "WeatherVisual", "TimeOfDaySweep", "PlayerView", "FarLodHorizon", "HeadlessServerTick", "All")]
    [string]$Mode = "All",

    [string]$BuildPreset = "debug",
    # T-I3-20: perf-lane preset selection (PerfRegression mode only).
    # "release" compares the release build (build/release) against
    # perf-baseline-release.json; "debug" or default (empty) preserves the
    # historical behavior exactly: build dir from -BuildPreset and the debug
    # baseline perf-baseline.json. Existing callers are unchanged.
    [ValidateSet("", "debug", "release")]
    [string]$Preset = "",
    [int]$SmokeSeconds = 30,
    # Debug-build wall-clock on a developer desktop drifts ~30% between
    # adjacent median-of-3 batches (background load, thermals). The gate is a
    # catastrophic-regression catcher: real algorithmic regressions are 2-10x,
    # so fail at +50% and warn at +25%. Tighten only with a quieter lane.
    [double]$MarginPercent = 50.0,
    [double]$WarnPercent = 25.0,
    # Absolute per-scenario allowance added on top of the relative margins.
    # The streaming optimizations (T-I2-14) dropped several scenario p99s from
    # 20-77 ms to 2-13 ms, where a purely relative margin sits below the
    # debug-build noise floor: identical code measured 3-25 ms p99 swings
    # between adjacent runs on a developer desktop with typical background
    # load. The floor is sized to absorb those observed outliers while the
    # relative margin still catches catastrophic (2-10x) regressions on the
    # slow scenarios; ceiling = baseline * (1 + margin) + this floor.
    [double]$NoiseFloorMs = 20.0
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

function Read-JsonArtifact {
    param(
        [string]$Path,
        [string]$Schema
    )
    Assert-FileExists $Path
    $artifact = Get-Content $Path -Raw | ConvertFrom-Json
    if ($artifact.schema -ne $Schema) {
        throw "Unexpected schema '$($artifact.schema)' in $Path"
    }
    return $artifact
}

function Assert-ArtifactPassed {
    param(
        [object]$Artifact,
        [string]$Name
    )
    if (-not $Artifact.passed) {
        throw "$Name artifact reported failure"
    }
}

function Assert-PpmArtifact {
    param([string]$Path)
    Assert-FileExists $Path
    $stream = [System.IO.File]::OpenRead((Resolve-Path $Path))
    try {
        $first = $stream.ReadByte()
        $second = $stream.ReadByte()
        if ($first -ne [byte][char]'P' -or ($second -ne [byte][char]'6' -and $second -ne [byte][char]'3')) {
            throw "Unexpected PPM magic in $Path"
        }
    } finally {
        $stream.Dispose()
    }
}

function Assert-ArrayContains {
    param(
        [object]$Values,
        [string]$Needle,
        [string]$Description
    )
    if (@($Values) -notcontains $Needle) {
        throw "$Description is missing '$Needle'"
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

function Invoke-Checked {
    param(
        [string]$FilePath,
        [string[]]$ArgumentList,
        [int[]]$AllowedExitCodes = @(0),
        [int]$TimeoutSeconds = 120
    )

    $argumentsText = ($ArgumentList | ForEach-Object {
        '"' + ($_ -replace '"', '\"') + '"'
    }) -join " "
    $stdoutPath = [System.IO.Path]::GetTempFileName()
    $stderrPath = [System.IO.Path]::GetTempFileName()
    try {
        $psi = New-Object System.Diagnostics.ProcessStartInfo
        $psi.FileName = "cmd.exe"
        $psi.Arguments = '/d /s /c ""{0}" {1} > "{2}" 2> "{3}""' -f $FilePath, $argumentsText, $stdoutPath, $stderrPath
        $psi.WorkingDirectory = (Get-Location).Path
        $psi.UseShellExecute = $false
        $process = [System.Diagnostics.Process]::Start($psi)

        if (-not $process.WaitForExit($TimeoutSeconds * 1000)) {
            try { $process.Kill($true) } catch { }
            throw "Command timed out after $TimeoutSeconds seconds: $FilePath $($ArgumentList -join ' ')"
        }

        $stdout = Get-Content -LiteralPath $stdoutPath -Raw -ErrorAction SilentlyContinue
        $stderr = Get-Content -LiteralPath $stderrPath -Raw -ErrorAction SilentlyContinue
        $exitCode = $process.ExitCode
        if ($AllowedExitCodes -notcontains $exitCode) {
            throw "Command failed with exit code $($exitCode): $FilePath $($ArgumentList -join ' ')`nstdout=$stdout`nstderr=$stderr"
        }
    }
    finally {
        Remove-Item -LiteralPath $stdoutPath, $stderrPath -Force -ErrorAction SilentlyContinue
    }
}

function Get-ClientExe {
    $exe = "build/$BuildPreset/bin/luminumbra_client_app.exe"
    if (-not (Test-Path $exe)) {
        throw "Missing client executable. Run -Mode Build first: $exe"
    }
    return $exe
}

function Test-MaterialVisual {
    $exe = Get-ClientExe
    $visualDir = "build/$BuildPreset/test-artifacts/runtime/material-visual"
    Remove-Item -Recurse -Force -ErrorAction SilentlyContinue $visualDir
    New-Item -ItemType Directory -Force -Path $visualDir | Out-Null

    $runSeconds = [Math]::Max(20, $SmokeSeconds)
    Invoke-Checked -FilePath $exe -ArgumentList @(
        "--scenario", "material_visual_smoke",
        "--auto-create-world",
        "--auto-enter-world",
        "--timed-run", "$runSeconds",
        "--no-audio",
        "--no-ui",
        "--runtime-artifact-dir", $visualDir
    ) -TimeoutSeconds ([Math]::Max(120, $runSeconds + 90))

    $analysisPath = Join-Path $visualDir "material-visual-analysis.json"
    if (-not (Test-Path $analysisPath)) {
        throw "material visual run did not produce $analysisPath (gate produced by task T-EF-1-material-visual-gate)"
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

    $grassEntries = @($analysis.materials | Where-Object { $_.name -eq "Grass" })
    if ($grassEntries.Count -lt 1) {
        throw "Material visual analysis has no Grass ROI entry; the composite vantage must show grass above the beach band"
    }

    $stoneEntries = @($analysis.materials | Where-Object { $_.name -eq "Stone" })
    if ($stoneEntries.Count -lt 1) {
        throw "Material visual analysis has no Stone ROI entry; the rim sub-ROI must show the cliff-rim stone band"
    }
    if ($stoneEntries[0].roi_scope -ne "rim_band") {
        throw "Stone ROI entry must be scoped to the rim sub-ROI (legitimate stone is grey-fallback-shaped; see classifier docs)"
    }

    $soilEntries = @($analysis.materials | Where-Object { $_.name -eq "Soil" })
    if ($soilEntries.Count -lt 1) {
        throw "Material visual analysis has no Soil ROI entry; the rim sub-ROI must show the depth 1-5 soil band"
    }
    if ($soilEntries[0].roi_scope -ne "rim_band") {
        throw "Soil ROI entry must be scoped to the rim sub-ROI (rim interpolation-error exposure; see classifier docs)"
    }

    foreach ($entry in $analysis.materials) {
        if ($null -eq $entry.pixels -or $null -eq $entry.thresholds) {
            throw "Material ROI entry '$($entry.name)' is missing pixels or thresholds"
        }
        if ([int64]$entry.pixels.classified_pixels -lt [int64]$entry.thresholds.min_classified_pixels) {
            throw "Material ROI '$($entry.name)' has too few classified pixels: $($entry.pixels.classified_pixels) < $($entry.thresholds.min_classified_pixels)"
        }
        if ([double]$entry.pixels.classified_ratio -lt [double]$entry.thresholds.min_classified_ratio) {
            throw "Material ROI '$($entry.name)' has too low a classified ratio: $($entry.pixels.classified_ratio) < $($entry.thresholds.min_classified_ratio)"
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

    if ($null -eq $analysis.gpu_timers) {
        throw "Render health analysis is missing the gpu_timers section"
    }
    if (-not $analysis.gpu_timers.api_present) {
        throw "Render health analysis reports missing per-pass GPU timer API"
    }
    $gpuTimerPasses = @($analysis.gpu_timers.passes)
    foreach ($requiredPass in $requiredPasses) {
        $timerEntries = @($gpuTimerPasses | Where-Object { $_.name -eq $requiredPass })
        if ($timerEntries.Count -ne 1) {
            throw "Render health gpu_timers is missing pass '$requiredPass'"
        }
        if ([double]$timerEntries[0].gpu_ms -lt 0) {
            throw "Render health gpu_timers pass '$requiredPass' reports a negative gpu_ms"
        }
    }
    if (-not [bool]$analysis.gpu_timers.supported) {
        # Unsupported GPU timer hardware is a PASS, but timings must be zeroed.
        foreach ($timerEntry in $gpuTimerPasses) {
            if ([double]$timerEntry.gpu_ms -ne 0) {
                throw "Render health gpu_timers reports non-zero gpu_ms while unsupported"
            }
        }
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

function Test-GpuSdfCallbackSafetyGate {
    $renderDir = "build/$BuildPreset/test-artifacts/render"
    $analysisPath = Join-Path $renderDir "gpu-sdf-callback-safety.json"

    if (-not (Test-Path $analysisPath)) {
        throw "gpu SDF callback safety gate not yet implemented - missing $analysisPath (produced by task T-EF-27-gpu-sdf-callback-safety-gate)"
    }

    $analysis = Get-Content $analysisPath -Raw | ConvertFrom-Json
    if ($analysis.schema -ne "luminumbra.render.gpu_sdf_callback_safety.v1") {
        throw "Unexpected GPU SDF callback safety schema '$($analysis.schema)'"
    }
    if (-not $analysis.passed) {
        throw "GPU SDF callback safety analysis reported failure"
    }
    if ($analysis.callback.source -ne "src/luminumbra_client/rendering/RenderPipeline.cpp") {
        throw "GPU SDF callback safety analysis must inspect RenderPipeline.cpp"
    }
    if ($analysis.callback.header -ne "src/luminumbra_client/rendering/RenderPipeline.h") {
        throw "GPU SDF callback safety analysis must inspect RenderPipeline.h"
    }
    if ($analysis.callback.setup_api -ne "SetupGPUSDFIntegration") {
        throw "GPU SDF callback safety analysis must require SetupGPUSDFIntegration"
    }
    if ($analysis.callback.generation_api -ne "generate_chunk_sdf_gpu") {
        throw "GPU SDF callback safety analysis must require generate_chunk_sdf_gpu"
    }
    if ($analysis.callback.world_callback -ne "SetGPUSDFCallback") {
        throw "GPU SDF callback safety analysis must require SetGPUSDFCallback"
    }
    if ($analysis.callback.disabled_gate -ne "kEnableExperimentalGpuSdfIntegration") {
        throw "GPU SDF callback safety analysis must require kEnableExperimentalGpuSdfIntegration"
    }
    if ($analysis.callback.default_enabled -ne $false) {
        throw "GPU SDF callback integration must remain disabled by default"
    }
    if (-not $analysis.callback.callback_api_present) {
        throw "GPU SDF callback safety analysis reports missing callback API"
    }
    if (-not $analysis.callback.clears_callback_when_disabled) {
        throw "GPU SDF callback setup must clear the world callback while disabled"
    }
    if (-not $analysis.callback.raw_this_capture_present) {
        throw "GPU SDF callback safety analysis must report the raw pipeline capture risk"
    }
    if (-not $analysis.callback.raw_this_capture_gated) {
        throw "Raw RenderPipeline capture must stay gated behind explicit opt-in"
    }
    if (-not $analysis.callback.gpu_readback_is_synchronous) {
        throw "GPU SDF callback safety analysis must record synchronous readback while callback path is disabled"
    }
    if (-not $analysis.callback.gl_context_required) {
        throw "GPU SDF callback safety analysis must record render GL context ownership"
    }
    if (-not $analysis.callback.safe_until_explicit_opt_in) {
        throw "GPU SDF callback path must be safe until explicit opt-in"
    }

    $requiredChecks = @(
        "gpu sdf callback API is present",
        "gpu sdf integration is disabled by default",
        "disabled setup clears any world callback",
        "raw pipeline capture is gated behind explicit opt-in",
        "gpu readback stays synchronous while callback path is disabled",
        "callback path requires render GL context ownership"
    )
    $checks = @($analysis.checks)
    foreach ($requiredCheck in $requiredChecks) {
        $matches = @($checks | Where-Object { $_.name -eq $requiredCheck })
        if ($matches.Count -ne 1) {
            throw "GPU SDF callback safety analysis is missing check '$requiredCheck'"
        }
        if (-not $matches[0].passed) {
            throw "GPU SDF callback safety check failed: $requiredCheck"
        }
    }
}

function Test-GpuSdfComputeParityGate {
    $renderDir = "build/$BuildPreset/test-artifacts/render"
    $analysisPath = Join-Path $renderDir "gpu-sdf-compute-parity.json"

    if (-not (Test-Path $analysisPath)) {
        throw "gpu SDF compute parity gate not yet implemented - missing $analysisPath (produced by task T-EF-28-gpu-sdf-compute-parity-gate)"
    }

    $analysis = Get-Content $analysisPath -Raw | ConvertFrom-Json
    if ($analysis.schema -ne "luminumbra.render.gpu_sdf_compute_parity.v1") {
        throw "Unexpected GPU SDF compute parity schema '$($analysis.schema)'"
    }
    if (-not $analysis.passed) {
        throw "GPU SDF compute parity analysis reported failure"
    }
    if ($analysis.parity.source -ne "src/luminumbra_client/rendering/RenderPipeline.cpp") {
        throw "GPU SDF compute parity analysis must inspect RenderPipeline.cpp"
    }
    if ($analysis.parity.header -ne "src/luminumbra_client/rendering/RenderPipeline.h") {
        throw "GPU SDF compute parity analysis must inspect RenderPipeline.h"
    }
    if ($analysis.parity.chunk_contract -ne "src/luminumbra_common/world/Chunk.h") {
        throw "GPU SDF compute parity analysis must inspect Chunk.h"
    }
    if ($analysis.parity.compute_api -ne "generate_chunk_sdf_gpu") {
        throw "GPU SDF compute parity analysis must require generate_chunk_sdf_gpu"
    }
    if ($analysis.parity.compute_shader -ne "res/shaders/sdf_generation.compute") {
        throw "GPU SDF compute parity analysis must require sdf_generation.compute"
    }
    if ($analysis.parity.cpu_reference -ne "authoritative CPU worldgen path") {
        throw "GPU SDF compute parity analysis must retain the authoritative CPU reference path"
    }
    if ($analysis.parity.disabled_gate -ne "kEnableExperimentalGpuSdfIntegration") {
        throw "GPU SDF compute parity analysis must require kEnableExperimentalGpuSdfIntegration"
    }
    if ($analysis.parity.default_enabled -ne $false) {
        throw "GPU SDF compute path must remain disabled by default until parity passes"
    }
    if ($analysis.parity.sample_grid -ne "17x17x17") {
        throw "GPU SDF compute parity sample grid must be 17x17x17"
    }
    if ([int64]$analysis.parity.sample_count -ne 4913) {
        throw "GPU SDF compute parity sample count must be 4913"
    }
    if ($analysis.parity.dispatch_groups -ne "3x3x3") {
        throw "GPU SDF compute parity dispatch groups must be 3x3x3"
    }
    if ($analysis.parity.workgroup_size -ne "8x8x8") {
        throw "GPU SDF compute parity workgroup size must be 8x8x8"
    }
    if ($analysis.parity.readback -ne "synchronous_ssbo_readback") {
        throw "GPU SDF compute parity readback must remain synchronous while the callback path is disabled"
    }
    if ([double]$analysis.parity.max_abs_error_threshold -le 0.0 -or [double]$analysis.parity.max_abs_error_threshold -gt 0.001) {
        throw "GPU SDF compute parity max_abs_error_threshold must be explicit and <= 0.001"
    }
    if ([double]$analysis.parity.mean_abs_error_threshold -le 0.0 -or [double]$analysis.parity.mean_abs_error_threshold -gt 0.0001) {
        throw "GPU SDF compute parity mean_abs_error_threshold must be explicit and <= 0.0001"
    }
    if ([int64]$analysis.parity.fixture_count -lt 3) {
        throw "GPU SDF compute parity must cover at least three fixtures"
    }
    if (-not $analysis.parity.gpu_callback_requires_passing_parity) {
        throw "GPU SDF callback activation must require passing compute parity"
    }
    if (-not $analysis.parity.gpu_path_blocked_until_parity_passes) {
        throw "GPU SDF compute path must stay blocked until parity passes"
    }
    if (-not $analysis.parity.authoritative_cpu_path_retained) {
        throw "GPU SDF compute parity must retain the authoritative CPU path"
    }

    $fixtures = @($analysis.parity.fixtures)
    foreach ($fixtureName in @("origin", "positive_offset", "negative_offset")) {
        $matches = @($fixtures | Where-Object { $_.name -eq $fixtureName })
        if ($matches.Count -ne 1) {
            throw "GPU SDF compute parity is missing fixture '$fixtureName'"
        }
    }

    $requiredChecks = @(
        "gpu sdf compute API is present",
        "gpu sdf output grid matches chunk-plus-padding contract",
        "gpu sdf dispatch covers every output sample",
        "gpu sdf readback produces deterministic sample buffer",
        "cpu worldgen remains authoritative until parity passes",
        "gpu sdf integration remains disabled by default",
        "parity thresholds are explicit",
        "parity fixtures cover origin positive and negative chunks"
    )
    $checks = @($analysis.checks)
    foreach ($requiredCheck in $requiredChecks) {
        $matches = @($checks | Where-Object { $_.name -eq $requiredCheck })
        if ($matches.Count -ne 1) {
            throw "GPU SDF compute parity analysis is missing check '$requiredCheck'"
        }
        if (-not $matches[0].passed) {
            throw "GPU SDF compute parity check failed: $requiredCheck"
        }
    }
}

function Test-GpuSdfRuntimeToggleGate {
    $renderDir = "build/$BuildPreset/test-artifacts/render"
    $analysisPath = Join-Path $renderDir "gpu-sdf-runtime-parity.json"
    $cpuPath = Join-Path $renderDir "gpu-sdf-cpu.ppm"
    $gpuPath = Join-Path $renderDir "gpu-sdf-gpu.ppm"

    if (-not (Test-Path $analysisPath)) {
        throw "gpu SDF runtime toggle gate not yet implemented - missing $analysisPath (produced by task T-EF-29-gpu-sdf-runtime-toggle-gate)"
    }

    Assert-PpmArtifact $cpuPath
    Assert-PpmArtifact $gpuPath

    $analysis = Get-Content $analysisPath -Raw | ConvertFrom-Json
    if ($analysis.schema -ne "luminumbra.render.gpu_sdf_runtime_toggle.v1") {
        throw "Unexpected GPU SDF runtime toggle schema '$($analysis.schema)'"
    }
    if (-not $analysis.passed) {
        throw "GPU SDF runtime toggle analysis reported failure"
    }
    if ($analysis.runtime_toggle.source -ne "src/luminumbra_client/rendering/RenderPipeline.cpp") {
        throw "GPU SDF runtime toggle analysis must inspect RenderPipeline.cpp"
    }
    if ($analysis.runtime_toggle.header -ne "src/luminumbra_client/rendering/RenderPipeline.h") {
        throw "GPU SDF runtime toggle analysis must inspect RenderPipeline.h"
    }
    if ($analysis.runtime_toggle.entrypoint -ne "src/luminumbra_client/main_client.cpp") {
        throw "GPU SDF runtime toggle analysis must inspect main_client.cpp"
    }
    if ($analysis.runtime_toggle.setter_api -ne "set_gpu_sdf_runtime_enabled") {
        throw "GPU SDF runtime toggle analysis must require set_gpu_sdf_runtime_enabled"
    }
    if ($analysis.runtime_toggle.state_api -ne "get_gpu_sdf_runtime_toggle_state") {
        throw "GPU SDF runtime toggle analysis must require get_gpu_sdf_runtime_toggle_state"
    }
    if ($analysis.runtime_toggle.setup_api -ne "SetupGPUSDFIntegration") {
        throw "GPU SDF runtime toggle analysis must require SetupGPUSDFIntegration"
    }
    if ($analysis.runtime_toggle.opt_in_flag -ne "--enable-gpu-sdf-runtime") {
        throw "GPU SDF runtime toggle must use the explicit --enable-gpu-sdf-runtime opt-in flag"
    }
    if ($analysis.runtime_toggle.disabled_gate -ne "kEnableExperimentalGpuSdfIntegration") {
        throw "GPU SDF runtime toggle must retain the compile-time disabled gate"
    }
    if ($analysis.runtime_toggle.default_enabled -ne $false) {
        throw "GPU SDF runtime toggle must remain disabled by default"
    }
    if ($analysis.runtime_toggle.compile_time_gate_enabled -ne $false) {
        throw "GPU SDF compile-time parity gate must remain closed by default"
    }
    if ($analysis.runtime_toggle.runtime_requested_by_default -ne $false) {
        throw "GPU SDF runtime must not be requested by default"
    }
    if (-not $analysis.runtime_toggle.runtime_requires_explicit_opt_in) {
        throw "GPU SDF runtime toggle must require explicit opt-in"
    }
    if (-not $analysis.runtime_toggle.runtime_allowed_requires_compile_time_gate) {
        throw "GPU SDF runtime toggle must require the compile-time gate"
    }
    if (-not $analysis.runtime_toggle.runtime_allowed_requires_explicit_flag) {
        throw "GPU SDF runtime toggle must require the explicit runtime flag"
    }
    if ($analysis.runtime_toggle.callback_registered_by_default -ne $false) {
        throw "GPU SDF callback must not be registered by default"
    }
    if (-not $analysis.runtime_toggle.cpu_fallback_active_by_default) {
        throw "GPU SDF runtime toggle must keep the CPU fallback active by default"
    }
    if (-not $analysis.runtime_toggle.runtime_setter_present) {
        throw "GPU SDF runtime toggle analysis reports missing setter API"
    }
    if (-not $analysis.runtime_toggle.runtime_state_present) {
        throw "GPU SDF runtime toggle analysis reports missing state API"
    }
    if (-not $analysis.runtime_toggle.runtime_flag_present) {
        throw "GPU SDF runtime toggle analysis reports missing opt-in flag"
    }
    if (-not $analysis.runtime_toggle.main_wires_runtime_flag) {
        throw "GPU SDF runtime toggle analysis reports missing client-to-renderer wiring"
    }
    if (-not $analysis.runtime_toggle.setup_invoked_for_world) {
        throw "GPU SDF runtime toggle analysis reports missing world callback setup"
    }
    if (-not $analysis.runtime_toggle.runtime_gate_blocks_callback) {
        throw "GPU SDF runtime toggle must block callback registration while disabled"
    }
    if (-not $analysis.runtime_toggle.callback_state_tracked) {
        throw "GPU SDF runtime toggle must track callback registration state"
    }

    if ($analysis.parity.cpu_reference -ne "gpu-sdf-cpu.ppm") {
        throw "GPU SDF runtime parity must reference gpu-sdf-cpu.ppm"
    }
    if ($analysis.parity.gpu_candidate -ne "gpu-sdf-gpu.ppm") {
        throw "GPU SDF runtime parity must reference gpu-sdf-gpu.ppm"
    }
    if ($analysis.parity.sample_grid -ne "17x17") {
        throw "GPU SDF runtime parity sample grid must be 17x17"
    }
    if ([int64]$analysis.parity.sample_count -ne 289) {
        throw "GPU SDF runtime parity sample count must be 289"
    }
    if ([string]::IsNullOrWhiteSpace($analysis.parity.cpu_checksum) -or
        $analysis.parity.cpu_checksum -ne $analysis.parity.gpu_checksum) {
        throw "GPU SDF runtime parity checksums must be present and equal"
    }
    if ([int64]$analysis.parity.max_pixel_delta -ne 0) {
        throw "GPU SDF runtime parity max_pixel_delta must be zero while runtime gate is closed"
    }
    if ([double]$analysis.parity.mean_pixel_delta -ne 0.0) {
        throw "GPU SDF runtime parity mean_pixel_delta must be zero while runtime gate is closed"
    }
    if (-not $analysis.parity.images_match) {
        throw "GPU SDF runtime parity images must match"
    }

    $requiredChecks = @(
        "gpu sdf runtime setter API is present",
        "gpu sdf runtime state API is present",
        "gpu sdf runtime opt-in flag is parsed",
        "client wires opt-in flag into render pipeline",
        "world creation invokes gpu sdf callback setup",
        "compile-time parity gate remains closed by default",
        "runtime gate blocks callback unless explicitly allowed",
        "runtime callback state is tracked",
        "cpu and gpu runtime parity artifacts match"
    )
    $checks = @($analysis.checks)
    foreach ($requiredCheck in $requiredChecks) {
        $matches = @($checks | Where-Object { $_.name -eq $requiredCheck })
        if ($matches.Count -ne 1) {
            throw "GPU SDF runtime toggle analysis is missing check '$requiredCheck'"
        }
        if (-not $matches[0].passed) {
            throw "GPU SDF runtime toggle check failed: $requiredCheck"
        }
    }
}

function Test-ChunkCollisionLifecycle {
    $artifactDir = "build/$BuildPreset/test-artifacts/runtime/chunk-collision-lifecycle"
    $analysisPath = Join-Path $artifactDir "chunk-collision-lifecycle.json"
    $testScriptPath = "test/physics/chunk-collision-lifecycle.ps1"

    if (-not (Test-Path $testScriptPath)) {
        throw "chunk collision lifecycle gate not yet implemented - missing $testScriptPath (produced by task T-EF-12-physics-collision-lifecycle-gate)"
    }

    & $testScriptPath -BuildPreset $BuildPreset
    if ($LASTEXITCODE -ne 0) {
        exit $LASTEXITCODE
    }

    if (-not (Test-Path $analysisPath)) {
        throw "chunk collision lifecycle gate not yet implemented - missing $analysisPath (produced by task T-EF-12-physics-collision-lifecycle-gate)"
    }

    $analysis = Get-Content $analysisPath -Raw | ConvertFrom-Json
    if ($analysis.schema -ne "luminumbra.physics.chunk_collision_lifecycle.v1") {
        throw "Unexpected chunk collision lifecycle schema '$($analysis.schema)'"
    }
    if (-not $analysis.passed) {
        throw "Chunk collision lifecycle analysis reported failure"
    }
    if ($analysis.world_system.source -ne "src/luminumbra_common/systems/SHIELD_WorldSystem.cpp") {
        throw "Chunk collision lifecycle analysis must inspect SHIELD_WorldSystem.cpp"
    }
    if ($analysis.world_system.replacement_helper -ne "replace_chunk_collision") {
        throw "Chunk collision lifecycle analysis must require replace_chunk_collision"
    }
    if ([int64]$analysis.world_system.direct_add_chunk_collision_calls -ne 0) {
        throw "Chunk collision lifecycle must not leave direct pointer add_chunk_collision calls"
    }
    if ([int64]$analysis.world_system.helper_add_chunk_collision_calls -ne 1) {
        throw "Chunk collision lifecycle must centralize add_chunk_collision in one helper"
    }

    $requiredChecks = @(
        "replace helper removes stale collision before add",
        "runtime update uses lifecycle replacement helper",
        "initial horizon collision uses lifecycle replacement helper",
        "chunk unload removes collision before erasing chunk",
        "clear world removes collisions before clearing chunks",
        "terrain mesh rebuild invalidates collision flag",
        "all collision adds flow through lifecycle replacement"
    )
    $checks = @($analysis.checks)
    foreach ($requiredCheck in $requiredChecks) {
        $matches = @($checks | Where-Object { $_.name -eq $requiredCheck })
        if ($matches.Count -ne 1) {
            throw "Chunk collision lifecycle analysis is missing check '$requiredCheck'"
        }
        if (-not $matches[0].passed) {
            throw "Chunk collision lifecycle check failed: $requiredCheck"
        }
    }
}

function Test-PhysicsReplay {
    $artifactDir = "build/$BuildPreset/test-artifacts/runtime/physics-replay"
    $analysisPath = Join-Path $artifactDir "physics-replay-endstate.json"
    $testScriptPath = "test/physics/physics-replay.ps1"

    if (-not (Test-Path $testScriptPath)) {
        throw "physics replay gate not yet implemented - missing $testScriptPath (produced by task T-EF-13-physics-replay-gate)"
    }

    & $testScriptPath -BuildPreset $BuildPreset
    if ($LASTEXITCODE -ne 0) {
        exit $LASTEXITCODE
    }

    if (-not (Test-Path $analysisPath)) {
        throw "physics replay gate not yet implemented - missing $analysisPath (produced by task T-EF-13-physics-replay-gate)"
    }

    $analysis = Get-Content $analysisPath -Raw | ConvertFrom-Json
    if ($analysis.schema -ne "luminumbra.physics.replay_endstate.v1") {
        throw "Unexpected physics replay schema '$($analysis.schema)'"
    }
    if (-not $analysis.passed) {
        throw "Physics replay analysis reported failure"
    }
    if ($analysis.controller.input_api -ne "ApplyReplayInput") {
        throw "Physics replay analysis must require PlayerController::ApplyReplayInput"
    }
    if ($analysis.controller.snapshot_api -ne "CaptureReplaySnapshot") {
        throw "Physics replay analysis must require PlayerController::CaptureReplaySnapshot"
    }
    if ($analysis.controller.frame_counter_api -ne "ResetReplayFrameCounter") {
        throw "Physics replay analysis must require PlayerController::ResetReplayFrameCounter"
    }
    if ($analysis.physics.include_bridge -ne "../systems/PhysicsSystem.h") {
        throw "Physics replay analysis must preserve the assigned PhysicsSystem include bridge"
    }
    if ([int64]$analysis.replay.frame_count -lt 10) {
        throw "Physics replay must execute at least 10 deterministic frames"
    }
    if ($analysis.replay.fixed_delta_seconds -ne 0.016666667) {
        throw "Physics replay must use the fixed 60Hz replay timestep"
    }
    if ([string]::IsNullOrWhiteSpace($analysis.replay.checksum)) {
        throw "Physics replay analysis is missing the replay checksum"
    }
    if ($null -eq $analysis.replay.endstate.position -or @($analysis.replay.endstate.position).Count -ne 3) {
        throw "Physics replay endstate must include a 3D position"
    }
    if ($null -eq $analysis.replay.endstate.velocity -or @($analysis.replay.endstate.velocity).Count -ne 3) {
        throw "Physics replay endstate must include a 3D velocity"
    }

    $requiredChecks = @(
        "replay input frame contract declared",
        "replay snapshot contract declared",
        "replay frame application api declared",
        "replay frame application api implemented",
        "live update routes through replay api",
        "replay snapshot captures endstate",
        "replay frame counter reset api implemented",
        "walking reducer avoids live sprint polling",
        "noclip reducer avoids live sprint polling",
        "physics include bridge remains intact"
    )
    $checks = @($analysis.checks)
    foreach ($requiredCheck in $requiredChecks) {
        $matches = @($checks | Where-Object { $_.name -eq $requiredCheck })
        if ($matches.Count -ne 1) {
            throw "Physics replay analysis is missing check '$requiredCheck'"
        }
        if (-not $matches[0].passed) {
            throw "Physics replay check failed: $requiredCheck"
        }
    }
}

function Test-AudioNullTelemetry {
    $artifactDir = "build/$BuildPreset/test-artifacts/audio"
    $analysisPath = Join-Path $artifactDir "audio-telemetry.json"
    $testScriptPath = "test/audio/audio-null-telemetry.ps1"

    if (-not (Test-Path $testScriptPath)) {
        throw "audio null telemetry gate not yet implemented - missing $testScriptPath (produced by task T-EF-14-audio-null-telemetry-gate)"
    }

    & $testScriptPath -BuildPreset $BuildPreset
    if ($LASTEXITCODE -ne 0) {
        exit $LASTEXITCODE
    }

    if (-not (Test-Path $analysisPath)) {
        throw "audio null telemetry gate not yet implemented - missing $analysisPath (produced by task T-EF-14-audio-null-telemetry-gate)"
    }

    $analysis = Get-Content $analysisPath -Raw | ConvertFrom-Json
    if ($analysis.schema -ne "luminumbra.audio.null_telemetry.v1") {
        throw "Unexpected audio telemetry schema '$($analysis.schema)'"
    }
    if (-not $analysis.passed) {
        throw "Audio null telemetry analysis reported failure"
    }
    if ($analysis.activation.flag -ne "--no-audio") {
        throw "Audio null telemetry must gate the --no-audio launch flag"
    }
    if (-not $analysis.activation.selected) {
        throw "Audio null telemetry must report the null manager selected"
    }
    if ($analysis.activation.manager_type -ne "NullAudioManager") {
        throw "Audio null telemetry manager type must be NullAudioManager"
    }
    if ($analysis.activation.hardware_backend_initialized) {
        throw "Audio null telemetry must prove no hardware backend initialized"
    }
    if ($analysis.activation.bank_files_touched) {
        throw "Audio null telemetry must not touch bank files in null mode"
    }

    $requiredChecks = @(
        "null manager lives in audio module",
        "--no-audio selects null manager",
        "null manager emits telemetry schema",
        "audio playback routes through null manager",
        "audio telemetry artifact is written"
    )
    $checks = @($analysis.checks)
    foreach ($requiredCheck in $requiredChecks) {
        $matches = @($checks | Where-Object { $_.name -eq $requiredCheck })
        if ($matches.Count -ne 1) {
            throw "Audio null telemetry analysis is missing check '$requiredCheck'"
        }
        if (-not $matches[0].passed) {
            throw "Audio null telemetry check failed: $requiredCheck"
        }
    }
}

function Test-AudioHandleApplication {
    $artifactDir = "build/$BuildPreset/test-artifacts/audio"
    $analysisPath = Join-Path $artifactDir "audio-handle-application.json"
    $testScriptPath = "test/audio/audio-handle-application.ps1"

    if (-not (Test-Path $testScriptPath)) {
        throw "audio handle application gate not yet implemented - missing $testScriptPath (produced by task T-EF-15-audio-handle-application-gate)"
    }

    & $testScriptPath -BuildPreset $BuildPreset
    if ($LASTEXITCODE -ne 0) {
        exit $LASTEXITCODE
    }

    if (-not (Test-Path $analysisPath)) {
        throw "audio handle application gate not yet implemented - missing $analysisPath (produced by task T-EF-15-audio-handle-application-gate)"
    }

    $analysis = Get-Content $analysisPath -Raw | ConvertFrom-Json
    if ($analysis.schema -ne "luminumbra.audio.handle_application.v1") {
        throw "Unexpected audio handle application schema '$($analysis.schema)'"
    }
    if (-not $analysis.passed) {
        throw "Audio handle application analysis reported failure"
    }
    if ($analysis.manager.source -ne "src/luminumbra_client/audio/MiniaudioManager.cpp") {
        throw "Audio handle application analysis must inspect MiniaudioManager.cpp"
    }
    if ($analysis.manager.interface -ne "src/luminumbra_client/audio/IAudioManager.h") {
        throw "Audio handle application analysis must inspect IAudioManager.h"
    }
    if ($analysis.handle_application.playback_handle_api -ne "PlayEvent") {
        throw "Audio handle application analysis must require PlayEvent handle issuance"
    }
    if (-not $analysis.handle_application.stopped_handles_removed) {
        throw "Audio handle application must remove immediately stopped handles"
    }
    if (-not $analysis.handle_application.spatial_cluster_updated) {
        throw "Audio handle application must keep spatial cluster state synchronized"
    }
    if (-not $analysis.handle_application.invalid_handles_rejected) {
        throw "Audio handle application must reject unknown handles"
    }
    foreach ($parameter in @("volume", "pitch")) {
        if (@($analysis.handle_application.supported_parameters) -notcontains $parameter) {
            throw "Audio handle application must support '$parameter' parameter application"
        }
    }

    $requiredChecks = @(
        "interface declares handle mutators",
        "miniaudio manager stores playable handles",
        "stop applies handle to active sound",
        "immediate stop releases active handle",
        "stop removes spatial cluster source",
        "position applies handle to ma_sound",
        "volume applies handle to ma_sound",
        "parameter applies supported miniaudio controls",
        "unknown handles are rejected"
    )
    $checks = @($analysis.checks)
    foreach ($requiredCheck in $requiredChecks) {
        $matches = @($checks | Where-Object { $_.name -eq $requiredCheck })
        if ($matches.Count -ne 1) {
            throw "Audio handle application analysis is missing check '$requiredCheck'"
        }
        if (-not $matches[0].passed) {
            throw "Audio handle application check failed: $requiredCheck"
        }
    }
}

function Test-UiTestBaseline {
    $artifactRoot = "build/$BuildPreset/test-artifacts"
    $ctestPath = Join-Path $artifactRoot "testing/ctest_manifest.json"
    $coveragePath = Join-Path $artifactRoot "coverage/coverage_summary.json"
    $uiScreenshotsPath = Join-Path $artifactRoot "ui/ui_screenshots.json"

    $ctest = Read-JsonArtifact -Path $ctestPath -Schema "luminumbra.testing.ctest_manifest.v1"
    Assert-ArtifactPassed -Artifact $ctest -Name "CTest manifest"
    if ($ctest.build_preset -ne $BuildPreset) {
        throw "CTest manifest build_preset '$($ctest.build_preset)' does not match '$BuildPreset'"
    }
    if ([int64]$ctest.minimum_test_executables -lt 10) {
        throw "CTest manifest must require at least 10 test executables"
    }
    if ([int64]$ctest.minimum_registered_tests -lt 10) {
        throw "CTest manifest must require at least 10 registered tests"
    }
    foreach ($executable in @(
        "world_generation_test",
        "sdf_gpu_cpu_parity_test",
        "worldgen_layer_snapshot_test",
        "asset_processor_round_trip_test",
        "common_tests",
        "render_smoke_test",
        "render_capture_test",
        "ui_smoke_test",
        "initial_world_loading_perf_test",
        "runtime_world_visual_validation_test"
    )) {
        Assert-ArrayContains -Values $ctest.required_executables -Needle $executable -Description "CTest manifest required_executables"
    }
    Assert-ArrayContains -Values $ctest.excluded_patterns -Needle "_NOT_BUILT$" -Description "CTest manifest excluded_patterns"
    foreach ($uiTest in @(
        "UiSmokeTest.AuthoredRmlDocumentsLoadAndExposeRequiredElements",
        "UiSmokeTest.UiStateNavigationMaintainsDocumentAndGameState",
        "UiSmokeTest.AuthoredMenuInteractionsNavigateAndInvokeCallbacks"
    )) {
        Assert-ArrayContains -Values $ctest.required_ui_tests -Needle $uiTest -Description "CTest manifest required_ui_tests"
    }

    $coverage = Read-JsonArtifact -Path $coveragePath -Schema "luminumbra.coverage_summary.v1"
    Assert-ArtifactPassed -Artifact $coverage -Name "Coverage summary"
    if ($coverage.build_preset -ne $BuildPreset) {
        throw "Coverage summary build_preset '$($coverage.build_preset)' does not match '$BuildPreset'"
    }
    if ($coverage.coverage_kind -ne "contract_baseline") {
        throw "Coverage summary must declare contract_baseline coverage kind"
    }
    if ($coverage.line_coverage.available) {
        if ([double]$coverage.line_coverage.percent -lt [double]$coverage.line_coverage.minimum_required_percent) {
            throw "Line coverage $($coverage.line_coverage.percent) is below required $($coverage.line_coverage.minimum_required_percent)"
        }
    } elseif ([string]::IsNullOrWhiteSpace($coverage.line_coverage.reason)) {
        throw "Coverage summary must explain unavailable line coverage"
    }
    if ([int64]$coverage.contract_coverage.covered_subsystem_count -lt [int64]$coverage.contract_coverage.minimum_subsystems) {
        throw "Coverage summary subsystem coverage is below baseline"
    }
    foreach ($subsystem in @("common", "rendering", "ui", "physics", "audio", "performance", "tools")) {
        Assert-ArrayContains -Values $coverage.contract_coverage.covered_subsystems -Needle $subsystem -Description "Coverage summary covered_subsystems"
    }
    foreach ($artifact in @("testing/ctest_manifest.json", "ui/ui_smoke.json", "ui/ui_interactions.json", "ui/ui_screenshots.json")) {
        Assert-ArrayContains -Values $coverage.required_artifacts -Needle $artifact -Description "Coverage summary required_artifacts"
    }

    $uiScreenshots = Read-JsonArtifact -Path $uiScreenshotsPath -Schema "luminumbra.ui_screenshots.v1"
    Assert-ArtifactPassed -Artifact $uiScreenshots -Name "UI screenshots"
    if ($uiScreenshots.build_preset -ne $BuildPreset) {
        throw "UI screenshots build_preset '$($uiScreenshots.build_preset)' does not match '$BuildPreset'"
    }
    if ([int64]$uiScreenshots.capture_window.width -ne 800 -or [int64]$uiScreenshots.capture_window.height -ne 600) {
        throw "UI screenshots baseline must use the 800x600 hidden UI smoke window"
    }
    $screenshots = @($uiScreenshots.screenshots)
    if ([int64]$uiScreenshots.screenshot_count -ne $screenshots.Count) {
        throw "UI screenshots screenshot_count does not match screenshots array"
    }
    if ($screenshots.Count -lt 3) {
        throw "UI screenshots baseline must cover at least three authored menu views"
    }
    foreach ($view in @("main_menu", "world_creation", "world_selection")) {
        $matches = @($screenshots | Where-Object { $_.view -eq $view })
        if ($matches.Count -ne 1) {
            throw "UI screenshots baseline is missing view '$view'"
        }
        if ([string]::IsNullOrWhiteSpace($matches[0].document)) {
            throw "UI screenshots view '$view' is missing document"
        }
        if ([string]::IsNullOrWhiteSpace($matches[0].expected_file)) {
            throw "UI screenshots view '$view' is missing expected_file"
        }
        if (@($matches[0].required_element_ids).Count -lt 4) {
            throw "UI screenshots view '$view' does not list enough required UI elements"
        }
    }
    foreach ($artifact in @("ui/ui_smoke.json", "ui/ui_interactions.json")) {
        Assert-ArrayContains -Values $uiScreenshots.required_artifacts -Needle $artifact -Description "UI screenshots required_artifacts"
    }
}

function Test-SimulationEventBusOrderGate {
    $artifactDir = "build/$BuildPreset/test-artifacts/simulation"
    $analysisPath = Join-Path $artifactDir "eventbus-replay.json"
    $testScriptPath = "test/simulation/eventbus-order-gate.ps1"

    if (-not (Test-Path $testScriptPath)) {
        throw "simulation event bus order gate not yet implemented - missing $testScriptPath (produced by task T-EF-18-simulation-eventbus-order-gate)"
    }

    & $testScriptPath -BuildPreset $BuildPreset
    if (-not $? ) {
        exit 1
    }
    if ($null -ne $LASTEXITCODE -and $LASTEXITCODE -ne 0) {
        exit $LASTEXITCODE
    }

    if (-not (Test-Path $analysisPath)) {
        throw "simulation event bus order gate not yet implemented - missing $analysisPath (produced by task T-EF-18-simulation-eventbus-order-gate)"
    }

    $analysis = Get-Content $analysisPath -Raw | ConvertFrom-Json
    if ($analysis.schema -ne "luminumbra.simulation.eventbus_replay.v1") {
        throw "Unexpected simulation event bus replay schema '$($analysis.schema)'"
    }
    if (-not $analysis.passed) {
        throw "Simulation event bus replay analysis reported failure"
    }
    if ($analysis.event_bus.source -ne "src/luminumbra_common/simulation/SimulationEventBus.cpp") {
        throw "Simulation event bus replay analysis must inspect SimulationEventBus.cpp"
    }
    if ($analysis.event_bus.header -ne "src/luminumbra_common/simulation/SimulationEventBus.h") {
        throw "Simulation event bus replay analysis must inspect SimulationEventBus.h"
    }
    if ($analysis.event_bus.order_contract -ne "tick_then_lane_then_sequence") {
        throw "Simulation event bus replay must declare the tick/lane/sequence ordering contract"
    }
    if (-not $analysis.event_bus.same_tick_fifo) {
        throw "Simulation event bus replay must preserve FIFO order within the same tick and lane"
    }
    if (-not $analysis.event_bus.future_ticks_queued) {
        throw "Simulation event bus replay must prove future tick events stay queued until eligible"
    }
    if ([int64]$analysis.replay.frame_count -lt 3) {
        throw "Simulation event bus replay must cover at least three simulation ticks"
    }
    if ([int64]$analysis.replay.delivered_event_count -lt 7) {
        throw "Simulation event bus replay must deliver the deterministic fixture events"
    }
    if ([string]::IsNullOrWhiteSpace($analysis.replay.checksum)) {
        throw "Simulation event bus replay analysis is missing the replay checksum"
    }

    $delivered = @($analysis.replay.delivered_events)
    if ([int64]$analysis.replay.delivered_event_count -ne $delivered.Count) {
        throw "Simulation event bus replay delivered_event_count does not match delivered_events array"
    }
    $expectedOrder = @(
        "1|-1|3|physics.impulse|crate:push",
        "1|0|1|input.command|player:move",
        "1|0|2|script.trigger|door:open",
        "2|-1|6|ai.intent|npc-2:wait",
        "2|0|0|ai.intent|npc-1:turn",
        "2|0|5|script.trigger|torch:light",
        "3|0|4|audio.event|stone:slide"
    )
    $actualOrder = @($delivered | ForEach-Object { "$($_.tick)|$($_.lane)|$($_.sequence)|$($_.topic)|$($_.payload)" })
    if (($actualOrder -join "`n") -ne ($expectedOrder -join "`n")) {
        throw "Simulation event bus replay delivered order does not match the deterministic fixture"
    }

    $requiredChecks = @(
        "ordered bus assigns monotonic sequence ids",
        "same tick delivery is stable by lane then sequence",
        "future tick events remain queued until eligible",
        "replay emits deterministic checksum",
        "gate artifact records delivered order"
    )
    $checks = @($analysis.checks)
    foreach ($requiredCheck in $requiredChecks) {
        $matches = @($checks | Where-Object { $_.name -eq $requiredCheck })
        if ($matches.Count -ne 1) {
            throw "Simulation event bus replay analysis is missing check '$requiredCheck'"
        }
        if (-not $matches[0].passed) {
            throw "Simulation event bus replay check failed: $requiredCheck"
        }
    }
}

function Test-LuaApiManifestGate {
    $artifactDir = "build/$BuildPreset/test-artifacts/scripting"
    $analysisPath = Join-Path $artifactDir "lua-api-manifest.json"
    $testScriptPath = "test/scripting/lua-api-manifest-gate.ps1"

    if (-not (Test-Path $testScriptPath)) {
        throw "lua api manifest gate not yet implemented - missing $testScriptPath (produced by task T-EF-19-lua-api-manifest-gate)"
    }

    & $testScriptPath -BuildPreset $BuildPreset
    if (-not $?) {
        exit 1
    }
    if ($null -ne $LASTEXITCODE -and $LASTEXITCODE -ne 0) {
        exit $LASTEXITCODE
    }

    if (-not (Test-Path $analysisPath)) {
        throw "lua api manifest gate not yet implemented - missing $analysisPath (produced by task T-EF-19-lua-api-manifest-gate)"
    }

    $analysis = Get-Content $analysisPath -Raw | ConvertFrom-Json
    if ($analysis.schema -ne "luminumbra.scripting.lua_api_manifest.v1") {
        throw "Unexpected lua api manifest schema '$($analysis.schema)'"
    }
    if (-not $analysis.passed) {
        throw "Lua API manifest analysis reported failure"
    }
    if ($analysis.build_preset -ne $BuildPreset) {
        throw "Lua API manifest build_preset '$($analysis.build_preset)' does not match '$BuildPreset'"
    }
    if ($analysis.manifest.source -ne "src/luminumbra_common/scripting/LuaApiManifest.cpp") {
        throw "Lua API manifest analysis must inspect LuaApiManifest.cpp"
    }
    if ($analysis.manifest.header -ne "src/luminumbra_common/scripting/LuaApiManifest.h") {
        throw "Lua API manifest analysis must inspect LuaApiManifest.h"
    }
    if ($analysis.manifest.lua_state_header -ne "src/luminumbra_common/scripting/LuaState.h") {
        throw "Lua API manifest analysis must inspect LuaState.h"
    }
    if ($analysis.manifest.serializer -ne "SerializeLuaApiManifestJson") {
        throw "Lua API manifest analysis must require SerializeLuaApiManifestJson"
    }
    if ($analysis.manifest.validation_api -ne "LuaApiManifestMeetsBaseline") {
        throw "Lua API manifest analysis must require LuaApiManifestMeetsBaseline"
    }
    if ($analysis.manifest.deterministic_order -ne "module_then_name") {
        throw "Lua API manifest must declare module_then_name deterministic ordering"
    }
    if ([int64]$analysis.manifest.entry_count -lt 9) {
        throw "Lua API manifest must cover the baseline scripting API entries"
    }

    foreach ($module in @("core", "entity", "simulation", "time", "world")) {
        Assert-ArrayContains -Values $analysis.manifest.required_modules -Needle $module -Description "Lua API manifest required_modules"
    }
    foreach ($entry in @(
        "core.log",
        "core.version",
        "entity.destroy",
        "entity.spawn",
        "simulation.emit_event",
        "simulation.subscribe",
        "time.delta_seconds",
        "world.get_block",
        "world.set_block"
    )) {
        Assert-ArrayContains -Values $analysis.manifest.required_entries -Needle $entry -Description "Lua API manifest required_entries"
    }

    $requiredChecks = @(
        "manifest schema declared",
        "manifest header declares API descriptors",
        "LuaState exposes manifest API",
        "manifest source lists required entries",
        "manifest entries are wired into common sources",
        "manifest gate test is wired into test sources",
        "manifest serializer emits deterministic order contract"
    )
    $checks = @($analysis.checks)
    foreach ($requiredCheck in $requiredChecks) {
        $matches = @($checks | Where-Object { $_.name -eq $requiredCheck })
        if ($matches.Count -ne 1) {
            throw "Lua API manifest analysis is missing check '$requiredCheck'"
        }
        if (-not $matches[0].passed) {
            throw "Lua API manifest check failed: $requiredCheck"
        }
    }
}

function Test-AethericDiffusionGate {
    $artifactDir = "build/$BuildPreset/test-artifacts/aetheric"
    $analysisPath = Join-Path $artifactDir "aetheric-field-diffusion.json"
    $testScriptPath = "test/aetheric/aetheric-field-diffusion.ps1"

    if (-not (Test-Path $testScriptPath)) {
        throw "aetheric field diffusion gate not yet implemented - missing $testScriptPath (produced by task T-EF-20-aetheric-diffusion-gate)"
    }

    & $testScriptPath -BuildPreset $BuildPreset
    if (-not $?) {
        exit 1
    }
    if ($null -ne $LASTEXITCODE -and $LASTEXITCODE -ne 0) {
        exit $LASTEXITCODE
    }

    if (-not (Test-Path $analysisPath)) {
        throw "aetheric field diffusion gate not yet implemented - missing $analysisPath (produced by task T-EF-20-aetheric-diffusion-gate)"
    }

    $analysis = Get-Content $analysisPath -Raw | ConvertFrom-Json
    if ($analysis.schema -ne "luminumbra.aetheric.field_diffusion.v1") {
        throw "Unexpected aetheric field diffusion schema '$($analysis.schema)'"
    }
    if (-not $analysis.passed) {
        throw "Aetheric field diffusion analysis reported failure"
    }
    if ($analysis.build_preset -ne $BuildPreset) {
        throw "Aetheric field diffusion build_preset '$($analysis.build_preset)' does not match '$BuildPreset'"
    }
    if ($analysis.field.source -ne "src/luminumbra_common/aetheric/AethericFieldDiffusion.cpp") {
        throw "Aetheric field diffusion analysis must inspect AethericFieldDiffusion.cpp"
    }
    if ($analysis.field.header -ne "src/luminumbra_common/aetheric/AethericFieldDiffusion.h") {
        throw "Aetheric field diffusion analysis must inspect AethericFieldDiffusion.h"
    }
    if ($analysis.diffusion.solver -ne "conservative_pairwise_flux") {
        throw "Aetheric field diffusion must use the conservative pairwise flux solver"
    }
    if ($analysis.diffusion.order_contract -ne "deterministic_row_major_edges") {
        throw "Aetheric field diffusion must declare deterministic row-major edge ordering"
    }
    if ([int64]$analysis.diffusion.iterations -lt 8) {
        throw "Aetheric field diffusion fixture must cover at least eight iterations"
    }
    if (-not $analysis.diffusion.stable) {
        throw "Aetheric field diffusion fixture reported an unstable solve"
    }
    if ([double]$analysis.diffusion.conservation_error -gt 1.0e-9) {
        throw "Aetheric field diffusion conservation error exceeded tolerance"
    }

    $requiredChecks = @(
        "field diffusion header declares gate API",
        "field diffusion source conserves pairwise flux",
        "fixture declares deterministic diffusion order",
        "diffusion gate validates conservation tolerance",
        "aetheric source is wired into common sources",
        "aetheric gate test is wired into test sources",
        "gate test exercises serializer and fixture"
    )
    $checks = @($analysis.checks)
    foreach ($requiredCheck in $requiredChecks) {
        $matches = @($checks | Where-Object { $_.name -eq $requiredCheck })
        if ($matches.Count -ne 1) {
            throw "Aetheric field diffusion analysis is missing check '$requiredCheck'"
        }
        if (-not $matches[0].passed) {
            throw "Aetheric field diffusion check failed: $requiredCheck"
        }
    }
}

function Test-InstinctPlannerGate {
    $artifactDir = "build/$BuildPreset/test-artifacts/ai"
    $analysisPath = Join-Path $artifactDir "instinct-grovestrider-hunger.json"
    $testScriptPath = "test/ai/instinct-planner-gate.ps1"

    if (-not (Test-Path $testScriptPath)) {
        throw "instinct planner gate not yet implemented - missing $testScriptPath (produced by task T-EF-21-instinct-planner-gate)"
    }

    & $testScriptPath -BuildPreset $BuildPreset
    if (-not $?) {
        exit 1
    }
    if ($null -ne $LASTEXITCODE -and $LASTEXITCODE -ne 0) {
        exit $LASTEXITCODE
    }

    if (-not (Test-Path $analysisPath)) {
        throw "instinct planner gate not yet implemented - missing $analysisPath (produced by task T-EF-21-instinct-planner-gate)"
    }

    $analysis = Get-Content $analysisPath -Raw | ConvertFrom-Json
    if ($analysis.schema -ne "luminumbra.ai.instinct_planner.v1") {
        throw "Unexpected instinct planner schema '$($analysis.schema)'"
    }
    if (-not $analysis.passed) {
        throw "Instinct planner analysis reported failure"
    }
    if ($analysis.build_preset -ne $BuildPreset) {
        throw "Instinct planner build_preset '$($analysis.build_preset)' does not match '$BuildPreset'"
    }
    if ($analysis.planner.source -ne "src/luminumbra_common/ai/InstinctPlanner.cpp") {
        throw "Instinct planner analysis must inspect InstinctPlanner.cpp"
    }
    if ($analysis.planner.header -ne "src/luminumbra_common/ai/InstinctPlanner.h") {
        throw "Instinct planner analysis must inspect InstinctPlanner.h"
    }
    if ($analysis.planner.serializer -ne "SerializeInstinctPlanJson") {
        throw "Instinct planner analysis must require SerializeInstinctPlanJson"
    }
    if ($analysis.planner.validation_api -ne "InstinctPlannerMeetsBaseline") {
        throw "Instinct planner analysis must require InstinctPlannerMeetsBaseline"
    }
    if ($analysis.planner.decision_contract -ne "deterministic_priority_then_cost") {
        throw "Instinct planner must declare deterministic priority/cost ordering"
    }
    if ($analysis.fixture.archetype -ne "grovestrider") {
        throw "Instinct planner fixture must cover the grovestrider archetype"
    }
    if ($analysis.fixture.dominant_need -ne "hunger") {
        throw "Instinct planner fixture must use hunger as the dominant need"
    }
    if ([double]$analysis.fixture.hunger_pressure -lt 0.9) {
        throw "Instinct planner hunger fixture must apply high hunger pressure"
    }
    if ($analysis.fixture.selected_action -ne "forage") {
        throw "Instinct planner hunger fixture must select the forage action"
    }
    if ($analysis.fixture.selected_target -ne "mossberry_grove") {
        throw "Instinct planner hunger fixture must target mossberry_grove"
    }
    if ([int64]$analysis.fixture.candidate_count -lt 4) {
        throw "Instinct planner fixture must rank at least four candidates"
    }
    if ([string]::IsNullOrWhiteSpace($analysis.fixture.checksum)) {
        throw "Instinct planner analysis is missing the deterministic checksum"
    }

    $candidates = @($analysis.candidates)
    if ([int64]$analysis.fixture.candidate_count -ne $candidates.Count) {
        throw "Instinct planner candidate_count does not match candidates array"
    }
    if ($candidates[0].rank -ne 1 -or $candidates[0].need -ne "hunger" -or $candidates[0].action -ne "forage") {
        throw "Instinct planner top-ranked candidate must be hunger forage"
    }
    foreach ($need in @("hunger", "safety", "curiosity", "fatigue")) {
        Assert-ArrayContains -Values $analysis.fixture.required_needs -Needle $need -Description "Instinct planner fixture required_needs"
    }

    $requiredChecks = @(
        "instinct planner header declares gate API",
        "instinct planner source ranks needs deterministically",
        "grovestrider hunger fixture selects forage intent",
        "planner serializer emits deterministic candidates",
        "ai source is wired into common sources",
        "ai gate test is wired into test sources",
        "gate test exercises serializer and fixture"
    )
    $checks = @($analysis.checks)
    foreach ($requiredCheck in $requiredChecks) {
        $matches = @($checks | Where-Object { $_.name -eq $requiredCheck })
        if ($matches.Count -ne 1) {
            throw "Instinct planner analysis is missing check '$requiredCheck'"
        }
        if (-not $matches[0].passed) {
            throw "Instinct planner check failed: $requiredCheck"
        }
    }
}

function Test-PersistenceRoundtripGate {
    $artifactDir = "build/$BuildPreset/test-artifacts/persistence"
    $analysisPath = Join-Path $artifactDir "world-persistence-roundtrip.json"
    $testScriptPath = "test/persistence/world-persistence-roundtrip.ps1"

    if (-not (Test-Path $testScriptPath)) {
        throw "persistence roundtrip gate not yet implemented - missing $testScriptPath (produced by task T-EF-23-persistence-roundtrip-gate)"
    }

    & $testScriptPath -BuildPreset $BuildPreset
    if (-not $?) {
        exit 1
    }
    if ($null -ne $LASTEXITCODE -and $LASTEXITCODE -ne 0) {
        exit $LASTEXITCODE
    }

    if (-not (Test-Path $analysisPath)) {
        throw "persistence roundtrip gate not yet implemented - missing $analysisPath (produced by task T-EF-23-persistence-roundtrip-gate)"
    }

    $analysis = Get-Content $analysisPath -Raw | ConvertFrom-Json
    if ($analysis.schema -ne "luminumbra.persistence.world_roundtrip.v1") {
        throw "Unexpected persistence roundtrip schema '$($analysis.schema)'"
    }
    if (-not $analysis.passed) {
        throw "Persistence roundtrip analysis reported failure"
    }
    if ($analysis.build_preset -ne $BuildPreset) {
        throw "Persistence roundtrip build_preset '$($analysis.build_preset)' does not match '$BuildPreset'"
    }
    if ($analysis.persistence.source -ne "src/luminumbra_common/persistence/WorldPersistenceRoundtrip.cpp") {
        throw "Persistence roundtrip analysis must inspect WorldPersistenceRoundtrip.cpp"
    }
    if ($analysis.persistence.header -ne "src/luminumbra_common/persistence/WorldPersistenceRoundtrip.h") {
        throw "Persistence roundtrip analysis must inspect WorldPersistenceRoundtrip.h"
    }
    if ($analysis.persistence.serializer -ne "SerializeWorldStreamingStateSnapshotJson") {
        throw "Persistence roundtrip analysis must require SerializeWorldStreamingStateSnapshotJson"
    }
    if ($analysis.persistence.loader -ne "LoadWorldStreamingStateSnapshotJson") {
        throw "Persistence roundtrip analysis must require LoadWorldStreamingStateSnapshotJson"
    }
    if ($analysis.persistence.validation_api -ne "WorldPersistenceRoundtripMeetsBaseline") {
        throw "Persistence roundtrip analysis must require WorldPersistenceRoundtripMeetsBaseline"
    }
    if ($analysis.persistence.order_contract -ne "chunk_id_ascending") {
        throw "Persistence roundtrip must declare chunk_id_ascending deterministic ordering"
    }
    if ([int64]$analysis.persistence.persisted_field_count -lt 20) {
        throw "Persistence roundtrip must cover the baseline persisted chunk fields"
    }
    if ($analysis.roundtrip.snapshot_schema -ne "luminumbra.persistence.world_state_snapshot.v1") {
        throw "Persistence roundtrip snapshot schema must be luminumbra.persistence.world_state_snapshot.v1"
    }
    if ([int64]$analysis.roundtrip.chunk_count -lt 3) {
        throw "Persistence roundtrip fixture must cover at least three chunks"
    }
    if (-not $analysis.roundtrip.stable_serialization) {
        throw "Persistence roundtrip must be byte-stable after save/load/save"
    }
    if ([string]::IsNullOrWhiteSpace($analysis.roundtrip.before_checksum) -or
        $analysis.roundtrip.before_checksum -ne $analysis.roundtrip.after_checksum) {
        throw "Persistence roundtrip checksums must be present and equal"
    }

    foreach ($field in @(
        "coords",
        "chunk_id",
        "state",
        "sdf_data",
        "mesh_vertices",
        "mesh_indices",
        "water_level_data",
        "water_flow_data",
        "water_sim_terrain_height",
        "water_state"
    )) {
        Assert-ArrayContains -Values $analysis.persistence.persisted_fields -Needle $field -Description "Persistence roundtrip persisted_fields"
    }

    $requiredChecks = @(
        "persistence header declares gate API",
        "world state serializer emits deterministic chunk order",
        "world state loader restores chunk coordinates and state",
        "roundtrip serialization is byte-stable",
        "chunk payload preserves terrain, mesh, and water data",
        "persistence source is wired into common sources",
        "persistence gate test is wired into test sources",
        "gate artifact records deterministic checksum"
    )
    $checks = @($analysis.checks)
    foreach ($requiredCheck in $requiredChecks) {
        $matches = @($checks | Where-Object { $_.name -eq $requiredCheck })
        if ($matches.Count -ne 1) {
            throw "Persistence roundtrip analysis is missing check '$requiredCheck'"
        }
        if (-not $matches[0].passed) {
            throw "Persistence roundtrip check failed: $requiredCheck"
        }
    }
}

function Test-PersistenceRuntimeRoundtrip {
    # T-I2-13: runtime save/load roundtrip through the live client. The save
    # phase applies deterministic voxel edits and persists the world to a
    # session dir; the load phase restores the same world identity from that
    # session dir and re-hashes the same edited chunk ids.
    $exe = Get-ClientExe
    $runtimeDir = "build/$BuildPreset/test-artifacts/persistence/runtime-roundtrip"
    Remove-Item -Recurse -Force -ErrorAction SilentlyContinue $runtimeDir
    New-Item -ItemType Directory -Force -Path $runtimeDir | Out-Null
    $sessionDir = Join-Path $runtimeDir "session"

    # Small radii keep the whole-world snapshot (and its JSON parse on load)
    # to a few dozen MB while still covering every scripted edit site.
    $commonArgs = @(
        "--scenario", "persistence_roundtrip_smoke",
        "--auto-create-world",
        "--auto-enter-world",
        "--no-audio",
        "--no-ui",
        "--hidden-window",
        "--horizon-radius", "4",
        "--collision-radius", "2",
        "--min-renderable-chunks", "32",
        "--min-collision-chunks", "4",
        "--persistence-session-dir", $sessionDir,
        "--runtime-artifact-dir", $runtimeDir
    )

    Invoke-Checked -FilePath $exe -ArgumentList ($commonArgs + @("--persistence-phase", "save")) -TimeoutSeconds 300
    Invoke-Checked -FilePath $exe -ArgumentList ($commonArgs + @("--persistence-phase", "load")) -TimeoutSeconds 300

    $savePath = Join-Path $runtimeDir "persistence-runtime-roundtrip-phase-save.json"
    $loadPath = Join-Path $runtimeDir "persistence-runtime-roundtrip-phase-load.json"
    foreach ($path in @($savePath, $loadPath)) {
        if (-not (Test-Path $path)) {
            throw "persistence runtime roundtrip did not produce $path"
        }
    }

    $save = Get-Content $savePath -Raw | ConvertFrom-Json
    $load = Get-Content $loadPath -Raw | ConvertFrom-Json
    foreach ($artifact in @($save, $load)) {
        if ($artifact.schema -ne "luminumbra.persistence_runtime_roundtrip_phase.v1") {
            throw "Unexpected persistence runtime roundtrip phase schema '$($artifact.schema)'"
        }
    }
    if ($save.phase -ne "save") {
        throw "save-phase artifact reports phase '$($save.phase)'"
    }
    if ($load.phase -ne "load") {
        throw "load-phase artifact reports phase '$($load.phase)'"
    }
    if ([string]::IsNullOrWhiteSpace($save.world_hash)) {
        throw "persistence runtime roundtrip save phase produced an empty world hash"
    }
    if ([string]::IsNullOrWhiteSpace($load.world_hash)) {
        throw "persistence runtime roundtrip load phase produced an empty world hash"
    }
    if ($save.world_hash -ne $load.world_hash) {
        throw "persistence runtime roundtrip hash mismatch: save=$($save.world_hash) load=$($load.world_hash)"
    }
    if ([int64]$save.chunks_saved -le 0) {
        throw "persistence runtime roundtrip save phase persisted no chunks"
    }
    if ([int64]$save.chunks_dirty -le 0) {
        throw "persistence runtime roundtrip save phase flushed no dirty chunks"
    }
    if ([int64]$load.chunks_loaded -le 0) {
        throw "persistence runtime roundtrip load phase loaded no chunks"
    }
    if ([int64]$load.chunks_adopted_runtime -le 0) {
        throw "persistence runtime roundtrip load phase adopted no chunks into the live world"
    }

    $combined = [ordered]@{
        schema = "luminumbra.persistence_runtime_roundtrip.v1"
        timestamp_utc = (Get-Date).ToUniversalTime().ToString("yyyy-MM-ddTHH:mm:ssZ")
        hash_before = $save.world_hash
        hash_after = $load.world_hash
        chunks_saved = [int64]$save.chunks_saved
        dirty_chunks_flushed = [int64]$save.chunks_dirty
        match = ($save.world_hash -eq $load.world_hash)
    }
    $combinedPath = Join-Path $runtimeDir "persistence-runtime-roundtrip.json"
    ($combined | ConvertTo-Json -Depth 4) | Out-File -FilePath $combinedPath -Encoding ascii

    $verify = Get-Content $combinedPath -Raw | ConvertFrom-Json
    if ($verify.schema -ne "luminumbra.persistence_runtime_roundtrip.v1") {
        throw "persistence runtime roundtrip combined artifact has unexpected schema '$($verify.schema)'"
    }
    if (-not $verify.match -or $verify.hash_before -ne $verify.hash_after) {
        throw "persistence runtime roundtrip combined artifact failed validation"
    }
    if ([int64]$verify.chunks_saved -le 0 -or [int64]$verify.dirty_chunks_flushed -le 0) {
        throw "persistence runtime roundtrip combined artifact reports no persisted work"
    }

    Write-Host "persistence runtime roundtrip: hash_before=$($save.world_hash) hash_after=$($load.world_hash) chunks_saved=$($save.chunks_saved) dirty_chunks_flushed=$($save.chunks_dirty)"
}

function Test-ChunkFormatValidationGate {
    $artifactDir = "build/$BuildPreset/test-artifacts/persistence"
    $analysisPath = Join-Path $artifactDir "chunk-format-validation.json"
    $testScriptPath = "test/persistence/chunk-format-validation.ps1"

    if (-not (Test-Path $testScriptPath)) {
        throw "chunk format validation gate not yet implemented - missing $testScriptPath (produced by task T-EF-24-chunk-format-validator-gate)"
    }

    & $testScriptPath -BuildPreset $BuildPreset
    if (-not $?) {
        exit 1
    }
    if ($null -ne $LASTEXITCODE -and $LASTEXITCODE -ne 0) {
        exit $LASTEXITCODE
    }

    if (-not (Test-Path $analysisPath)) {
        throw "chunk format validation gate not yet implemented - missing $analysisPath (produced by task T-EF-24-chunk-format-validator-gate)"
    }

    $analysis = Get-Content $analysisPath -Raw | ConvertFrom-Json
    if ($analysis.schema -ne "luminumbra.persistence.chunk_format_validation.v1") {
        throw "Unexpected chunk format validation schema '$($analysis.schema)'"
    }
    if (-not $analysis.passed) {
        throw "Chunk format validation analysis reported failure"
    }
    if ($analysis.build_preset -ne $BuildPreset) {
        throw "Chunk format validation build_preset '$($analysis.build_preset)' does not match '$BuildPreset'"
    }
    if ($analysis.validator.source -ne "src/luminumbra_common/persistence/WorldPersistenceRoundtrip.cpp") {
        throw "Chunk format validation analysis must inspect WorldPersistenceRoundtrip.cpp"
    }
    if ($analysis.validator.header -ne "src/luminumbra_common/persistence/WorldPersistenceRoundtrip.h") {
        throw "Chunk format validation analysis must inspect WorldPersistenceRoundtrip.h"
    }
    if ($analysis.validator.validation_api -ne "ValidateWorldStreamingChunkFormatJson") {
        throw "Chunk format validation analysis must require ValidateWorldStreamingChunkFormatJson"
    }
    if ($analysis.validator.serializer -ne "SerializeChunkFormatValidationJson") {
        throw "Chunk format validation analysis must require SerializeChunkFormatValidationJson"
    }
    if ($analysis.validator.artifact_writer -ne "WriteChunkFormatValidationArtifact") {
        throw "Chunk format validation analysis must require WriteChunkFormatValidationArtifact"
    }
    if ($analysis.validator.format_contract -ne "world_state_snapshot_chunk_v1_required_fields") {
        throw "Chunk format validation must declare the required-fields chunk format contract"
    }
    if ([int64]$analysis.validator.required_field_count -lt 20) {
        throw "Chunk format validation must cover the baseline required chunk fields"
    }
    if ($analysis.format.snapshot_schema -ne "luminumbra.persistence.world_state_snapshot.v1") {
        throw "Chunk format validation must validate world snapshot chunk payloads"
    }
    if ($analysis.format.order_contract -ne "chunk_id_ascending") {
        throw "Chunk format validation must preserve chunk_id_ascending ordering"
    }
    if (-not $analysis.format.snapshot_contract_valid) {
        throw "Chunk format validation reports invalid snapshot contract"
    }
    if ([int64]$analysis.format.accepted_chunk_count -lt 3) {
        throw "Chunk format validation fixture must accept at least three persisted chunks"
    }
    if ([int64]$analysis.format.rejected_fixture_count -lt 3) {
        throw "Chunk format validation must reject the negative chunk fixtures"
    }
    if (-not $analysis.format.negative_fixtures_rejected) {
        throw "Chunk format validation reports that malformed chunks were not rejected"
    }
    if ([string]::IsNullOrWhiteSpace($analysis.format.fixture_checksum)) {
        throw "Chunk format validation analysis is missing the deterministic checksum"
    }

    foreach ($field in @(
        "coords",
        "chunk_id",
        "state",
        "state_value",
        "sdf_data",
        "heightmap_data",
        "mesh_vertices",
        "mesh_indices",
        "water_level_data",
        "water_flow_data",
        "water_sim_terrain_height",
        "water_state"
    )) {
        Assert-ArrayContains -Values $analysis.validator.required_fields -Needle $field -Description "Chunk format validation required_fields"
    }

    $requiredChecks = @(
        "chunk format validator API is declared",
        "chunk format schema declares required fields",
        "world snapshot chunk order contract is enforced",
        "chunk validator accepts persisted fixture chunks",
        "chunk validator rejects missing required fields",
        "chunk validator rejects chunk id coordinate mismatches",
        "chunk validator rejects incomplete water state",
        "chunk format artifact records deterministic checksum"
    )
    $checks = @($analysis.checks)
    foreach ($requiredCheck in $requiredChecks) {
        $matches = @($checks | Where-Object { $_.name -eq $requiredCheck })
        if ($matches.Count -ne 1) {
            throw "Chunk format validation analysis is missing check '$requiredCheck'"
        }
        if (-not $matches[0].passed) {
            throw "Chunk format validation check failed: $requiredCheck"
        }
    }
}

function Test-WorldHashEntitySnapshotGate {
    $artifactDir = "build/$BuildPreset/test-artifacts/persistence"
    $worldHashPath = Join-Path $artifactDir "world-hash.json"
    $entitySnapshotPath = Join-Path $artifactDir "entity-snapshot.json"
    $testScriptPath = "test/persistence/world-hash-entity-snapshot.ps1"

    if (-not (Test-Path $testScriptPath)) {
        throw "world hash/entity snapshot gate not yet implemented - missing $testScriptPath (produced by task T-EF-25-world-hash-entity-snapshot-gate)"
    }

    & $testScriptPath -BuildPreset $BuildPreset
    if (-not $?) {
        exit 1
    }
    if ($null -ne $LASTEXITCODE -and $LASTEXITCODE -ne 0) {
        exit $LASTEXITCODE
    }

    foreach ($path in @($worldHashPath, $entitySnapshotPath)) {
        if (-not (Test-Path $path)) {
            throw "world hash/entity snapshot gate not yet implemented - missing $path (produced by task T-EF-25-world-hash-entity-snapshot-gate)"
        }
    }

    $worldHash = Get-Content $worldHashPath -Raw | ConvertFrom-Json
    if ($worldHash.schema -ne "luminumbra.persistence.world_hash.v1") {
        throw "Unexpected world hash schema '$($worldHash.schema)'"
    }
    if (-not $worldHash.passed) {
        throw "World hash analysis reported failure"
    }
    if ($worldHash.build_preset -ne $BuildPreset) {
        throw "World hash build_preset '$($worldHash.build_preset)' does not match '$BuildPreset'"
    }
    if ($worldHash.persistence.source -ne "src/luminumbra_common/persistence/WorldPersistenceRoundtrip.cpp") {
        throw "World hash analysis must inspect WorldPersistenceRoundtrip.cpp"
    }
    if ($worldHash.persistence.header -ne "src/luminumbra_common/persistence/WorldPersistenceRoundtrip.h") {
        throw "World hash analysis must inspect WorldPersistenceRoundtrip.h"
    }
    if ($worldHash.persistence.snapshot_serializer -ne "SerializeWorldStreamingStateSnapshotJson") {
        throw "World hash analysis must require SerializeWorldStreamingStateSnapshotJson"
    }
    if ($worldHash.persistence.hash_api -ne "BuildWorldHashAnalysis") {
        throw "World hash analysis must require BuildWorldHashAnalysis"
    }
    if ($worldHash.persistence.validation_api -ne "WorldHashMeetsBaseline") {
        throw "World hash analysis must require WorldHashMeetsBaseline"
    }
    if ($worldHash.persistence.artifact_writer -ne "WriteWorldHashArtifact") {
        throw "World hash analysis must require WriteWorldHashArtifact"
    }
    if ($worldHash.persistence.order_contract -ne "chunk_id_ascending") {
        throw "World hash must preserve chunk_id_ascending ordering"
    }
    if ($worldHash.world_hash.snapshot_schema -ne "luminumbra.persistence.world_state_snapshot.v1") {
        throw "World hash must hash world state snapshot bytes"
    }
    if ($worldHash.world_hash.hash_algorithm -ne "fnv1a_64_stable_json") {
        throw "World hash must declare fnv1a_64_stable_json"
    }
    if ([int64]$worldHash.world_hash.chunk_count -lt 3) {
        throw "World hash fixture must cover at least three chunks"
    }
    if ([int64]$worldHash.world_hash.snapshot_byte_count -le 0) {
        throw "World hash artifact must record the hashed snapshot byte count"
    }
    if ([string]::IsNullOrWhiteSpace($worldHash.world_hash.hash) -or
        $worldHash.world_hash.hash -ne $worldHash.world_hash.roundtrip_hash) {
        throw "World hash checksums must be present and equal"
    }
    if (-not $worldHash.world_hash.stable_hash) {
        throw "World hash artifact reports unstable hash generation"
    }
    if (-not $worldHash.world_hash.roundtrip_hash_matches) {
        throw "World hash artifact reports a roundtrip hash mismatch"
    }
    foreach ($chunkId in @("0", "4194302", "18446739675667234817")) {
        Assert-ArrayContains -Values $worldHash.world_hash.chunk_ids -Needle $chunkId -Description "World hash chunk_ids"
    }

    $requiredWorldHashChecks = @(
        "world hash API is declared",
        "world hash uses deterministic snapshot bytes",
        "world hash preserves chunk_id_ascending order",
        "world hash is stable across save/load/save",
        "world hash artifact records deterministic hash"
    )
    $worldChecks = @($worldHash.checks)
    foreach ($requiredCheck in $requiredWorldHashChecks) {
        $matches = @($worldChecks | Where-Object { $_.name -eq $requiredCheck })
        if ($matches.Count -ne 1) {
            throw "World hash analysis is missing check '$requiredCheck'"
        }
        if (-not $matches[0].passed) {
            throw "World hash check failed: $requiredCheck"
        }
    }

    $entitySnapshot = Get-Content $entitySnapshotPath -Raw | ConvertFrom-Json
    if ($entitySnapshot.schema -ne "luminumbra.persistence.entity_snapshot.v1") {
        throw "Unexpected entity snapshot schema '$($entitySnapshot.schema)'"
    }
    if (-not $entitySnapshot.passed) {
        throw "Entity snapshot analysis reported failure"
    }
    if ($entitySnapshot.build_preset -ne $BuildPreset) {
        throw "Entity snapshot build_preset '$($entitySnapshot.build_preset)' does not match '$BuildPreset'"
    }
    if ($entitySnapshot.ecs.source -ne "src/luminumbra_common/ecs/EntitySnapshot.h") {
        throw "Entity snapshot analysis must inspect EntitySnapshot.h"
    }
    if ($entitySnapshot.ecs.snapshot_api -ne "SerializeEntityRegistrySnapshotJson") {
        throw "Entity snapshot analysis must require SerializeEntityRegistrySnapshotJson"
    }
    if ($entitySnapshot.ecs.loader -ne "LoadEntityRegistrySnapshotJson") {
        throw "Entity snapshot analysis must require LoadEntityRegistrySnapshotJson"
    }
    if ($entitySnapshot.ecs.validation_api -ne "EntitySnapshotMeetsBaseline") {
        throw "Entity snapshot analysis must require EntitySnapshotMeetsBaseline"
    }
    if ($entitySnapshot.ecs.fixture_api -ne "BuildEntitySnapshotFixture") {
        throw "Entity snapshot analysis must require BuildEntitySnapshotFixture"
    }
    if ($entitySnapshot.ecs.order_contract -ne "entity_id_ascending_component_type_ascending") {
        throw "Entity snapshot must declare deterministic entity/component ordering"
    }
    if ($entitySnapshot.entity_snapshot.snapshot_schema -ne "luminumbra.ecs.entity_snapshot.v1") {
        throw "Entity snapshot must serialize luminumbra.ecs.entity_snapshot.v1"
    }
    if ([int64]$entitySnapshot.entity_snapshot.entity_count -lt 3) {
        throw "Entity snapshot fixture must cover at least three entities"
    }
    if ([int64]$entitySnapshot.entity_snapshot.component_count -lt 6) {
        throw "Entity snapshot fixture must cover at least six components"
    }
    if ([int64]$entitySnapshot.entity_snapshot.snapshot_byte_count -le 0) {
        throw "Entity snapshot artifact must record the serialized snapshot byte count"
    }
    if (-not $entitySnapshot.entity_snapshot.stable_serialization) {
        throw "Entity snapshot artifact reports unstable serialization"
    }
    if ([string]::IsNullOrWhiteSpace($entitySnapshot.entity_snapshot.before_checksum) -or
        $entitySnapshot.entity_snapshot.before_checksum -ne $entitySnapshot.entity_snapshot.after_checksum) {
        throw "Entity snapshot checksums must be present and equal"
    }
    foreach ($entityId in @("1001", "1002", "1003")) {
        Assert-ArrayContains -Values $entitySnapshot.entity_snapshot.entity_ids -Needle $entityId -Description "Entity snapshot entity_ids"
    }
    foreach ($componentType in @("AethericField", "Instinct", "PersistenceAnchor", "Transform", "WaterAffinity")) {
        Assert-ArrayContains -Values $entitySnapshot.entity_snapshot.component_types -Needle $componentType -Description "Entity snapshot component_types"
    }

    $requiredEntitySnapshotChecks = @(
        "entity snapshot API is declared",
        "entity snapshot serializer emits deterministic entity order",
        "entity snapshot serializer emits deterministic component order",
        "entity snapshot loader restores entity ids and components",
        "entity snapshot serialization is byte-stable",
        "entity snapshot artifact records deterministic checksum",
        "ecs snapshot source is present"
    )
    $entityChecks = @($entitySnapshot.checks)
    foreach ($requiredCheck in $requiredEntitySnapshotChecks) {
        $matches = @($entityChecks | Where-Object { $_.name -eq $requiredCheck })
        if ($matches.Count -ne 1) {
            throw "Entity snapshot analysis is missing check '$requiredCheck'"
        }
        if (-not $matches[0].passed) {
            throw "Entity snapshot check failed: $requiredCheck"
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

    # T-I2-12/T-I2-13: the persistence runtime save/load path is the first
    # gate-backed activation. It is allowed to run in default builds because
    # it is inert without an existing world snapshot or unsaved voxel edits,
    # and its behavior is enforced by the PersistenceRuntimeRoundtrip mode.
    # The activation must stay documented in the frontier-disabled artifact.
    foreach ($needle in @(
        "Persistence runtime save/load",
        "PersistenceRuntimeRoundtrip",
        "persistence_roundtrip_smoke"
    )) {
        Assert-Contains -Path $FrontierDisabledPath -Needle $needle
    }

    $text = Get-Content $FrontierDisabledPath -Raw
    if ($text -match "(?i)\benabled by default\b") {
        throw "frontier-disabled gate must not declare frontier behavior enabled by default"
    }
}

function Test-NetworkLoopbackAuthorityGate {
    $artifactDir = "build/$BuildPreset/test-artifacts/network"
    $analysisPath = Join-Path $artifactDir "network-loopback-convergence.json"
    $testScriptPath = "test/network/network-loopback-authority-gate.ps1"

    if (-not (Test-Path $testScriptPath)) {
        throw "network loopback authority gate not yet implemented - missing $testScriptPath (produced by task T-EF-31-network-loopback-authority-gate)"
    }

    & $testScriptPath -BuildPreset $BuildPreset
    if (-not $?) {
        exit 1
    }
    if ($null -ne $LASTEXITCODE -and $LASTEXITCODE -ne 0) {
        exit $LASTEXITCODE
    }

    if (-not (Test-Path $analysisPath)) {
        throw "network loopback authority gate not yet implemented - missing $analysisPath (produced by task T-EF-31-network-loopback-authority-gate)"
    }

    $analysis = Get-Content $analysisPath -Raw | ConvertFrom-Json
    if ($analysis.schema -ne "luminumbra.network.loopback_convergence.v1") {
        throw "Unexpected network loopback convergence schema '$($analysis.schema)'"
    }
    if (-not $analysis.passed) {
        throw "Network loopback convergence analysis reported failure"
    }
    if ($analysis.build_preset -ne $BuildPreset) {
        throw "Network loopback convergence build_preset '$($analysis.build_preset)' does not match '$BuildPreset'"
    }
    if ($analysis.network.source -ne "src/luminumbra_common/network/NetworkLoopbackAuthority.cpp") {
        throw "Network loopback analysis must inspect NetworkLoopbackAuthority.cpp"
    }
    if ($analysis.network.header -ne "src/luminumbra_common/network/NetworkLoopbackAuthority.h") {
        throw "Network loopback analysis must inspect NetworkLoopbackAuthority.h"
    }
    if ($analysis.network.serializer -ne "SerializeNetworkLoopbackConvergenceJson") {
        throw "Network loopback analysis must require SerializeNetworkLoopbackConvergenceJson"
    }
    if ($analysis.network.validation_api -ne "NetworkLoopbackAuthorityMeetsBaseline") {
        throw "Network loopback analysis must require NetworkLoopbackAuthorityMeetsBaseline"
    }
    if ($analysis.network.artifact_writer -ne "WriteNetworkLoopbackConvergenceArtifact") {
        throw "Network loopback analysis must require WriteNetworkLoopbackConvergenceArtifact"
    }
    if ($analysis.network.authority_contract -ne "server_authoritative_loopback_reconciliation") {
        throw "Network loopback must declare server_authoritative_loopback_reconciliation"
    }
    if ($analysis.network.order_contract -ne "tick_then_sequence_then_client_id") {
        throw "Network loopback must declare tick_then_sequence_then_client_id ordering"
    }
    if ($analysis.loopback.transport -ne "in_process_loopback") {
        throw "Network loopback fixture must use the in-process loopback transport"
    }
    if ($analysis.loopback.simulation -ne "authoritative_server_with_predicted_client") {
        throw "Network loopback fixture must simulate an authoritative server with a predicted client"
    }
    if ($analysis.loopback.authoritative_client_id -ne "client-alpha") {
        throw "Network loopback fixture must use client-alpha as the authorized client"
    }
    if ([int64]$analysis.loopback.submitted_frame_count -lt 6) {
        throw "Network loopback fixture must submit at least six loopback frames"
    }
    if ([int64]$analysis.loopback.accepted_frame_count -lt 5) {
        throw "Network loopback fixture must accept the authorized client frames"
    }
    if ([int64]$analysis.loopback.rejected_frame_count -lt 1) {
        throw "Network loopback fixture must reject at least one unauthorized frame"
    }
    if (-not $analysis.loopback.unauthorized_authority_claim_rejected) {
        throw "Network loopback fixture must reject unauthorized client authority claims"
    }
    if (-not $analysis.loopback.client_prediction_reconciled) {
        throw "Network loopback fixture must reconcile client prediction to authority"
    }
    if (-not $analysis.loopback.converged) {
        throw "Network loopback fixture did not converge"
    }
    if ([int64]$analysis.loopback.prediction_error_before_reconcile_mm -le 0) {
        throw "Network loopback fixture must record prediction error before reconciliation"
    }
    if ([int64]$analysis.loopback.prediction_error_after_reconcile_mm -ne 0) {
        throw "Network loopback fixture must eliminate prediction error after reconciliation"
    }
    if ([string]::IsNullOrWhiteSpace($analysis.loopback.authoritative_checksum)) {
        throw "Network loopback convergence analysis is missing the authoritative checksum"
    }
    if ([int64]$analysis.final_authoritative_state.tick -ne [int64]$analysis.reconciled_client_state.tick -or
        [int64]$analysis.final_authoritative_state.authoritative_revision -ne [int64]$analysis.reconciled_client_state.authoritative_revision -or
        [int64]$analysis.final_authoritative_state.position_x_mm -ne [int64]$analysis.reconciled_client_state.position_x_mm -or
        [int64]$analysis.final_authoritative_state.position_y_mm -ne [int64]$analysis.reconciled_client_state.position_y_mm) {
        throw "Network loopback final authoritative state must match the reconciled client state"
    }

    $decisions = @($analysis.decisions)
    if ([int64]$analysis.loopback.submitted_frame_count -ne $decisions.Count) {
        throw "Network loopback submitted_frame_count does not match decisions array"
    }
    $expectedDecisions = @(
        "1|1|client-alpha|accepted|authoritative_frame_applied",
        "2|2|client-alpha|accepted|authoritative_frame_applied",
        "2|1|client-beta|rejected|client_authority_claim_rejected",
        "3|3|client-alpha|accepted|authoritative_frame_applied",
        "4|4|client-alpha|accepted|authoritative_frame_applied",
        "5|5|client-alpha|accepted|authoritative_frame_applied"
    )
    $actualDecisions = @($decisions | ForEach-Object {
        $status = if ($_.accepted) { "accepted" } else { "rejected" }
        "$($_.tick)|$($_.sequence)|$($_.client_id)|$status|$($_.reason)"
    })
    if (($actualDecisions -join "`n") -ne ($expectedDecisions -join "`n")) {
        throw "Network loopback decisions do not match the deterministic authority fixture"
    }

    $requiredChecks = @(
        "network loopback authority API is declared",
        "loopback source applies server authority over client claims",
        "loopback fixture rejects client authority escalation",
        "loopback convergence reaches deterministic state",
        "network source is wired into common sources",
        "network gate test is wired into test sources",
        "gate artifact records authoritative checksum"
    )
    $checks = @($analysis.checks)
    foreach ($requiredCheck in $requiredChecks) {
        $matches = @($checks | Where-Object { $_.name -eq $requiredCheck })
        if ($matches.Count -ne 1) {
            throw "Network loopback convergence analysis is missing check '$requiredCheck'"
        }
        if (-not $matches[0].passed) {
            throw "Network loopback convergence check failed: $requiredCheck"
        }
    }
}

function Test-NetworkStateHash {
    $artifactDir = "build/$BuildPreset/test-artifacts/network"
    $analysisPath = Join-Path $artifactDir "network-state-hash.json"
    $testScriptPath = "test/network/network-state-hash-gate.ps1"

    if (-not (Test-Path $testScriptPath)) {
        throw "network state hash gate not yet implemented - missing $testScriptPath (produced by task T-EF-32-network-state-hash-gate)"
    }

    & $testScriptPath -BuildPreset $BuildPreset
    if (-not $?) {
        exit 1
    }
    if ($null -ne $LASTEXITCODE -and $LASTEXITCODE -ne 0) {
        exit $LASTEXITCODE
    }

    if (-not (Test-Path $analysisPath)) {
        throw "network state hash gate not yet implemented - missing $analysisPath (produced by task T-EF-32-network-state-hash-gate)"
    }

    $analysis = Get-Content $analysisPath -Raw | ConvertFrom-Json
    if ($analysis.schema -ne "luminumbra.network.state_hash.v1") {
        throw "Unexpected network state hash schema '$($analysis.schema)'"
    }
    if (-not $analysis.passed) {
        throw "Network state hash analysis reported failure"
    }
    if ($analysis.build_preset -ne $BuildPreset) {
        throw "Network state hash build_preset '$($analysis.build_preset)' does not match '$BuildPreset'"
    }
    if ($analysis.network.state_contract -ne "authoritative_sorted_state_per_tick") {
        throw "Network state hash must declare authoritative_sorted_state_per_tick"
    }
    if ($analysis.network.order_contract -ne "tick_ascending_sorted_state_fields") {
        throw "Network state hash must declare tick_ascending_sorted_state_fields ordering"
    }
    if ($analysis.state_hash.hash_algorithm -ne "fnv1a_64_canonical_state_string") {
        throw "Network state hash must use fnv1a_64_canonical_state_string"
    }
    if ([string]::IsNullOrWhiteSpace($analysis.state_hash.world_hash)) {
        throw "Network state hash must embed the persistence world hash"
    }
    if ([int64]$analysis.state_hash.durable_entity_id_count -lt 1) {
        throw "Network state hash must cover durable entity ids"
    }
    if ([int64]$analysis.state_hash.tick_count -lt 5) {
        throw "Network state hash must cover at least five authoritative ticks"
    }
    if (-not $analysis.state_hash.deterministic_replay) {
        throw "Network state hash replay was not deterministic"
    }
    if (-not $analysis.state_hash.monotonic_ticks) {
        throw "Network state hash ticks were not monotonically ascending"
    }
    if ($analysis.state_hash.final_state_hash -ne $analysis.state_hash.replay_final_state_hash) {
        throw "Network state hash final hash does not match the replay hash"
    }
    foreach ($tick in @($analysis.ticks)) {
        if ([string]::IsNullOrWhiteSpace($tick.state_hash) -or $tick.state_hash.Length -ne 16) {
            throw "Network state hash tick $($tick.tick) is missing a 64-bit hash"
        }
    }
}

function Test-PerfRegression {
    # T-I3-20: preset-aware lane. -Preset release selects the release build
    # dir and the release baseline file; default/empty keeps the historical
    # debug lane (build/$BuildPreset + perf-baseline.json) untouched.
    $perfBuildPreset = $BuildPreset
    if (-not [string]::IsNullOrWhiteSpace($Preset)) {
        $perfBuildPreset = $Preset
    }
    $baselineLeaf = "perf-baseline.json"
    if ($Preset -eq "release") {
        $baselineLeaf = "perf-baseline-release.json"
    }
    $baselinePath = "$ArtifactDir/$baselineLeaf"
    $baseline = Read-JsonArtifact -Path $baselinePath -Schema "luminumbra.perf_baseline.v1"

    # A provisional baseline (written by run-release-perf-lane.ps1 on a noisy
    # machine) is treated like a placeholder: regressions warn, never fail,
    # until the orchestrator blesses a real baseline on a quiet machine.
    $placeholderBaseline = ($baseline.status -eq "placeholder_pending_capture") -or
        ($baseline.status -eq "provisional") -or ($baseline.provisional -eq $true)
    if (-not $placeholderBaseline -and $baseline.status -ne "blessed") {
        throw "perf baseline has unexpected status '$($baseline.status)' (expected 'blessed', 'provisional' or 'placeholder_pending_capture')"
    }

    if (-not [string]::IsNullOrWhiteSpace($baseline.machine_id) -and
        $baseline.machine_id -ne "UNCAPTURED" -and
        $baseline.machine_id -ne $env:COMPUTERNAME) {
        throw "perf baseline captured on different machine ('$($baseline.machine_id)' vs '$($env:COMPUTERNAME)') - recapture required via .forge/scripts/capture-perf-baseline.ps1"
    }

    $exe = "build/$perfBuildPreset/bin/initial_world_loading_perf_test.exe"
    if (-not (Test-Path $exe)) {
        throw "Missing perf test executable. Run -Mode Build first: $exe"
    }

    $requiredScenarios = @(
        "boot", "create_world", "enter_spawn", "idle_horizon", "pan_camera",
        "streaming_walk", "chunk_churn", "shader_warmup", "shutdown"
    )

    # Noise policy: a single debug-build run is too noisy for a 10% p99 margin
    # (observed 2x swings minutes after a clean median-of-3 capture). Compare
    # the MEDIAN of up to 3 runs, short-circuiting after run 1 when everything
    # is already inside the fail ceiling.
    $maxRuns = 3
    $observedRuns = @{}
    foreach ($name in $requiredScenarios) { $observedRuns[$name] = @() }

    for ($run = 1; $run -le $maxRuns; $run++) {
        Invoke-Checked -FilePath $exe -ArgumentList @(
            "--gtest_filter=InitialWorldLoadingPerfTest.PerformanceFrameworkBenchmarkScenariosWriteBudgetArtifacts"
        ) -TimeoutSeconds 300

        $summaryPath = "build/$perfBuildPreset/test-artifacts/performance_framework/benchmark_summary.json"
        $summary = Read-JsonArtifact -Path $summaryPath -Schema "luminumbra.performance_framework.benchmark_summary.v1"

        $anyOverFail = $false
        foreach ($name in $requiredScenarios) {
            $observedEntries = @($summary.scenarios | Where-Object { $_.name -eq $name })
            if ($observedEntries.Count -ne 1) {
                throw "benchmark summary must contain exactly one '$name' scenario, found $($observedEntries.Count)"
            }
            $observed = $observedEntries[0]
            if ($null -eq $observed.regression_metrics) {
                throw "benchmark summary scenario '$name' is missing the regression_metrics block"
            }

            $baselineEntry = $baseline.scenarios.$name
            if ($null -eq $baselineEntry) {
                throw "perf baseline is missing scenario '$name'"
            }

            $observedP99 = [double]$observed.regression_metrics.p99_ms
            $observedRuns[$name] += $observedP99
            $failCeiling = ([double]$baselineEntry.p99_ms) * (1.0 + $MarginPercent / 100.0) + $NoiseFloorMs
            if ($observedP99 -gt $failCeiling) {
                $anyOverFail = $true
            }
        }

        if (-not $anyOverFail) {
            break
        }
        if ($run -lt $maxRuns) {
            Write-Host "perf-regression: run $run exceeded a fail ceiling; re-running for median comparison ($($run + 1)/$maxRuns)"
        }
    }

    $regressions = @()
    foreach ($name in $requiredScenarios) {
        $samples = @($observedRuns[$name] | Sort-Object)
        $medianP99 = [double]$samples[[int][Math]::Floor(($samples.Count - 1) / 2)]
        $baselineP99 = [double]$baseline.scenarios.$name.p99_ms
        $failCeiling = $baselineP99 * (1.0 + $MarginPercent / 100.0) + $NoiseFloorMs
        $warnCeiling = $baselineP99 * (1.0 + $WarnPercent / 100.0) + $NoiseFloorMs

        if ($medianP99 -gt $failCeiling) {
            $regressions += ("scenario '{0}' median p99 {1:N3} ms (of {2} runs) exceeds baseline {3:N3} ms by more than {4}%" -f $name, $medianP99, $samples.Count, $baselineP99, $MarginPercent)
        } elseif ($medianP99 -gt $warnCeiling) {
            Write-Host ("perf-regression warning: scenario '{0}' median p99 {1:N3} ms exceeds baseline {2:N3} ms by more than {3}% (fail threshold {4}%)" -f $name, $medianP99, $baselineP99, $WarnPercent, $MarginPercent)
        }
    }

    if ($regressions.Count -gt 0) {
        if ($placeholderBaseline) {
            foreach ($regression in $regressions) {
                Write-Host "perf-regression warning (placeholder baseline, not enforced): $regression"
            }
            Write-Host "perf baseline status is 'placeholder_pending_capture'; regressions are reported as warnings until the baseline is blessed via .forge/scripts/capture-perf-baseline.ps1"
        } else {
            throw "perf regression gate failed:`n$($regressions -join "`n")"
        }
    }
}

# --- T-I2-17 beautification track B (atmosphere) modes: append-only ---

function Test-SkyboxVisual {
    $exe = Get-ClientExe
    $visualDir = "build/$BuildPreset/test-artifacts/runtime/skybox-visual"
    Remove-Item -Recurse -Force -ErrorAction SilentlyContinue $visualDir
    New-Item -ItemType Directory -Force -Path $visualDir | Out-Null

    $runSeconds = [Math]::Max(15, $SmokeSeconds)
    Invoke-Checked -FilePath $exe -ArgumentList @(
        "--scenario", "skybox_visual_smoke",
        "--auto-create-world",
        "--auto-enter-world",
        "--timed-run", "$runSeconds",
        "--no-audio",
        "--no-ui",
        "--runtime-artifact-dir", $visualDir
    ) -TimeoutSeconds ([Math]::Max(120, $runSeconds + 90))

    $analysisPath = Join-Path $visualDir "skybox-visual-analysis.json"
    if (-not (Test-Path $analysisPath)) {
        throw "skybox visual run did not produce $analysisPath (gate produced by task T-I2-17a-enhanced-skybox)"
    }

    $analysis = Get-Content $analysisPath -Raw | ConvertFrom-Json
    if ($analysis.schema -ne "luminumbra.skybox_visual.v1") {
        throw "Unexpected skybox visual analysis schema '$($analysis.schema)'"
    }
    if ([int64]$analysis.gl_debug.errors -ne 0) {
        throw "Skybox visual run emitted GL debug errors: $($analysis.gl_debug.errors)"
    }
    if ([int]$analysis.render_pass.skybox_draws -le 0) {
        throw "Skybox visual run did not submit skybox draws"
    }
    if (-not $analysis.gradient.passed) {
        throw "Skybox horizon gradient check failed: drop=$($analysis.gradient.horizon_zenith_drop), violations=$($analysis.gradient.monotonic_violations)"
    }
    if ([double]$analysis.gradient.horizon_zenith_drop -lt [double]$analysis.thresholds.min_horizon_zenith_drop) {
        throw "Skybox horizon->zenith luminance drop $($analysis.gradient.horizon_zenith_drop) is below threshold $($analysis.thresholds.min_horizon_zenith_drop)"
    }
    if ([int]$analysis.gradient.monotonic_violations -gt [int]$analysis.thresholds.max_monotonic_violations) {
        throw "Skybox gradient has too many monotonicity violations: $($analysis.gradient.monotonic_violations)"
    }
    if (-not $analysis.sun_disc.passed) {
        throw "Skybox sun-disc check failed: on_screen=$($analysis.sun_disc.on_screen), pixels=$($analysis.sun_disc.pixels), cluster_fraction=$($analysis.sun_disc.sun_cluster_fraction)"
    }
    if ([int64]$analysis.sun_disc.pixels -lt [int64]$analysis.thresholds.min_sun_disc_pixels) {
        throw "Skybox sun-disc has too few high-luminance pixels: $($analysis.sun_disc.pixels)"
    }
    if ([double]$analysis.sun_disc.sun_cluster_fraction -lt [double]$analysis.thresholds.min_sun_cluster_fraction) {
        throw "Skybox sun-disc cluster is not localized at the expected sun position"
    }
    if (-not $analysis.passed) {
        throw "Skybox visual analysis reported failure"
    }

    Assert-PpmArtifact (Join-Path $visualDir $analysis.screenshot)
}

function Test-WeatherVisual {
    $exe = Get-ClientExe
    $visualDir = "build/$BuildPreset/test-artifacts/runtime/weather-visual"
    Remove-Item -Recurse -Force -ErrorAction SilentlyContinue $visualDir
    New-Item -ItemType Directory -Force -Path $visualDir | Out-Null

    $runSeconds = [Math]::Max(20, $SmokeSeconds)
    Invoke-Checked -FilePath $exe -ArgumentList @(
        "--scenario", "weather_visual_smoke",
        "--auto-create-world",
        "--auto-enter-world",
        "--timed-run", "$runSeconds",
        "--no-audio",
        "--no-ui",
        "--runtime-artifact-dir", $visualDir
    ) -TimeoutSeconds ([Math]::Max(120, $runSeconds + 90))

    $analysisPath = Join-Path $visualDir "weather-visual-analysis.json"
    if (-not (Test-Path $analysisPath)) {
        throw "weather visual run did not produce $analysisPath (gate produced by task T-I2-17b-weather-overlay)"
    }

    $analysis = Get-Content $analysisPath -Raw | ConvertFrom-Json
    if ($analysis.schema -ne "luminumbra.weather_visual.v1") {
        throw "Unexpected weather visual analysis schema '$($analysis.schema)'"
    }
    if ([int64]$analysis.gl_debug.errors -ne 0) {
        throw "Weather visual run emitted GL debug errors: $($analysis.gl_debug.errors)"
    }
    if ($analysis.weather.type -ne "rain" -or [double]$analysis.weather.intensity -lt 1.0) {
        throw "Weather visual run must exercise rain at intensity 1.0"
    }
    if (-not $analysis.overcast.passed) {
        throw "Weather overcast luminance drop check failed: drop=$($analysis.overcast.sky_luminance_drop)"
    }
    if ([double]$analysis.overcast.sky_luminance_drop -lt [double]$analysis.thresholds.min_overcast_luminance_drop) {
        throw "Weather sky luminance drop $($analysis.overcast.sky_luminance_drop) is below threshold $($analysis.thresholds.min_overcast_luminance_drop)"
    }
    if (-not $analysis.streaks.passed) {
        throw "Weather streak structure check failed: gradient_ratio=$($analysis.streaks.sky_horizontal_gradient_ratio)"
    }
    if ([double]$analysis.streaks.sky_horizontal_gradient_ratio -lt [double]$analysis.thresholds.min_streak_gradient_ratio) {
        throw "Weather streak gradient ratio $($analysis.streaks.sky_horizontal_gradient_ratio) is below threshold $($analysis.thresholds.min_streak_gradient_ratio)"
    }
    if (-not $analysis.passed) {
        throw "Weather visual analysis reported failure"
    }

    Assert-PpmArtifact (Join-Path $visualDir $analysis.baseline_screenshot)
    Assert-PpmArtifact (Join-Path $visualDir $analysis.weather_screenshot)
}

function Test-TimeOfDaySweep {
    $exe = Get-ClientExe
    $visualDir = "build/$BuildPreset/test-artifacts/runtime/timeofday-sweep"
    Remove-Item -Recurse -Force -ErrorAction SilentlyContinue $visualDir
    New-Item -ItemType Directory -Force -Path $visualDir | Out-Null

    $runSeconds = [Math]::Max(24, $SmokeSeconds)
    Invoke-Checked -FilePath $exe -ArgumentList @(
        "--scenario", "timeofday_sweep_smoke",
        "--auto-create-world",
        "--auto-enter-world",
        "--timed-run", "$runSeconds",
        "--no-audio",
        "--no-ui",
        "--runtime-artifact-dir", $visualDir
    ) -TimeoutSeconds ([Math]::Max(150, $runSeconds + 90))

    $analysisPath = Join-Path $visualDir "timeofday-sweep-analysis.json"
    if (-not (Test-Path $analysisPath)) {
        throw "time-of-day sweep run did not produce $analysisPath (gate produced by task T-I2-17c-timeofday-sweep)"
    }

    $analysis = Get-Content $analysisPath -Raw | ConvertFrom-Json
    if ($analysis.schema -ne "luminumbra.timeofday_sweep.v1") {
        throw "Unexpected time-of-day sweep analysis schema '$($analysis.schema)'"
    }
    if ([int64]$analysis.gl_debug.errors -ne 0) {
        throw "Time-of-day sweep emitted GL debug errors: $($analysis.gl_debug.errors)"
    }

    $phases = @($analysis.phases)
    if ($phases.Count -lt 3) {
        throw "Time-of-day sweep must capture noon, dusk, and night phases (found $($phases.Count))"
    }
    foreach ($phaseName in @("noon", "dusk", "night")) {
        $matches = @($phases | Where-Object { $_.name -eq $phaseName })
        if ($matches.Count -ne 1) {
            throw "Time-of-day sweep is missing the '$phaseName' phase capture"
        }
        Assert-PpmArtifact (Join-Path $visualDir $matches[0].screenshot)
    }

    if (-not $analysis.luminance_ordering.passed) {
        throw "Time-of-day luminance ordering failed: noon=$($analysis.luminance_ordering.noon_mean_luminance) dusk=$($analysis.luminance_ordering.dusk_mean_luminance) night=$($analysis.luminance_ordering.night_mean_luminance)"
    }
    if ([double]$analysis.luminance_ordering.noon_over_dusk_gap -lt [double]$analysis.thresholds.min_noon_over_dusk_gap) {
        throw "Noon-over-dusk luminance gap $($analysis.luminance_ordering.noon_over_dusk_gap) is below threshold"
    }
    if ([double]$analysis.luminance_ordering.dusk_over_night_gap -lt [double]$analysis.thresholds.min_dusk_over_night_gap) {
        throw "Dusk-over-night luminance gap $($analysis.luminance_ordering.dusk_over_night_gap) is below threshold"
    }
    if (-not $analysis.dusk_warm_shift.passed) {
        throw "Dusk warm-shift check failed: r/b increase $($analysis.dusk_warm_shift.r_b_ratio_increase)"
    }
    if ([double]$analysis.dusk_warm_shift.r_b_ratio_increase -lt [double]$analysis.thresholds.min_dusk_warm_shift) {
        throw "Dusk r/b warm shift $($analysis.dusk_warm_shift.r_b_ratio_increase) is below threshold $($analysis.thresholds.min_dusk_warm_shift)"
    }
    if (@("checked_surface_emissive", "not_applicable_no_surface_emissives") -notcontains $analysis.emissive_check.status) {
        throw "Time-of-day emissive check reported unexpected status '$($analysis.emissive_check.status)'"
    }
    if (-not $analysis.emissive_check.passed) {
        throw "Time-of-day emissive night check failed (status=$($analysis.emissive_check.status))"
    }
    if ($analysis.emissive_check.status -eq "checked_surface_emissive") {
        Assert-PpmArtifact (Join-Path $visualDir $analysis.emissive_check.screenshot)
        if ([int64]$analysis.emissive_check.center_glow_pixels -lt [int64]$analysis.thresholds.min_emissive_glow_pixels) {
            throw "Emissive night capture has too few glow pixels: $($analysis.emissive_check.center_glow_pixels)"
        }
    }
    if (-not $analysis.passed) {
        throw "Time-of-day sweep analysis reported failure"
    }
}

# --- T-I3-3 PlayerView mode: append-only ---
# Eye-level 360-degree player-view coverage gate (player_view_smoke): 12 yaw
# stations + a peak-aimed station per preset, plus the seed-424242
# archipelago degenerate-geometry region station. Per station:
# missing_frustum_surface_chunks == 0, renderable_frustum_ratio >= 0.98,
# near_black_cluster_count == 0 (strict max(r,g,b) <= 2 voids), and
# below_horizon_sky_ratio < 0.005 where the sky/water hue ambiguity does not
# apply (sky_ratio_enforced in the artifact). The mountains run also proves
# the surface-span streaming stays inside the 8192 active-chunk budget.

function Test-PlayerView {
    $exe = Get-ClientExe
    $runSeconds = [Math]::Max(45, $SmokeSeconds)

    foreach ($preset in @("default", "mountains", "archipelago")) {
        $viewDir = "build/$BuildPreset/test-artifacts/runtime/player-view-$preset"
        Remove-Item -Recurse -Force -ErrorAction SilentlyContinue $viewDir
        New-Item -ItemType Directory -Force -Path $viewDir | Out-Null

        Invoke-Checked -FilePath $exe -ArgumentList @(
            "--scenario", "player_view_smoke",
            "--auto-create-world",
            "--auto-enter-world",
            "--timed-run", "$runSeconds",
            "--world-preset", $preset,
            "--no-audio",
            "--no-ui",
            "--runtime-artifact-dir", $viewDir
        ) -TimeoutSeconds ([Math]::Max(180, $runSeconds + 120))

        $analysisPath = Join-Path $viewDir "player-view-analysis.json"
        $analysis = Read-JsonArtifact -Path $analysisPath -Schema "luminumbra.player_view.v1"

        if ($analysis.world_preset -ne $preset) {
            throw "player view ($preset) artifact has unexpected world_preset '$($analysis.world_preset)'"
        }
        if ([int64]$analysis.gl_debug.errors -ne 0) {
            throw "player view ($preset) run emitted GL debug errors: $($analysis.gl_debug.errors)"
        }
        if ([int64]$analysis.aggregates.captured_stations -ne [int64]$analysis.aggregates.expected_stations) {
            throw "player view ($preset) captured $($analysis.aggregates.captured_stations) of $($analysis.aggregates.expected_stations) stations"
        }
        $expectedStations = if ($preset -eq "archipelago") { 14 } else { 13 }
        if ([int64]$analysis.aggregates.expected_stations -ne $expectedStations) {
            throw "player view ($preset) expected $expectedStations stations (12 yaw + peak$(if ($preset -eq 'archipelago') { ' + degenerate region' })), found $($analysis.aggregates.expected_stations)"
        }

        $skyEnforced = [bool]$analysis.thresholds.sky_ratio_enforced
        foreach ($station in $analysis.stations) {
            Assert-PpmArtifact (Join-Path $viewDir $station.file)
            if ([int64]$station.coverage.missing_frustum_surface_chunks -gt 0) {
                throw "player view ($preset) station '$($station.name)' is missing $($station.coverage.missing_frustum_surface_chunks) frustum surface chunks"
            }
            if ([double]$station.coverage.renderable_frustum_ratio -lt 0.98) {
                throw "player view ($preset) station '$($station.name)' renderable frustum ratio $($station.coverage.renderable_frustum_ratio) below 0.98"
            }
            if ($skyEnforced -and [double]$station.pixels.below_horizon_sky_ratio -ge 0.005) {
                throw "player view ($preset) station '$($station.name)' shows sky below the horizon: ratio $($station.pixels.below_horizon_sky_ratio)"
            }
            if ([int64]$station.pixels.near_black_cluster_count -gt 0) {
                throw "player view ($preset) station '$($station.name)' has $($station.pixels.near_black_cluster_count) degenerate void clusters (largest $($station.pixels.largest_near_black_cluster_px)px)"
            }
            if (-not $station.passed) {
                throw "player view ($preset) station '$($station.name)' failed its thresholds"
            }
        }

        if ($preset -eq "archipelago") {
            $degenerate = @($analysis.stations | Where-Object { $_.name -eq "degenerate_region" })
            if ($degenerate.Count -ne 1) {
                throw "player view (archipelago) is missing the seed-424242 degenerate_region station"
            }
        }
        if ($preset -eq "mountains") {
            if ([int64]$analysis.runtime_chunks.total_chunks -gt [int64]$analysis.runtime_chunks.active_chunk_budget) {
                throw "player view (mountains) exceeded the active chunk budget: $($analysis.runtime_chunks.total_chunks) > $($analysis.runtime_chunks.active_chunk_budget)"
            }
            Write-Host "player view (mountains): active chunks $($analysis.runtime_chunks.total_chunks) of budget $($analysis.runtime_chunks.active_chunk_budget) (sdf skipped on $($analysis.runtime_chunks.sdf_skipped_chunks))"
        }

        if (-not $analysis.passed) {
            throw "player view ($preset) analysis reported failure"
        }
        Write-Host "player view ($preset): stations=$($analysis.aggregates.captured_stations), max_missing=$($analysis.aggregates.max_missing_frustum_surface_chunks), min_renderable_ratio=$($analysis.aggregates.min_renderable_frustum_ratio), max_sky_ratio=$($analysis.aggregates.max_below_horizon_sky_ratio) (enforced=$skyEnforced), max_void_clusters=$($analysis.aggregates.max_near_black_cluster_count)"
    }
}

# --- T-I3-9 FarLodHorizon mode: append-only ---
# Far-LOD horizon + live/far seam gate (farlod_horizon_smoke). Phase A of the
# run measures the gbuffer GPU time with far-LOD DISABLED (the honest in-run
# baseline; the committed perf baseline records frame times, not per-pass GPU
# times); phase B enables far-LOD and sweeps eye-level + elevated stations.
# Gates (design-decisions.md section 4): zero missing wanted regions to
# 1536 m after settle; farlod_resident_bytes < 64 MB; gbuffer_gpu_ms delta
# < 1.5 ms; horizon screenshots show terrain to the horizon (below-horizon
# sky ratio bounded); the live/far boundary band ROI (~192 m at the smoke
# radii) shows no sky-leak band and no strict void clusters (the
# Distant-Horizons failure mode).

function Test-FarLodHorizon {
    $exe = Get-ClientExe
    $runSeconds = [Math]::Max(50, $SmokeSeconds)

    foreach ($preset in @("mountains", "default")) {
        $viewDir = "build/$BuildPreset/test-artifacts/runtime/farlod-horizon-$preset"
        Remove-Item -Recurse -Force -ErrorAction SilentlyContinue $viewDir
        New-Item -ItemType Directory -Force -Path $viewDir | Out-Null

        Invoke-Checked -FilePath $exe -ArgumentList @(
            "--scenario", "farlod_horizon_smoke",
            "--auto-create-world",
            "--auto-enter-world",
            "--timed-run", "$runSeconds",
            "--world-preset", $preset,
            "--no-audio",
            "--no-ui",
            "--runtime-artifact-dir", $viewDir
        ) -TimeoutSeconds ([Math]::Max(180, $runSeconds + 120))

        $analysisPath = Join-Path $viewDir "farlod-horizon-analysis.json"
        $analysis = Read-JsonArtifact -Path $analysisPath -Schema "luminumbra.farlod_horizon.v1"

        if ($analysis.world_preset -ne $preset) {
            throw "farlod horizon ($preset) artifact has unexpected world_preset '$($analysis.world_preset)'"
        }
        if ([int64]$analysis.gl_debug.errors -ne 0) {
            throw "farlod horizon ($preset) run emitted GL debug errors: $($analysis.gl_debug.errors)"
        }
        if ([int64]$analysis.aggregates.captured_stations -ne [int64]$analysis.aggregates.expected_stations) {
            throw "farlod horizon ($preset) captured $($analysis.aggregates.captured_stations) of $($analysis.aggregates.expected_stations) stations"
        }

        # After settle: zero missing wanted regions out to 1536 m.
        if ([int64]$analysis.farlod.regions_missing -gt 0) {
            throw "farlod horizon ($preset) has $($analysis.farlod.regions_missing) missing wanted far regions after settle"
        }
        if ([int64]$analysis.farlod.regions_resident -le 0 -or [int64]$analysis.farlod.regions_wanted -le 0) {
            throw "farlod horizon ($preset) reports no resident/wanted far regions - the far path did not run"
        }
        # Resident byte budget.
        if ([int64]$analysis.farlod.farlod_resident_bytes -le 0 -or
            [int64]$analysis.farlod.farlod_resident_bytes -ge [int64]$analysis.thresholds.resident_budget_bytes) {
            throw "farlod horizon ($preset) resident bytes $($analysis.farlod.farlod_resident_bytes) outside (0, $($analysis.thresholds.resident_budget_bytes))"
        }
        if ([int64]$analysis.farlod.far_region_draws -le 0 -or [int64]$analysis.farlod.far_indices_drawn -le 0) {
            throw "farlod horizon ($preset) drew no far region meshes at capture time"
        }

        # gbuffer GPU delta vs the in-run far-disabled baseline.
        if ([bool]$analysis.gbuffer.gpu_timers_supported) {
            if ([double]$analysis.gbuffer.baseline_gbuffer_gpu_ms -le 0) {
                throw "farlod horizon ($preset) recorded no far-disabled gbuffer baseline samples"
            }
            if ([double]$analysis.gbuffer.gbuffer_delta_ms -ge [double]$analysis.thresholds.max_gbuffer_delta_ms) {
                throw "farlod horizon ($preset) gbuffer GPU delta $($analysis.gbuffer.gbuffer_delta_ms) ms exceeds $($analysis.thresholds.max_gbuffer_delta_ms) ms (baseline $($analysis.gbuffer.baseline_gbuffer_gpu_ms), far $($analysis.gbuffer.far_gbuffer_gpu_ms))"
            }
        } else {
            Write-Host "farlod horizon ($preset): GPU timers unsupported on this context; gbuffer delta gate not applicable"
        }

        # Live/far boundary seam: at least one station must resolve the
        # boundary band, and every resolved band must be free of sky leaks
        # (where enforced) and strict void clusters.
        if ([int64]$analysis.aggregates.bands_resolved -le 0) {
            throw "farlod horizon ($preset) resolved no boundary-band ROI on any station"
        }
        $skyEnforced = [bool]$analysis.thresholds.sky_ratio_enforced
        foreach ($station in $analysis.stations) {
            Assert-PpmArtifact (Join-Path $viewDir $station.file)
            if ([bool]$station.boundary_band.resolved) {
                if ($skyEnforced -and [double]$station.boundary_band.band_sky_ratio -ge [double]$analysis.thresholds.max_boundary_band_sky_ratio) {
                    throw "farlod horizon ($preset) station '$($station.name)' shows a sky band at the live/far boundary: ratio $($station.boundary_band.band_sky_ratio)"
                }
                if ([int64]$station.boundary_band.void_cluster_count -gt 0) {
                    throw "farlod horizon ($preset) station '$($station.name)' has $($station.boundary_band.void_cluster_count) void clusters at the live/far boundary"
                }
            }
            if ($skyEnforced -and [double]$station.horizon.below_horizon_sky_ratio -ge [double]$analysis.thresholds.max_below_horizon_sky_ratio) {
                throw "farlod horizon ($preset) station '$($station.name)' shows sky below the horizon with far-LOD active: ratio $($station.horizon.below_horizon_sky_ratio)"
            }
            if (-not $station.passed) {
                throw "farlod horizon ($preset) station '$($station.name)' failed its thresholds"
            }
        }

        # Telemetry surfaces in last-known-runtime.json.
        $runtimeState = Get-Content (Join-Path $viewDir "last-known-runtime.json") -Raw | ConvertFrom-Json
        if ($null -eq $runtimeState.farlod -or $null -eq $runtimeState.farlod.farlod_resident_bytes) {
            throw "farlod horizon ($preset) last-known-runtime.json is missing the farlod telemetry section"
        }

        if (-not $analysis.passed) {
            throw "farlod horizon ($preset) analysis reported failure"
        }
        Write-Host ("farlod horizon ({0}): wanted={1} resident={2} missing={3} resident_bytes={4} draws={5} indices={6} gbuffer baseline={7}ms far={8}ms delta={9}ms max_sky={10} bands_resolved={11}" -f `
            $preset, $analysis.farlod.regions_wanted, $analysis.farlod.regions_resident, $analysis.farlod.regions_missing, `
            $analysis.farlod.farlod_resident_bytes, $analysis.farlod.far_region_draws, $analysis.farlod.far_indices_drawn, `
            $analysis.gbuffer.baseline_gbuffer_gpu_ms, $analysis.gbuffer.far_gbuffer_gpu_ms, $analysis.gbuffer.gbuffer_delta_ms, `
            $analysis.aggregates.max_below_horizon_sky_ratio, $analysis.aggregates.bands_resolved)
    }
}

function Test-HeadlessServerTick {
    # T-I3-13: headless server boot + fixed 30 Hz tick determinism gate.
    # Hygiene first (script-side mirror of the ServerHeadlessHygiene ctest so
    # the gate is self-contained): nothing under src/luminumbra_server may
    # include a client-side library. "glm" stays allowed, hence gl/gl +
    # opengl patterns instead of bare "gl".
    $serverRoot = "src/luminumbra_server"
    if (-not (Test-Path $serverRoot)) {
        throw "headless server gate: missing $serverRoot"
    }
    $forbiddenIncludeTokens = @("glfw", "glad", "imgui", "miniaudio", "rmlui", "rml/", "soil2", "opengl", "gl/gl", "gles", "luminumbra_client")
    $serverSources = @(Get-ChildItem -Path $serverRoot -Recurse -File | Where-Object { $_.Extension -in @(".h", ".hpp", ".cpp", ".inl", ".c") })
    if ($serverSources.Count -lt 1) {
        throw "headless server gate: no sources found under $serverRoot"
    }
    foreach ($sourceFile in $serverSources) {
        $includeMatches = Select-String -Path $sourceFile.FullName -Pattern '^\s*#\s*include\s*[<"]([^">]+)[">]'
        foreach ($includeMatch in $includeMatches) {
            $includeTarget = $includeMatch.Matches[0].Groups[1].Value.ToLowerInvariant()
            foreach ($token in $forbiddenIncludeTokens) {
                if ($includeTarget.Contains($token)) {
                    throw "headless server hygiene violation: $($sourceFile.FullName):$($includeMatch.LineNumber) includes forbidden client dependency '$($includeMatch.Matches[0].Groups[1].Value)'"
                }
            }
        }
    }
    Write-Host ("headless server hygiene: {0} server sources clean of client-library includes" -f $serverSources.Count)

    $serverExe = "build/$BuildPreset/bin/luminumbra_server_app.exe"
    if (-not (Test-Path $serverExe)) {
        throw "headless server gate not yet built - missing $serverExe (cmake --build build/$BuildPreset)"
    }

    $artifactPath = "build/$BuildPreset/test-artifacts/server/server-tick.json"
    if (Test-Path $artifactPath) {
        Remove-Item $artifactPath
    }

    & $serverExe --smoke --ticks 90 --artifact $artifactPath
    if ($LASTEXITCODE -ne 0) {
        throw "headless server smoke exited with code $LASTEXITCODE"
    }

    $analysis = Read-JsonArtifact $artifactPath "luminumbra.server_tick.v1"
    Assert-ArtifactPassed $analysis "HeadlessServerTick"
    if (-not $analysis.deterministic) {
        throw "headless server smoke reported a non-deterministic double-run"
    }
    if ([string]::IsNullOrEmpty($analysis.world_hash) -or [string]::IsNullOrEmpty($analysis.world_hash_replay)) {
        throw "headless server smoke produced an empty world hash"
    }
    if ($analysis.world_hash -ne $analysis.world_hash_replay) {
        throw "headless server world_hash mismatch: $($analysis.world_hash) != $($analysis.world_hash_replay)"
    }
    if ($analysis.tick_rate_hz -ne 30.0) {
        throw "headless server must tick at the canonical 30 Hz (got $($analysis.tick_rate_hz))"
    }
    if ($analysis.ticks_requested -lt 90) {
        throw "headless server smoke must run at least 90 ticks per determinism run (got $($analysis.ticks_requested))"
    }
    if (@($analysis.runs).Count -ne 2) {
        throw "headless server smoke must contain exactly two determinism runs (got $(@($analysis.runs).Count))"
    }
    foreach ($run in $analysis.runs) {
        if (-not $run.ok) {
            throw "headless server determinism run reported failure"
        }
        if ($run.ticks_executed -ne $analysis.ticks_requested) {
            throw "headless server run completed $($run.ticks_executed)/$($analysis.ticks_requested) ticks"
        }
        if ($run.frames_executed -ne $run.ticks_executed) {
            throw "headless server fixed loop must execute exactly one tick per frame ($($run.frames_executed) frames for $($run.ticks_executed) ticks)"
        }
        if ($run.chunks_streamed -lt 1) {
            throw "headless server streamed no chunks around the spawn anchor"
        }
    }
    Write-Host ("headless server tick gate passed: world_hash={0} == world_hash_replay, {1} ticks x 2 runs, {2} chunks streamed per run" -f `
        $analysis.world_hash, $analysis.ticks_requested, $analysis.runs[0].chunks_streamed)
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
    "GpuSdfCallbackSafetyGate" { Test-GpuSdfCallbackSafetyGate }
    "GpuSdfComputeParityGate" { Test-GpuSdfComputeParityGate }
    "GpuSdfRuntimeToggleGate" { Test-GpuSdfRuntimeToggleGate }
    "ChunkCollisionLifecycle" { Test-ChunkCollisionLifecycle }
    "PhysicsReplay" { Test-PhysicsReplay }
    "AudioNullTelemetry" { Test-AudioNullTelemetry }
    "AudioHandleApplication" { Test-AudioHandleApplication }
    "UiTestBaseline" { Test-UiTestBaseline }
    "SimulationEventBusOrderGate" { Test-SimulationEventBusOrderGate }
    "LuaApiManifestGate" { Test-LuaApiManifestGate }
    "AethericDiffusionGate" { Test-AethericDiffusionGate }
    "InstinctPlannerGate" { Test-InstinctPlannerGate }
    "PersistenceRoundtripGate" { Test-PersistenceRoundtripGate }
    "PersistenceRuntimeRoundtrip" { Test-PersistenceRuntimeRoundtrip }
    "ChunkFormatValidationGate" { Test-ChunkFormatValidationGate }
    "WorldHashEntitySnapshotGate" { Test-WorldHashEntitySnapshotGate }
    "NetworkLoopbackAuthorityGate" { Test-NetworkLoopbackAuthorityGate }
    "NetworkStateHash" { Test-NetworkStateHash }
    "PerfRegression" { Test-PerfRegression }
    "FrontierDisabled" { Test-FrontierDisabled }
    "SkyboxVisual" { Test-SkyboxVisual }
    "WeatherVisual" { Test-WeatherVisual }
    "TimeOfDaySweep" { Test-TimeOfDaySweep }
    "PlayerView" { Test-PlayerView }
    "FarLodHorizon" { Test-FarLodHorizon }
    "HeadlessServerTick" { Test-HeadlessServerTick }
    "All" {
        Test-CodexOnly
        Test-Files
        Test-Sections
        Test-Panels
        Test-RenderHealth
        Test-ShaderInventory
        Test-GpuSdfCallbackSafetyGate
        Test-GpuSdfComputeParityGate
        Test-GpuSdfRuntimeToggleGate
        Test-ChunkCollisionLifecycle
        Test-PhysicsReplay
        Test-AudioNullTelemetry
        Test-AudioHandleApplication
        Test-UiTestBaseline
        Test-SimulationEventBusOrderGate
        Test-LuaApiManifestGate
        Test-AethericDiffusionGate
        Test-InstinctPlannerGate
        Test-PersistenceRoundtripGate
        Test-ChunkFormatValidationGate
        Test-WorldHashEntitySnapshotGate
        Test-NetworkLoopbackAuthorityGate
        Test-NetworkStateHash
        Test-FrontierDisabled
    }
}

Write-Host "engine-frontier validation passed: $Mode"
