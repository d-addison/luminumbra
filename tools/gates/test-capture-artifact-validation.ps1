<#
.SYNOPSIS
    File-only adversarial tests. Run in a clean PowerShell process with -NoProfile.
.DESCRIPTION
    Loads only the helper import declared by the gate. Never invokes the gate,
    engine, Blender, network, or graphics APIs. Owns one temporary fixture folder.
#>
[CmdletBinding()]
param()

$ErrorActionPreference = "Stop"
Set-StrictMode -Version 2.0
$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$repo = [System.IO.Path]::GetFullPath((Join-Path $ScriptDir "../.."))
$gate = Join-Path $ScriptDir "validate-engine-frontier.ps1"
$checks = 0

function Assert-Test {
    param([bool]$Condition, [string]$Because)
    if (-not $Condition) { throw "FAILED: $Because" }
    $script:checks++
}

function Assert-Rejected {
    param([scriptblock]$Action, [string]$Artifact, [string]$Because)
    $message = $null
    try { & $Action | Out-Null } catch { $message = $_.Exception.Message }
    Assert-Test ($null -ne $message) "$Because must be refused"
    Assert-Test ($message.Contains("FixtureScenario")) "$Because must identify the scenario: $message"
    Assert-Test ($message.Contains($Artifact)) "$Because must identify the artifact: $message"
}

# Parse (never run) the dispatcher and load its exact one-line helper import.
$gateText = Get-Content -LiteralPath $gate -Raw
$imports = @($gateText -split "`n" | Where-Object {
    $_.Trim() -ceq '. (Join-Path $ScriptDir "capture-artifact-validation.ps1")'
})
Assert-Test ($imports.Count -eq 1) "gate imports the helper exactly once"
$preferenceBefore = $ErrorActionPreference
$loadOutput = @(. ([scriptblock]::Create($imports[0])))
Assert-Test ($loadOutput.Count -eq 0) "helper import emits no output"
Assert-Test ($ErrorActionPreference -eq $preferenceBefore) "helper import preserves caller preferences"
foreach ($command in @("Assert-PpmArtifact", "Assert-CapturePinned")) {
    $resolved = Get-Command $command -CommandType Function -ErrorAction Stop
    Assert-Test ($resolved.ScriptBlock.File -eq (Join-Path $ScriptDir "capture-artifact-validation.ps1")) "clean import resolves $command from the helper"
}

$tokens = $null
$parseErrors = $null
$ast = [System.Management.Automation.Language.Parser]::ParseFile($gate, [ref]$tokens, [ref]$parseErrors)
Assert-Test ($parseErrors.Count -eq 0) "gate parses without executing its mode dispatcher"
$pinCalls = @($ast.FindAll({ param($node)
    $node -is [System.Management.Automation.Language.CommandAst] -and $node.GetCommandName() -eq "Assert-CapturePinned"
}, $true))
Assert-Test ($pinCalls.Count -ge 8) "all existing pin checks remain registered"
foreach ($call in $pinCalls) {
    $names = @($call.CommandElements | Where-Object { $_ -is [System.Management.Automation.Language.CommandParameterAst] } | ForEach-Object { $_.ParameterName })
    Assert-Test ($names -contains "ScreenshotPaths") "pin call at line $($call.Extent.StartLineNumber) joins explicit screenshot paths"
}

$profile = Get-CapturePinProfile
$config = Get-Content -LiteralPath (Join-Path $repo "src/luminumbra_client/core/RuntimeScenarioConfig.h") -Raw
foreach ($dimension in @("Width", "Height")) {
    $match = [regex]::Match($config, ('kCapturePinned' + $dimension + '\s*=\s*([0-9]+)\s*;'))
    Assert-Test ($match.Success -and [int]$match.Groups[1].Value -eq $profile.$dimension) "trusted profile $dimension matches the native source contract"
}

$fixtureRoot = Join-Path ([System.IO.Path]::GetTempPath()) ("luminumbra-capture-contract-" + [guid]::NewGuid().ToString("N"))
$run = Join-Path $fixtureRoot "run"
$metadata = Join-Path $run "last-known-runtime.json"

function Write-PpmFixture {
    param([string]$Path, [string]$Header = "P6`n1 1`n255`n", [long]$PayloadBytes = 3,
        [byte[]]$FirstBytes = @(1, 2, 3))
    $stream = [System.IO.File]::Create($Path)
    try {
        $headerBytes = [System.Text.Encoding]::ASCII.GetBytes($Header)
        $stream.Write($headerBytes, 0, $headerBytes.Length)
        $writeCount = [int][Math]::Min($PayloadBytes, $FirstBytes.Length)
        $stream.Write($FirstBytes, 0, $writeCount)
        $stream.SetLength($headerBytes.Length + $PayloadBytes)
    } finally { $stream.Dispose() }
}

