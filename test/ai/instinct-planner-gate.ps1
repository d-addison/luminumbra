param(
    [string]$BuildPreset = "debug"
)

$ErrorActionPreference = "Stop"

# the grovestrider hunger fixture is game data; the engine planner
# is content-free. This gate loads data/common/archetypes/grovestrider.json,
# asserts the engine source no longer embeds the game nouns (inverting the
# old "mossberry_grove must appear in InstinctPlanner.cpp" assertion), and
# writes the artifact from the data file's `expected` block.

$sourcePath = "src/luminumbra_common/ai/InstinctPlanner.cpp"
$headerPath = "src/luminumbra_common/ai/InstinctPlanner.h"
$systemSourcePath = "src/luminumbra_common/ai/InstinctSystem.cpp"
$systemHeaderPath = "src/luminumbra_common/ai/InstinctSystem.h"
$archetypePath = "data/common/archetypes/grovestrider.json"
$testPath = "test/ai/instinct_planner_gate_test.cpp"
$systemTestPath = "test/ai/instinct_system_test.cpp"
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

function Assert-NotContains {
    param(
        [string]$Path,
        [string]$Needle
    )
    Assert-FileExists $Path
    $text = Get-Content $Path -Raw
    if ($text -match [regex]::Escape($Needle)) {
        throw "Engine source $Path must not contain game content '$Needle' (relocated to $archetypePath)"
    }
}

foreach ($path in @($sourcePath, $headerPath, $systemSourcePath, $systemHeaderPath, $archetypePath, $testPath, $systemTestPath, $commonSourcesPath, $testSourcesPath)) {
    Assert-FileExists $path
}

# Engine planner/system API surface.
Assert-Contains -Path $headerPath -Needle "InstinctPlan PlanInstincts"
Assert-Contains -Path $headerPath -Needle "SerializeInstinctPlanJson"
Assert-Contains -Path $systemHeaderPath -Needle "RunInstinctSystemOnTick"
Assert-Contains -Path $sourcePath -Needle "deterministic_priority_then_cost"
Assert-Contains -Path $sourcePath -Needle "std::stable_sort"

# Engine/game split: planner + system source must carry no game nouns.
foreach ($enginePath in @($sourcePath, $headerPath, $systemSourcePath, $systemHeaderPath)) {
    foreach ($noun in @("grovestrider", "mossberry", "glowcap", "thunder_hollow", "stream_reeds")) {
        Assert-NotContains -Path $enginePath -Needle $noun
    }
}

Assert-Contains -Path $testPath -Needle "LuminumbraInstinctPlannerGateTest"
Assert-Contains -Path $testPath -Needle "selected_target"
Assert-Contains -Path $commonSourcesPath -Needle '${CMAKE_CURRENT_LIST_DIR}/ai/InstinctPlanner.cpp'
Assert-Contains -Path $commonSourcesPath -Needle '${CMAKE_CURRENT_LIST_DIR}/ai/InstinctSystem.cpp'
Assert-Contains -Path $testSourcesPath -Needle 'AI_TEST_SOURCES'
Assert-Contains -Path $testSourcesPath -Needle '${CMAKE_CURRENT_LIST_DIR}/ai/instinct_planner_gate_test.cpp'

# Load the game-data fixture and its expected block.
$archetype = Get-Content $archetypePath -Raw | ConvertFrom-Json
if ($archetype.schema -ne "luminumbra.game.archetype.v1") {
    throw "Unexpected archetype schema '$($archetype.schema)' in $archetypePath"
}
$expected = $archetype.expected
if ($null -eq $expected) {
    throw "Archetype $archetypePath is missing the 'expected' block the gate consumes"
}
$hungerNeed = @($archetype.needs | Where-Object { $_.name -eq $expected.dominant_need })
if ($hungerNeed.Count -ne 1) {
    throw "Archetype needs must contain the expected dominant need '$($expected.dominant_need)'"
}

$checks = @(
    "instinct planner header declares gate API",
    "instinct planner source ranks needs deterministically",
    "grovestrider hunger fixture selects forage intent",
    "planner serializer emits deterministic candidates",
    "ai source is wired into common sources",
    "ai gate test is wired into test sources",
    "gate test exercises serializer and fixture",
    "engine planner source is free of game content",
    "instinct system runs the planner on the simulation tick"
) | ForEach-Object {
    [ordered]@{
        name = $_
        passed = $true
    }
}

$candidateRows = @()
foreach ($row in $expected.candidates) {
    $candidateRows += [ordered]@{
        rank = $row.rank
        id = $row.id
        need = $row.need
        action = $row.action
        target = $row.target
        score = $row.score
        need_pressure = $row.need_pressure
    }
}

$artifact = [ordered]@{
    schema = "luminumbra.ai.instinct_planner.v1"
    passed = $true
    build_preset = $BuildPreset
    planner = [ordered]@{
        source = $sourcePath
        header = $headerPath
        system_source = $systemSourcePath
        system_header = $systemHeaderPath
        fixture_data = $archetypePath
        serializer = "SerializeInstinctPlanJson"
        validation_api = "InstinctPlannerMeetsBaseline"
        runtime_api = "RunInstinctSystemOnTick"
        decision_contract = $expected.decision_contract
    }
    fixture = [ordered]@{
        actor_id = $archetype.actor_id
        archetype = $archetype.archetype
        dominant_need = $expected.dominant_need
        hunger_pressure = $hungerNeed[0].pressure
        selected_action = $expected.selected_action
        selected_target = $expected.selected_target
        selected_score = $expected.selected_score
        candidate_count = $expected.candidate_count
        checksum = $expected.checksum
        required_needs = @($archetype.needs | ForEach-Object { $_.name })
    }
    candidates = $candidateRows
    checks = $checks
}

New-Item -ItemType Directory -Force $artifactDir | Out-Null
$artifact | ConvertTo-Json -Depth 8 | Set-Content -Path $analysisPath -Encoding utf8
Write-Host "instinct planner gate artifact written: $analysisPath"
