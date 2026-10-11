#include "core/BoundedConsoleSink.h"

#include <gtest/gtest.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <atomic>
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
#include <cerrno>
#include <csignal>
#include <unistd.h>
#endif

using namespace std::chrono_literals;

namespace {

// Output whose write() blocks while stalled, to model a reader that stops
// draining the console. An optional hook runs after each completed write, on
// the writer thread, outside the fake's lock.
class FakeOutput final : public ConsoleOutput {
public:
    Status write(const char* data, std::size_t length) override {
        std::function<void(const std::string&)> on_write;
        {
            std::unique_lock<std::mutex> lock(m_mutex);
            ++m_calls;
            m_cv.notify_all();
            m_cv.wait(lock, [this] { return !m_stalled; });
            if (m_dead) {
                return Status::Dead;
            }
            m_written.append(data, length);
            on_write = m_on_write;
        }
        if (on_write) {
            on_write(std::string(data, length));
        }
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

    void SetOnWrite(std::function<void(const std::string&)> hook) {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_on_write = std::move(hook);
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
    std::function<void(const std::string&)> m_on_write;
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
    // The EOL is explicit so that expectations hold on Windows, where the default
    // EOL is "\r\n".
    sink->set_formatter(
        std::make_unique<spdlog::pattern_formatter>("%v", spdlog::pattern_time_type::local, "\n"));
    auto logger = std::make_shared<spdlog::logger>("bounded_console_sink_test", std::move(sink));
    logger->set_level(spdlog::level::trace);
    return logger;
}

// Runs a callable on its own thread so that a blocked call fails the test
// instead of hanging it. Completion is observed with a bounded wait.
class Worker {
public:
    explicit Worker(std::function<void()> body)
        : m_result(m_done.get_future())
        , m_thread([this, run = std::move(body)]() mutable {
            run();
            m_done.set_value();
        }) {}

    ~Worker() {
        if (m_thread.joinable()) {
            m_thread.join();
        }
    }

    Worker(const Worker&) = delete;
    Worker& operator=(const Worker&) = delete;

    bool FinishedWithin(std::chrono::milliseconds timeout) {
        return m_result.wait_for(timeout) == std::future_status::ready;
    }

private:
    std::promise<void> m_done;
    std::future<void> m_result;
    std::thread m_thread;
};

// Owns the workers of one stalled scenario. Declare it after the sink and the
// logger, so it is destroyed first. Its destructor releases the output before
// joining any worker, so an early assertion exit cannot leave a worker blocked
// on the stalled fake.
class StallScope {
public:
    explicit StallScope(std::shared_ptr<FakeOutput> output)
        : m_output(std::move(output)) {}

    ~StallScope() {
        m_output->Release();
        m_workers.clear();
    }

    StallScope(const StallScope&) = delete;
    StallScope& operator=(const StallScope&) = delete;

    Worker& Run(std::function<void()> fn) {
        m_workers.push_back(std::make_unique<Worker>(std::move(fn)));
        return *m_workers.back();
    }

private:
    std::shared_ptr<FakeOutput> m_output;
    std::vector<std::unique_ptr<Worker>> m_workers;
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

#ifndef _WIN32

// Closes its descriptor on destruction unless released or reset.
class ScopedFd {
public:
    explicit ScopedFd(int fd = -1)
        : m_fd(fd) {}
    ~ScopedFd() {
        Reset();
    }

    ScopedFd(const ScopedFd&) = delete;
    ScopedFd& operator=(const ScopedFd&) = delete;

    int get() const {
        return m_fd;
    }

    int release() {
        const int fd = m_fd;
        m_fd = -1;
        return fd;
    }

    void Reset(int fd = -1) {
        if (m_fd >= 0) {
            ::close(m_fd);
        }
        m_fd = fd;
    }

private:
    int m_fd;
};

// Ignores SIGPIPE for its lifetime and restores the previous disposition.
class SigpipeIgnoreScope {
public:
    SigpipeIgnoreScope()
        : m_previous(std::signal(SIGPIPE, SIG_IGN)) {}
    ~SigpipeIgnoreScope() {
        std::signal(SIGPIPE, m_previous);
    }

    SigpipeIgnoreScope(const SigpipeIgnoreScope&) = delete;
    SigpipeIgnoreScope& operator=(const SigpipeIgnoreScope&) = delete;

private:
    void (*m_previous)(int);
};

// A pipe drained by a reader thread. The duplicated writer is created before
// the reader starts, and the reader owns and closes the read end. Finish()
// closes the writer and joins the reader. The destructor calls Finish(), so
// every exit path joins the thread.
class PipeHarness {
public:
    bool Open() {
        int fds[2] = {-1, -1};
        if (pipe(fds) != 0) {
            return false;
        }
        m_read.Reset(fds[0]);
        m_write.Reset(fds[1]);
        return true;
    }

    bool StartReader() {
        output = BoundedConsoleSink::MakeFileDescriptorOutput(m_write.get());
        if (!output) {
            return false;
        }
        // The duplicate is now the only write end, so closing the output sends EOF.
        m_write.Reset();
        const int read_fd = m_read.release();
        m_reader = std::thread([this, read_fd] { Drain(read_fd); });
        return true;
    }

    // Closes the writer, waits for the reader to reach EOF, and returns what it read.
    std::string Finish() {
        output.reset();
        if (m_reader.joinable()) {
            m_reader.join();
        }
        return received;
    }

    ~PipeHarness() {
        Finish();
    }

    PipeHarness(const PipeHarness&) = delete;
    PipeHarness& operator=(const PipeHarness&) = delete;

    std::shared_ptr<ConsoleOutput> output;

private:
    void Drain(int fd) {
        char buffer[4096];
        for (;;) {
            const ssize_t count = ::read(fd, buffer, sizeof buffer);
            if (count < 0 && errno == EINTR) {
                continue;
            }
            if (count <= 0) {
                break;
            }
            received.append(buffer, static_cast<std::size_t>(count));
        }
        ::close(fd);
    }

    ScopedFd m_read;
    ScopedFd m_write;
    std::thread m_reader;
    std::string received;
};

// Scripted replacement for ::write. Each step either fails with an errno or
// accepts at most `result` bytes. Once the script is exhausted every call
// accepts the whole request.
struct WriteStep {
    ssize_t result;
    int error;
};

struct ScriptedWriter {
    std::vector<WriteStep> steps;
    std::size_t next = 0;
    std::size_t calls = 0;
    std::string delivered;
};

ScriptedWriter g_scripted;

ssize_t ScriptedWrite(int /*fd*/, const void* data, std::size_t length) {
    ++g_scripted.calls;
    if (g_scripted.next < g_scripted.steps.size()) {
        const WriteStep step = g_scripted.steps[g_scripted.next++];
        if (step.result < 0) {
            errno = step.error;
            return -1;
        }
        length = std::min(length, static_cast<std::size_t>(step.result));
    }
    g_scripted.delivered.append(static_cast<const char*>(data), length);
    return static_cast<ssize_t>(length);
}

#endif

class BoundedConsoleSinkTest : public ::testing::Test {
protected:
    // Safety net: every fake output is released before the fixture is torn down.
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
    StallScope scope(out);

    out->Stall();
    Worker& worker = scope.Run([logger] { logger->info("blocked line"); });

    EXPECT_TRUE(out->WaitForCalls(1, 5000ms));
    EXPECT_FALSE(worker.FinishedWithin(1000ms));

    out->Release();
    ASSERT_TRUE(worker.FinishedWithin(5000ms));
    EXPECT_EQ(out->Written(), "blocked line\n");
}

TEST_F(BoundedConsoleSinkTest, BoundedSinkReturnsWhileStalledAndReportsDrops) {
    auto out = NewOutput();
    BoundedConsoleSink::Limits limits;
    limits.max_messages = 4;
    auto sink = std::make_shared<BoundedConsoleSink>(out, limits);
    auto logger = MakeLogger(sink);
    StallScope scope(out);

    out->Stall();
    Worker& first = scope.Run([logger] { logger->info("line 1"); });
    ASSERT_TRUE(first.FinishedWithin(1000ms));
    // Line 1 is now in flight inside the stalled write. Lines 2-5 fill the queue,
    // and lines 6-50 are dropped.
    ASSERT_TRUE(out->WaitForCalls(1, 5000ms));

    Worker& rest = scope.Run([logger] {
        for (int i = 2; i <= 50; ++i) {
            logger->info("line {}", i);
        }
    });
    ASSERT_TRUE(rest.FinishedWithin(1000ms));
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
    StallScope scope(out);

    out->Stall();
    Worker& first = scope.Run([logger] { logger->info("first"); });
    ASSERT_TRUE(first.FinishedWithin(1000ms));
    ASSERT_TRUE(out->WaitForCalls(1, 5000ms));
    Worker& second = scope.Run([logger] { logger->info("second"); });
    ASSERT_TRUE(second.FinishedWithin(1000ms));

    // Drop the last references on a worker so the destructor's bounded wait is
    // measured without blocking the test.
    Worker& destroy = scope.Run([s = std::move(sink), l = std::move(logger)]() mutable {
        l.reset();
        s.reset();
    });
    EXPECT_TRUE(destroy.FinishedWithin(1000ms));

    // The writer outlives the sink. Once the fake is released, the in-flight and
    // queued lines are still delivered without a crash.
    out->Release();
    EXPECT_TRUE(WaitUntil([&out] { return out->Written() == "first\nsecond\n"; }, 5000ms));
}

TEST_F(BoundedConsoleSinkTest, DropNoticeDoesNotStarveQueuedMessages) {
    auto out = NewOutput();
    BoundedConsoleSink::Limits limits;
    limits.max_messages = 1;
    auto sink = std::make_shared<BoundedConsoleSink>(out, limits);
    auto logger = MakeLogger(sink);

    // The first notice is written while line 4 overflows the queue again. The hook
    // runs on the writer thread, and the sink never blocks on it.
    std::atomic<bool> overflowed{false};
    spdlog::logger* raw_logger = logger.get();
    out->SetOnWrite([raw_logger, &overflowed](const std::string& chunk) {
        if (!chunk.empty() && chunk[0] == '[' && !overflowed.exchange(true)) {
            raw_logger->info("line 4");
        }
    });
    StallScope scope(out);

    out->Stall();
    Worker& first = scope.Run([logger] { logger->info("line 1"); });
    ASSERT_TRUE(first.FinishedWithin(1000ms));
    ASSERT_TRUE(out->WaitForCalls(1, 5000ms));
    // Line 2 fills the queue and line 3 is dropped while line 1 is stalled.
    Worker& queued = scope.Run([logger] {
        logger->info("line 2");
        logger->info("line 3");
    });
    ASSERT_TRUE(queued.FinishedWithin(1000ms));
    EXPECT_EQ(sink->dropped_count(), 1u);

    out->Release();
    // Each notice is followed by a queued line before another notice. Line 4 is
    // dropped during the first notice, so a second notice follows line 2.
    const std::string expected =
        "line 1\n[1 console lines dropped]\nline 2\n[1 console lines dropped]\n";
    EXPECT_TRUE(WaitUntil([&out, &expected] { return out->Written() == expected; }, 5000ms));
    EXPECT_EQ(sink->dropped_count(), 2u);
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

    EXPECT_EQ(sink->dropped_count(), 11u);
    EXPECT_EQ(out->Written(), "");
}

TEST_F(BoundedConsoleSinkTest, NullOutputCountsEveryMessageAsDroppedWithoutBlocking) {
    auto sink = std::make_shared<BoundedConsoleSink>(nullptr, BoundedConsoleSink::Limits{});
    auto logger = MakeLogger(sink);

    Worker worker([logger] {
        for (int i = 0; i < 3; ++i) {
            logger->info("line {}", i);
        }
    });
    ASSERT_TRUE(worker.FinishedWithin(1000ms));
    EXPECT_EQ(sink->dropped_count(), 3u);
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

#ifndef _WIN32
TEST_F(BoundedConsoleSinkTest, FileDescriptorOutputScriptedShortWritesAndEintrDeliverInOrder) {
    g_scripted = ScriptedWriter{};
    g_scripted.steps = {{1, 0}, {-1, EINTR}, {1, 0}, {2, 0}};

    int fds[2] = {-1, -1};
    ASSERT_EQ(pipe(fds), 0);
    ScopedFd read_end(fds[0]);
    ScopedFd write_end(fds[1]);

    auto output = BoundedConsoleSink::MakeFileDescriptorOutput(write_end.get(), &ScriptedWrite);
    ASSERT_NE(output, nullptr);
    const std::string payload = "abcdefghijk\n";
    EXPECT_EQ(output->write(payload.data(), payload.size()), ConsoleOutput::Status::Ok);

    // Four scripted calls, then one call that accepts the remaining 8 bytes.
    EXPECT_EQ(g_scripted.delivered, payload);
    EXPECT_EQ(g_scripted.next, g_scripted.steps.size());
    EXPECT_EQ(g_scripted.calls, 5u);
}

TEST_F(BoundedConsoleSinkTest, FileDescriptorOutputScriptedEpipeReportsDead) {
    g_scripted = ScriptedWriter{};
    g_scripted.steps = {{-1, EPIPE}};

    int fds[2] = {-1, -1};
    ASSERT_EQ(pipe(fds), 0);
    ScopedFd read_end(fds[0]);
    ScopedFd write_end(fds[1]);

    auto output = BoundedConsoleSink::MakeFileDescriptorOutput(write_end.get(), &ScriptedWrite);
    ASSERT_NE(output, nullptr);
    const char text[] = "x\n";
    EXPECT_EQ(output->write(text, 2), ConsoleOutput::Status::Dead);
    EXPECT_EQ(g_scripted.calls, 1u);
    EXPECT_TRUE(g_scripted.delivered.empty());
}

TEST_F(BoundedConsoleSinkTest, FileDescriptorOutputReportsDeadAfterReaderCloses) {
    int fds[2] = {-1, -1};
    ASSERT_EQ(pipe(fds), 0);
    ScopedFd write_end(fds[1]);
    ScopedFd read_end(fds[0]);
    read_end.Reset();

    // Without this, the write below would terminate the test process. The
    // scope restores the previous disposition on every exit path.
    SigpipeIgnoreScope sigpipe;
    auto output = BoundedConsoleSink::MakeFileDescriptorOutput(write_end.get());
    ASSERT_NE(output, nullptr);
    const char text[] = "x\n";
    EXPECT_EQ(output->write(text, 2), ConsoleOutput::Status::Dead);
}

TEST_F(BoundedConsoleSinkTest, FileDescriptorOutputDeliversLargeWriteThroughPipe) {
    PipeHarness pipe_harness;
    ASSERT_TRUE(pipe_harness.Open());
    ASSERT_TRUE(pipe_harness.StartReader());

    // Far larger than the pipe capacity, so the write has to loop until the
    // reader has drained enough.
    const std::string payload(1u << 20, 'p');
    EXPECT_EQ(pipe_harness.output->write(payload.data(), payload.size()),
              ConsoleOutput::Status::Ok);

    const std::string received = pipe_harness.Finish();
    EXPECT_EQ(received.size(), payload.size());
    EXPECT_TRUE(received == payload);
}
#endif
