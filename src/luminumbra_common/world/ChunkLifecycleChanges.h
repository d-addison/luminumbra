#pragma once

#include "luminumbra/core/Types.h"
#include <memory>
#include <mutex>
#include <unordered_set>

namespace Luminumbra {
class Chunk;

// State writers may run on blocking readiness workers. The owner consumes
// only changed resident IDs. Bindings and queued IDs are bounded by residency;
// replacement, erasure and index destruction unregister the old Chunk pointer.
// The external binding preserves Chunk's layout and byte-budget accounting.
class ChunkLifecycleChanges {
public:
    static void bind(const Chunk& chunk, const std::shared_ptr<ChunkLifecycleChanges>& changes);
    static void notify(const Chunk& chunk);

    void erase(ChunkID id) {
        const std::lock_guard lock(m_mutex);
        m_pending.erase(id);
    }
    void take(std::unordered_set<ChunkID>& pending) {
        const std::lock_guard lock(m_mutex);
        pending.clear();
        pending.swap(m_pending);
    }

private:
    void enqueue(ChunkID id) {
        const std::lock_guard lock(m_mutex);
        m_pending.insert(id);
    }
    std::mutex m_mutex;
    std::unordered_set<ChunkID> m_pending;
};
} // namespace Luminumbra
