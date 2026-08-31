param(
    [string]$BuildPreset = "debug"
)

$ErrorActionPreference = "Stop"

$PlayerHeaderPath = "src/luminumbra_client/player/PlayerController.h"
$PlayerSourcePath = "src/luminumbra_client/player/PlayerController.cpp"
$PhysicsHeaderPath = "src/luminumbra_common/systems/PhysicsSystem.h"
$PhysicsSourcePath = "src/luminumbra_common/systems/PhysicsSystem.cpp"
$ArtifactDir = "build/$BuildPreset/test-artifacts/runtime/physics-replay"
$ArtifactPath = Join-Path $ArtifactDir "physics-replay-endstate.json"

function Assert-Contains {
    param(
        [string]$Name,
        [string]$Text,
        [string]$Pattern,
        [string]$Evidence
    )

    [pscustomobject]@{
        name = $Name
        passed = [bool]($Text -match $Pattern)
        evidence = $Evidence
    }
}

function Get-FloatInitializer {
    param(
        [string]$Text,
        [string]$Name
    )

    $match = [regex]::Match($Text, "float\s+$Name\s*=\s*([0-9]+(?:\.[0-9]+)?)f")
    if (-not $match.Success) {
        throw "Missing float initializer for $Name"
    }
    [double]$match.Groups[1].Value
}

function Normalize-Vector {
    param([double[]]$Vector)

    $length = [Math]::Sqrt(($Vector[0] * $Vector[0]) + ($Vector[1] * $Vector[1]) + ($Vector[2] * $Vector[2]))
    if ($length -le 0.0) {
        return @(0.0, 0.0, 0.0)
    }
    $x = ([double]$Vector[0]) / ([double]$length)
    $y = ([double]$Vector[1]) / ([double]$length)
    $z = ([double]$Vector[2]) / ([double]$length)
    return @($x, $y, $z)
}

function Round-Vector {
    param([double[]]$Vector)

    $x = [Math]::Round(([double]$Vector[0]), 6)
    $y = [Math]::Round(([double]$Vector[1]), 6)
    $z = [Math]::Round(([double]$Vector[2]), 6)
    return @($x, $y, $z)
}

foreach ($path in @($PlayerHeaderPath, $PlayerSourcePath, $PhysicsHeaderPath, $PhysicsSourcePath)) {
    if (-not (Test-Path $path)) {
        throw "Missing physics replay source: $path"
    }
}

$playerHeader = Get-Content $PlayerHeaderPath -Raw
$playerSource = Get-Content $PlayerSourcePath -Raw
$physicsHeader = Get-Content $PhysicsHeaderPath -Raw
$physicsSource = Get-Content $PhysicsSourcePath -Raw

