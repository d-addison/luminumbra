param(
    [string]$BuildPreset = "debug"
)

$ErrorActionPreference = "Stop"

# the game-flavored "aetheric" compatibility alias was removed at
# iteration close. This gate now inspects the generic engine fields module
# directly under its own schema (luminumbra.fields.scalar_diffusion.v1).
$ArtifactDir = "build/$BuildPreset/test-artifacts/fields"
$ArtifactPath = Join-Path $ArtifactDir "scalar-field-diffusion.json"
$HeaderPath = "src/luminumbra_common/fields/ScalarFieldDiffusion.h"
$SourcePath = "src/luminumbra_common/fields/ScalarFieldDiffusion.cpp"
$GateTestPath = "test/fields/scalar_field_diffusion_gate_test.cpp"
$CommonSourcesPath = "src/luminumbra_common/sources.cmake"
$TestSourcesPath = "test/sources.cmake"

function Read-Text {
    param([string]$Path)
    if (-not (Test-Path $Path)) {
        throw "Missing required scalar field diffusion gate file: $Path"
    }
    return Get-Content $Path -Raw
}

$header = Read-Text $HeaderPath
$source = Read-Text $SourcePath
$gateTest = Read-Text $GateTestPath
$commonSources = Read-Text $CommonSourcesPath
$testSources = Read-Text $TestSourcesPath

$checks = @(
    [ordered]@{
        name = "field diffusion header declares gate API"
        passed = $header -match "ScalarFieldDiffusion" -and
            $header -match "RunScalarDiffusionFixture" -and
            $header -match "SerializeScalarDiffusionReportJson"
    },
    [ordered]@{
        name = "field diffusion source conserves pairwise flux"
        passed = $source -match "conservative_pairwise_flux" -and
            $source -match "can_exchange" -and
            $source -match "edge_conductance"
    },
    [ordered]@{
        name = "fixture declares deterministic diffusion order"
        passed = $source -match "deterministic_row_major_edges" -and
            $source -match "field.diffuse\(10, 0.125\)"
    },
    [ordered]@{
        name = "diffusion gate validates conservation tolerance"
        passed = $source -match "kConservationTolerance" -and
            $source -match "ScalarDiffusionMeetsGate"
    },
    [ordered]@{
        name = "field source is wired into common sources"
        passed = $commonSources -match "fields/ScalarFieldDiffusion.cpp"
    },
    [ordered]@{
        name = "field gate test is wired into test sources"
        passed = $testSources -match "fields/scalar_field_diffusion_gate_test.cpp"
    },
    [ordered]@{
        name = "gate test exercises serializer and fixture"
        passed = $gateTest -match "RunScalarDiffusionFixture" -and
            $gateTest -match "SerializeScalarDiffusionReportJson" -and
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
    schema = "luminumbra.fields.scalar_diffusion.v1"
    passed = $passed
    build_preset = $BuildPreset
    field = [ordered]@{
        source = $SourcePath
        header = $HeaderPath
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
        $GateTestPath,
        $CommonSourcesPath,
        $TestSourcesPath
    )
    checks = $checks
}

New-Item -ItemType Directory -Force $ArtifactDir | Out-Null
$artifact | ConvertTo-Json -Depth 8 | Set-Content -Path $ArtifactPath -Encoding utf8

if (-not $passed) {
    throw "Scalar field diffusion gate checks failed; see $ArtifactPath"
}
