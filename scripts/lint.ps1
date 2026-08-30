param(
    [string]$BuildDir = "build",
    [switch]$FormatOnly,
    [switch]$TidyOnly
)

$ErrorActionPreference = "Stop"

$repoRoot = Resolve-Path (Join-Path $PSScriptRoot "..")
$sourceRoots = @(
    "src",
    "include",
    "test"
)

foreach ($root in $sourceRoots) {
    $path = Join-Path $repoRoot $root
    if (-not (Test-Path -LiteralPath $path -PathType Container)) {
        Write-Error "Required source directory is missing: $root"
    }
}

function Get-FirstPartyCppFiles {
    $extensions = @("*.c", "*.cc", "*.cpp", "*.cxx", "*.h", "*.hh", "*.hpp", "*.hxx")
    foreach ($root in $sourceRoots) {
        $path = Join-Path $repoRoot $root
        if (Test-Path -LiteralPath $path) {
            Get-ChildItem -LiteralPath $path -Recurse -File -Include $extensions |
                Where-Object {
                    $_.FullName -notmatch "[\\/](vendor|external|build|out)[\\/]"
                }
        }
    }
}

function Test-CommandAvailable {
    param([string]$Name)
    return $null -ne (Get-Command $Name -ErrorAction SilentlyContinue)
}

$cppFiles = @(Get-FirstPartyCppFiles)
if ($cppFiles.Count -eq 0) {
    Write-Error "No first-party C/C++ files found."
}

if (-not $TidyOnly) {
    if (-not (Test-CommandAvailable "clang-format")) {
        Write-Error "clang-format was not found on PATH."
    }

    $formatArgs = @("--dry-run", "--Werror") + ($cppFiles | ForEach-Object { $_.FullName })
    & clang-format @formatArgs
}

if (-not $FormatOnly) {
    if (-not (Test-CommandAvailable "clang-tidy")) {
        Write-Error "clang-tidy was not found on PATH."
    }

    $compileDb = Join-Path (Join-Path $repoRoot $BuildDir) "compile_commands.json"
    if (-not (Test-Path -LiteralPath $compileDb)) {
        Write-Error "clang-tidy requires $compileDb. Configure CMake with compile commands enabled first."
    }

    $tidyFiles = $cppFiles | Where-Object { $_.Extension -in @(".c", ".cc", ".cpp", ".cxx") }
    if ($tidyFiles.Count -eq 0) {
        Write-Error "No first-party C/C++ translation units found for clang-tidy."
    }

    & clang-tidy -p (Join-Path $repoRoot $BuildDir) @($tidyFiles | ForEach-Object { $_.FullName })
}
