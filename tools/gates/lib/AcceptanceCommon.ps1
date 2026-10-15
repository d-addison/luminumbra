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
    # Start-Process joins an array with bare spaces, so each value is turned into one
    # command-line word using the standard Windows (MSVCRT) quoting rules:
    #  - values that are non-empty and contain no whitespace or double quote are emitted as is;
    #  - every other value is wrapped in double quotes;
    #  - a run of backslashes before a double quote, or before the closing quote, is doubled;
    #  - each double quote is written as \" (and a backslash run before it is doubled first).
    param([string[]]$Values)
    $parts = @()
    foreach ($value in $Values) {
        $text = [string]$value
        if ($text.Length -gt 0 -and $text -notmatch '[\s"]') {
            $parts += $text
            continue
        }
        $sb = New-Object System.Text.StringBuilder
        [void]$sb.Append([char]'"')
        $pending = 0
        foreach ($ch in $text.ToCharArray()) {
            if ($ch -eq [char]'\') {
                $pending++
                continue
            }
            if ($ch -eq [char]'"') {
                [void]$sb.Append([string]([char]'\') * ($pending * 2 + 1))
                [void]$sb.Append([char]'"')
            } else {
                [void]$sb.Append([string]([char]'\') * $pending)
                [void]$sb.Append($ch)
            }
            $pending = 0
        }
        [void]$sb.Append([string]([char]'\') * ($pending * 2))
        [void]$sb.Append([char]'"')
        $parts += $sb.ToString()
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

function Copy-AcceptanceEvidence {
    # Stage a byte-for-byte copy before replacing the destination. This also
    # works when source and destination name the same file through different
    # casing or Windows short/long paths, without comparing path strings.
    param(
        [Parameter(Mandatory = $true)][string]$Source,
        [Parameter(Mandatory = $true)][string]$Destination
    )
    $full = Get-AbsolutePath $Destination
    $dir = Split-Path -Parent $full
    if (-not (Test-Path -LiteralPath $dir -PathType Container)) {
        New-Item -ItemType Directory -Force -Path $dir | Out-Null
    }
    $staged = Join-Path $dir (".acceptance-evidence-" + [System.Guid]::NewGuid().ToString() + ".tmp")
    try {
        Copy-Item -LiteralPath $Source -Destination $staged
        Move-Item -LiteralPath $staged -Destination $full -Force
    } finally {
        if (Test-Path -LiteralPath $staged) { Remove-Item -LiteralPath $staged -Force }
    }
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

function Initialize-AcceptanceJobLauncher {
    # Compile lazily: dot-sourcing and -DryRun do not load native interop.
    # C# 5 syntax keeps Add-Type compatible with Windows PowerShell 5.1.
    if ($null -ne ("Luminumbra.AcceptanceJobLauncher" -as [type])) { return }
    Add-Type -TypeDefinition @'
using System;
using System.ComponentModel;
using System.Diagnostics;
using System.Runtime.InteropServices;
using System.Text;
using System.Threading;

namespace Luminumbra {
    public sealed class AcceptanceSessionResult {
        public int ExitCode;
        public bool TimedOut;
    }

    public static class AcceptanceJobLauncher {
        const uint CREATE_SUSPENDED = 0x00000004;
        const uint CREATE_NO_WINDOW = 0x08000000;
        const uint EXTENDED_STARTUPINFO_PRESENT = 0x00080000;
        const uint STARTF_USESTDHANDLES = 0x00000100;
        const uint JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE = 0x00002000;
        const int JobObjectExtendedLimitInformation = 9;
        const int JobObjectBasicAccountingInformation = 1;
        const int CleanupWaitMilliseconds = 10000;
        static readonly IntPtr InvalidHandle = new IntPtr(-1);

        [StructLayout(LayoutKind.Sequential)]
        struct SecurityAttributes {
            public int Length;
            public IntPtr SecurityDescriptor;
            public int InheritHandle;
        }
        [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
        struct StartupInfo {
            public int Size;
            public string Reserved, Desktop, Title;
            public uint X, Y, XSize, YSize, XCountChars, YCountChars, FillAttribute, Flags;
            public ushort ShowWindow, Reserved2Size;
            public IntPtr Reserved2, StdInput, StdOutput, StdError;
        }
        [StructLayout(LayoutKind.Sequential)]
        struct StartupInfoEx {
            public StartupInfo StartupInfo;
            public IntPtr AttributeList;
        }
        [StructLayout(LayoutKind.Sequential)]
        struct ProcessInformation {
            public IntPtr Process, Thread;
            public uint ProcessId, ThreadId;
        }
        [StructLayout(LayoutKind.Sequential)]
        struct BasicLimitInformation {
            public long PerProcessUserTimeLimit, PerJobUserTimeLimit;
            public uint LimitFlags;
            public UIntPtr MinimumWorkingSetSize, MaximumWorkingSetSize;
            public uint ActiveProcessLimit;
            public UIntPtr Affinity;
            public uint PriorityClass, SchedulingClass;
        }
        [StructLayout(LayoutKind.Sequential)]
        struct IoCounters {
            public ulong ReadOperationCount, WriteOperationCount, OtherOperationCount;
            public ulong ReadTransferCount, WriteTransferCount, OtherTransferCount;
        }
        [StructLayout(LayoutKind.Sequential)]
        struct ExtendedLimitInformation {
            public BasicLimitInformation BasicLimitInformation;
            public IoCounters IoInfo;
            public UIntPtr ProcessMemoryLimit, JobMemoryLimit, PeakProcessMemoryUsed, PeakJobMemoryUsed;
        }
        [StructLayout(LayoutKind.Sequential)]
        struct BasicAccountingInformation {
            public long TotalUserTime, TotalKernelTime, ThisPeriodTotalUserTime, ThisPeriodTotalKernelTime;
            public uint TotalPageFaultCount, TotalProcesses, ActiveProcesses, TotalTerminatedProcesses;
        }

        [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
        static extern IntPtr CreateJobObject(IntPtr attributes, string name);
        [DllImport("kernel32.dll", SetLastError = true)]
        static extern bool SetInformationJobObject(IntPtr job, int infoClass,
            ref ExtendedLimitInformation information, uint length);
        [DllImport("kernel32.dll", SetLastError = true)]
        static extern bool QueryInformationJobObject(IntPtr job, int infoClass,
            out BasicAccountingInformation information, uint length, IntPtr returnLength);
        [DllImport("kernel32.dll", SetLastError = true)]
        static extern bool AssignProcessToJobObject(IntPtr job, IntPtr process);
        [DllImport("kernel32.dll", SetLastError = true)]
        static extern bool TerminateJobObject(IntPtr job, uint exitCode);
        [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
        static extern bool CreateProcess(string application, StringBuilder commandLine,
            IntPtr processAttributes, IntPtr threadAttributes, bool inheritHandles,
            uint flags, IntPtr environment, string directory, ref StartupInfoEx startup,
            out ProcessInformation information);
        [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
        static extern IntPtr CreateFile(string path, uint access, uint share,
            ref SecurityAttributes attributes, uint disposition, uint flags, IntPtr template);
        [DllImport("kernel32.dll", SetLastError = true)]
        static extern bool InitializeProcThreadAttributeList(IntPtr list, int count, uint flags, ref IntPtr size);
        [DllImport("kernel32.dll", SetLastError = true)]
        static extern bool UpdateProcThreadAttribute(IntPtr list, uint flags, IntPtr attribute,
            IntPtr value, IntPtr size, IntPtr previous, IntPtr returnSize);
        [DllImport("kernel32.dll")]
        static extern void DeleteProcThreadAttributeList(IntPtr list);
        [DllImport("kernel32.dll", SetLastError = true)]
        static extern uint ResumeThread(IntPtr thread);
        [DllImport("kernel32.dll", SetLastError = true)]
        static extern bool GetExitCodeProcess(IntPtr process, out uint exitCode);
        [DllImport("kernel32.dll", SetLastError = true)]
        static extern bool TerminateProcess(IntPtr process, uint exitCode);
        [DllImport("kernel32.dll", SetLastError = true)]
        static extern uint WaitForSingleObject(IntPtr handle, uint milliseconds);
        [DllImport("kernel32.dll")]
        static extern bool CloseHandle(IntPtr handle);

        static Win32Exception NativeError(string operation) {
            int code = Marshal.GetLastWin32Error();
            return new Win32Exception(code, operation + " (Win32 error " + code + ": " +
                new Win32Exception(code).Message + ")");
        }
        static void Close(IntPtr handle) {
            if (handle != IntPtr.Zero && handle != InvalidHandle) CloseHandle(handle);
        }
        static IntPtr OpenStream(string path, bool input) {
            SecurityAttributes attributes = new SecurityAttributes();
            attributes.Length = Marshal.SizeOf(typeof(SecurityAttributes));
            attributes.InheritHandle = 1;
            // GENERIC_READ / GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
            // OPEN_EXISTING (NUL) / CREATE_ALWAYS (output), FILE_ATTRIBUTE_NORMAL.
            IntPtr handle = CreateFile(path, input ? 0x80000000u : 0x40000000u, 3,
                ref attributes, input ? 3u : 2u, 0x80, IntPtr.Zero);
            if (handle == InvalidHandle) throw NativeError("CreateFile: " + path);
            return handle;
        }
        static uint ActiveProcesses(IntPtr job) {
            BasicAccountingInformation information;
            if (!QueryInformationJobObject(job, JobObjectBasicAccountingInformation,
                out information, (uint)Marshal.SizeOf(typeof(BasicAccountingInformation)), IntPtr.Zero))
                throw NativeError("QueryInformationJobObject: cannot verify process cleanup");
            return information.ActiveProcesses;
        }
        static bool WaitUntilEmpty(IntPtr job, int milliseconds) {
            Stopwatch watch = Stopwatch.StartNew();
            while (ActiveProcesses(job) != 0) {
                if (watch.ElapsedMilliseconds >= milliseconds) return false;
                Thread.Sleep(25);
            }
            return true;
        }
        static void TerminateAndDrain(IntPtr job) {
            if (!TerminateJobObject(job, 1)) throw NativeError("TerminateJobObject");
            if (!WaitUntilEmpty(job, CleanupWaitMilliseconds))
                throw new InvalidOperationException("acceptance job still has active processes after 10000 ms cleanup wait");
        }

        public static AcceptanceSessionResult Run(string exe, string commandLine,
            string directory, string stdout, string stderr, int timeoutMilliseconds) {
            IntPtr job = IntPtr.Zero, input = IntPtr.Zero, output = IntPtr.Zero, error = IntPtr.Zero;
            IntPtr attributes = IntPtr.Zero, handleList = IntPtr.Zero;
            bool attributesInitialized = false, assigned = false, drained = false, cleanupAttempted = false;
            ProcessInformation process = new ProcessInformation();
            try {
                job = CreateJobObject(IntPtr.Zero, null);
                if (job == IntPtr.Zero) throw NativeError("CreateJobObject");
                ExtendedLimitInformation limits = new ExtendedLimitInformation();
                // No breakaway flags: descendants inherit membership, including
                // children of exited intermediates. The job handle is not inherited.
                limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
                if (!SetInformationJobObject(job, JobObjectExtendedLimitInformation, ref limits,
                    (uint)Marshal.SizeOf(typeof(ExtendedLimitInformation))))
                    throw NativeError("SetInformationJobObject: KILL_ON_JOB_CLOSE");
                input = OpenStream("NUL", true);
                output = OpenStream(stdout, false);
                error = OpenStream(stderr, false);
                StartupInfoEx startup = new StartupInfoEx();
                startup.StartupInfo.Size = Marshal.SizeOf(typeof(StartupInfoEx));
                startup.StartupInfo.Flags = STARTF_USESTDHANDLES;
                startup.StartupInfo.StdInput = input;
                startup.StartupInfo.StdOutput = output;
                startup.StartupInfo.StdError = error;
                // Inherit only these three streams, never other handles held by
                // the PowerShell host or another session on the shared machine.
                IntPtr size = IntPtr.Zero;
                InitializeProcThreadAttributeList(IntPtr.Zero, 1, 0, ref size);
                if (size == IntPtr.Zero) throw NativeError("InitializeProcThreadAttributeList: size");
                attributes = Marshal.AllocHGlobal(size);
                if (!InitializeProcThreadAttributeList(attributes, 1, 0, ref size))
                    throw NativeError("InitializeProcThreadAttributeList");
                attributesInitialized = true;
                handleList = Marshal.AllocHGlobal(3 * IntPtr.Size);
                Marshal.WriteIntPtr(handleList, 0, input);
                Marshal.WriteIntPtr(handleList, IntPtr.Size, output);
                Marshal.WriteIntPtr(handleList, 2 * IntPtr.Size, error);
                // PROC_THREAD_ATTRIBUTE_HANDLE_LIST = 0x00020002.
                if (!UpdateProcThreadAttribute(attributes, 0, new IntPtr(0x00020002),
                    handleList, new IntPtr(3 * IntPtr.Size), IntPtr.Zero, IntPtr.Zero))
                    throw NativeError("UpdateProcThreadAttribute: HANDLE_LIST");
                startup.AttributeList = attributes;
                if (!CreateProcess(exe, new StringBuilder(commandLine), IntPtr.Zero, IntPtr.Zero,
                    true, CREATE_SUSPENDED | CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT,
                    IntPtr.Zero, directory, ref startup, out process))
                    throw NativeError("CreateProcess: suspended acceptance session");
                // Windows 8+ supports nested jobs. Do not request breakaway or
                // fall back if the host's job restrictions forbid assignment.
                if (!AssignProcessToJobObject(job, process.Process))
                    throw NativeError("AssignProcessToJobObject failed (including host/nested-job restrictions); session was not resumed");
                assigned = true;
                if (ResumeThread(process.Thread) == UInt32.MaxValue) throw NativeError("ResumeThread");
                AcceptanceSessionResult result = new AcceptanceSessionResult();
                // Root exit alone is insufficient: keep ownership until every
                // descendant exits or the session deadline terminates the job.
                if (!WaitUntilEmpty(job, timeoutMilliseconds)) {
                    cleanupAttempted = true;
                    TerminateAndDrain(job);
                    result.ExitCode = -1;
                    result.TimedOut = true;
                } else {
                    uint exitCode;
                    if (!GetExitCodeProcess(process.Process, out exitCode)) throw NativeError("GetExitCodeProcess");
                    result.ExitCode = unchecked((int)exitCode);
                }
                drained = true;
                return result;
            } finally {
                try {
                    if (assigned && !drained && !cleanupAttempted) {
                        // Also verify cleanup on errors, not just timeouts.
                        TerminateAndDrain(job);
                    } else if (!assigned && process.Process != IntPtr.Zero) {
                        // Assignment failed while suspended. Use the original
                        // creation handle, never a PID lookup, to stop this root.
                        if (!TerminateProcess(process.Process, 1)) throw NativeError("TerminateProcess: unassigned suspended root");
                        if (WaitForSingleObject(process.Process, CleanupWaitMilliseconds) != 0)
                            throw new InvalidOperationException("unassigned suspended acceptance process did not exit");
                    }
                } finally {
                    Close(process.Thread);
                    Close(process.Process);
                    Close(job); // KILL_ON_JOB_CLOSE is the final safety net.
                    Close(input);
                    Close(output);
                    Close(error);
                    if (attributesInitialized) DeleteProcThreadAttributeList(attributes);
                    if (attributes != IntPtr.Zero) Marshal.FreeHGlobal(attributes);
                    if (handleList != IntPtr.Zero) Marshal.FreeHGlobal(handleList);
                }
            }
        }
    }
}
'@
}

function Invoke-ClientSession {
    # On Windows, owns the client/ctest and all descendants from suspended launch.
    # Other platforms retain root-only cleanup; not native qualification.
    # Returns @{ exit_code = <int, -1 on timeout>; timed_out = <bool> }.
    param(
        [Parameter(Mandatory = $true)][string]$Exe,
        [Parameter(Mandatory = $true)][AllowEmptyCollection()][AllowEmptyString()][string[]]$ArgumentList,
        [Parameter(Mandatory = $true)][string]$WorkingDirectory,
        [Parameter(Mandatory = $true)][string]$StdoutPath,
        [Parameter(Mandatory = $true)][string]$StderrPath,
        [ValidateRange(1, 86400)][int]$TimeoutSeconds = 300
    )
    $stdout = Get-AbsolutePath $StdoutPath
    $stderr = Get-AbsolutePath $StderrPath
    foreach ($file in @($stdout, $stderr)) {
        $dir = Split-Path -Parent $file
        if (-not (Test-Path -LiteralPath $dir -PathType Container)) {
            New-Item -ItemType Directory -Force -Path $dir | Out-Null
        }
    }
    if ([System.Environment]::OSVersion.Platform -eq [System.PlatformID]::Win32NT) {
        Initialize-AcceptanceJobLauncher
        $exePath = Get-AbsolutePath $Exe
        $commandLine = Join-ProcessArguments -Values (@($exePath) + @($ArgumentList))
        $run = [Luminumbra.AcceptanceJobLauncher]::Run($exePath, $commandLine,
            (Get-AbsolutePath $WorkingDirectory), $stdout, $stderr, ($TimeoutSeconds * 1000))
        return @{ exit_code = $run.ExitCode; timed_out = $run.TimedOut }
    }
    $process = Start-Process -FilePath $Exe `
        -ArgumentList (Join-ProcessArguments -Values $ArgumentList) `
        -WorkingDirectory (Get-AbsolutePath $WorkingDirectory) `
        -PassThru -NoNewWindow `
        -RedirectStandardOutput $stdout -RedirectStandardError $stderr
    try {
        $finished = $process.WaitForExit($TimeoutSeconds * 1000)
        if (-not $finished) {
            $process.Kill()
            if (-not $process.WaitForExit(10000)) { throw "timed-out acceptance process still running" }
            return @{ exit_code = -1; timed_out = $true }
        }
        $code = $process.ExitCode
        if ($null -eq $code) { $code = -1 }
        return @{ exit_code = [int]$code; timed_out = $false }
    } finally {
        $process.Dispose()
    }
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
    $worlds = @(Get-ChildItem -LiteralPath $saves -Directory -Force)
    if ($worlds.Count -ne 1) {
        throw "expected exactly one fixture world under $saves, found $($worlds.Count)"
    }
    return $worlds[0].Name
}
