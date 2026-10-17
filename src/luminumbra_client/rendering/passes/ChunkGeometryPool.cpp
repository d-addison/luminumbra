#include "ChunkGeometryPool.h"
#include "../UploadBudget.h"

#include <algorithm>
#include <cstring>
#include <limits>
#include <string>

namespace Luminumbra::Rendering {

namespace {
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

u32 ChunkGeometryPool::add_block(u32 min_vertices, u32 min_indices, const char* label_seed) {
    Block block;
    block.vertex_capacity = std::max(kPoolBlockVertexCapacity, round_up_pool(min_vertices));
    block.index_capacity = std::max(kPoolBlockIndexCapacity, round_up_pool(min_indices));
    std::size_t checked_bytes = 0;
    if (!mesh_upload_bytes(
            block.vertex_capacity, sizeof(VoxelVertex), block.index_capacity, checked_bytes) ||
        m_blocks.size() >= kMaxBlocks || checked_bytes > kMaxStorageBytes - m_storage_bytes)
        return kInvalid;

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

    const u32 block_index = static_cast<u32>(m_blocks.size());
    if (label_seed != nullptr) {
        const std::string base =
            std::string("terrain.pool.") + label_seed + "." + std::to_string(block_index);
        label_gl_object(GL_VERTEX_ARRAY, block.vao, base + ".vao");
        label_gl_object(GL_BUFFER, block.vbo, base + ".vbo");
        label_gl_object(GL_BUFFER, block.ebo, base + ".ebo");
    }

    m_blocks.push_back(block);
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

    auto take_slice = [](std::vector<std::pair<u32, u32>>& free_slices,
                         u32 high_water,
                         u32 capacity,
                         u32 want,
                         u32& out_offset,
                         u32& out_slot) -> bool {
        // Best-fit over the free list first.
        std::size_t best = free_slices.size();
        for (std::size_t i = 0; i < free_slices.size(); ++i) {
            if (free_slices[i].second >= want &&
                (best == free_slices.size() || free_slices[i].second < free_slices[best].second)) {
                best = i;
            }
        }
        if (best != free_slices.size()) {
            out_offset = free_slices[best].first;
            out_slot = free_slices[best].second; // reuse the whole freed slot
            free_slices[best] = free_slices.back();
            free_slices.pop_back();
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
    if (!take_slice(block.free_vertex_slices,
                    block.vertex_high_water,
                    block.vertex_capacity,
                    want_v,
                    v_off,
                    v_slot)) {
        return false;
    }
    if (!take_slice(block.free_index_slices,
                    block.index_high_water,
                    block.index_capacity,
                    want_i,
                    i_off,
                    i_slot)) {
        // Return the vertex reservation to the free list. A bump reservation
        // must advance the frontier so larger later slices cannot overlap it.
        if (v_off == block.vertex_high_water)
            block.vertex_high_water += v_slot;
        block.free_vertex_slices.emplace_back(v_off, v_slot);
        return false;
    }
    // Commit any bump-frontier advances now that both slices succeeded.
    if (v_off == block.vertex_high_water)
        block.vertex_high_water += v_slot;
    if (i_off == block.index_high_water)
        block.index_high_water += i_slot;

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
    std::size_t checked_bytes = 0;
    if (!vertices || !indices || vertex_count == 0 || index_count == 0 ||
        vertex_count > std::numeric_limits<u32>::max() - (kPoolSlotAlign - 1u) ||
        index_count > std::numeric_limits<u32>::max() - (kPoolSlotAlign - 1u) ||
        !mesh_upload_bytes(vertex_count, sizeof(VoxelVertex), index_count, checked_bytes))
        return kInvalid;
    reclaim();
    if (m_free_handles.empty() && m_allocations.size() >= kMaxAllocations)
        return kInvalid;
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
        block_index = add_block(vertex_count, index_count, label_seed);
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
    // Keep the old slice resident until its replacement has been allocated.
    return replace_upload(
        handle,
        kInvalid,
        [&] { return allocate(vertices, vertex_count, indices, index_count, label_seed); },
        [this](u32 resident) { free(resident); });
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
    m_blocks[block_index].submitted = true;
}

void ChunkGeometryPool::reclaim() {
    // <= eight zero-time polls per upload attempt, charged to its existing
    // preparation/work budget; no wait, world scan or unbounded retirement pass.
    for (std::size_t count = 0; count < kTerrainUploadLimits.count && !m_retired.empty(); ++count) {
        const auto retired = m_retired.front();
        if (!retired.completion)
            break;
        const GLenum status = glClientWaitSync(retired.completion, GL_SYNC_FLUSH_COMMANDS_BIT, 0);
        if (status != GL_ALREADY_SIGNALED && status != GL_CONDITION_SATISFIED)
            break;
        glDeleteSync(retired.completion);
        recycle(retired.handle);
        m_retired.pop_front();
    }
}

void ChunkGeometryPool::recycle(u32 handle) {
    const Allocation& alloc = m_allocations[handle];
    Block& block = m_blocks[alloc.block_index];
    block.free_vertex_slices.emplace_back(alloc.vertex_offset, alloc.vertex_slot);
    block.free_index_slices.emplace_back(alloc.index_offset, alloc.index_slot);
    release_handle(handle);
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
    for (Block& block : m_blocks) {
        glBindVertexArray(0);
        if (block.vao)
            glDeleteVertexArrays(1, &block.vao);
        if (block.vbo)
            glDeleteBuffers(1, &block.vbo);
        if (block.ebo)
            glDeleteBuffers(1, &block.ebo);
    }
    glBindVertexArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    m_blocks.clear();
    m_allocations.clear();
    m_free_handles.clear();
    m_live_count = 0;
    m_storage_bytes = 0;
}

} // namespace Luminumbra::Rendering
