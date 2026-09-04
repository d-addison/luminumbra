#include "ChunkGeometryPool.h"

#include <algorithm>
#include <cstring>
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
// live chunks; the pool grows by adding blocks only when the working set
// outgrows the current blocks. Powers of two keep the bump frontier aligned.
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

    const u32 block_index = static_cast<u32>(m_blocks.size());
    if (label_seed != nullptr) {
        const std::string base =
            std::string("terrain.pool.") + label_seed + "." + std::to_string(block_index);
        label_gl_object(GL_VERTEX_ARRAY, block.vao, base + ".vao");
        label_gl_object(GL_BUFFER, block.vbo, base + ".vbo");
        label_gl_object(GL_BUFFER, block.ebo, base + ".ebo");
    }

    m_blocks.push_back(block);
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
            free_slices.erase(free_slices.begin() + static_cast<std::ptrdiff_t>(best));
            return true;
        }
        // Otherwise bump-allocate from the frontier.
        if (high_water + want <= capacity) {
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
        // Roll back the vertex reservation: return it to the free list rather
        // than leaking it (do not touch the bump frontier here).
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
    if (vertex_count == 0 || index_count == 0)
        return kInvalid;
    // A fresh block always grows to host any single mesh (add_block uses
    // std::max(block_cap, rounded_request)), so allocation cannot fail on size.

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
    if (handle == kInvalid || handle >= m_allocations.size() || !m_allocations[handle].live) {
        return allocate(vertices, vertex_count, indices, index_count, label_seed);
    }
    Allocation& alloc = m_allocations[handle];
    // In-place overwrite when the new geometry fits the reserved slots.
    if (vertex_count <= alloc.vertex_slot && index_count <= alloc.index_slot && vertex_count != 0 &&
        index_count != 0) {
        Block& block = m_blocks[alloc.block_index];
        std::memcpy(block.vertex_ptr + alloc.vertex_offset,
                    vertices,
                    static_cast<std::size_t>(vertex_count) * sizeof(VoxelVertex));
        std::memcpy(block.index_ptr + alloc.index_offset,
                    indices,
                    static_cast<std::size_t>(index_count) * sizeof(u32));
        alloc.vertex_count = vertex_count;
        alloc.index_count = index_count;
        return handle;
    }
    // Outgrew the slot: free and reallocate (handle changes).
    free(handle);
    return allocate(vertices, vertex_count, indices, index_count, label_seed);
}

void ChunkGeometryPool::free(u32 handle) {
    if (handle == kInvalid || handle >= m_allocations.size() || !m_allocations[handle].live) {
        return;
    }
    Allocation& alloc = m_allocations[handle];
    Block& block = m_blocks[alloc.block_index];
    // Return the reserved slots to the block free lists for best-fit reuse.
    block.free_vertex_slices.emplace_back(alloc.vertex_offset, alloc.vertex_slot);
    block.free_index_slices.emplace_back(alloc.index_offset, alloc.index_slot);
    release_handle(handle);
    --m_live_count;
}

void ChunkGeometryPool::resident_capacity(std::size_t& vertices, std::size_t& indices) const {
    for (const Allocation& alloc : m_allocations) {
        if (alloc.live) {
            vertices += alloc.vertex_slot;
            indices += alloc.index_slot;
        }
    }
}

void ChunkGeometryPool::destroy() {
    for (Block& block : m_blocks) {
        if (block.vbo) {
            glBindBuffer(GL_ARRAY_BUFFER, block.vbo);
            glUnmapBuffer(GL_ARRAY_BUFFER);
        }
        if (block.ebo) {
            glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, block.ebo);
            glUnmapBuffer(GL_ELEMENT_ARRAY_BUFFER);
        }
        if (block.vao)
            glDeleteVertexArrays(1, &block.vao);
        if (block.vbo)
            glDeleteBuffers(1, &block.vbo);
        if (block.ebo)
            glDeleteBuffers(1, &block.ebo);
    }
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
    m_blocks.clear();
    m_allocations.clear();
    m_free_handles.clear();
    m_live_count = 0;
}

} // namespace Luminumbra::Rendering
