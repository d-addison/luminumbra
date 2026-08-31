#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

// the async GPU->CPU readback ring.
//
// THE PROBLEM: render code reads GPU buffers back with in-frame
// SYNCHRONOUS calls -- glClientWaitSync(..., GL_TIMEOUT_IGNORED) + glMapBuffer(
// GL_READ_ONLY) (the  path) and glGetBufferSubData (the foliage gate
// readback). Each blocks the main thread on GPU completion, surfacing as p99
// tail latency. This ring replaces that pattern: submit issues the copy + a
// fence and RETURNS IMMEDIATELY; the result is consumed a later frame, once the
// fence has signalled, by a STALE-SAFE consumer that never blocks the frame.
//
// BACKEND-AGNOSTIC INTERFACE: the public API is submit/poll/consume
// in plain types -- no GL names in any signature a caller sees (the source
// buffer is an opaque `unsigned int` handle today; 's RHI backs it with
// timeline semaphores / DX12 fences without callers changing). The GL fence +
// persistent-mapped SSBO are the IMPLEMENTATION, confined to the.cpp.
//
// DETERMINISM: render-only. The ring feeds render/gate consumers (foliage
// instance_hash now; exposure metering /  later) -- never world_hash. A
// stale (N-frame-old) result is the contract, not a bug.
//
// USAGE (per frame, gate/render path only):
//     if (ring.ensure(slot_bytes, 3) && ring.begin) {
//         ring.copy_region(src_ssbo, src_off, dst_off, n);   // 1+ regions
//         ring.submit;                                       // fence; returns now
//     }
//     const void* p; std::size_t n;
//     if (ring.consume(&p, &n)) { /* newest COMPLETED result; copy it out */ }
//     // else: no new result -- keep the previously consumed copy (stale-safe).

namespace Luminumbra::Rendering {

class AsyncReadbackRing {
public:
    AsyncReadbackRing() = default;
    ~AsyncReadbackRing();

    AsyncReadbackRing(const AsyncReadbackRing&) = delete;
    AsyncReadbackRing& operator=(const AsyncReadbackRing&) = delete;

    // Idempotent lazy allocation of `num_slots` readback slots of `slot_bytes`
    // each (persistent-mapped for read). Safe to call every frame; allocates only
    // on the first call (or after shutdown, or if the requested geometry grows).
    // `num_slots` is clamped to >= 2 (triple-buffer default keeps a free slot for
    // begin while two are in flight). Returns false if GPU allocation failed.
    bool ensure(std::size_t slot_bytes, int num_slots = 3);
    bool initialized() const {
        return m_initialized;
    }
    void shutdown();

    // Begin a readback into the next slot in round-robin order. Returns false if
    // that slot's previous copy is still in flight (its fence has not signalled)
    // -- the caller then skips this frame's submit and keeps its last consumed
    // result (stale-safe). On true, issue copy_region(s) then submit.
    bool begin();

    // GPU->GPU copy of [src_offset, src_offset + num_bytes) from `src_buffer` into
    // the current slot at `dst_offset`. Must be between begin and submit.
    // `src_buffer` is the GL buffer name today (an RHI buffer handle behind ).
    void copy_region(unsigned int src_buffer,
                     std::ptrdiff_t src_offset,
                     std::ptrdiff_t dst_offset,
                     std::size_t num_bytes);

    // Fence the current slot's copies and advance the cursor. Returns immediately
    // -- never waits on the GPU.
    void submit();

    // Non-blocking, NON-DESTRUCTIVE readiness check: polls every
    // in-flight fence with a ZERO timeout and returns true iff a result newer than
    // the last consumed one is now available. Does NOT advance the consume cursor,
    // so a true here means the next consume will deliver. Never blocks.
    bool poll();

    // Non-blocking poll. Polls every in-flight fence with a ZERO
    // timeout; if a slot newer than the last one returned has completed, sets
    // *out_ptr / *out_bytes to its mapped payload and returns true. Otherwise
    // returns false ("no new completed result yet") -- an ordinary, non-blocking
    // state. The pointer stays valid until that slot is reused (`num_slots`
    // submits later); copy what you need out promptly.
    bool consume(const void** out_ptr, std::size_t* out_bytes);

    int depth() const {
        return m_num_slots;
    }
    std::size_t slot_bytes() const {
        return m_slot_bytes;
    }

private:
    struct Slot {
        unsigned int ssbo = 0;         // GLuint readback buffer
        void* mapped = nullptr;        // persistent READ map base
        void* fence = nullptr;         // GLsync; non-null while in flight
        std::size_t payload_bytes = 0; // bytes written by the last submit
        std::uint64_t seq = 0;         // submit sequence number
        bool completed = false;        // fence signalled; holds a result
    };

    bool allocate(std::size_t slot_bytes, int num_slots);
    void free_slots();
    // Polls fences (non-blocking) and returns the newest completed slot index
    // newer than the last consumed, or -1. Shared by poll + consume.
    int scan_newest();

    std::vector<Slot> m_slots;
    std::size_t m_slot_bytes = 0;
    int m_num_slots = 0;
    int m_cursor = 0;             // next slot begin() targets
    int m_open_slot = -1;         // slot currently between begin()/submit()
    std::uint64_t m_next_seq = 1; // monotonic submit counter
    std::uint64_t m_last_consumed_seq = 0;
    bool m_initialized = false;
};

} // namespace Luminumbra::Rendering
