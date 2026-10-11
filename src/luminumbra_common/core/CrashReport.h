#pragma once

#include <filesystem>
#include <string_view>

namespace luminumbra::crash {

/// Installs process-level crash capture that writes a text report into crash_dir.
///
/// The directory is created if needed and crash-<pid>.txt is opened and given its
/// header now, so the crash path never allocates or opens files. Callers must invoke
/// this from the process entry point on the thread that should get the alternate signal
/// stack; calling it again replaces the previous report file and handlers. Returns false
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
/// Reinstalling into the same directory replaces the report. On Windows the previous
/// report handle is closed before the file is recreated; if the recreate fails, crash
/// capture stays disabled until the next successful install.
bool InstallCrashCapture(const std::filesystem::path& crash_dir, std::string_view program_name);

/// Report file opened by the last successful InstallCrashCapture, or empty before install.
const std::filesystem::path& CrashReportPath();

} // namespace luminumbra::crash
