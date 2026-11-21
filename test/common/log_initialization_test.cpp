#include "core/Log.h"
#include "core/LogDispatcherSink.h"

#include <gtest/gtest.h>
#include <spdlog/sinks/null_sink.h>

#include <array>
#include <barrier>
#include <cstddef>
#include <exception>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <system_error>
#include <thread>
#include <type_traits>

namespace {

// This ownership regression fails at compile time for the mutable accessor.
static_assert(
    std::is_same_v<decltype(Log::GetCoreLogger()), const std::shared_ptr<spdlog::logger>&>);

void CheckConcurrentFirstUse(bool initialize, std::size_t refuse_worker = 16) {
    // Fixed bounds: 16 workers, one retained owner and one exception slot each.
    constexpr std::size_t worker_count = 16;
    std::array<std::shared_ptr<spdlog::logger>, worker_count> owners{};
    std::array<std::exception_ptr, worker_count> failures{};
    std::barrier start(static_cast<std::ptrdiff_t>(worker_count + 1));
    std::array<std::jthread, worker_count> workers{};
    std::size_t launched = 0;
    try {
        for (; launched < worker_count; ++launched) {
            if (launched == refuse_worker)
                throw std::system_error(
                    std::make_error_code(std::errc::resource_unavailable_try_again));
            const auto index = launched;
            workers[index] = std::jthread([&, index] {
                start.arrive_and_wait();
                try {
                    if (initialize && index % 2 == 0)
                        Log::Init();
                    owners[index] = Log::GetCoreLogger();
                } catch (...) {
                    failures[index] = std::current_exception();
                }
            });
        }
    } catch (...) {
        // Release launched workers even if the host refuses another thread.
        for (auto missing = launched; missing < worker_count; ++missing)
            start.arrive_and_drop();
        start.arrive_and_wait();
        throw;
    }
    start.arrive_and_wait();
    for (auto& worker : workers)
        worker.join();

    ASSERT_NE(owners.front(), nullptr);
    for (std::size_t index = 0; index < worker_count; ++index) {
        EXPECT_EQ(failures[index], nullptr);
        EXPECT_EQ(owners[index], owners.front());
    }
    EXPECT_EQ(owners.front(), spdlog::get("LUMINUMBRA"));
    EXPECT_EQ(owners.front()->name(), "LUMINUMBRA");
    Log::Init();
    EXPECT_EQ(Log::GetCoreLogger(), owners.front());
}

// CTest discovers each case as a separate process, preserving true first use.
TEST(LogInitialization, ConcurrentLazyFirstUseHasOneOwner) {
    ASSERT_EQ(spdlog::get("LUMINUMBRA"), nullptr);
    CheckConcurrentFirstUse(false);
    const auto& logger = Log::GetCoreLogger();
    EXPECT_EQ(logger->level(), spdlog::level::trace);
    ASSERT_FALSE(logger->sinks().empty());
    EXPECT_EQ(logger->sinks().back(), Log::GetDispatcherSink());
}

TEST(LogInitialization, ConcurrentInitAndAccessHaveOneOwner) {
    ASSERT_EQ(spdlog::get("LUMINUMBRA"), nullptr);
    CheckConcurrentFirstUse(true);
}

TEST(LogInitialization, ExplicitInitAndRepeatedAccessHaveOneOwner) {
    Log::Init();
    const auto owner = Log::GetCoreLogger();
    ASSERT_NE(owner, nullptr);
    Log::Init();
    EXPECT_EQ(Log::GetCoreLogger(), owner);
    EXPECT_EQ(spdlog::get("LUMINUMBRA"), owner);
}

TEST(LogInitialization, ReusesPreviouslyRegisteredLogger) {
    auto sink = std::make_shared<spdlog::sinks::null_sink_mt>();
    auto existing = std::make_shared<spdlog::logger>("LUMINUMBRA", sink);
    existing->set_level(spdlog::level::warn);
    spdlog::register_logger(existing);
    CheckConcurrentFirstUse(true);
    EXPECT_EQ(Log::GetCoreLogger(), existing);
    EXPECT_EQ(existing->level(), spdlog::level::warn);
    ASSERT_EQ(existing->sinks().size(), 1u);
    EXPECT_EQ(existing->sinks().front(), sink);
}

TEST(LogInitialization, WorkerLaunchRefusalReleasesStartedWorkers) {
    for (const std::size_t refuse_worker : {0u, 8u}) {
        EXPECT_THROW(CheckConcurrentFirstUse(false, refuse_worker), std::system_error);
    }
}

TEST(LogHeaderContract, DoesNotIncludeConcreteSink) {
    std::ifstream header(std::filesystem::path(LUMINUMBRA_SOURCE_ROOT) /
                         "src/luminumbra_common/core/Log.h");
    ASSERT_TRUE(header.is_open());
    // Bound the source-policy scan to 64 KiB, including unusually long lines.
    std::array<char, 64 * 1024> bytes{};
    header.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    ASSERT_TRUE(header.eof());
    ASSERT_FALSE(header.bad());
    const std::string source(bytes.data(), static_cast<std::size_t>(header.gcount()));
    EXPECT_EQ(source.find("spdlog/sinks/"), std::string::npos);
}

} // namespace
