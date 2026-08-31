# Generate the game's core SFX from tools/audio/sfx_manifest.json via the ElevenLabs
# sound-generation API. The API key is read ONLY from the environment ($env:ELEVENLABS_API_KEY)
# and is never stored in this repo. mp3 from the API is transcoded to each entry's target
# extension (.ogg/.wav) with ffmpeg so files land at the exact paths the .bank.json files expect.
#
#   $env:ELEVENLABS_API_KEY = "<key>"; pwsh tools/audio/generate_sfx.ps1 [-Force]
#
# -Force regenerates files that already exist (default: skip existing).
param([switch]$Force)
$ErrorActionPreference = "Stop"

$key = $env:ELEVENLABS_API_KEY
if (-not $key) { Write-Error "ELEVENLABS_API_KEY is not set; export it before running."; exit 1 }
if (-not (Get-Command ffmpeg -ErrorAction SilentlyContinue)) { Write-Error "ffmpeg not found on PATH."; exit 1 }

$manifest  = Get-Content (Join-Path $PSScriptRoot "sfx_manifest.json") -Raw | ConvertFrom-Json
$model     = $manifest.model_id
$pinfl     = [double]$manifest.default_prompt_influence
$audioRoot = Join-Path (Resolve-Path (Join-Path $PSScriptRoot "../..")) "assets/audio"
$url       = "https://api.elevenlabs.io/v1/sound-generation"

$ok = 0; $fail = 0; $skip = 0
foreach ($s in $manifest.sounds) {
    $outPath = Join-Path $audioRoot $s.out
    if ((Test-Path $outPath) -and -not $Force) { $skip++; continue }
    New-Item -ItemType Directory -Force -Path (Split-Path $outPath) | Out-Null

    $body = @{ text = $s.prompt; model_id = $model; prompt_influence = $pinfl }
    if ($s.PSObject.Properties.Name -contains "duration" -and $s.duration) { $body.duration_seconds = [double]$s.duration }
    if ($s.PSObject.Properties.Name -contains "loop" -and $s.loop) { $body.loop = $true }

    $bodyFile = [System.IO.Path]::GetTempFileName()
    ($body | ConvertTo-Json -Compress) | Out-File -FilePath $bodyFile -Encoding utf8 -NoNewline
    $tmpMp3 = [System.IO.Path]::GetTempFileName() + ".mp3"

    curl.exe -s -X POST $url -H "xi-api-key: $key" -H "Content-Type: application/json" --data "@$bodyFile" -o $tmpMp3 2>$null
    Remove-Item $bodyFile -Force -ErrorAction SilentlyContinue

    $len = (Get-Item $tmpMp3 -ErrorAction SilentlyContinue).Length
    if (-not $len -or $len -lt 1024) {
        $err = (Get-Content $tmpMp3 -Raw -ErrorAction SilentlyContinue)
        $msg = if ($err) { ($err -replace '\s+', ' ') } else { '(empty body)' }
        if ($msg.Length -gt 160) { $msg = $msg.Substring(0, 160) }
        Write-Warning ("FAIL {0}: {1}" -f $s.out, $msg)
        $fail++; Remove-Item $tmpMp3 -Force -ErrorAction SilentlyContinue; continue
    }

    $ext = [System.IO.Path]::GetExtension($outPath).TrimStart('.').ToLower()
    if ($ext -eq "mp3") {
        Move-Item -Force $tmpMp3 $outPath
    } else {
        & ffmpeg -y -loglevel error -i $tmpMp3 $outPath 2>$null
        Remove-Item $tmpMp3 -Force -ErrorAction SilentlyContinue
    }
    if (Test-Path $outPath) { Write-Output ("OK  {0} ({1:N0} KB)" -f $s.out, ((Get-Item $outPath).Length / 1KB)); $ok++ }
    else { Write-Warning ("FAIL convert {0}" -f $s.out); $fail++ }
}
Write-Output "=== sfx generated=$ok skipped=$skip failed=$fail ==="
