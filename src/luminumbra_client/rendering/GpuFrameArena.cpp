#include "GpuFrameArena.h"

#include <limits>
#include <numeric>

namespace Luminumbra::Rendering {
namespace {
std::size_t combine_alignment(std::size_t first, std::size_t second) {
    if (first == 0 || second == 0) {
        return 0;
    }
    const auto factor = second / std::gcd(first, second);
    if (first > GpuFrameArena::kMaxStorageBytes / factor) {
        return 0;
    }
    return first * factor;
}

std::size_t padding(std::size_t offset, std::size_t align) {
    return (align - offset % align) % align;
}
} // namespace

GpuFrameArena::~GpuFrameArena() {
    destroy();
}

bool GpuFrameArena::create_buffer(std::size_t buffer_index, std::size_t bytes) {
    constexpr GLbitfield flags = GL_MAP_WRITE_BIT | GL_MAP_PERSISTENT_BIT | GL_MAP_COHERENT_BIT;
    glGenBuffers(1, &m_buffers[buffer_index]);
    if (m_buffers[buffer_index] == 0) {
        return false;
    }
    glBindBuffer(GL_COPY_WRITE_BUFFER, m_buffers[buffer_index]);
    glBufferStorage(GL_COPY_WRITE_BUFFER, static_cast<GLsizeiptr>(bytes), nullptr, flags);
    m_mappings[buffer_index] = static_cast<std::byte*>(
        glMapBufferRange(GL_COPY_WRITE_BUFFER, 0, static_cast<GLsizeiptr>(bytes), flags));
    return m_mappings[buffer_index] != nullptr;
}

bool GpuFrameArena::initialize(const Config& config) {
    if (m_storage_bytes != 0 || config.region_bytes == 0 || config.frame_regions == 0 ||
        config.frame_regions > kMaxFrameRegions || config.overflow_regions == 0 ||
        config.overflow_regions > kMaxOverflowRegions) {
        return false;
    }
    GLint uniform_alignment = 0;
    GLint storage_alignment = 0;
    glGetIntegerv(GL_UNIFORM_BUFFER_OFFSET_ALIGNMENT, &uniform_alignment);
    glGetIntegerv(GL_SHADER_STORAGE_BUFFER_OFFSET_ALIGNMENT, &storage_alignment);
    if (uniform_alignment <= 0 || storage_alignment <= 0) {
        return false;
    }
    const auto region_alignment =
        combine_alignment(combine_alignment(static_cast<std::size_t>(uniform_alignment),
                                            static_cast<std::size_t>(storage_alignment)),
                          4);
    if (region_alignment == 0 || config.region_bytes > kMaxStorageBytes) {
        return false;
    }
    const auto extra = padding(config.region_bytes, region_alignment);
    if (extra > kMaxStorageBytes - config.region_bytes) {
        return false;
    }
    const auto stride = config.region_bytes + extra;
    const auto count = config.frame_regions + config.overflow_regions;
    const auto byte_limit = static_cast<std::size_t>((std::numeric_limits<GLsizeiptr>::max)());
    if (stride > kMaxStorageBytes / count || stride > byte_limit / count) {
        return false;
    }

    GLint previous_binding = 0;
    glGetIntegerv(GL_COPY_WRITE_BUFFER_BINDING, &previous_binding);
    const bool created = create_buffer(0, stride * config.frame_regions) &&
                         create_buffer(1, stride * config.overflow_regions);
    glBindBuffer(GL_COPY_WRITE_BUFFER, static_cast<GLuint>(previous_binding));
    if (!created) {
        destroy();
        return false;
    }
    m_config = config;
    m_config.region_bytes = stride;
    m_uniform_alignment = static_cast<std::size_t>(uniform_alignment);
    m_storage_alignment = static_cast<std::size_t>(storage_alignment);
    m_storage_bytes = stride * count;
    return true;
}

bool GpuFrameArena::reclaim(Region& region) {
    if (region.pending) {
        if (region.completion == nullptr) {
            return false;
        }
        ++m_diagnostics.fence_polls;
        const auto status = glClientWaitSync(region.completion, GL_SYNC_FLUSH_COMMANDS_BIT, 0);
        if (status != GL_ALREADY_SIGNALED && status != GL_CONDITION_SATISFIED) {
            return false;
        }
        glDeleteSync(region.completion);
        region.completion = nullptr;
        region.pending = false;
    }
    region.cursor = 0;
    return true;
}

bool GpuFrameArena::begin_frame() {
    if (m_storage_bytes == 0 || m_active) {
        return false;
    }
    m_diagnostics = {};
    m_overflow_attempts = 0;
    m_overflow_current = kMaxOverflowRegions;
    m_primary_available = reclaim(m_regions[m_frame_cursor]);
    m_active = true;
    return true;
}

std::size_t GpuFrameArena::alignment(Use use) const noexcept {
    switch (use) {
        case Use::Uniform:
            return m_uniform_alignment;
        case Use::Storage:
            return m_storage_alignment;
        case Use::Vertex:
        case Use::Indirect:
            return 4;
    }
    return 0;
}

GpuFrameArena::Allocation
GpuFrameArena::reserve(std::size_t region_index, std::size_t bytes, std::size_t align) {
    Region& region = m_regions[region_index];
    const bool overflow = region_index >= m_config.frame_regions;
    const auto local_index = overflow ? region_index - m_config.frame_regions : region_index;
    const auto base = local_index * m_config.region_bytes;
    const auto extra = padding(base + region.cursor, align);
    const auto remaining = m_config.region_bytes - region.cursor;
    if (extra > remaining || bytes > remaining - extra) {
        return {};
    }
    const auto offset = base + region.cursor + extra;
    region.cursor += extra + bytes;
    region.touched = true;
    m_diagnostics.bytes += bytes;
    ++m_diagnostics.allocations;
    m_diagnostics.fallbacks += overflow ? 1u : 0u;
    const std::size_t buffer_index = overflow ? 1u : 0u;
    return {.buffer = m_buffers[buffer_index],
            .offset = offset,
            .bytes = bytes,
            .data = m_mappings[buffer_index] + offset,
            .fallback = overflow};
}

GpuFrameArena::Allocation
GpuFrameArena::allocate(std::size_t bytes, Use use, std::size_t extra_alignment) {
    const auto align = combine_alignment(alignment(use), extra_alignment);
    if (!m_active || bytes == 0 || bytes > m_config.region_bytes || align == 0) {
        ++m_diagnostics.refusals;
        return {};
    }
    if (m_primary_available) {
        if (auto allocation = reserve(m_frame_cursor, bytes, align)) {
            return allocation;
        }
    }
    if (m_overflow_current != kMaxOverflowRegions) {
        if (auto allocation = reserve(m_config.frame_regions + m_overflow_current, bytes, align)) {
            return allocation;
        }
    }
    // At most overflow_regions (<=8) candidates in TOTAL per frame. Exhaustion
    // makes subsequent requests O(1), even when every completion is quarantined.
    while (m_overflow_attempts < m_config.overflow_regions) {
        const auto candidate = m_overflow_cursor;
        m_overflow_cursor = (m_overflow_cursor + 1u) % m_config.overflow_regions;
        ++m_overflow_attempts;
        const auto index = m_config.frame_regions + candidate;
        if (!reclaim(m_regions[index])) {
            continue;
        }
        m_overflow_current = candidate;
        if (auto allocation = reserve(index, bytes, align)) {
            return allocation;
        }
    }
    ++m_diagnostics.refusals;
    return {};
}

bool GpuFrameArena::end_frame() {
    if (!m_active) {
        return false;
    }
    bool fenced = true;
    // At most 16 regions, independent of allocation/draw count.
    for (auto& region : m_regions) {
        if (!region.touched) {
            continue;
        }
        region.completion = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
        region.pending = true;
        region.touched = false;
        fenced = fenced && region.completion != nullptr;
    }
    m_frame_cursor = (m_frame_cursor + 1u) % m_config.frame_regions;
    m_active = false;
    return fenced;
}

void GpuFrameArena::destroy() {
    // Deleting GL objects preserves queued consumers under GL lifetime rules.
    // No wait is needed, including for missing/failed fences or an open frame.
    for (auto& region : m_regions) {
        if (region.completion != nullptr) {
            glDeleteSync(region.completion);
        }
        region = {};
    }
    for (auto& buffer : m_buffers) {
        if (buffer != 0) {
            glDeleteBuffers(1, &buffer);
        }
        buffer = 0;
    }
    m_mappings = {};
    m_uniform_alignment = 0;
    m_storage_alignment = 0;
    m_storage_bytes = 0;
    m_config = {};
    m_frame_cursor = 0;
    m_overflow_cursor = 0;
    m_overflow_attempts = 0;
    m_overflow_current = kMaxOverflowRegions;
    m_primary_available = false;
    m_active = false;
    m_diagnostics = {};
}

} // namespace Luminumbra::Rendering
