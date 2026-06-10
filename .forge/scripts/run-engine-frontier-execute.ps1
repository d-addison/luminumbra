$ErrorActionPreference = "Stop"

# Luminumbra is a C++ project: the Rust-oriented post-cherry-pick integration
# steps must stay disabled or every successful task gets reverted.
# See .forge/env.local.example.
$envLocal = ".forge/env.local"
if (Test-Path $envLocal) {
    foreach ($line in Get-Content $envLocal) {
        if ($line -match '^\s*(?:export\s+)?([A-Za-z_][A-Za-z0-9_]*)=(.*)$') {
            $name = $Matches[1]
            $value = $Matches[2].Trim().Trim('"').Trim("'")
            [System.Environment]::SetEnvironmentVariable($name, $value, "Process")
        }
    }
}
$env:FORGE_INTEGRATION_CARGO_CHECK = '0'
$env:FORGE_INTEGRATION_HYGIENE = '0'

# The dispatch spawner hardcodes a retired model id; route codex invocations
# through the shim that rewrites it to a supported model.
$shimDir = Join-Path $PSScriptRoot "codex-shim"
$env:Path = "$shimDir;" + $env:Path

$dispatchPath = ".forge/tasks/engine-frontier/dispatch.json"
if (-not (Test-Path $dispatchPath)) {
    throw "Missing engine-frontier dispatch graph: $dispatchPath"
}

# Gate analysis artifacts intentionally live under build/<preset>/test-artifacts
# (mirroring the runtime-stability gates), so gitignored creates are expected.
& forge contracts execute --tasks $dispatchPath --spawner codex --sandbox off --allow-gitignored-files

exit $LASTEXITCODE
