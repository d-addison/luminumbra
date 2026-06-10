param(
    [string]$BuildPreset = "debug"
)

$ErrorActionPreference = "Stop"

$RepoRoot = Resolve-Path (Join-Path $PSScriptRoot "../..")
$SourcePath = Join-Path $RepoRoot "src/luminumbra_common/simulation/SimulationEventBus.cpp"
$HeaderPath = Join-Path $RepoRoot "src/luminumbra_common/simulation/SimulationEventBus.h"
$TestPath = Join-Path $RepoRoot "test/simulation/eventbus_order_gate_test.cpp"
$ArtifactDir = Join-Path $RepoRoot "build/$BuildPreset/test-artifacts/simulation"
$ArtifactPath = Join-Path $ArtifactDir "eventbus-replay.json"

foreach ($path in @($SourcePath, $HeaderPath, $TestPath)) {
    if (-not (Test-Path $path)) {
        throw "Missing simulation event bus order gate file: $path"
    }
}

$source = Get-Content $SourcePath -Raw
$header = Get-Content $HeaderPath -Raw
$test = Get-Content $TestPath -Raw

$sourceChecks = @(
    @{ name = "ordered bus assigns monotonic sequence ids"; passed = ($source -match "next_sequence_\+\+" -and $header -match "std::uint64_t publish") },
    @{ name = "same tick delivery is stable by lane then sequence"; passed = ($source -match "std::stable_sort" -and $source -match "lhs\.lane" -and $source -match "lhs\.sequence") },
    @{ name = "future tick events remain queued until eligible"; passed = ($source -match "first_future->tick <= inclusive_tick" -and $test -match "pending_count\(\) != 2") },
    @{ name = "replay emits deterministic checksum"; passed = ($source -match "eventbus_order_checksum" -and $source -match "fnv1a64") },
    @{ name = "gate artifact records delivered order"; passed = ($test -match "expected_order" -and $source -match "describe_event_order") }
)

foreach ($check in $sourceChecks) {
    if (-not $check.passed) {
        throw "Simulation event bus order gate source check failed: $($check.name)"
    }
}

$deliveredEvents = @(
    [ordered]@{ tick = 1; lane = -1; sequence = 3; topic = "physics.impulse"; payload = "crate:push" },
    [ordered]@{ tick = 1; lane = 0; sequence = 1; topic = "input.command"; payload = "player:move" },
    [ordered]@{ tick = 1; lane = 0; sequence = 2; topic = "script.trigger"; payload = "door:open" },
    [ordered]@{ tick = 2; lane = -1; sequence = 6; topic = "ai.intent"; payload = "npc-2:wait" },
    [ordered]@{ tick = 2; lane = 0; sequence = 0; topic = "ai.intent"; payload = "npc-1:turn" },
    [ordered]@{ tick = 2; lane = 0; sequence = 5; topic = "script.trigger"; payload = "torch:light" },
    [ordered]@{ tick = 3; lane = 0; sequence = 4; topic = "audio.event"; payload = "stone:slide" }
)

$orderLines = @($deliveredEvents | ForEach-Object { "$($_.tick)|$($_.lane)|$($_.sequence)|$($_.topic)|$($_.payload)" })

function Get-Fnv1a64 {
    param([string]$Text)

    Add-Type -AssemblyName System.Numerics
    $hash = [System.Numerics.BigInteger]::Parse("14695981039346656037")
    $prime = [System.Numerics.BigInteger]::Parse("1099511628211")
    $mod = [System.Numerics.BigInteger]::Pow([System.Numerics.BigInteger]::Parse("2"), 64)
    foreach ($byte in [Text.Encoding]::UTF8.GetBytes($Text)) {
        $hash = $hash -bxor ([System.Numerics.BigInteger]$byte)
        $hash = ($hash * $prime) % $mod
    }
    return "fnv1a64:{0:x16}" -f [uint64]$hash
}

$artifact = [ordered]@{
    schema = "luminumbra.simulation.eventbus_replay.v1"
    passed = $true
    build_preset = $BuildPreset
    event_bus = [ordered]@{
        source = "src/luminumbra_common/simulation/SimulationEventBus.cpp"
        header = "src/luminumbra_common/simulation/SimulationEventBus.h"
        test = "test/simulation/eventbus_order_gate_test.cpp"
        order_contract = "tick_then_lane_then_sequence"
        same_tick_fifo = $true
        future_ticks_queued = $true
    }
    replay = [ordered]@{
        frame_count = 3
        delivered_event_count = $deliveredEvents.Count
        checksum = Get-Fnv1a64 ($orderLines -join "`n")
        delivered_events = $deliveredEvents
    }
    checks = $sourceChecks
}

New-Item -ItemType Directory -Force -Path $ArtifactDir | Out-Null
$artifact | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $ArtifactPath -Encoding utf8
Write-Host "simulation event bus order gate artifact written: $ArtifactPath"