function New-StateFixture {
    return [ordered]@{
        schema = "luminumbra.runtime_state.v1"
        capture_pin = [ordered]@{
            capture_width = 3840; capture_height = 1600
            pinned_width = 3840; pinned_height = 1600
            pinned = $true; window_mode = "headless"
        }
    }
}

function Write-StateFixture {
    param([object]$State)
    $json = ConvertTo-Json -InputObject $State -Depth 10
    [System.IO.File]::WriteAllText($metadata, $json, (New-Object System.Text.UTF8Encoding($false)))
}

function Assert-RunPinned {
    param([object[]]$Paths = @("pinned.ppm"))
    Assert-CapturePinned -ArtifactDir $run -Name "FixtureScenario" -ScreenshotPaths $Paths
}

try {
    New-Item -ItemType Directory -Path $run | Out-Null
    $small = Join-Path $run "small.ppm"
    Write-PpmFixture $small
    $result = Assert-PpmArtifact $small -Name "FixtureScenario" -PassThru
    Assert-Test ($result.Width -eq 1 -and $result.Height -eq 1 -and $result.PayloadBytes -eq 3 -and $result.RasterOffset -eq 11) "valid engine P6 returns dimensions deliberately"
    Assert-Test (@(Assert-PpmArtifact $small).Count -eq 0) "legacy positional assertion emits no pipeline values"
    foreach ($first in @(9, 10, 13, 32, 35)) {
        foreach ($header in @("P6`n1 1`n255`n", "P6`r`n# header comment`r`n1 # width comment`r`n1`r`n255`r`n")) {
            Write-PpmFixture $small -Header $header -FirstBytes @($first, 2, 3)
            $parsed = Assert-PpmArtifact $small -Name "FixtureScenario" -PassThru
            Assert-Test ($parsed.RasterOffset -eq [System.Text.Encoding]::ASCII.GetByteCount($header)) "first raster byte $first survives LF and CRLF headers"
        }
    }
    $badCases = @(
        @{ Name = "empty"; Header = ""; Bytes = 0 },
        @{ Name = "wrong-magic"; Header = "P3`n1 1`n255`n"; Bytes = 3 },
        @{ Name = "zero"; Header = "P6`n0 1`n255`n"; Bytes = 3 },
        @{ Name = "negative"; Header = "P6`n-1 1`n255`n"; Bytes = 3 },
        @{ Name = "overflow"; Header = "P6`n9223372036854775808 1`n255`n"; Bytes = 3 },
        @{ Name = "dimension-limit"; Header = "P6`n16385 1`n255`n"; Bytes = 3 },
        @{ Name = "pixel-limit"; Header = "P6`n16384 16384`n255`n"; Bytes = 3 },
        @{ Name = "decimal"; Header = "P6`n1.0 1`n255`n"; Bytes = 3 },
        @{ Name = "maxval"; Header = "P6`n1 1`n65535`n"; Bytes = 3 },
        @{ Name = "truncated"; Header = "P6`n1 1`n255`n"; Bytes = 2 },
        @{ Name = "truncated-crlf"; Header = "P6`r`n1 1`r`n255`r`n"; Bytes = 2 },
        @{ Name = "lone-cr"; Header = "P6`n1 1`n255`r"; Bytes = 3 },
        @{ Name = "extra"; Header = "P6`n1 1`n255`n"; Bytes = 4 },
        @{ Name = "comment-after-maxval"; Header = "P6`n1 1`n255#invalid`n"; Bytes = 3 },
        @{ Name = "unbounded-header"; Header = ("P6`n#" + ("x" * 4096) + "`n1 1`n255`n"); Bytes = 3 }
    )
    foreach ($case in $badCases) {
        $path = Join-Path $run ($case.Name + ".ppm")
        Write-PpmFixture $path -Header $case.Header -PayloadBytes $case.Bytes
        Assert-Rejected { Assert-PpmArtifact $path -Name "FixtureScenario" } $path $case.Name
    }
    Assert-Rejected { Assert-PpmArtifact (Join-Path $run "absent.ppm") -Name "FixtureScenario" } "absent.ppm" "missing file"
    Assert-Rejected { Assert-PpmArtifact $run -Name "FixtureScenario" } $run "directory as image"

    Write-PpmFixture (Join-Path $run "pinned.ppm") -Header "P6`n3840 1600`n255`n" -PayloadBytes (3840L * 1600 * 3)
    Write-StateFixture (New-StateFixture)
    Assert-Test (@(Assert-RunPinned).Count -eq 0) "matching typed metadata and image pass without output"
    Assert-Test (@(Assert-CapturePinned -ArtifactDir $run -Name "FixtureScenario").Count -eq 0) "legacy metadata-only signature remains supported"
    Assert-RunPinned -Paths @((Join-Path $run "pinned.ppm"))
    $script:checks++
    Assert-Rejected { Assert-RunPinned -Paths @("small.ppm") } "small.ppm" "pinned metadata with wrong image dimensions"
    Assert-Rejected { Assert-RunPinned -Paths @("pinned.ppm", "small.ppm") } "small.ppm" "one invalid image among explicit screenshots"
    Assert-Rejected { Assert-RunPinned -Paths @("missing.ppm") } "missing.ppm" "no recursive fallback to a different image"
    Assert-Rejected { Assert-RunPinned -Paths @() } "last-known-runtime.json" "empty screenshot list"
    Assert-Rejected { Assert-RunPinned -Paths @($null) } "last-known-runtime.json" "null screenshot path"
    Assert-Rejected { Assert-RunPinned -Paths @($true) } "last-known-runtime.json" "boolean screenshot path"
    Write-PpmFixture (Join-Path $fixtureRoot "old.ppm") -Header "P6`n3840 1600`n255`n" -PayloadBytes (3840L * 1600 * 3)
    Assert-Rejected { Assert-RunPinned -Paths @("../old.ppm") } "old.ppm" "stale sibling run path"
    Assert-Rejected { Assert-RunPinned -Paths @((Join-Path $fixtureRoot "old.ppm")) } "old.ppm" "absolute path escape"
    Assert-Rejected { Assert-CapturePinned -ArtifactDir $run -Name "FixtureScenario" -Profile "unregistered" } "last-known-runtime.json" "unknown trusted profile"
    $wrapped = "[" + (ConvertTo-Json -InputObject (New-StateFixture) -Depth 10) + "]"
    [System.IO.File]::WriteAllText($metadata, $wrapped)
    Assert-Rejected { Assert-RunPinned } "last-known-runtime.json" "one-element array cannot replace a state object"
    foreach ($json in @("{", "[]", "null", '"state"', '{"schema":"unknown"}', '{"schema":"luminumbra.runtime_state.v1"}', '{"schema":"luminumbra.runtime_state.v1","capture_pin":null}')) {
        [System.IO.File]::WriteAllText($metadata, $json)
        Assert-Rejected { Assert-RunPinned } "last-known-runtime.json" "malformed or missing runtime state"
    }
    foreach ($value in @($true, $false, "3840", 3840.0, $null, -1, 0)) {
        foreach ($field in @("capture_width", "capture_height", "pinned_width", "pinned_height")) {
            $state = New-StateFixture
            $state.capture_pin[$field] = $value
            Write-StateFixture $state
            # Windows PowerShell serializes 3840.0 as an integer: inject the
            # actual JSON decimal token to test both parsers without coercion.
            if ($value -is [double]) {
                $json = Get-Content -LiteralPath $metadata -Raw
                $json = [regex]::Replace($json, ('("' + $field + '"\s*:\s*)3840\b'), '${1}3840.0')
                [System.IO.File]::WriteAllText($metadata, $json)
            }
            Assert-Rejected { Assert-RunPinned } "last-known-runtime.json" "non-integer or nonpositive $field"
        }
    }
    foreach ($value in @("true", "false", $false, 1, $null)) {
        $state = New-StateFixture
        $state.capture_pin.pinned = $value
        Write-StateFixture $state
        Assert-Rejected { Assert-RunPinned } "last-known-runtime.json" "pin must be boolean true"
    }
    $state = New-StateFixture
    $state.capture_pin.capture_width = 1280; $state.capture_pin.pinned_width = 1280
    $state.capture_pin.capture_height = 720; $state.capture_pin.pinned_height = 720
    Write-StateFixture $state
    Assert-Rejected { Assert-RunPinned } "last-known-runtime.json" "self-consistent wrong dimensions"
    $state = New-StateFixture
    $state.capture_pin.window_mode = $true
    Write-StateFixture $state
    Assert-Rejected { Assert-RunPinned } "last-known-runtime.json" "mistyped window mode"
    Write-StateFixture (New-StateFixture)
    $json = (Get-Content -LiteralPath $metadata -Raw).Replace('"schema"', '"Schema"')
    [System.IO.File]::WriteAllText($metadata, $json)
    Assert-Rejected { Assert-RunPinned } "last-known-runtime.json" "schema field names are case-sensitive"
    $oversized = [System.IO.File]::OpenWrite($metadata)
    try { $oversized.SetLength(4194305) } finally { $oversized.Dispose() }
    Assert-Rejected { Assert-RunPinned } "last-known-runtime.json" "oversized runtime state"
    Remove-Item -LiteralPath $metadata
    Assert-Rejected { Assert-RunPinned } "last-known-runtime.json" "missing runtime metadata"
    Write-Host "CaptureArtifactValidationContract: $checks assertions passed (file-only; no visual approval)."
} finally {
    if (Test-Path -LiteralPath $fixtureRoot) { Remove-Item -LiteralPath $fixtureRoot -Recurse -Force }
}
