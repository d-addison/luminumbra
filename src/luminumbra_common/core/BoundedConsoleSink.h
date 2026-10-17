#pragma once
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <spdlog/sinks/base_sink.h>
#include <thread>

#ifndef _WIN32
#include <sys/types.h>
#include <unistd.h>
#endif

// Destination for console text. The sink hands it complete, already formatted
// lines from a single writer thread. Returning Dead marks the destination as
// unusable; every later message is counted as dropped and never written.
class ConsoleOutput {
public:
    enum class Status {
        Ok,
        Dead
    };

    virtual ~ConsoleOutput() = default;
    virtual Status write(const char* data, std::size_t length) = 0;
};

#ifndef _WIN32
// Signature of the raw write call used by the descriptor output. The default is
// ::write; tests substitute scripted results to exercise short writes and EINTR.
using FileDescriptorWriteFn = ssize_t (*)(int, const void*, std::size_t);
#endif

// Console sink that never lets a logging call wait on console I/O. Each
// formatted line is queued (bounded by message count and total bytes) and a
// writer thread delivers it to a ConsoleOutput. When the queue is full the
// newest message is dropped and counted. Once output resumes, the writer emits
// one "[N console lines dropped]" notice, and it emits another only after a
// queued line has been delivered.
//
// A null output is treated as Dead: every message is counted as dropped and
// nothing is ever written.
//
// SIGPIPE: MakeStdoutOutput() sets SIGPIPE to ignored when its disposition is
// SIG_DFL, so a closed reader surfaces as a write error instead of terminating
// the process. This is a process-wide change and is applied only in that case.
class BoundedConsoleSink : public spdlog::sinks::base_sink<std::mutex> {
public:
    struct Limits {
        std::size_t max_messages = 2048;
        std::size_t max_queued_bytes = 512 * 1024;
        std::size_t max_message_bytes = 8192;
    };

    BoundedConsoleSink(std::shared_ptr<ConsoleOutput> output, Limits limits);
    ~BoundedConsoleSink() override;

    BoundedConsoleSink(const BoundedConsoleSink&) = delete;
    BoundedConsoleSink& operator=(const BoundedConsoleSink&) = delete;

    // Number of messages discarded so far (overflow, or output reported Dead).
    std::uint64_t dropped_count() const;

    // Duplicates the process standard output. Returns nullptr if the handle
    // cannot be duplicated.
    static std::shared_ptr<ConsoleOutput> MakeStdoutOutput();

#ifndef _WIN32
    // Ignore the default SIGPIPE disposition so closed pipes report write errors.
    // Process-wide; preserves any handler installed by the host application.
    // Also used by tools that install their own console logger.
    static void IgnoreDefaultSigpipe();

    // Duplicates an arbitrary file descriptor and writes through write_fn.
    // Returns nullptr if dup fails.
    static std::shared_ptr<ConsoleOutput>
    MakeFileDescriptorOutput(int fd, FileDescriptorWriteFn write_fn = &::write);
#endif

    // True when standard output is a character device (terminal) or its type
    // cannot be determined. Such output stays on the synchronous sink.
    static bool StdoutIsCharacterDevice();

protected:
    void sink_it_(const spdlog::details::log_msg& msg) override;
    void flush_() override;

private:
    struct State;
    static void RunWriter(const std::shared_ptr<State>& state);

    std::shared_ptr<State> m_state;
    std::thread m_writer;
};
