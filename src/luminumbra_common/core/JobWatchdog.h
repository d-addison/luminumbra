#pragma once

// opt-in named-phase wedge watchdog for potentially-unbounded
// job-system waits. Observability ONLY — it never bails or alters the wait, so it
// is hash-neutral and stays OFF in determinism gates (--smoke). With
// LUMINUMBRA_JOB_WATCHDOG=1, a wait that outlives the report interval names its
// phase every interval instead of hanging silently (the "CONSTRUCTING WORLD
// GEOMETRY" freeze diagnosis surface).

#include "Environment.h"
#include "Log.h"

#include <atomic>
#include <chrono>
#include <string>
#include <thread>
#include <utility>

namespace Luminumbra::Core {

inline bool JobWatchdogEnabled() {
    static const bool enabled = []() {
        const auto value = ReadEnvironment("LUMINUMBRA_JOB_WATCHDOG");
        return value && !value->empty() && value->front() == '1';
    }();
    return enabled;
}

// Runs `wait_fn` (any blocking callable); while it blocks, a reporter thread warns
// with `phase` every `report_interval`. Returns the number of reports emitted (0
// when disabled or the wait finishes inside the first interval) so tests can
// assert the reporter actually fires. `enabled` is a parameter (callers pass
// JobWatchdogEnabled) so the test does not depend on process-env mutation.
template<typename WaitFn>
std::size_t
WaitWithJobWatchdog(bool enabled,
                    std::string phase,
                    WaitFn&& wait_fn,
                    std::chrono::milliseconds report_interval = std::chrono::seconds(30)) {
    if (!enabled) {
        std::forward<WaitFn>(wait_fn)();
        return 0;
    }
    std::atomic<bool> wait_done{false};
    std::atomic<std::size_t> reports{0};
    // Poll granularity: fine enough that short test intervals report promptly,
    // never coarser than the production 500 ms cadence.
    const auto poll = std::min<std::chrono::milliseconds>(
        std::chrono::milliseconds(500),
        std::max<std::chrono::milliseconds>(std::chrono::milliseconds(10), report_interval / 4));
    std::thread watchdog([&wait_done, &reports, &phase, report_interval, poll]() {
        const auto started = std::chrono::steady_clock::now();
        auto next_report = started + report_interval;
        while (!wait_done.load(std::memory_order_acquire)) {
            std::this_thread::sleep_for(poll);
            const auto now = std::chrono::steady_clock::now();
            if (!wait_done.load(std::memory_order_acquire) && now >= next_report) {
                const auto waited_s =
                    std::chrono::duration_cast<std::chrono::seconds>(now - started).count();
                LUMINUMBRA_CORE_WARN(
                    "JOB_WATCHDOG: phase '{}' still waiting after {} s — possible wedge",
                    phase,
                    waited_s);
                reports.fetch_add(1, std::memory_order_relaxed);
                next_report = now + report_interval;
            }
        }
    });
    std::forward<WaitFn>(wait_fn)();
    wait_done.store(true, std::memory_order_release);
    watchdog.join();
    return reports.load(std::memory_order_relaxed);
}

} // namespace Luminumbra::Core