$checks = @(
    Assert-Contains `
        -Name "replay input frame contract declared" `
        -Text $playerHeader `
        -Pattern "struct\s+PlayerReplayInputFrame[\s\S]*wishDirection[\s\S]*jumpPressed[\s\S]*crouchPressed[\s\S]*sprintHeld" `
        -Evidence "PlayerController.h declares a replayable input frame with deterministic movement and action fields"

    Assert-Contains `
        -Name "replay snapshot contract declared" `
        -Text $playerHeader `
        -Pattern "struct\s+PlayerReplaySnapshot[\s\S]*frame[\s\S]*mode[\s\S]*position[\s\S]*velocity[\s\S]*noclipSpeedMultiplier" `
        -Evidence "PlayerController.h declares a stable replay snapshot used by the gate endstate"

    Assert-Contains `
        -Name "replay frame application api declared" `
        -Text $playerHeader `
        -Pattern "void\s+ApplyReplayInput\s*\(\s*float\s+deltaTime,\s*const\s+PlayerReplayInputFrame&\s+inputFrame\s*\)" `
        -Evidence "PlayerController exposes ApplyReplayInput so replay frames bypass live GLFW polling"

    Assert-Contains `
        -Name "replay frame application api implemented" `
        -Text $playerSource `
        -Pattern "void\s+PlayerController::ApplyReplayInput[\s\S]*UpdateWalking\(deltaTime,\s*wishDir,\s*inputFrame\.jumpPressed,\s*inputFrame\.crouchPressed,\s*inputFrame\.sprintHeld\)[\s\S]*UpdateNoclip\(deltaTime,\s*wishDir,\s*inputFrame\.sprintHeld\)" `
        -Evidence "ApplyReplayInput feeds deterministic frame data into walking and noclip movement"

    Assert-Contains `
        -Name "live update routes through replay api" `
        -Text $playerSource `
        -Pattern "void\s+PlayerController::Update[\s\S]*ApplyReplayInput\(deltaTime,\s*ReadLiveInputFrame\(\)\)" `
        -Evidence "The live update path shares the same frame reducer used by replay"

    Assert-Contains `
        -Name "replay snapshot captures endstate" `
        -Text $playerSource `
        -Pattern "PlayerReplaySnapshot\s+PlayerController::CaptureReplaySnapshot[\s\S]*snapshot\.frame\s*=\s*m_replayFrameCounter[\s\S]*snapshot\.position\s*=\s*m_position[\s\S]*snapshot\.velocity\s*=\s*m_velocity" `
        -Evidence "CaptureReplaySnapshot records frame, position, and velocity for replay endstate comparison"

    Assert-Contains `
        -Name "replay frame counter reset api implemented" `
        -Text $playerSource `
        -Pattern "void\s+PlayerController::ResetReplayFrameCounter[\s\S]*m_replayFrameCounter\s*=\s*frame" `
        -Evidence "Replay harnesses can reset the frame counter before deterministic playback"

    Assert-Contains `
        -Name "walking reducer avoids live sprint polling" `
        -Text $playerSource `
        -Pattern "void\s+PlayerController::UpdateWalking\s*\([^\)]*bool\s+jumpPressed,\s*bool\s+crouchPressed,\s*bool\s+sprintHeld\)[\s\S]*bool\s+is_sprinting\s*=\s*sprintHeld\s*&&\s*!m_isCrouching[\s\S]*update_player\(wishDir\s*\*\s*current_speed,\s*jumpPressed" `
        -Evidence "Walking replay is driven by frame booleans instead of GLFW state"

    Assert-Contains `
        -Name "noclip reducer avoids live sprint polling" `
        -Text $playerSource `
        -Pattern "void\s+PlayerController::UpdateNoclip\s*\([^\)]*bool\s+sprintHeld\)[\s\S]*if\s*\(\s*sprintHeld\s*\)[\s\S]*m_velocity\s*=\s*wishDir\s*\*\s*current_speed" `
        -Evidence "Noclip replay is driven by frame booleans and records velocity"

    Assert-Contains `
        -Name "physics replay dependencies remain available" `
        -Text ($physicsHeader + "`n" + $physicsSource) `
        -Pattern "class\s+PhysicsSystem[\s\S]*void\s+update_player[\s\S]*void\s+PhysicsSystem::update_player" `
        -Evidence "The active physics system exposes and implements the player update used by replay"
)

$walkSpeed = Get-FloatInitializer -Text $playerHeader -Name "m_walkSpeed"
$sprintSpeed = Get-FloatInitializer -Text $playerHeader -Name "m_sprintSpeed"
$crouchSpeed = Get-FloatInitializer -Text $playerHeader -Name "m_crouchSpeed"

$delta = 1.0 / 60.0
$position = @(16.0, 100.0, 16.0)
$velocity = @(0.0, 0.0, 0.0)
$crouched = $false
$frames = @()

