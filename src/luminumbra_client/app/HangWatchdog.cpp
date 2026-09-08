#include "app/HangWatchdog.h"

#include <utility>

namespace Luminumbra::Client::App {

HangWatchdog::HangWatchdog(HeartbeatReader heartbeat,
                           std::chrono::milliseconds stall_after,
                           StallHandler on_stall,
                           std::chrono::milliseconds poll_interval)
    : m_heartbeat(std::move(heartbeat))
    , m_stall_after(stall_after)
    , m_on_stall(std::move(on_stall))
    , m_poll_interval(poll_interval)
    , m_thread([this] { run(); }) {}

HangWatchdog::~HangWatchdog() {
    stop();
}

void HangWatchdog::stop() {
    m_stop.store(true);
    if (m_thread.joinable()) {
        m_thread.join();
    }
}

void HangWatchdog::run() {
    using clock = std::chrono::steady_clock;
    std::uint64_t last_seen = m_heartbeat();
    clock::time_point last_change = clock::now();
    bool reported_this_episode = false;
    while (!m_stop.load()) {
        std::this_thread::sleep_for(m_poll_interval);
        if (m_stop.load()) {
            break;
        }
        const std::uint64_t now_beat = m_heartbeat();
        const clock::time_point now = clock::now();
        if (now_beat != last_seen) {
            last_seen = now_beat;
            last_change = now;
            reported_this_episode = false;
            continue;
        }
        if (reported_this_episode || now - last_change < m_stall_after) {
            continue;
        }
        reported_this_episode = true;
        m_stall_reports.fetch_add(1);
        const double stalled_seconds = std::chrono::duration<double>(now - last_change).count();
        if (m_on_stall) {
            m_on_stall(last_seen, stalled_seconds);
        }
    }
}

} // namespace Luminumbra::Client::App
