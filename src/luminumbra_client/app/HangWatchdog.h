#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <thread>

namespace Luminumbra::Client::App {

// Opt-in main-loop stall detector. The main thread bumps a heartbeat once per
// frame and once per shutdown milestone; a background thread polls it and, when
// the heartbeat has not advanced for `stall_after`, invokes `on_stall` exactly
// once per stall episode (it re-arms only after the heartbeat advances again).
// The callback runs on the watchdog thread; the Windows client uses it to write a
// symbolized main-thread stack and a minidump, which the unhandled-exception
// filter can never do for a hang because no exception is raised. Pure standard
// library so it is unit-testable without a window, device or GL context.
class HangWatchdog {
public:
    using HeartbeatReader = std::function<std::uint64_t()>;
    using StallHandler = std::function<void(std::uint64_t last_heartbeat, double stalled_seconds)>;

    HangWatchdog(HeartbeatReader heartbeat,
                 std::chrono::milliseconds stall_after,
                 StallHandler on_stall,
                 std::chrono::milliseconds poll_interval = std::chrono::milliseconds(250));
    ~HangWatchdog();

    HangWatchdog(const HangWatchdog&) = delete;
    HangWatchdog& operator=(const HangWatchdog&) = delete;

    // Idempotent; joins the polling thread. Safe to call from the main thread even
    // while a stall report is being written (the report finishes first).
    void stop();

    // Number of stall episodes reported so far (diagnostic/testing).
    std::uint64_t stall_reports() const {
        return m_stall_reports.load();
    }

private:
    void run();

    HeartbeatReader m_heartbeat;
    std::chrono::milliseconds m_stall_after;
    StallHandler m_on_stall;
    std::chrono::milliseconds m_poll_interval;
    std::atomic<bool> m_stop{false};
    std::atomic<std::uint64_t> m_stall_reports{0};
    std::thread m_thread;
};

} // namespace Luminumbra::Client::App
