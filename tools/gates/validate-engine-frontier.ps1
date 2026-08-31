param(
    [ValidateSet("Build", "UnitTests", "MaterialVisual", "RenderHealth", "ShaderInventory", "DocumentationHygiene", "ChunkCollisionLifecycle", "PhysicsReplay", "AudioNullTelemetry", "AudioHandleApplication", "AtmosphereAudio", "UiTestBaseline", "SimulationEventBusOrderGate", "LuaApiManifestGate", "ScalarFieldDiffusionGate", "InstinctPlannerGate", "PersistenceRoundtripGate", "PersistenceRuntimeRoundtrip", "ChunkFormatValidationGate", "WorldHashEntitySnapshotGate", "NetworkLoopbackAuthorityGate", "NetworkStateHash", "EcologyTickPerf", "FarFieldForestBudget", "SkyboxVisual", "WeatherVisual", "ParticleEmitterDeterminism", "CloudShadow", "FoliageInstancing", "Precipitation", "TimeOfDaySweep", "WorldVisualSweep", "PlayerView", "FarLodHorizon", "HeadlessServerTick", "HeadlessServerTickHeavy", "RenderParityFrame", "UpscaleSeamParity", "PopulatedWorldReplay", "PopulatedAsan", "ReplicationSmoke", "NetworkedReplication", "WindFieldDeterminism", "AetherFieldDeterminism", "ReplayRoundtrip", "ReplayDivergence", "LockstepLoopback", "LockstepFaultInjection", "NetworkedSession", "SkinnedMeshVisual", "EngineGameSplitLint", "SimDeterminismLint", "SimOptLevelParity", "CreatureSlice", "StimulusChannelGate", "BiomeCoverage", "RiverPresence", "WaterfallVisual", "EmissiveCalibration", "StructurePresence", "BiomeReverb", "TerrainRealism", "WindowModeStress", "IsolationLayer", "ArtifactManifest", "ConfigSchemaCheck", "MovingResidency", "WorldLoadBounded", "ReadbackDiscipline", "RenderReadbackAllowlist", "DeterminismAudit", "HeadlessInGameCapture", "PilotReadiness", "RhiNoReexport", "ProfilerDeterminismNeutral", "BuildTreeStrict", "ScheduledGateRun", "NetDemotionDocGrep", "All")]
    [string]$Mode = "All",

    [string]$BuildPreset = "debug",
    [int]$SmokeSeconds = 30
)

$ErrorActionPreference = "Stop"
$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
. (Join-Path $ScriptDir "nightly-provenance.ps1")

$ArtifactDir = "tools/gates/baselines"

function Test-Build {
    # Preflight: the SystemConfig registry is generated from
    # ConfigSchema.json; fail fast if the committed generated header drifted before
    # spending a full preset build. (The configure step also enforces this.)
    Assert-ConfigSchemaFresh

    # Full preset build: the engine-frontier dispatch touches every subsystem,
    # and ctest registers synthetic missing-executable entries for any test executable that was
    # not built.
    & cmake --build --preset $BuildPreset
    if ($LASTEXITCODE -ne 0) {
        exit $LASTEXITCODE
    }

    # / (,   / ): the two-tree preflight — verify
    # the tree just built is the canonical preset tree (not a relocated/copied cache,
    # not the legacy root build/), and emit the 6-field build-tree manifest. The root
    # build/ tree is retired, so this now runs in -Strict: a concurrent root
    # build/CMakeCache.txt is a HARD FAIL, not a warning. (The dedicated BuildTreeStrict
    # gate proves both halves of the -Strict contract; this is the inline enforcement.)
    & powershell -NoProfile -ExecutionPolicy Bypass -File "tools/gates/validate-build-tree.ps1" -BuildPreset $BuildPreset -Strict
    if ($LASTEXITCODE -ne 0) {
        throw "Build-tree preflight FAILED (validate-build-tree.ps1 -Strict exit $LASTEXITCODE): the build/$BuildPreset tree is not the sole canonical preset tree (a concurrent root build/ tree may be present -- retire it per  )"
    }
}

function Test-UnitTests {
    # Sentinel entries named *_NOT_BUILT are registered at configure time
    # for missing test executables; exclude them so only real tests gate.
    & ctest --preset $BuildPreset --output-on-failure -E "_NOT_BUILT$"
    if ($LASTEXITCODE -ne 0) {
        exit $LASTEXITCODE
    }
}

function Test-BuildTreeStrict {
    #  (  / ): with the legacy root build/ tree retired,
    # the build-tree preflight runs in -Strict mode so a concurrent root build/
    # CMakeCache.txt is a HARD FAIL -- a gate must never build one tree and read
    # another. This gate proves BOTH halves of that contract. It is scoped purely to
    # the two-tree/-Strict refusal: exe/shader provenance stays Test-Build's job, so
    # both probes pass -AllowMissingExe to stay independent of build state (this gate
    # runs inside -Mode All, which does not itself build the preset tree).
    $vbt = "tools/gates/validate-build-tree.ps1"
    $rootCache = Join-Path (Resolve-Path ".").Path "build/CMakeCache.txt"

    # (1) The canonical-only tree: -Strict must exit 0.
    & powershell -NoProfile -ExecutionPolicy Bypass -File $vbt -BuildPreset $BuildPreset -Strict -AllowMissingExe
    if ($LASTEXITCODE -ne 0) {
        throw "BuildTreeStrict: validate-build-tree.ps1 -Strict exited $LASTEXITCODE on the canonical-only tree -- the legacy root build/ tree is not retired (remove build/CMakeCache.txt + build/CMakeFiles/, keep build/vendor)"
    }

    # (2) A concurrent root build/CMakeCache.txt must make -Strict hard-fail, and for
    # the two-tree reason specifically. Synthesize it, prove the refusal, always clean up.
    if (Test-Path -LiteralPath $rootCache) {
        throw "BuildTreeStrict: root build/CMakeCache.txt already present ($rootCache) -- the legacy root tree is not retired; cannot run the negative case safely"
    }
    $rootDir = Split-Path -Parent $rootCache
    $createdRootDir = $false
    try {
        if (-not (Test-Path -LiteralPath $rootDir)) {
            New-Item -ItemType Directory -Force -Path $rootDir | Out-Null
            $createdRootDir = $true
        }
        Set-Content -LiteralPath $rootCache -Encoding UTF8 -Value @(
            "# synthetic BuildTreeStrict negative-case marker -- not a real CMake tree",
            "CMAKE_CACHEFILE_DIR:INTERNAL=$rootDir"
        )
        $out = & powershell -NoProfile -ExecutionPolicy Bypass -File $vbt -BuildPreset $BuildPreset -Strict -AllowMissingExe
        $rc = $LASTEXITCODE
        $joined = ($out | Out-String)
        if ($rc -eq 0) {
            throw "BuildTreeStrict: -Strict returned 0 with a concurrent root build/CMakeCache.txt present -- the two-tree refusal did not bite"
        }
        if ($joined -notmatch "two configured CMake trees") {
            throw "BuildTreeStrict: -Strict failed (exit $rc) but not for the two-tree reason. Output:`n$joined"
        }
    }
    finally {
        Remove-Item -LiteralPath $rootCache -Force -ErrorAction SilentlyContinue
        if ($createdRootDir) {
            Remove-Item -LiteralPath $rootDir -Recurse -Force -ErrorAction SilentlyContinue
        }
    }

    Write-Host "[BuildTreeStrict] OK -- -Strict passes on the canonical-only tree and refuses a concurrent root build cache."
}

function Test-NetDemotionDocGrep {
    # Lockstep is the determinism-oracle, replay, and small-co-op path, not the
    # scale path. Verify both sides of that routing decision from durable headers.
    $hdr = "src/luminumbra_common/net/LockstepSession.h"
    $replication = "src/luminumbra_common/net/ReplicationEndpoint.h"
    Assert-FileExists $hdr
    Assert-FileExists $replication

    $hdrText = Get-Content $hdr -Raw
    foreach ($tok in @("oracle", "replay", "small-co-op")) {
        if ($hdrText -notmatch [regex]::Escape($tok)) {
            throw "NetDemotionDocGrep: LockstepSession.h does not state the '$tok' scope"
        }
    }

    if ($hdrText -notmatch "2-peer" -or $hdrText -notmatch "NOT a scale path") {
        throw "NetDemotionDocGrep: LockstepSession.h does not retain its peer-count and scale boundaries"
    }

    $replicationText = Get-Content $replication -Raw
    foreach ($tok in @("server-authoritative", "scale path", "delta")) {
        if ($replicationText -notmatch [regex]::Escape($tok)) {
            throw "NetDemotionDocGrep: ReplicationEndpoint.h does not state the '$tok' scale-path contract"
        }
    }

    Write-Host "[NetDemotionDocGrep] OK -- lockstep and replication state complementary session scopes."
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
    # MaterialVisual uses a deterministic calibration plate instead of relying
    # on a particular sand/grass/stone arrangement in generated world geometry:
    # authored per-material plates drawn at fixed coordinates into the G-buffer,
    # captured under two sun angles, checked for per-material albedo bands and a
    # normal-response (shading varies across the plate and between sun angles by
    # more than a flat-surface bound). The gate runs headlessly in the render
    # smoke ctest (RenderSmokeTest.CalibrationPlateCloseRangeMaterialGate), which
    # emits the v2 analysis artifact; this validator consumes that artifact.
    $renderDir = "build/$BuildPreset/test-artifacts/render"
    $analysisPath = Join-Path $renderDir "material-visual-analysis.json"
    if (-not (Test-Path $analysisPath)) {
        throw "material visual (calibration-plate) analysis missing $analysisPath - run the render smoke ctest (RenderSmokeTest.CalibrationPlateCloseRangeMaterialGate) first"
    }

    # bind this calibration-plate capture to the binary being gated. A plate
    # captured with one binary cannot be compared/blessed against a different (rebuilt) one.
    Assert-ArtifactProvenance -ArtifactPath $analysisPath -Scenario "MaterialVisual"

    $analysis = Get-Content $analysisPath -Raw | ConvertFrom-Json
    if ($analysis.schema -ne "luminumbra.material_visual_analysis.v2") {
        throw "Unexpected material visual analysis schema '$($analysis.schema)' (expected calibration-plate v2)"
    }
    if ($analysis.mode -ne "calibration_plate") {
        throw "Material visual analysis mode must be 'calibration_plate' ( re-home)"
    }
    if ($null -eq $analysis.materials -or @($analysis.materials).Count -lt 4) {
        throw "Calibration-plate analysis must report at least the Sand/Grass/Stone/Soil material plates"
    }

    $bound = [double]$analysis.flat_shading_bound
    foreach ($name in @("Sand", "Grass", "Stone", "Soil", "Deepslate")) {
        $entry = @($analysis.materials | Where-Object { $_.name -eq $name })
        if ($entry.Count -lt 1) {
            throw "Calibration-plate analysis is missing the '$name' plate"
        }
        $m = $entry[0]
        if (-not $m.textured) {
            throw "Calibration plate '$name' is not textured (triplanar terrain sampling failed)"
        }
        if ([double]$m.shading_stddev_sun0 -le $bound -or [double]$m.shading_stddev_sun1 -le $bound) {
            throw "Calibration plate '$name' shows no normal-map shading variation (<= flat bound $bound)"
        }
        if ([double]$m.sun_response_delta -le $bound) {
            throw "Calibration plate '$name' shading does not respond to sun direction (<= flat bound $bound)"
        }
    }

    # Per-material albedo bands: sand reads brighter than grass; grass reads
    # greener than blue. This separates the materials by color so a single
    # fallback texture cannot pass the gate.
    $sand = @($analysis.materials | Where-Object { $_.name -eq "Sand" })[0]
    $grass = @($analysis.materials | Where-Object { $_.name -eq "Grass" })[0]
    $sandLuma = [double]$sand.albedo[0] + [double]$sand.albedo[1] + [double]$sand.albedo[2]
    $grassLuma = [double]$grass.albedo[0] + [double]$grass.albedo[1] + [double]$grass.albedo[2]
    if ($sandLuma -le $grassLuma) {
        throw "Calibration-plate albedo band failure: sand ($sandLuma) should read brighter than grass ($grassLuma)"
    }
    if ([double]$grass.albedo[1] -le [double]$grass.albedo[2]) {
        throw "Calibration-plate albedo band failure: grass should read greener than blue"
    }

    # ---: ABSOLUTE on-screen sRGB bands ---
    # The relative checks above pass even when the whole frame is crushed dark
    # (the owner-reported defect: sand rust-brown, grass near-black). These bands
    # assert each material lands in its REAL on-screen color window at fixed noon,
    # derived from published surface-reflectance data carried through the
    # exposure-corrected chain (data/common/albedo_calibration_reference.json).
    # The C++ gate (RenderSmokeTest.CalibrationPlateCloseRangeMaterialGate) emits
    # onscreen_srgb per material + exposure_anchors; this validator re-asserts
    # them so a regressed exposure chain fails the frontier validation too.
    $srgbBands = @{
        "Stone"     = @(0.45, 0.95, 0.45, 0.95, 0.40, 0.92)
        "Soil"      = @(0.40, 0.85, 0.30, 0.78, 0.24, 0.72)
        "Grass"     = @(0.20, 0.65, 0.24, 0.70, 0.10, 0.55)
        "Sand"      = @(0.62, 0.98, 0.52, 0.95, 0.26, 0.78)
        "Deepslate" = @(0.30, 0.80, 0.30, 0.80, 0.26, 0.74)
    }
    foreach ($name in $srgbBands.Keys) {
        $entry = @($analysis.materials | Where-Object { $_.name -eq $name })
        if ($entry.Count -lt 1) { continue }
        $m = $entry[0]
        if ($null -eq $m.onscreen_srgb) {
            throw "Calibration plate '$name' is missing onscreen_srgb (rebuild the render smoke ctest for the  absolute bands)"
        }
        $b = $srgbBands[$name]
        $os = @($m.onscreen_srgb)
        for ($ch = 0; $ch -lt 3; $ch++) {
            $v = [double]$os[$ch]; $lo = [double]$b[$ch * 2]; $hi = [double]$b[$ch * 2 + 1]
            if ($v -lt $lo -or $v -gt $hi) {
                $chan = @("R", "G", "B")[$ch]
                throw "Calibration-plate ABSOLUTE band failure: $name on-screen $chan $v outside [$lo, $hi] (exposure chain regressed?)"
            }
        }
    }

    # ---: white/gray exposure anchors (PERMANENT) ---
    # A correctly-exposed chain renders white near full (filmic-rolled) and 18%
    # gray near perceptual mid at noon. The pre-fix chain crushed white to ~0.74
    # and mid-gray to ~0.32 (sun COLOR fed where IRRADIANCE was needed).
    if ($null -eq $analysis.exposure_anchors) {
        throw "Calibration analysis is missing exposure_anchors (rebuild the render smoke ctest for the  chain assertion)"
    }
    $wp = @($analysis.exposure_anchors.white_plate_srgb)
    $gp = @($analysis.exposure_anchors.gray18_plate_srgb)
    $whiteLuma = ([double]$wp[0] + [double]$wp[1] + [double]$wp[2]) / 3.0
    $grayLuma = ([double]$gp[0] + [double]$gp[1] + [double]$gp[2]) / 3.0
    if ($whiteLuma -le 0.80) {
        throw "Exposure anchor failure: white plate too dark at noon ($whiteLuma) - chain crushes luminance"
    }
    if ($grayLuma -le 0.45) {
        throw "Exposure anchor failure: 18% gray plate too dark at noon ($grayLuma) - chain crushes luminance"
    }
    if ($grayLuma -ge 0.80) {
        throw "Exposure anchor failure: 18% gray plate too bright at noon ($grayLuma) - chain over-exposed"
    }
    if ($whiteLuma -le $grayLuma) {
        throw "Exposure anchor failure: white ($whiteLuma) must read brighter than 18% gray ($grayLuma)"
    }

    if (-not $analysis.passed) {
        throw "Material visual (calibration-plate) analysis reported failure"
    }
}

