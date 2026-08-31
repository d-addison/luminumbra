<#
.SYNOPSIS
      -- canonical build-root preflight + artifact-provenance manifest.

.DESCRIPTION
    Establishes ONE canonical build root per preset (build/$BuildPreset, per
    CMakePresets.json:13 "binaryDir": "${sourceDir}/build/${presetName}") and
    REFUSES to let a gate read a wrong-tree or stale artifact.

    Hard-fail conditions (a green run proves NONE of these are true):
      * the preset tree's CMakeCache.txt is missing (tree not configured),
      * the preset cache's CMAKE_CACHEFILE_DIR does NOT resolve to build/$BuildPreset
        (a relocated / copied-from-another-tree cache -- a genuine wrong-tree signal),
      * the required executable is missing under the preset tree,
      * -ExpectExeHash is supplied and the current binary's hash differs
        (the  stale-run refusal: captured-with-one-binary, gated-against-another),
      * -ExpectGitSha is supplied and HEAD differs,
      * -Strict is set AND a concurrent root build/CMakeCache.txt exists alongside the
        preset cache (the full  two-tree refusal).

    On every run it writes an artifact manifest (six fields:
    build_preset, git_sha, exe_hash, shader_hash, scenario, timestamp) recording the
    provenance of the binary + shaders that a gate would read, so a downstream gate can
    compare the manifest against the running binary () and never bless an image
    produced by a stale build.

    TWO-TREE POLICY ( , owner call). The repo currently carries BOTH a root
    build/ tree and the preset build/$BuildPreset tree. Per  the safest first step is
    "preflight-refusal + documentation, with physical removal a later cleanup." So the
    concurrent root build/ tree is a WARNING by default (this validator runs clean on the
    current two-tree tree) and becomes a hard FAILURE only under -Strict. Flip the gate to
    -Strict once the root build/ tree is retired.

    Determinism: this validator is non-sim, read-only over build state + git + res/shaders;
    it has ZERO world_hash impact and does not touch --smoke.

.NOTES
    PowerShell 5.1 compatible. Run via the PowerShell tool with C:\msys64\ucrt64\bin
    prepended (). Does NOT modify or rename build/debug -- the engine-frontier gate's
    build/$BuildPreset path keeps working unchanged.
#>
[CmdletBinding()]
param(
    # Canonical preset whose tree (build/$BuildPreset) is the single source of truth.
    [string]$BuildPreset = "debug",

    # Executable whose provenance the manifest records / verifies. Defaults to the client
    # exe the engine-frontier gate resolves (validate-engine-frontier.ps1 Get-ClientExe).
    [string]$ExeName = "luminumbra_client_app.exe",

    # Where to write the provenance manifest. Defaults under the preset tree's
    # test-artifacts/ so it sits beside the other gate artifacts it describes.
    [string]$ManifestOut = "",

    # Opt-in stale-run refusal: fail if the current binary's SHA256 differs.
    [string]$ExpectExeHash = "",

    # Opt-in: fail if HEAD differs from this short/long SHA.
    [string]$ExpectGitSha = "",

    # Free-form provenance label for the manifest's scenario field.
    [string]$Scenario = "build-tree-validation",

    # With -Strict, a concurrent root build/CMakeCache.txt is a hard failure
    # instead of a warning. The frontier gate always enables this; standalone
    # callers may omit it for diagnostics against older local layouts.
    [switch]$Strict,

    # Don't require the executable to exist (manifest still records what is present).
    [switch]$AllowMissingExe
)

$ErrorActionPreference = "Stop"

# --- locate the repo root (this script lives at <root>/tools/gates/) --------------
$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$RepoRoot = (Resolve-Path (Join-Path $ScriptDir "..\..")).Path

$failures = New-Object System.Collections.Generic.List[string]
$warnings = New-Object System.Collections.Generic.List[string]

function Resolve-RepoPath {
    param([string]$Relative)
    return [System.IO.Path]::GetFullPath((Join-Path $RepoRoot $Relative))
}

