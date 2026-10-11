#pragma once

#include <filesystem>
#include <string_view>

namespace luminumbra::crash {

/// Installs process-level crash capture that writes a text report into crash_dir.
/// The directory is created if needed and crash-<pid>.txt is opened and given its
/// header now, so the crash path itself never allocates or opens files. Callers
/// must invoke this from the process entry point; calling it again replaces the
/// previous report file and handlers. Returns false if the directory or report
/// file cannot be created.
bool InstallCrashCapture(const std::filesystem::path& crash_dir, std::string_view program_name);

/// Report file opened by the last successful InstallCrashCapture, or empty before install.
const std::filesystem::path& CrashReportPath();

} // namespace luminumbra::crash
