#pragma once

#include <glad/glad.h>

#include <array>
#include <cstddef>

namespace Luminumbra::Rendering {

// Context-thread upload storage. Write allocations before issuing their consumers,
// and call end_frame after the FINAL consumer (including replays/shadows). CPU
// pointers expire at end_frame; GPU ranges remain reserved until their fence signals.
// All methods, including destruction, require the owning GL context to be current.
class GpuFrameArena {
public:
    // Bounds include pending/quarantined regions even if the GPU never completes.
    static constexpr std::size_t kMaxFrameRegions = 8;
    static constexpr std::size_t kMaxOverflowRegions = 8;
    static constexpr std::size_t kMaxStorageBytes = 512u * 1024u * 1024u;

    struct Config {
        std::size_t region_bytes = 4u * 1024u * 1024u;
        std::size_t frame_regions = 3;
        std::size_t overflow_regions = 3;
    };
    enum class Use {
        Uniform,
        Storage,
        Vertex,
        Indirect
    };
    struct Allocation {
        GLuint buffer = 0;
        std::size_t offset = 0;
        std::size_t bytes = 0;
        std::byte* data = nullptr;
        bool fallback = false;

        explicit operator bool() const noexcept {
            return data != nullptr;
        }
    };
    struct Diagnostics {
        std::size_t bytes = 0; // payload bytes, excluding padding
        std::size_t allocations = 0;
        std::size_t fallbacks = 0; // successful allocations in separate overflow storage
        std::size_t refusals = 0;
        std::size_t fence_polls = 0;
        std::size_t fence_waits = 0; // always zero: only zero-timeout polls are permitted
    };

    GpuFrameArena() = default;
    ~GpuFrameArena();
    GpuFrameArena(const GpuFrameArena&) = delete;
    GpuFrameArena& operator=(const GpuFrameArena&) = delete;
    GpuFrameArena(GpuFrameArena&&) = delete;
    GpuFrameArena& operator=(GpuFrameArena&&) = delete;

    bool initialize(const Config& config);
    bool initialize() {
        return initialize(Config{});
    }
    // Invalid sequencing returns false without changing the active frame.
    // A busy primary region still opens a frame: allocate then uses overflow.
    bool begin_frame();
    // Empty, oversized, invalid alignment/use, inactive-frame and exhausted-pool
    // requests return an empty allocation. No implicit wait or storage growth.
    // Extra alignment may express a vertex stride; it need not be a power of two.
    Allocation allocate(std::size_t bytes, Use use, std::size_t extra_alignment = 1);
    // False on invalid sequencing or fence creation failure. Missing/failed
    // fences quarantine their regions; they can never authorize an overwrite.
    bool end_frame();
    void destroy();

    std::size_t alignment(Use use) const noexcept;
    std::size_t region_bytes() const noexcept {
        return m_config.region_bytes;
    }
    std::size_t storage_bytes() const noexcept {
        return m_storage_bytes;
    }
    // Per-frame counters reset only on a successful begin_frame.
    const Diagnostics& diagnostics() const noexcept {
        return m_diagnostics;
    }

private:
    struct Region {
        std::size_t cursor = 0;
        GLsync completion = nullptr;
        bool pending = false;
        bool touched = false;
    };

    bool reclaim(Region& region);
    Allocation reserve(std::size_t region_index, std::size_t bytes, std::size_t align);
    bool create_buffer(std::size_t buffer_index, std::size_t bytes);

    Config m_config{};
    std::array<GLuint, 2> m_buffers{};
    std::array<std::byte*, 2> m_mappings{};
    std::array<Region, kMaxFrameRegions + kMaxOverflowRegions> m_regions{};
    std::size_t m_uniform_alignment = 0;
    std::size_t m_storage_alignment = 0;
    std::size_t m_storage_bytes = 0;
    std::size_t m_frame_cursor = 0;
    std::size_t m_overflow_cursor = 0;
    std::size_t m_overflow_attempts = 0;
    std::size_t m_overflow_current = kMaxOverflowRegions;
    bool m_primary_available = false;
    bool m_active = false;
    Diagnostics m_diagnostics{};
};

} // namespace Luminumbra::Rendering
