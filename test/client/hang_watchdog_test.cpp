// HangWatchdog: fires exactly once per stall episode, never while the heartbeat
// advances or before the loop has started, re-arms after the heartbeat moves
// again, and contains exceptions from the stall handler. The logic tests drive
// poll() with an injected clock (no threads, no real time); one small
// real-thread test checks that the polling thread runs and stop() joins.
#include <gtest/gtest.h>

#include "app/HangWatchdog.h"

#include <atomic>
#include <chrono>
#include <stdexcept>
#include <thread>

using Luminumbra::Client::App::HangWatchdog;
using namespace std::chrono_literals;

namespace {

struct Fixture {
    std::atomic<std::uint64_t> beat{0};
    HangWatchdog::Clock::time_point now = HangWatchdog::Clock::time_point{} + 1000s;
    int reports = 0;
    std::uint64_t last_beat = 0;
    double last_age = 0.0;
    bool throw_in_handler = false;

    HangWatchdog make(std::chrono::milliseconds stall_after) {
        return HangWatchdog([this] { return beat.load(); },
                            stall_after,
                            [this](std::uint64_t last, double age) {
                                ++reports;
                                last_beat = last;
                                last_age = age;
                                if (throw_in_handler)
                                    throw std::runtime_error("diagnostic failure");
                            },
                            10ms,
                            /*start_thread=*/false,
                            [this] { return now; });
    }
};

} // namespace

TEST(HangWatchdog, NoReportBeforeTheLoopStarts) {
    Fixture f;
    HangWatchdog w = f.make(1s);
    for (int i = 0; i < 100; ++i) {
        f.now += 1s;
        EXPECT_FALSE(w.poll());
    }
    EXPECT_EQ(f.reports, 0);
}

TEST(HangWatchdog, StalledHeartbeatFiresOnce) {
    Fixture f;
    HangWatchdog w = f.make(2s);
    f.beat = 7;
    EXPECT_FALSE(w.poll()); // baseline
    f.now += 1s;
    EXPECT_FALSE(w.poll()); // within budget
    f.now += 1500ms;
    EXPECT_TRUE(w.poll()); // 2.5 s stalled
    f.now += 10s;
    EXPECT_FALSE(w.poll()); // same episode, no second report
    EXPECT_EQ(f.reports, 1);
    EXPECT_EQ(f.last_beat, 7u);
    EXPECT_NEAR(f.last_age, 2.5, 1e-9);
    EXPECT_EQ(w.stall_reports(), 1u);
}

TEST(HangWatchdog, AdvancingHeartbeatNeverFires) {
    Fixture f;
    HangWatchdog w = f.make(2s);
    for (int i = 1; i <= 50; ++i) {
        f.beat = static_cast<std::uint64_t>(i);
        f.now += 1s;
        EXPECT_FALSE(w.poll());
    }
    EXPECT_EQ(f.reports, 0);
}

TEST(HangWatchdog, RearmsAfterHeartbeatResumes) {
    Fixture f;
    HangWatchdog w = f.make(2s);
    f.beat = 1;
    w.poll();
    f.now += 3s;
    EXPECT_TRUE(w.poll());
    f.beat = 2; // recovered
    w.poll();
    f.now += 3s;
    EXPECT_TRUE(w.poll());
    EXPECT_EQ(f.reports, 2);
}

TEST(HangWatchdog, HandlerExceptionsAreContained) {
    Fixture f;
    f.throw_in_handler = true;
    HangWatchdog w = f.make(1s);
    f.beat = 1;
    w.poll();
    f.now += 2s;
    EXPECT_NO_THROW(EXPECT_TRUE(w.poll()));
    EXPECT_EQ(f.reports, 1);
}

TEST(HangWatchdog, PollingThreadRunsAndStopJoins) {
    std::atomic<std::uint64_t> beat{1};
    std::atomic<int> reports{0};
    HangWatchdog w([&] { return beat.load(); },
                   100ms,
                   [&](std::uint64_t, double) { reports.fetch_add(1); },
                   10ms);
    // Generous wait: the stall must be observed on a loaded runner, and a single
    // report is all that is asserted.
    for (int i = 0; i < 300 && reports.load() == 0; ++i)
        std::this_thread::sleep_for(20ms);
    w.stop();
    w.stop();
    EXPECT_EQ(reports.load(), 1);
}
