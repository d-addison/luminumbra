param(
    [ValidateSet("CodexOnly", "Panels", "Files", "Sections", "Build", "UnitTests", "MaterialVisual", "All")]
    [string]$Mode = "All",

    [string]$BuildPreset = "debug"
)

$ErrorActionPreference = "Stop"

$SpecPath = ".forge/specs/ENGINE-FRONTIER-2026-06-09.md"
$ArtifactDir = ".forge/artifacts/engine-frontier"
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

switch ($Mode) {
    "CodexOnly" { Test-CodexOnly }
    "Panels" { Test-Panels }
    "Files" { Test-Files }
    "Sections" { Test-Sections }
    "Build" { Test-Build }
    "UnitTests" { Test-UnitTests }
    "MaterialVisual" { Test-MaterialVisual }
    "All" {
        Test-CodexOnly
        Test-Files
        Test-Sections
        Test-Panels
    }
}

Write-Host "engine-frontier validation passed: $Mode"
