#pragma once

#include "Chunk.h"

#include <cstddef>
#include <memory>
#include <shared_mutex>
#include <unordered_map>
#include <vector>

namespace Luminumbra {

class WorldStreamingState {
public:
    using ChunkPtr = std::shared_ptr<Chunk>;
    using ChunkMap = std::unordered_map<ChunkID, ChunkPtr>;

    WorldStreamingState() = default;
    WorldStreamingState(const WorldStreamingState&) = delete;
    WorldStreamingState& operator=(const WorldStreamingState&) = delete;

    ChunkPtr find_chunk(ChunkID id) const;
    ChunkPtr find_chunk(const IVec3& coords) const;
    ChunkPtr get_or_create_chunk(const IVec3& coords);
    bool insert_chunk(ChunkPtr chunk);
    bool erase_chunk(ChunkID id);
    bool erase_chunk(const IVec3& coords);
    void clear();

    bool contains_chunk(ChunkID id) const;
    bool contains_chunk(const IVec3& coords) const;
    std::size_t size() const;
    bool empty() const;

    std::vector<ChunkPtr> snapshot_chunks() const;
    std::vector<ChunkPtr> snapshot_chunks_with_state(ChunkState state) const;
    // Ids of chunks whose voxel data is dirty (unsaved post-generation edits),
    // sorted ascending. Computed from the chunk flags so there is no separate
    // dirty set to keep in sync.
    std::vector<ChunkID> dirty_chunk_ids() const;

private:
    mutable std::shared_mutex m_chunks_mutex;
    ChunkMap m_chunks;
};

} // namespace Luminumbra
