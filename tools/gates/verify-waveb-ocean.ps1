# Enforced ocean verification: build, run the render and water tests, then run
# the world visual sweep gate. Any failure returns a non-zero exit code.
$ErrorActionPreference = 'Stop'
$env:PATH = "C:\msys64\ucrt64\bin;$env:PATH"
$root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
Set-Location $root

Write-Host "== [ocean] cmake --build build -j 3 =="
cmake --build build -j 3
if ($LASTEXITCODE -ne 0) { Write-Error "build failed"; exit 1 }

Write-Host "== [ocean] ctest RenderCaptureTest|WaterfallVisualTest|RuntimeWorldVisualValidationTest =="
ctest --test-dir build -R "RenderCaptureTest|WaterfallVisualTest|RuntimeWorldVisualValidationTest" --output-on-failure
if ($LASTEXITCODE -ne 0) { Write-Error "ctest failed"; exit 1 }

Write-Host "== [ocean] validate-engine-frontier -Mode WorldVisualSweep =="
& "$root\tools\gates\validate-engine-frontier.ps1" -Mode WorldVisualSweep
if ($LASTEXITCODE -ne 0) { Write-Error "WorldVisualSweep gate failed"; exit 1 }

Write-Host "== [ocean] ALL GATES PASSED =="
exit 0
