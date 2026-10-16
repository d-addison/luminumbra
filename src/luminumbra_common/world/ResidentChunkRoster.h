#pragma once

#include "Chunk.h"

#include <memory>
#include <span>
#include <unordered_map>
#include <vector>

namespace Luminumbra {

// Host-thread-only view owner. Refresh before every use; neither streaming-map
// mutation nor a second refresh may overlap a borrowed span's use. The retained
// owners keep all borrowed chunks alive throughout planning and publication.
class ResidentChunkRoster {
public:
    using Map = std::unordered_map<ChunkID, std::shared_ptr<Chunk>>;
    // Host-thread membership boundary only; invalidates any borrowed span.
    void clear() noexcept {
        m_chunks.clear();
    }

    std::span<const std::shared_ptr<Chunk>> refresh(const Map& chunks) {
        std::size_t position = 0;
        bool same = true;
        for (const auto& [id, chunk] : chunks) {
            (void)id;
            if (!chunk)
                continue;
            if (position >= m_chunks.size() || m_chunks[position].get() != chunk.get() ||
                m_chunks[position].owner_before(chunk) || chunk.owner_before(m_chunks[position])) {
                same = false;
                break;
            }
            ++position;
        }
        if (!same || position != m_chunks.size()) {
            std::vector<std::shared_ptr<Chunk>> next;
            next.reserve(chunks.size());
            for (const auto& [id, chunk] : chunks) {
                (void)id;
                if (chunk)
                    next.push_back(chunk);
            }
            m_chunks.swap(next);
        }
        // Do not cache Loading eligibility or any mutable chunk state.
        return m_chunks;
    }

private:
    std::vector<std::shared_ptr<Chunk>> m_chunks{};
};

} // namespace Luminumbra