# --- canonical preset tree ------------------------------------------------------------
$presetTree = Resolve-RepoPath "build/$BuildPreset"
$presetCache = Join-Path $presetTree "CMakeCache.txt"
$rootTree = Resolve-RepoPath "build"
$rootCache = Join-Path $rootTree "CMakeCache.txt"

$treesFound = New-Object System.Collections.Generic.List[string]
if (Test-Path -LiteralPath $rootCache) { $treesFound.Add("build (root)") }
if (Test-Path -LiteralPath $presetCache) { $treesFound.Add("build/$BuildPreset") }

# the preset tree must be configured.
if (-not (Test-Path -LiteralPath $presetCache)) {
    $failures.Add("preset tree not configured: missing $presetCache (run 'cmake --build --preset $BuildPreset' first)")
}
else {
    # Wrong-tree signal: a cache whose recorded home is NOT this preset dir (relocated/copied).
    $cacheDirLine = Select-String -LiteralPath $presetCache -Pattern '^CMAKE_CACHEFILE_DIR:INTERNAL=(.+)$' -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($cacheDirLine) {
        $recordedDir = $cacheDirLine.Matches[0].Groups[1].Value.Trim()
        $recordedNorm = ($recordedDir -replace '/', '\').TrimEnd('\').ToLowerInvariant()
        $expectedNorm = ($presetTree -replace '/', '\').TrimEnd('\').ToLowerInvariant()
        if ($recordedNorm -ne $expectedNorm) {
            $failures.Add("wrong-tree cache: build/$BuildPreset/CMakeCache.txt records CMAKE_CACHEFILE_DIR=$recordedDir but should be $presetTree (cache copied/relocated from another tree)")
        }
    }
    else {
        $warnings.Add("could not read CMAKE_CACHEFILE_DIR from $presetCache (cannot verify the cache is home to this preset tree)")
    }
}

#  two-tree refusal: a concurrent root build/ cache alongside the preset cache.
if ((Test-Path -LiteralPath $rootCache) -and (Test-Path -LiteralPath $presetCache)) {
    $msg = "two configured CMake trees present concurrently: root '$rootCache' AND preset '$presetCache' (intended preset: $BuildPreset). A gate could build one and read the other."
    if ($Strict) {
        $failures.Add($msg)
    }
    else {
        $warnings.Add($msg + " [WARN-only without -Strict; retire the root build/ tree per  , then run -Strict]")
    }
}

# --- executable provenance ------------------------------------------------------------
$exePath = Join-Path (Join-Path $presetTree "bin") $ExeName
$exeHash = $null
$exeSize = $null
$exeMtime = $null
if (Test-Path -LiteralPath $exePath) {
    $exeHash = (Get-FileHash -LiteralPath $exePath -Algorithm SHA256).Hash.ToLowerInvariant()
    $exeItem = Get-Item -LiteralPath $exePath
    $exeSize = $exeItem.Length
    $exeMtime = $exeItem.LastWriteTimeUtc.ToString("o")

    if ($ExpectExeHash -and ($exeHash -ne $ExpectExeHash.ToLowerInvariant())) {
        $failures.Add("stale/wrong-binary refusal: $ExeName SHA256=$exeHash does not match expected $($ExpectExeHash.ToLowerInvariant()) (artifact was produced by a different binary than the one being gated)")
    }
}
elseif (-not $AllowMissingExe) {
    $failures.Add("missing executable under the canonical tree: $exePath (run 'cmake --build --preset $BuildPreset' first)")
}

# --- shader provenance (hash of res/shaders sources) ----------------------------------
$shaderDir = Resolve-RepoPath "res/shaders"
$shaderHash = "none"
$shaderCount = 0
if (Test-Path -LiteralPath $shaderDir) {
    $shaderFiles = Get-ChildItem -LiteralPath $shaderDir -Recurse -File -Include *.frag, *.vert, *.comp, *.glsl, *.geom, *.tesc, *.tese -ErrorAction SilentlyContinue |
        Sort-Object FullName
    if ($shaderFiles) {
        $shaderCount = @($shaderFiles).Count
        $sb = New-Object System.Text.StringBuilder
        foreach ($f in $shaderFiles) {
            $rel = $f.FullName.Substring($RepoRoot.Length).TrimStart('\', '/') -replace '\\', '/'
            $h = (Get-FileHash -LiteralPath $f.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
            [void]$sb.AppendLine("$rel`:$h")
        }
        $bytes = [System.Text.Encoding]::UTF8.GetBytes($sb.ToString())
        $sha = [System.Security.Cryptography.SHA256]::Create()
        try {
            $shaderHash = ([System.BitConverter]::ToString($sha.ComputeHash($bytes)) -replace '-', '').ToLowerInvariant()
        }
        finally { $sha.Dispose() }
    }
}

# --- git provenance -------------------------------------------------------------------
$gitSha = "unknown"
$rawSha = & git -C $RepoRoot rev-parse HEAD
if ($LASTEXITCODE -eq 0 -and $rawSha) {
    $gitSha = ($rawSha | Select-Object -First 1).Trim()
}
else {
    $warnings.Add("could not resolve git HEAD (git absent or not a repo); git_sha=unknown")
}
if ($ExpectGitSha -and ($gitSha -ne "unknown")) {
    if (-not $gitSha.StartsWith($ExpectGitSha) -and -not $ExpectGitSha.StartsWith($gitSha)) {
        $failures.Add("git SHA mismatch: HEAD=$gitSha does not match expected $ExpectGitSha")
    }
}

# --- emit the manifest (: six provenance fields) ------------------------------
if (-not $ManifestOut) {
    $ManifestOut = Join-Path (Join-Path $presetTree "test-artifacts") "build-tree-manifest.json"
}
$manifestParent = Split-Path -Parent $ManifestOut
if ($manifestParent -and -not (Test-Path -LiteralPath $manifestParent)) {
    New-Item -ItemType Directory -Force -Path $manifestParent | Out-Null
}

$manifest = [ordered]@{
    schema       = "luminumbra.build-tree-manifest/v1"
    build_preset = $BuildPreset
    git_sha      = $gitSha
    exe_name     = $ExeName
    exe_path     = ($exePath -replace '\\', '/')
    exe_hash     = $exeHash
    exe_size     = $exeSize
    exe_mtime    = $exeMtime
    shader_hash  = $shaderHash
    shader_count = $shaderCount
    scenario     = $Scenario
    timestamp    = ([DateTime]::UtcNow.ToString("o"))
    binary_dir   = ($presetTree -replace '\\', '/')
    trees_found  = @($treesFound)
    strict       = [bool]$Strict
    warnings     = @($warnings)
    ok           = ($failures.Count -eq 0)
}
$manifest | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath $ManifestOut -Encoding UTF8

# --- report ---------------------------------------------------------------------------
Write-Host "[validate-build-tree] preset=$BuildPreset canonical=$($manifest.binary_dir)"
Write-Host "[validate-build-tree] git_sha=$gitSha exe_hash=$exeHash shader_hash=$shaderHash ($shaderCount shaders)"
Write-Host "[validate-build-tree] trees_found=[$([string]::Join(', ', $treesFound))]"
Write-Host "[validate-build-tree] manifest -> $ManifestOut"
foreach ($w in $warnings) { Write-Warning $w }

if ($failures.Count -gt 0) {
    Write-Host ""
    Write-Host "[validate-build-tree] FAIL ($($failures.Count) reason(s)):" -ForegroundColor Red
    foreach ($f in $failures) { Write-Host "  - $f" -ForegroundColor Red }
    exit 1
}

Write-Host "[validate-build-tree] PASS -- canonical preset tree build/$BuildPreset is the single source of truth." -ForegroundColor Green
exit 0
