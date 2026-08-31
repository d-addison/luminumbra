#include "AsyncReadbackRing.h"

#include <glad/glad.h>

#include <algorithm>

//   -- GL implementation of the async readback ring.
// The PUBLIC interface (AsyncReadbackRing.h) is backend-agnostic; all GL fence /
// persistent-map machinery lives here so 's RHI can re-back it without
// touching callers. Each slot is a persistent READ-mapped SSBO; submit copies
// GPU->slot and inserts a fence; consume polls fences with a ZERO timeout and
// returns the newest completed slot's coherent map -- never blocking the frame.

namespace Luminumbra::Rendering {

namespace {
// Persistent + coherent so the CPU sees the GPU's copy once the fence signals,
// with no explicit flush/map per frame (mirrors the proven write-side ring in
// RenderPipeline.cpp / FoliagePass.cpp, with READ instead of WRITE).
constexpr GLbitfield kReadbackFlags = GL_MAP_READ_BIT | GL_MAP_PERSISTENT_BIT | GL_MAP_COHERENT_BIT;
} // namespace

AsyncReadbackRing::~AsyncReadbackRing() {
    shutdown();
}

bool AsyncReadbackRing::allocate(std::size_t slot_bytes, int num_slots) {
    free_slots();
    m_slots.assign(static_cast<std::size_t>(num_slots), Slot{});
    for (auto& slot : m_slots) {
        glGenBuffers(1, &slot.ssbo);
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, slot.ssbo);
        glBufferStorage(
            GL_SHADER_STORAGE_BUFFER, static_cast<GLsizeiptr>(slot_bytes), nullptr, kReadbackFlags);
        slot.mapped = glMapBufferRange(
            GL_SHADER_STORAGE_BUFFER, 0, static_cast<GLsizeiptr>(slot_bytes), kReadbackFlags);
        if (slot.mapped == nullptr) {
            glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
            free_slots();
            return false;
        }
    }
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
    m_slot_bytes = slot_bytes;
    m_num_slots = num_slots;
    m_cursor = 0;
    m_open_slot = -1;
    m_next_seq = 1;
    m_last_consumed_seq = 0;
    m_initialized = true;
    return true;
}

void AsyncReadbackRing::free_slots() {
    for (auto& slot : m_slots) {
        if (slot.fence != nullptr) {
            glDeleteSync(static_cast<GLsync>(slot.fence));
            slot.fence = nullptr;
        }
        if (slot.ssbo != 0) {
            glBindBuffer(GL_SHADER_STORAGE_BUFFER, slot.ssbo);
            glUnmapBuffer(GL_SHADER_STORAGE_BUFFER);
            glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
            glDeleteBuffers(1, &slot.ssbo);
            slot.ssbo = 0;
        }
        slot.mapped = nullptr;
    }
    m_slots.clear();
    m_initialized = false;
}

bool AsyncReadbackRing::ensure(std::size_t slot_bytes, int num_slots) {
    num_slots = std::max(2, num_slots);
    if (m_initialized && slot_bytes <= m_slot_bytes && num_slots <= m_num_slots) {
        return true;
    }
    // (Re)allocate when first used or when the requested geometry grows.
    return allocate(std::max(slot_bytes, m_slot_bytes), std::max(num_slots, m_num_slots));
}

void AsyncReadbackRing::shutdown() {
    free_slots();
}

bool AsyncReadbackRing::begin() {
    if (!m_initialized || m_open_slot >= 0) {
        return false;
    }
    Slot& slot = m_slots[static_cast<std::size_t>(m_cursor)];
    // If the targeted slot is still in flight, poll its fence once (zero timeout).
    // GL_SYNC_FLUSH_COMMANDS_BIT flushes the queue so the fence can actually signal
    // even when nothing else flushed this frame (e.g. an offscreen render with no
    // SwapBuffers) -- still NON-blocking (timeout 0), and not GL_TIMEOUT_IGNORED.
    // Still unsignalled -> every slot is busy; skip this frame (stale-safe).
    if (slot.fence != nullptr) {
        GLenum r = glClientWaitSync(static_cast<GLsync>(slot.fence), GL_SYNC_FLUSH_COMMANDS_BIT, 0);
        if (r != GL_ALREADY_SIGNALED && r != GL_CONDITION_SATISFIED) {
            return false;
        }
        glDeleteSync(static_cast<GLsync>(slot.fence));
        slot.fence = nullptr;
        slot.completed = true; // it holds a (now older) result until overwritten
    }
    slot.payload_bytes = 0;
    m_open_slot = m_cursor;
    return true;
}

