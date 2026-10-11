#include "core/BoundedConsoleSink.h"

#include <gtest/gtest.h>
#include <spdlog/spdlog.h>

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#ifndef _WIN32
#include <csignal>
#include <unistd.h>
#endif

using namespace std::chrono_literals;

namespace {

// Output whose write() blocks while stalled, to model a reader that stops
// draining the console.
class FakeOutput final : public ConsoleOutput {
public:
    Status write(const char* data, std::size_t length) override {
        std::unique_lock<std::mutex> lock(m_mutex);
        ++m_calls;
        m_cv.notify_all();
        m_cv.wait(lock, [this] { return !m_stalled; });
        if (m_dead) {
            return Status::Dead;
        }
        m_written.append(data, length);
        return Status::Ok;
    }

    void Stall() {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_stalled = true;
    }

    void Release() {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_stalled = false;
        }
        m_cv.notify_all();
    }

    void SetDead() {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_dead = true;
    }

    bool WaitForCalls(std::size_t count, std::chrono::milliseconds timeout) {
        std::unique_lock<std::mutex> lock(m_mutex);
        return m_cv.wait_for(lock, timeout, [this, count] { return m_calls >= count; });
    }

    std::string Written() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_written;
    }

private:
    mutable std::mutex m_mutex;
    std::condition_variable m_cv;
    bool m_stalled = false;
    bool m_dead = false;
    std::size_t m_calls = 0;
    std::string m_written;
};

// Synchronous adapter: formats and writes on the logging thread, the way a
// plain console sink does. Used as the control for the stall case.
class SynchronousSink final : public spdlog::sinks::base_sink<std::mutex> {
public:
    explicit SynchronousSink(std::shared_ptr<ConsoleOutput> output)
        : m_output(std::move(output)) {}

protected:
    void sink_it_(const spdlog::details::log_msg& msg) override {
        spdlog::memory_buf_t formatted;
        formatter_->format(msg, formatted);
        m_output->write(formatted.data(), formatted.size());
    }

    void flush_() override {}

private:
    std::shared_ptr<ConsoleOutput> m_output;
};

std::shared_ptr<spdlog::logger> MakeLogger(std::shared_ptr<spdlog::sinks::sink> sink) {
    auto logger = std::make_shared<spdlog::logger>("bounded_console_sink_test", std::move(sink));
    logger->set_pattern("%v");
    logger->set_level(spdlog::level::trace);
    return logger;
}

// Runs a callable on its own thread so that a blocked call fails the test
// instead of hanging it.
class Worker {
public:
    template<typename Fn>
    explicit Worker(Fn fn)
        : m_result(m_done.get_future())
        , m_thread([this, fn = std::move(fn)]() mutable {
            fn();
            m_done.set_value();
        }) {}

    ~Worker() {
        Join();
    }

    Worker(const Worker&) = delete;
    Worker& operator=(const Worker&) = delete;

    bool FinishedWithin(std::chrono::milliseconds timeout) {
        return m_result.wait_for(timeout) == std::future_status::ready;
    }

    void Join() {
        if (m_thread.joinable()) {
            m_thread.join();
        }
    }

private:
    std::promise<void> m_done;
    std::future<void> m_result;
    std::thread m_thread;
};

bool WaitUntil(const std::function<bool()>& predicate, std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (predicate()) {
            return true;
        }
        std::this_thread::sleep_for(5ms);
    }
    return predicate();
}

class BoundedConsoleSinkTest : public ::testing::Test {
protected:
    // Every fake output is released before the test's locals are torn down,
    // so no writer thread is left blocked on a stalled fake.
    void TearDown() override {
        for (auto& output : m_outputs) {
            output->Release();
        }
    }

    std::shared_ptr<FakeOutput> NewOutput() {
        auto output = std::make_shared<FakeOutput>();
        m_outputs.push_back(output);
        return output;
    }

private:
    std::vector<std::shared_ptr<FakeOutput>> m_outputs;
};

} // namespace

TEST_F(BoundedConsoleSinkTest, SynchronousAdapterBlocksLoggingThreadWhileStalled) {
    auto out = NewOutput();
    auto logger = MakeLogger(std::make_shared<SynchronousSink>(out));

    out->Stall();
    Worker worker([logger] { logger->info("blocked line"); });

    EXPECT_TRUE(out->WaitForCalls(1, 5000ms));
    EXPECT_FALSE(worker.FinishedWithin(1000ms));

    out->Release();
    EXPECT_TRUE(worker.FinishedWithin(5000ms));
    worker.Join();
    EXPECT_EQ(out->Written(), "blocked line\n");
}

