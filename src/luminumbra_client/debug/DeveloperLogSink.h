#pragma once

#include <chrono>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace Luminumbra::Client::Debug {
enum class LogSeverity {
    Trace,
    Debug,
    Info,
    Warning,
    Error,
    Critical
};
struct DeveloperLogRecord {
    LogSeverity severity;
    std::chrono::system_clock::time_point timestamp;
    std::uint64_t sequence;
    std::string message;
};
// Shared producer handle survives client detachment; Close rejects subsequent records.
class DeveloperLogSink {
public:
    static constexpr std::size_t kRecordLimit = 2048;
    static constexpr std::size_t kByteLimit = 512 * 1024;
    static constexpr std::size_t kMessageLimit = 8 * 1024;
    void Push(LogSeverity severity,
              std::string message,
              std::chrono::system_clock::time_point timestamp = std::chrono::system_clock::now());
    std::vector<DeveloperLogRecord> Snapshot(LogSeverity minimum = LogSeverity::Trace) const;
    void Clear();
    void Close();
    std::size_t RetainedBytes() const;
    std::size_t RetainedCapacity() const;
    std::uint64_t Evictions() const;
    std::uint64_t Truncations() const;

private:
    mutable std::mutex m_mutex;
    std::deque<DeveloperLogRecord> m_records;
    std::size_t m_bytes = 0;
    std::uint64_t m_sequence = 0, m_evictions = 0, m_truncations = 0;
    bool m_closed = false;
};
} // namespace Luminumbra::Client::Debug
