#include "persistence/AsyncSavedWorldCatalog.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <future>
#include <mutex>
#include <stdexcept>
#include <thread>

using namespace std::chrono_literals;
using Luminumbra::Persistence::AsyncSavedWorldCatalog;
using Luminumbra::Persistence::SavedWorldCatalog;

namespace {
std::optional<SavedWorldCatalog> Await(AsyncSavedWorldCatalog& scan) {
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (std::chrono::steady_clock::now() < deadline) {
        if (auto result = scan.Poll())
            return result;
        std::this_thread::sleep_for(1ms);
    }
    return std::nullopt;
}
} // namespace

TEST(AsyncSavedWorldCatalogTest, CoalescesRefreshesAndDiscardsLateResults) {
    std::promise<void> started;
    std::promise<void> release;
    auto released = release.get_future().share();
    std::atomic<int> calls{0};
    AsyncSavedWorldCatalog scan;
    scan.Request([&](const std::stop_token&) {
        ++calls;
        started.set_value();
        // Deliberately ignore cancellation: an obsolete callback must still
        // never publish its result or make newer requests run concurrently.
        released.wait_for(5s);
        SavedWorldCatalog result;
        result.error = "obsolete";
        return result;
    });
    ASSERT_EQ(started.get_future().wait_for(5s), std::future_status::ready);
    for (int i = 0; i < 100; ++i) {
        scan.Request([&, i](const std::stop_token&) {
            ++calls;
            SavedWorldCatalog result;
            result.error = std::to_string(i);
            return result;
        });
    }
    EXPECT_EQ(calls.load(), 1);
    EXPECT_FALSE(scan.Poll());
    release.set_value();
    const auto result = Await(scan);
    ASSERT_TRUE(result);
    EXPECT_EQ(result->error, "99");
    EXPECT_EQ(calls.load(), 2);
    EXPECT_FALSE(scan.Poll());
}

TEST(AsyncSavedWorldCatalogTest, CancellationDrainsProviderBeforeDestruction) {
    std::promise<void> started;
    std::atomic<bool> cancelled{false};
    {
        AsyncSavedWorldCatalog scan;
        scan.Request([&](const std::stop_token& stop) {
            std::mutex mutex;
            std::condition_variable_any wake;
            std::unique_lock lock(mutex);
            started.set_value();
            wake.wait_for(lock, stop, 5s, [] { return false; });
            cancelled = stop.stop_requested();
            return SavedWorldCatalog{};
        });
        ASSERT_EQ(started.get_future().wait_for(5s), std::future_status::ready);
        scan.Cancel();
        EXPECT_FALSE(scan.Poll());
    }
    EXPECT_TRUE(cancelled.load());
}

TEST(AsyncSavedWorldCatalogTest, ProviderFailureIsReportedAndNextScanCanSucceed) {
    AsyncSavedWorldCatalog scan;
    scan.Request([](const std::stop_token&) -> SavedWorldCatalog {
        throw std::runtime_error("read failed");
    });
    auto result = Await(scan);
    ASSERT_TRUE(result);
    EXPECT_NE(result->error.find("read failed"), std::string::npos);
    scan.Request([](const std::stop_token&) { return SavedWorldCatalog{}; });
    result = Await(scan);
    ASSERT_TRUE(result);
    EXPECT_TRUE(result->error.empty());
    EXPECT_TRUE(result->worlds.empty());
}
