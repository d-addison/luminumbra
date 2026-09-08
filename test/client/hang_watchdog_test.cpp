// HangWatchdog: fires exactly once per stall episode, never while the heartbeat
// advances, and re-arms after the heartbeat moves again. Pure std, no GL/device.
#include <gtest/gtest.h>

#include "app/HangWatchdog.h"

#include <atomic>
#include <chrono>
#include <thread>

using Luminumbra::Client::App::HangWatchdog;
using namespace std::chrono_literals;

namespace {

struct Reports {
    std::atomic<int> count{0};
    std::atomic<std::uint64_t> last_beat{0};
    std::atomic<double> last_age{0.0};
};

} // namespace

TEST(HangWatchdog, StalledHeartbeatFiresOnce) {
    std::atomic<std::uint64_t> beat{7};
    Reports reports;
    HangWatchdog watchdog([&] { return beat.load(); },
                          200ms,
                          [&](std::uint64_t last, double age) {
                              reports.count.fetch_add(1);
                              reports.last_beat.store(last);
                              reports.last_age.store(age);
                          },
                          20ms);
    std::this_thread::sleep_for(900ms); // several stall windows without a heartbeat
    watchdog.stop();
    EXPECT_EQ(reports.count.load(), 1);
    EXPECT_EQ(reports.last_beat.load(), 7u);
    EXPECT_GE(reports.last_age.load(), 0.2);
    EXPECT_EQ(watchdog.stall_reports(), 1u);
}

TEST(HangWatchdog, AdvancingHeartbeatNeverFires) {
    std::atomic<std::uint64_t> beat{0};
    Reports reports;
    HangWatchdog watchdog([&] { return beat.load(); },
                          200ms,
                          [&](std::uint64_t, double) { reports.count.fetch_add(1); },
                          20ms);
    for (int i = 0; i < 20; ++i) {
        std::this_thread::sleep_for(40ms);
        beat.fetch_add(1);
    }
    watchdog.stop();
    EXPECT_EQ(reports.count.load(), 0);
}

TEST(HangWatchdog, RearmsAfterHeartbeatResumes) {
    std::atomic<std::uint64_t> beat{0};
    Reports reports;
    HangWatchdog watchdog([&] { return beat.load(); },
                          150ms,
                          [&](std::uint64_t, double) { reports.count.fetch_add(1); },
                          20ms);
    std::this_thread::sleep_for(500ms); // first stall
    beat.fetch_add(1);                  // recovered
    std::this_thread::sleep_for(100ms);
    EXPECT_EQ(reports.count.load(), 1);
    std::this_thread::sleep_for(500ms); // second stall
    watchdog.stop();
    EXPECT_EQ(reports.count.load(), 2);
}

TEST(HangWatchdog, StopIsIdempotent) {
    std::atomic<std::uint64_t> beat{0};
    HangWatchdog watchdog([&] { return beat.load(); }, 1s, [](std::uint64_t, double) {}, 20ms);
    watchdog.stop();
    watchdog.stop();
    EXPECT_EQ(watchdog.stall_reports(), 0u);
}
