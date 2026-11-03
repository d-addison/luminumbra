#pragma once

#include <memory>
#include <mutex>
#include <spdlog/sinks/base_sink.h>

// Permanent logger sink. Only the target changes after logger initialization.
class LogDispatcherSink final : public spdlog::sinks::base_sink<std::mutex> {
public:
    void Attach(std::shared_ptr<spdlog::sinks::sink> target) {
        std::lock_guard lock(mutex_);
        m_target = std::move(target);
    }
    void Detach(const std::shared_ptr<spdlog::sinks::sink>& target) {
        std::lock_guard lock(mutex_);
        if (m_target == target)
            m_target.reset();
    }

protected:
    void sink_it_(const spdlog::details::log_msg& message) override {
        if (m_target)
            m_target->log(message);
    }
    void flush_() override {
        if (m_target)
            m_target->flush();
    }

private:
    std::shared_ptr<spdlog::sinks::sink> m_target;
};
