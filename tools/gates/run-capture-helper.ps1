param(
    [ValidateSet("MarkerOnly", "RenderDoc", "PIX", "Nsight")]
    [string]$Backend = "MarkerOnly",

    [string]$Scenario = "auto_world_smoke",
    [string]$BuildPreset = "debug",
    [string]$OutputDir = "build/debug/test-artifacts/tooling"
)

$ErrorActionPreference = "Stop"

New-Item -ItemType Directory -Force -Path $OutputDir | Out-Null
$outputPath = Join-Path $OutputDir "capture_profiler_helper.json"
$clientExe = "build/$BuildPreset/bin/luminumbra_client_app.exe"

function Find-Tool {
    param([string]$Name)
    $cmd = Get-Command $Name -ErrorAction SilentlyContinue
    if ($null -eq $cmd) {
        return $null
    }
    return $cmd.Source
}

$toolName = $null
switch ($Backend) {
    "RenderDoc" { $toolName = "renderdoccmd" }
    "PIX" { $toolName = "WinPixEventRuntime" }
    "Nsight" { $toolName = "nsys" }
    default { $toolName = $null }
}

$toolPath = if ($toolName) { Find-Tool $toolName } else { $null }
$available = ($Backend -eq "MarkerOnly") -or ($null -ne $toolPath)
$marker = "luminumbra.capture.ready:${Scenario}:${Backend}"
$arguments = @(
    "--scenario", $Scenario,
    "--auto-create-world",
    "--auto-enter-world",
    "--timed-run", "30",
    "--no-audio",
    "--runtime-artifact-dir", $OutputDir
)

$report = [ordered]@{
    schema = "luminumbra.tooling.capture_profiler_helper.v1"
    backend = $Backend
    scenario = $Scenario
    marker = $marker
    output_dir = $OutputDir
    client_exe = $clientExe
    external_tool = $toolName
    external_tool_path = $toolPath
    external_tool_available = $available
    launch_arguments = $arguments
    status = if ($available) { "ready" } else { "missing_external_tool" }
    diagnostic = if ($available) {
        "Capture helper is ready; launch manually or through workflow-specific tooling."
    } else {
        "External capture tool '$toolName' was not found. Install it or use Backend MarkerOnly."
    }
}

$report | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath $outputPath
Write-Host "Capture helper report: $outputPath"
