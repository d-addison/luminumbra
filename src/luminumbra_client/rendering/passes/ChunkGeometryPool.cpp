#include "ChunkGeometryPool.h"
#include "../UploadBudget.h"

#include <algorithm>
#include <cstring>
#include <limits>
#include <string>

namespace Luminumbra::Rendering {

namespace {
// GL has a finite set of sticky error flags. Drain them before any mapped
// write; no GL operation is permitted between the two copies and publication.
bool pool_gl_error() {
    bool failed = false;
    for (unsigned flag = 0; flag < 8; ++flag) {
        if (glGetError() == GL_NO_ERROR)
            break;
        failed = true;
    }
    return failed;
}

bool is_valid_gl_object_name(GLenum identifier, GLuint name) {
    switch (identifier) {
        case GL_BUFFER:
            return glIsBuffer(name) == GL_TRUE;
        case GL_FRAMEBUFFER:
            return glIsFramebuffer(name) == GL_TRUE;
        case GL_PROGRAM:
            return glIsProgram(name) == GL_TRUE;
        case GL_QUERY:
            return glIsQuery(name) == GL_TRUE;
        case GL_RENDERBUFFER:
            return glIsRenderbuffer(name) == GL_TRUE;
        case GL_TEXTURE:
            return glIsTexture(name) == GL_TRUE;
        case GL_VERTEX_ARRAY:
            return glIsVertexArray(name) == GL_TRUE;
        default:
            return true;
    }
}

void label_gl_object(GLenum identifier, GLuint name, const std::string& label) {
    if (name == 0) {
        return;
    }
    if (!is_valid_gl_object_name(identifier, name)) {
        return;
    }
#ifdef GL_VERSION_4_3
    if (glObjectLabel) {
        glObjectLabel(identifier, name, -1, label.c_str());
    }
#else
    (void)identifier;
    (void)label;
#endif
}
} // namespace

// ===========================================================================
// ChunkGeometryPool - bucketed persistent-mapped geometry pool.
// ===========================================================================
namespace {
// Per-block capacities. A 16^3 marching-cubes chunk produces at most a few
// thousand vertices, so a 1M-vertex / 2M-index block (~36 MB) hosts hundreds of
// live chunks; growth includes slices awaiting completion as well as the live
// working set. Powers of two keep the bump frontier aligned.
constexpr u32 kPoolBlockVertexCapacity = 1u << 20; // 1,048,576 vertices (~28 MB)
constexpr u32 kPoolBlockIndexCapacity = 1u << 21;  // 2,097,152 indices  (~8 MB)
// Round a slot size up to keep slices loosely aligned and reduce free-list
// fragmentation when a chunk re-meshes to a slightly different size.
constexpr u32 kPoolSlotAlign = 64u;
inline u32 round_up_pool(u32 v) {
    return (v + (kPoolSlotAlign - 1u)) & ~(kPoolSlotAlign - 1u);
}

// Pack {block_index, allocation_index} is unnecessary: the handle IS the index
// into m_allocations, and the Allocation stores block_index. Keep handles small.
} // namespace

u32 ChunkGeometryPool::acquire_handle() {
    if (!m_free_handles.empty()) {
        const u32 h = m_free_handles.back();
        m_free_handles.pop_back();
        return h;
    }
    m_allocations.emplace_back();
    return static_cast<u32>(m_allocations.size() - 1u);
}

void ChunkGeometryPool::release_handle(u32 handle) {
    m_allocations[handle] = Allocation{};
    m_free_handles.push_back(handle);
}

u32 ChunkGeometryPool::add_block(u32 min_vertices,
                                 u32 min_indices,
                                 const char* label_seed,
                                 bool& capacity_refused) {
    Block block;
    block.vertex_capacity = std::max(kPoolBlockVertexCapacity, round_up_pool(min_vertices));
    block.index_capacity = std::max(kPoolBlockIndexCapacity, round_up_pool(min_indices));
    std::size_t checked_bytes = 0;
    if (!mesh_upload_bytes(
            block.vertex_capacity, sizeof(VoxelVertex), block.index_capacity, checked_bytes) ||
        checked_bytes > kMaxStorageBytes) {
        capacity_refused = true;
        return kInvalid;
    }

    // At most kMaxBlocks candidates. Empty blocks may be replaced in place;
    // never compact the vector or change another allocation's block index.
    u32 block_index = static_cast<u32>(m_blocks.size());
    std::size_t reclaimable_bytes = 0;
    for (u32 index = 0; index < m_blocks.size(); ++index) {
        const Block& candidate = m_blocks[index];
        if (candidate.allocation_count != 0)
            continue;
        if (block_index == m_blocks.size())
            block_index = index;
        reclaimable_bytes +=
            static_cast<std::size_t>(candidate.vertex_capacity) * sizeof(VoxelVertex) +
            static_cast<std::size_t>(candidate.index_capacity) * sizeof(u32);
    }
    if ((block_index == m_blocks.size() && m_blocks.size() >= kMaxBlocks) ||
        checked_bytes > kMaxStorageBytes - m_storage_bytes + reclaimable_bytes) {
        capacity_refused = true;
        return kInvalid;
    }
    if (block_index < m_blocks.size())
        release_block(m_blocks[block_index]);
    for (Block& candidate : m_blocks) {
        if (checked_bytes <= kMaxStorageBytes - m_storage_bytes)
            break;
        if (candidate.allocation_count == 0)
            release_block(candidate);
    }

    const GLbitfield storage_flags = GL_MAP_WRITE_BIT | GL_MAP_PERSISTENT_BIT | GL_MAP_COHERENT_BIT;
    const GLbitfield map_flags = GL_MAP_WRITE_BIT | GL_MAP_PERSISTENT_BIT | GL_MAP_COHERENT_BIT;

    glGenVertexArrays(1, &block.vao);
    glGenBuffers(1, &block.vbo);
    glGenBuffers(1, &block.ebo);

    const std::size_t vbytes =
        static_cast<std::size_t>(block.vertex_capacity) * sizeof(VoxelVertex);
    const std::size_t ibytes = static_cast<std::size_t>(block.index_capacity) * sizeof(u32);

    glBindVertexArray(block.vao);

    glBindBuffer(GL_ARRAY_BUFFER, block.vbo);
    glBufferStorage(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(vbytes), nullptr, storage_flags);
    block.vertex_ptr = static_cast<VoxelVertex*>(
        glMapBufferRange(GL_ARRAY_BUFFER, 0, static_cast<GLsizeiptr>(vbytes), map_flags));

    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, block.ebo);
    glBufferStorage(
        GL_ELEMENT_ARRAY_BUFFER, static_cast<GLsizeiptr>(ibytes), nullptr, storage_flags);
    block.index_ptr = static_cast<u32*>(
        glMapBufferRange(GL_ELEMENT_ARRAY_BUFFER, 0, static_cast<GLsizeiptr>(ibytes), map_flags));

