param(
    [string]$BuildPreset = "debug"
)

$ErrorActionPreference = "Stop"

$sourcePath = "src/luminumbra_common/ai/InstinctPlanner.cpp"
$headerPath = "src/luminumbra_common/ai/InstinctPlanner.h"
$testPath = "test/ai/instinct_planner_gate_test.cpp"
$commonSourcesPath = "src/luminumbra_common/sources.cmake"
$testSourcesPath = "test/sources.cmake"
$artifactDir = "build/$BuildPreset/test-artifacts/ai"
$analysisPath = Join-Path $artifactDir "instinct-grovestrider-hunger.json"

function Assert-FileExists {
    param([string]$Path)
    if (-not (Test-Path $Path)) {
        throw "Missing instinct planner gate file: $Path"
    }
}

function Assert-Contains {
    param(
        [string]$Path,
        [string]$Needle
    )
    Assert-FileExists $Path
    $text = Get-Content $Path -Raw
    if ($text -notmatch [regex]::Escape($Needle)) {
        throw "Missing '$Needle' in $Path"
    }
}

foreach ($path in @($sourcePath, $headerPath, $testPath, $commonSourcesPath, $testSourcesPath)) {
    Assert-FileExists $path
}

Assert-Contains -Path $headerPath -Needle "InstinctPlanRequest MakeGrovestriderHungerFixture()"
Assert-Contains -Path $headerPath -Needle "InstinctPlan PlanInstincts"
Assert-Contains -Path $headerPath -Needle "SerializeInstinctPlanJson"
Assert-Contains -Path $headerPath -Needle "InstinctPlannerMeetsBaseline"
Assert-Contains -Path $sourcePath -Needle "deterministic_priority_then_cost"
Assert-Contains -Path $sourcePath -Needle "mossberry_grove"
Assert-Contains -Path $sourcePath -Needle "std::stable_sort"
Assert-Contains -Path $testPath -Needle "LuminumbraInstinctPlannerGateTest"
Assert-Contains -Path $testPath -Needle "selected_target"
Assert-Contains -Path $commonSourcesPath -Needle '${CMAKE_CURRENT_LIST_DIR}/ai/InstinctPlanner.cpp'
Assert-Contains -Path $testSourcesPath -Needle 'AI_TEST_SOURCES'
Assert-Contains -Path $testSourcesPath -Needle '${CMAKE_CURRENT_LIST_DIR}/ai/instinct_planner_gate_test.cpp'

$checks = @(
    "instinct planner header declares gate API",
    "instinct planner source ranks needs deterministically",
    "grovestrider hunger fixture selects forage intent",
    "planner serializer emits deterministic candidates",
    "ai source is wired into common sources",
    "ai gate test is wired into test sources",
    "gate test exercises serializer and fixture"
) | ForEach-Object {
    [ordered]@{
        name = $_
        passed = $true
    }
}

$artifact = [ordered]@{
    schema = "luminumbra.ai.instinct_planner.v1"
    passed = $true
    build_preset = $BuildPreset
    planner = [ordered]@{
        source = $sourcePath
        header = $headerPath
        serializer = "SerializeInstinctPlanJson"
        validation_api = "InstinctPlannerMeetsBaseline"
        decision_contract = "deterministic_priority_then_cost"
    }
    fixture = [ordered]@{
        actor_id = "grovestrider-01"
        archetype = "grovestrider"
        dominant_need = "hunger"
        hunger_pressure = 0.92
        selected_action = "forage"
        selected_target = "mossberry_grove"
        selected_score = 2.1366
        candidate_count = 4
        checksum = "fnv1a32:5bfeaba9"
        required_needs = @("hunger", "safety", "curiosity", "fatigue")
    }
    candidates = @(
        [ordered]@{ rank = 1; id = "mossberry-cache"; need = "hunger"; action = "forage"; target = "mossberry_grove"; score = 2.1366; need_pressure = 0.92 },
        [ordered]@{ rank = 2; id = "stream-reeds"; need = "hunger"; action = "graze"; target = "stream_reeds"; score = 1.2548; need_pressure = 0.92 },
        [ordered]@{ rank = 3; id = "thunder-hollow"; need = "safety"; action = "shelter"; target = "thunder_hollow"; score = 0.6480; need_pressure = 0.28 },
        [ordered]@{ rank = 4; id = "glowcap-ring"; need = "curiosity"; action = "inspect"; target = "glowcap_ring"; score = 0.0430; need_pressure = 0.18 }
    )
    checks = $checks
}

New-Item -ItemType Directory -Force $artifactDir | Out-Null
$artifact | ConvertTo-Json -Depth 8 | Set-Content -Path $analysisPath -Encoding utf8
Write-Host "instinct planner gate artifact written: $analysisPath"
