# Enforced verify gate for T-I7-WAVEB-SKY (forge runs this via
# agent_contract.verify_command). Build clean + render/visual tests + the
# skybox + world visual sweep gates. Any failure -> non-zero exit so forge's
# post-merge verification marks the task Failed (PR #1728 enforcement).
$ErrorActionPreference = 'Stop'
$env:PATH = "C:\msys64\ucrt64\bin;$env:PATH"
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
Set-Location $root

Write-Host "== [sky] cmake --build build -j 3 =="
cmake --build build -j 3
if ($LASTEXITCODE -ne 0) { Write-Error "build failed"; exit 1 }

Write-Host "== [sky] ctest RenderCaptureTest|RuntimeWorldVisualValidationTest =="
ctest --test-dir build -R "RenderCaptureTest|RuntimeWorldVisualValidationTest" --output-on-failure
if ($LASTEXITCODE -ne 0) { Write-Error "ctest failed"; exit 1 }

Write-Host "== [sky] validate-engine-frontier -Mode SkyboxVisual =="
& "$root\tools\gates\validate-engine-frontier.ps1" -Mode SkyboxVisual
if ($LASTEXITCODE -ne 0) { Write-Error "SkyboxVisual gate failed"; exit 1 }

Write-Host "== [sky] validate-engine-frontier -Mode WorldVisualSweep =="
& "$root\tools\gates\validate-engine-frontier.ps1" -Mode WorldVisualSweep
if ($LASTEXITCODE -ne 0) { Write-Error "WorldVisualSweep gate failed"; exit 1 }

Write-Host "== [sky] ALL GATES PASSED =="
exit 0
