#include "BoundedConsoleSink.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <string>
#include <string_view>
#include <thread>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <cerrno>
#include <csignal>
#include <sys/stat.h>
#include <unistd.h>
#endif

struct BoundedConsoleSink::State {
    std::shared_ptr<ConsoleOutput> output;
    Limits limits;

    std::mutex mu;
    std::condition_variable cv;      // wakes the writer
    std::condition_variable done_cv; // signals writer exit to the destructor

    // Guarded by mu.
    std::deque<std::string> queue;
    std::size_t queued_bytes = 0;
    std::uint64_t reported = 0;
    bool dead = false;
    bool stop = false;
    bool writer_done = false;

    // Incremented under mu; read without the lock by dropped_count().
    std::atomic<std::uint64_t> dropped{0};
};

namespace {

constexpr std::string_view kTruncationMarker = "...[truncated]\n";

#ifdef _WIN32

class HandleOutput final : public ConsoleOutput {
public:
    explicit HandleOutput(HANDLE handle)
        : m_handle(handle) {}
    ~HandleOutput() override {
        CloseHandle(m_handle);
    }

    HandleOutput(const HandleOutput&) = delete;
    HandleOutput& operator=(const HandleOutput&) = delete;

    Status write(const char* data, std::size_t length) override {
        while (length > 0) {
            const DWORD chunk = static_cast<DWORD>(std::min<std::size_t>(length, 1u << 30));
            DWORD written = 0;
            // ERROR_BROKEN_PIPE, ERROR_INVALID_HANDLE and any other failure are
            // treated as a dead destination.
            if (!WriteFile(m_handle, data, chunk, &written, nullptr) || written == 0) {
                return Status::Dead;
            }
            data += written;
            length -= static_cast<std::size_t>(written);
        }
        return Status::Ok;
    }

private:
    HANDLE m_handle;
};

#else

class FdOutput final : public ConsoleOutput {
public:
    FdOutput(int fd, FileDescriptorWriteFn write_fn)
        : m_fd(fd)
        , m_write(write_fn) {}
    ~FdOutput() override {
        ::close(m_fd);
    }

    FdOutput(const FdOutput&) = delete;
    FdOutput& operator=(const FdOutput&) = delete;

    Status write(const char* data, std::size_t length) override {
        while (length > 0) {
            const ssize_t written = m_write(m_fd, data, length);
            if (written < 0) {
                if (errno == EINTR) {
                    continue;
                }
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    // Non-blocking descriptor with a full buffer: wait and retry.
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                    continue;
                }
                // EPIPE, EBADF and any other hard error: the destination is gone.
                return Status::Dead;
            }
            if (written == 0) {
                return Status::Dead;
            }
            data += written;
            length -= static_cast<std::size_t>(written);
        }
        return Status::Ok;
    }

private:
    int m_fd;
    FileDescriptorWriteFn m_write;
};

#endif

} // namespace

BoundedConsoleSink::BoundedConsoleSink(std::shared_ptr<ConsoleOutput> output, Limits limits)
    : m_state(std::make_shared<State>()) {
    m_state->output = std::move(output);
    m_state->limits = limits;
    // A null output starts Dead: sink_it_ counts every message as dropped, and
    // the writer exits without ever calling into a null output.
    m_state->dead = !m_state->output;
    m_writer = std::thread(&BoundedConsoleSink::RunWriter, m_state);
}

BoundedConsoleSink::~BoundedConsoleSink() {
    std::unique_lock<std::mutex> lock(m_state->mu);
    m_state->stop = true;
    m_state->cv.notify_all();
    const bool finished = m_state->done_cv.wait_for(
        lock, std::chrono::milliseconds(250), [this] { return m_state->writer_done; });
    lock.unlock();

    if (!m_writer.joinable()) {
        return;
    }
    if (finished) {
        m_writer.join();
    } else {
        // The writer is blocked inside ConsoleOutput::write. It holds its own
        // reference to State, so it can finish safely after this sink is gone.
        m_writer.detach();
    }
}

std::uint64_t BoundedConsoleSink::dropped_count() const {
    return m_state->dropped.load(std::memory_order_relaxed);
}

