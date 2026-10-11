# AcceptanceCommon.ps1 - shared helpers for the NEEDS-NATIVE-RUN acceptance drivers.
#
# NEEDS-NATIVE-RUN: these helpers launch the QA client and must never run in CI.
# Dot-source only; loading this file has no side effects:
#   . (Join-Path (Join-Path $PSScriptRoot "lib") "AcceptanceCommon.ps1")
# Compatible with Windows PowerShell 5.1 and pwsh. ASCII-only source on purpose.

function Get-AbsolutePath {
    param([Parameter(Mandatory = $true)][string]$Path)
    $resolved = $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath($Path)
    return [System.IO.Path]::GetFullPath($resolved)
}

function Join-ProcessArguments {
    # Start-Process joins an array with bare spaces, so quote every value that
    # contains whitespace or a double quote (and empty values).
    param([string[]]$Values)
    $parts = @()
    foreach ($value in $Values) {
        $text = [string]$value
        if ($text.Length -eq 0 -or $text -match '[\s"]') {
            $parts += ('"' + ($text -replace '"', '\"') + '"')
        } else {
            $parts += $text
        }
    }
    return ($parts -join " ")
}

function Test-JsonProperty {
    # True when a ConvertFrom-Json object has the named property (strict-mode safe).
    param($Object, [Parameter(Mandatory = $true)][string]$Name)
    if ($null -eq $Object) { return $false }
    return (@($Object.PSObject.Properties.Name) -contains $Name)
}

function Read-TextOrEmpty {
    param([Parameter(Mandatory = $true)][string]$Path)
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) { return "" }
    $text = [System.IO.File]::ReadAllText($Path, [System.Text.Encoding]::UTF8)
    if ($null -eq $text) { return "" }
    return $text
}

function Get-FileSha256Hex {
    param([Parameter(Mandatory = $true)][string]$Path)
    $full = Get-AbsolutePath $Path
    $hash = Get-FileHash -LiteralPath $full -Algorithm SHA256
    return $hash.Hash.ToLowerInvariant()
}

function Write-JsonFile {
    # UTF-8 without BOM, LF line endings, depth 12.
    param(
        [Parameter(Mandatory = $true)]$Object,
        [Parameter(Mandatory = $true)][string]$Path
    )
    $full = Get-AbsolutePath $Path
    $dir = Split-Path -Parent $full
    if (-not (Test-Path -LiteralPath $dir -PathType Container)) {
        New-Item -ItemType Directory -Force -Path $dir | Out-Null
    }
    $json = ConvertTo-Json -InputObject $Object -Depth 12
    $json = ($json -replace "`r`n", "`n") + "`n"
    $encoding = New-Object -TypeName System.Text.UTF8Encoding -ArgumentList $false
    [System.IO.File]::WriteAllText($full, $json, $encoding)
}

function Invoke-ClientSession {
    # Runs one client process with stdout/stderr redirected to local files.
    # Returns @{ exit_code = <int, -1 on timeout>; timed_out = <bool> }.
    param(
        [Parameter(Mandatory = $true)][string]$Exe,
        [Parameter(Mandatory = $true)][string[]]$ArgumentList,
        [Parameter(Mandatory = $true)][string]$WorkingDirectory,
        [Parameter(Mandatory = $true)][string]$StdoutPath,
        [Parameter(Mandatory = $true)][string]$StderrPath,
        [int]$TimeoutSeconds = 300
    )
    $stdout = Get-AbsolutePath $StdoutPath
    $stderr = Get-AbsolutePath $StderrPath
    foreach ($file in @($stdout, $stderr)) {
        $dir = Split-Path -Parent $file
        if (-not (Test-Path -LiteralPath $dir -PathType Container)) {
            New-Item -ItemType Directory -Force -Path $dir | Out-Null
        }
    }
    $process = Start-Process -FilePath $Exe `
        -ArgumentList (Join-ProcessArguments -Values $ArgumentList) `
        -WorkingDirectory (Get-AbsolutePath $WorkingDirectory) `
        -PassThru -NoNewWindow `
        -RedirectStandardOutput $stdout -RedirectStandardError $stderr
    $finished = $process.WaitForExit($TimeoutSeconds * 1000)
    if (-not $finished) {
        Stop-Process -Id $process.Id -Force -ErrorAction SilentlyContinue
        $null = $process.WaitForExit(10000)
        return @{ exit_code = -1; timed_out = $true }
    }
    $code = $process.ExitCode
    if ($null -eq $code) { $code = -1 }
    return @{ exit_code = [int]$code; timed_out = $false }
}

