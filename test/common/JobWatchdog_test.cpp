// the opt-in named-phase wedge watchdog that wraps the three
// unbounded EnsureSurfaceReadyNear waits (generation-drain / meshing-drain /
// collision-build). The helper is observability-only: it must never alter the
// wait, must emit nothing when disabled or when the wait finishes inside the
// first interval, and must report (with the phase name) once a wait outlives
// the interval. The report count is returned exactly so this test can assert
// the reporter actually fires without scraping logs.
#include <gtest/gtest.h>

#include <chrono>
#include <thread>

#include "luminumbra_common/core/JobWatchdog.h"

using Luminumbra::Core::WaitWithJobWatchdog;
using namespace std::chrono_literals;

TEST(JobWatchdog, DisabledRunsTheWaitAndNeverReports) {
    bool wait_ran = false;
    const std::size_t reports = WaitWithJobWatchdog(
        /*enabled=*/
        false,
        "test/disabled-phase",
        [&wait_ran]() {
            wait_ran = true;
            std::this_thread::sleep_for(30ms);
        },
        /*report_interval=*/10ms);
    EXPECT_TRUE(wait_ran) << "disabled watchdog must still run the wait";
    EXPECT_EQ(reports, 0u);
}

TEST(JobWatchdog, WedgedWaitEmitsNamedPhaseReports) {
    bool wait_ran = false;
    const std::size_t reports = WaitWithJobWatchdog(
        /*enabled=*/
        true,
        "test/wedged-phase",
        [&wait_ran]() {
            wait_ran = true;
            // Outlive several report intervals: the reporter must fire while
            // the "wedge" persists, then stop cleanly when the wait returns.
            std::this_thread::sleep_for(400ms);
        },
        /*report_interval=*/100ms);
    EXPECT_TRUE(wait_ran);
    EXPECT_GE(reports, 1u)
        << "a wait outliving the report interval must emit at least one named-phase report";
}

TEST(JobWatchdog, FastWaitEmitsNoReports) {
    const std::size_t reports = WaitWithJobWatchdog(
        /*enabled=*/
        true,
        "test/fast-phase",
        []() { std::this_thread::sleep_for(5ms); },
        /*report_interval=*/60s);
    EXPECT_EQ(reports, 0u) << "a wait finishing inside the first interval must stay silent";
}
