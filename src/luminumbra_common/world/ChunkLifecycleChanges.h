#pragma once

#include "luminumbra/core/Types.h"
#include <mutex>
#include <unordered_set>

namespace Luminumbra {
// State writers may run on blocking readiness workers. The owner consumes
// only changed resident IDs. Queued IDs are bounded by residency; replacement,
// erasure and index destruction unbind the chunk before draining old IDs.
// This is a polled diagnostic queue, with no condition-variable waiters.
class ChunkLifecycleChanges {
public:
    void enqueue(ChunkID id) {
        const std::lock_guard lock(m_mutex);
        m_pending.insert(id);
    }

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
    std::mutex m_mutex;
    std::unordered_set<ChunkID> m_pending;
};
} // namespace Luminumbra
