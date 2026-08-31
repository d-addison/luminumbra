# Enforced sky verification: build, run the rendering tests, then run the
# skybox and world visual sweep gates. Any failure returns a non-zero exit code.
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