function Read-ShutdownRecord {
    # Reads <ArtifactDir>\shutdown.json (schema luminumbra.shutdown.v1).
    # Returns an ordered hashtable {schema, complete (only if present), jobs_drained},
    # or $null when the file is missing or unparseable.
    param([Parameter(Mandatory = $true)][string]$ArtifactDir)
    $path = Join-Path (Get-AbsolutePath $ArtifactDir) "shutdown.json"
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) { return $null }
    try {
        $text = [System.IO.File]::ReadAllText($path, [System.Text.Encoding]::UTF8)
        $document = ConvertFrom-Json -InputObject $text
    } catch {
        return $null
    }
    if ($null -eq $document -or $document -isnot [pscustomobject]) { return $null }
    $record = [ordered]@{}
    $schemaValue = $null
    if (Test-JsonProperty -Object $document -Name "schema") { $schemaValue = $document.schema }
    $record["schema"] = $schemaValue
    if (Test-JsonProperty -Object $document -Name "complete") {
        $record["complete"] = $document.complete
    }
    $drained = $null
    if (Test-JsonProperty -Object $document -Name "jobs_drained") { $drained = $document.jobs_drained }
    $record["jobs_drained"] = $drained
    return $record
}

function Invoke-PythonTool {
    # Runs a Python script with `python`, falling back to `py -3`.
    # Returns @{ exit_code; stdout; stderr }. Output goes to temp files so stderr
    # never turns into PowerShell error records under $ErrorActionPreference = Stop.
    param(
        [Parameter(Mandatory = $true)][string]$ScriptPath,
        [string[]]$ArgumentList = @()
    )
    $script = Get-AbsolutePath $ScriptPath
    $launcher = Get-Command python -CommandType Application -ErrorAction SilentlyContinue | Select-Object -First 1
    $prefix = @()
    if ($null -eq $launcher) {
        $launcher = Get-Command py -CommandType Application -ErrorAction SilentlyContinue | Select-Object -First 1
        $prefix = @("-3")
    }
    if ($null -eq $launcher) { throw "no Python launcher found (tried python and py -3)" }
    $values = @($prefix) + @($script) + @($ArgumentList)
    $stdoutFile = [System.IO.Path]::GetTempFileName()
    $stderrFile = [System.IO.Path]::GetTempFileName()
    try {
        $process = Start-Process -FilePath $launcher.Source `
            -ArgumentList (Join-ProcessArguments -Values $values) `
            -PassThru -Wait -NoNewWindow `
            -RedirectStandardOutput $stdoutFile -RedirectStandardError $stderrFile
        $exitCode = $process.ExitCode
        $stdout = [System.IO.File]::ReadAllText($stdoutFile)
        $stderr = [System.IO.File]::ReadAllText($stderrFile)
    } finally {
        Remove-Item -LiteralPath $stdoutFile, $stderrFile -Force -ErrorAction SilentlyContinue
    }
    if ($null -eq $exitCode) { $exitCode = -1 }
    return @{ exit_code = [int]$exitCode; stdout = $stdout; stderr = $stderr }
}

function Get-ClientExe {
    param(
        [Parameter(Mandatory = $true)][string]$Package,
        [Parameter(Mandatory = $true)][string]$Name
    )
    $path = Join-Path $Package $Name
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
        throw "client executable missing: $path"
    }
    return $path
}


function Get-FixtureWorldId {
    # luminumbra_save_fixture writes exactly one world under <root>/worlds/saves;
    # its id is that directory name. Anything else is an error.
    param([Parameter(Mandatory = $true)][string]$FixtureDir)
    $saves = Join-Path (Join-Path (Get-AbsolutePath $FixtureDir) "worlds") "saves"
    if (-not (Test-Path -LiteralPath $saves -PathType Container)) {
        throw "fixture saves directory missing: $saves"
    }
    $worlds = @(Get-ChildItem -LiteralPath $saves -Directory)
    if ($worlds.Count -ne 1) {
        throw "expected exactly one fixture world under $saves, found $($worlds.Count)"
    }
    return $worlds[0].Name
}
