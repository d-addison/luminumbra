<#
.SYNOPSIS
    File-only round-trip artifact checks; NEEDS-NATIVE-RUN in both supported shells.
.DESCRIPTION
    Extracts two function definitions without executing the lifecycle dispatcher.
    Never launches the client, builds, or accesses the network.
#>
[CmdletBinding()]
param()

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

function Import-FileFunction {
    param([string]$Path, [string]$Name)
    $tokens = $null
    $errors = $null
    $ast = [System.Management.Automation.Language.Parser]::ParseFile($Path, [ref]$tokens, [ref]$errors)
    if ($errors.Count -ne 0) { throw "parse errors in $Path" }
    $definitions = @($ast.FindAll({ param($node)
        $node -is [System.Management.Automation.Language.FunctionDefinitionAst] -and
        $node.Name -ceq $Name
    }, $true))
    if ($definitions.Count -ne 1) { throw "expected one $Name definition in $Path" }
    return [scriptblock]::Create($definitions[0].Extent.Text)
}

. (Import-FileFunction (Join-Path $PSScriptRoot "lib/AcceptanceCommon.ps1") "Test-JsonProperty")
. (Import-FileFunction (Join-Path $PSScriptRoot "run-saved-world-lifecycle.ps1") "Get-EditRoundtrip")

$fixture = Join-Path ([System.IO.Path]::GetTempPath()) ([System.Guid]::NewGuid().ToString())
$hash = "0123456789abcdef"
$schema = "luminumbra.persistence_runtime_roundtrip_phase.v1"

function Write-Phase {
    param([string]$Phase)
    $doc = @{ schema = $schema; phase = $Phase; world_hash = $hash }
    if ($Phase -ceq "save") { $doc.edited_chunk_ids = @(1) }
    $path = Join-Path $fixture ("persistence-runtime-roundtrip-phase-" + $Phase + ".json")
    [System.IO.File]::WriteAllText($path, ($doc | ConvertTo-Json))
    return $path
}

function Assert-InvalidPhase {
    param([string]$Path)
    $message = $null
    try { Get-EditRoundtrip $fixture | Out-Null } catch { $message = $_.Exception.Message }
    if ($null -eq $message -or -not $message.Contains($Path)) {
        throw "expected failure identifying invalid artifact $Path; got $message"
    }
}

try {
    New-Item -ItemType Directory -Path $fixture | Out-Null
    if ($null -ne (Get-EditRoundtrip $fixture)) { throw "absent artifacts must be unevaluated" }
    foreach ($phase in @("save", "load")) {
        # The partner is absent throughout: it must not hide this invalid file.
        $path = Write-Phase $phase
        if ($null -ne (Get-EditRoundtrip $fixture)) { throw "one valid phase must be unevaluated" }
        foreach ($invalid in @(
            "{", "null", "[]",
            ('[{"schema":"' + $schema + '","phase":"' + $phase + '","world_hash":"' + $hash + '","edited_chunk_ids":[1]}]'),
            '{"schema":"wrong","phase":"save"}',
            ('{"schema":"' + $schema + '","phase":"wrong"}'),
            ('{"schema":"' + $schema + '","phase":"' + $phase + '"}'),
            ('{"schema":"' + $schema + '","phase":"' + $phase + '","world_hash":"bad","edited_chunk_ids":[1]}')
        )) {
            [System.IO.File]::WriteAllText($path, $invalid)
            Assert-InvalidPhase $path
        }
        Remove-Item -LiteralPath $path
        New-Item -ItemType Directory -Path $path | Out-Null
        Assert-InvalidPhase $path
        Remove-Item -LiteralPath $path
    }
    $save = Write-Phase "save"
    $load = Write-Phase "load"
    foreach ($chunks in @('null', '[]', '1')) {
        [System.IO.File]::WriteAllText($save,
            ('{"schema":"' + $schema + '","phase":"save","world_hash":"' + $hash + '","edited_chunk_ids":' + $chunks + '}'))
        Assert-InvalidPhase $save
    }
    $save = Write-Phase "save"
    $summary = Get-EditRoundtrip $fixture
    if ($summary.save_phase_schema -cne $schema -or $summary.load_phase_schema -cne $schema -or
        $summary.save_world_hash -cne $hash -or $summary.load_world_hash -cne $hash -or
        $summary.edited_chunk_count -ne 1) { throw "valid artifact pair must produce the expected summary" }
    Write-Host "saved-world round-trip artifact checks: PASS"
} finally {
    if (Test-Path -LiteralPath $fixture) { Remove-Item -LiteralPath $fixture -Recurse -Force }
}
