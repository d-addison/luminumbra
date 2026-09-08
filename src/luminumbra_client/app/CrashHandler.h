#pragma once

#include <cstdint>

// Process-wide crash handling for the client app, extracted verbatim from
// main_client.cpp. On Windows the installed unhandled-exception filter writes
// a symbolized stack trace + minidump into the recorder's crash dir and marks
// the last-known-runtime artifact; elsewhere installation only records the
// recorder so forced-crash plumbing stays uniform.

namespace Luminumbra::Client::App {

class RuntimeStateRecorder;

// Installs the crash handler and remembers `recorder` (TU-local) so the
// exception filter can reach the crash dir + last-known-runtime artifact.
// The recorder must outlive the process's crash-prone lifetime (main owns it
// on the stack for the whole run).
void InstallRuntimeCrashHandler(RuntimeStateRecorder& recorder);

// Deliberately raises a fatal error so gates can exercise the crash-artifact
// pipeline end to end (--forced-crash).
[[noreturn]] void TriggerForcedCrash();

// Hang diagnostics (called from the HangWatchdog thread): on Windows suspends the
// main thread, writes its symbolized stack to <crash_dir>/hang-<ts>.txt and a
// minidump beside it, resumes the thread and marks the runtime state
// "hang_suspected". Elsewhere writes the text record only. Never throws.
void ReportMainThreadHang(std::uint64_t last_heartbeat, double stalled_seconds);

} // namespace Luminumbra::Client::App
