# File-only capture checks. Dot-sourcing this file defines functions without
# running a gate, creating artifacts, or changing the caller's preferences.

function Get-CapturePinProfile {
    param([string]$Profile = "runtime_scenario_v1")

    # Trusted profiles live in source, never in a producer's self-reported JSON.
    # The file-only test checks these dimensions against RuntimeScenarioConfig.h.
    switch -CaseSensitive ($Profile) {
        "runtime_scenario_v1" {
            return [pscustomobject]@{ Width = 3840; Height = 1600 }
        }
        default { throw "Unknown capture pin profile '$Profile'" }
    }
}

function Resolve-CaptureArtifactPath {
    param([object]$Path, [object]$ArtifactDir, [string]$Name)

    if ($Path -isnot [string] -or [string]::IsNullOrWhiteSpace($Path)) {
        throw "$Name artifact '$Path': expected a nonempty screenshot path"
    }
    if ($null -eq $ArtifactDir) {
        return $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($Path)
    }
    if ($ArtifactDir -isnot [string] -or [string]::IsNullOrWhiteSpace($ArtifactDir)) {
        throw "$Name artifact '$Path': expected a nonempty artifact directory"
    }
    $root = $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($ArtifactDir)
    $root = [System.IO.Path]::GetFullPath($root).TrimEnd([char[]]@('/', '\'))
    $separator = [System.IO.Path]::DirectorySeparatorChar
    $comparison = [System.StringComparison]::Ordinal
    if ($separator -eq '\') { $comparison = [System.StringComparison]::OrdinalIgnoreCase }
    $candidate = [System.IO.Path]::GetFullPath([System.IO.Path]::Combine($root, $Path))
    if (-not $candidate.StartsWith($root + $separator, $comparison)) {
        throw "$Name artifact '$Path': screenshot escapes artifact directory '$ArtifactDir'"
    }
    # A lexical child reached through a symlink/junction can still refer to a
    # different run. Refuse links within the supplied root, without scanning it.
    $current = $candidate
    while ($current.Length -ge $root.Length) {
        if (Test-Path -LiteralPath $current -ErrorAction Stop) {
            $item = Get-Item -LiteralPath $current -Force -ErrorAction Stop
            if (($item.Attributes -band [System.IO.FileAttributes]::ReparsePoint) -ne 0) {
                throw "$Name artifact '$Path': linked artifact path '$current' is not a run-local file"
            }
        }
        if ($current.Equals($root, $comparison)) { break }
        $current = [System.IO.Path]::GetDirectoryName($current)
    }
    return $candidate
}

function Read-CapturePpmToken {
    param([byte[]]$Header, [int]$Count, [ref]$Offset)

    # Comments and ASCII whitespace are header syntax only. In particular, do
    # not use this tokenizer after the maxval separator: raster bytes can be
    # whitespace or '#'. Limit all header work to the caller's 4096-byte buffer.
    while ($Offset.Value -lt $Count) {
        $b = $Header[$Offset.Value]
        if ($b -eq 35) {
            while ($Offset.Value -lt $Count -and $Header[$Offset.Value] -notin @(10, 13)) {
                $Offset.Value++
            }
        } elseif ($b -in @(9, 10, 11, 12, 13, 32)) {
            $Offset.Value++
        } else { break }
    }
    $start = $Offset.Value
    while ($Offset.Value -lt $Count -and $Header[$Offset.Value] -notin @(9, 10, 11, 12, 13, 32, 35)) {
        $Offset.Value++
    }
    if ($Offset.Value -eq $start -or $Offset.Value -ge $Count) {
        throw "missing token, separator, or header exceeds 4096 bytes"
    }
    return [System.Text.Encoding]::ASCII.GetString($Header, $start, $Offset.Value - $start)
}

function Assert-PpmArtifact {
    [CmdletBinding()]
    param(
        [Parameter(Position = 0)][object]$Path,
        [object]$ArtifactDir = $null,
        [string]$Name = "PPM capture",
        [switch]$PassThru
    )

    $stream = $null
    try {
        $resolved = Resolve-CaptureArtifactPath -Path $Path -ArtifactDir $ArtifactDir -Name $Name
        $item = Get-Item -LiteralPath $resolved -Force -ErrorAction Stop
        if ($item -isnot [System.IO.FileInfo] -or
            ($item.Attributes -band [System.IO.FileAttributes]::ReparsePoint) -ne 0) {
            throw "expected a regular file"
        }
        $stream = [System.IO.File]::Open($resolved, [System.IO.FileMode]::Open,
            [System.IO.FileAccess]::Read, [System.IO.FileShare]::Read)
        $header = New-Object byte[] 4096
        $count = 0
        while ($count -lt $header.Length) {
            $read = $stream.Read($header, $count, $header.Length - $count)
            if ($read -eq 0) { break }
            $count += $read
        }
        if ($count -lt 3 -or $header[0] -ne 80 -or $header[1] -ne 54 -or
            $header[2] -notin @(9, 10, 11, 12, 13, 32)) {
            throw "expected binary P6 magic and separator"
        }
        $offset = 0
        $magic = Read-CapturePpmToken $header $count ([ref]$offset)
        $widthToken = Read-CapturePpmToken $header $count ([ref]$offset)
        $heightToken = Read-CapturePpmToken $header $count ([ref]$offset)
        $maxval = Read-CapturePpmToken $header $count ([ref]$offset)
        if ($magic -cne "P6" -or $maxval -cne "255") { throw "only P6 RGB with maxval 255 is supported" }
        [long]$width = 0
        [long]$height = 0
        if ($widthToken -cnotmatch '^[0-9]+$' -or $heightToken -cnotmatch '^[0-9]+$' -or
            -not [long]::TryParse($widthToken, [ref]$width) -or
            -not [long]::TryParse($heightToken, [ref]$height) -or
            $width -lt 1 -or $height -lt 1 -or $width -gt 16384 -or $height -gt 16384) {
            throw "dimensions must be integers in [1, 16384]"
        }
        # Bounds precede multiplication and allocation. The raster is checked
        # by file length; a large producer dimension never allocates a raster.
        [long]$pixels = $width * $height
        if ($pixels -gt 134217728) { throw "image exceeds the 128-megapixel structural limit" }
        [long]$payloadBytes = $pixels * 3
        if ($header[$offset] -notin @(9, 10, 11, 12, 13, 32)) { throw "missing maxval whitespace separator" }
        $rasterOffset = $offset + 1
        # CR at the raster boundary must form CRLF. Never infer the separator
        # from payload length: doing so could reinterpret a truncated CRLF image
        # as a valid lone-CR image with LF as its first pixel.
        if ($header[$offset] -eq 13) {
            if ($rasterOffset -ge $count -or $header[$rasterOffset] -ne 10) {
                throw "CR maxval separator must be followed by LF"
            }
            $rasterOffset++
        }
        if ($rasterOffset -gt 4096 -or $stream.Length - $rasterOffset -ne $payloadBytes) {
            throw "expected exactly $payloadBytes RGB payload bytes after the header; found $($stream.Length - $rasterOffset)"
        }
        if ($PassThru) {
            return [pscustomobject]@{
                Path = $resolved; Width = $width; Height = $height
                PayloadBytes = $payloadBytes; RasterOffset = $rasterOffset
            }
        }
    } catch {
        throw "$Name artifact '$Path': $($_.Exception.Message)"
    } finally {
        if ($null -ne $stream) { $stream.Dispose() }
    }
}

function Assert-CapturePinned {
    [CmdletBinding()]
    param(
        [object]$ArtifactDir,
        [string]$Name = "Capture",
        [object[]]$ScreenshotPaths,
        [string]$Profile = "runtime_scenario_v1"
    )

    $metadataPath = "last-known-runtime.json"
    try {
        if ($ArtifactDir -isnot [string] -or [string]::IsNullOrWhiteSpace($ArtifactDir)) {
            throw "expected a nonempty artifact directory"
        }
        $expected = Get-CapturePinProfile -Profile $Profile
        $metadataPath = Resolve-CaptureArtifactPath -Path $metadataPath -ArtifactDir $ArtifactDir -Name $Name
        $item = Get-Item -LiteralPath $metadataPath -Force -ErrorAction Stop
        if ($item -isnot [System.IO.FileInfo] -or $item.Length -gt 4194304) {
            throw "expected a runtime-state file no larger than 4 MiB"
        }
        $json = Get-Content -LiteralPath $metadataPath -Raw -ErrorAction Stop
        # Windows PowerShell can unwrap a one-element JSON array. Require the
        # root object in the source text before converting it into PS objects.
        if ($json -notmatch '^\s*\{') { throw "runtime-state root must be a JSON object" }
        $state = $json | ConvertFrom-Json -ErrorAction Stop
        if ($state -isnot [pscustomobject] -or $state.PSObject.Properties.Name -cnotcontains "schema" -or
            $state.schema -isnot [string] -or
            $state.schema -cne "luminumbra.runtime_state.v1") { throw "unknown or missing runtime-state schema" }
        if ($state.PSObject.Properties.Name -cnotcontains "capture_pin") { throw "missing capture_pin object" }
        $pin = $state.capture_pin
        if ($pin -isnot [pscustomobject]) { throw "missing or mistyped capture_pin object" }
        foreach ($field in @("capture_width", "capture_height", "pinned_width", "pinned_height")) {
            if ($pin.PSObject.Properties.Name -cnotcontains $field) { throw "missing capture_pin.$field" }
            $value = $pin.$field
            if (($value -isnot [int] -and $value -isnot [long]) -or $value -le 0) {
                throw "capture_pin.$field must be a positive JSON integer"
            }
        }
        if ($pin.PSObject.Properties.Name -cnotcontains "pinned" -or $pin.pinned -isnot [bool] -or -not $pin.pinned) {
            throw "capture_pin.pinned must be the JSON boolean true"
        }
        if ($pin.PSObject.Properties.Name -cnotcontains "window_mode" -or $pin.window_mode -isnot [string] -or
            $pin.window_mode -cnotin @("windowed", "borderless", "fullscreen", "headless")) {
            throw "capture_pin.window_mode must name a supported window mode"
        }
        if ($pin.capture_width -ne $expected.Width -or $pin.pinned_width -ne $expected.Width -or
            $pin.capture_height -ne $expected.Height -or $pin.pinned_height -ne $expected.Height) {
            throw "capture dimensions must match trusted profile '$Profile' ($($expected.Width)x$($expected.Height))"
        }
        if ($PSBoundParameters.ContainsKey("ScreenshotPaths")) {
            if ($null -eq $ScreenshotPaths -or $ScreenshotPaths.Count -eq 0) { throw "explicit screenshot list is empty" }
            foreach ($screenshot in $ScreenshotPaths) {
                $ppm = Assert-PpmArtifact -Path $screenshot -ArtifactDir $ArtifactDir -Name $Name -PassThru
                if ($ppm.Width -ne $expected.Width -or $ppm.Height -ne $expected.Height) {
                    throw "screenshot '$screenshot' is $($ppm.Width)x$($ppm.Height); expected $($expected.Width)x$($expected.Height)"
                }
            }
        }
        # Success establishes only structure, dimensions and this latest state
        # record. It does not establish capture-frame identity or visual approval.
    } catch {
        throw "$Name artifact '$metadataPath': $($_.Exception.Message)"
    }
}
