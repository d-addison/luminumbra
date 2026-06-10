param(
    [string]$BuildPreset = "debug"
)

$ErrorActionPreference = "Stop"

$ManagerSourcePath = "src/luminumbra_client/audio/MiniaudioManager.cpp"
$ManagerHeaderPath = "src/luminumbra_client/audio/MiniaudioManager.h"
$InterfacePath = "src/luminumbra_client/audio/IAudioManager.h"
$ArtifactPath = "build/$BuildPreset/test-artifacts/audio/audio-handle-application.json"

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

function Get-FunctionText {
    param(
        [string]$Text,
        [string]$StartPattern,
        [string]$EndPattern
    )

    $startMatch = [regex]::Match($Text, $StartPattern)
    if (-not $startMatch.Success) {
        return ""
    }

    $start = $startMatch.Index
    $afterStart = $start + $startMatch.Length
    $tail = $Text.Substring($afterStart)
    $endMatch = [regex]::Match($tail, $EndPattern)
    if (-not $endMatch.Success) {
        return $Text.Substring($start)
    }

    return $Text.Substring($start, $startMatch.Length + $endMatch.Index)
}

$managerSourceExists = Test-Path $ManagerSourcePath
$managerHeaderExists = Test-Path $ManagerHeaderPath
$interfaceExists = Test-Path $InterfacePath

$managerSource = if ($managerSourceExists) { Get-Content $ManagerSourcePath -Raw } else { "" }
$managerHeader = if ($managerHeaderExists) { Get-Content $ManagerHeaderPath -Raw } else { "" }
$interface = if ($interfaceExists) { Get-Content $InterfacePath -Raw } else { "" }

$playEventFn = Get-FunctionText `
    -Text $managerSource `
    -StartPattern 'bool\s+MiniaudioManager::PlayEvent\s*\(' `
    -EndPattern 'bool\s+MiniaudioManager::PlayOneShot2D\s*\('
$stopEventFn = Get-FunctionText `
    -Text $managerSource `
    -StartPattern 'bool\s+MiniaudioManager::StopEvent\s*\(' `
    -EndPattern 'bool\s+MiniaudioManager::SetEventPosition\s*\('
$setPositionFn = Get-FunctionText `
    -Text $managerSource `
    -StartPattern 'bool\s+MiniaudioManager::SetEventPosition\s*\(' `
    -EndPattern 'bool\s+MiniaudioManager::SetEventVolume\s*\('
$setVolumeFn = Get-FunctionText `
    -Text $managerSource `
    -StartPattern 'bool\s+MiniaudioManager::SetEventVolume\s*\(' `
    -EndPattern 'bool\s+MiniaudioManager::SetEventParameter\s*\('
