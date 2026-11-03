#include "debug/DeveloperLogSink.h"

#include <algorithm>

namespace Luminumbra::Client::Debug {
void DeveloperLogSink::Push(LogSeverity severity,
                            std::string message,
                            std::chrono::system_clock::time_point timestamp) {
    std::lock_guard lock(m_mutex);
    if (m_closed)
        return;
    if (message.size() > kMessageLimit)
        ++m_truncations;
    std::string bounded(message.data(), std::min(message.size(), kMessageLimit));
    while (!m_records.empty() &&
           (m_records.size() >= kRecordLimit || m_bytes + bounded.size() > kByteLimit)) {
        m_bytes -= m_records.front().message.size();
        m_records.pop_front();
        ++m_evictions;
    }
    m_bytes += bounded.size();
    m_records.push_back({severity, timestamp, ++m_sequence, std::move(bounded)});
}
std::vector<DeveloperLogRecord> DeveloperLogSink::Snapshot(LogSeverity minimum) const {
    std::lock_guard lock(m_mutex);
    std::vector<DeveloperLogRecord> result;
    for (const auto& record : m_records)
        if (record.severity >= minimum)
            result.push_back(record);
    return result;
}
void DeveloperLogSink::Clear() {
    std::lock_guard lock(m_mutex);
    m_records.clear();
    m_bytes = 0;
}
void DeveloperLogSink::Close() {
    std::lock_guard lock(m_mutex);
    m_closed = true;
}
std::size_t DeveloperLogSink::RetainedBytes() const {
    std::lock_guard lock(m_mutex);
    return m_bytes;
}
std::uint64_t DeveloperLogSink::Evictions() const {
    std::lock_guard lock(m_mutex);
    return m_evictions;
}
std::uint64_t DeveloperLogSink::Truncations() const {
    std::lock_guard lock(m_mutex);
    return m_truncations;
}
std::size_t DeveloperLogSink::RetainedCapacity() const {
    std::lock_guard lock(m_mutex);
    std::size_t capacity = 0;
    for (const auto& record : m_records)
        capacity += record.message.capacity();
    return capacity;
}
} // namespace Luminumbra::Client::Debug
