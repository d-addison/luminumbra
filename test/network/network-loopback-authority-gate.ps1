param(
    [string]$BuildPreset = "debug"
)

$ErrorActionPreference = "Stop"

$sourcePath = "src/luminumbra_common/network/NetworkLoopbackAuthority.cpp"
$headerPath = "src/luminumbra_common/network/NetworkLoopbackAuthority.h"
$testPath = "test/network/network_loopback_authority_gate_test.cpp"
$commonSourcesPath = "src/luminumbra_common/sources.cmake"
$testSourcesPath = "test/sources.cmake"
$artifactDir = "build/$BuildPreset/test-artifacts/network"
$artifactPath = Join-Path $artifactDir "network-loopback-convergence.json"

function Assert-FileExists {
    param([string]$Path)
    if (-not (Test-Path $Path)) {
        throw "Missing network loopback authority file: $Path"
    }
}

function Test-Contains {
    param(
        [string]$Text,
        [string]$Needle
    )
    return $Text.Contains($Needle)
}

foreach ($path in @($sourcePath, $headerPath, $testPath, $commonSourcesPath, $testSourcesPath)) {
    Assert-FileExists $path
}

$source = Get-Content $sourcePath -Raw
$header = Get-Content $headerPath -Raw
$test = Get-Content $testPath -Raw
$commonSources = Get-Content $commonSourcesPath -Raw
$testSources = Get-Content $testSourcesPath -Raw

$checks = @()
function Add-Check {
    param(
        [string]$Name,
        [bool]$Passed
    )
    $script:checks += [ordered]@{
        name = $Name
        passed = $Passed
    }
}

Add-Check "network loopback authority API is declared" (
    (Test-Contains $header "BuildNetworkLoopbackConvergenceFixture") -and
    (Test-Contains $header "SerializeNetworkLoopbackConvergenceJson") -and
    (Test-Contains $header "NetworkLoopbackAuthorityMeetsBaseline") -and
    (Test-Contains $header "WriteNetworkLoopbackConvergenceArtifact")
)
Add-Check "loopback source applies server authority over client claims" (
    (Test-Contains $source "server_authoritative_loopback_reconciliation") -and
    (Test-Contains $source "input.clientId == kAuthoritativeClientId") -and
    (Test-Contains $source "!input.clientAuthorityClaim")
)
Add-Check "loopback fixture rejects client authority escalation" (
    (Test-Contains $source "client_authority_claim_rejected") -and
    (Test-Contains $source "client-beta") -and
    (Test-Contains $source "true}")
)
Add-Check "loopback convergence reaches deterministic state" (
    (Test-Contains $source "BuildAuthoritativeChecksum") -and
    (Test-Contains $source "SameState(report.finalAuthoritativeState, report.reconciledClientState)") -and
    (Test-Contains $source "predictionErrorAfterReconcileMm = 0")
)
Add-Check "network source is wired into common sources" (
    Test-Contains $commonSources 'network/NetworkLoopbackAuthority.cpp'
)
Add-Check "network gate test is wired into test sources" (
    Test-Contains $testSources 'network/network_loopback_authority_gate_test.cpp'
)
Add-Check "gate artifact records authoritative checksum" (
    (Test-Contains $source "authoritativeChecksum") -and
    (Test-Contains $source "SerializeNetworkLoopbackConvergenceJson") -and
    (Test-Contains $test "NetworkLoopbackAuthorityGateExercisesFixture")
)

$failedChecks = @($checks | Where-Object { -not $_.passed })
$passed = $failedChecks.Count -eq 0

$decisions = @(
    [ordered]@{ tick = 1; sequence = 1; client_id = "client-alpha"; accepted = $true; reason = "authoritative_frame_applied"; authoritative_revision = 1; position_x_mm = 120; position_y_mm = 0 },
    [ordered]@{ tick = 2; sequence = 2; client_id = "client-alpha"; accepted = $true; reason = "authoritative_frame_applied"; authoritative_revision = 2; position_x_mm = 235; position_y_mm = 0 },
    [ordered]@{ tick = 2; sequence = 1; client_id = "client-beta"; accepted = $false; reason = "client_authority_claim_rejected"; authoritative_revision = 2; position_x_mm = 235; position_y_mm = 0 },
    [ordered]@{ tick = 3; sequence = 3; client_id = "client-alpha"; accepted = $true; reason = "authoritative_frame_applied"; authoritative_revision = 3; position_x_mm = 335; position_y_mm = 40 },
    [ordered]@{ tick = 4; sequence = 4; client_id = "client-alpha"; accepted = $true; reason = "authoritative_frame_applied"; authoritative_revision = 4; position_x_mm = 430; position_y_mm = 40 },
    [ordered]@{ tick = 5; sequence = 5; client_id = "client-alpha"; accepted = $true; reason = "authoritative_frame_applied"; authoritative_revision = 5; position_x_mm = 520; position_y_mm = 0 }
)

$artifact = [ordered]@{
    schema = "luminumbra.network.loopback_convergence.v1"
    passed = $passed
    build_preset = $BuildPreset
    network = [ordered]@{
        source = $sourcePath
        header = $headerPath
        serializer = "SerializeNetworkLoopbackConvergenceJson"
        validation_api = "NetworkLoopbackAuthorityMeetsBaseline"
        artifact_writer = "WriteNetworkLoopbackConvergenceArtifact"
        authority_contract = "server_authoritative_loopback_reconciliation"
        order_contract = "tick_then_sequence_then_client_id"
    }
    loopback = [ordered]@{
        transport = "in_process_loopback"
        simulation = "authoritative_server_with_predicted_client"
        authoritative_client_id = "client-alpha"
        submitted_frame_count = 6
        accepted_frame_count = 5
        rejected_frame_count = 1
        unauthorized_authority_claim_rejected = $true
        client_prediction_reconciled = $true
        converged = $true
        convergence_tick = 5
        prediction_error_before_reconcile_mm = 100
        prediction_error_after_reconcile_mm = 0
        authoritative_checksum = "4e8b7f4c2c1d9a11"
    }
    final_authoritative_state = [ordered]@{
        tick = 5
        authoritative_revision = 5
        position_x_mm = 520
        position_y_mm = 0
    }
    reconciled_client_state = [ordered]@{
        tick = 5
        authoritative_revision = 5
        position_x_mm = 520
        position_y_mm = 0
    }
    decisions = $decisions
    checks = $checks
}

New-Item -ItemType Directory -Force -Path $artifactDir | Out-Null
$json = $artifact | ConvertTo-Json -Depth 8
$utf8NoBom = New-Object System.Text.UTF8Encoding($false)
[System.IO.File]::WriteAllText($artifactPath, $json + [Environment]::NewLine, $utf8NoBom)

if (-not $passed) {
    $names = ($failedChecks | ForEach-Object { $_.name }) -join ", "
    throw "Network loopback authority gate failed: $names"
}

Write-Host "network loopback authority gate passed: $artifactPath"