    // Per-vertex VoxelVertex attributes via separate-format buffer binding 0
    // (binding 0 = the geometry VBO). 0 = position vec3, 1 = normal vec3,
    // 2 = material_id uint -- identical layout to the legacy per-chunk VAO.
    glBindVertexBuffer(0, block.vbo, 0, sizeof(VoxelVertex));
    glEnableVertexAttribArray(0);
    glVertexAttribFormat(0, 3, GL_FLOAT, GL_FALSE, offsetof(VoxelVertex, position));
    glVertexAttribBinding(0, 0);
    glEnableVertexAttribArray(1);
    glVertexAttribFormat(1, 3, GL_FLOAT, GL_FALSE, offsetof(VoxelVertex, normal));
    glVertexAttribBinding(1, 0);
    glEnableVertexAttribArray(2);
    glVertexAttribIFormat(2, 1, GL_UNSIGNED_INT, offsetof(VoxelVertex, material_id));
    glVertexAttribBinding(2, 0);

    // per-DRAW chunk world origin as an instanced attribute (location 3,
    // vec3) on buffer binding 1 with divisor 1. With instanceCount==1 and a
    // per-draw baseInstance, the GL fetches origins[baseInstance] for every
    // vertex of that draw -- this is how each MDI draw gets its chunk origin
    // without a per-draw uniform, and it is portable to GL 4.3 (unlike
    // gl_BaseInstance in GLSL, which is core only in 4.6). The actual origin
    // buffer is the per-frame ring buffer, bound to binding 1 at draw time.
    glEnableVertexAttribArray(3);
    glVertexAttribFormat(3, 3, GL_FLOAT, GL_FALSE, 0);
    glVertexAttribBinding(3, 1);
    glVertexBindingDivisor(1, 1);

    glBindVertexArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);

    if (label_seed != nullptr) {
        const std::string base =
            std::string("terrain.pool.") + label_seed + "." + std::to_string(block_index);
        label_gl_object(GL_VERTEX_ARRAY, block.vao, base + ".vao");
        label_gl_object(GL_BUFFER, block.vbo, base + ".vbo");
        label_gl_object(GL_BUFFER, block.ebo, base + ".ebo");
    }

    // A failed mapping must never enter the pool or reach a memcpy. Release
    // successful partial mappings and all object names before retrying later.
    const auto allocation_error = glGetError();
    if (!block.vertex_ptr || !block.index_ptr || allocation_error != GL_NO_ERROR || !block.vao ||
        !block.vbo || !block.ebo) {
        if (block.vertex_ptr) {
            glBindBuffer(GL_ARRAY_BUFFER, block.vbo);
            glUnmapBuffer(GL_ARRAY_BUFFER);
        }
        if (block.index_ptr) {
            glBindVertexArray(block.vao);
            glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, block.ebo);
            glUnmapBuffer(GL_ELEMENT_ARRAY_BUFFER);
        }
        glBindVertexArray(0);
        glBindBuffer(GL_ARRAY_BUFFER, 0);
        glDeleteVertexArrays(1, &block.vao);
        glDeleteBuffers(1, &block.vbo);
        glDeleteBuffers(1, &block.ebo);
        // Consume the failed allocation's error, including a null mapping.
        // GL exposes a finite set of error flags; each read clears one.
        for (unsigned error = 0; error < 8 && glGetError() != GL_NO_ERROR; ++error) {}
        return kInvalid;
    }

    if (block_index == m_blocks.size())
        m_blocks.push_back(std::move(block));
    else
        m_blocks[block_index] = std::move(block);
    m_storage_bytes += checked_bytes;
    return block_index;
}

bool ChunkGeometryPool::reserve_in_block(Block& block,
                                         u32 vertex_count,
                                         u32 index_count,
                                         u32& vertex_offset,
                                         u32& vertex_slot,
                                         u32& index_offset,
                                         u32& index_slot) {
    const u32 want_v = round_up_pool(vertex_count);
    const u32 want_i = round_up_pool(index_count);

    auto find_slice = [](const std::vector<std::pair<u32, u32>>& free_slices,
                         u32 high_water,
                         u32 capacity,
                         u32 want,
                         u32& out_offset,
                         u32& out_slot,
                         std::size_t& best) -> bool {
        // Best-fit over at most kMaxAllocations returned slots. Plan both
        // reservations before changing either list/frontier.
        best = free_slices.size();
        for (std::size_t i = 0; i < free_slices.size(); ++i) {
            if (free_slices[i].second >= want &&
                (best == free_slices.size() || free_slices[i].second < free_slices[best].second)) {
                best = i;
            }
        }
        if (best != free_slices.size()) {
            out_offset = free_slices[best].first;
            out_slot = free_slices[best].second; // reuse the whole freed slot
            return true;
        }
        // Otherwise bump-allocate from the frontier.
        if (high_water <= capacity && want <= capacity - high_water) {
            out_offset = high_water;
            out_slot = want;
            return true;
        }
        return false;
    };

    u32 v_off = 0, v_slot = 0, i_off = 0, i_slot = 0;
    std::size_t vertex_slice = 0, index_slice = 0;
    if (!find_slice(block.free_vertex_slices,
                    block.vertex_high_water,
                    block.vertex_capacity,
                    want_v,
                    v_off,
                    v_slot,
                    vertex_slice)) {
        return false;
    }
    if (!find_slice(block.free_index_slices,
                    block.index_high_water,
                    block.index_capacity,
                    want_i,
                    i_off,
                    i_slot,
                    index_slice)) {
        return false;
    }
    // Commit only after both reservations succeeded; refusal leaves the block
    // unchanged, including a completely reclaimed block's empty frontier.
    if (vertex_slice < block.free_vertex_slices.size()) {
        block.free_vertex_slices[vertex_slice] = block.free_vertex_slices.back();
        block.free_vertex_slices.pop_back();
    } else {
        block.vertex_high_water += v_slot;
    }
    if (index_slice < block.free_index_slices.size()) {
        block.free_index_slices[index_slice] = block.free_index_slices.back();
        block.free_index_slices.pop_back();
    } else {
        block.index_high_water += i_slot;
    }

    vertex_offset = v_off;
    vertex_slot = v_slot;
    index_offset = i_off;
    index_slot = i_slot;
    return true;
}

