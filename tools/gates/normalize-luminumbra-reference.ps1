[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$InputPath,

    [Parameter(Mandatory = $true)]
    [string]$OutputPath,

    [ValidateRange(1, 16384)]
    [int]$Width = 3840,

    [ValidateRange(1, 16384)]
    [int]$Height = 1600
)

$ErrorActionPreference = "Stop"
Add-Type -AssemblyName System.Drawing

$resolvedInput = (Resolve-Path -LiteralPath $InputPath).Path
$outputFullPath = [IO.Path]::GetFullPath($OutputPath)
$outputDirectory = Split-Path -Parent $outputFullPath
if ([string]::IsNullOrWhiteSpace($outputDirectory)) { throw "OutputPath must have a parent directory" }
[IO.Directory]::CreateDirectory($outputDirectory) | Out-Null

$source = [Drawing.Image]::FromFile($resolvedInput)
try {
    if ($source.Width -le 0 -or $source.Height -le 0) { throw "Reference image has invalid dimensions: $resolvedInput" }

    $scale = [Math]::Min($Width / [double]$source.Width, $Height / [double]$source.Height)
    $drawWidth = [Math]::Max(1, [int][Math]::Round($source.Width * $scale, [MidpointRounding]::AwayFromZero))
    $drawHeight = [Math]::Max(1, [int][Math]::Round($source.Height * $scale, [MidpointRounding]::AwayFromZero))
    $drawX = [int][Math]::Floor(($Width - $drawWidth) / 2.0)
    $drawY = [int][Math]::Floor(($Height - $drawHeight) / 2.0)

    $bitmap = [Drawing.Bitmap]::new($Width, $Height, [Drawing.Imaging.PixelFormat]::Format24bppRgb)
    try {
        $bitmap.SetResolution(96.0, 96.0)
        $graphics = [Drawing.Graphics]::FromImage($bitmap)
        try {
            $graphics.CompositingMode = [Drawing.Drawing2D.CompositingMode]::SourceCopy
            $graphics.CompositingQuality = [Drawing.Drawing2D.CompositingQuality]::HighQuality
            $graphics.InterpolationMode = [Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
            $graphics.PixelOffsetMode = [Drawing.Drawing2D.PixelOffsetMode]::HighQuality
            $graphics.SmoothingMode = [Drawing.Drawing2D.SmoothingMode]::None
            $graphics.Clear([Drawing.Color]::Black)
            $destination = [Drawing.Rectangle]::new($drawX, $drawY, $drawWidth, $drawHeight)
            $graphics.DrawImage($source, $destination, 0, 0, $source.Width, $source.Height, [Drawing.GraphicsUnit]::Pixel)
        } finally {
            $graphics.Dispose()
        }
        $bitmap.Save($outputFullPath, [Drawing.Imaging.ImageFormat]::Png)
    } finally {
        $bitmap.Dispose()
    }
} finally {
    $source.Dispose()
}

$written = [Drawing.Image]::FromFile($outputFullPath)
try {
    if ($written.Width -ne $Width -or $written.Height -ne $Height) {
        throw "Normalized reference has unexpected dimensions: $($written.Width)x$($written.Height)"
    }
} finally {
    $written.Dispose()
}

Write-Output $outputFullPath
