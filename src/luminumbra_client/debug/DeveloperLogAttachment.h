#pragma once

#include "debug/DeveloperLogSink.h"

namespace spdlog::sinks {
class sink;
}
namespace Luminumbra::Client::Debug {
// Attachment and detachment change only the permanent dispatcher target.
class DeveloperLogAttachment {
public:
    explicit DeveloperLogAttachment(std::shared_ptr<DeveloperLogSink> log);
    ~DeveloperLogAttachment();
    void Detach();
    DeveloperLogAttachment(const DeveloperLogAttachment&) = delete;
    DeveloperLogAttachment& operator=(const DeveloperLogAttachment&) = delete;

private:
    std::shared_ptr<spdlog::sinks::sink> m_sink;
};
} // namespace Luminumbra::Client::Debug
