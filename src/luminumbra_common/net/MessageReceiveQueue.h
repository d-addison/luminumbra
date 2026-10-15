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
        if (!CanReceive())
            return false;
        std::vector<std::uint8_t> frame;
        if (size != 0) {
            const auto* bytes = static_cast<const std::uint8_t*>(data);
            frame.assign(bytes, bytes + size);
        }
        m_frames.push_back(std::move(frame));
        // Both operands are bounded before allocation, including on 32-bit hosts.
        m_queued_bytes += size;
        return true;
    }

    // Check before pulling one message from a backend; leave excess messages there.
    [[nodiscard]] bool CanReceive() const {
        return !m_closed && HasReceiveQueueCapacity(m_queued_bytes, m_frames.size());
    }
    // Diagnostics only: payload bytes owned by this queue, excluding delivered frames.
    [[nodiscard]] std::size_t QueuedBytes() const {
        return m_queued_bytes;
    }

    bool TryReceiveFrame(std::vector<std::uint8_t>& out) {
        if (m_frames.empty()) {
            m_drained = m_closed;
            return false;
        }
        const std::size_t size = m_frames.front().size();
        out = std::move(m_frames.front());
        m_frames.pop_front();
        m_queued_bytes -= size;
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
    std::size_t m_queued_bytes = 0;
    std::deque<std::vector<std::uint8_t>> m_frames;
};

} // namespace Luminumbra::Net
