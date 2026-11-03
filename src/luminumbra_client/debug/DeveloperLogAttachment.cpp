#include "debug/DeveloperLogAttachment.h"

#include "core/Log.h"
#include "core/LogDispatcherSink.h"
#include <algorithm>
#include <spdlog/sinks/base_sink.h>

namespace Luminumbra::Client::Debug {
namespace {
class ClientLogBridge final : public spdlog::sinks::base_sink<std::mutex> {
public:
    explicit ClientLogBridge(std::shared_ptr<DeveloperLogSink> target)
        : m_target(std::move(target)) {}

protected:
    void sink_it_(const spdlog::details::log_msg& message) override {
        if (!m_target)
            return;
        const auto severity = static_cast<LogSeverity>(
            std::min(static_cast<int>(message.level), static_cast<int>(LogSeverity::Critical)));
        // Keep a one-byte overflow sentinel so the pure buffer counts truncations.
        const auto length = std::min(message.payload.size(), DeveloperLogSink::kMessageLimit + 1);
        m_target->Push(severity, std::string(message.payload.data(), length), message.time);
    }
    void flush_() override {}

private:
    std::shared_ptr<DeveloperLogSink> m_target;
};
} // namespace
DeveloperLogAttachment::DeveloperLogAttachment(std::shared_ptr<DeveloperLogSink> log)
    : m_sink(std::make_shared<ClientLogBridge>(std::move(log))) {
    Log::GetDispatcherSink()->Attach(m_sink);
}
DeveloperLogAttachment::~DeveloperLogAttachment() {
    Detach();
}
void DeveloperLogAttachment::Detach() {
    Log::GetDispatcherSink()->Detach(m_sink);
}
} // namespace Luminumbra::Client::Debug
