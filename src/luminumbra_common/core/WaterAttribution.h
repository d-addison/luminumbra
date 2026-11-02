#pragma once

#include <cstdint>

namespace Luminumbra::WaterAttribution {

// Nested water stages, inclusive in the existing stream bucket. Diagnostics only.
struct Frame {
    double selection_ms = 0;
    double seed_inputs_ms = 0;
    double initialization_ms = 0;
    double sim_ms = 0;
    double seam_ms = 0;
    double bookkeeping_ms = 0;
    std::uint64_t precomputed_columns = 0;
    std::uint64_t fallback_columns = 0;
    std::uint64_t stale_columns = 0;
};

inline Frame*& CurrentFrame() {
    static thread_local Frame* frame = nullptr;
    return frame;
}

// Same opt-in, thread-local, nestable collection as foliage frame attribution.
class FrameScope {
public:
    explicit FrameScope(bool enabled = false)
        : m_previous(CurrentFrame()) {
        CurrentFrame() = enabled ? &m_frame : nullptr;
    }
    ~FrameScope() {
        CurrentFrame() = m_previous;
    }
    FrameScope(const FrameScope&) = delete;
    FrameScope& operator=(const FrameScope&) = delete;
    [[nodiscard]] const Frame& snapshot() const {
        return m_frame;
    }

private:
    mutable Frame m_frame{};
    Frame* m_previous;
};

inline void Record(const Frame& update) {
    if (auto* frame = CurrentFrame()) {
        frame->selection_ms += update.selection_ms;
        frame->seed_inputs_ms += update.seed_inputs_ms;
        frame->initialization_ms += update.initialization_ms;
        frame->sim_ms += update.sim_ms;
        frame->seam_ms += update.seam_ms;
        frame->bookkeeping_ms += update.bookkeeping_ms;
        frame->precomputed_columns += update.precomputed_columns;
        frame->fallback_columns += update.fallback_columns;
        frame->stale_columns += update.stale_columns;
    }
}

} // namespace Luminumbra::WaterAttribution
