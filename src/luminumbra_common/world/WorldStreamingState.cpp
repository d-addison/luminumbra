#include "WorldStreamingState.h"

#include <algorithm>
#include <mutex>
#include <utility>

namespace Luminumbra {

WorldStreamingState::ChunkPtr WorldStreamingState::find_chunk(ChunkID id) const {
    std::shared_lock<std::shared_mutex> lock(m_chunks_mutex);
    auto it = m_chunks.find(id);
    return it != m_chunks.end() ? it->second : nullptr;
}

WorldStreamingState::ChunkPtr WorldStreamingState::find_chunk(const IVec3& coords) const {
    return find_chunk(Chunk::calculate_id(coords));
}

WorldStreamingState::ChunkPtr WorldStreamingState::get_or_create_chunk(const IVec3& coords) {
    const ChunkID id = Chunk::calculate_id(coords);

    {
        std::shared_lock<std::shared_mutex> read_lock(m_chunks_mutex);
        auto it = m_chunks.find(id);
        if (it != m_chunks.end()) {
            return it->second;
        }
    }

    auto chunk = std::make_shared<Chunk>(coords);
    std::unique_lock<std::shared_mutex> write_lock(m_chunks_mutex);
    auto [it, inserted] = m_chunks.emplace(id, chunk);
    return inserted ? chunk : it->second;
}

bool WorldStreamingState::insert_chunk(ChunkPtr chunk) {
    if (!chunk) {
        return false;
    }

    std::unique_lock<std::shared_mutex> lock(m_chunks_mutex);
    return m_chunks.emplace(chunk->get_id(), std::move(chunk)).second;
}

bool WorldStreamingState::erase_chunk(ChunkID id) {
    std::unique_lock<std::shared_mutex> lock(m_chunks_mutex);
    return m_chunks.erase(id) > 0;
}

bool WorldStreamingState::erase_chunk(const IVec3& coords) {
    return erase_chunk(Chunk::calculate_id(coords));
}

void WorldStreamingState::clear() {
    std::unique_lock<std::shared_mutex> lock(m_chunks_mutex);
    m_chunks.clear();
}

bool WorldStreamingState::contains_chunk(ChunkID id) const {
    std::shared_lock<std::shared_mutex> lock(m_chunks_mutex);
    return m_chunks.find(id) != m_chunks.end();
}

bool WorldStreamingState::contains_chunk(const IVec3& coords) const {
    return contains_chunk(Chunk::calculate_id(coords));
}

std::size_t WorldStreamingState::size() const {
    std::shared_lock<std::shared_mutex> lock(m_chunks_mutex);
    return m_chunks.size();
}

bool WorldStreamingState::empty() const {
    std::shared_lock<std::shared_mutex> lock(m_chunks_mutex);
    return m_chunks.empty();
}

std::vector<WorldStreamingState::ChunkPtr> WorldStreamingState::snapshot_chunks() const {
    std::shared_lock<std::shared_mutex> lock(m_chunks_mutex);
    std::vector<ChunkPtr> chunks;
    chunks.reserve(m_chunks.size());

    for (const auto& [id, chunk] : m_chunks) {
        (void)id;
        chunks.push_back(chunk);
    }

    return chunks;
}

std::vector<ChunkID> WorldStreamingState::dirty_chunk_ids() const {
    std::vector<ChunkID> ids;

    {
        std::shared_lock<std::shared_mutex> lock(m_chunks_mutex);
        for (const auto& [id, chunk] : m_chunks) {
            if (chunk && chunk->is_voxel_data_dirty()) {
                ids.push_back(id);
            }
        }
    }

    std::sort(ids.begin(), ids.end());
    return ids;
}

std::vector<WorldStreamingState::ChunkPtr>
WorldStreamingState::snapshot_chunks_with_state(ChunkState state) const {
    std::shared_lock<std::shared_mutex> lock(m_chunks_mutex);
    std::vector<ChunkPtr> chunks;

    for (const auto& [id, chunk] : m_chunks) {
        (void)id;
        if (chunk && chunk->get_state() == state) {
            chunks.push_back(chunk);
        }
    }

    return chunks;
}

} // namespace Luminumbra
