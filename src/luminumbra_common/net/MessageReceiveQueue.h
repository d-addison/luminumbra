#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <utility>
#include <vector>

#include "LockstepSession.h"

namespace Luminumbra::Net {

// Admission and ordered delivery for message-oriented socket backends. A bad
// message closes admission without discarding valid frames already received.
class MessageReceiveQueue {
public:
    bool Push(const void* data, std::size_t size) {
        if (m_closed)
            return false;
        if (size > kMaxFramePayloadBytes) {
            Close();
            return false;
        }
        m_frames.emplace_back();
        if (size != 0) {
            const auto* bytes = static_cast<const std::uint8_t*>(data);
            m_frames.back().assign(bytes, bytes + size);
        }
        return true;
    }

    bool TryReceiveFrame(std::vector<std::uint8_t>& out) {
        if (m_frames.empty()) {
            m_drained = m_closed;
            return false;
        }
        out = std::move(m_frames.front());
        m_frames.pop_front();
        return true;
    }

    void Close() {
        if (m_closed)
            return;
        m_closed = true;
        m_drained = m_frames.empty();
    }
    [[nodiscard]] bool IsClosed() const {
        return m_closed;
    }
    // Closure behind queued frames becomes observable on the first empty poll,
    // matching a TCP receiver that encounters EOF after its buffered frames.
    [[nodiscard]] bool IsDrained() const {
        return m_drained;
    }

private:
    bool m_closed = false;
    bool m_drained = false;
    std::deque<std::vector<std::uint8_t>> m_frames;
};

} // namespace Luminumbra::Net