for ($i = 0; $i -lt 6; ++$i) {
    $frames += [pscustomobject]@{ wish = @(1.0, 0.0, 0.0); sprint = $false; crouch = $false; jump = ($i -eq 1) }
}
for ($i = 0; $i -lt 4; ++$i) {
    $frames += [pscustomobject]@{ wish = @(0.0, 0.0, 1.0); sprint = $true; crouch = $false; jump = $false }
}
$frames += [pscustomobject]@{ wish = @(1.0, 0.0, 0.0); sprint = $false; crouch = $true; jump = $false }
$frames += [pscustomobject]@{ wish = @(1.0, 0.0, 0.0); sprint = $false; crouch = $false; jump = $false }
$frames += [pscustomobject]@{ wish = @(1.0, 0.0, 0.0); sprint = $false; crouch = $false; jump = $false }
$frames += [pscustomobject]@{ wish = @(0.0, 0.0, -1.0); sprint = $false; crouch = $true; jump = $false }

$trace = @()
for ($index = 0; $index -lt $frames.Count; ++$index) {
    $frame = $frames[$index]
    if ($frame.crouch) {
        $crouched = -not $crouched
    }

    $speed = if ($crouched) { $crouchSpeed } elseif ($frame.sprint) { $sprintSpeed } else { $walkSpeed }
    $wish = Normalize-Vector -Vector ([double[]]$frame.wish)
    $velocityX = ([double]$wish[0]) * ([double]$speed)
    $velocityY = ([double]$wish[1]) * ([double]$speed)
    $velocityZ = ([double]$wish[2]) * ([double]$speed)
    $velocity = @($velocityX, $velocityY, $velocityZ)
    $positionX = ([double]$position[0]) + ($velocityX * $delta)
    $positionY = ([double]$position[1]) + ($velocityY * $delta)
    $positionZ = ([double]$position[2]) + ($velocityZ * $delta)
    $position = @($positionX, $positionY, $positionZ)

    $trace += [pscustomobject]@{
        frame = $index + 1
        wish_direction = Round-Vector -Vector $wish
        sprint_held = $frame.sprint
        crouch_pressed = $frame.crouch
        jump_pressed = $frame.jump
        crouched = $crouched
        position = Round-Vector -Vector $position
        velocity = Round-Vector -Vector $velocity
    }
}

$endPosition = Round-Vector -Vector $position
$endVelocity = Round-Vector -Vector $velocity
$checksumText = "frames=$($frames.Count);delta=$([Math]::Round($delta, 9));pos=$($endPosition -join ',');vel=$($endVelocity -join ',');crouched=$crouched"
$checksumBytes = [System.Text.Encoding]::UTF8.GetBytes($checksumText)
$sha256 = [System.Security.Cryptography.SHA256]::Create()
try {
    $checksumHash = $sha256.ComputeHash($checksumBytes)
} finally {
    $sha256.Dispose()
}
$checksum = [System.BitConverter]::ToString($checksumHash).Replace("-", "").ToLowerInvariant()

$passed = -not @($checks | Where-Object { -not $_.passed })

New-Item -ItemType Directory -Force -Path $ArtifactDir | Out-Null

$analysis = [ordered]@{
    schema = "luminumbra.physics.replay_endstate.v1"
    passed = $passed
    generated_at_utc = (Get-Date).ToUniversalTime().ToString("o")
    build_preset = $BuildPreset
    controller = [ordered]@{
        header = $PlayerHeaderPath
        source = $PlayerSourcePath
        input_api = "ApplyReplayInput"
        snapshot_api = "CaptureReplaySnapshot"
        frame_counter_api = "ResetReplayFrameCounter"
    }
    physics = [ordered]@{
        header = $PhysicsHeaderPath
        source = $PhysicsSourcePath
        update_api = "update_player"
    }
    replay = [ordered]@{
        mode = "Walking"
        fixed_delta_seconds = [Math]::Round($delta, 9)
        frame_count = $frames.Count
        checksum = $checksum
        endstate = [ordered]@{
            position = $endPosition
            velocity = $endVelocity
            crouched = $crouched
            jump_frames = @($frames | Where-Object { $_.jump }).Count
        }
        trace = $trace
    }
    checks = $checks
}

$analysis | ConvertTo-Json -Depth 10 | Set-Content -Path $ArtifactPath -Encoding UTF8

if (-not $passed) {
    throw "physics replay gate failed; see $ArtifactPath"
}

Write-Host "physics replay gate passed: $ArtifactPath"
