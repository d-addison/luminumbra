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
// A heartbeat of zero means the loop has not started; nothing is reported until
// the first beat. The callback runs on the watchdog thread inside a catch-all so
// a diagnostic failure can never terminate the application. Pure standard
// library; `poll` is exposed with an injected clock so the logic is testable
// without threads or real time.
class HangWatchdog {
public:
    using Clock = std::chrono::steady_clock;
    using HeartbeatReader = std::function<std::uint64_t()>;
    using StallHandler = std::function<void(std::uint64_t last_heartbeat, double stalled_seconds)>;
    using Now = std::function<Clock::time_point()>;

    // start_thread=false constructs a manual watchdog driven only by poll().
    HangWatchdog(HeartbeatReader heartbeat,
                 std::chrono::milliseconds stall_after,
                 StallHandler on_stall,
                 std::chrono::milliseconds poll_interval = std::chrono::milliseconds(250),
                 bool start_thread = true,
                 Now now = nullptr);
    ~HangWatchdog();

    HangWatchdog(const HangWatchdog&) = delete;
    HangWatchdog& operator=(const HangWatchdog&) = delete;

    // One evaluation step; returns true when a stall report was issued. Safe to
    // call from tests; the polling thread calls it every poll_interval.
    bool poll();

    // Idempotent; joins the polling thread.
    void stop();

    std::uint64_t stall_reports() const {
        return m_stall_reports.load();
    }

private:
    void run();

    HeartbeatReader m_heartbeat;
    std::chrono::milliseconds m_stall_after;
    StallHandler m_on_stall;
    std::chrono::milliseconds m_poll_interval;
    Now m_now;
    std::uint64_t m_last_seen = 0;
    Clock::time_point m_last_change{};
    bool m_have_baseline = false;
    bool m_reported_this_episode = false;
    std::atomic<bool> m_stop{false};
    std::atomic<std::uint64_t> m_stall_reports{0};
    std::thread m_thread;
};

} // namespace Luminumbra::Client::App