$setParameterFn = Get-FunctionText `
    -Text $managerSource `
    -StartPattern 'bool\s+MiniaudioManager::SetEventParameter\s*\(' `
    -EndPattern 'const\s+AudioEventDefinition\*\s+MiniaudioManager::GetEventDefinition\s*\('

$checks = @(
    (New-Check `
        -Name "interface declares handle mutators" `
        -Passed ($interfaceExists `
            -and $interface -match 'virtual\s+bool\s+StopEvent\s*\(\s*AudioEventHandle\s+handle' `
            -and $interface -match 'virtual\s+bool\s+SetEventPosition\s*\(\s*AudioEventHandle\s+handle' `
            -and $interface -match 'virtual\s+bool\s+SetEventVolume\s*\(\s*AudioEventHandle\s+handle' `
            -and $interface -match 'virtual\s+bool\s+SetEventParameter\s*\(\s*AudioEventHandle\s+handle') `
        -Detail "IAudioManager must expose handle-based stop, position, volume, and parameter application"),
    (New-Check `
        -Name "miniaudio manager stores playable handles" `
        -Passed ($managerSourceExists -and $managerHeaderExists `
            -and $managerHeader -match 'std::unordered_map<AudioEventHandle,\s*std::unique_ptr<ma_sound>>\s+m_activeSounds' `
            -and $playEventFn -match 'outHandle\s*=\s*m_nextHandle\+\+' `
            -and $playEventFn -match 'm_activeSounds\s*\[\s*outHandle\s*\]\s*=\s*std::move\s*\(\s*sound\s*\)') `
        -Detail "PlayEvent must assign caller-visible handles and retain the active ma_sound by handle"),
    (New-Check `
        -Name "stop applies handle to active sound" `
        -Passed ($stopEventFn -match 'm_activeSounds\.find\s*\(\s*handle\s*\)' `
            -and $stopEventFn -match 'ma_sound_stop\s*\(\s*it->second\.get\s*\(\s*\)\s*\)') `
        -Detail "StopEvent must look up the requested handle and stop that ma_sound"),
    (New-Check `
        -Name "immediate stop releases active handle" `
        -Passed ($stopEventFn -match 'if\s*\(\s*immediate\s*\)' `
            -and $stopEventFn -match 'ma_sound_uninit\s*\(\s*it->second\.get\s*\(\s*\)\s*\)' `
            -and $stopEventFn -match 'm_activeSounds\.erase\s*\(\s*it\s*\)') `
        -Detail "Immediate StopEvent must uninitialize the ma_sound and erase the active handle"),
    (New-Check `
        -Name "stop removes spatial cluster source" `
        -Passed ($stopEventFn -match 'm_spatial_cluster->RemoveAudioSource\s*\(\s*handle\s*\)') `
        -Detail "Stopping a 3D handle must remove the matching spatial cluster source"),
    (New-Check `
        -Name "position applies handle to ma_sound" `
        -Passed ($setPositionFn -match 'm_activeSounds\.find\s*\(\s*handle\s*\)' `
            -and $setPositionFn -match 'ma_sound_set_position\s*\(\s*it->second\.get\s*\(\s*\),\s*position\.x,\s*position\.y,\s*position\.z\s*\)' `
            -and $setPositionFn -match 'm_spatial_cluster->UpdateSourcePosition\s*\(\s*handle,\s*position\s*\)') `
        -Detail "SetEventPosition must apply the handle position to miniaudio and spatial clustering"),
    (New-Check `
        -Name "volume applies handle to ma_sound" `
        -Passed ($setVolumeFn -match 'm_activeSounds\.find\s*\(\s*handle\s*\)' `
            -and $setVolumeFn -match 'ma_sound_set_volume\s*\(\s*it->second\.get\s*\(\s*\),\s*volume\s*\)' `
            -and $setVolumeFn -match 'm_spatial_cluster->UpdateSourceVolume\s*\(\s*handle,\s*volume\s*\)') `
        -Detail "SetEventVolume must apply caller volume to the handle and keep the spatial source in sync"),
    (New-Check `
        -Name "parameter applies supported miniaudio controls" `
        -Passed ($setParameterFn -match 'paramID\s*==\s*"volume"' `
            -and $setParameterFn -match 'ma_sound_set_volume\s*\(\s*it->second\.get\s*\(\s*\),\s*value\s*\)' `
            -and $setParameterFn -match 'paramID\s*==\s*"pitch"' `
            -and $setParameterFn -match 'ma_sound_set_pitch\s*\(\s*it->second\.get\s*\(\s*\),\s*value\s*\)') `
        -Detail "SetEventParameter must apply supported per-handle controls instead of always failing"),
    (New-Check `
        -Name "unknown handles are rejected" `
        -Passed ($stopEventFn -match 'return\s+false\s*;' `
            -and $setPositionFn -match 'return\s+false\s*;' `
            -and $setVolumeFn -match 'return\s+false\s*;' `
            -and $setParameterFn -match 'm_activeSounds\.end\s*\(\s*\)[\s\S]{0,80}return\s+false\s*;') `
        -Detail "Handle mutators must return false when the handle is not active")
)

$passed = @($checks | Where-Object { -not $_.passed }).Count -eq 0

$artifact = [ordered]@{
    schema = "luminumbra.audio.handle_application.v1"
    timestamp_utc = (Get-Date).ToUniversalTime().ToString("yyyy-MM-ddTHH:mm:ssZ")
    build_preset = $BuildPreset
    passed = $passed
    manager = [ordered]@{
        source = $ManagerSourcePath
        header = $ManagerHeaderPath
        interface = $InterfacePath
        implementation = "MiniaudioManager"
        active_handle_map = "m_activeSounds"
    }
    handle_application = [ordered]@{
        playback_handle_api = "PlayEvent"
        stop_api = "StopEvent"
        position_api = "SetEventPosition"
        volume_api = "SetEventVolume"
        parameter_api = "SetEventParameter"
        stopped_handles_removed = ($stopEventFn -match 'm_activeSounds\.erase\s*\(\s*it\s*\)')
        spatial_cluster_updated = ($setPositionFn -match 'UpdateSourcePosition' -and $setVolumeFn -match 'UpdateSourceVolume' -and $stopEventFn -match 'RemoveAudioSource')
        supported_parameters = @("volume", "pitch")
        invalid_handles_rejected = ($setParameterFn -match 'm_activeSounds\.end\s*\(\s*\)[\s\S]{0,80}return\s+false\s*;')
    }
    checks = $checks
}

$artifactDir = Split-Path $ArtifactPath -Parent
New-Item -ItemType Directory -Force -Path $artifactDir | Out-Null
$artifact | ConvertTo-Json -Depth 8 | Set-Content -Path $ArtifactPath -Encoding UTF8

if (-not $passed) {
    $failed = @($checks | Where-Object { -not $_.passed } | ForEach-Object { $_.name }) -join ", "
    throw "audio handle application gate failed: $failed"
}

Write-Host "audio handle application gate passed: $ArtifactPath"
