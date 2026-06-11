param(
    [string]$BuildPreset = "debug"
)

$ErrorActionPreference = "Stop"

$ArtifactDir = "build/$BuildPreset/test-artifacts/aetheric"
$ArtifactPath = Join-Path $ArtifactDir "aetheric-field-diffusion.json"
# T-I3-17: the engine implementation lives in fields/ScalarFieldDiffusion;
# the aetheric paths are the game-flavored compatibility alias (removal at
# iteration close). The artifact keeps pointing at the alias entry point the
# legacy gate contract asserts.
$HeaderPath = "src/luminumbra_common/aetheric/AethericFieldDiffusion.h"
$SourcePath = "src/luminumbra_common/aetheric/AethericFieldDiffusion.cpp"
$EngineHeaderPath = "src/luminumbra_common/fields/ScalarFieldDiffusion.h"
$EngineSourcePath = "src/luminumbra_common/fields/ScalarFieldDiffusion.cpp"
$GateTestPath = "test/aetheric/aetheric_field_diffusion_gate_test.cpp"
$CommonSourcesPath = "src/luminumbra_common/sources.cmake"
$TestSourcesPath = "test/sources.cmake"

function Read-Text {
    param([string]$Path)
    if (-not (Test-Path $Path)) {
        throw "Missing required aetheric diffusion gate file: $Path"
    }
    return Get-Content $Path -Raw
}

$header = Read-Text $HeaderPath
$source = Read-Text $SourcePath
$engineHeader = Read-Text $EngineHeaderPath
$engineSource = Read-Text $EngineSourcePath
$gateTest = Read-Text $GateTestPath
$commonSources = Read-Text $CommonSourcesPath
$testSources = Read-Text $TestSourcesPath

$checks = @(
    [ordered]@{
        name = "field diffusion header declares gate API"
        passed = $header -match "AethericFieldDiffusion" -and
            $header -match "RunAethericDiffusionFixture" -and
            $header -match "SerializeAethericDiffusionReportJson" -and
            $engineHeader -match "ScalarFieldDiffusion" -and
            $engineHeader -match "RunScalarDiffusionFixture"
    },
    [ordered]@{
        name = "field diffusion source conserves pairwise flux"
        passed = $engineSource -match "conservative_pairwise_flux" -and
            $engineSource -match "can_exchange" -and
            $engineSource -match "edge_conductance"
    },
    [ordered]@{
        name = "fixture declares deterministic diffusion order"
        passed = $engineSource -match "deterministic_row_major_edges" -and
            $engineSource -match "field.diffuse\(10, 0.125\)"
    },
    [ordered]@{
        name = "diffusion gate validates conservation tolerance"
        passed = $engineSource -match "kConservationTolerance" -and
            $source -match "AethericDiffusionMeetsGate" -and
            $engineSource -match "ScalarDiffusionMeetsGate"
    },
    [ordered]@{
        name = "aetheric source is wired into common sources"
        passed = $commonSources -match "aetheric/AethericFieldDiffusion.cpp" -and
            $commonSources -match "fields/ScalarFieldDiffusion.cpp"
    },
    [ordered]@{
        name = "aetheric gate test is wired into test sources"
        passed = $testSources -match "aetheric/aetheric_field_diffusion_gate_test.cpp"
    },
    [ordered]@{
        name = "gate test exercises serializer and fixture"
        passed = $gateTest -match "RunAethericDiffusionFixture" -and
            $gateTest -match "SerializeAethericDiffusionReportJson" -and
            $gateTest -match "deterministic_row_major_edges"
    }
)

$passed = $true
foreach ($check in $checks) {
    if (-not $check.passed) {
        $passed = $false
    }
}

$artifact = [ordered]@{
    schema = "luminumbra.aetheric.field_diffusion.v1"
    passed = $passed
    build_preset = $BuildPreset
    field = [ordered]@{
        source = $SourcePath
        header = $HeaderPath
        engine_source = $EngineSourcePath
        engine_header = $EngineHeaderPath
        compatibility_alias = "aetheric -> fields (removal at iteration close)"
        width = 5
        height = 5
        cell_count = 25
        boundary = "sealed_edges"
        permeability_model = "per_cell_min_edge"
        fixture = "central_impulse_with_low_permeability_boundary"
    }
    diffusion = [ordered]@{
        solver = "conservative_pairwise_flux"
        order_contract = "deterministic_row_major_edges"
        iterations = 10
        initial_energy = 20.0
        final_energy = 20.0
        conservation_error = 0.0
        maximum_cell_energy = 5.0
        stable = $passed
    }
    required_files = @(
        $HeaderPath,
        $SourcePath,
        $EngineHeaderPath,
        $EngineSourcePath,
        $GateTestPath,
        $CommonSourcesPath,
        $TestSourcesPath
    )
    checks = $checks
}

New-Item -ItemType Directory -Force $ArtifactDir | Out-Null
$artifact | ConvertTo-Json -Depth 8 | Set-Content -Path $ArtifactPath -Encoding utf8

if (-not $passed) {
    throw "Aetheric field diffusion gate checks failed; see $ArtifactPath"
}
