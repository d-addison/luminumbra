#pragma once
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <spdlog/sinks/base_sink.h>
#include <thread>

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

// Console sink that never lets a logging call wait on console I/O. Each
// formatted line is queued (bounded by message count and total bytes) and a
// writer thread delivers it to a ConsoleOutput. When the queue is full the
// newest message is dropped and counted; once output resumes, the writer emits
// "[N console lines dropped]" before the next line.
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
    // Duplicates an arbitrary file descriptor. Returns nullptr if dup fails.
    static std::shared_ptr<ConsoleOutput> MakeFileDescriptorOutput(int fd);
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