u32 ChunkGeometryPool::allocate(const VoxelVertex* vertices,
                                u32 vertex_count,
                                const u32* indices,
                                u32 index_count,
                                const char* label_seed) {
    bool capacity_refused = false;
    return allocate_with_reclaim(vertices,
                                 vertex_count,
                                 indices,
                                 index_count,
                                 label_seed,
                                 kTerrainUploadLimits.count,
                                 capacity_refused);
}

u32 ChunkGeometryPool::allocate_with_reclaim(const VoxelVertex* vertices,
                                             u32 vertex_count,
                                             const u32* indices,
                                             u32 index_count,
                                             const char* label_seed,
                                             std::size_t visits,
                                             bool& capacity_refused) {
    capacity_refused = false;
    std::size_t checked_bytes = 0;
    if (!vertices || !indices || vertex_count == 0 || index_count == 0 ||
        vertex_count > std::numeric_limits<u32>::max() - (kPoolSlotAlign - 1u) ||
        index_count > std::numeric_limits<u32>::max() - (kPoolSlotAlign - 1u) ||
        !mesh_upload_bytes(vertex_count, sizeof(VoxelVertex), index_count, checked_bytes))
        return kInvalid;
    if (pool_gl_error())
        return kInvalid;
    reclaim(visits);
    if (pool_gl_error())
        return kInvalid;
    if (m_free_handles.empty() && m_allocations.size() >= kMaxAllocations) {
        capacity_refused = true;
        return kInvalid;
    }
    // Storage and handle caps include retired slices. Pressure fails closed;
    // the caller retains its resident and retries through the upload budget.

    u32 vertex_offset = 0, vertex_slot = 0, index_offset = 0, index_slot = 0;
    u32 block_index = kInvalid;
    for (u32 b = 0; b < static_cast<u32>(m_blocks.size()); ++b) {
        if (reserve_in_block(m_blocks[b],
                             vertex_count,
                             index_count,
                             vertex_offset,
                             vertex_slot,
                             index_offset,
                             index_slot)) {
            block_index = b;
            break;
        }
    }
    if (block_index == kInvalid) {
        block_index = add_block(vertex_count, index_count, label_seed, capacity_refused);
        if (block_index == kInvalid)
            return kInvalid;
        if (!reserve_in_block(m_blocks[block_index],
                              vertex_count,
                              index_count,
                              vertex_offset,
                              vertex_slot,
                              index_offset,
                              index_slot)) {
            return kInvalid; // a fresh block could not host it -> hard failure
        }
    }

    Block& block = m_blocks[block_index];
    std::memcpy(block.vertex_ptr + vertex_offset,
                vertices,
                static_cast<std::size_t>(vertex_count) * sizeof(VoxelVertex));
    std::memcpy(block.index_ptr + index_offset,
                indices,
                static_cast<std::size_t>(index_count) * sizeof(u32));

    const u32 handle = acquire_handle();
    Allocation& alloc = m_allocations[handle];
    alloc.block_index = block_index;
    alloc.vertex_offset = vertex_offset;
    alloc.vertex_count = vertex_count;
    alloc.vertex_slot = vertex_slot;
    alloc.index_offset = index_offset;
    alloc.index_count = index_count;
    alloc.index_slot = index_slot;
    alloc.live = true;
    ++block.allocation_count;
    ++m_live_count;
    return handle;
}

u32 ChunkGeometryPool::update(u32 handle,
                              const VoxelVertex* vertices,
                              u32 vertex_count,
                              const u32* indices,
                              u32 index_count,
                              const char* label_seed) {
    if (!vertices || !indices || vertex_count == 0 || index_count == 0)
        return kInvalid;
    if (handle == kInvalid || handle >= m_allocations.size() || !m_allocations[handle].live) {
        return allocate(vertices, vertex_count, indices, index_count, label_seed);
    }
    // Leave one of the eight zero-time polls for pressure reuse. Graphics and
    // input failures must never fall through to a mapped overwrite.
    bool capacity_refused = false;
    const u32 replacement = allocate_with_reclaim(vertices,
                                                  vertex_count,
                                                  indices,
                                                  index_count,
                                                  label_seed,
                                                  kTerrainUploadLimits.count - 1,
                                                  capacity_refused);
    if (replacement == kInvalid) {
        return capacity_refused
                   ? reuse_completed(handle, vertices, vertex_count, indices, index_count)
                   : kInvalid;
    }

    // A retirement fence is part of the transaction too. On failure, discard
    // only the unpublished slice; it has never had a GPU consumer.
    const auto block_index = m_allocations[handle].block_index;
    GLsync completion =
        m_blocks[block_index].submitted ? glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0) : nullptr;
    const bool graphics_failed = pool_gl_error();
    if (graphics_failed || (m_blocks[block_index].submitted && !completion)) {
        if (completion)
            glDeleteSync(completion);
        m_allocations[replacement].live = false;
        --m_live_count;
        recycle(replacement);
        return kInvalid;
    }
    m_allocations[handle].live = false;
    --m_live_count;
    if (completion)
        m_retired.push_back({handle, completion});
    else
        recycle(handle);
    return replacement;
}

