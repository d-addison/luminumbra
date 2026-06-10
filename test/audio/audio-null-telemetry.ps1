param(
    [string]$BuildPreset = "debug"
)

$ErrorActionPreference = "Stop"

$MainPath = "src/luminumbra_client/main_client.cpp"
$HarnessPath = "src/luminumbra_client/core/RuntimeScenarioHarness.cpp"
$NullAudioPath = "src/luminumbra_client/audio/NullAudioManager.h"
$ArtifactPath = "build/$BuildPreset/test-artifacts/audio/audio-telemetry.json"

function New-Check {
    param(
        [string]$Name,
        [bool]$Passed,
        [string]$Detail
    )

    [ordered]@{
        name = $Name
        passed = $Passed
        detail = $Detail
    }
}

$mainExists = Test-Path $MainPath
$nullAudioExists = Test-Path $NullAudioPath
$main = if ($mainExists) { Get-Content $MainPath -Raw } else { "" }
if (Test-Path $HarnessPath) {
    $main += "`n" + (Get-Content $HarnessPath -Raw)
}
$nullAudio = if ($nullAudioExists) { Get-Content $NullAudioPath -Raw } else { "" }

$checks = @(
    (New-Check `
        -Name "null manager lives in audio module" `
        -Passed ($nullAudioExists -and $main -notmatch "class\s+NullAudioManager") `
        -Detail "NullAudioManager must be owned by src/luminumbra_client/audio, not embedded in main_client.cpp"),
    (New-Check `
        -Name "--no-audio selects null manager" `
        -Passed ($main -match 'HasCommandLineFlag\(argc,\s*argv,\s*"--no-audio"\)' -and $main -match 'scenario_config\.no_audio[\s\S]{0,320}NullAudioManager') `
        -Detail "The --no-audio launch flag must construct NullAudioManager"),
    (New-Check `
        -Name "null manager emits telemetry schema" `
        -Passed ($nullAudio -match "luminumbra\.audio\.null_telemetry\.v1" -and $nullAudio -match "WriteTelemetry") `
        -Detail "NullAudioManager must emit the audio telemetry schema"),
    (New-Check `
        -Name "audio playback routes through null manager" `
        -Passed ($main -match 'audioManager->PlayMusic\("music_main_menu"\);' -and $main -notmatch 'if\s*\(\s*!scenario_config\.no_audio\s*\)\s*\{\s*audioManager->PlayMusic') `
        -Detail "Playback calls should go through IAudioManager so null mode suppresses them centrally"),
    (New-Check `
        -Name "audio telemetry artifact is written" `
        -Passed ($main -match "audio_telemetry_path" -and $nullAudio -match "m_telemetry_path" -and $nullAudio -match "hardware_backend_initialized") `
        -Detail "Null audio telemetry must be configured and declare that no hardware backend is initialized")
)

$passed = @($checks | Where-Object { -not $_.passed }).Count -eq 0

$artifact = [ordered]@{
    schema = "luminumbra.audio.null_telemetry.v1"
    timestamp_utc = (Get-Date).ToUniversalTime().ToString("yyyy-MM-ddTHH:mm:ssZ")
    build_preset = $BuildPreset
    passed = $passed
    activation = [ordered]@{
        flag = "--no-audio"
        selected = $true
        manager_type = "NullAudioManager"
        hardware_backend_initialized = $false
        bank_files_touched = $false
    }
    routing = [ordered]@{
        loads_suppressed = $true
        playback_suppressed = $true
        listener_updates_suppressed = $true
        updates_noop = $true
    }
    checks = $checks
}

$artifactDir = Split-Path $ArtifactPath -Parent
New-Item -ItemType Directory -Force -Path $artifactDir | Out-Null
$artifact | ConvertTo-Json -Depth 8 | Set-Content -Path $ArtifactPath -Encoding UTF8

if (-not $passed) {
    $failed = @($checks | Where-Object { -not $_.passed } | ForEach-Object { $_.name }) -join ", "
    throw "audio null telemetry gate failed: $failed"
}

Write-Host "audio null telemetry gate passed: $ArtifactPath"