TEST_F(BoundedConsoleSinkTest, BoundedSinkReturnsWhileStalledAndReportsDrops) {
    auto out = NewOutput();
    BoundedConsoleSink::Limits limits;
    limits.max_messages = 4;
    auto sink = std::make_shared<BoundedConsoleSink>(out, limits);
    auto logger = MakeLogger(sink);

    out->Stall();
    logger->info("line 1");
    // Line 1 is now in flight inside the stalled write. Lines 2-5 fill the queue,
    // and lines 6-50 are dropped.
    ASSERT_TRUE(out->WaitForCalls(1, 5000ms));

    Worker worker([logger] {
        for (int i = 2; i <= 50; ++i) {
            logger->info("line {}", i);
        }
    });
    EXPECT_TRUE(worker.FinishedWithin(1000ms));
    worker.Join();
    EXPECT_EQ(sink->dropped_count(), 45u);

    out->Release();
    EXPECT_TRUE(WaitUntil(
        [&out] {
            const std::string written = out->Written();
            return written.find("line 5\n") != std::string::npos &&
                   written.find("[45 console lines dropped]\n") != std::string::npos;
        },
        5000ms));

    const std::string written = out->Written();
    EXPECT_NE(written.find("line 1\n"), std::string::npos);
    EXPECT_NE(written.find("line 2\n"), std::string::npos);
    EXPECT_NE(written.find("line 5\n"), std::string::npos);
    EXPECT_NE(written.find("[45 console lines dropped]\n"), std::string::npos);
    EXPECT_EQ(written.find("line 6\n"), std::string::npos);
    EXPECT_EQ(written.find("line 50\n"), std::string::npos);
}

TEST_F(BoundedConsoleSinkTest, DestroyingSinkWhileWriterBlockedReturnsPromptly) {
    auto out = NewOutput();
    auto sink = std::make_shared<BoundedConsoleSink>(out, BoundedConsoleSink::Limits{});
    auto logger = MakeLogger(sink);

    out->Stall();
    logger->info("first");
    ASSERT_TRUE(out->WaitForCalls(1, 5000ms));
    logger->info("second");

    // Drop the last references on a separate thread so the destructor's bounded
    // wait is measured without blocking the test.
    Worker destroy([s = std::move(sink), l = std::move(logger)]() mutable {
        l.reset();
        s.reset();
    });
    EXPECT_TRUE(destroy.FinishedWithin(1000ms));
    destroy.Join();

    // The writer outlives the sink. Once the fake is released, the in-flight and
    // queued lines are still delivered without a crash.
    out->Release();
    EXPECT_TRUE(WaitUntil([&out] { return out->Written() == "first\nsecond\n"; }, 5000ms));
}

TEST_F(BoundedConsoleSinkTest, OversizedMessageIsTruncatedWithMarker) {
    auto out = NewOutput();
    BoundedConsoleSink::Limits limits;
    limits.max_message_bytes = 64;
    auto sink = std::make_shared<BoundedConsoleSink>(out, limits);
    auto logger = MakeLogger(sink);

    const std::string big(1000, 'x');
    logger->info("{}", big);

    EXPECT_TRUE(WaitUntil([&out] { return !out->Written().empty(); }, 5000ms));
    const std::string expected = std::string(49, 'x') + "...[truncated]\n";
    EXPECT_EQ(out->Written(), expected);
    EXPECT_EQ(expected.size(), 64u);
    EXPECT_EQ(sink->dropped_count(), 0u);
}

TEST_F(BoundedConsoleSinkTest, DeadOutputCountsLaterMessagesAsDroppedWithoutBlocking) {
    auto out = NewOutput();
    out->SetDead();
    auto sink = std::make_shared<BoundedConsoleSink>(out, BoundedConsoleSink::Limits{});
    auto logger = MakeLogger(sink);

    logger->info("first");
    // The first write reports Dead, so that message is counted as dropped.
    ASSERT_TRUE(WaitUntil([&sink] { return sink->dropped_count() == 1; }, 5000ms));

    Worker worker([logger] {
        for (int i = 0; i < 10; ++i) {
            logger->info("later {}", i);
        }
    });
    EXPECT_TRUE(worker.FinishedWithin(1000ms));
    worker.Join();

    EXPECT_EQ(sink->dropped_count(), 11u);
    EXPECT_EQ(out->Written(), "");
}

#ifndef _WIN32
TEST_F(BoundedConsoleSinkTest, FileDescriptorOutputReportsDeadAfterReaderCloses) {
    int fds[2] = {-1, -1};
    ASSERT_EQ(pipe(fds), 0);
    ::close(fds[0]);

    // Without this, the write below would terminate the test process.
    auto previous = std::signal(SIGPIPE, SIG_IGN);
    {
        auto output = BoundedConsoleSink::MakeFileDescriptorOutput(fds[1]);
        ASSERT_NE(output, nullptr);
        const char text[] = "x\n";
        EXPECT_EQ(output->write(text, 2), ConsoleOutput::Status::Dead);
    }
    ::close(fds[1]);
    std::signal(SIGPIPE, previous);
}

TEST_F(BoundedConsoleSinkTest, FileDescriptorOutputDeliversLargeWriteThroughPipe) {
    int fds[2] = {-1, -1};
    ASSERT_EQ(pipe(fds), 0);

    // Far larger than the pipe capacity, so the write has to loop until the
    // reader has drained enough.
    const std::string payload(1u << 20, 'p');
    std::string received;
    std::thread reader([&received, read_fd = fds[0]] {
        char buffer[4096];
        ssize_t count = 0;
        while ((count = ::read(read_fd, buffer, sizeof buffer)) > 0) {
            received.append(buffer, static_cast<std::size_t>(count));
        }
    });

    {
        auto output = BoundedConsoleSink::MakeFileDescriptorOutput(fds[1]);
        ASSERT_NE(output, nullptr);
        EXPECT_EQ(output->write(payload.data(), payload.size()), ConsoleOutput::Status::Ok);
    }
    ::close(fds[1]);
    reader.join();
    ::close(fds[0]);

    EXPECT_EQ(received.size(), payload.size());
    EXPECT_TRUE(received == payload);
}
#endif
