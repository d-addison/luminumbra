# Converts a binary P6 PPM capture to PNG (System.Drawing).
param(
    [Parameter(Mandatory = $true)][string]$PpmPath,
    [string]$PngPath
)
$ErrorActionPreference = "Stop"
if ([string]::IsNullOrWhiteSpace($PngPath)) {
    $PngPath = [System.IO.Path]::ChangeExtension($PpmPath, ".png")
}
Add-Type -AssemblyName System.Drawing

$bytes = [System.IO.File]::ReadAllBytes($PpmPath)
$script:pos = 0
function Read-Token {
    while ($script:pos -lt $bytes.Length -and [char]$bytes[$script:pos] -match '\s') { $script:pos++ }
    if ($script:pos -lt $bytes.Length -and [char]$bytes[$script:pos] -eq '#') {
        while ($script:pos -lt $bytes.Length -and [char]$bytes[$script:pos] -ne "`n") { $script:pos++ }
        return Read-Token
    }
    $start = $script:pos
    while ($script:pos -lt $bytes.Length -and -not ([char]$bytes[$script:pos] -match '\s')) { $script:pos++ }
    return [System.Text.Encoding]::ASCII.GetString($bytes, $start, $script:pos - $start)
}
$magic = Read-Token
if ($magic -ne "P6") { throw "Not a binary P6 PPM: $PpmPath (magic '$magic')" }
$width = [int](Read-Token)
$height = [int](Read-Token)
$maxval = [int](Read-Token)
if ($maxval -ne 255) { throw "Unsupported maxval $maxval (expected 255)" }
$script:pos++ # single whitespace after maxval

$bitmap = New-Object System.Drawing.Bitmap($width, $height, [System.Drawing.Imaging.PixelFormat]::Format24bppRgb)
$rect = New-Object System.Drawing.Rectangle(0, 0, $width, $height)
$data = $bitmap.LockBits($rect, [System.Drawing.Imaging.ImageLockMode]::WriteOnly, $bitmap.PixelFormat)
try {
    $row = New-Object byte[] ($width * 3)
    for ($y = 0; $y -lt $height; $y++) {
        $src = $script:pos + $y * $width * 3
        for ($x = 0; $x -lt $width; $x++) {
            # PPM is RGB, GDI+ scanlines are BGR
            $row[$x * 3]     = $bytes[$src + $x * 3 + 2]
            $row[$x * 3 + 1] = $bytes[$src + $x * 3 + 1]
            $row[$x * 3 + 2] = $bytes[$src + $x * 3]
        }
        $dst = [IntPtr]::Add($data.Scan0, $y * $data.Stride)
        [System.Runtime.InteropServices.Marshal]::Copy($row, 0, $dst, $row.Length)
    }
}
finally {
    $bitmap.UnlockBits($data)
}
$bitmap.Save($PngPath, [System.Drawing.Imaging.ImageFormat]::Png)
$bitmap.Dispose()
Write-Host "wrote $PngPath ($width x $height)"
