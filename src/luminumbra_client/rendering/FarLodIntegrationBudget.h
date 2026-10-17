#pragma once

#include <cstddef>
#include <cstdint>

namespace Luminumbra::Rendering {

// FIFO admission policy, measured in frames rather than wall time. Limits are
// per tile (terrain + water together). An old or backlogged oversized head may
// consume a frame alone; the count cap always holds.
class FarLodIntegrationBudget {
public:
    static constexpr std::size_t kMaxUploads = 6;
    static constexpr std::size_t kMaxBytes = 4u * 1024u * 1024u;
    static constexpr std::size_t kMaxBacklog = 64; // builds + completions + ready uploads
    static constexpr std::size_t kBacklogPressure = 48;
    static constexpr std::uint64_t kMaxDeferredFrames = 8;

    bool admit(std::size_t bytes, std::uint64_t age_frames, std::size_t backlog) {
        if (m_uploads >= kMaxUploads)
            return false;
        const bool fits = m_bytes <= kMaxBytes && bytes <= kMaxBytes - m_bytes;
        if (!fits &&
            (m_uploads != 0 || (age_frames < kMaxDeferredFrames && backlog < kBacklogPressure)))
            return false;
        // Each accepted buffer is checked against GLsizeiptr before this call.
        // An oversized item is alone, so this addition cannot overflow.
        m_bytes += bytes;
        ++m_uploads;
        return true;
    }

private:
    std::size_t m_uploads = 0;
    std::size_t m_bytes = 0;
};

} // namespace Luminumbra::Rendering
