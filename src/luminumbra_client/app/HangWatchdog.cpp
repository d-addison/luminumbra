#include "app/HangWatchdog.h"

#include <utility>

namespace Luminumbra::Client::App {

HangWatchdog::HangWatchdog(HeartbeatReader heartbeat,
                           std::chrono::milliseconds stall_after,
                           StallHandler on_stall,
                           std::chrono::milliseconds poll_interval,
                           bool start_thread,
                           Now now)
    : m_heartbeat(std::move(heartbeat))
    , m_stall_after(stall_after)
    , m_on_stall(std::move(on_stall))
    , m_poll_interval(poll_interval)
    , m_now(now ? std::move(now) : Now([] { return Clock::now(); })) {
    if (start_thread) {
        m_thread = std::thread([this] { run(); });
    }
}

HangWatchdog::~HangWatchdog() {
    stop();
}

void HangWatchdog::stop() {
    m_stop.store(true);
    if (m_thread.joinable()) {
        m_thread.join();
    }
}

bool HangWatchdog::poll() {
    const std::uint64_t beat = m_heartbeat();
    const Clock::time_point now = m_now();
    if (beat == 0) {
        // The main loop has not started; startup is not a stall.
        m_have_baseline = false;
        return false;
    }
    if (!m_have_baseline || beat != m_last_seen) {
        m_have_baseline = true;
        m_last_seen = beat;
        m_last_change = now;
        m_reported_this_episode = false;
        return false;
    }
    if (m_reported_this_episode || now - m_last_change < m_stall_after) {
        return false;
    }
    m_reported_this_episode = true;
    m_stall_reports.fetch_add(1);
    const double stalled_seconds = std::chrono::duration<double>(now - m_last_change).count();
    if (m_on_stall) {
        try {
            m_on_stall(m_last_seen, stalled_seconds);
        } catch (...) {
            // A diagnostic must never take the application down with it.
        }
    }
    return true;
}

void HangWatchdog::run() {
    while (!m_stop.load()) {
        std::this_thread::sleep_for(m_poll_interval);
        if (m_stop.load()) {
            break;
        }
        poll();
    }
}

} // namespace Luminumbra::Client::App
