#pragma once

#include "../../../include/luminumbra/core/Types.h"
#include "../Mesh.h"
#include "luminumbra_common/world/Chunk.h"
#include <cstddef>
#include <deque>
#include <utility>
#include <vector>

namespace Luminumbra::Rendering {

// bucketed persistent-mapped geometry pool for live terrain chunks.
//
// Replaces the one-VBO/EBO/VAO-per-chunk model with a small set of large,
// immutable, persistently+coherently mapped GL buffers ("blocks"). Each block
// owns a vertex buffer, an index buffer, and a VAO with the VoxelVertex
// attribute layout pre-bound, and acts as ONE glMultiDrawElementsIndirect
// bucket. A chunk's geometry is sub-allocated as a contiguous vertex slice and
// a contiguous index slice inside a single block (a free-list suballocator per
// block, best-fit). Uploads memcpy straight into the persistent mapping (no
// glBufferSubData / driver staging copy). Draw submission builds one indirect
// command per visible chunk (firstIndex/baseVertex into the block) plus a
// per-draw chunk origin, and issues one MDI per block that has visible chunks.
//
// GL floor: glBufferStorage + GL_MAP_PERSISTENT_BIT (GL 4.4) and
// glMultiDrawElementsIndirect (GL 4.3). The engine requests a 4.5 core context
// (main_client.cpp), so both are guaranteed; no runtime capability fallback.
class ChunkGeometryPool {
public:
    static constexpr u32 kInvalid = 0xFFFFFFFFu;
    // Bound storage, handles and sync objects even if the GPU never completes.
    static constexpr std::size_t kMaxStorageBytes = 512u * 1024u * 1024u;
    static constexpr std::size_t kMaxBlocks = 16;
    static constexpr std::size_t kMaxAllocations = 65536;

    // One per-chunk allocation record. block_index selects the GL block; the
    // vertex/index slices are expressed in ELEMENTS (vertices / indices), so the
    // draw command derives baseVertex = vertex_offset and firstIndex =
    // index_offset directly.
    struct Allocation {
        u32 block_index = 0;
        u32 vertex_offset = 0; // first vertex (baseVertex)
        u32 vertex_count = 0;  // vertices written
        u32 vertex_slot = 0;   // capacity of the reserved vertex slot
        u32 index_offset = 0;  // first index (firstIndex)
        u32 index_count = 0;   // indices written (drawn element_count)
        u32 index_slot = 0;    // capacity of the reserved index slot
        bool live = false;
    };

    // GL handles + persistent pointers + free lists for one pool block.
    struct Block {
        u32 vao = 0;
        u32 vbo = 0;
        u32 ebo = 0;
        VoxelVertex* vertex_ptr = nullptr; // persistent+coherent mapping
        u32* index_ptr = nullptr;
        u32 vertex_capacity = 0;   // vertices
        u32 index_capacity = 0;    // indices
        u32 vertex_high_water = 0; // bump allocator frontier
        u32 index_high_water = 0;
        bool submitted = false; // a draw may still consume this block
        // Free slices returned by freed chunks, reused best-fit before bumping.
        std::vector<std::pair<u32, u32>> free_vertex_slices; // {offset, size}
        std::vector<std::pair<u32, u32>> free_index_slices;
    };

    ChunkGeometryPool() = default;

    // Allocates a slice for {vertex_count, index_count}, growing the pool with a
    // fresh block if no existing block can host it. Writes the geometry into the
    // persistent mapping. Returns a handle, or kInvalid on failure (e.g. a mesh
    // larger than a whole block). label_seed feeds GL debug labels.
    u32 allocate(const VoxelVertex* vertices,
                 u32 vertex_count,
                 const u32* indices,
                 u32 index_count,
                 const char* label_seed);
    // Always stages replacement geometry in a different slice. Coherent mapping
    // does not permit overwriting bytes still consumed by a previous draw.
    // Failure returns kInvalid and leaves the resident slice untouched.
    u32 update(u32 handle,
               const VoxelVertex* vertices,
               u32 vertex_count,
               const u32* indices,
               u32 index_count,
               const char* label_seed);
    void free(u32 handle);
    // Call immediately after every draw consuming this block, including replay
    // and shadow draws. Retirement fences cover all earlier commands in context.
    void mark_submitted(u32 block_index);
    // Poll at most one upload lane's count limit, with zero timeout. Failed or
    // missing fences quarantine slices until destroy; no speculative reuse.
    void reclaim();
    // Const diagnostics for storage/retirement pressure.
    std::size_t retired_count() const {
        return m_retired.size();
    }

    bool valid_allocation(u32 handle, u32 indices) const {
        return handle < m_allocations.size() && m_allocations[handle].live &&
               m_allocations[handle].index_count == indices && indices != 0;
    }
    const Allocation& allocation(u32 handle) const {
        return m_allocations[handle];
    }
    const std::vector<Block>& blocks() const {
        return m_blocks;
    }
    std::size_t block_count() const {
        return m_blocks.size();
    }
    std::size_t live_allocation_count() const {
        return m_live_count;
    }

    // Sum of reserved vertex/index slot capacities across live allocations
    // (feeds the runtime VRAM estimate, matching the old per-chunk accounting).
    void resident_capacity(std::size_t& vertices, std::size_t& indices) const;

    // Retire a world's live slices without resetting storage/retirement caps.
    void clear();
    // Final teardown only: delete GL objects; queued consumers retain backing
    // storage under GL object lifetime rules. No later world reuses these bytes.
    void destroy();
    bool empty() const {
        return m_blocks.empty();
    }

private:
    u32 acquire_handle();
    void release_handle(u32 handle);
    bool reserve_in_block(Block& block,
                          u32 vertex_count,
                          u32 index_count,
                          u32& vertex_offset,
                          u32& vertex_slot,
                          u32& index_offset,
                          u32& index_slot);
    u32 add_block(u32 min_vertices, u32 min_indices, const char* label_seed);

    struct Retired {
        u32 handle = kInvalid;
        GLsync completion = nullptr;
    };
    void recycle(u32 handle);
    std::deque<Retired> m_retired; // <= kMaxAllocations; FIFO, never front-erased
    std::size_t m_storage_bytes = 0;
    std::vector<Block> m_blocks;
    std::vector<Allocation> m_allocations;
    std::vector<u32> m_free_handles;
    std::size_t m_live_count = 0;
};

} // namespace Luminumbra::Rendering
