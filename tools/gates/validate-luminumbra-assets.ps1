param(
    [string]$Root = ".",
    [string]$OutputPath = "build/debug/test-artifacts/tooling/asset_validation.json"
)

$ErrorActionPreference = "Stop"

function Resolve-RootPath {
    param([string]$Path)
    return [System.IO.Path]::GetFullPath((Join-Path $Root $Path))
}

$requiredFiles = @(
    "worlds/atlas/presets/default.json",
    "res/shaders/basic.vert",
    "res/shaders/basic.frag",
    "res/shaders/g_buffer.vert",
    "res/shaders/g_buffer.frag",
    "res/shaders/lighting_pass.vert",
    "res/shaders/lighting_pass.frag",
    "res/shaders/rml.vert",
    "res/shaders/rml.frag",
    "data/ui/main_menu.rml",
    "data/fonts/Lora/static/Lora-Regular.ttf"
)

$requiredDirectories = @(
    "data/textures",
    "assets/models",
    "res/shaders",
    "worlds/atlas/presets"
)

$missingFiles = New-Object System.Collections.Generic.List[string]
$missingDirectories = New-Object System.Collections.Generic.List[string]

foreach ($file in $requiredFiles) {
    if (-not (Test-Path -LiteralPath (Resolve-RootPath $file) -PathType Leaf)) {
        $missingFiles.Add($file)
    }
}

foreach ($dir in $requiredDirectories) {
    if (-not (Test-Path -LiteralPath (Resolve-RootPath $dir) -PathType Container)) {
        $missingDirectories.Add($dir)
    }
}

$report = [ordered]@{
    schema = "luminumbra.tooling.asset_validation.v1"
    root = [System.IO.Path]::GetFullPath($Root)
    required_files = $requiredFiles
    required_directories = $requiredDirectories
    missing_files = @($missingFiles)
    missing_directories = @($missingDirectories)
    passed = ($missingFiles.Count -eq 0 -and $missingDirectories.Count -eq 0)
}

$parent = Split-Path -Parent $OutputPath
if ($parent) {
    New-Item -ItemType Directory -Force -Path $parent | Out-Null
}
$report | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath $OutputPath

if (-not $report.passed) {
    throw "Asset validation failed. report=$OutputPath missing_files=$($missingFiles -join ',') missing_dirs=$($missingDirectories -join ',')"
}

Write-Host "Asset validation passed: $OutputPath"