void ChunkGeometryPool::fence_reuse(Block& block) {
    if (block.reuse_completion)
        glDeleteSync(block.reuse_completion);
    block.reuse_completion = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
    const bool graphics_failed = pool_gl_error();
    block.reuse_quarantined = graphics_failed || !block.reuse_completion;
}

bool ChunkGeometryPool::complete_for_reuse(Block& block) {
    if (block.reuse_quarantined)
        return false;
    if (!block.submitted)
        return !pool_gl_error();
    if (!block.reuse_requested) {
        block.reuse_requested = true;
        fence_reuse(block);
    }
    if (block.reuse_quarantined)
        return false;
    const GLenum status = glClientWaitSync(block.reuse_completion, GL_SYNC_FLUSH_COMMANDS_BIT, 0);
    const bool graphics_failed = pool_gl_error();
    if (graphics_failed || status == GL_WAIT_FAILED) {
        block.reuse_quarantined = true;
        return false;
    }
    if (status != GL_ALREADY_SIGNALED && status != GL_CONDITION_SATISFIED)
        return false;
    glDeleteSync(block.reuse_completion);
    block.reuse_completion = nullptr;
    block.reuse_requested = false;
    if (pool_gl_error()) {
        block.reuse_quarantined = true;
        return false;
    }
    return true;
}

u32 ChunkGeometryPool::reuse_completed(u32 handle,
                                       const VoxelVertex* vertices,
                                       u32 vertex_count,
                                       const u32* indices,
                                       u32 index_count) {
    Allocation& alloc = m_allocations[handle];
    Block& block = m_blocks[alloc.block_index];
    const u32 vertex_slot = std::max(alloc.vertex_slot, round_up_pool(vertex_count));
    const u32 index_slot = std::max(alloc.index_slot, round_up_pool(index_count));
    // A slot can extend only into its adjacent unused bump tail. Plan BOTH
    // capacities before polling or writing; no other live/retired slot moves.
    const auto fits = [](u32 offset, u32 old_slot, u32 new_slot, u32 frontier, u32 capacity) {
        return new_slot == old_slot ||
               (offset <= capacity && old_slot <= capacity - offset &&
                offset + old_slot == frontier && new_slot <= capacity - offset);
    };
    if (!fits(alloc.vertex_offset,
              alloc.vertex_slot,
              vertex_slot,
              block.vertex_high_water,
              block.vertex_capacity) ||
        !fits(alloc.index_offset,
              alloc.index_slot,
              index_slot,
              block.index_high_water,
              block.index_capacity) ||
        !complete_for_reuse(block))
        return kInvalid;

    // All fallible GL work is finished. These coherent mapped stores and the
    // record update run on the context thread, with no intervening GL calls,
    // allocation, callback or consumer. Thus neither buffer can fail alone.
    std::memcpy(block.vertex_ptr + alloc.vertex_offset,
                vertices,
                static_cast<std::size_t>(vertex_count) * sizeof(VoxelVertex));
    std::memcpy(block.index_ptr + alloc.index_offset,
                indices,
                static_cast<std::size_t>(index_count) * sizeof(u32));
    block.vertex_high_water += vertex_slot - alloc.vertex_slot;
    block.index_high_water += index_slot - alloc.index_slot;
    alloc.vertex_slot = vertex_slot;
    alloc.index_slot = index_slot;
    alloc.vertex_count = vertex_count;
    alloc.index_count = index_count;
    return handle;
}