void AsyncReadbackRing::copy_region(unsigned int src_buffer,
                                    std::ptrdiff_t src_offset,
                                    std::ptrdiff_t dst_offset,
                                    std::size_t num_bytes) {
    if (m_open_slot < 0) {
        return;
    }
    Slot& slot = m_slots[static_cast<std::size_t>(m_open_slot)];
    const std::size_t end = static_cast<std::size_t>(dst_offset) + num_bytes;
    if (end > m_slot_bytes) {
        return; // would overflow the slot; drop (caller sized ensure() too small)
    }
    glBindBuffer(GL_COPY_READ_BUFFER, src_buffer);
    glBindBuffer(GL_COPY_WRITE_BUFFER, slot.ssbo);
    glCopyBufferSubData(GL_COPY_READ_BUFFER,
                        GL_COPY_WRITE_BUFFER,
                        static_cast<GLintptr>(src_offset),
                        static_cast<GLintptr>(dst_offset),
                        static_cast<GLsizeiptr>(num_bytes));
    glBindBuffer(GL_COPY_READ_BUFFER, 0);
    glBindBuffer(GL_COPY_WRITE_BUFFER, 0);
    slot.payload_bytes = std::max(slot.payload_bytes, end);
}

void AsyncReadbackRing::submit() {
    if (m_open_slot < 0) {
        return;
    }
    Slot& slot = m_slots[static_cast<std::size_t>(m_open_slot)];
    // Ensure the copies are ordered before the fence's commands, then fence.
    glMemoryBarrier(GL_BUFFER_UPDATE_BARRIER_BIT);
    slot.fence = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
    slot.seq = m_next_seq++;
    slot.completed = false;
    m_open_slot = -1;
    m_cursor = (m_cursor + 1) % m_num_slots;
}

int AsyncReadbackRing::scan_newest() {
    // Poll every in-flight fence (zero timeout = non-blocking); promote signalled
    // ones to completed. Return the index of the newest completed slot that is
    // newer than the last consumed, or -1. Does NOT advance the consume cursor.
    int best = -1;
    std::uint64_t best_seq = m_last_consumed_seq;
    for (int i = 0; i < m_num_slots; ++i) {
        Slot& slot = m_slots[static_cast<std::size_t>(i)];
        if (slot.fence != nullptr) {
            // GL_SYNC_FLUSH_COMMANDS_BIT ensures the fence is flushed to the GPU so
            // it can signal even when nothing else flushed (offscreen render); the
            // zero timeout keeps the poll non-blocking (not GL_TIMEOUT_IGNORED).
            GLenum r =
                glClientWaitSync(static_cast<GLsync>(slot.fence), GL_SYNC_FLUSH_COMMANDS_BIT, 0);
            if (r == GL_ALREADY_SIGNALED || r == GL_CONDITION_SATISFIED) {
                glDeleteSync(static_cast<GLsync>(slot.fence));
                slot.fence = nullptr;
                slot.completed = true;
            }
        }
        if (slot.completed && slot.seq > best_seq) {
            best = i;
            best_seq = slot.seq;
        }
    }
    return best;
}

bool AsyncReadbackRing::poll() {
    if (!m_initialized) {
        return false;
    }
    return scan_newest() >= 0;
}

bool AsyncReadbackRing::consume(const void** out_ptr, std::size_t* out_bytes) {
    if (!m_initialized) {
        return false;
    }
    const int best = scan_newest();
    if (best < 0) {
        return false; // no completed result newer than the last consumed
    }
    Slot& slot = m_slots[static_cast<std::size_t>(best)];
    m_last_consumed_seq = slot.seq;
    if (out_ptr != nullptr) {
        *out_ptr = slot.mapped;
    }
    if (out_bytes != nullptr) {
        *out_bytes = slot.payload_bytes;
    }
    return true;
}

} // namespace Luminumbra::Rendering