void BoundedConsoleSink::sink_it_(const spdlog::details::log_msg& msg) {
    spdlog::memory_buf_t formatted;
    formatter_->format(msg, formatted);

    const std::size_t limit = m_state->limits.max_message_bytes;
    std::string line;
    if (formatted.size() > limit) {
        const std::size_t keep =
            limit > kTruncationMarker.size() ? limit - kTruncationMarker.size() : 0;
        line.assign(formatted.data(), keep);
        line.append(kTruncationMarker);
    } else {
        line.assign(formatted.data(), formatted.size());
    }

    std::lock_guard<std::mutex> lock(m_state->mu);
    if (m_state->dead || m_state->queue.size() >= m_state->limits.max_messages ||
        m_state->queued_bytes + line.size() > m_state->limits.max_queued_bytes) {
        m_state->dropped.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    m_state->queued_bytes += line.size();
    m_state->queue.push_back(std::move(line));
    m_state->cv.notify_one();
}

void BoundedConsoleSink::flush_() {
    // Nothing is buffered outside the queue, and the writer owns delivery, so
    // there is no I/O to perform. Taking the lock only orders this call with
    // queue updates.
    std::lock_guard<std::mutex> lock(m_state->mu);
}

void BoundedConsoleSink::RunWriter(const std::shared_ptr<State>& state) {
    std::unique_lock<std::mutex> lock(state->mu);

    // Marks the destination dead and accounts for everything that will never
    // be delivered: the in-flight line (if any) and everything still queued.
    auto mark_dead = [&state](std::uint64_t in_flight) {
        state->dead = true;
        state->dropped.fetch_add(in_flight + state->queue.size(), std::memory_order_relaxed);
        state->queue.clear();
        state->queued_bytes = 0;
    };

    // A drop notice may be written only after a queued line has been delivered
    // since the previous notice. Otherwise a sustained overflow could keep the
    // writer emitting notices and never reach the queue. The one exception is
    // shutdown with an empty queue, where the pending count is still reported.
    bool notice_allowed = true;

    while (!state->dead) {
        state->cv.wait(lock, [&state, &notice_allowed] {
            return state->stop || !state->queue.empty() ||
                   (notice_allowed &&
                    state->reported != state->dropped.load(std::memory_order_relaxed));
        });

        const std::uint64_t total_dropped = state->dropped.load(std::memory_order_relaxed);
        const bool notice_due = state->reported != total_dropped;
        if (notice_due && (notice_allowed || (state->stop && state->queue.empty()))) {
            const std::uint64_t count = total_dropped - state->reported;
            state->reported = total_dropped;
            notice_allowed = false;
            lock.unlock();
            const std::string notice = "[" + std::to_string(count) + " console lines dropped]\n";
            const ConsoleOutput::Status status = state->output->write(notice.data(), notice.size());
            lock.lock();
            if (status == ConsoleOutput::Status::Dead) {
                mark_dead(0);
            }
            continue;
        }

        if (state->queue.empty()) {
            // Only reachable after stop was requested: everything has been delivered.
            break;
        }

        std::string line = std::move(state->queue.front());
        state->queue.pop_front();
        state->queued_bytes -= line.size();
        lock.unlock();

        const ConsoleOutput::Status status = state->output->write(line.data(), line.size());

        lock.lock();
        if (status == ConsoleOutput::Status::Dead) {
            mark_dead(1);
        } else {
            notice_allowed = true;
        }
    }

    state->writer_done = true;
    state->done_cv.notify_all();
}

std::shared_ptr<ConsoleOutput> BoundedConsoleSink::MakeStdoutOutput() {
#ifdef _WIN32
    HANDLE duplicate = nullptr;
    const HANDLE process = GetCurrentProcess();
    if (!DuplicateHandle(process,
                         GetStdHandle(STD_OUTPUT_HANDLE),
                         process,
                         &duplicate,
                         0,
                         FALSE,
                         DUPLICATE_SAME_ACCESS)) {
        return nullptr;
    }
    return std::make_shared<HandleOutput>(duplicate);
#else
    // A write to a pipe whose reader has exited raises SIGPIPE, which terminates
    // the process by default. Only the default disposition is changed; a handler
    // the host application installed is left alone.
    struct sigaction current {};
    if (sigaction(SIGPIPE, nullptr, &current) == 0 && current.sa_handler == SIG_DFL) {
        std::signal(SIGPIPE, SIG_IGN);
    }
    return MakeFileDescriptorOutput(STDOUT_FILENO);
#endif
}

#ifndef _WIN32
std::shared_ptr<ConsoleOutput>
BoundedConsoleSink::MakeFileDescriptorOutput(int fd, FileDescriptorWriteFn write_fn) {
    const int duplicate = ::dup(fd);
    if (duplicate < 0) {
        return nullptr;
    }
    return std::make_shared<FdOutput>(duplicate, write_fn);
}
#endif

bool BoundedConsoleSink::StdoutIsCharacterDevice() {
#ifdef _WIN32
    const DWORD type = GetFileType(GetStdHandle(STD_OUTPUT_HANDLE));
    return type == FILE_TYPE_CHAR || type == FILE_TYPE_UNKNOWN;
#else
    struct stat info {};
    if (fstat(STDOUT_FILENO, &info) != 0) {
        return true;
    }
    return S_ISCHR(info.st_mode) != 0;
#endif
}