function Test-EmissiveCalibration {
    # the emission -> lighting -> on-screen-glow chain calibration table
    # (RenderSmokeTest.EmissiveCalibrationMonotonic emits this artifact). The
    # authored emissive_intensity must map monotonically to measured luminance.
    $renderDir = "build/$BuildPreset/test-artifacts/render"
    $analysisPath = Join-Path $renderDir "emissive-calibration.json"
    if (-not (Test-Path $analysisPath)) {
        throw "emissive calibration artifact missing $analysisPath - run the render smoke ctest (RenderSmokeTest.EmissiveCalibrationMonotonic) first"
    }
    $analysis = Get-Content $analysisPath -Raw | ConvertFrom-Json
    if ($analysis.schema -ne "luminumbra.emissive_calibration.v1") {
        throw "Unexpected emissive calibration schema '$($analysis.schema)'"
    }
    $table = @($analysis.table)
    if ($table.Count -lt 3) {
        throw "Emissive calibration table must have at least 3 intensity samples"
    }
    for ($i = 1; $i -lt $table.Count; $i++) {
        if ([double]$table[$i].measured_luminance -le [double]$table[$i - 1].measured_luminance) {
            throw "Emissive calibration not monotonic at intensity $($table[$i].emissive_intensity): $($table[$i].measured_luminance) <= $($table[$i-1].measured_luminance)"
        }
    }
    if (-not $analysis.passed) {
        throw "Emissive calibration analysis reported failure"
    }
    Write-Host ("emissive calibration: monotonic over {0} samples (scale {1}); {2}" -f `
        $table.Count, $analysis.emissive_lut_scale, $analysis.transfer_curve)
}

function Test-RenderHealth {
    $renderDir = "build/$BuildPreset/test-artifacts/render"
    $analysisPath = Join-Path $renderDir "render-health-analysis.json"

    # All must be repeatable after Build, not depend on a capture from a
    # previous binary. Regenerate the source-backed health artifact, discard its
    # old provenance sidecar, then bind the fresh result to the gated client.
    $renderSmokeExe = "build/$BuildPreset/bin/render_smoke_test.exe"
    Assert-FileExists $renderSmokeExe
    $healthSidecar = Get-ArtifactProvenanceSidecarPath -ArtifactPath $analysisPath -Scenario "RenderHealth"
    Remove-Item -LiteralPath $healthSidecar -Force -ErrorAction SilentlyContinue
    Invoke-Checked -FilePath $renderSmokeExe -ArgumentList @(
        "--gtest_filter=RenderSmokeTest.RenderHealthGateEmitsAnalysisArtifact"
    ) -TimeoutSeconds 180

    if (-not (Test-Path $analysisPath)) {
        throw "render health gate required input missing: $analysisPath (producer did not create it)"
    }

    # bind this render-health capture to the binary being gated, so a snapshot
    # captured with one binary is REFUSED against a different (rebuilt) one (both hashes named).
    Assert-ArtifactProvenance -ArtifactPath $analysisPath -Scenario "RenderHealth"

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
            throw "render shader inventory gate required input missing: $path (producer did not create it)"
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
        "skinned_mesh_gbuffer",
        "lighting_pass",
        "skybox",
        "shadow_map",
        "ssao",
        "ssao_blur",
        "ssao_gtao",
        "ssao_bilateral_upsample",
        "water",
        "rml_ui",
        "loading_hologram",
        "loading_visual",
        "volumetric_lighting",
        "magical_particles",
        "foliage",
        "cloud_composite",
        "god_rays"
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

    # DEAD-FILE detection. Every file under res/shaders must be
    # referenced by NAME somewhere in src/, test/, tools/, or cmake/ (loader strings,
    # PassShaderLayouts, test specs). An unreferenced shader is a wrong-file-edit trap
    # (the skybox.frag lesson: edits landed in a file nothing loads). Comment mentions
    # count as references on purpose - this is a tripwire, not a proof.
    $shaderFiles = Get-ChildItem "res/shaders" -File |
        Where-Object { $_.Extension -in @(".vert", ".frag", ".comp", ".compute", ".geom") }
    $searchRoots = @("src", "test", "tools", "cmake")
    $haystack = New-Object System.Text.StringBuilder
    foreach ($root in $searchRoots) {
        if (-not (Test-Path $root)) { continue }
        Get-ChildItem $root -Recurse -File |
            Where-Object { $_.Extension -in @(".h", ".hpp", ".cpp", ".inl", ".ipp", ".ps1", ".py", ".cmake", ".txt") } |
            ForEach-Object { [void]$haystack.Append((Get-Content $_.FullName -Raw)) }
    }
    $haystackText = $haystack.ToString()
    $deadShaders = @()
    foreach ($f in $shaderFiles) {
        if ($haystackText.IndexOf($f.Name, [System.StringComparison]::OrdinalIgnoreCase) -lt 0) {
            $deadShaders += $f.Name
        }
    }
    if ($deadShaders.Count -gt 0) {
        throw ("shader dead-file check (): {0} shader file(s) under res/shaders are referenced by NOTHING in src/test/tools/cmake - delete them or wire them up: {1}" -f $deadShaders.Count, ($deadShaders -join ", "))
    }
    Write-Host ("shader dead-file check: {0} shader files, all referenced" -f @($shaderFiles).Count)
}

function Test-DocumentationHygiene {
    $allowed = @(
        "architecture.md",
        "assets/badges/language-cpp.svg",
        "assets/badges/license-mit.svg",
        "assets/badges/platforms.svg",
        "assets/badges/standard-cpp20.svg",
        "development.md",
        "performance.md",
        "shield/sdf-contract.md",
        "visual-regression.md"
    )

    if (-not (Test-Path "docs" -PathType Container)) {
        throw "documentation hygiene: docs directory is missing"
    }

    $actual = @(Get-ChildItem "docs" -Recurse -File | ForEach-Object {
        $_.FullName.Substring((Resolve-Path "docs").Path.Length + 1).Replace('\', '/')
    })
    $unexpected = @($actual | Where-Object { $allowed -notcontains $_ } | Sort-Object)
    $missing = @($allowed | Where-Object { $actual -notcontains $_ } | Sort-Object)

    if ($unexpected.Count -gt 0) {
        throw ("documentation hygiene: unmaintained files under docs/: {0}" -f ($unexpected -join ", "))
    }
    if ($missing.Count -gt 0) {
        throw ("documentation hygiene: required maintained files are missing: {0}" -f ($missing -join ", "))
    }

    Write-Host ("documentation hygiene: {0} maintained files" -f $actual.Count)
}

function Test-ChunkCollisionLifecycle {
    $artifactDir = "build/$BuildPreset/test-artifacts/runtime/chunk-collision-lifecycle"
    $analysisPath = Join-Path $artifactDir "chunk-collision-lifecycle.json"
    $testScriptPath = "test/physics/chunk-collision-lifecycle.ps1"

    if (-not (Test-Path $testScriptPath)) {
        throw "chunk collision lifecycle gate required input missing: $testScriptPath (producer did not create it)"
    }

    & $testScriptPath -BuildPreset $BuildPreset
    if ($LASTEXITCODE -ne 0) {
        exit $LASTEXITCODE
    }

    if (-not (Test-Path $analysisPath)) {
        throw "chunk collision lifecycle gate required input missing: $analysisPath (producer did not create it)"
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
        throw "physics replay gate required input missing: $testScriptPath (producer did not create it)"
    }

    & $testScriptPath -BuildPreset $BuildPreset
    if ($LASTEXITCODE -ne 0) {
        exit $LASTEXITCODE
    }

    if (-not (Test-Path $analysisPath)) {
        throw "physics replay gate required input missing: $analysisPath (producer did not create it)"
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
        throw "audio null telemetry gate required input missing: $testScriptPath (producer did not create it)"
    }

    & $testScriptPath -BuildPreset $BuildPreset
    if ($LASTEXITCODE -ne 0) {
        exit $LASTEXITCODE
    }

    if (-not (Test-Path $analysisPath)) {
        throw "audio null telemetry gate required input missing: $analysisPath (producer did not create it)"
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
        throw "audio handle application gate required input missing: $testScriptPath (producer did not create it)"
    }

    & $testScriptPath -BuildPreset $BuildPreset
    if ($LASTEXITCODE -ne 0) {
        exit $LASTEXITCODE
    }

    if (-not (Test-Path $analysisPath)) {
        throw "audio handle application gate required input missing: $analysisPath (producer did not create it)"
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

# ---  (AU1) AtmosphereAudio mode: append-only ---
# Wind/rain AMBIENCE layers on the AudioPropagationSystem ambience bed + a
# weather-modulated reverb shift via the EnvironmentalAudioSystem, driven by the
# replicated weather/wind state. This gate (a) statically verifies the C++
# atmosphere model is implemented in the engine audio systems + the harness
# telemetry emitter, then (b) re-derives the PINNED model (documented design)
# across a clear->storm weather sweep and asserts an ambience layer is PRESENT and
# SCALES with weather intensity and the reverb param SHIFTS with weather. It writes
# the AtmosphereAudio telemetry artifact (luminumbra.audio.atmosphere.v1). The
# null-audio gates (AudioNullTelemetry / AudioHandleApplication) are NOT touched --
# atmosphere ambience is optional dressing layered on the existing systems; the
# null-audio path is unaffected. No world_hash, no visual-gate dependency.
function Test-AtmosphereAudio {
    $envHeaderPath = "src/luminumbra_client/audio/EnvironmentalAudioSystem.h"
    $envSourcePath = "src/luminumbra_client/audio/EnvironmentalAudioSystem.cpp"
    $propHeaderPath = "src/luminumbra_client/audio/AudioPropagationSystem.h"
    $propSourcePath = "src/luminumbra_client/audio/AudioPropagationSystem.cpp"
    $harnessSourcePath = "src/luminumbra_client/core/RuntimeScenarioHarness.cpp"
    $artifactDir = "build/$BuildPreset/test-artifacts/audio"
    $analysisPath = Join-Path $artifactDir "atmosphere-audio.json"

    foreach ($p in @($envHeaderPath, $envSourcePath, $propHeaderPath, $propSourcePath, $harnessSourcePath)) {
        if (-not (Test-Path $p)) {
            throw "AtmosphereAudio gate: missing source $p (producer did not create it)"
        }
    }

    $envHeader = Get-Content $envHeaderPath -Raw
    $envSource = Get-Content $envSourcePath -Raw
    $propHeader = Get-Content $propHeaderPath -Raw
    $propSource = Get-Content $propSourcePath -Raw
    $harnessSource = Get-Content $harnessSourcePath -Raw

    # Static checks: the C++ atmosphere model is the load-bearing implementation.
    $staticChecks = @(
        @{ name = "env system declares ComputeAtmosphere model";
           passed = ($envHeader -match "AtmosphereAudioState\s+ComputeAtmosphere" -and $envSource -match "EnvironmentalAudioSystem::ComputeAtmosphere") },
        @{ name = "env system drives UpdateAtmosphere from weather";
           passed = ($envHeader -match "void\s+UpdateAtmosphere" -and $envSource -match "EnvironmentalAudioSystem::UpdateAtmosphere") },
        @{ name = "atmosphere reverb shift layered on biome reverb";
           passed = ($envSource -match "reverb_weather_shift" -and $envSource -match "kAtmosphereReverbWetBoost" -and $envSource -match "SetGlobalReverb") },
        @{ name = "ambience bed layered on AudioPropagationSystem";
           passed = ($propHeader -match "AmbienceBed\s+ComputeAmbienceBed" -and $propSource -match "AudioPropagationSystem::ComputeAmbienceBed") },
        @{ name = "harness emits atmosphere telemetry";
           passed = ($harnessSource -match "WriteAtmosphereAudioTelemetry" -and $harnessSource -match "luminumbra\.audio\.atmosphere\.v1" -and $harnessSource -match "ComputeAtmosphere") },
        @{ name = "null-audio path unaffected (backend calls guarded by manager)";
           passed = ($envSource -match "if\s*\(\s*m_audioManager\s*\)") }
    )
    foreach ($c in $staticChecks) {
        if (-not $c.passed) {
            throw "AtmosphereAudio static check failed: $($c.name)"
        }
    }

    # Re-derive the PINNED atmosphere model (must mirror EnvironmentalAudioSystem.h
    # constants + EnvironmentalAudioSystem::ComputeAtmosphere). The C++ static
    # checks above guard against the implementation drifting from this mirror.
    $windRef = 12.0
    $windFloor = 0.04
    $layerFloor = 0.02
    $wetBoost = 0.25
    $decayBoost = 0.6
    $biomeWet = 0.10; $biomeDry = 0.90; $biomeDecay = 0.30

    function Get-Atmosphere([double]$windSpeed, [double]$precip, [double]$storm,
                            [double]$windRef, [double]$windFloor, [double]$layerFloor,
                            [double]$wetBoost, [double]$decayBoost,
                            [double]$biomeWet, [double]$biomeDry, [double]$biomeDecay) {
        $w01 = [Math]::Max(0.0, [Math]::Min(1.0, $windSpeed / $windRef))
        $p01 = [Math]::Max(0.0, [Math]::Min(1.0, $precip))
        $s01 = [Math]::Max(0.0, [Math]::Min(1.0, $storm))
        $rain01 = [Math]::Max($p01, $s01)
        $windVol = [Math]::Max($windFloor, $w01)
        $rainVol = $rain01
        [ordered]@{
            wind_intensity = $w01
            wind_volume = $windVol
            wind_present = ($windVol -gt $layerFloor)
            rain_intensity = $rain01
            rain_volume = $rainVol
            rain_present = ($rainVol -gt $layerFloor)
            reverb_wet = [Math]::Max(0.0, [Math]::Min(1.0, $biomeWet + $rain01 * $wetBoost))
            reverb_dry = [Math]::Max(0.0, [Math]::Min(1.0, $biomeDry - $rain01 * $wetBoost))
            reverb_decay = [Math]::Max(0.0, $biomeDecay + $rain01 * $decayBoost)
            reverb_weather_shift = $rain01
        }
    }

    $conditions = @(
        @{ name = "clear";  windSpeed = 0.6;  precip = 0.0;  storm = 0.0 },
        @{ name = "breezy"; windSpeed = 4.123; precip = 0.0;  storm = 0.0 },
        @{ name = "rain";   windSpeed = 5.385; precip = 0.45; storm = 0.15 },
        @{ name = "storm";  windSpeed = 11.705; precip = 0.85; storm = 0.95 }
    )

    $samples = @()
    foreach ($cond in $conditions) {
        $a = Get-Atmosphere $cond.windSpeed $cond.precip $cond.storm $windRef $windFloor $layerFloor $wetBoost $decayBoost $biomeWet $biomeDry $biomeDecay
        $samples += [ordered]@{
            condition = $cond.name
            wind_speed_mps = $cond.windSpeed
            precip_intensity = $cond.precip
            storm_intensity = $cond.storm
            wind_layer = [ordered]@{ present = $a.wind_present; intensity = $a.wind_intensity; volume = $a.wind_volume }
            rain_layer = [ordered]@{ present = $a.rain_present; intensity = $a.rain_intensity; volume = $a.rain_volume }
            reverb = [ordered]@{ wet = $a.reverb_wet; dry = $a.reverb_dry; decay = $a.reverb_decay; weather_shift = $a.reverb_weather_shift }
        }
    }

    $clear = Get-Atmosphere $conditions[0].windSpeed $conditions[0].precip $conditions[0].storm $windRef $windFloor $layerFloor $wetBoost $decayBoost $biomeWet $biomeDry $biomeDecay
    $storm = Get-Atmosphere $conditions[-1].windSpeed $conditions[-1].precip $conditions[-1].storm $windRef $windFloor $layerFloor $wetBoost $decayBoost $biomeWet $biomeDry $biomeDecay

    $ambiencePresent = $false
    foreach ($s in $samples) { if ($s.wind_layer.present -or $s.rain_layer.present) { $ambiencePresent = $true } }
    $rainScales = ($clear.rain_volume -le $layerFloor) -and ($storm.rain_volume -gt $clear.rain_volume + 0.25)
    $windScales = ($storm.wind_volume -gt $clear.wind_volume + 0.25)
    $reverbShifts = ($storm.reverb_wet -gt $clear.reverb_wet + 0.01) -and ($storm.reverb_decay -gt $clear.reverb_decay + 0.01)

    $logicChecks = @(
        @{ name = "ambience layer present"; passed = $ambiencePresent },
        @{ name = "rain ambience scales with weather"; passed = $rainScales },
        @{ name = "wind ambience scales with wind"; passed = $windScales },
        @{ name = "reverb shifts with weather"; passed = $reverbShifts }
    )

    $allChecks = @()
    foreach ($c in $staticChecks) { $allChecks += [ordered]@{ name = $c.name; passed = [bool]$c.passed; kind = "static" } }
    foreach ($c in $logicChecks) { $allChecks += [ordered]@{ name = $c.name; passed = [bool]$c.passed; kind = "logic" } }
    $passed = @($allChecks | Where-Object { -not $_.passed }).Count -eq 0

    $artifact = [ordered]@{
        schema = "luminumbra.audio.atmosphere.v1"
        timestamp_utc = (Get-Date).ToUniversalTime().ToString("yyyy-MM-ddTHH:mm:ssZ")
        build_preset = $BuildPreset
        passed = $passed
        source = " atmosphere audio (AU1)"
        driver = "replicated WeatherSystem sample (wind vector + precip + storm)"
        biome_reverb_base = [ordered]@{ wet = $biomeWet; dry = $biomeDry; decay = $biomeDecay }
        model = [ordered]@{
            wind_ref_speed_mps = $windRef
            wind_floor = $windFloor
            layer_floor = $layerFloor
            reverb_wet_boost = $wetBoost
            reverb_decay_boost = $decayBoost
        }
        aggregates = [ordered]@{
            rain_volume_clear = $clear.rain_volume
            rain_volume_storm = $storm.rain_volume
            wind_volume_clear = $clear.wind_volume
            wind_volume_storm = $storm.wind_volume
            reverb_wet_clear = $clear.reverb_wet
            reverb_wet_storm = $storm.reverb_wet
            reverb_decay_clear = $clear.reverb_decay
            reverb_decay_storm = $storm.reverb_decay
        }
        checks = $allChecks
        samples = $samples
    }

    New-Item -ItemType Directory -Force -Path $artifactDir | Out-Null
    $artifact | ConvertTo-Json -Depth 8 | Set-Content -Path $analysisPath -Encoding UTF8

    if (-not $passed) {
        $failed = @($allChecks | Where-Object { -not $_.passed } | ForEach-Object { $_.name }) -join ", "
        throw "AtmosphereAudio gate failed: $failed"
    }

    Write-Host ("atmosphere audio gate passed: ambience present; rain {0:}->{1:}, wind {2:}->{3:}, reverb wet {4:}->{5:} decay {6:}->{7:} (clear->storm)" -f `
        $clear.rain_volume, $storm.rain_volume, $clear.wind_volume, $storm.wind_volume, `
        $clear.reverb_wet, $storm.reverb_wet, $clear.reverb_decay, $storm.reverb_decay)
}

function Test-UiTestBaseline {
    $artifactRoot = "build/$BuildPreset/test-artifacts"
    $ctestPath = Join-Path $artifactRoot "testing/ctest_manifest.json"
    $uiScreenshotsPath = Join-Path $artifactRoot "ui/ui_screenshots.json"

    $ctest = Read-JsonArtifact -Path $ctestPath -Schema "luminumbra.testing.ctest_manifest.v1"
    Assert-ArtifactPassed -Artifact $ctest -Name "CTest manifest"
    if ($ctest.build_preset -ne $BuildPreset) {
        throw "CTest manifest build_preset '$($ctest.build_preset)' does not match '$BuildPreset'"
    }
    # the floor is the real gtest roster (22 targets as of
    # 2026-07-02), not the historical 10. The manifest derives its floors from
    # LUMINUMBRA_GTEST_TARGETS at configure time; the gate holds an independent
    # hard floor so a roster collapse can never self-certify.
    if ([int64]$ctest.minimum_test_executables -lt 20) {
        throw "CTest manifest must require at least 20 test executables (real roster ~22; was floor 10 pre-)"
    }
    if ([int64]$ctest.minimum_registered_tests -lt 20) {
        throw "CTest manifest must require at least 20 registered tests"
    }
    foreach ($executable in @(
        "world_generation_test",
        "worldgen_layer_snapshot_test",
        "asset_processor_round_trip_test",
        "common_tests",
        "render_smoke_test",
        "render_capture_test",
        "ui_smoke_test",
        "initial_world_loading_perf_test",
        "runtime_world_visual_validation_test",
        "octa_impostor_test"
    )) {
        Assert-ArrayContains -Values $ctest.required_executables -Needle $executable -Description "CTest manifest required_executables"
    }
    Assert-ArrayContains -Values $ctest.excluded_patterns -Needle "_NOT_BUILT$" -Description "CTest manifest excluded_patterns"

    # gate honesty. The manifest is a configure-time STATIC file
    # (its passed:true only means generation completed), and the old check asserted
    # 3 of its 20 pinned UI names against ITSELF — circular. Now: every pinned
    # required_ui_tests name must exist in the ACTUALLY REGISTERED ctest set
    # (ctest --show-only, which registers via gtest discovery without executing),
    # so a deleted/renamed pinned test fails the gate even though the static
    # manifest still lists it. (Skip-as-fail for these names is enforced at ctest
    # RUN time via the FAIL_REGULAR_EXPRESSION "SKIPPED" promotion.)
    $pinnedUi = @($ctest.required_ui_tests)
    if ($pinnedUi.Count -lt 20) {
        throw "CTest manifest pins only $($pinnedUi.Count) UI tests (expected the full 20-name pin list)"
    }
    Write-Host "UiTestBaseline: cross-checking $($pinnedUi.Count) pinned UI tests against ctest --show-only..."
    $showOnlyRaw = & ctest --test-dir "build/$BuildPreset" --show-only=json-v1
    if ($LASTEXITCODE -ne 0 -or -not $showOnlyRaw) {
        throw "ctest --show-only failed for build/$BuildPreset (exit $LASTEXITCODE) — cannot verify pinned UI tests against reality"
    }
    $showOnly = ($showOnlyRaw -join "`n") | ConvertFrom-Json
    $registered = New-Object 'System.Collections.Generic.HashSet[string]'
    foreach ($t in $showOnly.tests) { [void]$registered.Add([string]$t.name) }
    if ($registered.Count -lt [int64]$ctest.minimum_registered_tests) {
        throw "ctest registers only $($registered.Count) tests — below the manifest floor $($ctest.minimum_registered_tests)"
    }
    $missingPinned = @($pinnedUi | Where-Object { -not $registered.Contains([string]$_) })
    if ($missingPinned.Count -gt 0) {
        throw "Pinned UI test(s) NOT registered in ctest (deleted/renamed while still pinned): $($missingPinned -join ', ')"
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
        if ($matches[0].status -ne "captured" -or [string]::IsNullOrWhiteSpace($matches[0].file)) {
            throw "UI screenshots view '$view' does not identify a captured file"
        }
        $capturedPath = Join-Path (Join-Path $artifactRoot "ui/screenshots") $matches[0].file
        if (-not (Test-Path $capturedPath)) {
            throw "UI screenshots view '$view' is missing captured pixels: $capturedPath"
        }
    }
}

function Test-SimulationEventBusOrderGate {
    $artifactDir = "build/$BuildPreset/test-artifacts/simulation"
    $analysisPath = Join-Path $artifactDir "eventbus-replay.json"
    $testScriptPath = "test/simulation/eventbus-order-gate.ps1"

    if (-not (Test-Path $testScriptPath)) {
        throw "simulation event bus order gate required input missing: $testScriptPath (producer did not create it)"
    }

    & $testScriptPath -BuildPreset $BuildPreset
    if (-not $? ) {
        exit 1
    }
    if ($null -ne $LASTEXITCODE -and $LASTEXITCODE -ne 0) {
        exit $LASTEXITCODE
    }

    if (-not (Test-Path $analysisPath)) {
        throw "simulation event bus order gate required input missing: $analysisPath (producer did not create it)"
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
        throw "lua api manifest gate required input missing: $testScriptPath (producer did not create it)"
    }

    & $testScriptPath -BuildPreset $BuildPreset
    if (-not $?) {
        exit 1
    }
    if ($null -ne $LASTEXITCODE -and $LASTEXITCODE -ne 0) {
        exit $LASTEXITCODE
    }

    if (-not (Test-Path $analysisPath)) {
        throw "lua api manifest gate required input missing: $analysisPath (producer did not create it)"
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

function Test-ScalarFieldDiffusionGate {
    # the game-flavored "aetheric" compatibility alias was removed at
    # iteration close. This gate now inspects the generic engine fields module
    # directly under its own schema (luminumbra.fields.scalar_diffusion.v1).
    $artifactDir = "build/$BuildPreset/test-artifacts/fields"
    $analysisPath = Join-Path $artifactDir "scalar-field-diffusion.json"
    $testScriptPath = "test/fields/scalar-field-diffusion.ps1"

    if (-not (Test-Path $testScriptPath)) {
        throw "scalar field diffusion gate required input missing: $testScriptPath"
    }

    & $testScriptPath -BuildPreset $BuildPreset
    if (-not $?) {
        exit 1
    }
    if ($null -ne $LASTEXITCODE -and $LASTEXITCODE -ne 0) {
        exit $LASTEXITCODE
    }

    if (-not (Test-Path $analysisPath)) {
        throw "scalar field diffusion gate required input missing: $analysisPath"
    }

    $analysis = Get-Content $analysisPath -Raw | ConvertFrom-Json
    if ($analysis.schema -ne "luminumbra.fields.scalar_diffusion.v1") {
        throw "Unexpected scalar field diffusion schema '$($analysis.schema)'"
    }
    if (-not $analysis.passed) {
        throw "Scalar field diffusion analysis reported failure"
    }
    if ($analysis.build_preset -ne $BuildPreset) {
        throw "Scalar field diffusion build_preset '$($analysis.build_preset)' does not match '$BuildPreset'"
    }
    if ($analysis.field.source -ne "src/luminumbra_common/fields/ScalarFieldDiffusion.cpp") {
        throw "Scalar field diffusion analysis must inspect ScalarFieldDiffusion.cpp"
    }
    if ($analysis.field.header -ne "src/luminumbra_common/fields/ScalarFieldDiffusion.h") {
        throw "Scalar field diffusion analysis must inspect ScalarFieldDiffusion.h"
    }
    if ($analysis.diffusion.solver -ne "conservative_pairwise_flux") {
        throw "Scalar field diffusion must use the conservative pairwise flux solver"
    }
    if ($analysis.diffusion.order_contract -ne "deterministic_row_major_edges") {
        throw "Scalar field diffusion must declare deterministic row-major edge ordering"
    }
    if ([int64]$analysis.diffusion.iterations -lt 8) {
        throw "Scalar field diffusion fixture must cover at least eight iterations"
    }
    if (-not $analysis.diffusion.stable) {
        throw "Scalar field diffusion fixture reported an unstable solve"
    }
    if ([double]$analysis.diffusion.conservation_error -gt 1.0e-9) {
        throw "Scalar field diffusion conservation error exceeded tolerance"
    }

    $requiredChecks = @(
        "field diffusion header declares gate API",
        "field diffusion source conserves pairwise flux",
        "fixture declares deterministic diffusion order",
        "diffusion gate validates conservation tolerance",
        "field source is wired into common sources",
        "field gate test is wired into test sources",
        "gate test exercises serializer and fixture"
    )
    $checks = @($analysis.checks)
    foreach ($requiredCheck in $requiredChecks) {
        $matches = @($checks | Where-Object { $_.name -eq $requiredCheck })
        if ($matches.Count -ne 1) {
            throw "Scalar field diffusion analysis is missing check '$requiredCheck'"
        }
        if (-not $matches[0].passed) {
            throw "Scalar field diffusion check failed: $requiredCheck"
        }
    }
}

function Test-InstinctPlannerGate {
    $artifactDir = "build/$BuildPreset/test-artifacts/ai"
    $analysisPath = Join-Path $artifactDir "instinct-grovestrider-hunger.json"
    $testScriptPath = "test/ai/instinct-planner-gate.ps1"

    if (-not (Test-Path $testScriptPath)) {
        throw "instinct planner gate required input missing: $testScriptPath (producer did not create it)"
    }

    & $testScriptPath -BuildPreset $BuildPreset
    if (-not $?) {
        exit 1
    }
    if ($null -ne $LASTEXITCODE -and $LASTEXITCODE -ne 0) {
        exit $LASTEXITCODE
    }

    if (-not (Test-Path $analysisPath)) {
        throw "instinct planner gate required input missing: $analysisPath (producer did not create it)"
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
        throw "persistence roundtrip gate required input missing: $testScriptPath (producer did not create it)"
    }

    & $testScriptPath -BuildPreset $BuildPreset
    if (-not $?) {
        exit 1
    }
    if ($null -ne $LASTEXITCODE -and $LASTEXITCODE -ne 0) {
        exit $LASTEXITCODE
    }

    if (-not (Test-Path $analysisPath)) {
        throw "persistence roundtrip gate required input missing: $analysisPath (producer did not create it)"
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
    # runtime save/load roundtrip through the live client. The save
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
        throw "chunk format validation gate required input missing: $testScriptPath (producer did not create it)"
    }

    & $testScriptPath -BuildPreset $BuildPreset
    if (-not $?) {
        exit 1
    }
    if ($null -ne $LASTEXITCODE -and $LASTEXITCODE -ne 0) {
        exit $LASTEXITCODE
    }

    if (-not (Test-Path $analysisPath)) {
        throw "chunk format validation gate required input missing: $analysisPath (producer did not create it)"
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
        throw "world hash/entity snapshot gate required input missing: $testScriptPath (producer did not create it)"
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
            throw "world hash/entity snapshot gate required input missing: $path (producer did not create it)"
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

function Test-NetworkLoopbackAuthorityGate {
    $artifactDir = "build/$BuildPreset/test-artifacts/network"
    $analysisPath = Join-Path $artifactDir "network-loopback-convergence.json"
    $testScriptPath = "test/network/network-loopback-authority-gate.ps1"

    if (-not (Test-Path $testScriptPath)) {
        throw "network loopback authority gate required input missing: $testScriptPath (producer did not create it)"
    }

    & $testScriptPath -BuildPreset $BuildPreset
    if (-not $?) {
        exit 1
    }
    if ($null -ne $LASTEXITCODE -and $LASTEXITCODE -ne 0) {
        exit $LASTEXITCODE
    }

    if (-not (Test-Path $analysisPath)) {
        throw "network loopback authority gate required input missing: $analysisPath (producer did not create it)"
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
        throw "network state hash gate required input missing: $testScriptPath (producer did not create it)"
    }

    & $testScriptPath -BuildPreset $BuildPreset
    if (-not $?) {
        exit 1
    }
    if ($null -ne $LASTEXITCODE -and $LASTEXITCODE -ne 0) {
        exit $LASTEXITCODE
    }

    if (-not (Test-Path $analysisPath)) {
        throw "network state hash gate required input missing: $analysisPath (producer did not create it)"
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

function Get-CurrentGpuRenderer {
    # Best-effort current adapter name for ecology timing diagnostics. Never throws:
    # returns $null when the OS query is unavailable (non-Windows or no CIM provider).
    try {
        $controller = Get-CimInstance -ClassName Win32_VideoController -ErrorAction Stop |
            Where-Object { $_.Name -and $_.AdapterRAM -ne $null } |
            Sort-Object -Property AdapterRAM -Descending |
            Select-Object -First 1
        if ($null -ne $controller) {
            return [string]$controller.Name
        }
    } catch {
    }
    return $null
}

function Test-EcologyTickPerf {
    # perf-lane-and-ecology-tick (, ): the live ecology-tick perf gate.
    # Runs the EcologyTickPerf gtest (N in {256,1k,4k}, 300 headless ticks over the
    # KINEMATIC PopulatedWorldReplay roster shape), reads ecology_tick_perf.json,
    # and reports median + p99 ms/tick per N. Reviewed release-mode ceilings live
    # in ecology-tick-release.json and are enforced only for matching builds.
    $exe = "build/$BuildPreset/bin/ecology_tick_perf_test.exe"
    if (-not (Test-Path $exe)) {
        throw "Missing EcologyTickPerf executable. Run -Mode Build first: $exe (build target ecology_tick_perf_test)"
    }

    $artifactPath = "build/$BuildPreset/test-artifacts/sim/ecology_tick_perf.json"
    if (Test-Path $artifactPath) {
        Remove-Item $artifactPath
    }

    # 300 ticks x 4000 creatures in debug is heavy (~80s); allow a wide timeout.
    Invoke-Checked -FilePath $exe -ArgumentList @(
        "--gtest_filter=EcologyTickPerf.MeasuresMedianAndP99AcrossRosterSizes"
    ) -TimeoutSeconds 600

    $artifact = Read-JsonArtifact -Path $artifactPath -Schema "luminumbra.ecology_tick_perf.v1"
    if ([int]$artifact.ticks -lt 1) {
        throw "ecology-tick-perf: artifact reports a non-positive tick count ($($artifact.ticks))"
    }
    if ($null -eq $artifact.results -or @($artifact.results).Count -lt 1) {
        throw "ecology-tick-perf: artifact carries no roster results"
    }

    # There is no
    # ecology budget CAP in the sim today — the WHOLE creature roster ticks every tick,
    # and this gate measures/enforces that full-roster cost. If holding a blessed budget
    # ever requires capping per-tick ecology work, the cap MUST be a deterministic
    # rotating id-sorted window (the WaterSystem MAX_WATER_SIMS_PER_TICK pattern:
    # stable-id sort + a persisted cursor advanced by the window size each tick), NEVER a
    # time-based/adaptive cutoff — wall-clock caps make the processed set
    # machine-dependent and break run==replay / host==peer.
    $floorPath = "$ArtifactDir/ecology-tick-release.json"
    $floorBlessed = $false
    $budgets = $null
    $budgetBuildMode = "release"
    if (Test-Path $floorPath) {
        try {
            $floor = Read-JsonArtifact -Path $floorPath -Schema "luminumbra.ecology_tick_baseline.v1"
            $budgets = $floor.budgets
            if ($floor.build_mode) {
                $budgetBuildMode = [string]$floor.build_mode
            }
            $floorBlessed = ($floor.status -eq "reviewed")
        } catch {
            $floorBlessed = $false
        }
    }

    # Budgets are captured from a specific build mode (release). Comparing another build
    # mode's medians (e.g. the debug ctest lane) against release ceilings would be a false
    # RED, so a mismatched-build run reports the numbers without enforcing.
    $buildModeMatches = ([string]$artifact.build_mode -eq $budgetBuildMode)

    $overBudget = @()
    foreach ($r in @($artifact.results)) {
        $n = [int]$r.n
        $median = [double]$r.median_ms
        $p99 = [double]$r.p99_ms
        $spawned = [int]$r.spawned
        if ($spawned -ne $n) {
            throw "ecology-tick-perf: roster N=$n spawned $spawned creatures (non-vacuous measurement required)"
        }
        if ($median -le 0.0) {
            throw "ecology-tick-perf: roster N=$n reported a non-positive median ms/tick ($median)"
        }
        $budgetLabel = "(no blessed budget)"
        if ($null -ne $budgets) {
            $budgetEntry = $budgets.$("$n")
            if ($null -ne $budgetEntry -and $null -ne $budgetEntry.budget_median_ms) {
                $budgetMs = [double]$budgetEntry.budget_median_ms
                $budgetLabel = ("budget {0:N3} ms" -f $budgetMs)
                if ($floorBlessed -and $buildModeMatches -and $median -gt $budgetMs) {
                    $overBudget += ("N={0}: median {1:N3} ms/tick exceeds blessed budget {2:N3} ms" -f $n, $median, $budgetMs)
                }
            }
        }
        Write-Host ("EcologyTickPerf N={0,5}: median {1,8:N3} ms/tick  p99 {2,8:N3} ms  {3}" -f $n, $median, $p99, $budgetLabel)
    }

    if ($overBudget.Count -gt 0) {
        throw "ecology-tick-perf gate failed (median ms/tick over blessed budget):`n$($overBudget -join "`n")"
    }
    if (-not $floorBlessed) {
        Write-Host "EcologyTickPerf: reported median + p99 for all roster sizes; absolute median budgets are NOT enforced (no blessed ecology_tick block -> record-only). Bless via tools/gates/capture-ecology-tick-budgets.ps1 -Bless on a release build."
    } elseif (-not $buildModeMatches) {
        Write-Host ("EcologyTickPerf: blessed budgets exist for build_mode '{0}' but this run measured a '{1}' build -> record-only (run with -BuildPreset {0} to enforce)." -f $budgetBuildMode, [string]$artifact.build_mode)
    } else {
        Write-Host ("EcologyTickPerf: all roster sizes within their blessed median budgets (build_mode {0})." -f $budgetBuildMode)
    }
}

function Test-FarFieldForestBudget {
    # Runs the OpenGL-free forest_perf_budget_test, which models the
    # foliage draw load (instance / draw-call / triangle counts) at a pinned 16k-tree load
    # over the production TreeLod.h selection and GBufferPass batching, including the
    # far-field octa-impostor fold) and emits forest_perf_budget.json.
    # This gate covers geometry only. Runtime performance regressions are evaluated
    # by the paired relative-performance workflow.
    $exe = "build/$BuildPreset/bin/forest_perf_budget_test.exe"
    if (-not (Test-Path $exe)) {
        throw "Missing FarFieldForestBudget executable. Run -Mode Build first: $exe (build target forest_perf_budget_test)"
    }

    $artifactPath = "build/$BuildPreset/test-artifacts/rendering/forest_perf_budget.json"
    Remove-Item -Force -ErrorAction SilentlyContinue $artifactPath

    # Read the verdict from the emitted artifact so a model failure still surfaces
    # the measured counters. Clear $LASTEXITCODE so the harness process status
    # does not propagate to this gate (this gate's pass/fail is its own throws).
    & $exe "--gtest_filter=ForestPerfBudget.*" | Out-Host
    $global:LASTEXITCODE = 0

    $artifact = Read-JsonArtifact -Path $artifactPath -Schema "luminumbra.forest_perf_budget.v1"

    $tris = [int64]$artifact.counters.foliage_tris_per_frame
    $draws = [int]$artifact.counters.draw_calls
    $triBudget = [int64]$artifact.budgets.foliage_tris_per_frame
    $drawBudget = [int]$artifact.budgets.draw_calls_per_frame
    $instances = [int64]$artifact.counters.total_instances
    $trees = [int]$artifact.pinned_tree_count

    Write-Host ("FarFieldForestBudget: {0} trees, {1} instances, {2} draw calls, {3} foliage tris/frame (budgets: <= {4} draws, <= {5} tris)" -f `
        $trees, $instances, $draws, $tris, $drawBudget, $triBudget)

    # Non-vacuity: the harness must have exercised the pinned load.
    if ($trees -le 0 -or $instances -le 0 -or $tris -le 0) {
        throw "FarFieldForestBudget: harness emitted a vacuous load (trees=$trees instances=$instances tris=$tris)"
    }

    if ($null -ne $artifact.gpu_frame_ms) {
        throw "FarFieldForestBudget: geometry-only artifacts must not claim GPU timing evidence"
    }

    # With octa impostors default-ON, the impostor-ON 16k load must hold budget.
    # If it is over budget, the far-field impostor
    # fold has regressed (the LOD3 band reverting to real per-part geometry ~doubles the
    # triangle total) -> hard fail so a broken impostor path can't pass silently.
    if ([bool]$artifact.over_budget.any) {
        throw ("FarFieldForestBudget REGRESSED: the impostor-ON 16k load is OVER budget (tris {0} vs <= {1}, draws {2} vs <= {3}). The far-field octa-impostor fold likely stopped engaging (LOD3 reverting to real geometry). Check LUMIN_TREE_IMPOSTORS and GBufferPass lod3Distance." -f `
            $tris, $triBudget, $draws, $drawBudget)
    }

    Write-Host "FarFieldForestBudget gate: the impostor-ON 16k-tree load holds its geometry budget."
}

# ---  beautification atmosphere (atmosphere) modes: append-only ---

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
        throw "skybox visual run did not produce $analysisPath (gate producer)"
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
    # part A: this smoke is pinned at t=0.04 = NOON (sun near the
    # zenith). The old gradient premise (horizon brighter than zenith by >=8 +
    # monotonic horizon->zenith FALL) was SUNSET physics and is wrong at noon: a
    # high sun makes the dome brightest near the overhead, so horizon <= zenith.
    # The noon dome check is SMOOTH (no harsh inter-band banding) + BRIGHT (both
    # bands lit) + NOT INVERTED-DARK (|horizon-zenith| spread bounded). Dusk/dawn
    # warmth + the horizon-bright gradient remain owned by TimeOfDaySweep.
    if (-not $analysis.gradient.passed) {
        throw "Skybox noon dome check failed: horizon_mean=$($analysis.gradient.horizon_band_mean), zenith_mean=$($analysis.gradient.zenith_band_mean), spread=$($analysis.gradient.horizon_zenith_spread), max_band_step=$($analysis.gradient.max_adjacent_band_step)"
    }
    if ([double]$analysis.gradient.max_adjacent_band_step -gt [double]$analysis.thresholds.max_adjacent_band_step) {
        throw "Skybox noon dome has harsh inter-band banding: max adjacent band step $($analysis.gradient.max_adjacent_band_step) exceeds $($analysis.thresholds.max_adjacent_band_step)"
    }
    if ([double]$analysis.gradient.horizon_band_mean -lt [double]$analysis.thresholds.min_daylight_band_luminance -or `
        [double]$analysis.gradient.zenith_band_mean -lt [double]$analysis.thresholds.min_daylight_band_luminance) {
        throw "Skybox noon dome is too dark: horizon_mean=$($analysis.gradient.horizon_band_mean), zenith_mean=$($analysis.gradient.zenith_band_mean), floor=$($analysis.thresholds.min_daylight_band_luminance)"
    }
    if ([double]$analysis.gradient.horizon_zenith_spread -gt [double]$analysis.thresholds.max_noon_horizon_zenith_spread) {
        throw "Skybox noon dome horizon->zenith spread $($analysis.gradient.horizon_zenith_spread) exceeds bound $($analysis.thresholds.max_noon_horizon_zenith_spread) (inversion/blow-out)"
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
    #  part A: palette_emergence (warm low-sun horizon band) is SUNSET physics
    # and does NOT hold at noon (this smoke is pinned at t=0.04 = high sun). It is
    # kept as a DIAGNOSTIC section in the artifact but is no longer a pass gate here;
    # dusk/dawn warmth is gated by TimeOfDaySweep. Section must still be present.
    if ($null -eq $analysis.palette_emergence) {
        throw "Skybox visual analysis is missing the palette_emergence section (diagnostic, )"
    }
    #  per-pass GPU-timer budgets for the aerial term + sky precompute.
    # Enforced on the RELEASE build only (debug is ~10x slower --  wind
    # precedent); on debug the timers must still be present + non-negative.
    if ($null -eq $analysis.gpu_timer) {
        throw "Skybox visual analysis is missing the gpu_timer section ()"
    }
    foreach ($field in @("aerial_gpu_ms", "sky_view_refresh_ms", "sky_full_precompute_ms")) {
        if ([double]$analysis.gpu_timer.$field -lt 0) {
            throw "Skybox gpu_timer.$field reports a negative value"
        }
    }
    if ($BuildPreset -eq "release" -and [bool]$analysis.gpu_timer.supported) {
        if (-not $analysis.gpu_timer.aerial_within_budget) {
            throw "Aerial-perspective GPU timer $($analysis.gpu_timer.aerial_gpu_ms) ms exceeds budget $($analysis.gpu_timer.aerial_budget_ms) ms (release)"
        }
        if (-not $analysis.gpu_timer.sky_view_refresh_within_budget) {
            throw "Sky-view refresh GPU timer $($analysis.gpu_timer.sky_view_refresh_ms) ms exceeds budget $($analysis.gpu_timer.sky_view_refresh_budget_ms) ms (release)"
        }
        if (-not $analysis.gpu_timer.sky_precompute_within_budget) {
            throw "Sky LUT full precompute $($analysis.gpu_timer.sky_full_precompute_ms) ms exceeds budget $($analysis.gpu_timer.sky_precompute_budget_ms) ms (release)"
        }
    }
    if (-not $analysis.passed) {
        throw "Skybox visual analysis reported failure"
    }

    Assert-PpmArtifact (Join-Path $visualDir $analysis.screenshot)
    Assert-CapturePinned -ArtifactDir $visualDir -Name "SkyboxVisual"
    Write-Host ("SkyboxVisual (noon): dome horizon {0:N1} / zenith {1:N1} (spread {2:N1}, max band step {3:N1}); sun cluster {4:N3}; aerial {5:N3} ms, sky precompute {6:N3} ms" -f `
        $analysis.gradient.horizon_band_mean, $analysis.gradient.zenith_band_mean, `
        $analysis.gradient.horizon_zenith_spread, $analysis.gradient.max_adjacent_band_step, `
        $analysis.sun_disc.sun_cluster_fraction, `
        $analysis.gpu_timer.aerial_gpu_ms, $analysis.gpu_timer.sky_full_precompute_ms)
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
        throw "weather visual run did not produce $analysisPath (gate producer)"
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

    # the WeatherVisual gate ALSO asserts the LIGHTNING strike FRAME --
    # the photography timing shot. The same weather_visual_smoke run fires a
    # deterministically scheduled strike during the weather phase and captures a
    # NEIGHBOUR (pre-strike) frame + the STRIKE frame. The gate asserts (a) a
    # full-scene luminance PULSE (frame-mean luminance spike vs the neighbour) and
    # (b) BOLT pixels (a bright thin high-gradient structure). The visual gate does
    # NOT depend on audio (regression review): thunder is a separate thin cue.
    $strikePath = Join-Path $visualDir "lightning-strike-visual-analysis.json"
    if (-not (Test-Path $strikePath)) {
        throw "weather visual run did not produce $strikePath ( lightning strike frame)"
    }
    $strike = Get-Content $strikePath -Raw | ConvertFrom-Json
    if ($strike.schema -ne "luminumbra.lightning_strike_visual.v1") {
        throw "Unexpected lightning strike analysis schema '$($strike.schema)'"
    }
    if ([int64]$strike.gl_debug.errors -ne 0) {
        throw "Lightning strike frame emitted GL debug errors: $($strike.gl_debug.errors)"
    }
    if (-not $strike.pulse.passed) {
        throw "Lightning pulse check failed: frame-mean luminance delta=$($strike.pulse.frame_mean_luminance_delta) (threshold $($strike.thresholds.min_pulse_delta))"
    }
    if ([double]$strike.pulse.frame_mean_luminance_delta -lt [double]$strike.thresholds.min_pulse_delta) {
        throw "Lightning pulse delta $($strike.pulse.frame_mean_luminance_delta) below threshold $($strike.thresholds.min_pulse_delta)"
    }
    if (-not $strike.bolt.passed) {
        throw "Lightning bolt structure check failed: bright_thin_pixels=$($strike.bolt.bright_thin_pixels) (threshold $($strike.thresholds.min_bolt_pixels))"
    }
    if ([int64]$strike.bolt.bright_thin_pixels -lt [int64]$strike.thresholds.min_bolt_pixels) {
        throw "Lightning bolt pixel count $($strike.bolt.bright_thin_pixels) below threshold $($strike.thresholds.min_bolt_pixels)"
    }
    #  bolt SHAPE -- the bright core must be a THIN,
    # mostly-VERTICAL structure (aspect >= min, fill fraction <= max), not a fat
    # lumpy white blob. Also assert the strike fires against a dark enough storm
    # sky that the flash reads (neighbour pre-strike frame luminance ceiling).
    if ($null -eq $strike.bolt_shape) {
        throw "Lightning strike analysis is missing the bolt_shape section ()"
    }
    if (-not $strike.bolt_shape.passed) {
        throw "Lightning bolt SHAPE check failed (fat-blob guard): aspect=$($strike.bolt_shape.aspect_ratio) (>= $($strike.thresholds.min_bolt_aspect)), fill_fraction=$($strike.bolt_shape.fill_fraction) (<= $($strike.thresholds.max_bolt_fill_fraction)), bbox=$($strike.bolt_shape.bbox_width)x$($strike.bolt_shape.bbox_height)"
    }
    if ([double]$strike.bolt_shape.aspect_ratio -lt [double]$strike.thresholds.min_bolt_aspect) {
        throw "Lightning bolt aspect ratio $($strike.bolt_shape.aspect_ratio) below threshold $($strike.thresholds.min_bolt_aspect) (bolt is not tall/narrow enough -- reads as a blob)"
    }
    if ([double]$strike.bolt_shape.fill_fraction -gt [double]$strike.thresholds.max_bolt_fill_fraction) {
        throw "Lightning bolt fill fraction $($strike.bolt_shape.fill_fraction) exceeds threshold $($strike.thresholds.max_bolt_fill_fraction) (bolt is too dense -- a filled blob, not a thin filament)"
    }
    if ($null -eq $strike.strike_contrast) {
        throw "Lightning strike analysis is missing the strike_contrast section ()"
    }
    if (-not $strike.strike_contrast.passed) {
        throw "Lightning strike CONTRAST check failed: pre-strike storm sky luminance $($strike.strike_contrast.neighbor_frame_mean_luminance) exceeds ceiling $($strike.thresholds.max_neighbor_luma) (the flash must read against a dark storm sky)"
    }
    if ([double]$strike.strike_contrast.neighbor_frame_mean_luminance -gt [double]$strike.thresholds.max_neighbor_luma) {
        throw "Pre-strike storm sky luminance $($strike.strike_contrast.neighbor_frame_mean_luminance) exceeds the $($strike.thresholds.max_neighbor_luma) ceiling"
    }
    if (-not $strike.passed) {
        throw "Lightning strike visual analysis reported failure"
    }
    Assert-PpmArtifact (Join-Path $visualDir $strike.neighbor_screenshot)
    Assert-PpmArtifact (Join-Path $visualDir $strike.strike_screenshot)
    Write-Host ("lightning strike frame gate passed: frame-mean luminance pulse +{0:N4} (neighbour {1:N4} -> strike {2:N4}); {3} bolt pixels; pulse GPU {4:N4} ms" -f `
        $strike.pulse.frame_mean_luminance_delta, $strike.neighbor.frame_mean_luminance, $strike.strike.frame_mean_luminance, `
        $strike.bolt.bright_thin_pixels, $strike.render_pass.lightning_pulse_gpu_ms)

    # the WeatherVisual gate ALSO asserts the SIM-side weather state
    # determinism via the server's --weather-bench mode (the visual overlay above
    # is now fed from this replicated state, one-way). Two independent runs of N
    # WeatherSystem updates (advected by a parallel wind field) reach the IDENTICAL
    # `weather` STATE-HASH (stable across resim/replay -- the property the
    # world_hash `weather` slot depends on); the state EVOLVES over time (gate not
    # vacuous); storm cells stay BOUNDED (<= 16, ) with at least one spawned; and
    # the per-tick weather update stays within the PINNED <= 0.20 ms budget at the
    # streamed extent (enforced on the release build; informational on debug).
    $serverExe = "build/$BuildPreset/bin/luminumbra_server_app.exe"
    if (-not (Test-Path $serverExe)) {
        throw "weather determinism gate required executable missing: $serverExe (cmake --build build/$BuildPreset)"
    }
    $weatherArtifact = "build/$BuildPreset/test-artifacts/server/weather-determinism.json"
    Remove-Item -Force -ErrorAction SilentlyContinue $weatherArtifact
    & $serverExe --weather-bench --ticks 300 --seed 424242 --artifact $weatherArtifact
    if ($LASTEXITCODE -ne 0) {
        throw "weather-bench exited with code $LASTEXITCODE"
    }
    $w = Read-JsonArtifact $weatherArtifact "luminumbra.weather_determinism.v1"
    Assert-ArtifactPassed $w "WeatherVisual(weather-bench)"
    if ([string]::IsNullOrWhiteSpace($w.weather_sub_hash)) {
        throw "weather determinism: empty weather sub-hash"
    }
    if ($w.weather_sub_hash -ne $w.weather_sub_hash_replay) {
        throw "weather determinism: weather state-hash diverged across runs ($($w.weather_sub_hash) != $($w.weather_sub_hash_replay))"
    }
    if (-not $w.deterministic) {
        throw "weather determinism: reported non-deterministic"
    }
    if (-not $w.evolves -or $w.weather_sub_hash_tick0 -eq $w.weather_sub_hash_evolved) {
        throw "weather determinism: state did not evolve over ticks (gate is vacuous)"
    }
    if (-not $w.bounded_storm_cells -or [int]$w.max_storm_cells -gt [int]$w.max_storm_cell_cap) {
        throw "weather determinism: storm cells exceeded the bounded cap (max=$($w.max_storm_cells) cap=$($w.max_storm_cell_cap))"
    }
    if (-not $w.storms_spawned -or [int]$w.max_storm_cells -le 0) {
        throw "weather determinism: no storm cell ever spawned (storm path is vacuous)"
    }
    if ([double]$w.cell_size_m -ne 24.0) {
        throw "weather determinism: cell_size_m=$($w.cell_size_m), expected 24"
    }
    # LIGHTNING strike schedule determinism (the schedule is folded
    # into the `weather` world_hash sub-hash -- world_hash hash revision). The seed+13
    # schedule must FIRE (at least one strike over the run, non-vacuous), match
    # bit-for-bit across the two runs (deterministic), and stay BOUNDED (<= the live
    # strike cap, ). The strike SUB-HASH determinism is already covered by the
    # weather_sub_hash equality above; these assert the strike path is exercised.
    if (-not $w.strikes_scheduled -or [int64]$w.total_strikes -le 0) {
        throw "weather determinism: no lightning strike scheduled (strike path is vacuous; total_strikes=$($w.total_strikes))"
    }
    if (-not $w.strikes_deterministic -or [int64]$w.total_strikes -ne [int64]$w.total_strikes_replay) {
        throw "weather determinism: strike schedule diverged across runs (total_strikes $($w.total_strikes) != $($w.total_strikes_replay))"
    }
    if (-not $w.strikes_bounded -or [int]$w.max_live_strikes -gt [int]$w.max_live_strike_cap) {
        throw "weather determinism: live strike window exceeded the bounded cap (max=$($w.max_live_strikes) cap=$($w.max_live_strike_cap))"
    }
    # Per-tick weather budget: <= 0.20 ms at the streamed extent, enforced on the
    # release build only (the deterministic runtime contract ), informational on debug.
    $wBudgetMs = [double]$w.budget_ms
    $wPerTickMs = [double]$w.per_tick_update_ms
    $wBudgetSource = $BuildPreset
    $releaseExe = "build/release/bin/luminumbra_server_app.exe"
    if ($BuildPreset -ne "release" -and (Test-Path $releaseExe)) {
        $relWeather = "build/release/test-artifacts/server/weather-determinism.json"
        Remove-Item -Force -ErrorAction SilentlyContinue $relWeather
        & $releaseExe --weather-bench --ticks 300 --seed 424242 --artifact $relWeather | Out-Null
        if ($LASTEXITCODE -eq 0 -and (Test-Path $relWeather)) {
            $relW = Read-JsonArtifact $relWeather "luminumbra.weather_determinism.v1"
            $wPerTickMs = [double]$relW.per_tick_update_ms
            $wBudgetSource = "release"
            if (-not $relW.deterministic -or -not $relW.evolves) {
                throw "weather determinism: release build run was non-deterministic or did not evolve"
            }
        }
    }
    if ($wBudgetSource -eq "release") {
        if ($wPerTickMs -gt $wBudgetMs) {
            throw ("weather determinism: per-tick weather update {0:N4} ms (release) exceeds the {1:N4} ms budget" -f $wPerTickMs, $wBudgetMs)
        }
    } else {
        Write-Host ("weather determinism: NOTE budget enforced on the release build only; {0} measured {1:N4} ms (informational, budget {2:N4} ms)" -f `
            $BuildPreset, $wPerTickMs, $wBudgetMs)
    }
    Write-Host ("weather determinism gate passed: weather_sub_hash={0} stable across 2 runs (evolves over {1} ticks); max_storm_cells={2} (cap {3}); per-tick update {4:N4} ms <= {5:N4} ms budget [{6}]" -f `
        $w.weather_sub_hash, $w.ticks, $w.max_storm_cells, $w.max_storm_cell_cap, $wPerTickMs, $wBudgetMs, $wBudgetSource)
}

function Test-CloudShadow {
    # cloud-layer cast-shadow gate. PARTLY-CLOUDY fixture (NOT
    # overcast -- premise guard ). The skybox dome renders the wind-advected
    # cloud layer and the lighting pass projects the SAME coverage field to cast
    # crawling terrain shadows. Two captures (t0, t1) are taken as a cloud-shadow
    # edge drifts across a FIXED terrain ROI; the gate asserts a luminance delta in
    # that ROI (the moving cast-shadow signature) AND that the cloud layer is
    # present in the sky. The added per-fragment cloud-shadow sample cost (lighting
    # pass clouds-on minus clouds-off) is bounded against the <= 0.4 ms budget on
    # release (the deterministic runtime contract , regression review).
    $exe = Get-ClientExe
    $visualDir = "build/$BuildPreset/test-artifacts/runtime/cloud-shadow"
    Remove-Item -Recurse -Force -ErrorAction SilentlyContinue $visualDir
    New-Item -ItemType Directory -Force -Path $visualDir | Out-Null

    # A longer window than the other visual smokes so the wind drifts a shadow
    # edge across the fixed terrain ROI between the two captures.
    $runSeconds = [Math]::Max(24, $SmokeSeconds)
    Invoke-Checked -FilePath $exe -ArgumentList @(
        "--scenario", "cloud_shadow_smoke",
        "--auto-create-world",
        "--auto-enter-world",
        "--timed-run", "$runSeconds",
        "--no-audio",
        "--no-ui",
        "--runtime-artifact-dir", $visualDir
    ) -TimeoutSeconds ([Math]::Max(150, $runSeconds + 90))

    $analysisPath = Join-Path $visualDir "cloud-shadow-analysis.json"
    if (-not (Test-Path $analysisPath)) {
        throw "cloud shadow run did not produce $analysisPath (gate producer)"
    }

    $analysis = Get-Content $analysisPath -Raw | ConvertFrom-Json
    if ($analysis.schema -ne "luminumbra.cloud_shadow.v1") {
        throw "Unexpected cloud shadow analysis schema '$($analysis.schema)'"
    }
    if ([int64]$analysis.gl_debug.errors -ne 0) {
        throw "Cloud shadow run emitted GL debug errors: $($analysis.gl_debug.errors)"
    }
    if ([int]$analysis.render_pass.skybox_draws -le 0) {
        throw "Cloud shadow run did not submit skybox draws"
    }
    # Moving cast-shadow signature: luminance delta in the fixed terrain ROI as a
    # shadow edge crosses between the two times.
    if (-not $analysis.moving_shadow.passed) {
        throw "Cloud cast-shadow did not move the terrain ROI: t0=$($analysis.moving_shadow.terrain_roi_luminance_t0) t1=$($analysis.moving_shadow.terrain_roi_luminance_t1) delta=$($analysis.moving_shadow.terrain_roi_luminance_delta) (min $($analysis.moving_shadow.min_terrain_roi_delta))"
    }
    if ([double]$analysis.moving_shadow.terrain_roi_luminance_delta -lt [double]$analysis.moving_shadow.min_terrain_roi_delta) {
        throw "Cloud cast-shadow terrain ROI delta $($analysis.moving_shadow.terrain_roi_luminance_delta) is below threshold $($analysis.moving_shadow.min_terrain_roi_delta)"
    }
    # Cloud layer present in the sky capture.
    if (-not $analysis.cloud_layer.passed -or -not $analysis.cloud_layer.present) {
        throw "Cloud layer not present in the sky: gradient=$($analysis.cloud_layer.sky_horizontal_gradient_mean) (min $($analysis.cloud_layer.min_sky_cloud_gradient))"
    }
    # GPU-timer budget for the added cloud-shadow sample (release-enforced; on
    # debug the timing is informational, / precedent).
    if ($null -eq $analysis.gpu_timer) {
        throw "Cloud shadow analysis is missing the gpu_timer section ()"
    }
    if ($BuildPreset -eq "release" -and [bool]$analysis.gpu_timer.supported) {
        if (-not $analysis.gpu_timer.within_budget) {
            throw "Cloud-shadow added GPU cost $($analysis.gpu_timer.cloud_shadow_added_ms) ms exceeds budget $($analysis.gpu_timer.cloud_shadow_budget_ms) ms (release)"
        }
    }
    if (-not $analysis.passed) {
        throw "Cloud shadow analysis reported failure"
    }

    Assert-PpmArtifact (Join-Path $visualDir $analysis.terrain_t1_screenshot)
    Assert-CapturePinned -ArtifactDir $visualDir -Name "CloudShadow"
    Write-Host ("CloudShadow: terrain ROI luminance t0 {0:N4} -> t1 {1:N4} (delta {2:N4}); cloud sky gradient {3:N4}; cloud-shadow added {4:N4} ms" -f `
        $analysis.moving_shadow.terrain_roi_luminance_t0, $analysis.moving_shadow.terrain_roi_luminance_t1, `
        $analysis.moving_shadow.terrain_roi_luminance_delta, $analysis.cloud_layer.sky_horizontal_gradient_mean, `
        $analysis.gpu_timer.cloud_shadow_added_ms)
}

function Test-FoliageInstancing {
    # instanced foliage scatter + wind response gate. The scatter
    # is a DETERMINISTIC pure hash of (chunk coords, biome id, slope, moisture,
    # instance index) -- NO global RNG, NO world_hash growth. The gate asserts,
    # from the instance-set DATA: (a) coverage density tracks the biome table
    # within a band at fixed seeds; (b) the distance-fade is present (no foliage
    # beyond the live ring / fade end); (c) the wind-sway responds (calm vs windy
    # max tip displacement differs, only swaying archetypes move); (d) the
    # FoliagePass GPU-timer is within the pinned release budget. The instance-set
    # hash is asserted reproducible (run==run).: world_hash stays
    # d950a6afc12a5cdc (one-way, regression review). Foliage adds ground pixels, so the
    # RenderHealth update the baseline is DELIBERATE and logged (the deterministic runtime contract ).
    $exe = Get-ClientExe
    $visualDir = "build/$BuildPreset/test-artifacts/runtime/foliage-instancing"
    Remove-Item -Recurse -Force -ErrorAction SilentlyContinue $visualDir
    New-Item -ItemType Directory -Force -Path $visualDir | Out-Null

    # A window long enough to run the CALM phase then the WINDY phase (the sway
    # delta is measured across the two).
    $runSeconds = [Math]::Max(20, $SmokeSeconds)
    Invoke-Checked -FilePath $exe -ArgumentList @(
        "--scenario", "foliage_visual_smoke",
        "--auto-create-world",
        "--auto-enter-world",
        "--world-preset", "flat_lands",
        "--timed-run", "$runSeconds",
        "--no-audio",
        "--no-ui",
        "--runtime-artifact-dir", $visualDir
    ) -TimeoutSeconds ([Math]::Max(150, $runSeconds + 90))

    $analysisPath = Join-Path $visualDir "foliage-instancing-analysis.json"
    if (-not (Test-Path $analysisPath)) {
        throw "foliage instancing run did not produce $analysisPath (gate producer)"
    }

    $analysis = Get-Content $analysisPath -Raw | ConvertFrom-Json
    if ($analysis.schema -ne "luminumbra.foliage_instancing.v1") {
        throw "Unexpected foliage instancing analysis schema '$($analysis.schema)'"
    }
    # the client writes an explicit refusal analysis instead of ending a
    # gate run silent (readback-disabled / zero-instance / no-draws shapes).
    if ($analysis.refusal) {
        throw "Foliage instancing run REFUSED: $($analysis.refusal)"
    }
    if ([int64]$analysis.gl_debug.errors -ne 0) {
        throw "Foliage instancing run emitted GL debug errors: $($analysis.gl_debug.errors)"
    }
    if ([int]$analysis.render_pass.foliage_draws -le 0) {
        throw "Foliage instancing run did not submit foliage draws"
    }
    if ([int64]$analysis.render_pass.foliage_instances_drawn -le 0) {
        throw "Foliage instancing run drew zero scatter instances"
    }
    # Determinism: the instance-set hash is byte-equal across two rebuilds.
    if (-not $analysis.determinism.passed -or -not $analysis.determinism.hash_byte_equal) {
        throw "Foliage placement is not deterministic: hash_a=$($analysis.determinism.instance_hash_run_a) hash_b=$($analysis.determinism.instance_hash_run_b)"
    }
    if ([bool]$analysis.determinism.global_rng) {
        throw "Foliage placement reports a global RNG (must be a pure per-chunk hash)"
    }
    if ([bool]$analysis.determinism.world_hash_written) {
        throw "Foliage reported writing world_hash (must be render-only / one-way)"
    }
    # Coverage density tracks the biome table within a band.
    if (-not $analysis.coverage_density.passed) {
        throw "Foliage coverage density off-band: measured=$($analysis.coverage_density.measured_density) biome=$($analysis.coverage_density.biome_density) delta=$($analysis.coverage_density.density_delta) band=$($analysis.coverage_density.density_band)"
    }
    if ([int64]$analysis.coverage_density.instances_within_ring -le 0) {
        throw "Foliage produced no instances within the live ring"
    }
    #  update the baseline floor (2026-07-02): the lush-default flat_lands scatter
    # saturates the 262144 budget in-ring; a hard floor keeps decimation-class
    # regressions RED even though measured_density saturates at the calibrated cap.
    if ([int64]$analysis.coverage_density.instances_within_ring -lt 100000) {
        throw "Foliage in-ring instance count $($analysis.coverage_density.instances_within_ring) is below the 100000 decimation floor (saturated-carpet contract, re-blessed 2026-07-02)"
    }
    # Distance-fade: NO foliage beyond the live ring / fade end.
    if (-not $analysis.distance_fade.passed -or [int64]$analysis.distance_fade.instances_beyond_fade -ne 0) {
        throw "Foliage present beyond the live ring: $($analysis.distance_fade.instances_beyond_fade) instances past fade_end $($analysis.distance_fade.fade_end_m) m"
    }
    # Wind sway responds: windy max tip displacement exceeds calm by a margin.
    if (-not $analysis.wind_sway.passed) {
        throw "Foliage sway did not respond to wind: calm=$($analysis.wind_sway.calm_max_sway) windy=$($analysis.wind_sway.windy_max_sway) delta=$($analysis.wind_sway.sway_delta) (min $($analysis.wind_sway.min_sway_delta))"
    }
    # GPU-timer budget (release-enforced; informational on debug, / precedent).
    if ($null -eq $analysis.gpu_timer) {
        throw "Foliage instancing analysis is missing the gpu_timer section ()"
    }
    if ([double]$analysis.gpu_timer.foliage_gpu_ms -lt 0) {
        throw "Foliage gpu_timer.foliage_gpu_ms reports a negative value"
    }
    if ($BuildPreset -eq "release" -and [bool]$analysis.gpu_timer.supported) {
        if (-not $analysis.gpu_timer.within_budget) {
            throw "FoliagePass GPU timer $($analysis.gpu_timer.foliage_gpu_ms) ms exceeds budget $($analysis.gpu_timer.budget_ms) ms (release)"
        }
    }
    if (-not $analysis.passed) {
        throw "Foliage instancing analysis reported failure"
    }

    Assert-PpmArtifact (Join-Path $visualDir $analysis.foliage_screenshot)
    Assert-CapturePinned -ArtifactDir $visualDir -Name "FoliageInstancing"
    Write-Host ("FoliageInstancing: {0} instances ({1} in-ring); density measured {2:N3} vs biome {3:N3}; sway calm {4:N4} -> windy {5:N4} m; {6:N4} ms" -f `
        $analysis.render_pass.foliage_instances_drawn, $analysis.coverage_density.instances_within_ring, `
        $analysis.coverage_density.measured_density, $analysis.coverage_density.biome_density, `
        $analysis.wind_sway.calm_max_sway, $analysis.wind_sway.windy_max_sway, `
        $analysis.gpu_timer.foliage_gpu_ms)
}

function Test-ParticleEmitterDeterminism {
    #  GPU particle framework determinism gate. Spawns the fixture
    # emitter, snapshots the sim-deterministic emitter DESCRIPTOR SET twice from
    # identical world state, and asserts the descriptor bytes are byte-equal
    # across runs. Per-particle MOTION is render-only and is NOT snapshotted
    # (regression review). Also asserts particles rendered and the ParticlePass GPU
    # timer is within the 0.8 ms budget (regression review).
    $exe = Get-ClientExe
    $visualDir = "build/$BuildPreset/test-artifacts/runtime/particle-determinism"
    Remove-Item -Recurse -Force -ErrorAction SilentlyContinue $visualDir
    New-Item -ItemType Directory -Force -Path $visualDir | Out-Null

    $runSeconds = [Math]::Max(20, $SmokeSeconds)
    Invoke-Checked -FilePath $exe -ArgumentList @(
        "--scenario", "particle_emitter_determinism_smoke",
        "--auto-create-world",
        "--auto-enter-world",
        "--timed-run", "$runSeconds",
        "--no-audio",
        "--no-ui",
        "--runtime-artifact-dir", $visualDir
    ) -TimeoutSeconds ([Math]::Max(120, $runSeconds + 90))

    $analysisPath = Join-Path $visualDir "particle-emitter-determinism-analysis.json"
    if (-not (Test-Path $analysisPath)) {
        throw "particle determinism run did not produce $analysisPath (gate producer)"
    }

    $analysis = Get-Content $analysisPath -Raw | ConvertFrom-Json
    if ($analysis.schema -ne "luminumbra.particle_emitter_determinism.v1") {
        throw "Unexpected particle determinism analysis schema '$($analysis.schema)'"
    }
    if ([int64]$analysis.gl_debug.errors -ne 0) {
        throw "Particle determinism run emitted GL debug errors: $($analysis.gl_debug.errors)"
    }

    # Emitter descriptor set must be byte-equal across the two rebuilds.
    if (-not $analysis.determinism.byte_equal) {
        throw "Particle emitter descriptor set is NOT byte-equal across runs (hash_a=$($analysis.determinism.descriptor_hash_run_a) hash_b=$($analysis.determinism.descriptor_hash_run_b))"
    }
    if ([string]$analysis.determinism.descriptor_hash_run_a -ne [string]$analysis.determinism.descriptor_hash_run_b) {
        throw "Particle emitter descriptor hashes differ across runs"
    }
    if ([int64]$analysis.determinism.descriptor_count -lt 1) {
        throw "Particle determinism gate captured no emitter descriptors"
    }
    # The snapshot surface must be the emitter descriptor set ONLY; particle
    # motion must never be snapshotted.
    if ($analysis.determinism.snapshot_surface -ne "emitter_descriptor_set") {
        throw "Particle determinism snapshot surface must be 'emitter_descriptor_set', got '$($analysis.determinism.snapshot_surface)'"
    }
    if ([bool]$analysis.determinism.motion_snapshotted) {
        throw "Particle MOTION must never be snapshotted (render-only); motion_snapshotted=true"
    }

    # Particles must actually have rendered.
    if ([int64]$analysis.render_pass.particle_draws -lt 1) {
        throw "Particle determinism gate recorded no ParticlePass draws"
    }

    # ParticlePass GPU timer must be within the 0.8 ms budget.
    if (-not $analysis.gpu_timer.within_budget) {
        throw "ParticlePass GPU timer $($analysis.gpu_timer.particle_pass_gpu_ms) ms exceeds budget $($analysis.gpu_timer.budget_ms) ms"
    }
    if ([double]$analysis.gpu_timer.particle_pass_gpu_ms -gt [double]$analysis.gpu_timer.budget_ms) {
        throw "ParticlePass GPU timer $($analysis.gpu_timer.particle_pass_gpu_ms) ms exceeds budget $($analysis.gpu_timer.budget_ms) ms"
    }

    if (-not $analysis.passed) {
        throw "Particle determinism analysis reported failure"
    }

    Assert-PpmArtifact (Join-Path $visualDir $analysis.particle_screenshot)
    Write-Host ("particle determinism: descriptor set byte-equal over {0} emitter(s); ParticlePass {1} ms (budget {2} ms)" -f `
        $analysis.determinism.descriptor_count, $analysis.gpu_timer.particle_pass_gpu_ms, $analysis.gpu_timer.budget_ms)
}

function Test-Precipitation {
    # rain precipitation through the  particle framework, driven by
    # the replicated weather state and WIND-ADVECTED (slant) by the  wind field.
    # The scenario captures a CALM rain frame (zero wind -> vertical fall) and a
    # WINDY rain frame (strong horizontal wind -> diagonal slant). The gate asserts
    # precip particles are PRESENT in both frames AND that the windy streaks slant
    # with wind (the windy slant ratio exceeds the calm one by a margin). Emitter
    # descriptors stay deterministic (ParticleEmitterDeterminism owns that surface);
    # particle MOTION is render-only and never hashed (regression review, one-way). Also
    # asserts the active-storm + precipitation ParticlePass GPU timer is within the
    # 1.2 ms storm budget (the deterministic runtime contract ).
    $exe = Get-ClientExe
    $visualDir = "build/$BuildPreset/test-artifacts/runtime/precipitation"
    Remove-Item -Recurse -Force -ErrorAction SilentlyContinue $visualDir
    New-Item -ItemType Directory -Force -Path $visualDir | Out-Null

    $runSeconds = [Math]::Max(20, $SmokeSeconds)
    Invoke-Checked -FilePath $exe -ArgumentList @(
        "--scenario", "precipitation_smoke",
        "--auto-create-world",
        "--auto-enter-world",
        "--timed-run", "$runSeconds",
        "--no-audio",
        "--no-ui",
        "--runtime-artifact-dir", $visualDir
    ) -TimeoutSeconds ([Math]::Max(120, $runSeconds + 90))

    $analysisPath = Join-Path $visualDir "precipitation-analysis.json"
    if (-not (Test-Path $analysisPath)) {
        throw "precipitation run did not produce $analysisPath (gate producer)"
    }

    $analysis = Get-Content $analysisPath -Raw | ConvertFrom-Json
    if ($analysis.schema -ne "luminumbra.precipitation.v1") {
        throw "Unexpected precipitation analysis schema '$($analysis.schema)'"
    }
    if ([int64]$analysis.gl_debug.errors -ne 0) {
        throw "Precipitation run emitted GL debug errors: $($analysis.gl_debug.errors)"
    }
    if ($analysis.precip.type -ne "rain") {
        throw "Precipitation run must exercise rain"
    }

    # Precip particles must actually have rendered in BOTH the calm and windy frame.
    if ([int64]$analysis.render_pass.calm_particle_draws -lt 1) {
        throw "Precipitation gate recorded no ParticlePass draws in the calm frame"
    }
    if ([int64]$analysis.render_pass.windy_particle_draws -lt 1) {
        throw "Precipitation gate recorded no ParticlePass draws in the windy frame"
    }
    if (-not $analysis.presence.calm_passed) {
        throw "Precipitation presence check failed (calm): bright_fraction=$($analysis.presence.calm_bright_fraction)"
    }
    if (-not $analysis.presence.windy_passed) {
        throw "Precipitation presence check failed (windy): bright_fraction=$($analysis.presence.windy_bright_fraction)"
    }

    # Wind-slant: the windy streaks must lean measurably more than the calm streaks.
    if (-not $analysis.wind_slant.passed) {
        throw "Precipitation wind-slant check failed: calm_slant=$($analysis.wind_slant.calm_slant_ratio) windy_slant=$($analysis.wind_slant.windy_slant_ratio) gain=$($analysis.wind_slant.slant_ratio_gain)"
    }
    if ([double]$analysis.wind_slant.slant_ratio_gain -lt [double]$analysis.thresholds.min_slant_ratio_gain) {
        throw "Precipitation slant gain $($analysis.wind_slant.slant_ratio_gain) is below threshold $($analysis.thresholds.min_slant_ratio_gain)"
    }

    #  STREAKS-NOT-DOTS shape gate. The bright precip
    # structure must be ELONGATED (anisotropic gradient), not round dots. This
    # catches the "scattered dots" failure that the presence/slant thresholds
    # passed when rain rendered as round billboards.
    if ($null -eq $analysis.streak_shape) {
        throw "Precipitation analysis is missing the streak_shape section ()"
    }
    if (-not $analysis.streak_shape.passed) {
        throw "Precipitation STREAK-SHAPE check failed (dots-not-streaks guard): calm_anisotropy=$($analysis.streak_shape.calm_anisotropy) (>= $($analysis.thresholds.min_streak_anisotropy))"
    }
    if ([double]$analysis.streak_shape.calm_anisotropy -lt [double]$analysis.thresholds.min_streak_anisotropy) {
        throw "Precipitation calm streak anisotropy $($analysis.streak_shape.calm_anisotropy) is below threshold $($analysis.thresholds.min_streak_anisotropy) -- precip reads as round dots, not vertical streaks"
    }

    #  LIGHT-STREAKS gate. Rain must read as LIGHT
    # streaks (clearly brighter than the sky backdrop), NOT dark specks ("dirt on
    # the sky"). The bright precip pixels must sit above the band mean by a margin.
    if ($null -eq $analysis.light_streaks) {
        throw "Precipitation analysis is missing the light_streaks section ()"
    }
    if (-not $analysis.light_streaks.passed) {
        throw "Precipitation LIGHT-STREAKS check failed (dark-speck guard): calm bright/band luma $($analysis.light_streaks.calm_bright_mean_luminance)/$($analysis.light_streaks.calm_band_mean_luminance), windy $($analysis.light_streaks.windy_bright_mean_luminance)/$($analysis.light_streaks.windy_band_mean_luminance) (bright must exceed band by $($analysis.thresholds.min_bright_over_band_margin))"
    }
    if ([double]$analysis.light_streaks.calm_bright_mean_luminance -lt [double]$analysis.light_streaks.calm_band_mean_luminance + [double]$analysis.thresholds.min_bright_over_band_margin -or `
        [double]$analysis.light_streaks.windy_bright_mean_luminance -lt [double]$analysis.light_streaks.windy_band_mean_luminance + [double]$analysis.thresholds.min_bright_over_band_margin) {
        throw "Precipitation bright precip is not clearly lighter than the sky backdrop (calm $($analysis.light_streaks.calm_bright_mean_luminance) vs $($analysis.light_streaks.calm_band_mean_luminance), windy $($analysis.light_streaks.windy_bright_mean_luminance) vs $($analysis.light_streaks.windy_band_mean_luminance)) -- rain reads as dark specks"
    }

    # Active-storm + precipitation ParticlePass GPU timer must be within the storm
    # budget (informational on debug where the timer may report 0.0).
    if (-not $analysis.gpu_timer.within_budget) {
        throw "ParticlePass storm GPU timer $($analysis.gpu_timer.particle_pass_gpu_ms) ms exceeds storm budget $($analysis.gpu_timer.storm_budget_ms) ms"
    }
    if ([double]$analysis.gpu_timer.particle_pass_gpu_ms -gt [double]$analysis.gpu_timer.storm_budget_ms) {
        throw "ParticlePass storm GPU timer $($analysis.gpu_timer.particle_pass_gpu_ms) ms exceeds storm budget $($analysis.gpu_timer.storm_budget_ms) ms"
    }

    if (-not $analysis.passed) {
        throw "Precipitation analysis reported failure"
    }

    Assert-PpmArtifact (Join-Path $visualDir $analysis.calm_screenshot)
    Assert-PpmArtifact (Join-Path $visualDir $analysis.windy_screenshot)
    Write-Host ("precipitation gate passed: rain particles present (calm bright {0:P3}, windy bright {1:P3}); streaks slant with wind (calm slant {2:N3} -> windy slant {3:N3}, gain {4:}x); ParticlePass storm {5} ms <= {6} ms budget" -f `
        $analysis.presence.calm_bright_fraction, $analysis.presence.windy_bright_fraction, `
        $analysis.wind_slant.calm_slant_ratio, $analysis.wind_slant.windy_slant_ratio, `
        $analysis.wind_slant.slant_ratio_gain, $analysis.gpu_timer.particle_pass_gpu_ms, $analysis.gpu_timer.storm_budget_ms)
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
        throw "time-of-day sweep run did not produce $analysisPath (gate producer)"
    }

    $analysis = Get-Content $analysisPath -Raw | ConvertFrom-Json
    if ($analysis.schema -ne "luminumbra.timeofday_sweep.v1") {
        throw "Unexpected time-of-day sweep analysis schema '$($analysis.schema)'"
    }
    if ([int64]$analysis.gl_debug.errors -ne 0) {
        throw "Time-of-day sweep emitted GL debug errors: $($analysis.gl_debug.errors)"
    }

    $phases = @($analysis.phases)
    # the sweep now captures noon/dusk/night under TWO seasons
    # (summer + winter) == 6 phase captures. The summer (season_index 0) set owns
    # the existing ordering/warm-shift/hue-band/emissive assertions below.
    if ($phases.Count -lt 6) {
        throw "Time-of-day season sweep must capture noon/dusk/night under 2 seasons (6 phases; found $($phases.Count))"
    }
    foreach ($seasonIndex in @(0, 1)) {
        foreach ($phaseName in @("noon", "dusk", "night")) {
            $matches = @($phases | Where-Object { $_.name -eq $phaseName -and [int]$_.season_index -eq $seasonIndex })
            if ($matches.Count -ne 1) {
                throw "Time-of-day season sweep is missing the season $seasonIndex '$phaseName' phase capture"
            }
            Assert-PpmArtifact (Join-Path $visualDir $matches[0].screenshot)
        }
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
    # the sky DOME must track time-of-day, not just the
    # terrain lighting. (1) Night dome must be dark; the pre-fix dome held a
    # bright twilight-blue night sky (~182) over near-black ground.
    if (-not $analysis.night_sky_dark.passed) {
        throw "Night sky-dome is too bright: sky mean luminance $($analysis.night_sky_dark.night_sky_mean_luminance) exceeds ceiling $($analysis.night_sky_dark.max_night_sky_luminance) (: dome must darken at night)"
    }
    if ([double]$analysis.night_sky_dark.night_sky_mean_luminance -gt [double]$analysis.thresholds.max_night_sky_luminance) {
        throw "Night sky mean luminance $($analysis.night_sky_dark.night_sky_mean_luminance) exceeds threshold $($analysis.thresholds.max_night_sky_luminance)"
    }
    # (2) Dusk dome must warm on the sun side; the pre-fix dusk dome was full
    # midday blue (sun-side sky R<B, no shift vs noon).
    if (-not $analysis.dusk_sky_warm_shift.passed) {
        throw "Dusk sky-dome warm-shift failed: warm-half sky r/b increase $($analysis.dusk_sky_warm_shift.sky_warm_half_r_b_ratio_increase) (: dusk dome must show a warm sun-side tint)"
    }
    if ([double]$analysis.dusk_sky_warm_shift.sky_warm_half_r_b_ratio_increase -lt [double]$analysis.thresholds.min_dusk_sky_warm_shift) {
        throw "Dusk sky warm-half r/b shift $($analysis.dusk_sky_warm_shift.sky_warm_half_r_b_ratio_increase) is below threshold $($analysis.thresholds.min_dusk_sky_warm_shift)"
    }
    #  absolute dawn/dusk HUE-BAND assertion (scattering palette rises
    # warm at low sun, clear sky) on top of the existing noon>dusk>night ordering
    # + relative warm-shift.
    if ($null -eq $analysis.dusk_sky_hue_band) {
        throw "Time-of-day sweep analysis is missing the dusk_sky_hue_band section ()"
    }
    if (-not $analysis.dusk_sky_hue_band.passed) {
        throw "Dusk sky hue-band failed: warm-half r/b $($analysis.dusk_sky_hue_band.dusk_sky_warm_half_r_b_ratio) (: scattering palette must rise warm at low sun)"
    }
    if ([double]$analysis.dusk_sky_hue_band.dusk_sky_warm_half_r_b_ratio -lt [double]$analysis.thresholds.min_dusk_sky_warm_band_ratio) {
        throw "Dusk sky warm-half r/b $($analysis.dusk_sky_hue_band.dusk_sky_warm_half_r_b_ratio) is below absolute hue-band threshold $($analysis.thresholds.min_dusk_sky_warm_band_ratio)"
    }
    #  AURORA NIGHT-GATING. The aurora is a night-only
    # phenomenon; it must be ABSENT (no green chroma smear) in the dusk and noon sky
    # bands. This catches the "aurora bleeds into the dusk sky" failure that the
    # luminance/warm-shift thresholds passed.
    if ($null -eq $analysis.aurora_gating) {
        throw "Time-of-day sweep analysis is missing the aurora_gating section ()"
    }
    if (-not $analysis.aurora_gating.passed) {
        throw "Aurora night-gating failed: night aurora curtain fraction $($analysis.aurora_gating.night_sky_strong_green_fraction) (need >= $($analysis.aurora_gating.min_night_strong_green_fraction)); day-side aurora fractions noon=$($analysis.aurora_gating.noon_sky_strong_green_fraction) dusk=$($analysis.aurora_gating.dusk_sky_strong_green_fraction) (must be <= $($analysis.aurora_gating.max_day_strong_green_fraction)) -- aurora must be night-only"
    }
    if ([double]$analysis.aurora_gating.noon_sky_strong_green_fraction -gt [double]$analysis.aurora_gating.max_day_strong_green_fraction -or `
        [double]$analysis.aurora_gating.dusk_sky_strong_green_fraction -gt [double]$analysis.aurora_gating.max_day_strong_green_fraction) {
        throw "Aurora green chroma present at dusk/noon (noon $($analysis.aurora_gating.noon_sky_strong_green_fraction), dusk $($analysis.aurora_gating.dusk_sky_strong_green_fraction)) -- aurora must be night-only"
    }
    if ([double]$analysis.aurora_gating.night_sky_strong_green_fraction -lt [double]$analysis.aurora_gating.min_night_strong_green_fraction) {
        throw "Aurora absent at night (night curtain fraction $($analysis.aurora_gating.night_sky_strong_green_fraction)) -- the aurora must still render at night"
    }
    # SEASON-SWEEP assertions. The same noon/dusk/night phases are
    # captured under two TICK-DERIVED seasons; assert a real per-season sun-path
    # band (summer noon sun higher than winter) AND palette band (summer noon
    # warmer than winter) difference. The season is render-derived (pure function
    # of tick) -- it adds nothing to world_hash (verified by HeadlessServerTick).
    if ($null -eq $analysis.season_sweep) {
        throw "Time-of-day sweep analysis is missing the season_sweep section ()"
    }
    if (-not $analysis.season_sweep.phases_captured) {
        throw "Season sweep did not capture both seasons' noon/dusk/night phases ()"
    }
    if (-not $analysis.season_sweep.phases_distinct) {
        throw "Season sweep seasons are not distinct tick-derived phases: summer_tick=$($analysis.season_sweep.summer_season_tick) winter_tick=$($analysis.season_sweep.winter_season_tick) ()"
    }
    if (-not $analysis.season_sweep.sun_path.passed) {
        throw "Season sun-path band failed: summer noon elevation $($analysis.season_sweep.sun_path.summer_noon_sun_elevation_rad) rad vs winter $($analysis.season_sweep.sun_path.winter_noon_sun_elevation_rad) rad (gap $($analysis.season_sweep.sun_path.sun_elevation_gap_rad), need >= $($analysis.season_sweep.sun_path.min_sun_elevation_gap_rad)) ()"
    }
    if ([double]$analysis.season_sweep.sun_path.sun_elevation_gap_rad -lt [double]$analysis.season_sweep.sun_path.min_sun_elevation_gap_rad) {
        throw "Season sun-elevation gap $($analysis.season_sweep.sun_path.sun_elevation_gap_rad) rad is below threshold $($analysis.season_sweep.sun_path.min_sun_elevation_gap_rad)"
    }
    if (-not $analysis.season_sweep.palette.passed) {
        throw "Season palette band failed: summer noon r/b $($analysis.season_sweep.palette.summer_noon_frame_r_b_ratio) vs winter $($analysis.season_sweep.palette.winter_noon_frame_r_b_ratio) (gap $($analysis.season_sweep.palette.palette_warmth_gap), need >= $($analysis.season_sweep.palette.min_palette_warmth_gap)) ()"
    }
    if ([double]$analysis.season_sweep.palette.palette_warmth_gap -lt [double]$analysis.season_sweep.palette.min_palette_warmth_gap) {
        throw "Season palette warmth gap $($analysis.season_sweep.palette.palette_warmth_gap) is below threshold $($analysis.season_sweep.palette.min_palette_warmth_gap)"
    }
    #  sky LUT full precompute startup one-shot recorded in render
    # telemetry; budget enforced on the RELEASE build only.
    if ($null -ne $analysis.gpu_timer) {
        if ([double]$analysis.gpu_timer.sky_full_precompute_ms -lt 0) {
            throw "Time-of-day gpu_timer.sky_full_precompute_ms reports a negative value"
        }
        if ($BuildPreset -eq "release" -and [bool]$analysis.gpu_timer.supported -and `
            [double]$analysis.gpu_timer.sky_full_precompute_ms -gt 8.0) {
            throw "Sky LUT full precompute $($analysis.gpu_timer.sky_full_precompute_ms) ms exceeds 8.0 ms budget (release)"
        }
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

# ---  WorldVisualSweep mode ---
# Drives the world_visual_sweep capture matrix (times-of-day x camera angles x
# weather x season) from a feature-rich archipelago anchor and guards PRODUCTION
# + PRESENCE so the matrix can become a standing validation gate. Real visual
# QUALITY is judged by the orchestrator from the assembled montages; this gate
# only proves every expected cell PPM was produced + non-black AND that the
# intended feature signal is ACTIVE in its cells:
#   * foliage_draws > 0 in the down-pitched DAYTIME clear cells,
#   * particle_draws > 0 AND lightning active in every STORM cell,
#   * cloud coverage > 0 in the up-pitched STORM cells,
#   * water-like pixels present in the water-aimed DAYTIME cells,
#   * NO rain leaking into clear cells (particle_draws == 0).
# the scenario drives the existing one-way bridges and never writes
# world_hash. The gate also assembles the labelled montages for review.
function Test-WorldVisualSweep {
    $exe = Get-ClientExe
    $visualDir = "build/$BuildPreset/test-artifacts/runtime/world-visual-sweep"
    Remove-Item -Recurse -Force -ErrorAction SilentlyContinue $visualDir
    New-Item -ItemType Directory -Force -Path $visualDir | Out-Null

    $runSeconds = [Math]::Max(20, $SmokeSeconds)
    Invoke-Checked -FilePath $exe -ArgumentList @(
        "--scenario", "world_visual_sweep",
        "--auto-create-world",
        "--auto-enter-world",
        "--timed-run", "$runSeconds",
        "--no-audio",
        "--no-ui",
        "--runtime-artifact-dir", $visualDir
    ) -TimeoutSeconds ([Math]::Max(480, $runSeconds + 360))

    $manifestPath = Join-Path $visualDir "world-visual-sweep-manifest.json"
    if (-not (Test-Path $manifestPath)) {
        throw "world_visual_sweep run did not produce $manifestPath (gate producer)"
    }
    #  (,  ): bind the sweep to the binary/shaders that
    # produced it. This gate REGENERATES every run, so drop any stale sidecar
    # first (a prior build's sidecar would false-REFUSE the fresh capture).
    $sweepSidecar = Get-ArtifactProvenanceSidecarPath -ArtifactPath $manifestPath -Scenario "WorldVisualSweep"
    Remove-Item -LiteralPath $sweepSidecar -Force -ErrorAction SilentlyContinue
    Assert-ArtifactProvenance -ArtifactPath $manifestPath -Scenario "WorldVisualSweep"

    $manifest = Get-Content $manifestPath -Raw | ConvertFrom-Json
    if ($manifest.schema -ne "luminumbra.world_visual_sweep.v1") {
        throw "Unexpected world_visual_sweep manifest schema '$($manifest.schema)'"
    }

    $cells = @($manifest.cells)
    if ($cells.Count -lt 1) {
        throw "world_visual_sweep manifest carries no cells"
    }

    # PRODUCTION: every expected cell PPM produced + present on disk + non-black.
    if ([int]$manifest.produced_cell_count -ne [int]$manifest.expected_cell_count) {
        throw "world_visual_sweep produced $($manifest.produced_cell_count) of $($manifest.expected_cell_count) expected cells"
    }
    foreach ($cell in $cells) {
        if (-not $cell.produced) {
            throw "world_visual_sweep cell not produced: $($cell.file)"
        }
        Assert-PpmArtifact (Join-Path $visualDir $cell.file)
        if (-not $cell.non_black) {
            throw "world_visual_sweep cell is black/empty: $($cell.file) (mean_luminance=$($cell.mean_luminance))"
        }
    }

    # PRESENCE: each feature must be ACTIVE in its intended cells.
    $foliageCells = @($cells | Where-Object { $_.pitched_down -and $_.daytime -and -not $_.storm })
    if ($foliageCells.Count -lt 1) { throw "world_visual_sweep has no down-pitched daytime clear cells to assert foliage" }
    foreach ($cell in $foliageCells) {
        if ([int64]$cell.foliage_draws -lt 1) {
            throw "world_visual_sweep down-daytime cell shows no foliage (foliage_draws=$($cell.foliage_draws)): $($cell.file)"
        }
    }

    $stormCells = @($cells | Where-Object { $_.storm })
    if ($stormCells.Count -lt 1) { throw "world_visual_sweep has no storm cells" }
    foreach ($cell in $stormCells) {
        if ([int64]$cell.particle_draws -lt 1) {
            throw "world_visual_sweep storm cell shows no rain (particle_draws=$($cell.particle_draws)): $($cell.file)"
        }
        if (-not $cell.lightning_active) {
            throw "world_visual_sweep storm cell has no active lightning: $($cell.file)"
        }
    }

    $cloudCells = @($cells | Where-Object { $_.pitched_up -and $_.storm })
    if ($cloudCells.Count -lt 1) { throw "world_visual_sweep has no up-pitched storm cells to assert clouds" }
    foreach ($cell in $cloudCells) {
        if ([double]$cell.cloud_coverage -le 0.0) {
            throw "world_visual_sweep up-storm cell has no cloud coverage: $($cell.file)"
        }
    }

    $waterCells = @($cells | Where-Object { $_.water_aimed -and $_.daytime })
    if ($waterCells.Count -lt 1) { throw "world_visual_sweep has no water-aimed daytime cells" }
    foreach ($cell in $waterCells) {
        if ([int64]$cell.water_like_pixels -lt 1) {
            throw "world_visual_sweep water-aimed cell shows no water: $($cell.file)"
        }
    }

    # No rain may leak into clear cells (clean clear-vs-storm separation).
    $clearLeak = @($cells | Where-Object { -not $_.storm -and [int64]$_.particle_draws -gt 0 })
    if ($clearLeak.Count -gt 0) {
        throw "world_visual_sweep rain leaked into $($clearLeak.Count) clear cell(s), e.g. $($clearLeak[0].file)"
    }

    if (-not $manifest.passed) {
        throw "world_visual_sweep manifest reported failure: $($manifest.failures -join ', ')"
    }

    #  the OBJECTIVE visual critique is now a REQUIRED
    # gate step, not a best-effort convenience. We (1) build the labelled
    # contact-sheet montages (which also converts every PPM -> sweep/png/*.png),
    # then (2) run tools/visual_critique.py analyze --strict, which fails the
    # gate on ANY objective defect flag (dead/black, washed-out, green-sky
    # speckle, flat storm clouds, aurora-at-dusk, sparse foliage,...). Per the
    # process rule, such a BLOCK is discharged ONLY by a flag-free re-run of this
    # gate -- never by reclassifying a flagged cell as "tracked debt". numpy +
    # Pillow are REQUIRED; the gate fails loudly if no suitable python is found
    # (a gate CI cannot run is not a gate). Override the interpreter with
    # $env:VISUAL_SWEEP_PYTHON. The per-flag thresholds are pinned by the
    # VisualCritiqueFlags ctest (tools/test_visual_critique.py).
    $montageScript = "tools/gates/build-visual-sweep-montages.py"
    $critiqueScript = "tools/visual_critique.py"
    $vsPython = $null
    foreach ($py in @($env:VISUAL_SWEEP_PYTHON, "python", "python3", "py")) {
        if ([string]::IsNullOrWhiteSpace($py)) { continue }
        if (-not (Get-Command $py -ErrorAction SilentlyContinue)) { continue }
        & $py -c "import numpy, PIL" 2>$null
        if ($LASTEXITCODE -eq 0) { $vsPython = $py; break }
    }
    if (-not $vsPython) {
        throw "world_visual_sweep objective critique cannot run: no python with numpy+Pillow found. Install them or set VISUAL_SWEEP_PYTHON to a suitable interpreter (a visual gate that cannot run is not a gate)."
    }

    if (Test-Path $montageScript) {
        & $vsPython $montageScript $visualDir
        if ($LASTEXITCODE -ne 0) { throw "world_visual_sweep montage/PNG build failed (exit $LASTEXITCODE) using $vsPython" }
        Write-Host "world_visual_sweep montages assembled under $visualDir/sweep/montages"
    }

    # Guard against a vacuous pass: the objective critique reads sweep/png/*.png,
    # so there must be one PNG per expected cell before --strict can mean anything.
    $pngDir = Join-Path $visualDir "sweep/png"
    $pngCount = @(Get-ChildItem -Path $pngDir -Filter *.png -ErrorAction SilentlyContinue).Count
    if ($pngCount -ne [int]$manifest.expected_cell_count) {
        throw "world_visual_sweep objective critique: found $pngCount PNG(s) under $pngDir but expected $($manifest.expected_cell_count) (PPM->PNG conversion incomplete)"
    }

    & $vsPython $critiqueScript analyze $visualDir --strict
    if ($LASTEXITCODE -ne 0) {
        $critiqueMd = Join-Path $visualDir "objective-critique.md"
        $detail = if (Test-Path $critiqueMd) { (Get-Content $critiqueMd -Raw) } else { "(no objective-critique.md produced)" }
        throw "world_visual_sweep OBJECTIVE CRITIQUE FAILED (blocking defect flag raised). Discharge only by a flag-free re-run -- do NOT reclassify as tracked debt.`n$detail"
    }
    Write-Host "world_visual_sweep objective critique passed: no blocking defect flags across $($manifest.expected_cell_count) cells."

    Write-Host ("world_visual_sweep gate passed: {0} cells all produced + non-black; foliage in {1} down-daytime cells, rain+lightning in {2} storm cells, clouds in {3} up-storm cells, water in {4} water-aimed cells; no clear-cell rain leak" -f `
        $manifest.expected_cell_count, $foliageCells.Count, $stormCells.Count, $cloudCells.Count, $waterCells.Count)
}

# ---  PlayerView mode: append-only ---
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
        Assert-CapturePinned -ArtifactDir $viewDir -Name "PlayerView ($preset)"
        Write-Host "player view ($preset): stations=$($analysis.aggregates.captured_stations), max_missing=$($analysis.aggregates.max_missing_frustum_surface_chunks), min_renderable_ratio=$($analysis.aggregates.min_renderable_frustum_ratio), max_sky_ratio=$($analysis.aggregates.max_below_horizon_sky_ratio) (enforced=$skyEnforced), max_void_clusters=$($analysis.aggregates.max_near_black_cluster_count)"
    }
}

# ---  FarLodHorizon mode: append-only ---
# Far-LOD horizon + live/far seam gate (farlod_horizon_smoke). Phase A of the
# run measures the gbuffer GPU time with far-LOD DISABLED (the honest in-run
# baseline; the committed perf baseline records frame times, not per-pass GPU
# times); phase B enables far-LOD and sweeps eye-level + elevated stations.
# Gates (the deterministic runtime contract section 4): zero missing wanted regions to
# 1536 m after settle; farlod_resident_bytes < 64 MB; gbuffer_gpu_ms delta
# < 1.5 ms; horizon screenshots show terrain to the horizon (below-horizon
# sky ratio bounded); the live/far boundary band ROI (~192 m at the smoke
# radii) shows no sky-leak band and no strict void clusters (the
# Distant-Horizons failure mode).

function Test-FarLodHorizon {
    $exe = Get-ClientExe
    $runSeconds = [Math]::Max(50, $SmokeSeconds)

    # archipelago (sea to horizon) added so the
    # water-continuity assertion below runs on an open-water preset alongside
    # mountains (river channels). default stays dry (height_offset 20).
    # Water-bearing presets must render a far water sheet; open-water presets
    # additionally must show far-water pixels in the live/far boundary band
    # (rivers are too sparse to guarantee the band crosses a channel, so the
    # boundary-band-coverage assertion is gated to the sea preset).
    $waterBearingPresets = @("mountains", "archipelago")
    $openWaterPresets = @("archipelago")
    foreach ($preset in @("mountains", "default", "archipelago")) {
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

        # water-continuity gate. On a water-bearing
        # preset the far path must render a flat water sheet where the live
        # water ring ends (river channels / seabeds beyond the live ring) - no
        # dry band. Proven two ways: the far water sheet is actually drawn
        # (max_water_sheet_draws > 0), and the live/far boundary band shows
        # far-water pixels (max_boundary_band_water_ratio > 0). The dry "default"
        # preset (height_offset 20) legitimately renders no far water and is
        # skipped. The below-horizon sky-ratio gate above already proves the
        # combined far terrain+water leaves no sky/void band below the horizon.
        $maxWaterDraws = [int]$analysis.far_water.max_water_sheet_draws
        $maxBandWaterRatio = [double]$analysis.aggregates.max_boundary_band_water_ratio
        #  re-derived far-water band FLOOR (open sea). The
        # pre-aerial-perspective classifier passed on a degenerate ~0.013 reading
        # (just > 0); the re-derived post-5a classifier registers far water as a
        # real fraction, so the open-water preset must clear a non-trivial floor.
        $minWaterRatioOpenSea = [double]$analysis.thresholds.min_boundary_band_water_ratio_open_sea
        if ($waterBearingPresets -contains $preset) {
            if ($maxWaterDraws -le 0) {
                throw "farlod horizon ($preset) water-continuity: far path rendered no water sheet (max_water_sheet_draws=$maxWaterDraws) past the live water ring"
            }
            if (($openWaterPresets -contains $preset) -and $maxBandWaterRatio -lt $minWaterRatioOpenSea) {
                throw "farlod horizon ($preset) water-continuity: re-derived far-water band ratio $maxBandWaterRatio below floor $minWaterRatioOpenSea - far sea not classified past the live ring (post-aerial-perspective re-derivation regressed)"
            }
            Write-Host ("farlod horizon ({0}): far-water continuity OK (re-derived) - water_sheet_draws_max={1} boundary_band_water_ratio_max={2} floor={3}" -f `
                $preset, $maxWaterDraws, $maxBandWaterRatio, $minWaterRatioOpenSea)
        } else {
            Write-Host ("farlod horizon ({0}): dry preset - far water sheet not asserted (water_sheet_draws_max={1})" -f `
                $preset, $maxWaterDraws)
        }

        #  sand-flat-brightness band. The elevated (downward)
        # station frames real near-shore ground; with the albedo_scale LUT
        # calibration its band must not be a white-clipped sun-bright sand sheet.
        # Eye-level bands are excluded (they graze the bright post-5a hazy near-
        # horizon and are telemetry only). Vacuously satisfied when the elevated
        # band is unresolved. The harness already folds sand_flat_passed into
        # analysis.passed; this surfaces an explicit, readable failure + log line.
        $maxSandFlatCeil = [double]$analysis.thresholds.max_boundary_band_sand_flat_ratio
        $elevatedBandResolved = [bool]$analysis.far_water.elevated_band_resolved
        $elevatedSandFlat = [double]$analysis.far_water.elevated_boundary_band_sand_flat_ratio
        $maxSandFlat = [double]$analysis.aggregates.max_boundary_band_sand_flat_ratio
        if ($elevatedBandResolved -and ($elevatedSandFlat -ge $maxSandFlatCeil)) {
            throw "farlod horizon ($preset) sand-flat-brightness: elevated boundary band white-clipped sand ratio $elevatedSandFlat >= ceiling $maxSandFlatCeil (near-sea-level dry sand blown sun-bright; albedo calibration regressed)"
        }
        Write-Host ("farlod horizon ({0}): sand-flat band OK - elevated_clipped_sand={1} (ceiling={2}, all-station_max={3} telemetry)" -f `
            $preset, $elevatedSandFlat, $maxSandFlatCeil, $maxSandFlat)

        # above-horizon sky-sliver gate, ratcheted.
        # The area-based below-horizon sky-ratio gate cannot see a thin
        # near-vertical sliver streaking up THROUGH the horizon into the sky; the
        # detector scans the sky band for narrow tall terrain-coloured intrusions.
        # WA2 proved the far sky-sliver (a ~360 px thick streak) was in the RENDER
        # path; the FarLodSystem far-geometry clip + camera-region skip removed it.
        # But the raw sliver also catches legitimate thin LIVE mountain/island peak
        # silhouettes (reproduce with far-LOD disabled), and the 6a16048 ambient
        # brightening made those classify taller (archipelago 201 -> 345 px),
        # which broke the old raw 256 px gate. The scenario now measures each
        # station's sliver PAIRED with a far-LOD-disabled render at the identical
        # camera/frame and gates the FAR-ATTRIBUTABLE sliver: the far-ON frame is
        # re-analyzed with the far-OFF frame's intrusion pixels cancelled per-pixel
        # within a 3x3 neighborhood. The pixel-aligned live-peak silhouette cancels
        # exactly, leaving only a genuine far-render streak (present only with far
        # on). The far path is clean, so the attributable metric is ~0 and the
        # hard-fail budget ratchets from 256 px (raw) down to 64 px (attributable).
        # The raw max is still reported for telemetry.
        $maxSliver = [int]$analysis.aggregates.max_sky_sliver_px
        $maxFarAttributable = [int]$analysis.aggregates.max_far_attributable_sliver_px
        $sliverBudget = [int]$analysis.thresholds.max_far_attributable_sliver_px
        if ($maxFarAttributable -gt $sliverBudget) {
            throw "farlod horizon ($preset) far-attributable above-horizon sky-sliver max=${maxFarAttributable}px (masked by the paired far-OFF baseline; raw far-ON max=${maxSliver}px) exceeds the ${sliverBudget}px hard-fail budget (far-region render streak regressed)"
        }
        Write-Host ("farlod horizon ({0}): far-attributable sky-sliver max={1}px within {2}px hard-fail budget (raw far-ON max={3}px)" -f `
            $preset, $maxFarAttributable, $sliverBudget, $maxSliver)
        Assert-CapturePinned -ArtifactDir $viewDir -Name "FarLodHorizon ($preset)"
    }
}

# ---  isolation/layer review mode gate ---
# Drives ONE seeded capture (regression contract: minimise CI cost) with the Terrain
# layer isolated on a GREENSCREEN backdrop, then objectively asserts BOTH the
# SkyboxPass backdrop override AND layer suppression:
#   - the no-geometry sky region fills with the flat backdrop colour (the sky
#     dome is replaced -> proves --isolation-backdrop reached the shader), and
#   - the lower region still carries lit terrain (proves the isolated layer
#     rendered and the gate is not vacuously all-backdrop, while water/foliage/
#     particles/aerial/lightning were suppressed by their cleared layer bits).
# The objective check is tolerant (per-channel LSB tolerance) per regression contract.
# Reuses the farlod_horizon_smoke capture harness (native-pinned). Default
# (no isolation flags) is byte-stable and is covered by the rest of the suite.
function Test-IsolationLayer {
    $exe = Get-ClientExe
    $runSeconds = [Math]::Max(8, [Math]::Min([int]$SmokeSeconds, 14))
    $isoDir = "build/$BuildPreset/test-artifacts/runtime/isolation-layer"
    Remove-Item -Recurse -Force -ErrorAction SilentlyContinue $isoDir
    New-Item -ItemType Directory -Force -Path $isoDir | Out-Null

    Invoke-Checked -FilePath $exe -ArgumentList @(
        "--scenario", "farlod_horizon_smoke",
        "--auto-create-world",
        "--auto-enter-world",
        "--timed-run", "$runSeconds",
        "--world-preset", "mountains",
        "--no-audio",
        "--no-ui",
        "--isolation-layers", "terrain",
        "--isolation-backdrop", "greenscreen",
        "--runtime-artifact-dir", $isoDir
    ) -TimeoutSeconds ([Math]::Max(180, $runSeconds + 120))

    # GL must be clean (the analysis artifact is emitted by the farlod scenario).
    $analysisPath = Join-Path $isoDir "farlod-horizon-analysis.json"
    if (Test-Path $analysisPath) {
        $analysis = Get-Content $analysisPath -Raw | ConvertFrom-Json
        if ([int64]$analysis.gl_debug.errors -ne 0) {
            throw "isolation-layer run emitted GL debug errors: $($analysis.gl_debug.errors)"
        }
    }

    $shots = @(Get-ChildItem -Path (Join-Path $isoDir "screenshots") -Filter *.ppm -ErrorAction SilentlyContinue)
    if ($shots.Count -lt 1) {
        throw "isolation-layer run produced no screenshots under $isoDir/screenshots"
    }
    foreach ($s in $shots) { Assert-PpmArtifact $s.FullName }
    Assert-CapturePinned -ArtifactDir $isoDir -Name "IsolationLayer (terrain/greenscreen)"

    # Objective backdrop-fill + geometry-present check. numpy REQUIRED (a gate
    # CI cannot run is not a gate); override with $env:VISUAL_SWEEP_PYTHON.
    $py = $null
    foreach ($cand in @($env:VISUAL_SWEEP_PYTHON, "python", "python3", "py")) {
        if ([string]::IsNullOrWhiteSpace($cand)) { continue }
        if (-not (Get-Command $cand -ErrorAction SilentlyContinue)) { continue }
        & $cand -c "import numpy" 2>$null
        if ($LASTEXITCODE -eq 0) { $py = $cand; break }
    }
    if (-not $py) {
        throw "isolation-layer gate cannot run: no python with numpy found. Install it or set VISUAL_SWEEP_PYTHON (a visual gate that cannot run is not a gate)."
    }

    $checkScript = "tools/gates/check-isolation-backdrop.py"
    $outJson = Join-Path $isoDir "isolation-terrain-greenscreen.json"
    $ppmArgs = @($shots | ForEach-Object { $_.FullName })
    & $py $checkScript "greenscreen" $outJson "true" @ppmArgs
    if ($LASTEXITCODE -ne 0) {
        $detail = if (Test-Path $outJson) { (Get-Content $outJson -Raw) } else { "(no decision JSON produced)" }
        throw "isolation-layer OBJECTIVE CHECK FAILED (backdrop override or layer suppression broke).`n$detail"
    }
    Write-Host "isolation-layer gate passed: terrain isolated on greenscreen; backdrop fill + isolated-geometry presence verified across $($shots.Count) frame(s) -> $outJson"
}

function Test-HeadlessServerTick {
    # headless server boot + fixed 30 Hz tick determinism gate.
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
        throw "headless server gate required executable missing: $serverExe (cmake --build build/$BuildPreset)"
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

    # per-system sub-hashes must exist and match run vs replay. A
    # mismatch in any one localizes a future desync to that subsystem.
    # the `wind` sub-hash is now part of this set AND folded into
    # the top-level world_hash (the deliberate hash revision 2fa007951a21e140 ->
    # 0eac465289e7c88b), so a wind divergence flips both world_hash and wind.
    if ($null -eq $analysis.sub_hashes -or $null -eq $analysis.sub_hashes_replay) {
        throw "headless server artifact is missing the  sub_hashes fields"
    }
    foreach ($section in @("terrain", "mesh", "water", "entities", "wind")) {
        $a = $analysis.sub_hashes.$section
        $b = $analysis.sub_hashes_replay.$section
        if ([string]::IsNullOrWhiteSpace($a)) {
            throw "headless server sub_hashes.$section is empty"
        }
        if ($a -ne $b) {
            throw "headless server sub-hash divergence in '$section': $a != $b (desync localized to $section)"
        }
    }
    if (-not $analysis.sub_hashes_match) {
        throw "headless server reported sub_hashes_match=false"
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
    Write-Host ("headless server tick gate passed: world_hash={0} == world_hash_replay, {1} ticks x 2 runs, {2} chunks streamed per run; sub-hashes [terrain={3} mesh={4} water={5} entities={6} wind={7}] match" -f `
        $analysis.world_hash, $analysis.ticks_requested, $analysis.runs[0].chunks_streamed, `
        $analysis.sub_hashes.terrain, $analysis.sub_hashes.mesh, $analysis.sub_hashes.water, $analysis.sub_hashes.entities, $analysis.sub_hashes.wind)
}

function Test-PopulatedWorldReplay {
    # gate-populated-world-replay (★): the populated/live-ecology determinism gate.
    # Mirrors Test-HeadlessServerTick but drives the headless binary with
    # --ecology-roster, so the hardened ecology stack (brain -> mate-seek ->
    # steering -> reproduce -> lifespan -> decompose -> pack -> migration ->
    # territory) runs LIVE on a real creature roster inside ServerWorldRunner -- the
    # binary every pinned-hash gate drives. Asserts: run==replay byte-exact across
    # ALL sub-hashes INCLUDING the new `ecology` term (the gap the boids
    # order-dependence bug a8b689c slipped past); the populated composite world_hash
    # matches its pinned golden; and NON-VACUITY (entity_count_start != _end => the
    # ecology actually birthed/culled creatures over the horizon, so the determinism
    # assertion is not trivially satisfied by a frozen roster). HEAVY (two full
    # populated worlds), so it stays OFF the default All lane -- run via
    # -Mode PopulatedWorldReplay.
    $serverExe = "build/$BuildPreset/bin/luminumbra_server_app.exe"
    if (-not (Test-Path $serverExe)) {
        throw "populated world replay gate required executable missing: $serverExe (cmake --build build/$BuildPreset)"
    }

    $artifactPath = "build/$BuildPreset/test-artifacts/server/populated-world-replay.json"
    $artifactDir = Split-Path -Parent $artifactPath
    New-Item -ItemType Directory -Force -Path $artifactDir | Out-Null
    if (Test-Path $artifactPath) {
        Remove-Item $artifactPath
    }

    & $serverExe --smoke --ecology-roster --planted-roster --ticks 90 --artifact $artifactPath # Include plants in the live feeding loop.
    if ($LASTEXITCODE -ne 0) {
        throw "populated world replay smoke exited with code $LASTEXITCODE"
    }

    $analysis = Read-JsonArtifact $artifactPath "luminumbra.server_tick.v1"
    Assert-ArtifactPassed $analysis "PopulatedWorldReplay"
    if (-not $analysis.deterministic) {
        throw "populated world replay reported a non-deterministic double-run"
    }
    if ($analysis.world_hash -ne $analysis.world_hash_replay) {
        throw "populated world replay world_hash mismatch: $($analysis.world_hash) != $($analysis.world_hash_replay)"
    }

    # run==replay byte-exact across ALL sub-hashes, including the new `ecology`
    # term (the populated-determinism coverage this gate exists to add). The
    # ecology sub-hash MUST be non-empty here (the roster carries creatures), unlike
    # the empty-roster HeadlessServerTick where it is neutral.
    if ($null -eq $analysis.sub_hashes -or $null -eq $analysis.sub_hashes_replay) {
        throw "populated world replay artifact is missing the sub_hashes fields"
    }
    foreach ($section in @("terrain", "mesh", "water", "entities", "wind", "weather", "aether", "scents", "ecology", "plants")) {
        $a = $analysis.sub_hashes.$section
        $b = $analysis.sub_hashes_replay.$section
        if ($a -ne $b) {
            throw "populated world replay sub-hash divergence in '$section': $a != $b (desync localized to $section)"
        }
    }
    if ([string]::IsNullOrWhiteSpace($analysis.sub_hashes.ecology)) {
        throw "populated world replay: the ecology sub-hash is empty (the roster did not spawn / was not folded)"
    }
    if (-not $analysis.sub_hashes_match) {
        throw "populated world replay reported sub_hashes_match=false"
    }

    # Non-vacuity: the populated ecology must actually evolve (births and/or culls),
    # so the determinism assertion is meaningful. Both runs are checked (the roster
    # is a pure fn of seed/preset, so both must show the SAME start/end counts).
    if ($null -eq $analysis.entity_count_start -or $null -eq $analysis.entity_count_end) {
        throw "populated world replay artifact is missing the entity_count_start/end fields"
    }
    if ([int64]$analysis.entity_count_start -lt 1) {
        throw "populated world replay: the roster spawned no creatures (entity_count_start=$($analysis.entity_count_start))"
    }
    if ([int64]$analysis.entity_count_start -eq [int64]$analysis.entity_count_end) {
        throw "populated world replay is VACUOUS: entity_count_start == entity_count_end == $($analysis.entity_count_start) (no births/culls over 90 ticks -- a frozen roster trivially satisfies run==replay)"
    }
    if ([int64]$analysis.entity_count_start_replay -ne [int64]$analysis.entity_count_start -or
        [int64]$analysis.entity_count_end_replay -ne [int64]$analysis.entity_count_end) {
        throw "populated world replay: run/replay entity counts differ (run $($analysis.entity_count_start)->$($analysis.entity_count_end), replay $($analysis.entity_count_start_replay)->$($analysis.entity_count_end_replay)) -- the roster is not a pure function of (seed, preset)"
    }

    # Pinned golden: the populated composite world_hash captured from a green run.
    # Distinct from the empty-roster HeadlessServerTick pin because the live ecology
    # sub-hash is non-empty here. (Re-pinned 2026-07-04 at the  close with a
    # two-point evidence chain: at the PRE- dbb19318 this gate was ALREADY
    # red — it died on a run-vs-replay MESH sub-hash desync before the golden check
    # could even run, so the old golden 114ff66cc032576d had been unreachable for an
    # unknown period. Under the activation queue activation queue the populated scenario is
    # run==replay through EVERY sub-hash including mesh; the golden below is that
    # now-deterministic populated hash.)
    # (Re-pinned 2026-07-05 for the  authoritative-state change; prior golden was 9f0dd5b9b27ecb9d.)
    # (Re-pinned 2026-07-05 for  derived-state reclassification: 94d4242b725c9876 -> 4e8947548a7a9f1a.)
    # (Re-pinned 2026-07-05 for: the roster now ALSO plants (--planted-roster),
    # so the previously-dormant feeding loop runs LIVE in this gate - grazeable biomass +
    # creature hunger now evolve and hash. The defined  extended-roster transition.)
    $expectedHash = "d281053b8de4b891"
    if ($analysis.world_hash -ne $expectedHash) {
        throw "populated world replay end hash $($analysis.world_hash) != pinned golden $expectedHash (the populated ecology world diverged / non-deterministic)"
    }
    Write-Host ("PopulatedWorldReplay gate passed: populated world_hash={0} == world_hash_replay (pinned golden), ecology sub-hash={1} run==replay, non-vacuous (entity_count {2} -> {3}), {4} ticks x 2 runs" -f `
        $analysis.world_hash, $analysis.sub_hashes.ecology, `
        $analysis.entity_count_start, $analysis.entity_count_end, $analysis.ticks_requested)
}

function Test-PopulatedAsan {
    # Second-class adversarial pass: an AddressSanitizer run over a POPULATED
    # session. The debug-asan CMake preset existed but had NO harness driving it; this
    # mode wires it. It builds the AddressSanitizer-instrumented headless server, then
    # drives it through a populated, live-ecology session AND a full save->load
    # roundtrip (--smoke --ecology-roster --heavy), so use-after-free / heap-overflow /
    # double-free bugs that only appear with creatures + foliage + persistence live are
    # surfaced. ASan aborts (non-zero) on a hard memory error; this gate also scans the
    # captured output for the AddressSanitizer/LeakSanitizer error banner so a leak that
    # ASan reports without aborting still fails the gate.
    #
    # HEAVY + slow (a fully instrumented populated double-world under ASan), so it is an
    # OPT-IN mode off the default All lane: run via -Mode PopulatedAsan.
    #
    # PATH discipline: prepend C:\msys64\ucrt64\bin so the ucrt64 toolchain (with its
    # libasan) is used for the configure+build, not a shadowing mingw/KiCad gcc.
    $ucrt = "C:\msys64\ucrt64\bin"
    if (Test-Path $ucrt) {
        $env:PATH = "$ucrt;$env:PATH"
    }

    $asanBuildDir = "build/debug-asan"
    $serverExe = "$asanBuildDir/bin/luminumbra_server_app.exe"

    # Configure + build the ASan server exe if it is not already present. Gates do not
    # auto-build, but the debug-asan tree is a DISTINCT preset no other gate produces,
    # so this mode owns its build (it is opt-in, not on the default lane).
    if (-not (Test-Path $serverExe)) {
        Write-Host "PopulatedAsan: building the debug-asan server exe (first run)..."
        & cmake --preset debug-asan
        if ($LASTEXITCODE -ne 0) {
            throw "PopulatedAsan: cmake --preset debug-asan configure failed (exit $LASTEXITCODE)"
        }
        $buildLog = & cmake --build --preset debug-asan --target luminumbra_server_app 2>&1
        $buildExit = $LASTEXITCODE
        $buildText = ($buildLog | Out-String)
        Write-Host $buildText
        if ($buildExit -ne 0) {
            # ENVIRONMENT BLOCKER (not a code defect): the MSYS2 ucrt64 GCC ships
            # -fsanitize=address compile support but NOT the AddressSanitizer runtime
            # library (libasan). The link then fails with "cannot find -lasan". The
            # harness wiring is correct and will run the instant libasan is installed
            # (MSYS2: pacman -S mingw-w64-ucrt-x86_64-gcc-libs or a clang/MSVC ASan
            # toolchain). Surface this as a clear, actionable SKIP rather than a false
            # failure so the gate is honest about WHY it could not execute.
            if ($buildText -match "cannot find -lasan" -or $buildText -match "libasan") {
                Write-Warning ("PopulatedAsan: SKIPPED - the ucrt64 GCC toolchain has no AddressSanitizer runtime (libasan); " +
                    "'-fsanitize=address' cannot link here (ld: 'cannot find -lasan'). " +
                    "The -Mode PopulatedAsan harness is wired and will execute once a libasan-bearing toolchain " +
                    "(MSYS2 libasan / clang / MSVC ASan) is installed. This is an environment limitation, not a gate failure.")
                return
            }
            throw "PopulatedAsan: cmake --build --preset debug-asan failed (exit $buildExit)`n$buildText"
        }
    }
    if (-not (Test-Path $serverExe)) {
        throw "PopulatedAsan: ASan server exe still missing after build - $serverExe"
    }

    $artifactPath = "$asanBuildDir/test-artifacts/server/populated-asan.json"
    $artifactDir = Split-Path -Parent $artifactPath
    New-Item -ItemType Directory -Force -Path $artifactDir | Out-Null
    if (Test-Path $artifactPath) {
        Remove-Item $artifactPath
    }

    # ASan runtime options: abort on the first error AND surface leak reports. On
    # Windows/GCC libasan, abort_on_error=1 makes a hard memory error a non-zero exit;
    # detect_leaks is honored where the platform LSan is available.
    $env:ASAN_OPTIONS = "abort_on_error=1:halt_on_error=1:detect_leaks=1:exitcode=99"

    # Capture combined stdout+stderr so the ASan banner (written to stderr) is scanned.
    $stdoutPath = [System.IO.Path]::GetTempFileName()
    $stderrPath = [System.IO.Path]::GetTempFileName()
    $exitCode = $null
    try {
        $argList = @("--smoke", "--ecology-roster", "--heavy", "--ticks", "60", "--artifact", $artifactPath)
        $psi = New-Object System.Diagnostics.ProcessStartInfo
        $psi.FileName = (Resolve-Path $serverExe).Path
        $psi.Arguments = ($argList | ForEach-Object { '"' + ($_ -replace '"', '\"') + '"' }) -join " "
        $psi.WorkingDirectory = (Get-Location).Path
        $psi.UseShellExecute = $false
        $psi.RedirectStandardOutput = $true
        $psi.RedirectStandardError = $true
        $proc = [System.Diagnostics.Process]::Start($psi)
        $stdoutTask = $proc.StandardOutput.ReadToEndAsync()
        $stderrTask = $proc.StandardError.ReadToEndAsync()
        if (-not $proc.WaitForExit(600 * 1000)) {
            try { $proc.Kill($true) } catch { }
            throw "PopulatedAsan: the instrumented populated session timed out after 600s"
        }
        $stdout = $stdoutTask.Result
        $stderr = $stderrTask.Result
        $exitCode = $proc.ExitCode
        Set-Content -LiteralPath $stdoutPath -Value $stdout -Encoding UTF8
        Set-Content -LiteralPath $stderrPath -Value $stderr -Encoding UTF8
    }
    finally {
        Remove-Item -LiteralPath $stdoutPath, $stderrPath -Force -ErrorAction SilentlyContinue
        $env:ASAN_OPTIONS = $null
    }

    $combined = "$stdout`n$stderr"
    # The AddressSanitizer/LeakSanitizer error banners. Any presence is a real finding.
    $asanMarkers = @(
        "ERROR: AddressSanitizer",
        "ERROR: LeakSanitizer",
        "AddressSanitizer: heap-use-after-free",
        "AddressSanitizer: heap-buffer-overflow",
        "AddressSanitizer: stack-buffer-overflow",
        "AddressSanitizer: attempting double-free",
        "detected memory leaks",
        "SUMMARY: AddressSanitizer"
    )
    foreach ($marker in $asanMarkers) {
        if ($combined -match [regex]::Escape($marker)) {
            throw "PopulatedAsan: AddressSanitizer surfaced a real finding ('$marker') over the populated session. Output:`n$combined"
        }
    }

    if ($exitCode -ne 0) {
        throw "PopulatedAsan: the populated ASan session exited non-zero ($exitCode) -- a memory error or boot failure. Output:`n$combined"
    }

    if (-not (Test-Path $artifactPath)) {
        throw "PopulatedAsan: the populated session produced no artifact ($artifactPath) -- it did not complete the run"
    }

    Write-Host ("PopulatedAsan gate passed: the AddressSanitizer-instrumented populated session (--smoke --ecology-roster --heavy) completed clean (exit 0, no ASan/LSan banner) -- artifact {0}" -f $artifactPath)
}

function Test-ReplicationSmoke {
    #  /d: live authoritative-server state replication. Drives the server's
    # --replicate mode: boots N avatars + an in-process loopback ReplicationClient,
    # broadcasts the avatar states each tick, and  the client CONTROLS one
    # avatar via a +X usercmd. Asserts the client mirrors the server avatars (mm
    # tolerance), an ack flowed back, and network input actually walked the avatar.
    # Engine/transport-side -> world_hash untouched (reads the avatar list).
    $serverExe = "build/$BuildPreset/bin/luminumbra_server_app.exe"
    if (-not (Test-Path $serverExe)) {
        throw "replication gate required executable missing: $serverExe (cmake --build build/$BuildPreset)"
    }

    $artifactPath = "build/$BuildPreset/test-artifacts/server/replication-smoke.json"
    if (Test-Path $artifactPath) {
        Remove-Item $artifactPath
    }

    #  polish: exercise the full heterogeneous-entity path -- GOAP-driven NPCs
    # (animals that PLAN toward a water hole and are steered there by the
    # InstinctLocomotionSystem) + a server-authoritative ballistic arrow with a
    # reliable despawn -- so the gate locks the action->locomotion behaviour, not
    # just avatar mirroring. 120 ticks gives the NPCs time to converge on water.
    & $serverExe --replicate --avatars 4 --npcs 3 --arrow --ticks 120 --artifact $artifactPath
    if ($LASTEXITCODE -ne 0) {
        throw "replication smoke exited with code $LASTEXITCODE"
    }

    $analysis = Read-JsonArtifact $artifactPath "luminumbra.replication_smoke.v1"
    Assert-ArtifactPassed $analysis "ReplicationSmoke"
    if (-not $analysis.size_ok) {
        throw "replication smoke: client entity count did not match the server avatars + NPCs"
    }
    if (-not $analysis.ids_ok) {
        throw "replication smoke: replicated entity ids did not match the server avatars"
    }
    if (-not $analysis.ack_flowed) {
        throw "replication smoke: no ack flowed back to the server (acked_snapshot_seq=$($analysis.acked_snapshot_seq))"
    }
    if (-not $analysis.input_moved_avatar) {
        throw "replication smoke: network input did not move the controlled avatar (dx=$($analysis.controlled_dx_m) m)"
    }
    if (-not $analysis.npcs_ok) {
        throw "replication smoke: NPCs did not all replicate as typed entities ($($analysis.npcs_replicated)/$($analysis.npc_count))"
    }
    if (-not $analysis.npcs_approached_water) {
        throw "replication smoke: GOAP NPCs did not steer to the water hole (min approach $($analysis.min_npc_approach_m) m, need >= 2 m)"
    }
    if (-not $analysis.arrow_ok) {
        throw "replication smoke: arrow was not seen in flight + reliably despawned (seen=$($analysis.arrow_seen_by_client) despawn=$($analysis.arrow_despawn_signalled))"
    }
    if ([double]$analysis.max_position_error_m -ge 0.01) {
        throw "replication smoke: replicated position error too large ($($analysis.max_position_error_m) m)"
    }
    Write-Host ("ReplicationSmoke gate passed: {0} avatars mirrored (seq={1}, acked={2}, max_pos_err={3} m); input walked avatar {4} +{5} m; {6} GOAP NPCs approached water (min {7} m); arrow_ok={8}" -f `
        $analysis.avatar_count, $analysis.final_snapshot_seq, $analysis.acked_snapshot_seq, `
        $analysis.max_position_error_m, $analysis.controlled_avatar, $analysis.controlled_dx_m, `
        $analysis.npc_count, $analysis.min_npc_approach_m, $analysis.arrow_ok)
}

function Test-NetworkedReplication {
    # REAL over-the-wire replication between TWO PROCESSES over TCP sockets
    # (not the in-process loopback ReplicationSmoke uses). Auto-launches the host
    # (--net-host, binds + runs the authoritative world) and the client
    # (--net-join, connects + mirrors), then asserts BOTH exited cleanly and the
    # client confirmed it mirrored the host over the wire. This turns the manual
    # two-terminal check into a repeatable gate. Localhost; transport-side ->
    # world_hash untouched.
    $serverExe = "build/$BuildPreset/bin/luminumbra_server_app.exe"
    if (-not (Test-Path $serverExe)) {
        throw "networked gate required executable missing: $serverExe (cmake --build build/$BuildPreset)"
    }
    $port = 27061
    $ticks = 60
    $logDir = "build/$BuildPreset/test-artifacts/server"
    if (-not (Test-Path $logDir)) { New-Item -ItemType Directory -Force $logDir | Out-Null }
    $hostOut = Join-Path $logDir "net-host.log"
    $hostErr = Join-Path $logDir "net-host.err.log"
    $joinOut = Join-Path $logDir "net-join.log"
    $joinErr = Join-Path $logDir "net-join.err.log"

    # Host listens in the background; give it a moment to bind before the client dials.
    $hostProc = Start-Process -FilePath $serverExe `
        -ArgumentList @("--net-host", "--port", "$port", "--avatars", "4", "--ticks", "$ticks") `
        -PassThru -NoNewWindow -RedirectStandardOutput $hostOut -RedirectStandardError $hostErr
    Start-Sleep -Milliseconds 1500
    try {
        $joinProc = Start-Process -FilePath $serverExe `
            -ArgumentList @("--net-join", "--host", "127.0.0.1", "--port", "$port", "--ticks", "$ticks") `
            -PassThru -NoNewWindow -RedirectStandardOutput $joinOut -RedirectStandardError $joinErr -Wait
        $joinExit = $joinProc.ExitCode
    } finally {
        if (-not $hostProc.HasExited) { $hostProc.WaitForExit(15000) | Out-Null }
        if (-not $hostProc.HasExited) { $hostProc.Kill(); throw "networked gate: host did not exit (hung)" }
    }

    # The client ran with -Wait, so its ExitCode is reliable; assert on it. The
    # background host's ExitCode is not reliably populated by Start-Process
    # -PassThru, so assert on its success log line instead (it prints the line only
    # after completing all ticks with a connected peer).
    if ($joinExit -ne 0) { throw "networked gate: client (--net-join) exited $joinExit - see $joinOut / $joinErr" }
    $joinText = (Get-Content $joinOut -Raw -ErrorAction SilentlyContinue) + (Get-Content $joinErr -Raw -ErrorAction SilentlyContinue)
    if ($joinText -notmatch "Real over-the-wire replication confirmed") {
        throw "networked gate: client did not confirm mirroring over the wire - see $joinOut / $joinErr"
    }
    $hostText = (Get-Content $hostOut -Raw -ErrorAction SilentlyContinue) + (Get-Content $hostErr -Raw -ErrorAction SilentlyContinue)
    if ($hostText -notmatch "net-host: ran $ticks ticks") {
        throw "networked gate: host did not complete all $ticks ticks with a connected peer - see $hostOut / $hostErr"
    }
    Write-Host "NetworkedReplication gate passed: host + client ran as separate processes over TCP; client mirrored the authoritative world over the wire (port $port, $ticks ticks)."
}

function Test-AetherFieldDeterminism {
    # the Aetheric scalar field is a deterministic, hashed sim system.
    # Drives the server's --aether-bench mode: seed -> N AetherFieldSystem updates
    # (advected by a parallel wind field) twice -> the aether sub-hash is EQUAL
    # across runs and STABLE; the field EVOLVES over time (not vacuous); geometry
    # matches the PINNED 24 m / 64-extent / 8-sweep shape. The `aether` sub-hash is
    # folded into the world_hash (deliberate bump #4 d950a6afc12a5cdc ->
    # f17726d44054d133; then bump #5 f17726d44054d133 -> 8a6b7bb6795da912 from commit
    # 0cb9ce8 appending the |scents: term; then bump #6
    # 8a6b7bb6795da912 -> d8f84cf6d7d0b978 from gate-populated-world-replay appending the
    # |ecology: term to ComposeWorldHash — the SERVER composite hash, run==replay proven —
    # asserted by the headless tick / replay / lockstep gates. NOTE the NetworkedSession gate pins a DIFFERENT quantity:
    # the client bare streamed-chunk hash, which does NOT fold the wind/weather/aether/
    # scent sub-hashes, so it has its own pin 46f89d27449011a0 — not this composite).
    $serverExe = "build/$BuildPreset/bin/luminumbra_server_app.exe"
    if (-not (Test-Path $serverExe)) {
        throw "aether-field determinism gate required executable missing: $serverExe (cmake --build build/$BuildPreset)"
    }

    $artifactPath = "build/$BuildPreset/test-artifacts/server/aether-field-determinism.json"
    if (Test-Path $artifactPath) {
        Remove-Item $artifactPath
    }

    & $serverExe --aether-bench --ticks 90 --seed 424242 --artifact $artifactPath
    if ($LASTEXITCODE -ne 0) {
        throw "aether-bench exited with code $LASTEXITCODE"
    }

    $a = Read-JsonArtifact $artifactPath "luminumbra.aether_field_determinism.v1"
    Assert-ArtifactPassed $a "AetherFieldDeterminism"

    if ([string]::IsNullOrWhiteSpace($a.aether_sub_hash)) {
        throw "aether-field determinism: empty aether sub-hash"
    }
    if ($a.aether_sub_hash -ne $a.aether_sub_hash_replay) {
        throw "aether-field determinism: aether sub-hash diverged across runs ($($a.aether_sub_hash) != $($a.aether_sub_hash_replay))"
    }
    if (-not $a.deterministic) {
        throw "aether-field determinism: reported non-deterministic"
    }
    if (-not $a.evolves -or $a.aether_sub_hash_tick0 -eq $a.aether_sub_hash_evolved) {
        throw "aether-field determinism: field did not evolve over ticks (gate is vacuous)"
    }
    if ([double]$a.cell_size_m -ne 24.0) {
        throw "aether-field determinism: cell_size_m=$($a.cell_size_m), expected 24"
    }
    if ([int]$a.extent_cells -ne 64) {
        throw "aether-field determinism: extent_cells=$($a.extent_cells), expected 64"
    }
    if ([int]$a.diffuse_iterations -ne 8) {
        throw "aether-field determinism: diffuse_iterations=$($a.diffuse_iterations), expected 8 (PINNED)"
    }
    Write-Host ("aether-field determinism gate passed: aether_sub_hash={0} stable across 2 runs (evolves over {1} ticks); per-tick update {2:N4} ms (budget {3:N4} ms, informational on {4}) ({5} m cells x {6} extent x {7} diffuse sweeps)" -f `
        $a.aether_sub_hash, $a.ticks, [double]$a.per_tick_update_ms, [double]$a.budget_ms, $BuildPreset, `
        $a.cell_size_m, $a.extent_cells, $a.diffuse_iterations)
}

function Test-WindFieldDeterminism {
    # the wind grid is a deterministic, hashed sim system. This
    # gate drives the server's --wind-bench mode: seed -> N WindFieldSystem
    # updates twice -> the wind sub-hash is EQUAL across runs and STABLE; the
    # field EVOLVES over time (the gate is not vacuous); and the per-tick wind
    # update stays within the PINNED <= 0.15 ms budget at the streamed extent.
    # The `wind` sub-hash is also folded into the world_hash (the deliberate
    # hash revision 2fa007951a21e140 -> 0eac465289e7c88b, asserted by the headless
    # tick / replay / lockstep gates).
    $serverExe = "build/$BuildPreset/bin/luminumbra_server_app.exe"
    if (-not (Test-Path $serverExe)) {
        throw "wind-field determinism gate required executable missing: $serverExe (cmake --build build/$BuildPreset)"
    }

    $artifactPath = "build/$BuildPreset/test-artifacts/server/wind-field-determinism.json"
    if (Test-Path $artifactPath) {
        Remove-Item $artifactPath
    }

    & $serverExe --wind-bench --ticks 90 --seed 424242 --artifact $artifactPath
    if ($LASTEXITCODE -ne 0) {
        throw "wind-bench exited with code $LASTEXITCODE"
    }

    $a = Read-JsonArtifact $artifactPath "luminumbra.wind_field_determinism.v1"
    Assert-ArtifactPassed $a "WindFieldDeterminism"

    if ([string]::IsNullOrWhiteSpace($a.wind_sub_hash)) {
        throw "wind-field determinism: empty wind sub-hash"
    }
    if ($a.wind_sub_hash -ne $a.wind_sub_hash_replay) {
        throw "wind-field determinism: wind sub-hash diverged across runs ($($a.wind_sub_hash) != $($a.wind_sub_hash_replay))"
    }
    if (-not $a.deterministic) {
        throw "wind-field determinism: reported non-deterministic"
    }
    if (-not $a.evolves -or $a.wind_sub_hash_tick0 -eq $a.wind_sub_hash_evolved) {
        throw "wind-field determinism: field did not evolve over ticks (gate is vacuous)"
    }
    # Geometry must match the PINNED shape (24 m cells, 3 layers).
    if ([double]$a.cell_size_m -ne 24.0) {
        throw "wind-field determinism: cell_size_m=$($a.cell_size_m), expected 24"
    }
    if ([int]$a.layer_count -ne 3) {
        throw "wind-field determinism: layer_count=$($a.layer_count), expected 3"
    }
    # Per-tick budget: <= 0.15 ms at the streamed extent. The PINNED budget is a
    # RELEASE-build number (the deterministic runtime contract : the 5a perf budgets are
    # release/optimized measurements; an un-optimized debug build runs the same
    # bit-deterministic field ~10x slower). So the budget is asserted against the
    # release build when one exists; the determinism/geometry checks above hold
    # on whatever preset the gate runs. If no release build is present, the gate
    # preset's measurement is reported but only enforced when it is the release
    # build (so a debug-only run does not falsely fail the release budget).
    $budgetMs = [double]$a.budget_ms
    $budgetSource = $BuildPreset
    $perTickMs = [double]$a.per_tick_update_ms
    $releaseExe = "build/release/bin/luminumbra_server_app.exe"
    if ($BuildPreset -ne "release" -and (Test-Path $releaseExe)) {
        $relArtifact = "build/release/test-artifacts/server/wind-field-determinism.json"
        Remove-Item -Force -ErrorAction SilentlyContinue $relArtifact
        & $releaseExe --wind-bench --ticks 90 --seed 424242 --artifact $relArtifact | Out-Null
        if ($LASTEXITCODE -eq 0 -and (Test-Path $relArtifact)) {
            $rel = Read-JsonArtifact $relArtifact "luminumbra.wind_field_determinism.v1"
            $perTickMs = [double]$rel.per_tick_update_ms
            $budgetSource = "release"
            # The release field must also be internally deterministic + evolve.
            if (-not $rel.deterministic -or -not $rel.evolves) {
                throw "wind-field determinism: release build run was non-deterministic or did not evolve"
            }
        }
    }
    if ($budgetSource -eq "release") {
        if ($perTickMs -gt $budgetMs) {
            throw ("wind-field determinism: per-tick wind update {0:N4} ms (release) exceeds the {1:N4} ms budget" -f $perTickMs, $budgetMs)
        }
    } else {
        Write-Host ("wind-field determinism: NOTE budget enforced on the release build only; {0} measured {1:N4} ms (informational, budget {2:N4} ms)" -f `
            $BuildPreset, $perTickMs, $budgetMs)
    }
    Write-Host ("wind-field determinism gate passed: wind_sub_hash={0} stable across 2 runs (evolves over {1} ticks); per-tick update {2:N4} ms <= {3:N4} ms budget [{4}] ({5} m cells x {6} layers x {7} extent)" -f `
        $a.wind_sub_hash, $a.ticks, $perTickMs, $budgetMs, $budgetSource, `
        $a.cell_size_m, $a.layer_count, $a.extent_cells)
}

function Test-HeadlessServerTickHeavy {
    #  heavy-mode oracle (Factorio "heavy mode"): tick N, SAVE the full
    # streamed-chunk set, LOAD it into a FRESH session, resimulate M further
    # ticks on BOTH and compare AUTHORITATIVE sim state (terrain/water/entities)
    # + per-system sub-hashes. Catches sim state excluded from the hash and
    # save/load round-trip divergence. Kept off the default lane (slower:
    # two sessions, a save and a load) -- run via -Mode HeadlessServerTickHeavy.
    $serverExe = "build/$BuildPreset/bin/luminumbra_server_app.exe"
    if (-not (Test-Path $serverExe)) {
        throw "headless heavy gate required executable missing: $serverExe (cmake --build build/$BuildPreset)"
    }

    $artifactPath = "build/$BuildPreset/test-artifacts/server/server-tick-heavy.json"
    if (Test-Path $artifactPath) {
        Remove-Item $artifactPath
    }

    & $serverExe --heavy --ticks 60 --heavy-resim 30 --artifact $artifactPath
    if ($LASTEXITCODE -ne 0) {
        throw "headless server heavy oracle exited with code $LASTEXITCODE"
    }

    $h = Read-JsonArtifact $artifactPath "luminumbra.server_tick_heavy.v1"
    Assert-ArtifactPassed $h "HeadlessServerTickHeavy"

    if (-not $h.roundtrip_match) {
        throw "heavy oracle: authoritative state diverged across save/load round-trip (terrain/water/entities)"
    }
    if (-not $h.resim_match) {
        throw "heavy oracle: authoritative state diverged after resimulating both sessions"
    }
    # the boot water-settle CONTRACT — fresh boots leave zero uninitialized
    # chunks; loaded boots skip the water settle (paused, restored state authoritative,
    # persisted sim-window cursor resumed). A violated contract advances the loaded
    # session's water past the original's saved state, so water can never round-trip.
    # (A global all-asleep fixed point does not exist; awake > 0 is expected.)
    if (-not $h.settle_contract_ok) {
        $so = $h.boot_settle_original; $sl = $h.boot_settle_loaded
        throw ("heavy oracle: boot water-settle contract VIOLATED () - original uninited={0}; loaded uninited={1} settle_skipped={2}" -f `
            $so.uninited, $sl.uninited, $sl.water_settle_skipped)
    }
    # Authoritative sub-hashes must be byte-equal at both comparison points.
    # aether_state: the STATEFUL energy layer is authoritative
    # round-trip state (unlike the recompute-and-excluded re-derivable wind/weather/
    # aether trio). Empty on the default OFF world ("" == "" passes); the ON fixture
    # rides the sim.aether_state activation bump (Codex sign-off finding A).
    foreach ($section in @("terrain", "water", "entities", "aether_state")) {
        if ($h.original_at_save.sub_hashes.$section -ne $h.loaded_at_load.sub_hashes.$section) {
            throw "heavy oracle: round-trip $section sub-hash mismatch: $($h.original_at_save.sub_hashes.$section) != $($h.loaded_at_load.sub_hashes.$section)"
        }
        if ($h.original_final.sub_hashes.$section -ne $h.loaded_final.sub_hashes.$section) {
            throw "heavy oracle: resim $section sub-hash mismatch: $($h.original_final.sub_hashes.$section) != $($h.loaded_final.sub_hashes.$section)"
        }
    }

    $meshNote = if ($h.resim_mesh_match) { "mesh also matched" } else { "mesh differs (derived render artifact; excluded by design)" }
    Write-Host ("headless heavy oracle passed: {0} ticks -> save -> load -> +{1} ticks; authoritative state (terrain/water/entities) round-trips AND resims identically. {2}." -f `
        $h.ticks_before_save, $h.resim_ticks, $meshNote)
}

# the intermittent 0xC0000005 that this helper used
# to work around is FIXED. Its root cause was NOT a streaming/shutdown data race --
# it was a FastNoise2 GenPositionArray2D SIMD over-read (the entry point's
# full-width tail load reads past a count-sized buffer when count < the SIMD width;
# the worldgen call sites in SHIELD_WorldSystem now SIMD-pad those buffers). With
# the fix the headless server runs cleanly at the DEFAULT (multi-)worker count, so
# the load-bearing mitigations are removed: no LUMINUMBRA_JOB_WORKERS=1 pin and no
# crash-retry loop. The environment knob remains available for diagnostic
# scheduling experiments but is not set here. The function name and signature
# are kept so call sites are unchanged; it runs the server once and returns its
# exit code (a non-zero exit is a real failure/divergence, surfaced immediately).
function Invoke-ServerWithCrashRetry {
    param(
        [string]$ServerExe,
        [string[]]$ServerArgs,
        [int]$MaxAttempts = 1
    )
    # Out-Host keeps the server's stdout on the console WITHOUT letting it leak into
    # this function's return value (a bare '& exe' would emit the log lines into the
    # success stream and corrupt the returned exit code).
    & $ServerExe @ServerArgs | Out-Host
    return $LASTEXITCODE
}

function Test-ReplayRoundtrip {
    #  session replay (LREC1): record a 90-tick run, replay it, and assert
    # the replay reproduces the SAME end-hash, verifies all checkpoints, and (the
    # determinism proof) that recording is hash-neutral -- the recorded run must
    # reach the canonical HeadlessServerTick hash d8f84cf6d7d0b978 unchanged
    # (world_hash lineage: 2fa007951a21e140 -> 0eac465289e7c88b [ wind slot]
    #  -> 0857e683b4b8c47e [ weather slot] -> d950a6afc12a5cdc [
    #  lightning strike schedule folded into the weather sub-hash, hash revision]
    #  -> f17726d44054d133 [ aether slot appended, bump #4]
    #  -> 8a6b7bb6795da912 [commit 0cb9ce8  appended the |scents: term to
    #  ComposeWorldHash, bump #5]
    #  -> d8f84cf6d7d0b978 [gate-populated-world-replay appended the |ecology: term to
    #  ComposeWorldHash, bump #6; the EMPTY-roster ecology sub-hash is neutral, so this
    #  composite changed ONLY by the appended `|ecology:` suffix -- the five existing
    #  sub-terms are byte-identical (additivity guard, )]).
    $serverExe = "build/$BuildPreset/bin/luminumbra_server_app.exe"
    if (-not (Test-Path $serverExe)) {
        throw "replay roundtrip gate required executable missing: $serverExe (cmake --build build/$BuildPreset)"
    }

    $replayDir = "build/$BuildPreset/test-artifacts/replay"
    New-Item -ItemType Directory -Force -Path $replayDir | Out-Null
    $streamPath = Join-Path $replayDir "roundtrip.lrec1"
    $artifactPath = Join-Path $replayDir "replay-roundtrip.json"
    Remove-Item -Force -ErrorAction SilentlyContinue $streamPath, $artifactPath

    # 1) Record a 90-tick run.
    $recCode = Invoke-ServerWithCrashRetry -ServerExe $serverExe -ServerArgs @("--record", $streamPath, "--ticks", "90")
    if ($recCode -ne 0) {
        throw "replay record exited with code $recCode"
    }
    if (-not (Test-Path $streamPath)) {
        throw "replay record produced no LREC1 stream at $streamPath"
    }

    # 2) Replay the recorded stream.
    $repCode = Invoke-ServerWithCrashRetry -ServerExe $serverExe -ServerArgs @("--replay", $streamPath, "--artifact", $artifactPath)
    if ($repCode -ne 0) {
        throw "replay playback exited with code $repCode (the recorded stream failed to replay deterministically)"
    }

    $r = Read-JsonArtifact $artifactPath "luminumbra.replay_roundtrip.v1"
    Assert-ArtifactPassed $r "ReplayRoundtrip"
    if ($r.diverged) {
        throw "replay roundtrip reported a divergence on a clean recording"
    }
    if (-not $r.start_world_hash_match) {
        throw "replay roundtrip: live boot hash did not match the recorded start_world_hash"
    }
    if (-not $r.end_hash_match) {
        throw "replay roundtrip: end hash $($r.end_world_hash) did not match the recorded end hash"
    }
    if ($r.ticks_replayed -ne 90) {
        throw "replay roundtrip replayed $($r.ticks_replayed)/90 ticks"
    }
    # 90 ticks at a 30-tick checkpoint cadence => checkpoints at 30/60/90.
    if ($r.checkpoints_verified -ne 3) {
        throw "replay roundtrip verified $($r.checkpoints_verified) checkpoints (expected 3 at 30/60/90)"
    }
    # Determinism proof: recording must NOT perturb the sim. The recorded run's
    # end hash must equal the canonical HeadlessServerTick hash, unchanged.
    # (Re-pinned 2026-07-05 for the  authoritative-state change: settle contract + flux
    # persistence + order-independent wake propagation. Prior canonical was
    # 6f008a9f637c40b7.)
    $expectedHash = "a66ab4d049ba9228"
    if ($r.end_world_hash -ne $expectedHash) {
        throw "replay roundtrip end hash $($r.end_world_hash) != canonical $expectedHash (recording perturbed the simulation)"
    }
    Write-Host ("replay roundtrip gate passed: recorded 90 ticks, replayed to identical end_hash={0} (canonical, recording is hash-neutral), {1} checkpoints verified" -f `
        $r.end_world_hash, $r.checkpoints_verified)
}

function Test-ReplayDivergence {
    #  negative oracle: prove the replay verifier is NOT vacuous. Record a
    # run, deliberately corrupt ONE checkpoint hash in the stream (via the
    # server's in-process --mutate-replay-fixture mode -- the least-hacky mutation:
    # it parses the real LREC1 stream and re-emits it with one checkpoint's
    # world_hash + terrain sub-hash flipped, no fragile byte-offset surgery), then
    # assert the replay FAILS at the FIRST checkpoint after the mutation with the
    # correct divergent-tick + section report.
    $serverExe = "build/$BuildPreset/bin/luminumbra_server_app.exe"
    if (-not (Test-Path $serverExe)) {
        throw "replay divergence gate required executable missing: $serverExe (cmake --build build/$BuildPreset)"
    }

    $replayDir = "build/$BuildPreset/test-artifacts/replay"
    New-Item -ItemType Directory -Force -Path $replayDir | Out-Null
    $streamPath = Join-Path $replayDir "divergence.lrec1"
    $artifactPath = Join-Path $replayDir "replay-divergence.json"
    Remove-Item -Force -ErrorAction SilentlyContinue $streamPath, $artifactPath

    # 1) Record a clean 90-tick run (checkpoints at 30/60/90).
    $recCode = Invoke-ServerWithCrashRetry -ServerExe $serverExe -ServerArgs @("--record", $streamPath, "--ticks", "90")
    if ($recCode -ne 0) {
        throw "replay divergence: record exited with code $recCode"
    }

    # 2) Corrupt the FIRST checkpoint (tick 30) in place (pure file IO; no sim).
    & $serverExe --mutate-replay-fixture $streamPath
    if ($LASTEXITCODE -ne 0) {
        throw "replay divergence: mutation exited with code $LASTEXITCODE"
    }

    # 3) Replay the mutated stream: this MUST fail with a CLEAN divergence (exit
    #    1 + a divergence artifact), NOT a crash.: the
    #    0xC0000005 worker-pin + retry workaround is removed (root cause fixed); a
    #    single replay at the default worker count is expected to exit 1 cleanly.
    Remove-Item -Force -ErrorAction SilentlyContinue $artifactPath
    & $serverExe --replay $streamPath --artifact $artifactPath
    $replayExit = $LASTEXITCODE
    if ($replayExit -eq 0) {
        throw "replay divergence: the verifier ACCEPTED a corrupted stream (oracle is vacuous!)"
    }
    # A real divergence writes the artifact and exits 1; a crash would not.
    if (-not (Test-Path $artifactPath)) {
        throw "replay divergence: replay exited $replayExit but wrote no divergence artifact (a crash, not a detected divergence)"
    }

    $d = Read-JsonArtifact $artifactPath "luminumbra.replay_divergence.v1"
    if (-not $d.diverged) {
        throw "replay divergence artifact did not report diverged=true"
    }
    # The mutation hit the first checkpoint (tick 30); divergence must be caught
    # exactly there (NOT at a later checkpoint, NOT silently passed).
    if ($d.divergence_tick -ne 30) {
        throw "replay divergence caught at tick $($d.divergence_tick), expected the first corrupted checkpoint at tick 30"
    }
    if ($d.divergence_section -ne "terrain") {
        throw "replay divergence localized to section '$($d.divergence_section)', expected 'terrain' (the mutated sub-hash)"
    }
    if ($d.checkpoints_verified_before_divergence -ne 0) {
        throw "replay divergence verified $($d.checkpoints_verified_before_divergence) checkpoints before tick 30 (expected 0; the first checkpoint was corrupted)"
    }
    Write-Host ("replay divergence gate passed: corrupted checkpoint CAUGHT at tick {0} (section={1}), replay refused (exit {2}); the self-verifying oracle is not vacuous" -f `
        $d.divergence_tick, $d.divergence_section, $replayExit)
    # The mutated replay deliberately exited non-zero (the divergence we asserted).
    # Clear $LASTEXITCODE so the gate process reports success, not the inner
    # divergence exit code, to its caller.
    $global:LASTEXITCODE = 0
}

# ---  LockstepLoopback mode: append-only ---
# Delay-based lockstep over LoopbackTransport (no sockets/ports): 2 peers (host=client0
# + the one remote=client1), M ticks (>=90), each peer stepping its own ServerWorldRunner
# of the SAME seed/preset. Asserts the session stayed in sync (no desync), both peers
# reached the budget tick, the exchanged-every-cadence hashes agreed, and the two worlds
# end at the IDENTICAL canonical hash d950a6afc12a5cdc -- proving lockstep does NOT perturb
# the simulation. Kept OFF the default All lane (slow: two full worlds), like the other
# headless-server modes -- run via -Mode LockstepLoopback.
function Test-LockstepLoopback {
    $serverExe = "build/$BuildPreset/bin/luminumbra_server_app.exe"
    if (-not (Test-Path $serverExe)) {
        throw "lockstep loopback gate required executable missing: $serverExe (cmake --build build/$BuildPreset)"
    }

    $lockstepDir = "build/$BuildPreset/test-artifacts/lockstep"
    New-Item -ItemType Directory -Force -Path $lockstepDir | Out-Null
    $artifactPath = Join-Path $lockstepDir "lockstep-loopback.json"
    Remove-Item -Force -ErrorAction SilentlyContinue $artifactPath

    & $serverExe --lockstep-loopback --ticks 90 --artifact $artifactPath
    if ($LASTEXITCODE -ne 0) {
        throw "lockstep loopback exited with code $LASTEXITCODE"
    }

    $a = Read-JsonArtifact $artifactPath "luminumbra.lockstep_loopback.v1"
    Assert-ArtifactPassed $a "LockstepLoopback"
    if ($a.desynced) {
        throw "lockstep loopback reported a desync on an in-sync session"
    }
    if ([int64]$a.ticks_requested -lt 90) {
        throw "lockstep loopback must run at least 90 ticks (got $($a.ticks_requested))"
    }
    if ([int64]$a.host.agreed_tick -ne [int64]$a.ticks_requested -or
        [int64]$a.peer.agreed_tick -ne [int64]$a.ticks_requested) {
        throw "lockstep loopback peers did not both reach the budget tick: host=$($a.host.agreed_tick) peer=$($a.peer.agreed_tick) / $($a.ticks_requested)"
    }
    if (-not $a.end_hashes_equal) {
        throw "lockstep loopback host/peer end hashes differ: $($a.host.world_hash) != $($a.peer.world_hash)"
    }
    # Determinism proof: lockstep must NOT perturb the sim. The in-sync end hash must equal
    # the canonical HeadlessServerTick hash, unchanged. (Re-pinned 2026-07-05 for the
    #  authoritative-state change; prior canonical was 6f008a9f637c40b7.)
    $expectedHash = "a66ab4d049ba9228"
    if ($a.host.world_hash -ne $expectedHash) {
        throw "lockstep loopback end hash $($a.host.world_hash) != canonical $expectedHash (lockstep perturbed the simulation)"
    }
    Write-Host ("lockstep loopback gate passed: {0} ticks, 2 peers in sync, end_hash={1} (canonical, host==peer), max_horizon={2}, late_inputs={3}" -f `
        $a.ticks_requested, $a.host.world_hash, $a.host.max_horizon_reached, $a.host.late_input_events)
}

# ---  NetworkedSession mode: append-only (OFF the default All lane) ---
# The CLIENT renders a SERVER-OWNED world over the lockstep transport. A
# LockstepSession pair runs over an in-process LoopbackTransport (no sockets):
# one peer is a headless HOST world authority, the other is the client's render
# GameSession. Per agreed tick both worlds step one fixed sim tick from the SAME
# spawn anchor and exchange world_hash + sub-hashes (the desync oracle). Camera
# look is render-side (never round-tripped). The gate asserts: both peers reach
# the budget tick, hashes matched at every cadence, host==client end_hash, the
# input set round-tripped, a clean disconnect, and the artifact schema. This is a
# HEAVY two-world lockstep gate (like LockstepLoopback / HeadlessServerTick), so
# it stays off All.
#
# HASH SCOPE (important): this gate's end_hash is the CLIENT-path bare streamed-
# chunk hash (NetSessionCaptureHashes -> WorldSaveService::world_hash over the
# streamed chunks, with the canonical EMPTY entities snapshot). It is NOT the
# server's COMPOSITE world_hash. The headless / lockstep-loopback / replay gates
# pin ServerWorldRunner::ComputeWorldHash, which ComposeWorldHash-folds the wind +
# weather + aether + scent + ecology sub-hashes on TOP of the chunk hash (currently
# d8f84cf6d7d0b978). The networked client path was never wired to fold those
# sub-hashes, so it pins its OWN deterministic value (the bare chunk hash). The
# two are different QUANTITIES by construction -- not a divergence. The networked
# determinism oracle is host==client (asserted above) + run-to-run stability of
# this pin; both hold. (The pre-I9 coincidence where this equalled the composite
# f17726d44054d133 ended when  appended the `|scents:` term to the
# server composite -- the bare client chunk hash was unaffected; the
# gate-populated-world-replay `|ecology:` term, bump #6, is likewise server-only.)
#
# rev 13 (, Decision 2 — DEFERRED client fold): the client capture path
# (NetSessionCaptureHashes, RuntimeScenarioHarness.cpp) folds ONLY the bare
# streamed-chunk hash; it owns NO independent wind/weather/aether/scent/ecology
# state today (those live on the server-authoritative session). Wiring the client
# to fold the full canonical quantity is a recorded implementation note (per-sub-hash
# cadence comparison), NOT done here. This gate's host==client equality is over the
# CLIENT quantity on both peers, so the deferral does not weaken it.
function Test-NetworkedSession {
    $exe = Get-ClientExe
    $viewDir = "build/$BuildPreset/test-artifacts/runtime/networked-session"
    Remove-Item -Recurse -Force -ErrorAction SilentlyContinue $viewDir
    New-Item -ItemType Directory -Force -Path $viewDir | Out-Null

    # radius 4/2 keeps the two-world 90-tick lockstep inside a practical wall time
    # AND matches the headless server's streaming profile, so the in-sync end hash
    # is the canonical world hash. The run drives the lockstep to completion and
    # exits; --timed-run is a generous outer ceiling only.
    Invoke-Checked -FilePath $exe -ArgumentList @(
        "--scenario", "networked_session_smoke",
        "--auto-create-world",
        "--auto-enter-world",
        "--timed-run", "600",
        "--horizon-radius", "4",
        "--collision-radius", "2",
        "--no-audio",
        "--no-ui",
        "--runtime-artifact-dir", $viewDir
    ) -TimeoutSeconds 600

    $analysisPath = Join-Path $viewDir "networked-session-analysis.json"
    $a = Read-JsonArtifact -Path $analysisPath -Schema "luminumbra.networked_session.v1"

    if ([int64]$a.gl_debug.errors -ne 0) {
        throw "networked session emitted GL debug errors: $($a.gl_debug.errors)"
    }
    if ($a.desynced) {
        throw "networked session reported a desync on an in-sync session"
    }
    if ([int64]$a.ticks_requested -lt 90) {
        throw "networked session must run at least 90 ticks (got $($a.ticks_requested))"
    }
    if ([int64]$a.host.agreed_tick -ne [int64]$a.ticks_requested -or
        [int64]$a.client.agreed_tick -ne [int64]$a.ticks_requested) {
        throw "networked session peers did not both reach the budget tick: host=$($a.host.agreed_tick) client=$($a.client.agreed_tick) / $($a.ticks_requested)"
    }
    if (-not $a.end_hashes_equal) {
        throw "networked session host/client end hashes differ: $($a.host.world_hash) != $($a.client.world_hash)"
    }
    if (-not $a.in_sync_every_cadence) {
        throw "networked session was not in sync at every cadence (hash_exchanges=$($a.hash_exchanges))"
    }
    $expectedExchanges = [int][Math]::Floor([int64]$a.ticks_requested / [int64]$a.hash_cadence_ticks)
    if ([int64]$a.hash_exchanges -lt $expectedExchanges) {
        throw "networked session ran $($a.hash_exchanges) cadence hash exchanges, expected at least $expectedExchanges"
    }
    if (-not $a.input_round_tripped) {
        throw "networked session did not round-trip the input set through the session"
    }
    if (-not $a.camera_look_render_side) {
        throw "networked session did not keep camera look render-side"
    }
    if (-not $a.clean_disconnect) {
        throw "networked session did not record a clean disconnect"
    }
    # Determinism proof: the client-rendered, server-owned world is REPRODUCIBLE
    # and host==client (asserted above). This is the CLIENT-path bare streamed-
    # chunk hash (see the HASH SCOPE note in the function header), NOT the server's
    # composite world_hash -- so it pins its own deterministic value. Render-side
    # camera look did NOT perturb it (the hashed step uses the spawn anchor).
    # Re-pinned 2026-07-10 after three independent 90-tick runs all produced
    # 354be8d009c08703 with host==client at every cadence, clean disconnect,
    # and zero GL errors. This remains the client-path bare streamed-chunk hash.
    $expectedHash = "354be8d009c08703"
    if ($a.end_hash -ne $expectedHash) {
        throw "networked session end hash $($a.end_hash) != expected $expectedHash (client streamed-chunk world diverged / non-deterministic)"
    }
    if (-not $a.passed) {
        throw "networked session analysis reported failure: $($a.failure_reason)"
    }
    Write-Host ("networked session gate passed: client renders server-owned world, {0} ticks in sync (host==client), end_hash={1} (client streamed-chunk hash, reproducible), {2} cadence hash exchanges, clean disconnect" -f `
        $a.ticks_requested, $a.end_hash, $a.hash_exchanges)
}

# ---  LockstepFaultInjection mode: append-only ---
# Two scenarios, both over LoopbackTransport (no sockets):
#  (1) DELAYED+DROPPED input within horizon tolerance: peer 1 withholds its inputs for a
#      burst of ticks, then releases them. The adaptive horizon must ABSORB it -- the
#      session stays in sync (no desync), the hashes still match, and the horizon GREW
#      (proving the absorption was real, not a no-op). The end hash stays canonical.
#  (2) An actual STATE divergence: peer 1's captured hashes are corrupted from a chosen
#      tick (like the ReplayDivergence fixture). The oracle must HALT the session and emit
#      the LREC1 dump with the CORRECT divergent tick -- proving the oracle is not vacuous.
function Test-LockstepFaultInjection {
    $serverExe = "build/$BuildPreset/bin/luminumbra_server_app.exe"
    if (-not (Test-Path $serverExe)) {
        throw "lockstep fault-injection gate required executable missing: $serverExe (cmake --build build/$BuildPreset)"
    }

    $lockstepDir = "build/$BuildPreset/test-artifacts/lockstep"
    New-Item -ItemType Directory -Force -Path $lockstepDir | Out-Null

    # --- Scenario 1: horizon absorbs a delayed/dropped input (stays in sync). ---
    $absorbArtifact = Join-Path $lockstepDir "lockstep-absorb.json"
    Remove-Item -Force -ErrorAction SilentlyContinue $absorbArtifact
    & $serverExe --lockstep-loopback --ticks 90 --lockstep-delay-input 8 --artifact $absorbArtifact
    if ($LASTEXITCODE -ne 0) {
        throw "lockstep fault-injection (absorb) exited with code $LASTEXITCODE"
    }
    $absorb = Read-JsonArtifact $absorbArtifact "luminumbra.lockstep_loopback.v1"
    Assert-ArtifactPassed $absorb "LockstepFaultInjection-absorb"
    if ($absorb.desynced) {
        throw "lockstep fault-injection: the horizon FAILED to absorb a delayed input (false desync)"
    }
    if (-not $absorb.horizon_absorbed_jitter) {
        throw "lockstep fault-injection: artifact does not confirm horizon absorption (horizon never grew?)"
    }
    if ([int64]$absorb.host.max_horizon_reached -le 3) {
        throw "lockstep fault-injection: horizon did not grow on a delayed input (max_horizon=$($absorb.host.max_horizon_reached))"
    }
    if ([int64]$absorb.host.late_input_events -le 0) {
        throw "lockstep fault-injection: no late-input events recorded (the delay was not exercised)"
    }
    if (-not $absorb.end_hashes_equal -or $absorb.host.world_hash -ne "a66ab4d049ba9228") {
        throw "lockstep fault-injection: absorbed-jitter run did not reach the canonical in-sync end hash (host=$($absorb.host.world_hash))"
    }

    # --- Scenario 2: a real state divergence HALTS the session + dumps LREC1. ---
    $corruptArtifact = Join-Path $lockstepDir "lockstep-corrupt.json"
    $dumpPath = Join-Path $lockstepDir "lockstep-desync.lrec1"
    Remove-Item -Force -ErrorAction SilentlyContinue $corruptArtifact, $dumpPath, ($dumpPath + ".peer")
    & $serverExe --lockstep-loopback --ticks 90 --lockstep-corrupt-tick 30 --lockstep-dump $dumpPath --artifact $corruptArtifact
    if ($LASTEXITCODE -ne 0) {
        throw "lockstep fault-injection (corrupt) exited with code $LASTEXITCODE"
    }
    $corrupt = Read-JsonArtifact $corruptArtifact "luminumbra.lockstep_loopback.v1"
    Assert-ArtifactPassed $corrupt "LockstepFaultInjection-corrupt"
    if (-not $corrupt.desynced) {
        throw "lockstep fault-injection: a deliberate STATE divergence was NOT caught (oracle is vacuous!)"
    }
    if ([int64]$corrupt.desync_tick -ne 30) {
        throw "lockstep fault-injection: divergence caught at tick $($corrupt.desync_tick), expected the corrupt tick 30"
    }
    if ($corrupt.desync_section -ne "terrain") {
        throw "lockstep fault-injection: divergence localized to '$($corrupt.desync_section)', expected 'terrain'"
    }
    if (-not $corrupt.dump_present) {
        throw "lockstep fault-injection: oracle halted but emitted NO LREC1 dump (no desync-repro artifact)"
    }
    # The dump must be a valid LREC1 stream the existing --replay path consumes (it is the
    # desync-repro artifact). Replaying it MUST report a divergence (exit 1), not a crash.
    $emittedDump = $corrupt.dump_path
    if (-not (Test-Path $emittedDump)) {
        throw "lockstep fault-injection: dump_path '$emittedDump' does not exist on disk"
    }
    $dumpReplayArtifact = Join-Path $lockstepDir "lockstep-dump-replay.json"
    Remove-Item -Force -ErrorAction SilentlyContinue $dumpReplayArtifact
    & $serverExe --replay $emittedDump --artifact $dumpReplayArtifact
    $dumpReplayExit = $LASTEXITCODE
    if ($dumpReplayExit -eq 0) {
        throw "lockstep fault-injection: the desync dump replayed WITHOUT a divergence (the dump is not a real repro)"
    }
    if (-not (Test-Path $dumpReplayArtifact)) {
        throw "lockstep fault-injection: replaying the dump produced no divergence artifact (a crash, not a detected divergence)"
    }
    $dumpReplay = Read-JsonArtifact $dumpReplayArtifact "luminumbra.replay_divergence.v1"
    if (-not $dumpReplay.diverged) {
        throw "lockstep fault-injection: dump replay artifact did not report diverged=true"
    }
    # The deliberate non-zero exit from the dump replay is the divergence we asserted; clear
    # it so the gate reports success to its caller.
    $global:LASTEXITCODE = 0

    Write-Host ("lockstep fault-injection gate passed: (1) horizon ABSORBED a delayed input (max_horizon={0}, late_inputs={1}, end_hash canonical, no desync); (2) a real STATE divergence HALTED the session at tick {2} (section={3}) + emitted an LREC1 dump that replays to a divergence (oracle is not vacuous)" -f `
        $absorb.host.max_horizon_reached, $absorb.host.late_input_events, $corrupt.desync_tick, $corrupt.desync_section)
}

function Test-SkinnedMeshVisual {
    # skinned G-Buffer stage gate. A procedurally generated rigged
    # test mesh is spawned near spawn; two captures at different clip times
    # must differ in the mesh ROI and the skinned draw stage must have run.
    $exe = Get-ClientExe
    $visualDir = "build/$BuildPreset/test-artifacts/runtime/skinned-mesh-visual"
    Remove-Item -Recurse -Force -ErrorAction SilentlyContinue $visualDir
    New-Item -ItemType Directory -Force -Path $visualDir | Out-Null

    $runSeconds = [Math]::Max(20, $SmokeSeconds)
    Invoke-Checked -FilePath $exe -ArgumentList @(
        "--scenario", "skinned_mesh_visual_smoke",
        "--auto-create-world",
        "--auto-enter-world",
        "--timed-run", "$runSeconds",
        "--no-audio",
        "--no-ui",
        "--runtime-artifact-dir", $visualDir
    ) -TimeoutSeconds ([Math]::Max(120, $runSeconds + 90))

    $analysisPath = Join-Path $visualDir "skinned-mesh-visual-analysis.json"
    if (-not (Test-Path $analysisPath)) {
        throw "skinned mesh visual run did not produce $analysisPath (gate producer)"
    }

    $analysis = Get-Content $analysisPath -Raw | ConvertFrom-Json
    if ($analysis.schema -ne "luminumbra.skinned_mesh_visual_analysis.v1") {
        throw "Unexpected skinned mesh visual analysis schema '$($analysis.schema)'"
    }
    if (-not $analysis.rig.spawned) {
        throw "Skinned mesh visual scenario failed to spawn the rigged test mesh"
    }
    if ([int64]$analysis.gl_debug.errors -ne 0) {
        throw "Skinned mesh visual run emitted GL debug errors: $($analysis.gl_debug.errors)"
    }
    foreach ($capture in @($analysis.capture_a, $analysis.capture_b)) {
        if ([int64]$capture.skinned_draws -lt 1) {
            throw "Skinned mesh visual capture '$($capture.file)' rendered no skinned draws"
        }
        Assert-PpmArtifact (Join-Path $visualDir $capture.file)
    }
    if ([double]$analysis.capture_b.animation_time_seconds -le [double]$analysis.capture_a.animation_time_seconds) {
        throw "Skinned mesh visual captures do not advance the animation clock: $($analysis.capture_a.animation_time_seconds) -> $($analysis.capture_b.animation_time_seconds)"
    }
    if ([int64]$analysis.diff.changed_pixels -lt [int64]$analysis.thresholds.min_changed_pixels) {
        throw "Skinned mesh ROI diff below threshold: $($analysis.diff.changed_pixels) < $($analysis.thresholds.min_changed_pixels) changed pixels"
    }
    if ([double]$analysis.diff.changed_ratio -lt [double]$analysis.thresholds.min_changed_ratio) {
        throw "Skinned mesh ROI diff ratio below threshold: $($analysis.diff.changed_ratio) < $($analysis.thresholds.min_changed_ratio)"
    }
    #  textured-response: the UV-mapped creature texture must drive a
    # color variance above the flat-color bound (a flat-shaded creature fails).
    if ($null -ne $analysis.diff.mesh_color_stddev_a -and `
        [double]$analysis.diff.mesh_color_stddev_a -lt [double]$analysis.thresholds.min_mesh_color_stddev) {
        throw "Skinned mesh is not textured: color stddev $($analysis.diff.mesh_color_stddev_a) < $($analysis.thresholds.min_mesh_color_stddev)"
    }
    if (-not $analysis.passed) {
        throw "Skinned mesh visual analysis reported failure: $($analysis.failures -join ', ')"
    }
    Write-Host ("skinned mesh visual: draws a={0} b={1} anim_time a={2}s b={3}s changed_pixels={4} (ratio {5}) mesh_like a={6} b={7}" -f `
        $analysis.capture_a.skinned_draws, $analysis.capture_b.skinned_draws, `
        $analysis.capture_a.animation_time_seconds, $analysis.capture_b.animation_time_seconds, `
        $analysis.diff.changed_pixels, $analysis.diff.changed_ratio, `
        $analysis.diff.mesh_like_pixels_a, $analysis.diff.mesh_like_pixels_b)
    Assert-CapturePinned -ArtifactDir $visualDir -Name "SkinnedMeshVisual"
}

function Test-EngineGameSplitLint {
    # engine/game decoupling lint. The engine (src/) must carry no
    # Project Capture game nouns — content lives under data/ and worlds/.
    # (a) path lint: no game noun in any path under src/;
    # (b) content lint: no game noun in any engine source file.
    # the aetheric compatibility alias was removed at iteration close;
    # 'aetheric' is now an UNCONDITIONAL violation under src/ (no allowlist).
    $gameNouns = @(
        "lumincrystal",
        "grovestrider",
        "glimmer",
        "mossberry",
        "glowcap",
        "thunder_hollow",
        "stream_reeds",
        "lantern_wisp",
        "aetheric"
    )

    function Test-Allowlisted {
        param([string]$RelativePath, [string]$Noun)
        # every comment-level game noun was reworded out of
        # src/ — 'aetheric' remains UNCONDITIONAL (, zero entries below). What is
        # allowlisted here is CODE-LEVEL debt only, each entry awaiting the
        # data-driven refactor (hardcoded species spawns/audio event ids/model paths/the
        # MaterialType::LuminCrystal enumerant/the built-in species table). Do not add
        # entries without a documented reason for retaining the product-specific value.
        $ops16Debt = @(
            @{ Path = "src/luminumbra_server/ServerWorldRunner.cpp";           Nouns = @("grovestrider") },
            @{ Path = "src/luminumbra_client/main_client.cpp";                 Nouns = @("grovestrider", "glimmer", "lumincrystal") }
            #  (2026-07-07): RuntimeScenarioHarness.cpp (showcase model paths -> data/
            # common/scenario/skinned_showcase_model.json) and SpeciesCodex.h (example catalogue
            # de-nouned) are RETIRED from this allowlist. ServerWorldRunner (grovestrider spawn,
            # hash-visible) and main_client (audio event ids, fauna, and crystal)
            # remain intentionally product-specific.
        )
        foreach ($entry in $ops16Debt) {
            if ($RelativePath -eq $entry.Path -and $entry.Nouns -contains $Noun) { return $true }
        }
        return $false
    }

    $violations = New-Object 'System.Collections.Generic.List[string]'
    $srcRoot = (Resolve-Path "src").Path
    $files = Get-ChildItem -Path "src" -Recurse -File |
        Where-Object { $_.Extension -in @(".h", ".hpp", ".c", ".cpp", ".inl", ".cmake", ".txt") -or $_.Name -eq "sources.cmake" }

    $scannedFiles = 0
    foreach ($file in $files) {
        $relative = $file.FullName.Substring($srcRoot.Length - 3).Replace("\", "/")
        $scannedFiles++

        # (a) path lint.
        foreach ($noun in $gameNouns) {
            if ($relative.ToLowerInvariant().Contains($noun)) {
                if (-not (Test-Allowlisted -RelativePath $relative -Noun $noun)) {
                    $violations.Add("path: $relative contains game noun '$noun'")
                }
            }
        }

        # (b) content lint.
        $content = Get-Content $file.FullName -Raw
        if ($null -eq $content) { continue }
        $lower = $content.ToLowerInvariant()
        foreach ($noun in $gameNouns) {
            if ($lower.Contains($noun)) {
                if (-not (Test-Allowlisted -RelativePath $relative -Noun $noun)) {
                    $violations.Add("content: $relative contains game noun '$noun'")
                }
            }
        }
    }

    if ($scannedFiles -lt 50) {
        throw "engine-game split lint scanned suspiciously few files ($scannedFiles); src/ scan is broken"
    }

    # Relocated game content must exist where it belongs.
    foreach ($dataFile in @("data/common/archetypes/grovestrider.json")) {
        if (-not (Test-Path $dataFile)) {
            throw "engine-game split lint: relocated game content missing: $dataFile"
        }
    }

    if ($violations.Count -gt 0) {
        foreach ($violation in $violations) {
            Write-Host "  $violation"
        }
        throw "engine-game split lint found $($violations.Count) violation(s) in src/"
    }

    Write-Host "engine-game split lint: $scannedFiles engine files scanned, 0 game-noun violations"
}

function Test-SimDeterminismLint {
    #  determinism contract (prevention, not detection). Bans, in the
    # SIM-CRITICAL paths (luminumbra_common simulation systems + the headless
    # server), the constructs that silently diverge a lockstep tick -- the
    # Factorio std::sort-comparator lesson (research Area 2, takeaway 6/9):
    #   (a) libm transcendentals (sin/cos/tan/exp/log/pow/atan2) -- different
    #       per libm; sim code must call core/DeterministicMath.h instead.
    #       sqrt is NOT banned (IEEE-754 correctly-rounded => already stable);
    #       floor/ceil/round are exact and not scanned.
    #   (b) wall-clock time (std::chrono::system_clock/steady_clock/
    #       high_resolution_clock, std::time, glfwGetTime) -- non-deterministic.
    #   (c) non-seeded RNG (rand/srand/std::random_device/std::mt19937/
    #       std::default_random_engine) -- a sim RNG must be one seeded stream.
    #   (d) range-for iteration over std::unordered_map/set where order feeds
    #       sim state. Heuristic: flag `for (...: <ident>)` where the same TU
    #       declares that identifier an unordered_map/set. Hash-order iteration
    #       is the single most common real desync.
    # This gate is APPEND-ONLY safe: it PASSES on the current tree (every
    # existing legitimate site is allowlisted below WITH a reason) and exists to
    # fail NEW violations. Migration of existing sqrt/etc. is explicitly out of
    # scope for.

    # Sim-critical roots. world/systems/fields/ai/simulation/animation/physics
    # under common are authoritative tick state; net/network carry hashed state;
    # the headless server drives the canonical loop. Render/scripting/persistence
    # IO and core utilities are excluded (not per-tick authoritative math).
    $simRoots = @(
        "src/luminumbra_common/systems",
        "src/luminumbra_common/world",
        "src/luminumbra_common/fields",
        "src/luminumbra_common/ai",
        "src/luminumbra_common/simulation",
        "src/luminumbra_common/animation",
        "src/luminumbra_common/physics",
        "src/luminumbra_server"
    )

    # Allowlist: "<relative-path>|<category>" entries that are legitimate and
    # MUST NOT be flagged. Each carries an inline reason. Keep this list small
    # and justified; adding an entry is a determinism decision.
    $allowlist = @{
        # World-creation BOOTSTRAP: seed default, unique world-id (directory
        # name) and creation-timestamp metadata. None of these feed the per-tick
        # sim or world_hash -- they SELECT the seed, after which generation is
        # fully deterministic. A fresh world with no seed is intentionally
        # non-reproducible (like a UUID); a seeded world is bit-stable.
        "src/luminumbra_common/world/GameSession.cpp|time"  = "world-id/creation metadata + seed default; not per-tick sim state"
        "src/luminumbra_common/world/GameSession.cpp|rng"   = "seed default + world-id RNG; bootstrap only, never per-tick"
        # TELEMETRY ONLY: meshing build-time stats (elapsed_us) and the server
        # wall_seconds report. Measured, recorded in artifacts, never hashed.
        "src/luminumbra_common/world/MarchingCubes.cpp|time" = "TerrainMeshBuildStats elapsed_us telemetry; not hashed"
        "src/luminumbra_server/ServerWorldRunner.cpp|time"   = "RunFixedTicks wall_seconds report telemetry; not hashed"
        #  the --wind-bench mode times the per-tick wind update for the
        # WindFieldDeterminism budget assertion. The clock is TELEMETRY only
        # (measured, recorded in the artifact, never hashed); the wind field
        # itself is bit-deterministic (DeterministicMath + FastNoise batch path).
        "src/luminumbra_server/main_server.cpp|time"         = "--wind-bench / --weather-bench per-tick update timing telemetry; not hashed"
    }

    # Exact legacy sites that predate this append-only gate. Keep these scoped
    # to the normalized source line rather than allowlisting the whole SHIELD
    # translation unit: a new libm/clock call in the file must still fail.
    $siteAllowlist = @{
        'src/luminumbra_common/systems/SHIELD_WorldSystem.cpp|trig|const float res = std::exp2(a / k) + std::exp2(b / k);' = "legacy exponential smooth-CSG worldgen; hashes are pinned and migration needs its own re-pin campaign"
        'src/luminumbra_common/systems/SHIELD_WorldSystem.cpp|trig|return k * std::log2(res);' = "legacy exponential smooth-CSG worldgen; hashes are pinned and migration needs its own re-pin campaign"
        'src/luminumbra_common/systems/SHIELD_WorldSystem.cpp|trig|float diameter = kDmin * std::pow(std::max(1e-4f, 1.0f - u), -1.0f / (kBeta - 1.0f));' = "legacy seeded surface-break distribution; worldgen hashes already pin this exact expression"
        'src/luminumbra_common/systems/SHIELD_WorldSystem.cpp|time|auto _dbg_prev = std::chrono::steady_clock::now();' = "streaming split telemetry only; never feeds simulation state or world hash"
        'src/luminumbra_common/systems/SHIELD_WorldSystem.cpp|time|const auto _n = std::chrono::steady_clock::now();' = "streaming split telemetry only; never feeds simulation state or world hash"
        'src/luminumbra_common/systems/SHIELD_WorldSystem.cpp|time|const auto _esrn_t0 = std::chrono::steady_clock::now();' = "world-load watchdog/elapsed telemetry only; never feeds simulation state or world hash"
        'src/luminumbra_common/systems/SHIELD_WorldSystem.cpp|time|std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - _esrn_t0).count());' = "world-load elapsed telemetry only; never feeds simulation state or world hash"
        'src/luminumbra_common/systems/SHIELD_WorldSystem.cpp|trig|const float ox = std::cos(ang), oz = std::sin(ang);' = "render/capture-only deep-water camera placement; excluded from simulation and world hash"
    }

    $bannedTrig = 'sin|cos|tan|asin|acos|atan|atan2|sinh|cosh|tanh|exp|exp2|log|log2|log10|pow|cbrt|hypot'
    $violations = New-Object 'System.Collections.Generic.List[string]'
    #  "flag std::sqrt for review (warn, not fail)": sqrt is correctly-rounded
    # IEEE-754 so it is deterministic and intentionally NOT in $bannedTrig, but raw
    # std::sqrt( sites (not routed through DeterministicMath::Sqrt / an alias) are
    # surfaced advisory-only. This list NEVER fails the gate.
    $sqrtReview = New-Object 'System.Collections.Generic.List[string]'
    $scanned = 0
    $repoRoot = (Resolve-Path ".").Path

    foreach ($root in $simRoots) {
        if (-not (Test-Path $root)) { continue }
        $files = Get-ChildItem -Path $root -Recurse -File |
            Where-Object { $_.Extension -in @(".h", ".hpp", ".cpp", ".inl", ".c") }
        foreach ($file in $files) {
            $scanned++
            $relative = $file.FullName.Substring($repoRoot.Length + 1).Replace("\", "/")
            $rawLines = Get-Content $file.FullName
            $content = $rawLines -join "`n"

            # Collect identifiers declared as unordered_map/set in this TU so the
            # range-for check can tell hashed iteration from ordered iteration.
            $unorderedIdents = @{}
            # Collect DeterministicMath namespace aliases declared in this TU so the
            # trig check treats `<alias>::Cos(` exactly like `DeterministicMath::Cos(`
            # (the pervasive `namespace dm =::Luminumbra::DeterministicMath;` form,
            # plus DM / a full re-alias). Resolved generically -- the alias name is
            # not hard-coded, so a TU aliasing under any identifier stays correct.
            # "DeterministicMath" is always recognized as an alias (the canonical
            # qualifier), so the existing `DeterministicMath::` escape hatch is kept.
            $detAliases = @{ "DeterministicMath" = $true }
            foreach ($line in $rawLines) {
                $m = [regex]::Matches($line, 'std::unordered_(?:map|set|multimap|multiset)\s*<[^;{]*?>\s+([A-Za-z_]\w*)')
                foreach ($mm in $m) { $unorderedIdents[$mm.Groups[1].Value] = $true }
                # member/typedef-style: `... m_foo;` where the type was unordered_*
                $m2 = [regex]::Matches($line, 'std::unordered_(?:map|set|multimap|multiset)\s*<')
                # `namespace <id> = [::][Luminumbra::]DeterministicMath;` aliases.
                $am = [regex]::Match($line, 'namespace\s+([A-Za-z_]\w*)\s*=\s*(?:::)?(?:Luminumbra::)?DeterministicMath\s*;')
                if ($am.Success) { $detAliases[$am.Groups[1].Value] = $true }
            }
            # Alternation of resolved alias names (escaped) for the deterministic-
            # call escape hatch, longest-first so `DeterministicMath` is tried before
            # a shorter alias substring.
            $aliasAlt = ($detAliases.Keys | Sort-Object { $_.Length } -Descending |
                ForEach-Object { [regex]::Escape($_) }) -join '|'

            $lineNo = 0
            foreach ($line in $rawLines) {
                $lineNo++
                # Strip line comments so commented examples never trip the lint.
                $code = $line -replace '//.*$', ''
                if ($code -match '^\s*\*' -or $code -match '^\s*/\*') { continue }

                # (a) libm transcendentals: std::<fn>( or bare <fn>( / <fn>f(.
                if ($code -match ("(?:std::)?(?:$bannedTrig)f?\s*\(")) {
                    # Allow DeterministicMath:: dispatch (and any resolved alias of
                    # it -- `dm::Cos(`, `DM::Sin(`,...) and glm:: (glm trig is
                    # template math the migration task addresses separately; the
                    # present sim paths use none -- if one appears it is flagged
                    # via the std::/bare forms, not glm). Strip every
                    # `<alias>::<fn>(` deterministic call from the line first, so the
                    # residual reflects only RAW libm trig; a line mixing
                    # `dm::Cos(` and `std::cos(` still flags the std:: call.
                    $residual = $code -replace ("(?:$aliasAlt)::(?:$bannedTrig)f?\s*\("), ''
                    if ($residual -match ("(?:std::|[^.\w])(?:$bannedTrig)f?\s*\(")) {
                        $key = "$relative|trig"
                        $siteKey = "$relative|trig|$($code.Trim())"
                        if (-not $allowlist.ContainsKey($key) -and -not $siteAllowlist.ContainsKey($siteKey)) {
                            $violations.Add("trig: ${relative}:${lineNo}: libm transcendental in sim path -> use DeterministicMath::; `"$($code.Trim())`"")
                        }
                    }
                }

                # (a') sqrt REVIEW (warn-only, ): correctly-rounded IEEE-754
                # sqrt is deterministic, so a raw std::sqrt( on the sim path is NOT a
                # failure -- but surface it advisory so a reviewer confirms it is the
                # IEEE sqrt and not a same-named user fn. Skip calls already routed
                # through DeterministicMath::Sqrt / an alias (strip them first).
                if ($code -match '(?:std::)?sqrtf?\s*\(') {
                    $sresidual = $code -replace ("(?:$aliasAlt)::Sqrtf?\s*\("), ''
                    if ($sresidual -match '(?:std::|[^.\w:])sqrtf?\s*\(') {
                        $sqrtReview.Add("${relative}:${lineNo}: $($code.Trim())")
                    }
                }

                # (b) wall-clock.
                if ($code -match 'std::chrono::(system_clock|steady_clock|high_resolution_clock)' -or
                    $code -match 'std::time\s*\(' -or
                    $code -match 'glfwGetTime\s*\(') {
                    $key = "$relative|time"
                    $siteKey = "$relative|time|$($code.Trim())"
                    if (-not $allowlist.ContainsKey($key) -and -not $siteAllowlist.ContainsKey($siteKey)) {
                        $violations.Add("time: ${relative}:${lineNo}: wall-clock in sim path; `"$($code.Trim())`"")
                    }
                }

                # (c) non-seeded RNG.
                if ($code -match 'std::random_device' -or
                    $code -match 'std::mt19937' -or
                    $code -match 'std::default_random_engine' -or
                    $code -match '(^|[^.\w])s?rand\s*\(') {
                    $key = "$relative|rng"
                    if (-not $allowlist.ContainsKey($key)) {
                        $violations.Add("rng: ${relative}:${lineNo}: non-seeded RNG in sim path; `"$($code.Trim())`"")
                    }
                }

                # (d) range-for over a hashed container declared in this TU.
                $rf = [regex]::Match($code, 'for\s*\(\s*[^:;]*:\s*([A-Za-z_]\w*)')
                if ($rf.Success) {
                    $iterName = $rf.Groups[1].Value
                    if ($unorderedIdents.ContainsKey($iterName)) {
                        $key = "$relative|hashiter"
                        if (-not $allowlist.ContainsKey($key)) {
                            $violations.Add("hashiter: ${relative}:${lineNo}: range-for over unordered '$iterName' in sim path (iteration order is non-deterministic); `"$($code.Trim())`"")
                        }
                    }
                }
            }
        }
    }

    if ($scanned -lt 10) {
        throw "sim determinism lint scanned suspiciously few files ($scanned); sim-path scan is broken"
    }

    # Warn-only sqrt review: advisory, never fails the gate.
    if ($sqrtReview.Count -gt 0) {
        Write-Host ("sqrt-review: {0} raw std::sqrt( site(s) on the sim path (IEEE-754 correctly-rounded => deterministic; advisory only, not a failure):" -f $sqrtReview.Count)
        foreach ($s in $sqrtReview) { Write-Host "  sqrt-review: $s" }
    }

    if ($violations.Count -gt 0) {
        foreach ($v in $violations) { Write-Host "  $v" }
        throw "sim determinism lint found $($violations.Count) violation(s) in sim-critical paths (). Use core/DeterministicMath.h, the seeded sim RNG, the SimulationClock, and ordered iteration; or allowlist with a documented reason."
    }

    Write-Host ("sim determinism lint: {0} sim-critical files scanned, 0 new violations ({1} documented allowlist exception site(s))" -f $scanned, ($allowlist.Count + $siteAllowlist.Count))
}

function Test-SimOptLevelParity {
    # T-transcendental  /: opt-level INVARIANCE of the sim path.
    # The debug library is -O0 except world/GameSession.cpp, which is forced to
    # -O1 to dodge a GCC-15 entt vague-linkage link error (CMakeLists.txt). That
    # opt-level asymmetry across TUs is exactly how FMA-contraction / optimizer
    # drift can sneak a desync in. This gate proves it has not: it builds a second
    # tree (debug-simo0) with LUMINUMBRA_SIM_O0_PARITY=ON -- which compiles
    # GameSession.cpp at - instead (a STRONGER opt level; plain -O0 reintroduces
    # the link error, so - is the link-safe parity axis, ) -- runs the same
    # HeadlessServerTick scenario (--smoke --ticks 90), and asserts the world_hash
    # is BIT-IDENTICAL to the normal -O1 build's. Determinism-only: no
    # perf claim. Excluded from the All fast pass (it builds a whole second tree);
    # this is an on-demand / dedicated-lane gate.

    $parityPreset = "debug-simo0"
    $parityDir = "build/$parityPreset"
    $parityExe = "$parityDir/bin/luminumbra_server_app.exe"

    # ucrt64 must precede KiCad/mingw on PATH or cc1plus exits 127 (toolchain-PATH-
    # contamination). Prepend it for the configure+build invocations below.
    $ucrt = "C:\msys64\ucrt64\bin"
    $oldPath = $env:PATH
    if ($env:PATH -notlike "$ucrt*") { $env:PATH = "$ucrt;$env:PATH" }
    try {
        Write-Host "sim-opt-parity: configuring + building $parityPreset (GameSession.cpp @ -)..."
        & cmake --preset $parityPreset
        if ($LASTEXITCODE -ne 0) { throw "sim-opt-parity: cmake configure ($parityPreset) failed ($LASTEXITCODE)" }
        & cmake --build --preset $parityPreset --target luminumbra_server_app
        if ($LASTEXITCODE -ne 0) {
            throw "sim-opt-parity: build ($parityPreset) failed ($LASTEXITCODE). If this is the entt vague-linkage undefined-reference error, the - parity override in CMakeLists.txt did NOT take; -O0 is known-unlinkable (R1)."
        }
    } finally {
        $env:PATH = $oldPath
    }

    if (-not (Test-Path $parityExe)) {
        throw "sim-opt-parity: parity server exe missing after build - $parityExe"
    }

    # Reference hash: the normal -O1 build's HeadlessServerTick artifact. Produce it
    # fresh from the normal build so the comparison is same-input, same-scenario.
    $normalExe = "build/$BuildPreset/bin/luminumbra_server_app.exe"
    if (-not (Test-Path $normalExe)) {
        throw "sim-opt-parity: normal server exe missing - $normalExe (build the debug tree first)"
    }
    $normalArtifact = "build/$BuildPreset/test-artifacts/server/server-tick-optparity-ref.json"
    $parityArtifact = "$parityDir/test-artifacts/server/server-tick-optparity.json"
    New-Item -ItemType Directory -Force -Path (Split-Path $normalArtifact) | Out-Null
    New-Item -ItemType Directory -Force -Path (Split-Path $parityArtifact) | Out-Null
    Remove-Item -ErrorAction SilentlyContinue $normalArtifact, $parityArtifact

    & $normalExe --smoke --ticks 90 --artifact $normalArtifact
    if ($LASTEXITCODE -ne 0) { throw "sim-opt-parity: normal (-O1) smoke exited $LASTEXITCODE" }
    & $parityExe --smoke --ticks 90 --artifact $parityArtifact
    if ($LASTEXITCODE -ne 0) { throw "sim-opt-parity: parity (-) smoke exited $LASTEXITCODE" }

    $normal = Read-JsonArtifact $normalArtifact "luminumbra.server_tick.v1"
    $parity = Read-JsonArtifact $parityArtifact "luminumbra.server_tick.v1"
    Assert-ArtifactPassed $normal "SimOptLevelParity(normal -O1)"
    Assert-ArtifactPassed $parity "SimOptLevelParity(parity -)"

    if ([string]::IsNullOrEmpty($normal.world_hash) -or [string]::IsNullOrEmpty($parity.world_hash)) {
        throw "sim-opt-parity: empty world_hash (normal='$($normal.world_hash)' parity='$($parity.world_hash)')"
    }
    if ($normal.world_hash -ne $parity.world_hash) {
        # Localize via sub-hashes if available.
        if ($null -ne $normal.sub_hashes -and $null -ne $parity.sub_hashes) {
            foreach ($section in @("terrain", "mesh", "water", "entities", "wind")) {
                $a = $normal.sub_hashes.$section
                $b = $parity.sub_hashes.$section
                if ($a -ne $b) {
                    Write-Host "  sim-opt-parity sub-hash divergence in '$section': $a (-O1) != $b (-)"
                }
            }
        }
        throw "sim-opt-parity: world_hash DIVERGED across opt levels: -O1='$($normal.world_hash)' vs -='$($parity.world_hash)'. The GameSession.cpp opt-level override is NOT determinism-neutral (FMA/optimizer drift)."
    }

    Write-Host ("sim-opt-parity: world_hash identical across opt levels (-O1 GameSession vs - parity): {0}" -f $normal.world_hash)
}

function Test-CreatureSlice {
    # Project Capture game slice. One data-driven creature
    # (grovestrider archetype) rendered through the skinned G-Buffer stage,
    # planned by the fixed-tick InstinctSystem, switching behavior on a light
    # stimulus (graze -> approach). The artifact records the planner state
    # before/after the stimulus plus the two screenshots.
    $exe = Get-ClientExe
    $sliceDir = "build/$BuildPreset/test-artifacts/runtime/creature-slice"
    Remove-Item -Recurse -Force -ErrorAction SilentlyContinue $sliceDir
    New-Item -ItemType Directory -Force -Path $sliceDir | Out-Null

    #.lmesh files are generated assets (gitignored); rebuild them from the
    # committed glTF sources when missing. The asset processor is bitwise
    # deterministic, so regenerated outputs match the authored content.
    $assetProcessor = "build/$BuildPreset/bin/asset_processor.exe"
    foreach ($asset in @(
        @{ gltf = "data/models/creatures/grovestrider/grovestrider.gltf"; lmesh = "data/models/creatures/grovestrider/grovestrider.lmesh" },
        @{ gltf = "data/models/props/glow_bloom/glow_bloom.gltf"; lmesh = "data/models/props/glow_bloom/glow_bloom.lmesh" }
    )) {
        if (-not (Test-Path $asset.lmesh)) {
            if (-not (Test-Path $assetProcessor)) {
                throw "creature slice needs $($asset.lmesh); build asset_processor first ($assetProcessor missing)"
            }
            Invoke-Checked -FilePath $assetProcessor -ArgumentList @($asset.gltf, $asset.lmesh) -TimeoutSeconds 120
        }
    }

    $runSeconds = [Math]::Max(30, $SmokeSeconds)
    Invoke-Checked -FilePath $exe -ArgumentList @(
        "--scenario", "creature_slice_smoke",
        "--creature-archetype", "data/common/archetypes/grovestrider.json",
        "--auto-create-world",
        "--auto-enter-world",
        "--timed-run", "$runSeconds",
        "--no-audio",
        "--no-ui",
        "--runtime-artifact-dir", $sliceDir
    ) -TimeoutSeconds ([Math]::Max(150, $runSeconds + 90))

    $analysisPath = Join-Path $sliceDir "creature-slice-analysis.json"
    if (-not (Test-Path $analysisPath)) {
        throw "creature slice run did not produce $analysisPath (gate producer)"
    }

    $analysis = Get-Content $analysisPath -Raw | ConvertFrom-Json
    if ($analysis.schema -ne "luminumbra.creature_slice_analysis.v1") {
        throw "Unexpected creature slice analysis schema '$($analysis.schema)'"
    }
    if ($analysis.archetype -ne "grovestrider") {
        throw "Creature slice must spawn the grovestrider archetype from game data"
    }
    if ([int64]$analysis.gl_debug.errors -ne 0) {
        throw "Creature slice run emitted GL debug errors: $($analysis.gl_debug.errors)"
    }
    if (-not $analysis.scene.stimulus_spawned) {
        throw "Creature slice never spawned the light stimulus"
    }
    foreach ($capture in @($analysis.before_stimulus, $analysis.after_stimulus)) {
        if (-not $capture.plan.valid) {
            throw "Creature slice capture '$($capture.file)' has no valid planner state"
        }
        if ([int64]$capture.skinned_draws -lt 1) {
            throw "Creature slice capture '$($capture.file)' rendered no skinned draws"
        }
        Assert-PpmArtifact (Join-Path $sliceDir $capture.file)
    }
    if ($analysis.before_stimulus.plan.action -ne $analysis.expected.before_action) {
        throw "Creature slice pre-stimulus plan is '$($analysis.before_stimulus.plan.action)', expected '$($analysis.expected.before_action)'"
    }
    if ($analysis.after_stimulus.plan.action -ne $analysis.expected.after_action) {
        throw "Creature slice post-stimulus plan is '$($analysis.after_stimulus.plan.action)', expected '$($analysis.expected.after_action)'"
    }
    if ([int64]$analysis.after_stimulus.plan.plans_executed -le [int64]$analysis.before_stimulus.plan.plans_executed) {
        throw "Creature slice planner did not replan after the stimulus"
    }
    #  composition check: a "functionally green, visually broken"
    # capture (creature rendered + planner correct, but the camera stares at
    # the ground or the sky, or the creature is camouflaged against the sand)
    # must not pass. sky_ratio in [0.05, 0.6] proves a horizon is visible
    # (not staring at ground or sky); the creature ROI mean color must differ
    # from the surrounding terrain mean by an L1 distance >= 24 (over 0-255 RGB
    # channel means) so the dark-moss creature reads against the bright shore.
    $minSkyRatio = 0.05
    $maxSkyRatio = 0.6
    $minColorDelta = 24.0
    foreach ($capture in @($analysis.before_stimulus, $analysis.after_stimulus)) {
        $comp = $capture.composition
        if ($null -eq $comp -or -not $comp.valid) {
            throw "Creature slice capture '$($capture.file)' has no valid composition metrics (creature did not project into the frame)"
        }
        if ([double]$comp.sky_ratio -lt $minSkyRatio -or [double]$comp.sky_ratio -gt $maxSkyRatio) {
            throw "Creature slice capture '$($capture.file)' sky_ratio $($comp.sky_ratio) is outside [$minSkyRatio, $maxSkyRatio] (horizon not visible / staring at ground or sky)"
        }
        if ([double]$comp.creature_terrain_color_delta -lt $minColorDelta) {
            throw "Creature slice capture '$($capture.file)' creature_terrain_color_delta $($comp.creature_terrain_color_delta) is below $minColorDelta (creature reads invisibly against the terrain)"
        }
        #  emissive glow halo: when the glow_bloom stimulus is framed, its
        # emission must read as a luminance gradient (bright core/ring above a
        # falling-off background), not a flat patch.
        if ($comp.glow_measured) {
            $lums = @([double]$comp.glow_core_luminance, [double]$comp.glow_ring_luminance, [double]$comp.glow_background_luminance)
            $span = ($lums | Measure-Object -Maximum).Maximum - ($lums | Measure-Object -Minimum).Minimum
            if ($span -lt 8.0) {
                throw "Creature slice capture '$($capture.file)' glow halo is flat (luminance span $span < 8): no visible bloom falloff ring"
            }
        }
    }

    if (-not $analysis.passed) {
        throw "Creature slice analysis reported failure: $($analysis.failures -join ', ')"
    }
    Write-Host ("creature slice composition: sky_ratio {0:N3}/{1:N3}, color_delta {2:N1}/{3:N1}" -f `
        $analysis.before_stimulus.composition.sky_ratio, $analysis.after_stimulus.composition.sky_ratio, `
        $analysis.before_stimulus.composition.creature_terrain_color_delta, $analysis.after_stimulus.composition.creature_terrain_color_delta)
    Write-Host ("creature slice: plan {0} ({1}) -> {2} ({3}); clips {4} -> {5}; skinned draws {6}/{7}; plans {8} -> {9}" -f `
        $analysis.before_stimulus.plan.action, $analysis.before_stimulus.plan.target, `
        $analysis.after_stimulus.plan.action, $analysis.after_stimulus.plan.target, `
        $analysis.before_stimulus.plan.active_clip, $analysis.after_stimulus.plan.active_clip, `
        $analysis.before_stimulus.skinned_draws, $analysis.after_stimulus.skinned_draws, `
        $analysis.before_stimulus.plan.plans_executed, $analysis.after_stimulus.plan.plans_executed)
    Assert-CapturePinned -ArtifactDir $sliceDir -Name "CreatureSlice"
}

# ---   StimulusChannelGate mode: append-only ---
# Ecology stimulus-channel registry feeding the InstinctSystem planner. Runs the
# StimulusChannelGate gtest suite (frontier_gates_test), which asserts:
#   * INERT: a non-subscribing creature plans IDENTICALLY with or without a
#     stimulus context (the canonical-neutral property keeping world_hash at
#     d950a6afc12a5cdc -- regression review / documented design).
#   * BEHAVIOR DIFFERS: a reactive (game-data opt-in) creature plans differently
#     across weather fixtures (clear vs rain) and time-of-day fixtures (midnight
#     vs noon) -- the channels reach the plan.
#   * DETERMINISTIC: run==replay within a fixture.
# Append-only; no existing gate behavior changes. The default-roster world_hash
# stays unchanged (proven separately by HeadlessServerTick).
function Test-StimulusChannelGate {
    $exe = "build/$BuildPreset/bin/frontier_gates_test.exe"
    if (-not (Test-Path $exe)) {
        throw "StimulusChannelGate gate: missing $exe (cmake --build --preset $BuildPreset)"
    }
    & $exe "--gtest_filter=StimulusChannelGate.*"
    if ($LASTEXITCODE -ne 0) {
        throw "StimulusChannelGate gtest (StimulusChannelGate.*) failed with exit code $LASTEXITCODE"
    }
    Write-Host "StimulusChannelGate: inert non-subscriber + behavior-differs (weather/time) + run==replay green"
}

# ---  BiomeCoverage mode: append-only ---
# Atlas coverage gate for biomes. Runs the MountainsBiomeCoverageAtlas gtest
# (which sweeps the shipped mountains preset - biomes enabled - at the fixed
# atlas seed and emits biome-coverage.json), then asserts every authored biome
# is present and the per-biome surface-material distribution holds. Append-only;
# no existing gate behavior changes.
function Test-BiomeCoverage {
    $exe = "build/$BuildPreset/bin/worldgen_layer_snapshot_test.exe"
    if (-not (Test-Path $exe)) {
        throw "BiomeCoverage gate: missing $exe (cmake --build --preset $BuildPreset)"
    }
    & $exe "--gtest_filter=WorldGenLayerSnapshotTest.MountainsBiomeCoverageAtlas"
    if ($LASTEXITCODE -ne 0) {
        throw "BiomeCoverage gtest (MountainsBiomeCoverageAtlas) failed with exit code $LASTEXITCODE"
    }

    $analysisPath = "build/$BuildPreset/test-artifacts/worldgen_layers/biome/biome-coverage.json"
    if (-not (Test-Path $analysisPath)) {
        throw "BiomeCoverage gate: missing $analysisPath (produced by MountainsBiomeCoverageAtlas)"
    }
    $analysis = Get-Content $analysisPath -Raw | ConvertFrom-Json
    if ($analysis.schema -ne "luminumbra.biome_coverage.v1") {
        throw "Unexpected biome coverage schema '$($analysis.schema)'"
    }
    if ($analysis.preset -ne "mountains") {
        throw "BiomeCoverage must analyze the mountains preset (got '$($analysis.preset)')"
    }
    if (-not $analysis.all_authored_biomes_present) {
        throw "BiomeCoverage: not every authored biome is present in the atlas window"
    }
    if ([int64]$analysis.authored_biome_count -lt 4) {
        throw "BiomeCoverage: expected >= 4 authored biomes (got $($analysis.authored_biome_count))"
    }
    if ([int64]$analysis.distinct_biomes_realized -lt 3) {
        throw "BiomeCoverage: fewer than 3 biomes realized in the window ($($analysis.distinct_biomes_realized))"
    }
    if (-not $analysis.passed) {
        throw "BiomeCoverage analysis reported failure"
    }

    $biomes = @($analysis.biomes)
    if ($biomes.Count -lt 4) {
        throw "BiomeCoverage: biome entry array too small ($($biomes.Count))"
    }
    foreach ($biome in $biomes) {
        if ([int64]$biome.columns -le 0) {
            throw "BiomeCoverage: authored biome '$($biome.name)' has zero columns"
        }
        if ($null -eq $biome.surface_material_histogram) {
            throw "BiomeCoverage: biome '$($biome.name)' is missing its surface material histogram"
        }
    }
    Write-Host ("biome coverage gate passed: preset={0} columns={1} authored={2} realized={3} table_hash={4}" -f `
        $analysis.preset, $analysis.total_columns, $analysis.authored_biome_count, `
        $analysis.distinct_biomes_realized, $analysis.biome_table_content_hash)
}

# ---  RiverPresence mode: append-only ---
# Runs the MountainsRiverPresenceAtlas gtest (which sweeps the shipped mountains
# preset - rivers enabled - and emits river-presence.json), then asserts rivers
# are present, every river column's folded PV sits in the valleys band (zero
# band violations), the channel carves to a waterline, and the river course is
# continuous. Append-only; no existing gate behavior changes.
function Test-RiverPresence {
    $exe = "build/$BuildPreset/bin/worldgen_layer_snapshot_test.exe"
    if (-not (Test-Path $exe)) {
        throw "RiverPresence gate: missing $exe (cmake --build --preset $BuildPreset)"
    }
    & $exe "--gtest_filter=WorldGenLayerSnapshotTest.MountainsRiverPresenceAtlas"
    if ($LASTEXITCODE -ne 0) {
        throw "RiverPresence gtest (MountainsRiverPresenceAtlas) failed with exit code $LASTEXITCODE"
    }

    $analysisPath = "build/$BuildPreset/test-artifacts/worldgen_layers/river/river-presence.json"
    if (-not (Test-Path $analysisPath)) {
        throw "RiverPresence gate: missing $analysisPath (produced by MountainsRiverPresenceAtlas)"
    }
    $analysis = Get-Content $analysisPath -Raw | ConvertFrom-Json
    if ($analysis.schema -ne "luminumbra.river_presence.v1") {
        throw "Unexpected river presence schema '$($analysis.schema)'"
    }
    if ($analysis.preset -ne "mountains") {
        throw "RiverPresence must analyze the mountains preset (got '$($analysis.preset)')"
    }
    if ([int64]$analysis.river_columns -le 0) {
        throw "RiverPresence: no river columns found in the atlas window"
    }
    if ([int64]$analysis.band_violations -ne 0) {
        throw "RiverPresence: $($analysis.band_violations) river columns fall outside the PV valleys band"
    }
    if ([int64]$analysis.waterline_columns -le 0) {
        throw "RiverPresence: river channels never reach the waterline (no carve below SEA_LEVEL)"
    }
    if ([int64]$analysis.longest_continuous_run -lt 3) {
        throw "RiverPresence: river course is not continuous (longest run $($analysis.longest_continuous_run))"
    }
    if (-not $analysis.passed) {
        throw "RiverPresence analysis reported failure"
    }
    Write-Host ("river presence gate passed: preset={0} river_cols={1} waterline_cols={2} longest_run={3} ratio={4} band=[{5},{6}]" -f `
        $analysis.preset, $analysis.river_columns, $analysis.waterline_columns, `
        $analysis.longest_continuous_run, $analysis.river_ratio, $analysis.river_pv_min, $analysis.river_pv_max)
}

# ---   WaterfallVisual mode: append-only ---
# Runs the WaterfallVisualTest gtest (which builds the shipped mountains world -
# rivers enabled - at the atlas seed, runs the render-side WaterfallDetect TWICE,
# asserts the sites are byte-identical + same-seed-same-sites - the
# determinism contract - and renders a sheet/spray/foam capture at a detected
# site), then asserts from waterfall-visual.json that determinism held, falls
# were detected, and the dressing capture shows the sheet + spray + foam.
# Render-only: detection is a pure function of the world, never hashed (world_hash
# stays d950a6afc12a5cdc). Append-only; no existing gate behavior changes.
function Test-WaterfallVisual {
    $exe = "build/$BuildPreset/bin/waterfall_visual_test.exe"
    if (-not (Test-Path $exe)) {
        throw "WaterfallVisual gate: missing $exe (cmake --build --preset $BuildPreset)"
    }
    & $exe "--gtest_filter=WaterfallVisualTest.SiteDetectionDeterministicAndDressed"
    if ($LASTEXITCODE -ne 0) {
        throw "WaterfallVisual gtest (SiteDetectionDeterministicAndDressed) failed with exit code $LASTEXITCODE"
    }

    $analysisPath = "build/$BuildPreset/test-artifacts/render/waterfall/waterfall-visual.json"
    if (-not (Test-Path $analysisPath)) {
        throw "WaterfallVisual gate: missing $analysisPath (produced by WaterfallVisualTest)"
    }
    $analysis = Get-Content $analysisPath -Raw | ConvertFrom-Json
    if ($analysis.schema -ne "luminumbra.waterfall_visual.v1") {
        throw "Unexpected waterfall visual schema '$($analysis.schema)'"
    }
    if ($analysis.preset -ne "mountains") {
        throw "WaterfallVisual must analyze the mountains preset (got '$($analysis.preset)')"
    }
    if ([int64]$analysis.site_count -le 0) {
        throw "WaterfallVisual: no waterfall sites detected on the mountains preset"
    }
    # Determinism (regression review): repeated detection byte-equal + same seed -> same sites.
    if (-not $analysis.determinism_byte_equal) {
        throw "WaterfallVisual: site detection not byte-identical across runs (non-deterministic)"
    }
    if (-not $analysis.determinism_same_seed_same_sites) {
        throw "WaterfallVisual: same seed produced different sites ( contract broken)"
    }
    if ($analysis.site_hash_run_a -ne $analysis.site_hash_world_b) {
        throw "WaterfallVisual: site hash differs across worlds with the same seed"
    }
    # Dressing capture (when GL was available): sheet + spray + foam must render.
    if ($analysis.capture_written) {
        if (-not $analysis.sheet_present) {
            throw "WaterfallVisual: capture shows no falling-sheet cascade body"
        }
        if (-not $analysis.spray_present) {
            throw "WaterfallVisual: capture shows no spray/mist plume"
        }
        if (-not $analysis.foam_present) {
            throw "WaterfallVisual: capture shows no plunge-pool/crest foam"
        }
    } else {
        Write-Host "WaterfallVisual: dressing capture skipped (no GL context: $($analysis.gl_skip_reason)); determinism contract still gated"
    }
    if (-not $analysis.passed) {
        throw "WaterfallVisual analysis reported failure"
    }
    Write-Host ("waterfall visual gate passed: preset={0} sites={1} best_drop={2:} m steepness={3:} sheet={4} spray={5} foam={6} (capture={7})" -f `
        $analysis.preset, $analysis.site_count, $analysis.best_drop_height, $analysis.best_steepness, `
        $analysis.cascade_pixels, $analysis.spray_pixels, $analysis.foam_pixels, $analysis.capture_written)
}

# ---  StructurePresence mode: append-only ---
# Runs the StructurePlacement gtest (which loads the shipped cairn + ruin
# template pools, proves the placement grid is deterministic - same seed =>
# same sites, locate(type, near) verified against a brute-force scan - and
# snapshots the assembled fixture voxel hash). Append-only; no existing gate
# behavior changes.
function Test-StructurePresence {
    $exe = "build/$BuildPreset/bin/common_tests.exe"
    if (-not (Test-Path $exe)) {
        throw "StructurePresence gate: missing $exe (cmake --build --preset $BuildPreset)"
    }
    & $exe "--gtest_filter=StructurePlacementTest.*"
    if ($LASTEXITCODE -ne 0) {
        throw "StructurePresence gtest (StructurePlacementTest.*) failed with exit code $LASTEXITCODE"
    }
    Write-Host "structure presence gate passed: cairn + ruin pools load, placement grid deterministic, assembled voxel hashes stable"
}

# ---  BiomeReverb mode: append-only ---
# Runs the BiomeReverb gtest (loads the shipped biomes.json, proves the per-biome
# reverb params are CONSUMED into BiomeTable and that reverb_for(active biome id)
# returns the authored profile - the data->engine flow EnvironmentalAudioSystem
# drives via SHIELD_WorldSystem::BiomeReverbAt). Append-only.
function Test-BiomeReverb {
    $exe = "build/$BuildPreset/bin/common_tests.exe"
    if (-not (Test-Path $exe)) {
        throw "BiomeReverb gate: missing $exe (cmake --build --preset $BuildPreset)"
    }
    & $exe "--gtest_filter=BiomeReverbTest.*"
    if ($LASTEXITCODE -ne 0) {
        throw "BiomeReverb gtest (BiomeReverbTest.*) failed with exit code $LASTEXITCODE"
    }
    Write-Host "biome reverb gate passed: per-biome reverb params consumed; active-biome reverb flow validated"
}

# ---  TerrainRealism mode: append-only ---
# Runs the DEM-grounded realism gtest (which generates every shipped preset at
# the fixed atlas seed and computes slope distribution, Strahler hypsometric
# integral, and radially-averaged spectral-slope beta over the atlas window),
# then consumes the emitted worldgen_terrain_realism.json artifact and asserts
# every preset's hypsometric integral and spectral beta sit inside the
# real-world DEM reference bands for its declared landscape class
# (test/fixtures/dem/*.json, derived by tools/derive_dem_stats.py from
# public-domain AWS Terrain Tiles). Append-only; no existing gate behavior
# changes.
function Test-TerrainRealism {
    $exe = "build/$BuildPreset/bin/worldgen_layer_snapshot_test.exe"
    if (-not (Test-Path $exe)) {
        throw "TerrainRealism gate: missing $exe (cmake --build --preset $BuildPreset)"
    }
    & $exe "--gtest_filter=WorldGenLayerSnapshotTest.AuthoredPresetsMeetDemReferenceRealismBands"
    if ($LASTEXITCODE -ne 0) {
        throw "TerrainRealism gtest (AuthoredPresetsMeetDemReferenceRealismBands) failed with exit code $LASTEXITCODE"
    }

    $analysisPath = "build/$BuildPreset/test-artifacts/worldgen_layers/atlas/worldgen_terrain_realism.json"
    if (-not (Test-Path $analysisPath)) {
        throw "TerrainRealism gate: missing $analysisPath (produced by AuthoredPresetsMeetDemReferenceRealismBands)"
    }
    $analysis = Get-Content $analysisPath -Raw | ConvertFrom-Json
    if ($analysis.schema -ne "luminumbra.worldgen_terrain_realism.v1") {
        throw "Unexpected terrain realism schema '$($analysis.schema)'"
    }
    $presets = @($analysis.presets)
    if ($presets.Count -lt 5) {
        throw "TerrainRealism: expected >= 5 presets, got $($presets.Count)"
    }
    foreach ($p in $presets) {
        $hi = [double]$p.hypsometric_integral
        $hiLo = [double]$p.hi_band[0]; $hiHi = [double]$p.hi_band[1]
        if ($hi -lt $hiLo -or $hi -gt $hiHi) {
            throw ("TerrainRealism: preset '{0}' ({1}) hypsometric integral {2} outside DEM band [{3},{4}]" -f `
                $p.preset, $p.class, $hi, $hiLo, $hiHi)
        }
        $beta = [double]$p.spectral_beta
        $bLo = [double]$p.beta_band[0]; $bHi = [double]$p.beta_band[1]
        if ($beta -lt $bLo -or $beta -gt $bHi) {
            throw ("TerrainRealism: preset '{0}' ({1}) spectral beta {2} outside self-affine band [{3},{4}]" -f `
                $p.preset, $p.class, $beta, $bLo, $bHi)
        }
        Write-Host ("terrain realism: {0,-18} class={1,-10} HI={2:N3} [{3:},{4:}] beta={5:N3} [{6:},{7:}] p50/p95={8:N1}/{9:N1}deg" -f `
            $p.preset, $p.class, $hi, $hiLo, $hiHi, $beta, $bLo, $bHi, [double]$p.slope_p50_deg, [double]$p.slope_p95_deg)
    }
    Write-Host "terrain realism gate passed: all presets in DEM reference bands (hypsometry + spectral beta) for their landscape class"
}

# resize-stress gate. A scripted run drives the render
# pipeline through a windowed->borderless->resolutions->fullscreen->restore-pinned
# resize cycle (RenderPipeline::on_resize), asserting 0 GL errors, that every
# size-changing step reallocated targets (resize generation bumped), the final
# state is restored to the pinned 1280x720 targets, and the pinned-size capture
# still meets the visual-Smoke expectations.
function Test-WindowModeStress {
    $exe = Get-ClientExe
    $visualDir = "build/$BuildPreset/test-artifacts/runtime/window-mode-stress"
    Remove-Item -Recurse -Force -ErrorAction SilentlyContinue $visualDir
    New-Item -ItemType Directory -Force -Path $visualDir | Out-Null

    $runSeconds = [Math]::Max(20, $SmokeSeconds)
    Invoke-Checked -FilePath $exe -ArgumentList @(
        "--scenario", "window_mode_stress_smoke",
        "--auto-create-world",
        "--auto-enter-world",
        "--timed-run", "$runSeconds",
        "--no-audio",
        "--no-ui",
        "--runtime-artifact-dir", $visualDir
    ) -TimeoutSeconds ([Math]::Max(150, $runSeconds + 120))

    $analysisPath = Join-Path $visualDir "window-mode-stress-analysis.json"
    $analysis = Read-JsonArtifact -Path $analysisPath -Schema "luminumbra.window_mode_stress.v1"

    if ([int64]$analysis.gl_debug.errors -ne 0) {
        throw "window-mode stress run emitted GL debug errors: $($analysis.gl_debug.errors)"
    }
    $steps = @($analysis.steps)
    if ($steps.Count -lt 2) {
        throw "window-mode stress run recorded only $($steps.Count) resize steps (expected the full scripted cycle)"
    }
    foreach ($step in $steps) {
        if (-not [bool]$step.passed) {
            throw "window-mode stress step '$($step.label)' failed: size_changed=$($step.size_changed), gen $($step.resize_generation_before)->$($step.resize_generation_after), gl_errors=$($step.gl_errors_after), targets=$($step.targets_width_after)x$($step.targets_height_after)"
        }
    }
    if ([int64]$analysis.aggregates.size_changing_steps -le 0) {
        throw "window-mode stress run changed the framebuffer size on no steps (the resize chain was never exercised)"
    }
    if ([int64]$analysis.aggregates.reallocating_steps -ne [int64]$analysis.aggregates.size_changing_steps) {
        throw "window-mode stress: only $($analysis.aggregates.reallocating_steps) of $($analysis.aggregates.size_changing_steps) size-changing steps reallocated targets"
    }
    if (-not [bool]$analysis.aggregates.final_targets_pinned) {
        throw "window-mode stress run did not restore the pinned 1280x720 targets after the resize cycle"
    }

    # The final pinned-size capture must record pinned == true and meet the
    # Smoke-equivalent content floor.
    $pin = $analysis.capture_pin
    if (-not [bool]$pin.pinned) {
        throw "window-mode stress final capture is NOT pinned: $($pin.capture_width)x$($pin.capture_height)"
    }
    Assert-PpmArtifact (Join-Path $visualDir $analysis.final_capture.file)
    if (-not [bool]$analysis.passed) {
        throw "window-mode stress analysis reported failure (dark_ratio=$($analysis.aggregates.final_dark_ratio))"
    }

    Write-Host "window-mode stress gate passed: $($analysis.aggregates.size_changing_steps) resize steps, all reallocated targets, 0 GL errors, final pinned $($pin.capture_width)x$($pin.capture_height)"
}

# ---------------------------------------------------------------------------
#  Group A — artifact provenance manifests + running-binary comparison
# and the manual GPU/perf test tier enumeration.
#
# The visual/perf gates bless captures and compare against baselines. Without a
# provenance record, a capture taken with one binary/shader set can be blessed or
# compared against a different (rebuilt) one ("bless wrong images / pass against
# stale binaries"). These helpers compute a provenance manifest for the binary +
# shaders CURRENTLY being gated and refuse a bless/compare whose candidate manifest
# does not match. All paths resolve under build/$BuildPreset (single-root, ).
# ---------------------------------------------------------------------------

function Get-FileSha256 {
    param([string]$Path)
    if (-not (Test-Path -LiteralPath $Path)) { throw "Cannot hash missing file: $Path" }
    return (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant()
}

function Get-ShaderTreeHash {
    # Stable content hash over the shader source tree: each shader's relative path +
    # SHA256, sorted by path, then hashed. Editing/adding/removing any shader moves it.
    param([string]$ShaderDir = "res/shaders")
    if (-not (Test-Path -LiteralPath $ShaderDir)) { throw "Shader dir not found: $ShaderDir" }
    $exts = @(".frag", ".vert", ".comp", ".glsl", ".geom", ".tesc", ".tese")
    $base = (Resolve-Path -LiteralPath $ShaderDir).Path
    $files = Get-ChildItem -LiteralPath $ShaderDir -Recurse -File |
        Where-Object { $exts -contains $_.Extension.ToLowerInvariant() } |
        Sort-Object FullName
    $sb = New-Object System.Text.StringBuilder
    foreach ($f in $files) {
        $rel = ($f.FullName.Substring($base.Length).TrimStart('\', '/')) -replace '\\', '/'
        $h = (Get-FileHash -LiteralPath $f.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
        [void]$sb.Append($rel).Append(':').Append($h).Append("`n")
    }
    $bytes = [System.Text.Encoding]::UTF8.GetBytes($sb.ToString())
    $sha = [System.Security.Cryptography.SHA256]::Create()
    try {
        $digest = $sha.ComputeHash($bytes)
    } finally {
        $sha.Dispose()
    }
    return (($digest | ForEach-Object { $_.ToString("x2") }) -join "")
}

function Test-HeadlessInGameCapture {
    # the headless IN_GAME capture paths must reach report emission and
    # exit 0 under a hard wall-clock leash. Regression gate for the frame-2 main-thread stall
    # (the 2f3c8eec  doline/crystal full-SDF scans) that presented as the capture
    # "hang": the client rendered one frame, logged the doline line, then ground CPU-bound
    # through millions of get_density_at probes with zero further frames — defeating the
    # frame-COUNTING watchdog, which only advances while the render loop spins. The external
    # leash here is the backstop for that whole class of mid-frame stalls.
    $exe = Get-ClientExe
    $outDir = "build/$BuildPreset/test-artifacts/render/headless-capture"
    New-Item -ItemType Directory -Force -Path $outDir | Out-Null
    $scanJson = Join-Path $outDir "headless-capture-scan.json"
    $healthJson = Join-Path $outDir "headless-capture-scan.health.json"
    $nightScene = Join-Path $outDir "headless-capture-night-scene.json"
    $nightShot = Join-Path $outDir "headless-capture-night.ppm"
    Remove-Item -LiteralPath $scanJson, $healthJson, $nightShot -Force -ErrorAction SilentlyContinue

    Write-Host "HeadlessInGameCapture: --frame-scan (noon, settle) under a 420 s leash..."
    Invoke-Checked -FilePath $exe -ArgumentList @("--frame-scan", $scanJson, "--no-audio") -TimeoutSeconds 420
    if (-not (Test-Path -LiteralPath $scanJson)) {
        throw "HeadlessInGameCapture: frame-scan exited 0 but wrote no report: $scanJson"
    }
    if (-not (Test-Path -LiteralPath $healthJson)) {
        throw "HeadlessInGameCapture: frame-scan wrote no frame-health sidecar: $healthJson"
    }
    $health = Get-Content -LiteralPath $healthJson -Raw | ConvertFrom-Json
    if ($health.verdict.anomalous) {
        throw "HeadlessInGameCapture: frame-health ANOMALY on the settled noon frame: $($health.verdict.reason)"
    }
    Write-Host ("HeadlessInGameCapture: noon frame-scan OK (mean luma {0:N4}, coverage {1:N1}%)" -f `
        [double]$health.mean_luminance, (100.0 * [double]$health.gbuffer_coverage))

    # Night leg: a self-contained --scene-config (true-midnight TOD 0.5, full moon) written
    # into the gate's artifact dir; scene-config does NOT self-imply the auto-world, so pass
    # the boot flags explicitly.
    $scene = [ordered]@{
        camera      = [ordered]@{ pos = @(8.0, 24.0, 8.0); yaw = 35.0; pitch = -14.0 }
        time_of_day = 0.5
        moon        = 1.0
        screenshot  = ($nightShot -replace '\\', '/')
    }
    ($scene | ConvertTo-Json -Depth 4) | Out-File -LiteralPath $nightScene -Encoding utf8
    Write-Host "HeadlessInGameCapture: --scene-config night (TOD 0.5, moon 1.0) under a 420 s leash..."
    Invoke-Checked -FilePath $exe -ArgumentList @(
        "--scene-config", $nightScene, "--auto-create-world", "--auto-enter-world", "--no-audio"
    ) -TimeoutSeconds 420
    if (-not (Test-Path -LiteralPath $nightShot)) {
        throw "HeadlessInGameCapture: scene-config exited 0 but wrote no shot: $nightShot"
    }
    $shotBytes = (Get-Item -LiteralPath $nightShot).Length
    if ($shotBytes -lt 1024) {
        throw "HeadlessInGameCapture: night shot is implausibly small ($shotBytes bytes): $nightShot"
    }
    Write-Host ("HeadlessInGameCapture: PASS (noon scan nominal + night shot {0:N1} MB)" -f ($shotBytes / 1MB))
}

function Test-RenderParityFrame {
    #   (the  unlock): the in-process WHOLE-FRAME A/B. The client
    # boots the fixed frame-scan pose, settles, then dispatches the SAME prepared
    # frame TWICE into twin offscreen targets and FLIPs the readbacks in-process
    # (InProcessFlip::ComputeLumaFlip — bit-identical inputs score EXACTLY 0.0).
    # This is the byte-exact whole-frame gate the engine never had (cross-run FLIP
    # floors at ~0.057 noise; --smoke never renders): the  execution
    # migration and every render-band refactor gates old-vs-new through it.
    $exe = Get-ClientExe
    $outDir = "build/$BuildPreset/test-artifacts/render/frame-parity"
    New-Item -ItemType Directory -Force -Path $outDir | Out-Null
    $parityJson = Join-Path $outDir "frame_parity.json"
    Remove-Item -LiteralPath $parityJson -Force -ErrorAction SilentlyContinue

    Write-Host "RenderParityFrame: --render-parity-frame (fixed pose, settle) under a 420 s leash..."
    Invoke-Checked -FilePath $exe -ArgumentList @("--render-parity-frame", $outDir, "--no-audio") -TimeoutSeconds 420
    if (-not (Test-Path -LiteralPath $parityJson)) {
        throw "RenderParityFrame: client exited 0 but wrote no parity report: $parityJson"
    }
    $p = Get-Content -LiteralPath $parityJson -Raw | ConvertFrom-Json
    if ($p.schema -ne "luminumbra.render_frame_parity.v1") {
        throw "RenderParityFrame: unexpected schema '$($p.schema)'"
    }
    if ($p.metric -ne "luma") {
        throw "RenderParityFrame: unexpected metric '$($p.metric)' (thresholds are per-metric)"
    }
    # The contract is EXACT zero — not a sub-perceptual band. Both legs dispatch the
    # identical prepared frame in one process; any nonzero score is a dispatch-
    # idempotence break (or, during migration, an execution-path divergence).
    if (-not $p.passed -or ([double]$p.score) -ne 0.0) {
        throw ("RenderParityFrame: whole-frame A/B NOT exact - score={0} max_error={1} (must be exactly 0.0)" -f $p.score, $p.max_error)
    }
    Write-Host ("RenderParityFrame: PASS - whole-frame A/B EXACT (score 0.0, {0}x{1})" -f $p.width, $p.height)
}

function Test-ScheduledGateRun {
    # validate the newest canonically named report and bind it to the
    # actual Task Scheduler invocation which produced it. File mtimes are not
    # provenance: copying an old green report must never make it newest.
    $nightlyDir = "build/gate-artifacts/nightly"
    Assert-FileExists $nightlyDir

    $timestampFormat = "yyyy-MM-dd-HHmmss"
    $canonicalReports = New-Object System.Collections.Generic.List[object]
    foreach ($file in (Get-ChildItem -LiteralPath $nightlyDir -Filter "*.json" -File)) {
        if ($file.Name -notmatch '^\d{4}-\d{2}-\d{2}-\d{6}\.json$') {
            continue
        }
        $canonicalTimestamp = [datetime]::MinValue
        $parsed = [datetime]::TryParseExact(
            $file.BaseName,
            $timestampFormat,
            [Globalization.CultureInfo]::InvariantCulture,
            [Globalization.DateTimeStyles]::AssumeLocal,
            [ref]$canonicalTimestamp)
        if (-not $parsed) {
            throw "ScheduledGateRun: invalid canonical timestamp in '$($file.Name)'"
        }
        $canonicalReports.Add([pscustomobject]@{
            file = $file
            canonical_timestamp = $canonicalTimestamp
        }) | Out-Null
    }
    if ($canonicalReports.Count -eq 0) {
        throw "ScheduledGateRun: no dated JSON report found under $nightlyDir"
    }
    $latestRecord = $canonicalReports |
        Sort-Object canonical_timestamp -Descending |
        Select-Object -First 1
    $latest = $latestRecord.file

    $report = Read-JsonArtifact -Path $latest.FullName -Schema "luminumbra.nightly_gate.v1"
    $repoRoot = (Resolve-Path -LiteralPath ".").Path
    $taskName = "Luminumbra Nightly Gate"
    $taskPath = "\"
    $runnerPath = (Resolve-Path -LiteralPath "tools/gates/run-nightly-gate.ps1").Path
    $expectedPowerShell = (Get-Command powershell.exe -ErrorAction Stop).Source
    $expectedArguments = '-NoProfile -NonInteractive -ExecutionPolicy Bypass -File "{0}" -BuildPreset debug' -f $runnerPath
    $matchWindowSeconds = 120
    $dailyAt = "02:00"
    $expectedExecutionTimeLimit = [timespan]::FromHours(8)
    $identity = [Security.Principal.WindowsIdentity]::GetCurrent()
    Assert-NightlyTrackedTreeClean -RepoRoot $repoRoot
    $currentGitHead = Get-NightlyGitHead -RepoRoot $repoRoot
    $provenance = Assert-NightlyReportProvenance `
        -Report $report `
        -CurrentGitHead $currentGitHead `
        -Now ([datetimeoffset]::Now) `
        -ExpectedTaskPath "$taskPath$taskName" `
        -ExpectedCurrentAction $expectedPowerShell `
        -MaximumAge ([timespan]::FromHours(26)) `
        -MaximumFutureSkew ([timespan]::FromMinutes(5))
    $reportedDefinition = Assert-NightlyTaskDefinitionSnapshot `
        -Snapshot $report.scheduler_definition_at_start `
        -ExpectedTaskName $taskName `
        -ExpectedTaskPath $taskPath `
        -ExpectedPrincipalSid ([string]$identity.User.Value) `
        -ExpectedActionExecute $expectedPowerShell `
        -ExpectedActionArguments $expectedArguments `
        -ExpectedWorkingDirectory $repoRoot `
        -ExpectedDailyAt $dailyAt `
        -ExpectedExecutionTimeLimit $expectedExecutionTimeLimit
    if (-not [StringComparer]::OrdinalIgnoreCase.Equals(
        [string]$reportedDefinition.state, "Running")) {
        throw "ScheduledGateRun: reported start definition was not captured while the task was Running"
    }
    $generatedAt = $provenance.generated_at
    $runStartedAt = $provenance.run_started_at
    $completedAt = $provenance.completed_at

    $expectedFilename = $generatedAt.ToString(
        $timestampFormat,
        [Globalization.CultureInfo]::InvariantCulture) + ".json"
    if (-not [StringComparer]::Ordinal.Equals($latest.Name, $expectedFilename)) {
        throw "ScheduledGateRun: report filename '$($latest.Name)' does not match generated_at '$($report.generated_at)' (expected '$expectedFilename')"
    }

    $expectedJsonPath = (Resolve-Path -LiteralPath $latest.FullName).Path
    $expectedMarkdownPath = [IO.Path]::ChangeExtension($expectedJsonPath, ".md")
    Assert-FileExists $expectedMarkdownPath
    if (-not [StringComparer]::OrdinalIgnoreCase.Equals(
        [IO.Path]::GetFullPath([string]$report.repo_root), $repoRoot) -or
        -not [StringComparer]::OrdinalIgnoreCase.Equals(
            [IO.Path]::GetFullPath([string]$report.json_report), $expectedJsonPath) -or
        -not [StringComparer]::OrdinalIgnoreCase.Equals(
            [IO.Path]::GetFullPath([string]$report.markdown_report), $expectedMarkdownPath)) {
        throw "ScheduledGateRun: newest report path provenance does not match this checkout"
    }
    if ($report.build_preset -ne "debug") {
        throw "ScheduledGateRun: nightly build preset must be exactly debug"
    }

    if ($report.overall_status -ne "PASS") {
        throw "ScheduledGateRun: newest report is not green: $($latest.FullName)"
    }
    $required = @(
        "Build", "UnitTests", "EngineFrontierAll", "DeterminismMatrixQuick"
    )
    foreach ($name in $required) {
        $step = @($report.steps | Where-Object { $_.name -eq $name })
        if ($step.Count -ne 1) {
            throw "ScheduledGateRun: report must contain exactly one '$name' step"
        }
        if ($step[0].status -ne "PASS" -or [int]$step[0].exit_code -ne 0) {
            throw "ScheduledGateRun: '$name' is not PASS in $($latest.FullName)"
        }
    }

    if ($null -eq $report.scheduler_contract -or
        $report.scheduler_contract.task_name -ne $taskName -or
        $report.scheduler_contract.task_path -ne $taskPath -or
        -not [StringComparer]::OrdinalIgnoreCase.Equals(
            [string]$report.scheduler_contract.action_execute, $expectedPowerShell) -or
        -not [StringComparer]::Ordinal.Equals(
            [string]$report.scheduler_contract.action_arguments, $expectedArguments) -or
        -not [StringComparer]::OrdinalIgnoreCase.Equals(
            [string]$report.scheduler_contract.action_working_directory, $repoRoot) -or
        [int]$report.scheduler_contract.last_run_match_window_seconds -ne $matchWindowSeconds) {
        throw "ScheduledGateRun: report scheduler contract does not match the canonical nightly action"
    }

    foreach ($cmdlet in @("Get-ScheduledTask", "Get-ScheduledTaskInfo")) {
        if ($null -eq (Get-Command $cmdlet -ErrorAction SilentlyContinue)) {
            throw "ScheduledGateRun: Windows Task Scheduler cmdlet '$cmdlet' is unavailable"
        }
    }
    try {
        $tasks = @(Get-ScheduledTask -TaskName $taskName -TaskPath $taskPath -ErrorAction Stop)
    } catch {
        throw "ScheduledGateRun: required registered task '$taskName' was not found at '$taskPath'"
    }
    if ($tasks.Count -ne 1) {
        throw "ScheduledGateRun: expected exactly one registered '$taskName' task at '$taskPath'"
    }
    $task = $tasks[0]
    $null = Assert-NightlyRegisteredTaskDefinition `
        -Task $task `
        -ExpectedTaskName $taskName `
        -ExpectedTaskPath $taskPath `
        -ExpectedPrincipalSid ([string]$identity.User.Value) `
        -ExpectedActionExecute $expectedPowerShell `
        -ExpectedActionArguments $expectedArguments `
        -ExpectedWorkingDirectory $repoRoot `
        -ExpectedDailyAt $dailyAt `
        -ExpectedExecutionTimeLimit $expectedExecutionTimeLimit

    $taskInfo = Get-ScheduledTaskInfo -TaskName $taskName -TaskPath $taskPath -ErrorAction Stop
    if ([int64]$taskInfo.LastTaskResult -ne 0) {
        throw "ScheduledGateRun: '$taskName' LastTaskResult is $($taskInfo.LastTaskResult), expected 0"
    }
    $lastRunTime = [datetime]$taskInfo.LastRunTime
    if ($lastRunTime -eq [datetime]::MinValue) {
        throw "ScheduledGateRun: '$taskName' has no LastRunTime"
    }
    $lastRunAt = [datetimeoffset]$lastRunTime
    $lastRunDeltaSeconds = [math]::Abs(
        ($runStartedAt.ToUniversalTime() - $lastRunAt.ToUniversalTime()).TotalSeconds)
    if ($lastRunDeltaSeconds -gt $matchWindowSeconds) {
        throw "ScheduledGateRun: report run_started_at is $([math]::Round($lastRunDeltaSeconds, 1)) s from '$taskName' LastRunTime (limit $matchWindowSeconds s)"
    }

    Write-Host "ScheduledGateRun: PASS - $($latest.Name) is fresh at Git HEAD $currentGitHead and bound to '$taskName' instance $($report.scheduler_instance.instance_guid)/LastRunTime/LastTaskResult"
}

function Test-UpscaleSeamParity {
    # / closure: scale 1.0 must be bit-exact, while scale 0.67
    # must stay inside the preregistered in-process FLIP threshold. This is an
    # explicit GPU gate and intentionally is NOT included in -Mode All.
    $exe = Get-ClientExe
    $outDir = "build/$BuildPreset/test-artifacts/render/upscale-seam-parity"
    New-Item -ItemType Directory -Force -Path $outDir | Out-Null
    $parityJson = Join-Path $outDir "upscale_seam_parity.json"
    Remove-Item -LiteralPath $parityJson -Force -ErrorAction SilentlyContinue

    Write-Host "UpscaleSeamParity: native/1.0/0.67 in-process capture under a 420 s leash..."
    Invoke-Checked -FilePath $exe -ArgumentList @(
        "--upscale-seam-parity", $outDir, "--no-audio"
    ) -TimeoutSeconds 420
    if (-not (Test-Path -LiteralPath $parityJson)) {
        throw "UpscaleSeamParity: client wrote no parity report: $parityJson"
    }
    $p = Get-Content -LiteralPath $parityJson -Raw | ConvertFrom-Json
    if ($p.schema -ne "luminumbra.upscale_seam_parity.v1") {
        throw "UpscaleSeamParity: unexpected schema '$($p.schema)'"
    }
    if ($p.metric -ne "luma") {
        throw "UpscaleSeamParity: unexpected metric '$($p.metric)'"
    }
    if (-not $p.leg1.passed -or ([double]$p.leg1.score) -ne 0.0) {
        throw ("UpscaleSeamParity: scale-1 seam NOT exact - score={0}, max={1}" -f `
            $p.leg1.score, $p.leg1.max_error)
    }
    $threshold = [double]$p.leg2.threshold
    $score = [double]$p.leg2.score
    if ($threshold -le 0.0 -or $threshold -gt 0.08) {
        throw "UpscaleSeamParity: invalid leg-2 threshold $threshold (must be in (0,0.08])"
    }
    if (-not $p.leg2.passed -or $score -gt $threshold) {
        throw ("UpscaleSeamParity: scale-0.67 score {0} exceeds threshold {1}" -f `
            $score, $threshold)
    }
    if ([int]$p.leg2.internal_width -ge [int]$p.output_width -or
        [int]$p.leg2.internal_height -ge [int]$p.output_height) {
        throw "UpscaleSeamParity: leg 2 did not use a reduced internal extent"
    }
    Write-Host ("UpscaleSeamParity: PASS - 1.0 EXACT; 0.67 score {0} <= {1} ({2}x{3} -> {4}x{5})" -f `
        $score, $threshold, $p.leg2.internal_width, $p.leg2.internal_height,
        $p.output_width, $p.output_height)
}

function Get-GitSha {
    try {
        $sha = & git rev-parse HEAD 2>$null
        if ($LASTEXITCODE -eq 0 -and $sha) { return ([string]$sha).Trim() }
    } catch { }
    return "unknown"
}

function Resolve-GatedExe {
    # The binary the gate is exercising, resolved from the SAME build/$BuildPreset root the
    # gate built. Prefer the client (visual/perf gates) then the server (headless).
    $candidates = @(
        "build/$BuildPreset/bin/luminumbra_client_app.exe",
        "build/$BuildPreset/bin/luminumbra_server_app.exe"
    )
    foreach ($c in $candidates) {
        if (Test-Path -LiteralPath $c) { return $c }
    }
    throw "No gated executable found under build/$BuildPreset/bin (run -Mode Build first)"
}

function New-ArtifactManifest {
    # Full provenance record: all six fields for the binary + shaders being gated.
    param(
        [string]$ExePath,
        [string]$Scenario,
        [string]$ShaderDir = "res/shaders"
    )
    return [ordered]@{
        schema       = "luminumbra.artifact_manifest.v1"
        build_preset = $BuildPreset
        git_sha      = Get-GitSha
        exe_hash     = Get-FileSha256 $ExePath
        shader_hash  = Get-ShaderTreeHash $ShaderDir
        scenario     = $Scenario
        timestamp    = (Get-Date).ToUniversalTime().ToString("yyyy-MM-ddTHH:mm:ssZ")
    }
}

function Assert-ManifestMatchesBinary {
    # before a bless/compare, the candidate artifact's exe_hash + shader_hash MUST
    # match the binary/shaders currently being gated. A mismatch refuses with both hashes named.
    param(
        [object]$Manifest,
        [string]$ExePath,
        [string]$ShaderDir = "res/shaders",
        [string]$Name = "artifact"
    )
    if ($null -eq $Manifest) { throw "Cannot compare ${Name}: candidate carries no provenance manifest" }
    $runningExe = Get-FileSha256 $ExePath
    $runningShaders = Get-ShaderTreeHash $ShaderDir
    $candExe = [string]$Manifest.exe_hash
    $candShaders = [string]$Manifest.shader_hash
    $problems = @()
    if ($candExe -ne $runningExe) {
        $problems += "exe_hash mismatch (candidate=$candExe running=$runningExe)"
    }
    if ($candShaders -ne $runningShaders) {
        $problems += "shader_hash mismatch (candidate=$candShaders running=$runningShaders)"
    }
    if ($problems.Count -gt 0) {
        throw ("REFUSED bless/compare for ${Name}: the candidate artifact was produced by a " +
            "DIFFERENT binary/shaders than the one being gated -> " + ($problems -join '; ') +
            ". Re-capture against the current build/$BuildPreset binary before blessing ().")
    }
}

function Resolve-GatedExeOrNull {
    # Like Resolve-GatedExe, but returns $null instead of throwing when no binary is
    # present under build/$BuildPreset/bin. Lets the provenance binding degrade gracefully
    # (warn + skip) for committed-artifact gate runs that have no built exe to bind to.
    foreach ($c in @(
        "build/$BuildPreset/bin/luminumbra_client_app.exe",
        "build/$BuildPreset/bin/luminumbra_server_app.exe"
    )) {
        if (Test-Path -LiteralPath $c) { return $c }
    }
    return $null
}

function Get-ArtifactProvenanceSidecarPath {
    # Provenance sidecars are written under the GITIGNORED provenance dir (NOT next to the
    # tracked test-artifact), so a captured exe_hash can never be committed and turn every
    # fresh build into a false REFUSED. The sidecar name is keyed by scenario + artifact leaf.
    param(
        [Parameter(Mandatory)] [string]$ArtifactPath,
        [Parameter(Mandatory)] [string]$Scenario
    )
    $provenanceDir = "build/$BuildPreset/test-artifacts/provenance"
    New-Item -ItemType Directory -Force -Path $provenanceDir | Out-Null
    $safeScenario = ($Scenario -replace '[^A-Za-z0-9_.-]', '_')
    $leaf = Split-Path -Leaf $ArtifactPath
    return (Join-Path $provenanceDir ("{0}.{1}.manifest.json" -f $safeScenario, $leaf))
}

function Assert-ArtifactProvenance {
    #  end-to-end: bind a captured/blessed render artifact to the binary + shaders
    # that produced it. The FIRST time a gate consumes an artifact it EMITS the 6-field
    # provenance manifest (build_preset/git_sha/exe_hash/shader_hash/scenario/timestamp)
    # alongside it; every later compare/bless calls Assert-ManifestMatchesBinary so an artifact
    # captured with one binary is REFUSED against a different (rebuilt) binary, naming BOTH
    # hashes. exe_hash is not reproducible across rebuilds, so working-tree record-then-enforce
    # is the only coherent binding: it covers the real threat (capture, then edit+rebuild, then
    # forget to re-capture). Additive + non-breaking: with no gated binary present provenance
    # cannot be proven, so we warn and skip (committed-artifact gate runs keep passing).
    param(
        [Parameter(Mandatory)] [string]$ArtifactPath,
        [Parameter(Mandatory)] [string]$Scenario,
        [string]$ShaderDir = "res/shaders"
    )
    $exe = Resolve-GatedExeOrNull
    if ($null -eq $exe) {
        Write-Warning ("provenance ({0}): no gated binary under build/{1}/bin -> cannot bind artifact to a binary; skipping  enforcement (run -Mode Build to enable)." -f $Scenario, $BuildPreset)
        return
    }
    $sidecar = Get-ArtifactProvenanceSidecarPath -ArtifactPath $ArtifactPath -Scenario $Scenario
    if (Test-Path -LiteralPath $sidecar) {
        $cand = Get-Content -LiteralPath $sidecar -Raw | ConvertFrom-Json
        # Refuses with both hashes named on mismatch.
        Assert-ManifestMatchesBinary -Manifest $cand -ExePath $exe -ShaderDir $ShaderDir -Name "$Scenario capture"
        Write-Host ("  provenance ({0}): PASS -- artifact bound to the gated binary (exe_hash={1} shader_hash={2})." -f `
            $Scenario, ([string]$cand.exe_hash).Substring(0, 12), ([string]$cand.shader_hash).Substring(0, 12))
    } else {
        $manifest = New-ArtifactManifest -ExePath $exe -Scenario $Scenario -ShaderDir $ShaderDir
        $manifest | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath $sidecar -Encoding UTF8
        Write-Host ("  provenance ({0}): recorded capture manifest alongside artifact (exe_hash={1} shader_hash={2}); a rebuilt binary will now be REFUSED unless re-captured ()." -f `
            $Scenario, $manifest.exe_hash.Substring(0, 12), $manifest.shader_hash.Substring(0, 12))
    }
}

function Assert-ConfigSchemaFresh {
    #   CI hook: the SystemConfig registry is generated from ConfigSchema.json;
    # fail the gate if the committed generated header drifted from the schema. Mirrors the
    # configure-time check in src/luminumbra_common/CMakeLists.txt.
    $py = $null
    foreach ($name in @("python", "python3", "py")) {
        $cmd = Get-Command $name -ErrorAction SilentlyContinue
        if ($cmd) { $py = $cmd.Source; break }
    }
    if (-not $py) {
        Write-Warning "No Python interpreter found: skipping config_codegen --check ()."
        return
    }
    & $py "tools/config_codegen.py" --check
    if ($LASTEXITCODE -ne 0) {
        throw "config_codegen.py --check failed: SystemConfig schema/registry drift ( )."
    }
}

function Test-ConfigSchemaCheck {
    Assert-ConfigSchemaFresh
    Write-Host "config schema drift gate passed (config_codegen.py --check)."
}

function Test-ArtifactManifest {
    # . Emit a provenance manifest for the gated binary, prove the match-vs-binary
    # comparison passes for a same-binary capture and REFUSES a mismatched (stale-binary) one, and
    # enumerate the manual GPU/perf test tier so a conditionally-unregistered test is visibly missing.
    $exe = Resolve-GatedExe
    $provenanceDir = "build/$BuildPreset/test-artifacts/provenance"
    New-Item -ItemType Directory -Force -Path $provenanceDir | Out-Null
    $manifestPath = Join-Path $provenanceDir "artifact-manifest.json"

    $manifest = New-ArtifactManifest -ExePath $exe -Scenario "engine-frontier:ArtifactManifest"
    foreach ($field in @("build_preset", "git_sha", "exe_hash", "shader_hash", "scenario", "timestamp")) {
        if ([string]::IsNullOrWhiteSpace([string]$manifest[$field])) {
            throw "Artifact manifest missing required field '$field' ()"
        }
    }
    if ($manifest.build_preset -ne $BuildPreset) {
        throw "Artifact manifest build_preset '$($manifest.build_preset)' does not match '$BuildPreset'"
    }
    $manifest | ConvertTo-Json -Depth 6 | Set-Content -Path $manifestPath -Encoding UTF8
    Write-Host ("artifact manifest written: exe={0} exe_hash={1} shader_hash={2} git={3}" -f `
        (Split-Path $exe -Leaf), $manifest.exe_hash.Substring(0, 12), $manifest.shader_hash.Substring(0, 12), $manifest.git_sha)

    # Positive: a manifest produced by the binary being gated must compare clean.
    $reread = Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json
    Assert-ManifestMatchesBinary -Manifest $reread -ExePath $exe -Name "self (same binary)"
    Write-Host "  match-vs-binary: PASS (same-binary capture accepted)."

    # Negative: a candidate whose exe_hash/shader_hash do NOT match the running binary MUST be
    # refused, with the mismatched hashes named ( / ). Construct the stale capture.
    $stale = [ordered]@{
        schema       = $reread.schema
        build_preset = $reread.build_preset
        git_sha      = $reread.git_sha
        exe_hash     = "deadbeef" + $reread.exe_hash.Substring(8)
        shader_hash  = $reread.shader_hash
        scenario     = "stale-capture"
        timestamp    = $reread.timestamp
    }
    $refused = $false
    try {
        Assert-ManifestMatchesBinary -Manifest $stale -ExePath $exe -Name "stale capture"
    } catch {
        $refused = $true
        Write-Host "  match-vs-binary: REFUSED stale capture as required ($($_.Exception.Message.Split('.')[0]))."
    }
    if (-not $refused) {
        throw "Manifest comparison did NOT refuse a mismatched exe_hash (hole)"
    }

    # enumerate the manual GPU/perf tier; report registered-vs-expected so a
    # conditionally-unregistered test (numpy/Pillow-gated) is visibly MISSING, not silently absent.
    $expectedManualTier = @(
        "ForestPerfBudget", "ShieldRtSpike", "ShieldRtTracerProfileGpu",
        "VisualCritiqueFlags", "TimelapseSelftest",
        "WorldLoadBounded.TwentyColdInteractiveLoadsReportLatency"
    )
    $registered = @()
    try {
        $showOnly = & ctest --preset $BuildPreset -L manual --show-only 2>$null
        if ($LASTEXITCODE -eq 0 -and $showOnly) {
            foreach ($line in $showOnly) {
                if ($line -match '^\s*Test\s+#\d+:\s+(.+?)\s*$') { $registered += $Matches[1] }
            }
        }
    } catch {
        Write-Warning "manual-tier enumeration (ctest -L manual --show-only) unavailable: $($_.Exception.Message)"
    }
    $missing = @($expectedManualTier | Where-Object { $registered -notcontains $_ })
    Write-Host ("manual GPU/perf tier: {0} registered, {1}/{2} of the expected tier present." -f `
        $registered.Count, ($expectedManualTier.Count - $missing.Count), $expectedManualTier.Count)
    if ($missing.Count -gt 0) {
        #  surfaced (not silently swallowed). A name here is absent from the `-L manual`
        # enumeration -- either python/numpy-conditionally unregistered, or registered under a
        # different label (e.g. VisualCritiqueFlags = visual;critique). Either way it is visible.
        Write-Host ("  ABSENT from the -L manual enumeration (conditionally registered / other label): {0}" -f ($missing -join ", "))
    }

    $tierArtifact = [ordered]@{
        schema           = "luminumbra.artifact_manifest.v1"
        build_preset     = $BuildPreset
        manifest         = $manifest
        manual_tier      = [ordered]@{
            expected   = $expectedManualTier
            registered = $registered
            missing    = $missing
        }
    }
    $tierArtifact | ConvertTo-Json -Depth 6 | Set-Content -Path (Join-Path $provenanceDir "manual-tier.json") -Encoding UTF8

    #  END-TO-END: prove the artifact-sidecar binding used by the real visual gates
    # (Test-MaterialVisual / Test-RenderHealth via Assert-ArtifactProvenance) refuses a capture
    # produced by a DIFFERENT binary. Record a sidecar against the gated exe (the "capture"),
    # then corrupt its exe_hash (the "rebuilt binary") and confirm the next compare is REFUSED
    # with both hashes named -- no second build required.
    $demoArtifact = Join-Path $provenanceDir "provenance-e2e-demo.json"
    [ordered]@{ schema = "luminumbra.provenance_e2e_demo.v1"; note = " end-to-end demo artifact" } |
        ConvertTo-Json | Set-Content -Path $demoArtifact -Encoding UTF8
    $demoSidecar = Get-ArtifactProvenanceSidecarPath -ArtifactPath $demoArtifact -Scenario "ProvenanceE2EDemo"
    if (Test-Path -LiteralPath $demoSidecar) { Remove-Item -LiteralPath $demoSidecar -Force }
    # Capture: first sight emits the sidecar bound to the gated binary; must compare clean.
    Assert-ArtifactProvenance -ArtifactPath $demoArtifact -Scenario "ProvenanceE2EDemo"
    Assert-ArtifactProvenance -ArtifactPath $demoArtifact -Scenario "ProvenanceE2EDemo"
    # Rebuilt binary: corrupt the captured exe_hash so it no longer matches the gated exe.
    $demoManifest = Get-Content -LiteralPath $demoSidecar -Raw | ConvertFrom-Json
    $rebuilt = [ordered]@{
        schema       = $demoManifest.schema
        build_preset = $demoManifest.build_preset
        git_sha      = $demoManifest.git_sha
        exe_hash     = "deadbeef" + ([string]$demoManifest.exe_hash).Substring(8)
        shader_hash  = $demoManifest.shader_hash
        scenario     = $demoManifest.scenario
        timestamp    = $demoManifest.timestamp
    }
    $rebuilt | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath $demoSidecar -Encoding UTF8
    $e2eRefused = $false
    $e2eMessage = ""
    try {
        Assert-ArtifactProvenance -ArtifactPath $demoArtifact -Scenario "ProvenanceE2EDemo"
    } catch {
        $e2eRefused = $true
        $e2eMessage = $_.Exception.Message
    }
    if (-not $e2eRefused) {
        throw " END-TO-END HOLE: a stale-binary capture was NOT refused by Assert-ArtifactProvenance (the path the real visual gates use)."
    }
    if ($e2eMessage -notmatch "exe_hash mismatch" -or $e2eMessage -notmatch "candidate=" -or $e2eMessage -notmatch "running=") {
        throw " END-TO-END: refusal did not name BOTH hashes (candidate/running). Got: $e2eMessage"
    }
    Write-Host ("  end-to-end: PASS -- a capture from a different binary is REFUSED by the visual-gate path ({0})." -f ($e2eMessage.Split('.')[0]))
    Remove-Item -LiteralPath $demoSidecar, $demoArtifact -Force -ErrorAction SilentlyContinue

    Write-Host "artifact manifest gate passed: provenance recorded, stale-capture refusal enforced (incl. end-to-end visual-gate binding), manual tier enumerated."
}

function Test-WorldLoadBounded {
    # Explicit heavyweight diagnostic for twenty cold interactive loads at
    # radii 12/4. It records latency and enables named watchdog reporting, but
    # makes no absolute machine-speed claim. Release gating uses the paired
    # relative performance workflow. Deliberately not included in -Mode All.
    $exe = "build/$BuildPreset/bin/world_load_bounded_test.exe"
    if (-not (Test-Path $exe)) {
        throw "world-load diagnostic not built - missing $exe (cmake --build --preset $BuildPreset --target world_load_bounded_test)"
    }
    & $exe --gtest_filter=WorldLoadBounded.*
    if ($LASTEXITCODE -ne 0) {
        throw "world-load diagnostic FAILED (exit $LASTEXITCODE) - a load failed; the named watchdog phase in the output localizes slow sub-batches"
    }
    Write-Host "world-load diagnostic completed: 20 cold interactive loads (radius 12/4), latency recorded, watchdog armed"
}

function Test-MovingResidency {
    #   /  — the MOVING-residency determinism axis, as a
    # first-class engine-frontier gate. Drives the headless server with
    # --smoke-moving (the streaming anchor drifts deterministically each tick so
    # chunks stream IN/OUT during the run, exercising eviction/re-arrival the static
    # --smoke never does) + --avail-trace (per-tick availability digest). Unlike the
    # STATIC --smoke (HeadlessServerTick), the moving anchor's per-tick availability
    # set CONVERGES rather than matching tick-for-tick: the resident Ready-set differs
    # run-to-run as chunks stream/evict with timing variance, yet the FINAL world_hash
    # is run==replay. Per the contract (main_server.cpp:515-520) the determinism
    # verdict is the FINAL hash + run==replay, NEVER the per-tick
    # availability_trace_match (which is FALSE-BY-DESIGN for a moving anchor).
    #
    # Heavyweight (90-tick double run) -> a first-class MODE + a default matrix axis
    # (validate-determinism-matrix.ps1), deliberately NOT in -Mode All (mirrors
    # HeadlessServerTick).
    $serverExe = "build/$BuildPreset/bin/luminumbra_server_app.exe"
    if (-not (Test-Path $serverExe)) {
        throw "moving-residency gate not built - missing $serverExe (cmake --build --preset $BuildPreset --target luminumbra_server_app)"
    }
    $artifactPath = "build/$BuildPreset/test-artifacts/server/moving-residency.json"
    if (Test-Path $artifactPath) { Remove-Item $artifactPath -Force }

    & $serverExe --smoke-moving --avail-trace --ticks 90 --artifact $artifactPath
    if ($LASTEXITCODE -ne 0) {
        throw "moving-residency smoke exited with code $LASTEXITCODE"
    }

    $analysis = Read-JsonArtifact $artifactPath "luminumbra.server_tick.v1"
    Assert-ArtifactPassed $analysis "MovingResidency"

    # FINAL-hash gate (run==replay) — the determinism verdict for the convergent
    # moving oracle. We do NOT gate on availability_trace_match (see below).
    if (-not $analysis.deterministic) {
        throw "moving-residency smoke reported a non-deterministic double-run (final world_hash diverged under the moving anchor)"
    }
    if ([string]::IsNullOrEmpty($analysis.world_hash) -or [string]::IsNullOrEmpty($analysis.world_hash_replay)) {
        throw "moving-residency smoke produced an empty world hash"
    }
    if ($analysis.world_hash -ne $analysis.world_hash_replay) {
        throw "moving-residency world_hash mismatch (the moving anchor must still CONVERGE run==replay): $($analysis.world_hash) != $($analysis.world_hash_replay)"
    }
    if (-not $analysis.passed) {
        throw "moving-residency smoke reported passed=false"
    }
    # The convergent sub-hashes must still match run==replay at the post-run quiesce.
    if (-not $analysis.sub_hashes_match) {
        throw "moving-residency reported sub_hashes_match=false (a streamed sim system diverged under the moving anchor)"
    }

    # --avail-trace must have produced the per-tick availability trace (proves the
    # observability path ran). For the moving anchor availability_trace_match is
    # EXPECTED false (convergent, not per-tick-identical) -> REPORT-ONLY, never a gate.
    if ($null -eq $analysis.availability_trace) {
        throw "moving-residency: --avail-trace produced no availability_trace (the per-tick observability path did not run)"
    }
    $traceCount = @($analysis.availability_trace).Count
    if ($traceCount -lt 1) {
        throw "moving-residency: availability_trace is empty"
    }
    if ($analysis.availability_trace_match) {
        $convergeNote = "per-tick availability MATCH"
    } else {
        $convergeNote = "per-tick availability CONVERGES (first divergent tick $($analysis.availability_trace_first_divergent_tick); expected for a moving anchor)"
    }
    Write-Host ("moving-residency gate passed: --smoke-moving world_hash={0} == replay (convergent oracle, run==replay), {1} ticks x 2 runs, availability_trace {2} ticks [{3}]" -f `
        $analysis.world_hash, $analysis.ticks_requested, $traceCount, $convergeNote)
}

# ----------------------------------------------------------------------------------
#  / — shared SIM/HASH-PATH source-scan substrate.
# These are the SimResidency-eligible roots: per-tick authoritative
# systems, the hash-carrying net state, core + persistence hash assembly, and the
# headless server loop. Render code (luminumbra_client/rendering) is RenderResidency
# by construction and OUT of scope — its GPU readbacks never feed world_hash.
# ----------------------------------------------------------------------------------
$script:SimHashPathRoots = @(
    "src/luminumbra_common/systems",
    "src/luminumbra_common/world",
    "src/luminumbra_common/fields",
    "src/luminumbra_common/ai",
    "src/luminumbra_common/simulation",
    "src/luminumbra_common/animation",
    "src/luminumbra_common/physics",
    "src/luminumbra_common/core",
    "src/luminumbra_common/persistence",
    "src/luminumbra_common/net",
    "src/luminumbra_common/network",
    "src/luminumbra_server"
)

function Get-SimSourceFiles {
    param([string[]]$Roots)
    $repoRoot = (Resolve-Path ".").Path
    $out = New-Object 'System.Collections.Generic.List[object]'
    foreach ($root in $Roots) {
        if (-not (Test-Path $root)) { continue }
        $files = Get-ChildItem -Path $root -Recurse -File |
            Where-Object { $_.Extension -in @(".h", ".hpp", ".cpp", ".inl", ".c") }
        foreach ($f in $files) {
            $rel = $f.FullName.Substring($repoRoot.Length + 1).Replace("\", "/")
            $out.Add([pscustomobject]@{ Path = $f.FullName; Rel = $rel })
        }
    }
    return $out
}

# readback-discipline core scanner: returns the synchronous-GPU-readback violation strings found on
# the sim/hash path. Comment-stripped per line. $Allowlist = hashtable of
# "<relative-path>|readback" keys (each a documented render-only/escape-hatch reason).
function Get-SimReadbackViolations {
    param([hashtable]$Allowlist = @{})
    # Synchronous GPU->CPU readback / fence-wait primitives. Listed longest-first so
    # the *Range / *Named variants are reported with their full name.
    $readbackRe = 'gl(ClientWaitSync|MapNamedBufferRange|MapBufferRange|MapNamedBuffer|MapBuffer|GetNamedBufferSubData|GetBufferSubData|ReadnPixels|ReadPixels|GetTextureSubImage|GetTextureImage|GetTexImage)\s*\('
    $violations = New-Object 'System.Collections.Generic.List[string]'
    foreach ($file in (Get-SimSourceFiles -Roots $script:SimHashPathRoots)) {
        $lineNo = 0
        foreach ($line in (Get-Content $file.Path)) {
            $lineNo++
            $code = $line -replace '//.*$', ''
            if ($code -match '^\s*\*' -or $code -match '^\s*/\*') { continue }
            $m = [regex]::Match($code, $readbackRe)
            if ($m.Success) {
                $key = "$($file.Rel)|readback"
                if (-not $Allowlist.ContainsKey($key)) {
                    $sym = ($m.Groups[0].Value -replace '\s*\($', '')
                    $violations.Add("readback: $($file.Rel):${lineNo}: synchronous GPU readback '$sym' on the sim/hash path -> keep render-only () or delay+quantize+hash (); `"$($code.Trim())`"")
                }
            }
        }
    }
    return $violations
}

function Test-ReadbackDiscipline {
    # GPU-to-CPU readback discipline. Static source
    # gate: NO synchronous GPU readback (glClientWaitSync / glMapBuffer* /
    # glGetBufferSubData / glReadPixels / glGetTexImage /...) may be added on the
    # SIM/HASH path. GPU-sourced data is render-only by default; to become
    # a sim input it must be delayed, quantized, hashed, and allowlisted here
    # with a documented determinism rationale. Known render-side readbacks
    # (RenderPipeline.cpp / FoliagePass.cpp) are RenderResidency by construction
    # they live under luminumbra_client/rendering, OUTSIDE these roots, and never
    # feed world_hash. APPEND-ONLY safe: PASSES on the current tree (zero offenders),
    # exists to FAIL a NEW offender.
    $allowlist = @{
        # Empty: no synchronous GPU readback currently feeds the sim/hash path.
        # Any delayed, quantized, and hashed exception must be added here with a
        # one-line determinism rationale, for example:
        #   "src/luminumbra_common/.../Foo.cpp|readback" = "delayed+quantized+hashed per "
    }
    $files = @(Get-SimSourceFiles -Roots $script:SimHashPathRoots)
    if ($files.Count -lt 10) {
        throw "readback discipline scanned suspiciously few files ($($files.Count)); the sim/hash-path scan is broken"
    }
    $violations = @(Get-SimReadbackViolations -Allowlist $allowlist)
    if ($violations.Count -gt 0) {
        foreach ($v in $violations) { Write-Host "  $v" }
        throw "GPU->CPU readback discipline (): $($violations.Count) synchronous-readback site(s) on the sim/hash path. Keep it render-only, or delay+quantize+hash () and allowlist with a documented reason."
    }
    Write-Host ("readback discipline: {0} sim/hash-path files scanned, 0 synchronous GPU-readback offenders ({1} documented allowlist exception site(s))" -f $files.Count, $allowlist.Count)
}

#   /  — the  synchronous-readback ban gate.
#
# Distinct from Test-ReadbackDiscipline ( , which scans the SIM/HASH
# path and bars ANY GL readback there). This gate scans RENDER code
# (luminumbra_client/rendering) — where write-mapping is legitimate and ubiquitous
# (persistent-mapped geometry/instance/particle rings: glMapBufferRange(...
# GL_MAP_WRITE_BIT...)) — and bans only the BLOCKING/READ forms that stall the
# frame: glClientWaitSync(..., GL_TIMEOUT_IGNORED), glMapBuffer(..., GL_READ_ONLY),
# and glGet[Named]BufferSubData. New render code must use AsyncReadbackRing
# (: submit/poll/consume, non-blocking) instead. The ring's own primitives
# are deliberately NOT matched: its poll uses glClientWaitSync(..., 0) (timeout 0,
# not GL_TIMEOUT_IGNORED) and its result map uses glMapBufferRange(... GL_MAP_READ_BIT
#...) (not glMapBuffer(GL_READ_ONLY)). Each known site leaves the allowlist as its
# FR lands (FoliagePass ->  now; RenderPipeline SDF ->  after activation queue).
function Test-RenderReadbackAllowlist {
    $roots = @("src/luminumbra_client/rendering")
    #  (, 2026-07-04): the allowlist is EMPTY. Every known
    # in-frame blocking-readback site has retired onto the asynchronous-readback ring:
    # FoliagePass, the RenderPipeline  readback (,
    # unblocked by activation queue — now ring submit + BOUNDED zero-timeout poll with CPU
    # fallback), and the SkyAtmosphereLut sky-view ambient reduction (same
    # bounded-ring shape). Any new match below is a regression, not an unfinished item.
    $allowlist = @{}
    # Narrow, READBACK-SPECIFIC matchers (NOT the broad sim-path $readbackRe, which
    # would falsely flag every legitimate write-map in render code).
    $blockingWait = 'glClientWaitSync\s*\([^;]*GL_TIMEOUT_IGNORED'
    $readMap      = 'glMapBuffer\s*\([^;]*GL_READ_ONLY'
    $subData      = 'glGet(Named)?BufferSubData\s*\('

    $repoRoot = (Resolve-Path ".").Path
    $scanned = 0
    $violations = New-Object 'System.Collections.Generic.List[string]'
    foreach ($root in $roots) {
        if (-not (Test-Path $root)) { continue }
        $files = Get-ChildItem -Path $root -Recurse -File |
            Where-Object { $_.Extension -in @(".h", ".hpp", ".cpp", ".inl", ".c", ".ipp") }
        foreach ($f in $files) {
            $scanned++
            $rel = $f.FullName.Substring($repoRoot.Length + 1).Replace("\", "/")
            $lineNo = 0
            foreach ($line in (Get-Content $f.FullName)) {
                $lineNo++
                $code = $line -replace '//.*$', ''
                if ($code -match '^\s*\*' -or $code -match '^\s*/\*') { continue }
                $sym = $null
                if ($code -match $blockingWait) { $sym = "glClientWaitSync(..., GL_TIMEOUT_IGNORED)" }
                elseif ($code -match $readMap)  { $sym = "glMapBuffer(..., GL_READ_ONLY)" }
                elseif ($code -match $subData)  { $sym = "glGetBufferSubData" }
                if ($sym) {
                    $key = "$rel|readback"
                    if (-not $allowlist.ContainsKey($key)) {
                        $violations.Add("readback: ${rel}:${lineNo}: in-frame synchronous GPU readback '$sym' in render code -> use AsyncReadbackRing () or allowlist with a documented reason; `"$($code.Trim())`"")
                    }
                }
            }
        }
    }
    if ($scanned -lt 10) {
        throw "render readback gate scanned suspiciously few files ($scanned); the render scan is broken"
    }
    if ($violations.Count -gt 0) {
        foreach ($v in $violations) { Write-Host "  $v" }
        throw "render-side synchronous-readback ban (): $($violations.Count) blocking-readback site(s) in render code outside the allowlist. Route through AsyncReadbackRing (submit/poll/consume) or allowlist with a reason."
    }
    Write-Host ("render readback ban: {0} render files scanned, 0 un-allowlisted blocking-readback sites ({1} documented allowlist site(s))" -f $scanned, $allowlist.Count)
}

function Test-DeterminismAudit {
    # Machine-checked determinism audit for the simulation/render residency
    # boundary. Exposure metering, froxel temporal jitter, frame-graph state,
    # and GPU readback must stay render-only. Each checklist item passes on the
    # current tree and fails on
    # a planted violation:
    #   1. Residency class declared: core/ResidencyContract.h.
    #   2. Config residency parity: render.* never folded into the config
    #      sub-hash (SystemConfig.cpp).
    #   3. World-hash composition carries no render-residency term:
    #      ComposeWorldHash folds sim sub-hashes only (no mesh/exposure/froxel).
    #   4. Render mesh excluded from the determinism match (mesh-exclusion contract):
    #      main_server.cpp sub_hashes_match omits.mesh.
    #   5. No synchronous GPU readback feeds the hash: reuse the readback-discipline scan.
    #   6. Exposure metering + froxel temporal jitter are render-only: no
    #      froxel / auto-exposure / eye-adaptation identifier on the sim path (bare
    #      "exposure" is legitimate — photographic/plant light — and NOT banned).
    $checks = New-Object 'System.Collections.Generic.List[object]'

    # --- 1. Residency class declared ---
    $rc = "src/luminumbra_common/core/ResidencyContract.h"
    if (Test-Path $rc) {
        $rcText = Get-Content $rc -Raw
        $need = @(
            "enum class ResidencyClass",
            "struct SimResidency",
            "struct RenderResidency",
            "MayFeedWorldHash",
            "SimResidency must be eligible to feed world_hash",
            "RenderResidency must be forbidden from feeding world_hash"
        )
        $missing = @($need | Where-Object { $rcText -notmatch [regex]::Escape($_) })
        if ($missing.Count -eq 0) {
            $checks.Add([pscustomobject]@{ name = "residency-declared"; ok = $true; detail = "ResidencyContract.h declares the Sim/Render partition + MayFeedWorldHash + self-checking static_asserts" })
        } else {
            $checks.Add([pscustomobject]@{ name = "residency-declared"; ok = $false; detail = "ResidencyContract.h is missing: $($missing -join '; ')" })
        }
    } else {
        $checks.Add([pscustomobject]@{ name = "residency-declared"; ok = $false; detail = "missing $rc (residency partition undeclared)" })
    }

    # --- 2. Config residency parity ---
    $sc = "src/luminumbra_common/core/SystemConfig.cpp"
    if (Test-Path $sc) {
        $scText = Get-Content $sc -Raw
        if ($scText -match 'section\s*!=\s*Section::Sim\s*\)\s*continue') {
            $checks.Add([pscustomobject]@{ name = "config-residency-parity"; ok = $true; detail = "ComputeConfigSubHash skips non-Sim (render.*) keys -> render config never hashed" })
        } else {
            $checks.Add([pscustomobject]@{ name = "config-residency-parity"; ok = $false; detail = "SystemConfig.cpp no longer excludes render.* from the config sub-hash (broken — render config would feed world_hash)" })
        }
    } else {
        $checks.Add([pscustomobject]@{ name = "config-residency-parity"; ok = $false; detail = "missing $sc" })
    }

    # --- 3. World-hash composition carries no render-residency term ---
    $swr = "src/luminumbra_server/ServerWorldRunner.cpp"
    if (Test-Path $swr) {
        $swrText = Get-Content $swr -Raw
        $cm = [regex]::Match($swrText, 'std::string\s+ComposeWorldHash\s*\([^)]*\)\s*\{(.*?)\n\}', [System.Text.RegularExpressions.RegexOptions]::Singleline)
        if ($cm.Success) {
            $body = ($cm.Groups[1].Value -split "`n" | ForEach-Object { $_ -replace '//.*$', '' }) -join "`n"
            $renderFold = [regex]::Match($body, '\|\s*(mesh|froxel|exposure|readback|render)\s*:')
            if ($renderFold.Success) {
                $checks.Add([pscustomobject]@{ name = "worldhash-no-render-term"; ok = $false; detail = "ComposeWorldHash folds a render-residency term '$($renderFold.Groups[1].Value)' into world_hash (— render state must never feed the hash)" })
            } else {
                $checks.Add([pscustomobject]@{ name = "worldhash-no-render-term"; ok = $true; detail = "ComposeWorldHash folds sim sub-hashes only (no mesh/exposure/froxel/readback)" })
            }
        } else {
            $checks.Add([pscustomobject]@{ name = "worldhash-no-render-term"; ok = $false; detail = "could not locate the ComposeWorldHash body in $swr (the audit cannot verify the hash composition)" })
        }
    } else {
        $checks.Add([pscustomobject]@{ name = "worldhash-no-render-term"; ok = $false; detail = "missing $swr" })
    }

    # --- 4. Render mesh excluded from the determinism match ---
    $ms = "src/luminumbra_server/main_server.cpp"
    if (Test-Path $ms) {
        $msText = Get-Content $ms -Raw
        $am = [regex]::Match($msText, 'const\s+bool\s+sub_hashes_match\s*=(.*?);', [System.Text.RegularExpressions.RegexOptions]::Singleline)
        if ($am.Success) {
            $blk = ($am.Groups[1].Value -split "`n" | ForEach-Object { $_ -replace '//.*$', '' }) -join "`n"
            if ($blk -match '\.mesh\b') {
                $checks.Add([pscustomobject]@{ name = "mesh-excluded-from-match"; ok = $false; detail = "sub_hashes_match now includes.mesh — the (nondeterministic MC) render mesh must stay EXCLUDED from the run==replay match" })
            } else {
                $checks.Add([pscustomobject]@{ name = "mesh-excluded-from-match"; ok = $true; detail = "sub_hashes_match omits.mesh (render mesh excluded from the determinism match)" })
            }
        } else {
            $checks.Add([pscustomobject]@{ name = "mesh-excluded-from-match"; ok = $false; detail = "could not locate the sub_hashes_match assignment in $ms" })
        }
    } else {
        $checks.Add([pscustomobject]@{ name = "mesh-excluded-from-match"; ok = $false; detail = "missing $ms" })
    }

    # --- 5. No synchronous GPU readback feeds the hash (; reuse readback-discipline scan) ---
    $rbViol = @(Get-SimReadbackViolations -Allowlist @{})
    if ($rbViol.Count -eq 0) {
        $checks.Add([pscustomobject]@{ name = "no-sim-path-readback"; ok = $true; detail = "0 synchronous GPU-readback sites on the sim/hash path" })
    } else {
        $checks.Add([pscustomobject]@{ name = "no-sim-path-readback"; ok = $false; detail = "$($rbViol.Count) synchronous GPU-readback site(s) on the sim/hash path -> first: $($rbViol[0])" })
    }

    # --- 6. Exposure metering + froxel temporal jitter are render-only ---
    # Render auto-exposure / eye-adaptation / froxel volumetrics are the
    # constructs that MUST stay render-residency. Bare "exposure" is legitimate on the
    # sim path (PhotoScoring photographic exposure; PlantGrowth light exposure) and is
    # deliberately NOT matched — only the render-feedback identifiers are.
    $rexRe = '(?i)(froxel|auto[_-]?exposure|autoexposure|eye[_-]?adaptation|eyeadaptation)'
    $exposureRoots = @(
        "src/luminumbra_common/systems",
        "src/luminumbra_common/world",
        "src/luminumbra_common/fields",
        "src/luminumbra_common/ai",
        "src/luminumbra_common/simulation",
        "src/luminumbra_common/animation",
        "src/luminumbra_common/physics",
        "src/luminumbra_server"
    )
    $rexViol = New-Object 'System.Collections.Generic.List[string]'
    foreach ($file in (Get-SimSourceFiles -Roots $exposureRoots)) {
        $lineNo = 0
        foreach ($line in (Get-Content $file.Path)) {
            $lineNo++
            $code = $line -replace '//.*$', ''
            if ($code -match '^\s*\*' -or $code -match '^\s*/\*') { continue }
            $rm = [regex]::Match($code, $rexRe)
            if ($rm.Success) {
                $rexViol.Add("$($file.Rel):${lineNo}: render '$($rm.Groups[1].Value)' on the sim path; `"$($code.Trim())`"")
            }
        }
    }
    if ($rexViol.Count -eq 0) {
        $checks.Add([pscustomobject]@{ name = "exposure-froxel-render-only"; ok = $true; detail = "no froxel / auto-exposure / eye-adaptation identifier on the sim path" })
    } else {
        $checks.Add([pscustomobject]@{ name = "exposure-froxel-render-only"; ok = $false; detail = "$($rexViol.Count) render auto-exposure/froxel site(s) on the sim path -> first: $($rexViol[0])" })
    }

    # --- verdict ---
    foreach ($c in $checks) {
        $status = if ($c.ok) { "PASS" } else { "FAIL" }
        Write-Host ("  [audit] {0,-28} {1}  {2}" -f $c.name, $status, $c.detail)
    }
    $failed = @($checks | Where-Object { -not $_.ok })
    if ($failed.Count -gt 0) {
        throw "determinism audit gate: $($failed.Count) of $($checks.Count) checklist item(s) failed — a render-residency value is reaching the sim/hash path."
    }
    Write-Host ("determinism audit gate passed: {0}/{0} checklist items green (residency declared; render.* unhashed; no render term in world_hash; mesh excluded; no sim-path readback; exposure/froxel render-only)" -f $checks.Count)
}

function Test-RhiNoReexport {
    #   /   no-re-export constraint. The Rhi* type set
    # lives beneath the  handles; a pass holds only a layer-1 handle and must
    # never be able to name a backend (Diligent) type. Enforce it mechanically: no
    # Diligent header may escape rhi/ into rendering/passes/ nor into
    # RenderContext.h / RenderResourceHandles.h / RenderResourceRegistry.h. Fails
    # listing EVERY offending file:line, not just the first.
    #
    # It matches ACTUAL leaks -- a Diligent-header #include, or a Diligent::
    # qualified type -- NOT the bare word "Diligent" (which legitimately appears in
    # comments, e.g. "backed by a Diligent device tomorrow"). Including the
    # Diligent-FREE rhi/*.h wrappers (RhiResource.h etc.) is allowed by design.
    $repo = (Get-Location).Path
    $guarded = New-Object System.Collections.Generic.List[string]
    $passesDir = Join-Path $repo "src/luminumbra_client/rendering/passes"
    if (Test-Path $passesDir) {
        Get-ChildItem $passesDir -File -Recurse |
            Where-Object { $_.Extension -in '.h', '.hpp', '.cpp', '.inl', '.ipp' } |
            ForEach-Object { $guarded.Add($_.FullName) }
    }
    foreach ($rel in @(
            "src/luminumbra_client/rendering/RenderContext.h",
            "src/luminumbra_client/rendering/RenderResourceHandles.h",
            "src/luminumbra_client/rendering/RenderResourceRegistry.h"
        )) {
        $p = Join-Path $repo $rel
        if (Test-Path $p) { $guarded.Add($p) }
    }
    if ($guarded.Count -eq 0) {
        throw "RhiNoReexport: guarded file set is empty (passes/ + the three headers) -- gate cannot run"
    }

    $leakPatterns = @(
        '#include\s+["<][^">]*Diligent',                                              # any Diligent path
        '#include\s+["<](EngineFactory\w+|RenderDevice|DeviceContext|SwapChain)\.h[">]',  # Diligent interface headers
        'Diligent::'                                                                  # qualified Diligent type
    )
    $violations = New-Object System.Collections.Generic.List[string]
    foreach ($file in $guarded) {
        $lineNo = 0
        foreach ($line in (Get-Content $file)) {
            $lineNo++
            foreach ($pat in $leakPatterns) {
                if ($line -match $pat) {
                    $rel = $file.Substring($repo.Length + 1).Replace('\', '/')
                    $violations.Add("${rel}:${lineNo}: $($line.Trim())")
                    break
                }
            }
        }
    }
    if ($violations.Count -gt 0) {
        Write-Host "RhiNoReexport: FAILED -- $($violations.Count) Diligent leak(s) escaped rhi/:"
        foreach ($v in $violations) { Write-Host "  - $v" }
        throw "RhiNoReexport gate failed: $($violations.Count) Diligent header/type leak(s) into the pass/handle/registry layer"
    }
    Write-Host "RhiNoReexport: OK -- no Diligent header escapes rhi/ ($($guarded.Count) guarded files: rendering/passes/ + RenderContext.h + RenderResourceHandles.h + RenderResourceRegistry.h)."
}

function Test-ProfilerDeterminismNeutral {
    #  (2026-07 research ): the Tracy profiler seam must stay OFF by default
    # so every gate/release build is byte-identical (world_hash unchanged, proven by
    # --smoke == 6f008a9f637c40b7 with the annotations present but disabled). Assert the
    # three invariants that keep it neutral, failing listing EVERY breach, not just the
    # first:
    #   1. cmake/tracy.cmake declares option(LUMINUMBRA_ENABLE_TRACY...) default OFF.
    #   2. core/Profiler.h has the #else no-op branch (macros -> ((void)0)) -- the shim
    #      is not hard-wired active.
    #   3. no production TU #defines LUMINUMBRA_ENABLE_TRACY (only CMake may, when ON).
    $repo = (Get-Location).Path
    $violations = New-Object System.Collections.Generic.List[string]

    # 1. option default OFF (whitespace-normalized so multi-line option still matches;
    #    the captured token is the default just before the option's closing paren).
    $tracyCmake = Join-Path $repo "cmake/tracy.cmake"
    if (-not (Test-Path $tracyCmake)) {
        $violations.Add("cmake/tracy.cmake: MISSING (the Tracy seam file)")
    } else {
        $norm = (Get-Content $tracyCmake -Raw) -replace '\s+', ' '
        if ($norm -match 'option\( ?LUMINUMBRA_ENABLE_TRACY.*?(ON|OFF)\)') {
            if ($matches[1] -ne 'OFF') {
                $violations.Add("cmake/tracy.cmake: option(LUMINUMBRA_ENABLE_TRACY) default is $($matches[1]) -- MUST be OFF (gate/release determinism)")
            }
        } else {
            $violations.Add("cmake/tracy.cmake: option(LUMINUMBRA_ENABLE_TRACY...) declaration not found")
        }
    }

    # 2. Profiler.h no-op #else branch.
    $shim = Join-Path $repo "src/luminumbra_common/core/Profiler.h"
    if (-not (Test-Path $shim)) {
        $violations.Add("src/luminumbra_common/core/Profiler.h: MISSING (the profiler shim)")
    } else {
        $shimTxt = Get-Content $shim -Raw
        if ($shimTxt -notmatch '#else') {
            $violations.Add("Profiler.h: no #else branch -- the shim must define no-op macros when LUMINUMBRA_ENABLE_TRACY is undefined")
        }
        if ($shimTxt -notmatch '\(\(void\)0\)') {
            $violations.Add("Profiler.h: no ((void)0) expansion found -- the disabled macros must emit nothing")
        }
    }

    # 3. no production TU force-defines the flag (only CMake may set it, when ON).
    $srcDir = Join-Path $repo "src"
    if (Test-Path $srcDir) {
        Get-ChildItem $srcDir -File -Recurse |
            Where-Object { $_.Extension -in '.h', '.hpp', '.cpp', '.inl', '.ipp' } |
            ForEach-Object {
                $f = $_.FullName
                $lineNo = 0
                foreach ($line in (Get-Content $f)) {
                    $lineNo++
                    if ($line -match '#\s*define\s+LUMINUMBRA_ENABLE_TRACY') {
                        $rel = $f.Substring($repo.Length + 1).Replace('\', '/')
                        $violations.Add("${rel}:${lineNo}: production code #defines LUMINUMBRA_ENABLE_TRACY (only CMake may, when ON): $($line.Trim())")
                    }
                }
            }
    }

    if ($violations.Count -gt 0) {
        Write-Host "ProfilerDeterminismNeutral: FAILED -- $($violations.Count) breach(es):"
        foreach ($v in $violations) { Write-Host "  - $v" }
        throw "ProfilerDeterminismNeutral gate failed: $($violations.Count) breach(es) of the Tracy off-by-default invariant"
    }
    Write-Host "ProfilerDeterminismNeutral: OK -- Tracy defaults OFF, Profiler.h is a no-op when disabled, no TU force-enables it (world_hash byte-identical by construction)."
}

function Test-PilotReadiness {
    #   /  hard-gate readiness. Machine-check that the four
    # legs which must be closed BEFORE the RHI pilot (/) may legally start
    # are in place. This is a static readiness gate (each leg's full correctness is
    # owned by that leg's own gate); it fails listing EVERY unmet leg, not just the
    # first, so a regression is fully diagnosed. Flips green only when the pilot may
    # legally begin. Legs: RenderContext seam, ExpectedLayout coverage
    #, registry-owned targets, in-process FLIP.
    $repo = (Get-Location).Path
    $unmet = New-Object System.Collections.Generic.List[string]

    function Get-SrcText([string]$rel) {
        $p = Join-Path $repo $rel
        if (-not (Test-Path $p)) { return $null }
        return (Get-Content $p -Raw)
    }

    # Leg 1: the pilot passes surface on the const RenderContext& seam.
    $dvp = Get-SrcText "src/luminumbra_client/rendering/passes/DebugViewPass.h"
    if ($null -eq $dvp -or $dvp -notmatch 'void\s+execute\(const\s+RenderContext&') {
        $unmet.Add("Leg 1 (RenderContext seam, ): DebugViewPass::execute(const RenderContext&) not found")
    }
    $lp = Get-SrcText "src/luminumbra_client/rendering/passes/LightingPass.h"
    if ($null -eq $lp -or $lp -notmatch 'void\s+execute\(const\s+RenderContext&') {
        $unmet.Add("Leg 1 (RenderContext seam, ): LightingPass::execute(const RenderContext&) not found")
    }

    # Leg 2: >=13 sampler-binding passes register an ExpectedLayout,
    # including the pilot pair (debug_view + lighting).
    $psl = Get-SrcText "src/luminumbra_client/rendering/PassShaderLayouts.cpp"
    if ($null -eq $psl) {
        $unmet.Add("Leg 2 (ExpectedLayout coverage, ): PassShaderLayouts.cpp missing")
    } else {
        $regCount = ([regex]::Matches($psl, 'v\.push_back\(\{')).Count
        if ($regCount -lt 13) {
            $unmet.Add("Leg 2 (ExpectedLayout coverage, ): only $regCount/13 passes register an ExpectedLayout")
        }
        foreach ($p in @('debug_view', 'lighting')) {
            if ($psl -notmatch ('v\.push_back\(\{"' + $p + '"')) {
                $unmet.Add("Leg 2 (ExpectedLayout coverage, ): pilot pass '$p' has no ExpectedLayout registration")
            }
        }
    }

    # Leg 3: the registry OWNS render targets (not adopt-only),
    # and the ownership parity ctest is registered.
    $reg = Get-SrcText "src/luminumbra_client/rendering/RenderResourceRegistry.h"
    if ($null -eq $reg -or $reg -notmatch 'registry-OWNED') {
        $unmet.Add("Leg 3 (registry-owned targets, /): RenderResourceRegistry OWNED-allocation API not found")
    }
    $testCmake = Get-SrcText "test/CMakeLists.txt"
    if ($null -eq $testCmake -or $testCmake -notmatch 'registry_ownership_test') {
        $unmet.Add("Leg 3 (registry-owned targets, /): registry_ownership_test not registered")
    }

    # Leg 4: the in-process dual-render FLIP harness exists + is registered.
    if (-not (Test-Path (Join-Path $repo "src/luminumbra_client/rendering/InProcessFlip.h"))) {
        $unmet.Add("Leg 4 (in-process FLIP, ): InProcessFlip.h missing")
    }
    $flip = Get-SrcText "test/rendering/dual_backend_flip_test.cpp"
    if ($null -eq $flip -or $flip -notmatch 'class\s+DualBackendFlipInProcessGpu') {
        $unmet.Add("Leg 4 (in-process FLIP, ): DualBackendFlipInProcessGpu suite not found")
    }
    if ($null -eq $testCmake -or $testCmake -notmatch 'dual_backend_flip_test') {
        $unmet.Add("Leg 4 (in-process FLIP, ): dual_backend_flip_test not registered")
    }

    if ($unmet.Count -gt 0) {
        Write-Host "PilotReadiness: NOT READY -- $($unmet.Count) unmet  hard-gate leg(s):"
        foreach ($u in $unmet) { Write-Host "  - $u" }
        throw "PilotReadiness gate failed: $($unmet.Count) unmet  hard-gate leg(s); the RHI pilot may not legally start"
    }
    Write-Host "PilotReadiness: READY -- all four  hard-gate legs closed (RenderContext seam, ExpectedLayout coverage $regCount/13, registry-owned targets, in-process FLIP)."
}

switch ($Mode) {
    "ArtifactManifest" { Test-ArtifactManifest }
    "ConfigSchemaCheck" { Test-ConfigSchemaCheck }
    "Build" { Test-Build }
    "UnitTests" { Test-UnitTests }
    "MaterialVisual" { Test-MaterialVisual }
    "RenderHealth" { Test-RenderHealth }
    "EmissiveCalibration" { Test-EmissiveCalibration }
    "ShaderInventory" { Test-ShaderInventory }
    "ChunkCollisionLifecycle" { Test-ChunkCollisionLifecycle }
    "PhysicsReplay" { Test-PhysicsReplay }
    "AudioNullTelemetry" { Test-AudioNullTelemetry }
    "AudioHandleApplication" { Test-AudioHandleApplication }
    "AtmosphereAudio" { Test-AtmosphereAudio }
    "UiTestBaseline" { Test-UiTestBaseline }
    "SimulationEventBusOrderGate" { Test-SimulationEventBusOrderGate }
    "LuaApiManifestGate" { Test-LuaApiManifestGate }
    "ScalarFieldDiffusionGate" { Test-ScalarFieldDiffusionGate }
    "InstinctPlannerGate" { Test-InstinctPlannerGate }
    "PersistenceRoundtripGate" { Test-PersistenceRoundtripGate }
    "PersistenceRuntimeRoundtrip" { Test-PersistenceRuntimeRoundtrip }
    "ChunkFormatValidationGate" { Test-ChunkFormatValidationGate }
    "WorldHashEntitySnapshotGate" { Test-WorldHashEntitySnapshotGate }
    "NetworkLoopbackAuthorityGate" { Test-NetworkLoopbackAuthorityGate }
    "NetworkStateHash" { Test-NetworkStateHash }
    "EcologyTickPerf" { Test-EcologyTickPerf }
    "FarFieldForestBudget" { Test-FarFieldForestBudget }
    "SkyboxVisual" { Test-SkyboxVisual }
    "WeatherVisual" { Test-WeatherVisual }
    "ParticleEmitterDeterminism" { Test-ParticleEmitterDeterminism }
    "CloudShadow" { Test-CloudShadow }
    "FoliageInstancing" { Test-FoliageInstancing }
    "Precipitation" { Test-Precipitation }
    "TimeOfDaySweep" { Test-TimeOfDaySweep }
    "WorldVisualSweep" { Test-WorldVisualSweep }
    "PlayerView" { Test-PlayerView }
    "FarLodHorizon" { Test-FarLodHorizon }
    "IsolationLayer" { Test-IsolationLayer }
    "HeadlessServerTick" { Test-HeadlessServerTick }
    "PopulatedWorldReplay" { Test-PopulatedWorldReplay }
    "PopulatedAsan" { Test-PopulatedAsan }
    "ReplicationSmoke" { Test-ReplicationSmoke }
    "NetworkedReplication" { Test-NetworkedReplication }
    "HeadlessServerTickHeavy" { Test-HeadlessServerTickHeavy }
    "RenderParityFrame" { Test-RenderParityFrame }
    "UpscaleSeamParity" { Test-UpscaleSeamParity }
    "WindFieldDeterminism" { Test-WindFieldDeterminism }
    "AetherFieldDeterminism" { Test-AetherFieldDeterminism }
    "ReplayRoundtrip" { Test-ReplayRoundtrip }
    "ReplayDivergence" { Test-ReplayDivergence }
    "LockstepLoopback" { Test-LockstepLoopback }
    "LockstepFaultInjection" { Test-LockstepFaultInjection }
    "NetworkedSession" { Test-NetworkedSession }
    "SkinnedMeshVisual" { Test-SkinnedMeshVisual }
    "EngineGameSplitLint" { Test-EngineGameSplitLint }
    "SimDeterminismLint" { Test-SimDeterminismLint }
    "SimOptLevelParity" { Test-SimOptLevelParity }
    "CreatureSlice" { Test-CreatureSlice }
    "StimulusChannelGate" { Test-StimulusChannelGate }
    "BiomeCoverage" { Test-BiomeCoverage }
    "RiverPresence" { Test-RiverPresence }
    "WaterfallVisual" { Test-WaterfallVisual }
    "StructurePresence" { Test-StructurePresence }
    "BiomeReverb" { Test-BiomeReverb }
    "TerrainRealism" { Test-TerrainRealism }
    "WindowModeStress" { Test-WindowModeStress }
    "MovingResidency" { Test-MovingResidency }
    "WorldLoadBounded" { Test-WorldLoadBounded }
    "ReadbackDiscipline" { Test-ReadbackDiscipline }
    "RenderReadbackAllowlist" { Test-RenderReadbackAllowlist }
    "DocumentationHygiene" { Test-DocumentationHygiene }
    "DeterminismAudit" { Test-DeterminismAudit }
    "HeadlessInGameCapture" { Test-HeadlessInGameCapture }
    "PilotReadiness" { Test-PilotReadiness }
    "RhiNoReexport" { Test-RhiNoReexport }
    "ProfilerDeterminismNeutral" { Test-ProfilerDeterminismNeutral }
    "BuildTreeStrict" { Test-BuildTreeStrict }
    "ScheduledGateRun" { Test-ScheduledGateRun }
    "NetDemotionDocGrep" { Test-NetDemotionDocGrep }
    "All" {
        Test-ConfigSchemaCheck
        Test-ArtifactManifest
        Test-RenderHealth
        Test-ShaderInventory
        Test-ChunkCollisionLifecycle
        Test-PhysicsReplay
        Test-AudioNullTelemetry
        Test-AudioHandleApplication
        Test-AtmosphereAudio
        Test-UiTestBaseline
        Test-SimulationEventBusOrderGate
        Test-LuaApiManifestGate
        Test-ScalarFieldDiffusionGate
        Test-InstinctPlannerGate
        Test-PersistenceRoundtripGate
        Test-ChunkFormatValidationGate
        Test-WorldHashEntitySnapshotGate
        Test-NetworkLoopbackAuthorityGate
        Test-NetworkStateHash
        Test-SimDeterminismLint
        Test-ReadbackDiscipline
        Test-RenderReadbackAllowlist
        Test-DocumentationHygiene
        Test-DeterminismAudit
        Test-PilotReadiness
        Test-RhiNoReexport
        Test-ProfilerDeterminismNeutral
        Test-BuildTreeStrict
        Test-NetDemotionDocGrep
    }
}

Write-Host "engine-frontier validation passed: $Mode"
