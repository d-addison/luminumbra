param(
    [ValidateSet("Files", "Sections", "CodexOnly", "Source", "Build", "UnitTests", "Smoke", "LodGround", "WaterVisual", "EnduranceStreamDrain", "LodBoundaryHysteresis", "Endurance300", "CrashDump", "MemoryWatermark", "RuntimeArtifacts", "All")]
    [string]$Mode = "All",

    [string]$BuildPreset = "debug",
    [int]$SmokeSeconds = 5
)

$ErrorActionPreference = "Stop"

$SpecPath = ".forge/specs/RUNTIME-STABILITY-PHASE-1-2026-06-08.md"
$ArtifactDir = ".forge/artifacts/runtime-stability-phase-1"
$WorkflowPath = ".forge/workflows/runtime-stability-phase-1.yaml"
$DispatchPath = ".forge/tasks/runtime-stability-phase-1/dispatch.json"
$RuntimeArtifactDir = "build/$BuildPreset/test-artifacts/runtime"
$CrashDir = "build/$BuildPreset/crashes"

function Assert-FileExists {
    param([string]$Path)
    if (-not (Test-Path $Path)) {
        throw "Missing Phase 1 file: $Path"
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

function Test-Files {
    foreach ($file in @(
        $SpecPath,
        "$ArtifactDir/research.md",
        "$ArtifactDir/plan.md",
        "$ArtifactDir/critique.md",
        "$ArtifactDir/verification.md",
        "$ArtifactDir/run-log.json",
        $DispatchPath,
        $WorkflowPath,
        ".forge/scripts/validate-runtime-stability-phase-1.ps1"
    )) {
        Assert-FileExists $file
    }
}

function Test-Sections {
    $checks = @{
        $SpecPath = @("Objective", "Scope", "Acceptance Criteria", "Verification Commands", "No-Deferral Rules")
        "$ArtifactDir/research.md" = @("Existing Anchors", "Harness Design", "Artifact Schema Targets", "Risk Notes")
        "$ArtifactDir/plan.md" = @("Wave 1", "Wave 6", "Blocker Handling")
        "$ArtifactDir/critique.md" = @("Findings", "Mitigations", "Review Decision")
        "$ArtifactDir/verification.md" = @("Gates", "Current Result")
    }

    foreach ($path in $checks.Keys) {
        foreach ($needle in $checks[$path]) {
            Assert-Contains -Path $path -Needle $needle
        }
    }
}

function Test-CodexOnly {
    Assert-FileExists ".forge/config.yaml"
    Assert-FileExists $WorkflowPath
    Assert-FileExists $DispatchPath

    $config = Get-Content ".forge/config.yaml" -Raw
    if ($config -notmatch "spawner_override:\s*codex") {
        throw "Forge config must set dispatch.spawner_override to codex"
    }
    if ($config -notmatch "allowed_spawners:\s*\[codex\]") {
        throw "Forge config must restrict dispatch.allowed_spawners to [codex]"
    }

    $workflow = Get-Content $WorkflowPath -Raw
    $dispatch = Get-Content $DispatchPath -Raw
    if ($workflow -notmatch "#runtime:codex") {
        throw "Phase 1 workflow must carry #runtime:codex tag"
    }
    if ($workflow -notmatch "--spawner codex") {
        throw "Phase 1 workflow must pass --spawner codex for contract dispatch dry-runs"
    }
    if ($dispatch -notmatch "Codex-only") {
        throw "Phase 1 dispatch graph must declare Codex-only execution"
    }
    if (($workflow + "`n" + $dispatch) -match "(?i)\b(claude|gemini|cursor|antigravity)\b") {
        throw "Phase 1 routing files must not reference non-Codex agents"
    }
}

function Test-Source {
    $main = "src/luminumbra_client/main_client.cpp"
    $harness = "src/luminumbra_client/core/RuntimeScenarioHarness.cpp"
    $jobHeader = "src/luminumbra_common/core/JobSystem.h"
    $jobSource = "src/luminumbra_common/core/JobSystem.cpp"
    $worldHeader = "src/luminumbra_common/systems/SHIELD_WorldSystem.h"
    $renderHeader = "src/luminumbra_client/rendering/RenderPipeline.h"
    $renderSource = "src/luminumbra_client/rendering/RenderPipeline.cpp"

    Assert-FileExists $main
    Assert-FileExists $harness
    $scenarioText = (Get-Content $main -Raw) + "`n" + (Get-Content $harness -Raw)
    foreach ($needle in @(
        "--scenario",
        "auto_world_smoke",
        "--auto-create-world",
        "--auto-enter-world",
        "--timed-run",
        "--coverage-radius",
        "--no-audio",
        "--no-ui",
        "--hidden-window",
        "--memory-watermark-mb",
        "last-known-runtime.json",
        "lod_ground_smoke",
        "water_visual_smoke",
        "runtime-frames.json",
        "lod-ground-screenshots.json",
        "lod-ground-visual-analysis.json",
        "water-visual-analysis.json",
        "camera_local_coverage",
        "gl_debug",
        "memory-watermark.json",
        "forced_crash",
        "MiniDumpWriteDump",
        "RuntimeScenarioConfig",
        "RuntimeStateRecorder"
    )) {
        if ($scenarioText -notmatch [regex]::Escape($needle)) {
            throw "Missing '$needle' in $main or $harness"
        }
    }

    Assert-Contains -Path $jobHeader -Needle "RuntimeStats"
    Assert-Contains -Path $jobHeader -Needle "get_runtime_stats"
    Assert-Contains -Path $jobSource -Needle "queue_depth"
    Assert-Contains -Path $worldHeader -Needle "RuntimeChunkStats"
    Assert-Contains -Path $worldHeader -Needle "get_runtime_chunk_stats"
    Assert-Contains -Path $renderHeader -Needle "RuntimeRenderStats"
    Assert-Contains -Path $renderHeader -Needle "get_runtime_render_stats"
    Assert-Contains -Path $renderSource -Needle "estimated_vram_bytes"
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

        return [pscustomobject]@{
            ExitCode = $exitCode
            Stdout = $stdout
            Stderr = $stderr
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

function Test-Build {
    & cmake --build --preset $BuildPreset --target luminumbra_client_app common_tests
    if ($LASTEXITCODE -ne 0) {
        exit $LASTEXITCODE
    }
}

function Test-UnitTests {
    & ctest --preset $BuildPreset --output-on-failure -R "JobSystem|WorldStreamingState"
    if ($LASTEXITCODE -ne 0) {
        exit $LASTEXITCODE
    }
}

function Test-Smoke {
    $exe = Get-ClientExe
    $artifactRoot = "build/$BuildPreset/test-artifacts/runtime"
    New-Item -ItemType Directory -Force -Path $artifactRoot | Out-Null

    $hiddenDir = Join-Path $artifactRoot "hidden-smoke"
    $visibleDir = Join-Path $artifactRoot "visible-smoke"
    Remove-Item -Recurse -Force -ErrorAction SilentlyContinue $hiddenDir, $visibleDir

    Invoke-Checked -FilePath $exe -ArgumentList @(
        "--scenario", "auto_world_smoke",
        "--auto-create-world",
        "--auto-enter-world",
        "--timed-run", "$SmokeSeconds",
        "--no-audio",
        "--no-ui",
        "--hidden-window",
        "--runtime-artifact-dir", $hiddenDir
    ) -TimeoutSeconds ([Math]::Max(90, $SmokeSeconds + 60)) | Out-Null

    Invoke-Checked -FilePath $exe -ArgumentList @(
        "--scenario", "auto_world_smoke",
        "--auto-create-world",
        "--auto-enter-world",
        "--timed-run", "$SmokeSeconds",
        "--no-audio",
        "--no-ui",
        "--runtime-artifact-dir", $visibleDir
    ) -TimeoutSeconds ([Math]::Max(90, $SmokeSeconds + 60)) | Out-Null

    foreach ($dir in @($hiddenDir, $visibleDir)) {
        Assert-FileExists (Join-Path $dir "last-known-runtime.json")
        Assert-FileExists (Join-Path $dir "shutdown.json")
    }
}

function Test-LodGround {
    $exe = Get-ClientExe
    $artifactRoot = "build/$BuildPreset/test-artifacts/runtime"
    New-Item -ItemType Directory -Force -Path $artifactRoot | Out-Null

    $baselineDir = Join-Path $artifactRoot "lod-ground-baseline"
    Remove-Item -Recurse -Force -ErrorAction SilentlyContinue $baselineDir

    $runSeconds = [Math]::Max(10, $SmokeSeconds)
    Invoke-Checked -FilePath $exe -ArgumentList @(
        "--scenario", "lod_ground_smoke",
        "--auto-create-world",
        "--auto-enter-world",
        "--timed-run", "$runSeconds",
        "--coverage-radius", "3",
        "--no-audio",
        "--no-ui",
        "--runtime-artifact-dir", $baselineDir
    ) -TimeoutSeconds ([Math]::Max(120, $runSeconds + 90)) | Out-Null

    $statePath = Join-Path $baselineDir "last-known-runtime.json"
    $framesPath = Join-Path $baselineDir "runtime-frames.json"
    $framesCsvPath = Join-Path $baselineDir "runtime-frames.csv"
    $screenshotsPath = Join-Path $baselineDir "lod-ground-screenshots.json"
    $visualAnalysisPath = Join-Path $baselineDir "lod-ground-visual-analysis.json"
    Assert-FileExists $statePath
    Assert-FileExists $framesPath
    Assert-FileExists $framesCsvPath
    Assert-FileExists $screenshotsPath
    Assert-FileExists $visualAnalysisPath

    $state = Get-Content $statePath -Raw | ConvertFrom-Json
    if ($state.scenario -ne "lod_ground_smoke") {
        throw "LOD ground artifact has unexpected scenario '$($state.scenario)'"
    }
    foreach ($property in @("camera", "camera_local_coverage", "gl_debug", "streaming", "upload_queue", "render_pass")) {
        if ($null -eq $state.$property) {
            throw "LOD ground runtime state missing '$property'"
        }
    }
    if ([int]$state.camera_local_coverage.expected_surface_chunks -le 0) {
        throw "LOD ground coverage did not sample surface chunks"
    }
    if (-not $state.camera_local_coverage.center_chunk_present) {
        throw "LOD ground camera surface chunk is missing"
    }
    if (-not $state.camera_local_coverage.center_chunk_renderable) {
        throw "LOD ground camera surface chunk is not renderable"
    }
    if (-not $state.camera_local_coverage.near_field_renderable) {
        throw "LOD ground near-field coverage is not fully renderable"
    }
    if ([int64]$state.gl_debug.errors -ne 0) {
        throw "LOD ground run emitted GL debug errors: $($state.gl_debug.errors)"
    }
    if ([int]$state.upload_queue.terrain_deferred_nearer_than_selected -ne 0) {
        throw "LOD ground terrain upload priority deferred nearer chunks behind selected uploads"
    }
    if ([int]$state.upload_queue.water_deferred_nearer_than_selected -ne 0) {
        throw "LOD ground water upload priority deferred nearer chunks behind selected uploads"
    }
    if ($null -eq $state.upload_queue.water_upload_candidates) {
        throw "LOD ground runtime state missing water upload candidate telemetry"
    }

    $frames = Get-Content $framesPath -Raw | ConvertFrom-Json
    if ($frames.schema -ne "luminumbra.runtime_frames.v1") {
        throw "Unexpected LOD ground frame schema '$($frames.schema)'"
    }
    if ([int]$frames.frames_recorded -le 0) {
        throw "LOD ground frame artifact recorded no frames"
    }
    $missingWaterCandidateTelemetry = @($frames.frames | Where-Object { $null -eq $_.upload_queue.water_upload_candidates })
    if ($missingWaterCandidateTelemetry.Count -gt 0) {
        throw "LOD ground frame artifact is missing water upload candidate telemetry on $($missingWaterCandidateTelemetry.Count) frames"
    }
    $coverageFailures = @($frames.frames | Where-Object { -not $_.coverage.near_field_renderable })
    if ($coverageFailures.Count -gt 0) {
        throw "LOD ground frame artifact contains $($coverageFailures.Count) frames with incomplete near-field coverage"
    }
    $priorityFailures = @($frames.frames | Where-Object {
        [int]$_.upload_queue.terrain_deferred_nearer_than_selected -gt 0 -or
        [int]$_.upload_queue.water_deferred_nearer_than_selected -gt 0
    })
    if ($priorityFailures.Count -gt 0) {
        throw "LOD ground frame artifact contains $($priorityFailures.Count) frames with near upload priority inversions"
    }

    $screenshots = Get-Content $screenshotsPath -Raw | ConvertFrom-Json
    if (@($screenshots.captures).Count -lt 3) {
        throw "LOD ground run did not capture start/mid/end screenshots"
    }
    foreach ($capture in $screenshots.captures) {
        Assert-FileExists (Join-Path $baselineDir $capture.file)
    }

    $visualAnalysis = Get-Content $visualAnalysisPath -Raw | ConvertFrom-Json
    if ($visualAnalysis.schema -ne "luminumbra.lod_ground_visual_analysis.v1") {
        throw "Unexpected LOD ground visual analysis schema '$($visualAnalysis.schema)'"
    }
    if (-not $visualAnalysis.passed) {
        $failed = @($visualAnalysis.captures | Where-Object { -not $_.passed } | Select-Object -First 1)
        if ($failed.Count -gt 0) {
            throw "LOD ground visual analysis failed for '$($failed[0].role)': dark_void_pixels=$($failed[0].pixels.dark_void_pixels), dark_void_ratio=$($failed[0].pixels.dark_void_ratio), file=$($failed[0].file)"
        }
        throw "LOD ground visual analysis failed"
    }
    $enforcedCaptures = @($visualAnalysis.captures | Where-Object { $_.enforced })
    if ($enforcedCaptures.Count -lt 2) {
        throw "LOD ground visual analysis did not enforce mid/end captures"
    }
}

function Test-WaterVisual {
    $exe = Get-ClientExe
    $artifactRoot = "build/$BuildPreset/test-artifacts/runtime"
    New-Item -ItemType Directory -Force -Path $artifactRoot | Out-Null

    $visualDir = Join-Path $artifactRoot "water-visual"
    Remove-Item -Recurse -Force -ErrorAction SilentlyContinue $visualDir

    $runSeconds = [Math]::Max(20, $SmokeSeconds)
    Invoke-Checked -FilePath $exe -ArgumentList @(
        "--scenario", "water_visual_smoke",
        "--auto-create-world",
        "--auto-enter-world",
        "--timed-run", "$runSeconds",
        "--no-audio",
        "--no-ui",
        "--runtime-artifact-dir", $visualDir
    ) -TimeoutSeconds ([Math]::Max(120, $runSeconds + 90)) | Out-Null

    $statePath = Join-Path $visualDir "last-known-runtime.json"
    $analysisPath = Join-Path $visualDir "water-visual-analysis.json"
    Assert-FileExists $statePath
    Assert-FileExists $analysisPath

    $state = Get-Content $statePath -Raw | ConvertFrom-Json
    if ($state.scenario -ne "water_visual_smoke") {
        throw "Water visual artifact has unexpected scenario '$($state.scenario)'"
    }
    if (-not $state.readiness.ready) {
        throw "Water visual run did not finish ready: $($state.readiness.reasons -join ', ')"
    }
    if ([int]$state.render_pass.water_draws -le 0 -or [int]$state.render_pass.water_indices_drawn -le 0) {
        throw "Water visual run did not submit water draws"
    }
    if ([int64]$state.gl_debug.errors -ne 0) {
        throw "Water visual run emitted GL debug errors: $($state.gl_debug.errors)"
    }

    $analysis = Get-Content $analysisPath -Raw | ConvertFrom-Json
    if ($analysis.schema -ne "luminumbra.water_visual_analysis.v1") {
        throw "Unexpected water visual analysis schema '$($analysis.schema)'"
    }
    if (-not $analysis.target.found) {
        throw "Water visual scenario did not find a water target"
    }
    if (-not $analysis.passed) {
        throw "Water visual pixel analysis failed: pixels=$($analysis.pixels.water_like_pixels), ratio=$($analysis.pixels.water_like_ratio)"
    }
    if ([int64]$analysis.pixels.water_like_pixels -lt [int64]$analysis.thresholds.min_water_like_pixels) {
        throw "Water visual screenshot has too few water-like pixels"
    }
    if ([double]$analysis.pixels.water_like_ratio -lt [double]$analysis.thresholds.min_water_like_ratio) {
        throw "Water visual screenshot has too low a water-like pixel ratio"
    }
    if ([int]$analysis.render_pass.water_draws -le 0 -or [int]$analysis.render_pass.water_indices_drawn -le 0) {
        throw "Water visual analysis did not record water draw submission"
    }
    if ([int64]$analysis.gl_debug.errors -ne 0) {
        throw "Water visual analysis recorded GL debug errors: $($analysis.gl_debug.errors)"
    }
    Assert-FileExists (Join-Path $visualDir $analysis.screenshot)
}

function Test-EnduranceStreamDrain {
    $exe = Get-ClientExe
    $artifactRoot = "build/$BuildPreset/test-artifacts/runtime"
    New-Item -ItemType Directory -Force -Path $artifactRoot | Out-Null

    $drainDir = Join-Path $artifactRoot "endurance-stream-drain"
    Remove-Item -Recurse -Force -ErrorAction SilentlyContinue $drainDir

    $runSeconds = [Math]::Max(20, $SmokeSeconds)
    Invoke-Checked -FilePath $exe -ArgumentList @(
        "--scenario", "auto_world_smoke",
        "--auto-create-world",
        "--auto-enter-world",
        "--timed-run", "$runSeconds",
        "--no-audio",
        "--no-ui",
        "--runtime-artifact-dir", $drainDir
    ) -TimeoutSeconds ([Math]::Max(120, $runSeconds + 90)) | Out-Null

    $telemetryPath = Join-Path $drainDir "streaming-telemetry.json"
    Assert-FileExists $telemetryPath

    $telemetry = Get-Content $telemetryPath -Raw | ConvertFrom-Json
    if ($telemetry.schema -ne "luminumbra.streaming_telemetry.v1") {
        throw "Unexpected streaming telemetry schema '$($telemetry.schema)'"
    }
    if ($telemetry.scenario -ne "auto_world_smoke") {
        throw "Streaming telemetry has unexpected scenario '$($telemetry.scenario)'"
    }
    if ([double]$telemetry.duration_seconds -le 0.0) {
        throw "Streaming telemetry recorded no run duration"
    }
    if (-not $telemetry.backlog_bounded) {
        throw "Streaming backlog is not bounded: final_queue_depth=$($telemetry.final_queue_depth), max_deferred_age_frames=$($telemetry.max_deferred_age_frames)"
    }
    if ([int64]$telemetry.final_queue_depth -ne 0) {
        throw "Streaming queue did not drain: final_queue_depth=$($telemetry.final_queue_depth)"
    }
    if ([double]$telemetry.drain_rate_per_s -le 0.0) {
        throw "Streaming telemetry recorded no meshing drain: drain_rate_per_s=$($telemetry.drain_rate_per_s)"
    }
}

function Test-LodBoundaryHysteresis {
    $exe = Get-ClientExe
    $artifactRoot = "build/$BuildPreset/test-artifacts/runtime"
    New-Item -ItemType Directory -Force -Path $artifactRoot | Out-Null

    $boundaryDir = Join-Path $artifactRoot "lod-boundary-oscillation"
    Remove-Item -Recurse -Force -ErrorAction SilentlyContinue $boundaryDir

    $runSeconds = [Math]::Max(20, $SmokeSeconds)
    Invoke-Checked -FilePath $exe -ArgumentList @(
        "--scenario", "lod_boundary_oscillation_smoke",
        "--auto-create-world",
        "--auto-enter-world",
        "--timed-run", "$runSeconds",
        "--no-audio",
        "--no-ui",
        "--runtime-artifact-dir", $boundaryDir
    ) -TimeoutSeconds ([Math]::Max(120, $runSeconds + 90)) | Out-Null

    $analysisPath = Join-Path $boundaryDir "lod-boundary-oscillation.json"
    Assert-FileExists $analysisPath

    $analysis = Get-Content $analysisPath -Raw | ConvertFrom-Json
    if ($analysis.schema -ne "luminumbra.lod_boundary_oscillation.v1") {
        throw "Unexpected LOD boundary oscillation schema '$($analysis.schema)'"
    }
    if ([int64]$analysis.gl_debug.errors -ne 0) {
        throw "LOD boundary oscillation run emitted GL debug errors: $($analysis.gl_debug.errors)"
    }
    if ([int64]$analysis.frames_observed -le 0) {
        throw "LOD boundary oscillation run observed no frames"
    }
    if ([int64]$analysis.chunks_observed -le 0) {
        throw "LOD boundary oscillation run observed no chunks"
    }
    if ([double]$analysis.boundary_distance -le 0.0) {
        throw "LOD boundary oscillation run recorded no boundary distance"
    }
    $baseline = $analysis.known_oscillation_baseline
    if ($null -eq $baseline) {
        throw "LOD boundary oscillation analysis is missing known_oscillation_baseline"
    }
    if ([double]$analysis.max_transitions_per_chunk_per_s -gt [double]$baseline.max_transitions_per_chunk_per_s) {
        throw "LOD boundary oscillation exceeded baseline per-chunk transition rate: $($analysis.max_transitions_per_chunk_per_s) > $($baseline.max_transitions_per_chunk_per_s)"
    }
    if ([int64]$analysis.oscillating_chunk_count -gt [int64]$baseline.max_oscillating_chunk_count) {
        throw "LOD boundary oscillation exceeded baseline oscillating chunk count: $($analysis.oscillating_chunk_count) > $($baseline.max_oscillating_chunk_count)"
    }
    if ([double]$analysis.total_transitions_per_s -gt [double]$baseline.max_total_transitions_per_s) {
        throw "LOD boundary oscillation exceeded baseline total transition rate: $($analysis.total_transitions_per_s) > $($baseline.max_total_transitions_per_s)"
    }
    if (-not $analysis.passed) {
        throw "LOD boundary oscillation analysis reported failure"
    }

    Write-Host "LOD boundary oscillation at $($analysis.boundary_distance)m over $($analysis.duration_seconds)s: chunks_observed=$($analysis.chunks_observed), max_transitions_per_chunk=$($analysis.max_transitions_per_chunk), oscillating_chunk_count=$($analysis.oscillating_chunk_count), total_transitions=$($analysis.total_transitions)"
}

function Test-Endurance300 {
    $exe = Get-ClientExe
    $artifactRoot = "build/$BuildPreset/test-artifacts/runtime"
    New-Item -ItemType Directory -Force -Path $artifactRoot | Out-Null

    $visibleDir = Join-Path $artifactRoot "visible-300"
    Remove-Item -Recurse -Force -ErrorAction SilentlyContinue $visibleDir

    Invoke-Checked -FilePath $exe -ArgumentList @(
        "--scenario", "auto_world_smoke",
        "--auto-create-world",
        "--auto-enter-world",
        "--timed-run", "300",
        "--no-audio",
        "--no-ui",
        "--runtime-artifact-dir", $visibleDir
    ) -TimeoutSeconds 420 | Out-Null

    Assert-FileExists (Join-Path $visibleDir "last-known-runtime.json")
    Assert-FileExists (Join-Path $visibleDir "shutdown.json")

    $state = Get-Content (Join-Path $visibleDir "last-known-runtime.json") -Raw | ConvertFrom-Json
    if ($state.launch_flags.timed_run_seconds -ne 300) {
        throw "Endurance run did not record timed_run_seconds=300"
    }
    if ($state.launch_flags.hidden_window) {
        throw "Endurance run must be visible; hidden_window was recorded"
    }
    if ([double]$state.elapsed_seconds -lt 300.0) {
        throw "Endurance run ended before 300 seconds: elapsed_seconds=$($state.elapsed_seconds)"
    }
    if ([int]$state.frame_count -le 0) {
        throw "Endurance run did not render frames"
    }
    if (-not $state.readiness.ready) {
        throw "Endurance run did not reach readiness"
    }
    if ([int]$state.render_pass.water_draws -le 0 -or [int]$state.render_pass.water_indices_drawn -le 0) {
        throw "Endurance run did not render visible water"
    }
}

function Test-CrashDump {
    $exe = Get-ClientExe
    $artifactRoot = "build/$BuildPreset/test-artifacts/runtime/forced-crash"
    $crashRoot = "build/$BuildPreset/crashes"
    Remove-Item -Recurse -Force -ErrorAction SilentlyContinue $artifactRoot, $crashRoot
    New-Item -ItemType Directory -Force -Path $artifactRoot, $crashRoot | Out-Null

    Invoke-Checked -FilePath $exe -ArgumentList @(
        "--scenario", "forced_crash",
        "--hidden-window",
        "--no-audio",
        "--no-ui",
        "--runtime-artifact-dir", $artifactRoot,
        "--crash-dir", $crashRoot
    ) -AllowedExitCodes @(1, -1073741819, -536870911, -529697949) -TimeoutSeconds 90 | Out-Null

    Assert-FileExists (Join-Path $artifactRoot "last-known-runtime.json")
    $dumps = @(Get-ChildItem $crashRoot -Filter "*.dmp" -ErrorAction SilentlyContinue)
    if ($dumps.Count -lt 1) {
        throw "Forced crash did not write a minidump under $crashRoot"
    }
}

function Test-MemoryWatermark {
    $exe = Get-ClientExe
    $artifactRoot = "build/$BuildPreset/test-artifacts/runtime/memory-watermark"
    Remove-Item -Recurse -Force -ErrorAction SilentlyContinue $artifactRoot
    New-Item -ItemType Directory -Force -Path $artifactRoot | Out-Null

    Invoke-Checked -FilePath $exe -ArgumentList @(
        "--scenario", "auto_world_smoke",
        "--auto-create-world",
        "--auto-enter-world",
        "--timed-run", "$SmokeSeconds",
        "--no-audio",
        "--no-ui",
        "--hidden-window",
        "--memory-watermark-mb", "1",
        "--runtime-artifact-dir", $artifactRoot
    ) -AllowedExitCodes @(3) -TimeoutSeconds ([Math]::Max(90, $SmokeSeconds + 60)) | Out-Null

    Assert-FileExists (Join-Path $artifactRoot "last-known-runtime.json")
    Assert-FileExists (Join-Path $artifactRoot "memory-watermark.json")
    $watermark = Get-Content (Join-Path $artifactRoot "memory-watermark.json") -Raw | ConvertFrom-Json
    if ($watermark.phase -ne "main_loop") {
        throw "Expected memory watermark artifact phase 'main_loop', found '$($watermark.phase)'"
    }
    if ([int64]$watermark.measured_bytes -le [int64](1024 * 1024)) {
        throw "Memory watermark artifact did not record measured bytes above the 1 MB test threshold"
    }
}

function Test-RuntimeArtifacts {
    $candidateDirs = @(
        "build/$BuildPreset/test-artifacts/runtime/hidden-smoke",
        "build/$BuildPreset/test-artifacts/runtime/visible-smoke",
        "build/$BuildPreset/test-artifacts/runtime/forced-crash",
        "build/$BuildPreset/test-artifacts/runtime/memory-watermark"
    )

    foreach ($dir in $candidateDirs) {
        if (-not (Test-Path $dir)) {
            continue
        }
        $statePath = Join-Path $dir "last-known-runtime.json"
        Assert-FileExists $statePath
        $state = Get-Content $statePath -Raw | ConvertFrom-Json
        foreach ($property in @("schema", "scenario", "phase", "memory", "estimated_vram_bytes", "chunk_states", "job_queue", "upload_queue", "render_pass", "shader_health", "readiness")) {
            if ($null -eq $state.$property) {
                throw "Runtime state missing property '$property': $statePath"
            }
        }
        if ($dir -match "(hidden-smoke|visible-smoke)") {
            if ([int]$state.render_pass.water_draws -le 0 -or [int]$state.render_pass.water_indices_drawn -le 0) {
                throw "Runtime smoke did not render water: $statePath"
            }
        }
    }

    $enduranceDir = "build/$BuildPreset/test-artifacts/runtime/visible-300"
    Assert-FileExists (Join-Path $enduranceDir "last-known-runtime.json")
    Assert-FileExists (Join-Path $enduranceDir "shutdown.json")
    $endurance = Get-Content (Join-Path $enduranceDir "last-known-runtime.json") -Raw | ConvertFrom-Json
    if ($endurance.launch_flags.timed_run_seconds -ne 300) {
        throw "visible-300 artifact must come from --timed-run 300"
    }
    if ($endurance.launch_flags.hidden_window) {
        throw "visible-300 artifact must come from a visible window run"
    }
    if ([double]$endurance.elapsed_seconds -lt 300.0) {
        throw "visible-300 artifact ended before 300 seconds"
    }
    if ([int]$endurance.frame_count -le 0) {
        throw "visible-300 artifact must contain rendered frames"
    }
    if (-not $endurance.readiness.ready) {
        throw "visible-300 artifact must report readiness"
    }
    if ([int]$endurance.render_pass.water_draws -le 0 -or [int]$endurance.render_pass.water_indices_drawn -le 0) {
        throw "visible-300 artifact must render water"
    }
}

switch ($Mode) {
    "Files" { Test-Files }
    "Sections" { Test-Sections }
    "CodexOnly" { Test-CodexOnly }
    "Source" { Test-Source }
    "Build" { Test-Build }
    "UnitTests" { Test-UnitTests }
    "Smoke" { Test-Smoke }
    "LodGround" { Test-LodGround }
    "WaterVisual" { Test-WaterVisual }
    "EnduranceStreamDrain" { Test-EnduranceStreamDrain }
    "LodBoundaryHysteresis" { Test-LodBoundaryHysteresis }
    "Endurance300" { Test-Endurance300 }
    "CrashDump" { Test-CrashDump }
    "MemoryWatermark" { Test-MemoryWatermark }
    "RuntimeArtifacts" { Test-RuntimeArtifacts }
    "All" {
        Test-Files
        Test-Sections
        Test-CodexOnly
        Test-Source
    }
}

Write-Host "runtime-stability-phase-1 validation passed: $Mode"