void ChunkGeometryPool::free(u32 handle) {
    if (handle == kInvalid || handle >= m_allocations.size() || !m_allocations[handle].live) {
        return;
    }
    Allocation& alloc = m_allocations[handle];
    alloc.live = false;
    --m_live_count;
    if (!m_blocks[alloc.block_index].submitted) {
        recycle(handle);
        return;
    }
    // Insert after the final submitted consumer, never at upload time. A null
    // fence remains quarantined, as does GL_WAIT_FAILED in reclaim().
    m_retired.push_back({handle, glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0)});
}

void ChunkGeometryPool::mark_submitted(u32 block_index) {
    Block& block = m_blocks[block_index];
    block.submitted = true;
    // Replay/shadow/MDI consumers arriving after a refused update invalidate
    // its earlier completion. Keep only the final consumer's fence (<=16).
    if (block.reuse_requested && !block.reuse_quarantined)
        fence_reuse(block);
}

void ChunkGeometryPool::reclaim() {
    reclaim(kTerrainUploadLimits.count);
}

void ChunkGeometryPool::reclaim(std::size_t limit) {
    // <= eight zero-time polls per upload attempt, charged to its existing
    // preparation/work budget; no wait, world scan or unbounded retirement pass.
    const auto visits = std::min(limit, m_retired.size());
    for (std::size_t count = 0; count < visits; ++count) {
        const auto retired = m_retired.front();
        m_retired.pop_front();
        if (!retired.completion) {
            m_retired.push_back(retired);
            continue;
        }
        const GLenum status = glClientWaitSync(retired.completion, GL_SYNC_FLUSH_COMMANDS_BIT, 0);
        if (status != GL_ALREADY_SIGNALED && status != GL_CONDITION_SATISFIED) {
            m_retired.push_back(retired);
            continue;
        }
        glDeleteSync(retired.completion);
        recycle(retired.handle);
    }
}

void ChunkGeometryPool::recycle(u32 handle) {
    const Allocation& alloc = m_allocations[handle];
    Block& block = m_blocks[alloc.block_index];
    if (--block.allocation_count == 0) {
        block.vertex_high_water = 0;
        block.index_high_water = 0;
        block.free_vertex_slices.clear();
        block.free_index_slices.clear();
        if (block.reuse_completion)
            glDeleteSync(block.reuse_completion);
        block.reuse_completion = nullptr;
        block.reuse_requested = false;
        block.reuse_quarantined = false;
    } else {
        block.free_vertex_slices.emplace_back(alloc.vertex_offset, alloc.vertex_slot);
        block.free_index_slices.emplace_back(alloc.index_offset, alloc.index_slot);
    }
    release_handle(handle);
}

void ChunkGeometryPool::release_block(Block& block) {
    // Reuse requires zero allocations, including quarantined retirements.
    // Final teardown instead relies on GL retaining queued backing stores.
    // Keep the vector slot for stable indices.
    m_storage_bytes -= static_cast<std::size_t>(block.vertex_capacity) * sizeof(VoxelVertex) +
                       static_cast<std::size_t>(block.index_capacity) * sizeof(u32);
    if (block.reuse_completion)
        glDeleteSync(block.reuse_completion);
    glBindVertexArray(0);
    if (block.vao)
        glDeleteVertexArrays(1, &block.vao);
    if (block.vbo)
        glDeleteBuffers(1, &block.vbo);
    if (block.ebo)
        glDeleteBuffers(1, &block.ebo);
    block = Block{};
}

void ChunkGeometryPool::resident_capacity(std::size_t& vertices, std::size_t& indices) const {
    for (const Allocation& alloc : m_allocations) {
        if (alloc.live) {
            vertices += alloc.vertex_slot;
            indices += alloc.index_slot;
        }
    }
}

void ChunkGeometryPool::clear() {
    // World-swap work is bounded by kMaxAllocations, including retired handles.
    // Keep pending storage charged to the same caps across successive worlds.
    for (u32 handle = 0; handle < m_allocations.size(); ++handle)
        free(handle);
}

void ChunkGeometryPool::destroy() {
    for (const auto& retired : m_retired) {
        if (retired.completion)
            glDeleteSync(retired.completion);
    }
    m_retired.clear();
    // Delete objects without writing their mappings. GL retains backing stores
    // referenced by queued draws; world swaps never recycle those stores.
    for (Block& block : m_blocks)
        release_block(block);
    glBindVertexArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    m_blocks.clear();
    m_allocations.clear();
    m_free_handles.clear();
    m_live_count = 0;
    m_storage_bytes = 0;
}

} // namespace Luminumbra::Rendering
