param(
    [string]$BuildPreset = "debug"
)

$ErrorActionPreference = "Stop"

$SourcePath = "src/luminumbra_common/scripting/LuaApiManifest.cpp"
$HeaderPath = "src/luminumbra_common/scripting/LuaApiManifest.h"
$LuaStateHeaderPath = "src/luminumbra_common/scripting/LuaState.h"
$CommonSourcesPath = "src/luminumbra_common/sources.cmake"
$TestSourcesPath = "test/sources.cmake"
$ArtifactDir = "build/$BuildPreset/test-artifacts/scripting"
$ArtifactPath = Join-Path $ArtifactDir "lua-api-manifest.json"

$RequiredEntries = @(
    [pscustomobject]@{ module = "core"; name = "log"; qualified = "core.log" },
    [pscustomobject]@{ module = "core"; name = "version"; qualified = "core.version" },
    [pscustomobject]@{ module = "entity"; name = "destroy"; qualified = "entity.destroy" },
    [pscustomobject]@{ module = "entity"; name = "spawn"; qualified = "entity.spawn" },
    [pscustomobject]@{ module = "simulation"; name = "emit_event"; qualified = "simulation.emit_event" },
    [pscustomobject]@{ module = "simulation"; name = "subscribe"; qualified = "simulation.subscribe" },
    [pscustomobject]@{ module = "time"; name = "delta_seconds"; qualified = "time.delta_seconds" },
    [pscustomobject]@{ module = "world"; name = "get_block"; qualified = "world.get_block" },
    [pscustomobject]@{ module = "world"; name = "sample_energy_field"; qualified = "world.sample_energy_field" },
    [pscustomobject]@{ module = "world"; name = "set_block"; qualified = "world.set_block" }
)

$Checks = New-Object System.Collections.Generic.List[object]

function Add-Check {
    param(
        [string]$Name,
        [bool]$Passed,
        [string]$Detail = ""
    )

    $Checks.Add([pscustomobject][ordered]@{
        name = $Name
        passed = $Passed
        detail = $Detail
    }) | Out-Null
}

foreach ($path in @($SourcePath, $HeaderPath, $LuaStateHeaderPath, $CommonSourcesPath, $TestSourcesPath)) {
    if (-not (Test-Path $path)) {
        throw "Missing Lua API manifest gate input: $path"
    }
}

$SourceText = Get-Content $SourcePath -Raw
$HeaderText = Get-Content $HeaderPath -Raw
$LuaStateHeaderText = Get-Content $LuaStateHeaderPath -Raw
$CommonSourcesText = Get-Content $CommonSourcesPath -Raw
$TestSourcesText = Get-Content $TestSourcesPath -Raw

Add-Check `
    -Name "manifest schema declared" `
    -Passed ($SourceText -match [regex]::Escape("luminumbra.scripting.lua_api_manifest.v1")) `
    -Detail "LuaApiManifest.cpp must declare the stable manifest schema."

Add-Check `
    -Name "manifest header declares API descriptors" `
    -Passed (($HeaderText -match "LuaApiManifestEntry") -and ($HeaderText -match "GetLuaApiManifest") -and ($HeaderText -match "SerializeLuaApiManifestJson") -and ($HeaderText -match "LuaApiManifestMeetsBaseline")) `
    -Detail "LuaApiManifest.h must expose descriptor, serializer, and baseline validation APIs."

Add-Check `
    -Name "LuaState exposes manifest API" `
    -Passed ($LuaStateHeaderText -match "api_manifest") `
    -Detail "LuaState must expose the manifest for script host integration."

$MissingEntries = @()
foreach ($entry in $RequiredEntries) {
    $entryLiteral = '{"' + $entry.module + '", "' + $entry.name + '"'
    if ($SourceText -notmatch [regex]::Escape($entryLiteral)) {
        $MissingEntries += $entry.qualified
    }
}

if ($MissingEntries.Count -eq 0) {
    $MissingEntryDetail = "Missing entries: none"
} else {
    $MissingEntryDetail = "Missing entries: " + ($MissingEntries -join ", ")
}

Add-Check `
    -Name "manifest source lists required entries" `
    -Passed ($MissingEntries.Count -eq 0) `
    -Detail $MissingEntryDetail

Add-Check `
    -Name "manifest entries are wired into common sources" `
    -Passed ($CommonSourcesText -match [regex]::Escape("scripting/LuaApiManifest.cpp")) `
    -Detail "src/luminumbra_common/sources.cmake must compile LuaApiManifest.cpp."

Add-Check `
    -Name "manifest gate test is wired into test sources" `
    -Passed (($TestSourcesText -match "SCRIPTING_TEST_SOURCES") -and ($TestSourcesText -match [regex]::Escape("scripting/lua_api_manifest_gate_test.cpp"))) `
    -Detail "test/sources.cmake must register the scripting manifest gate test source."

Add-Check `
    -Name "manifest serializer emits deterministic order contract" `
    -Passed (($SourceText -match [regex]::Escape("module_then_name")) -and ($SourceText -match "SerializeLuaApiManifestJson")) `
    -Detail "The serializer must publish the deterministic order contract."

$Passed = @($Checks | Where-Object { -not $_.passed }).Count -eq 0
$RequiredEntryNames = @($RequiredEntries | ForEach-Object { $_.qualified })

New-Item -ItemType Directory -Force -Path $ArtifactDir | Out-Null

$Artifact = [ordered]@{
    schema = "luminumbra.scripting.lua_api_manifest.v1"
    build_preset = $BuildPreset
    passed = $Passed
    manifest = [ordered]@{
        source = $SourcePath
        header = $HeaderPath
        lua_state_header = $LuaStateHeaderPath
        serializer = "SerializeLuaApiManifestJson"
        validation_api = "LuaApiManifestMeetsBaseline"
        deterministic_order = "module_then_name"
        entry_count = $RequiredEntries.Count
        required_modules = @("core", "entity", "simulation", "time", "world")
        required_entries = $RequiredEntryNames
    }
    checks = $Checks.ToArray()
}

$Artifact | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $ArtifactPath -Encoding UTF8

if (-not $Passed) {
    throw "Lua API manifest gate failed; see $ArtifactPath"
}

Write-Host "lua api manifest gate passed: $ArtifactPath"
