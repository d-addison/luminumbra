#pragma once

#include <filesystem>
#include <string_view>

namespace luminumbra::crash {

/// Installs process-level crash capture that writes a text report into crash_dir.
///
/// The directory is created if needed and a fresh crash-<pid>.txt (or
/// crash-<pid>-<suffix>.txt on collision) is opened and given its header now, so the
/// crash path never allocates or opens files. Existing files are never truncated.
/// Callers must invoke
/// this from the process entry point on the thread that should get the alternate signal
/// stack; calling it again installs the requested target and handlers. POSIX rejects
/// reinstallation from a different thread with a stderr diagnostic. Returns false
/// if the directory or report file cannot be created.
///
/// What the report contains:
///   POSIX: the signal number and name, si_code, the fault address, and the faulting
///   program counter and stack pointer (x86_64 and aarch64 Linux only). The handler does
///   not unwind the stack: unwinding is not async-signal-safe, so full stacks come from
///   core files or an external debugger. The handler re-raises the signal with the default
///   disposition, so the process still terminates with the original signal.
///   Windows: the exception code and address, the cached main-module name and RVA, the
///   faulting thread id, and a frame list unwound from the exception context. The
///   exception is passed on to WER afterwards so LocalDumps can still write a dump.
///
/// Stack overflow is captured only on the thread that installed the handler. Worker
/// threads do not get an alternate stack, so an overflow there is not reported.
///
/// Reinstalling into a previously used directory reuses its descriptor/handle when
/// the report's file identity is unchanged, preserving its header, contents and offset
/// (including the original program name). Otherwise a fresh file is created exclusively.
/// At most eight distinct report targets are retained until process exit, so a handler
/// racing reinstallation can safely finish writing. Further new targets are refused
/// with a stderr diagnostic; retained targets can still be reused without consuming
/// resources. A failed reinstall leaves the previous capture target installed.
/// Read CrashReportPath only when installation is not running concurrently.
bool InstallCrashCapture(const std::filesystem::path& crash_dir, std::string_view program_name);

/// Report file opened by the last successful InstallCrashCapture, or empty before install.
const std::filesystem::path& CrashReportPath();

} // namespace luminumbra::crash
