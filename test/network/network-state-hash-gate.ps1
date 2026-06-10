param(
    [string]$BuildPreset = "debug"
)

$ErrorActionPreference = "Stop"

$sourcePath = "src/luminumbra_common/network/NetworkStateHash.cpp"
$headerPath = "src/luminumbra_common/network/NetworkStateHash.h"
$testPath = "test/network/network_state_hash_gate_test.cpp"
$commonSourcesPath = "src/luminumbra_common/sources.cmake"
$testSourcesPath = "test/sources.cmake"
$gateExePath = "build/$BuildPreset/bin/frontier_gates_test.exe"
$artifactDir = "build/$BuildPreset/test-artifacts/network"
$artifactPath = Join-Path $artifactDir "network-state-hash.json"

foreach ($path in @($sourcePath, $headerPath, $testPath, $commonSourcesPath, $testSourcesPath)) {
    if (-not (Test-Path $path)) {
        throw "Missing network state hash file: $path"
    }
}

if (-not (Test-Path $gateExePath)) {
    throw "Missing frontier gate executable. Build frontier_gates_test first: $gateExePath"
}

# The artifact is produced by the compiled fixture, not assembled here: the
# gate executable replays the loopback fixture twice and asserts identical
# per-tick hashes before writing network-state-hash.json.
& $gateExePath --gtest_filter="NetworkStateHash.*"
if ($LASTEXITCODE -ne 0) {
    throw "frontier_gates_test NetworkStateHash cases failed with exit code $LASTEXITCODE"
}

if (-not (Test-Path $artifactPath)) {
    throw "network state hash gate did not produce $artifactPath"
}

$source = Get-Content $sourcePath -Raw
$testSources = Get-Content $testSourcesPath -Raw
foreach ($needle in @(
    "authoritative_sorted_state_per_tick",
    "tick_ascending_sorted_state_fields",
    "fnv1a_64_canonical_state_string",
    "BuildWorldHashAnalysis",
    "SortEntityRegistrySnapshot"
)) {
    if (-not $source.Contains($needle)) {
        throw "NetworkStateHash.cpp is missing required contract token '$needle'"
    }
}
if (-not $testSources.Contains("network/network_state_hash_gate_test.cpp")) {
    throw "network state hash gate test is not wired into test sources"
}

Write-Host "network state hash gate passed: $artifactPath"
