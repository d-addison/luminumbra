param(
    [Parameter(Mandatory = $true)]
    [ValidateSet(
        "WorldStreaming", "RenderingVisualLoop", "PhysicsPlayer", "Audio",
        "SimulationAiScripting", "PersistenceGpuNetwork", "UiToolingTests",
        "Synthesis", "Critique", "Revision", "All")]
    [string]$Panel,

    [switch]$DryRun
)

$ErrorActionPreference = "Stop"

$projectRoot = (Resolve-Path ".").Path
$promptDir = Join-Path $projectRoot ".forge/prompts/engine-frontier"
$artifactDir = Join-Path $projectRoot ".forge/artifacts/engine-frontier"

$panelPrompts = [ordered]@{
    WorldStreaming        = "panel-1-world-streaming.md"
    RenderingVisualLoop   = "panel-2-rendering-visual-loop.md"
    PhysicsPlayer         = "panel-3-physics-player.md"
    Audio                 = "panel-4-audio.md"
    SimulationAiScripting = "panel-5-simulation-ai-scripting.md"
    PersistenceGpuNetwork = "panel-6-persistence-gpu-network.md"
    UiToolingTests        = "panel-7-ui-tooling-tests.md"
    Synthesis             = "synthesis.md"
    Critique              = "critique.md"
    Revision              = "revision.md"
}

function Invoke-FrontierPanel {
    param([string]$Name)

    $promptPath = Join-Path $promptDir $panelPrompts[$Name]
    if (-not (Test-Path $promptPath)) {
        throw "Missing engine-frontier prompt: $promptPath"
    }

    New-Item -ItemType Directory -Force -Path $artifactDir | Out-Null
    $outputPath = Join-Path $artifactDir ("codex-last-message-" + $Name.ToLowerInvariant() + ".md")

    if ($DryRun) {
        Write-Host "[dry-run] panel=$Name prompt=$promptPath"
        Write-Host "[dry-run] codex exec --ephemeral --cd $projectRoot --sandbox workspace-write -c approval_policy=never -o $outputPath -"
        return
    }

    $prompt = Get-Content $promptPath -Raw

    $prompt | & codex exec `
        --ephemeral `
        --cd $projectRoot `
        --sandbox workspace-write `
        -c 'approval_policy="never"' `
        -o $outputPath `
        -

    if ($LASTEXITCODE -ne 0) {
        throw "codex exec failed for engine-frontier panel '$Name' with exit code $LASTEXITCODE"
    }

    Write-Host "engine-frontier Codex panel completed: $Name"
}

if ($Panel -eq "All") {
    foreach ($name in @($panelPrompts.Keys)) {
        Invoke-FrontierPanel -Name $name
    }
}
else {
    Invoke-FrontierPanel -Name $Panel
}
