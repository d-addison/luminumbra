#include "RenderPipeline.h"
#include "luminumbra_common/systems/SHIELD_WorldSystem.h"
#include "luminumbra_common/world/Chunk.h"
#include "core/Log.h"
#include "rendering/Shader.h"
#include "rendering/Camera.h"
#include <algorithm>
#include <unordered_set>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/constants.hpp>
#include <random>
#include <cstring>
#include <exception>
#include <fstream>
#include <limits>
#include <utility>
#include <GLFW/glfw3.h>
#include "Mesh.h"
#include "../../include/luminumbra/core/Types.h"
#include "luminumbra_common/components/CoreComponents.h"
#include <cmath>
#include "luminumbra_common/components/LightingComponents.h"
#include "RenderSystem.h"
#include "FarLodSystem.h"
#include "passes/GBufferPass.h"
#include "passes/LightingPass.h"
#include "passes/ShadowPass.h"
#include "passes/SkyboxPass.h"
#include "passes/SsaoPass.h"
#include "passes/WaterPass.h"
#include <stb_image.h>
#include <nlohmann/json.hpp>

namespace {
// Helper for frustum culling
inline void ExtractFrustumPlanes(const glm::mat4& m, glm::vec4 planes[6]) {
    planes[0] = glm::vec4(m[0][3] + m[0][0], m[1][3] + m[1][0], m[2][3] + m[2][0], m[3][3] + m[3][0]);
    planes[1] = glm::vec4(m[0][3] - m[0][0], m[1][3] - m[1][0], m[2][3] - m[2][0], m[3][3] - m[3][0]);
    planes[2] = glm::vec4(m[0][3] + m[0][1], m[1][3] + m[1][1], m[2][3] + m[2][1], m[3][3] + m[3][1]);
    planes[3] = glm::vec4(m[0][3] - m[0][1], m[1][3] - m[1][1], m[2][3] - m[2][1], m[3][3] - m[3][1]);
    planes[4] = glm::vec4(m[0][3] + m[0][2], m[1][3] + m[1][2], m[2][3] + m[2][2], m[3][3] + m[3][2]);
    planes[5] = glm::vec4(m[0][3] - m[0][2], m[1][3] - m[1][2], m[2][3] - m[2][2], m[3][3] - m[3][2]);
    for (int i = 0; i < 6; ++i) {
        float inv_len = 1.0f / glm::length(glm::vec3(planes[i]));
        planes[i] *= inv_len;
    }
}
inline bool AABBOutsidePlane(const glm::vec3& minp, const glm::vec3& maxp, const glm::vec4& plane) {
    glm::vec3 p = glm::vec3(plane.x >= 0 ? maxp.x : minp.x, plane.y >= 0 ? maxp.y : minp.y, plane.z >= 0 ? maxp.z : minp.z);
    return (glm::dot(glm::vec3(plane), p) + plane.w) < 0.0f;
}
inline bool AABBFrustumCulled(const glm::vec3& minp, const glm::vec3& maxp, const glm::vec4 planes[6]) {
    for (int i = 0; i < 6; ++i) if (AABBOutsidePlane(minp, maxp, planes[i])) return true;
    return false;
}
}

namespace Luminumbra::Rendering {

namespace {

constexpr bool kEnableExperimentalGpuSdfIntegration = false;
constexpr size_t kMaxFreeChunkRenderSlots = 2048;
constexpr size_t kMaxFreeWaterRenderSlots = 1024;
constexpr int kTerrainFallbackTileSize = 32;

void set_default_shadow_cascade_splits(ShadowMap& shadow_map) {
    shadow_map.cascade_splits.resize(ShadowMap::CASCADE_COUNT + 1);
    shadow_map.cascade_splits[0] = 0.1f;
    shadow_map.cascade_splits[1] = 15.0f;
    shadow_map.cascade_splits[2] = 40.0f;
    shadow_map.cascade_splits[3] = 100.0f;
    shadow_map.cascade_splits[4] = 250.0f;
}

bool has_valid_shadow_cascade_splits(const ShadowMap& shadow_map) {
    return shadow_map.cascade_splits.size() >= ShadowMap::CASCADE_COUNT + 1;
}

void delete_chunk_slot(ChunkRenderData& data) {
    if (data.vao_id) { glDeleteVertexArrays(1, &data.vao_id); data.vao_id = 0; }
    if (data.vbo_id) { glDeleteBuffers(1, &data.vbo_id); data.vbo_id = 0; }
    if (data.ebo_id) { glDeleteBuffers(1, &data.ebo_id); data.ebo_id = 0; }
    data = {};
}

void delete_water_slot(WaterRenderData& data) {
    if (data.vao_id) { glDeleteVertexArrays(1, &data.vao_id); data.vao_id = 0; }
    if (data.vbo_id) { glDeleteBuffers(1, &data.vbo_id); data.vbo_id = 0; }
    if (data.ebo_id) { glDeleteBuffers(1, &data.ebo_id); data.ebo_id = 0; }
    data = {};
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

std::vector<unsigned char> make_terrain_fallback_texture(int width, int height, int layer) {
    std::vector<unsigned char> pixels(static_cast<size_t>(width) * static_cast<size_t>(height) * 4u);
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const bool bright = (((x / kTerrainFallbackTileSize) + (y / kTerrainFallbackTileSize) + layer) % 2) == 0;
            const size_t index = (static_cast<size_t>(y) * static_cast<size_t>(width) + static_cast<size_t>(x)) * 4u;
            pixels[index + 0u] = bright ? 255 : 35;
            pixels[index + 1u] = bright ? 0 : 35;
            pixels[index + 2u] = bright ? 220 : 35;
            pixels[index + 3u] = 255;
        }
    }
    return pixels;
}

bool gl_extension_present(const char* extension_name) {
    if (glGetStringi != nullptr) {
        GLint extension_count = 0;
        glGetIntegerv(GL_NUM_EXTENSIONS, &extension_count);
        for (GLint i = 0; i < extension_count; ++i) {
            const char* name = reinterpret_cast<const char*>(glGetStringi(GL_EXTENSIONS, static_cast<GLuint>(i)));
            if (name != nullptr && std::strcmp(name, extension_name) == 0) {
                return true;
            }
        }
        return false;
    }
    // Legacy (pre-3.0) contexts expose the extension list as a single string.
    const char* extensions = reinterpret_cast<const char*>(glGetString(GL_EXTENSIONS));
    return extensions != nullptr && std::strstr(extensions, extension_name) != nullptr;
}

constexpr const char* kGpuTimerPassNames[] = {
    "shadow",
    "gbuffer",
    "ssao",
    "ssao_blur",
    "lighting",
    "water",
    "skybox",
    "final_blit",
};

} // namespace

// ===========================================================================
// T-I4-16: ChunkGeometryPool - bucketed persistent-mapped geometry pool.
// ===========================================================================
namespace {
// Per-block capacities. A 16^3 marching-cubes chunk produces at most a few
// thousand vertices, so a 1M-vertex / 2M-index block (~36 MB) hosts hundreds of
// live chunks; the pool grows by adding blocks only when the working set
// outgrows the current blocks. Powers of two keep the bump frontier aligned.
constexpr u32 kPoolBlockVertexCapacity = 1u << 20; // 1,048,576 vertices (~28 MB)
constexpr u32 kPoolBlockIndexCapacity  = 1u << 21; // 2,097,152 indices  (~8 MB)
// Round a slot size up to keep slices loosely aligned and reduce free-list
// fragmentation when a chunk re-meshes to a slightly different size.
constexpr u32 kPoolSlotAlign = 64u;
inline u32 round_up_pool(u32 v) { return (v + (kPoolSlotAlign - 1u)) & ~(kPoolSlotAlign - 1u); }

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

    const std::size_t vbytes = static_cast<std::size_t>(block.vertex_capacity) * sizeof(VoxelVertex);
    const std::size_t ibytes = static_cast<std::size_t>(block.index_capacity) * sizeof(u32);

    glBindVertexArray(block.vao);

    glBindBuffer(GL_ARRAY_BUFFER, block.vbo);
    glBufferStorage(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(vbytes), nullptr, storage_flags);
    block.vertex_ptr = static_cast<VoxelVertex*>(
        glMapBufferRange(GL_ARRAY_BUFFER, 0, static_cast<GLsizeiptr>(vbytes), map_flags));

    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, block.ebo);
    glBufferStorage(GL_ELEMENT_ARRAY_BUFFER, static_cast<GLsizeiptr>(ibytes), nullptr, storage_flags);
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

    // T-I4-16: per-DRAW chunk world origin as an instanced attribute (location 3,
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
        const std::string base = std::string("terrain.pool.") + label_seed + "." + std::to_string(block_index);
        label_gl_object(GL_VERTEX_ARRAY, block.vao, base + ".vao");
        label_gl_object(GL_BUFFER, block.vbo, base + ".vbo");
        label_gl_object(GL_BUFFER, block.ebo, base + ".ebo");
    }

    m_blocks.push_back(block);
    return block_index;
}

bool ChunkGeometryPool::reserve_in_block(Block& block, u32 vertex_count, u32 index_count,
                                         u32& vertex_offset, u32& vertex_slot,
                                         u32& index_offset, u32& index_slot) {
    const u32 want_v = round_up_pool(vertex_count);
    const u32 want_i = round_up_pool(index_count);

    auto take_slice = [](std::vector<std::pair<u32, u32>>& free_slices, u32 high_water,
                         u32 capacity, u32 want, u32& out_offset, u32& out_slot) -> bool {
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
    if (!take_slice(block.free_vertex_slices, block.vertex_high_water, block.vertex_capacity, want_v, v_off, v_slot)) {
        return false;
    }
    if (!take_slice(block.free_index_slices, block.index_high_water, block.index_capacity, want_i, i_off, i_slot)) {
        // Roll back the vertex reservation: return it to the free list rather
        // than leaking it (do not touch the bump frontier here).
        block.free_vertex_slices.emplace_back(v_off, v_slot);
        return false;
    }
    // Commit any bump-frontier advances now that both slices succeeded.
    if (v_off == block.vertex_high_water) block.vertex_high_water += v_slot;
    if (i_off == block.index_high_water) block.index_high_water += i_slot;

    vertex_offset = v_off; vertex_slot = v_slot;
    index_offset = i_off; index_slot = i_slot;
    return true;
}

u32 ChunkGeometryPool::allocate(const VoxelVertex* vertices, u32 vertex_count,
                                const u32* indices, u32 index_count, const char* label_seed) {
    if (vertex_count == 0 || index_count == 0) return kInvalid;
    // A fresh block always grows to host any single mesh (add_block uses
    // std::max(block_cap, rounded_request)), so allocation cannot fail on size.

    u32 vertex_offset = 0, vertex_slot = 0, index_offset = 0, index_slot = 0;
    u32 block_index = kInvalid;
    for (u32 b = 0; b < static_cast<u32>(m_blocks.size()); ++b) {
        if (reserve_in_block(m_blocks[b], vertex_count, index_count,
                             vertex_offset, vertex_slot, index_offset, index_slot)) {
            block_index = b;
            break;
        }
    }
    if (block_index == kInvalid) {
        block_index = add_block(vertex_count, index_count, label_seed);
        if (!reserve_in_block(m_blocks[block_index], vertex_count, index_count,
                              vertex_offset, vertex_slot, index_offset, index_slot)) {
            return kInvalid; // a fresh block could not host it -> hard failure
        }
    }

    Block& block = m_blocks[block_index];
    std::memcpy(block.vertex_ptr + vertex_offset, vertices, static_cast<std::size_t>(vertex_count) * sizeof(VoxelVertex));
    std::memcpy(block.index_ptr + index_offset, indices, static_cast<std::size_t>(index_count) * sizeof(u32));

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

u32 ChunkGeometryPool::update(u32 handle, const VoxelVertex* vertices, u32 vertex_count,
                              const u32* indices, u32 index_count, const char* label_seed) {
    if (handle == kInvalid || handle >= m_allocations.size() || !m_allocations[handle].live) {
        return allocate(vertices, vertex_count, indices, index_count, label_seed);
    }
    Allocation& alloc = m_allocations[handle];
    // In-place overwrite when the new geometry fits the reserved slots.
    if (vertex_count <= alloc.vertex_slot && index_count <= alloc.index_slot &&
        vertex_count != 0 && index_count != 0) {
        Block& block = m_blocks[alloc.block_index];
        std::memcpy(block.vertex_ptr + alloc.vertex_offset, vertices,
                    static_cast<std::size_t>(vertex_count) * sizeof(VoxelVertex));
        std::memcpy(block.index_ptr + alloc.index_offset, indices,
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
        if (block.vao) glDeleteVertexArrays(1, &block.vao);
        if (block.vbo) glDeleteBuffers(1, &block.vbo);
        if (block.ebo) glDeleteBuffers(1, &block.ebo);
    }
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
    m_blocks.clear();
    m_allocations.clear();
    m_free_handles.clear();
    m_live_count = 0;
}

// --- HIERARCHICAL CULLING IMPLEMENTATION ---

void RenderPipeline::HierarchicalCuller::BuildHierarchy(const std::vector<ChunkMeshSnapshot>& chunks) {
    if (chunks.empty()) {
        m_root.reset();
        return;
    }

    std::vector<ChunkCullEntry> chunk_refs;
    chunk_refs.reserve(chunks.size());
    for (const auto& chunk : chunks) {
        glm::vec3 chunk_min(
            static_cast<float>(chunk.coords.x * CHUNK_SIZE_X),
            static_cast<float>(chunk.coords.y * CHUNK_SIZE_Y),
            static_cast<float>(chunk.coords.z * CHUNK_SIZE_Z));
        glm::vec3 chunk_max = chunk_min + glm::vec3(CHUNK_SIZE_X, CHUNK_SIZE_Y, CHUNK_SIZE_Z);
        chunk_refs.push_back(ChunkCullEntry{chunk.id, chunk.coords, AABB(chunk_min, chunk_max)});
    }
    
    // Calculate root bounding box from all chunks
    glm::vec3 min(std::numeric_limits<float>::max());
    glm::vec3 max(std::numeric_limits<float>::lowest());
    
    for (const auto& chunk : chunk_refs) {
        min = glm::min(min, chunk.bounds.min);
        max = glm::max(max, chunk.bounds.max);
    }
    
    m_root = std::make_unique<CullingNode>(AABB(min, max));
    BuildRecursive(m_root.get(), chunk_refs, 0);
}

void RenderPipeline::HierarchicalCuller::BuildRecursive(CullingNode* node, const std::vector<ChunkCullEntry>& chunks, int depth) {
    // Base cases: too few chunks or maximum depth reached
    if (chunks.size() <= MAX_CHUNKS_PER_NODE || depth >= MAX_DEPTH) {
        node->chunks = chunks;
        node->is_leaf = true;
        return;
    }
    
    // Split the node into 4 quadrants (X-Z plane)
    glm::vec3 center = (node->bounds.min + node->bounds.max) * 0.5f;
    
    // Create child bounds
    AABB child_bounds[4] = {
        AABB(node->bounds.min, glm::vec3(center.x, node->bounds.max.y, center.z)),  // Bottom-left
        AABB(glm::vec3(center.x, node->bounds.min.y, node->bounds.min.z), glm::vec3(node->bounds.max.x, node->bounds.max.y, center.z)),  // Bottom-right
        AABB(glm::vec3(node->bounds.min.x, node->bounds.min.y, center.z), glm::vec3(center.x, node->bounds.max.y, node->bounds.max.z)),  // Top-left
        AABB(glm::vec3(center.x, node->bounds.min.y, center.z), node->bounds.max)   // Top-right
    };
    
    // Distribute chunks to children
    std::vector<std::vector<ChunkCullEntry>> child_chunks(4);
    
    for (const auto& chunk : chunks) {
        glm::ivec3 coords = chunk.coords;
        glm::vec3 chunk_center(coords.x * CHUNK_SIZE_X + CHUNK_SIZE_X * 0.5f, 
                              coords.y * CHUNK_SIZE_Y + CHUNK_SIZE_Y * 0.5f,
                              coords.z * CHUNK_SIZE_Z + CHUNK_SIZE_Z * 0.5f);
        
        int child_index = 0;
        if (chunk_center.x >= center.x) child_index += 1;
        if (chunk_center.z >= center.z) child_index += 2;
        
        child_chunks[child_index].push_back(chunk);
    }
    
    // Create children for non-empty quadrants
    node->is_leaf = false;
    for (int i = 0; i < 4; ++i) {
        if (!child_chunks[i].empty()) {
            node->children[i] = std::make_unique<CullingNode>(child_bounds[i]);
            BuildRecursive(node->children[i].get(), child_chunks[i], depth + 1);
        }
    }
}

void RenderPipeline::HierarchicalCuller::CullRecursive(const glm::vec4 frustum_planes[6], CullingNode* node, std::vector<const ChunkCullEntry*>& visible) {
    if (!node) return;
    
    // Test this node's bounding box against the frustum
    if (AABBFrustumCulled(node->bounds, frustum_planes)) {
        return; // Entire subtree is culled
    }
    
    if (node->is_leaf) {
        for (const auto& chunk : node->chunks) {
            if (!AABBFrustumCulled(chunk.bounds, frustum_planes)) {
                visible.push_back(&chunk);
            }
        }
    } else {
        // Recursively test children
        for (int i = 0; i < 4; ++i) {
            if (node->children[i]) {
                CullRecursive(frustum_planes, node->children[i].get(), visible);
            }
        }
    }
}

bool RenderPipeline::HierarchicalCuller::AABBFrustumCulled(const AABB& aabb, const glm::vec4 frustum_planes[6]) {
    for (int i = 0; i < 6; ++i) {
        glm::vec3 p = glm::vec3(frustum_planes[i].x >= 0 ? aabb.max.x : aabb.min.x,
                               frustum_planes[i].y >= 0 ? aabb.max.y : aabb.min.y,
                               frustum_planes[i].z >= 0 ? aabb.max.z : aabb.min.z);
        if ((glm::dot(glm::vec3(frustum_planes[i]), p) + frustum_planes[i].w) < 0.0f) {
            return true; // Outside this plane
        }
    }
    return false; // Inside all planes
}

void RenderPipeline::HierarchicalCuller::CullHierarchical(const glm::vec4 frustum_planes[6], std::vector<const ChunkCullEntry*>& visible) {
    if (m_root) {
        CullRecursive(frustum_planes, m_root.get(), visible);
    }
}

void RenderPipeline::HierarchicalCuller::Clear() {
    m_root.reset();
}

// --- END HIERARCHICAL CULLING ---

// --- CONSTRUCTOR / DESTRUCTOR ---

RenderPipeline::RenderPipeline()
    : m_farlod(std::make_unique<FarLodSystem>()),
      m_gbuffer_pass(std::make_unique<GBufferPass>()),
      m_shadow_pass(std::make_unique<ShadowPass>()),
      m_ssao_pass(std::make_unique<SsaoPass>()),
      m_lighting_pass(std::make_unique<LightingPass>()),
      m_water_pass(std::make_unique<WaterPass>()),
      m_skybox_pass(std::make_unique<SkyboxPass>()) {}
RenderPipeline::~RenderPipeline() {
    cleanup_gpu_resources();
}

// --- PUBLIC INTERFACE ---

bool RenderPipeline::startup(u32 screen_width, u32 screen_height, const std::filesystem::path& root_path) {
    m_started = false;
    m_screen_width = screen_width;
    m_screen_height = screen_height;
    m_root_path = root_path;

    try {
        set_default_shadow_cascade_splits(m_shadow_pass->shadow_map());

        init_shaders();
        m_lighting_pass->init_lighting_fbo(screen_width, screen_height);
        m_gbuffer_pass->init_gbuffer(screen_width, screen_height);
        m_shadow_pass->init_shadow_map();
        m_ssao_pass->init_ssao(screen_width, screen_height);
        init_screen_quad();
        m_skybox_pass->init_geometry();
        load_material_texture_lut();
        init_terrain_textures();
        init_skinned_texture_array();
        init_material_lut();
        init_texture_residency();
        m_water_pass->init_water_fallback_textures();
        init_gpu_sdf_system();
        init_gpu_pass_timers();
        init_mdi_buffers(); // T-I4-16: per-frame MDI command + origin SSBO ring

        m_gbuffer_pass->init_instanced_static_mesh(m_root_path);
        m_gbuffer_pass->init_skinned_mesh(m_root_path);

        refresh_render_pass_metadata();
        m_started = true;
        LUMINUMBRA_CORE_INFO("Render Pipeline Initialized.");
        return true;
    } catch (const std::exception& e) {
        LUMINUMBRA_CORE_ERROR("Render Pipeline startup failed: {}", e.what());
    } catch (...) {
        LUMINUMBRA_CORE_ERROR("Render Pipeline startup failed with an unknown exception.");
    }

    cleanup_gpu_resources();
    return false;
}

void RenderPipeline::shutdown() {
    cleanup_gpu_resources();
}

void RenderPipeline::attach_farlod_job_system(JobSystem* job_system) {
    if (m_farlod) {
        m_farlod->attach_job_system(job_system);
    }
}

void RenderPipeline::prepare_world_swap() {
    if (m_farlod) {
        m_farlod->prepare_world_swap();
    }
}

void RenderPipeline::set_gpu_sdf_runtime_enabled(bool enabled) {
    m_gpu_sdf.runtime_requested = enabled;
    if (!enabled) {
        m_gpu_sdf.callback_registered = false;
    }
}

RenderPipeline::GpuSdfRuntimeToggleState RenderPipeline::get_gpu_sdf_runtime_toggle_state() const {
    GpuSdfRuntimeToggleState state;
    state.compile_time_enabled = kEnableExperimentalGpuSdfIntegration;
    state.runtime_requested = m_gpu_sdf.runtime_requested;
    state.runtime_allowed = kEnableExperimentalGpuSdfIntegration && m_gpu_sdf.runtime_requested && m_gpu_sdf.initialized;
    state.callback_registered = m_gpu_sdf.callback_registered;
    state.cpu_fallback_active = !m_gpu_sdf.callback_registered;
    return state;
}

void RenderPipeline::SetupGPUSDFIntegration(Systems::SHIELD_WorldSystem& world_system) {
    if (!kEnableExperimentalGpuSdfIntegration || !m_gpu_sdf.runtime_requested) {
        world_system.SetGPUSDFCallback({});
        m_gpu_sdf.callback_registered = false;
        if (m_gpu_sdf.runtime_requested) {
            LUMINUMBRA_CORE_WARN("GPU SDF runtime opt-in requested, but compile-time parity gate is closed; using authoritative CPU worldgen path");
        } else {
            LUMINUMBRA_CORE_WARN("GPU SDF integration disabled; using authoritative CPU worldgen path until GPU/CPU parity is implemented; pass --enable-gpu-sdf-runtime only after parity gate approval");
        }
        return;
    }

    if (!m_gpu_sdf.initialized) {
        world_system.SetGPUSDFCallback({});
        m_gpu_sdf.callback_registered = false;
        LUMINUMBRA_CORE_WARN("GPU SDF system not initialized, cannot set up integration");
        return;
    }
    
    // Set up callback for GPU SDF generation
    world_system.SetGPUSDFCallback(
        [this](const IVec3& chunk_coords, const ::Luminumbra::Systems::TerrainGenParams& params, int seed, std::vector<float>& out_sdf) -> bool {
            return this->generate_chunk_sdf_gpu(chunk_coords, params, seed, out_sdf);
        }
    );
    m_gpu_sdf.callback_registered = true;
    
    LUMINUMBRA_CORE_INFO("GPU SDF integration with world system established");
}

RenderPipeline::RuntimeRenderStats RenderPipeline::get_runtime_render_stats() const {
    RuntimeRenderStats stats;
    stats.started = m_started;
    stats.terrain_gpu_chunks = m_chunk_render_data.size();
    stats.water_gpu_chunks = m_water_render_data.size();
    stats.free_terrain_slots = m_free_chunk_render_slots.size();
    stats.free_water_slots = m_free_water_render_slots.size();

    auto add_chunk_capacity = [](const ChunkRenderData& data, size_t& vertices, size_t& indices) {
        vertices += data.vertex_capacity;
        indices += data.index_capacity;
    };
    auto add_water_capacity = [](const WaterRenderData& data, size_t& vertices, size_t& indices) {
        vertices += data.vertex_capacity;
        indices += data.index_capacity;
    };

    for (const auto& [id, data] : m_chunk_render_data) {
        (void)id;
        add_chunk_capacity(data, stats.terrain_vertex_capacity, stats.terrain_index_capacity);
    }
    for (const auto& data : m_free_chunk_render_slots) {
        add_chunk_capacity(data, stats.terrain_vertex_capacity, stats.terrain_index_capacity);
    }
    for (const auto& [id, data] : m_water_render_data) {
        (void)id;
        add_water_capacity(data, stats.water_vertex_capacity, stats.water_index_capacity);
    }
    for (const auto& data : m_free_water_render_slots) {
        add_water_capacity(data, stats.water_vertex_capacity, stats.water_index_capacity);
    }

    const size_t pixel_count = static_cast<size_t>(m_screen_width) * static_cast<size_t>(m_screen_height);
    size_t estimated_vram_bytes = 0;
    estimated_vram_bytes += (stats.terrain_vertex_capacity + stats.water_vertex_capacity) * sizeof(VoxelVertex);
    estimated_vram_bytes += (stats.terrain_index_capacity + stats.water_index_capacity) * sizeof(u32);
    const FrameBufferObject& lighting_fbo = m_lighting_pass->lighting_fbo();
    if (lighting_fbo.color_texture) estimated_vram_bytes += pixel_count * 8u; // RGBA16F
    if (lighting_fbo.opaque_color_texture) estimated_vram_bytes += pixel_count * 8u; // RGBA16F
    if (lighting_fbo.depth_texture) estimated_vram_bytes += pixel_count * 4u;
    const GBuffer& gbuffer = m_gbuffer_pass->gbuffer();
    if (gbuffer.position_texture) estimated_vram_bytes += pixel_count * 6u; // RGB16F
    if (gbuffer.normal_texture) estimated_vram_bytes += pixel_count * 4u;
    if (gbuffer.albedo_texture) estimated_vram_bytes += pixel_count * 4u;
    if (gbuffer.material_texture) estimated_vram_bytes += pixel_count * 4u; // RG16F
    if (gbuffer.depth_texture) estimated_vram_bytes += pixel_count * 4u;
    if (m_shadow_pass->shadow_map().depth_texture_array) {
        estimated_vram_bytes += static_cast<size_t>(m_shadow_pass->shadow_map().resolution) *
                                static_cast<size_t>(m_shadow_pass->shadow_map().resolution) *
                                static_cast<size_t>(ShadowMap::CASCADE_COUNT) * 4u;
    }
    if (m_ssao_pass->ssao().ssaoColorBuffer) estimated_vram_bytes += pixel_count * 2u;
    if (m_ssao_pass->ssao().ssaoColorBufferBlur) estimated_vram_bytes += pixel_count * 2u;
    if (m_ssao_pass->ssao().noiseTexture) estimated_vram_bytes += 4u * 4u * 6u;
    if (m_screen_quad_vbo) estimated_vram_bytes += 20u * sizeof(float);
    if (m_skybox_pass->vbo()) estimated_vram_bytes += 108u * sizeof(float);
    if (m_gbuffer_pass->instance_matrix_vbo()) estimated_vram_bytes += 10000u * sizeof(glm::mat4);
    if (m_terrainTextureArray) estimated_vram_bytes += static_cast<size_t>(kTerrainTextureResolution) * kTerrainTextureResolution * 5u * 4u;
    if (m_terrainNormalArray) estimated_vram_bytes += static_cast<size_t>(kTerrainTextureResolution) * kTerrainTextureResolution * 5u * 4u;
    if (m_skinnedTextureArray) estimated_vram_bytes += static_cast<size_t>(kSkinnedTextureResolution) * kSkinnedTextureResolution * 2u * 4u;
    if (m_materialLUT) estimated_vram_bytes += 256u * 2u * 4u;
    if (m_water_pass->flat_normal_texture()) estimated_vram_bytes += 4u;
    if (m_water_pass->neutral_flow_texture()) estimated_vram_bytes += 4u;
    if (m_water_pass->black_fallback_texture()) estimated_vram_bytes += 4u;
    if (m_water_pass->underwater_texture()) estimated_vram_bytes += 4u;
    if (m_water_pass->caustics_texture()) {
        estimated_vram_bytes += static_cast<size_t>(WaterPass::kCausticsResolution) *
                                static_cast<size_t>(WaterPass::kCausticsResolution) * 4u; // RGBA8
    }
    if (m_farlod) estimated_vram_bytes += m_farlod->stats().resident_bytes;
    if (m_gpu_sdf.sdf_buffer) estimated_vram_bytes += 17u * 17u * 17u * sizeof(float);
    if (m_gpu_sdf.terrain_noise_texture) estimated_vram_bytes += 128u * 128u * 128u * sizeof(float);
    if (m_gpu_sdf.cave_noise_texture) estimated_vram_bytes += 128u * 128u * 128u * sizeof(float);
    if (m_gpu_sdf.island_mask_texture) estimated_vram_bytes += 128u * 128u * sizeof(float);
    estimated_vram_bytes += m_texture_residency.resident_bytes;
    stats.estimated_vram_bytes = estimated_vram_bytes;

    stats.geometry_shader_ok = m_gbuffer_pass->geometry_shader() && m_gbuffer_pass->geometry_shader()->IsValid();
    stats.lighting_shader_ok = m_lighting_pass->shader() && m_lighting_pass->shader()->IsValid();
    stats.skybox_shader_ok = m_skybox_pass->shader() && m_skybox_pass->shader()->IsValid();
    stats.shadow_shader_ok = m_shadow_pass->shader() && m_shadow_pass->shader()->IsValid();
    stats.ssao_shader_ok = m_ssao_pass->ssao().ssaoShader && m_ssao_pass->ssao().ssaoShader->IsValid();
    stats.ssao_blur_shader_ok = m_ssao_pass->ssao().blurShader && m_ssao_pass->ssao().blurShader->IsValid();
    stats.water_shader_ok = m_water_pass->shader() && m_water_pass->shader()->IsValid();
    stats.instanced_static_mesh_shader_ok = m_gbuffer_pass->instanced_static_mesh_shader() && m_gbuffer_pass->instanced_static_mesh_shader()->IsValid();
    stats.gpu_sdf_initialized = m_gpu_sdf.initialized;
    const GpuSdfRuntimeToggleState gpu_sdf_runtime = get_gpu_sdf_runtime_toggle_state();
    stats.gpu_sdf_compile_time_enabled = gpu_sdf_runtime.compile_time_enabled;
    stats.gpu_sdf_runtime_requested = gpu_sdf_runtime.runtime_requested;
    stats.gpu_sdf_runtime_allowed = gpu_sdf_runtime.runtime_allowed;
    stats.gpu_sdf_callback_registered = gpu_sdf_runtime.callback_registered;
    stats.gpu_sdf_cpu_fallback_active = gpu_sdf_runtime.cpu_fallback_active;
    stats.terrain_texture_array_ok = m_terrainTextureArray != 0;
    stats.material_lut_ok = m_materialLUT != 0;
    stats.terrain_texture_fallback_layers = m_terrain_texture_fallback_layers;

    stats.texture_resident_bytes = m_texture_residency.resident_bytes;
    stats.texture_resident_budget_bytes = kTextureResidentBudgetBytes;
    stats.texture_resident_within_budget =
        m_texture_residency.resident_bytes <= kTextureResidentBudgetBytes;
    stats.texture_residency_array_count = m_texture_residency.arrays.size();
    {
        size_t layer_total = 0;
        for (const TextureResidencyArray& array : m_texture_residency.arrays) {
            layer_total += array.layer_count;
        }
        stats.texture_residency_layer_count = layer_total;
    }
    return stats;
}

std::vector<RenderPipeline::ShaderHealthEntry> RenderPipeline::get_shader_health() const {
    std::vector<ShaderHealthEntry> health;
    auto add_shader = [&health](const char* name, const std::unique_ptr<Shader>& shader) {
        ShaderHealthEntry entry;
        entry.name = name;
        entry.ok = shader && shader->IsValid();
        if (shader && !shader->Diagnostic().empty()) {
            entry.diagnostic = shader->Diagnostic();
        } else if (!shader) {
            entry.diagnostic = "not initialized";
        }
        health.push_back(std::move(entry));
    };

    add_shader("geometry", m_gbuffer_pass->geometry_shader());
    add_shader("lighting", m_lighting_pass->shader());
    add_shader("skybox", m_skybox_pass->shader());
    add_shader("shadow", m_shadow_pass->shader());
    add_shader("ssao", m_ssao_pass->ssao().ssaoShader);
    add_shader("ssao_blur", m_ssao_pass->ssao().blurShader);
    add_shader("water", m_water_pass->shader());
    add_shader("instanced_static_mesh", m_gbuffer_pass->instanced_static_mesh_shader());
    add_shader("skinned_mesh", m_gbuffer_pass->skinned_mesh_shader());
    add_shader("weather_overlay", m_skybox_pass->weather_shader());
    health.push_back({"gpu_sdf_compute", m_gpu_sdf.compute_program != 0, m_gpu_sdf.compute_program != 0 ? "" : "not initialized"});
    return health;
}

RenderPipeline::RenderResourceRegistryStats RenderPipeline::get_resource_registry_stats() const {
    RenderResourceRegistryStats stats;
    auto count = [](GLuint id) -> size_t { return id != 0 ? 1u : 0u; };

    stats.framebuffers += count(m_lighting_pass->lighting_fbo().fbo_id);
    stats.framebuffers += count(m_gbuffer_pass->gbuffer().fbo_id);
    stats.framebuffers += count(m_shadow_pass->shadow_map().fbo_id);
    stats.framebuffers += count(m_ssao_pass->ssao().fbo);
    stats.framebuffers += count(m_ssao_pass->ssao().blurFBO);
    stats.framebuffers += count(m_water_pass->caustics_fbo());

    stats.textures += count(m_lighting_pass->lighting_fbo().color_texture);
    stats.textures += count(m_lighting_pass->lighting_fbo().opaque_color_texture);
    stats.textures += count(m_gbuffer_pass->gbuffer().position_texture);
    stats.textures += count(m_gbuffer_pass->gbuffer().normal_texture);
    stats.textures += count(m_gbuffer_pass->gbuffer().albedo_texture);
    stats.textures += count(m_gbuffer_pass->gbuffer().material_texture);
    stats.textures += count(m_gbuffer_pass->gbuffer().depth_texture);
    stats.textures += count(m_shadow_pass->shadow_map().depth_texture_array);
    stats.textures += count(m_ssao_pass->ssao().ssaoColorBuffer);
    stats.textures += count(m_ssao_pass->ssao().ssaoColorBufferBlur);
    stats.textures += count(m_ssao_pass->ssao().noiseTexture);
    stats.textures += count(m_terrainTextureArray);
    stats.textures += count(m_terrainNormalArray);
    stats.textures += count(m_skinnedTextureArray);
    stats.textures += count(m_materialLUT);
    stats.textures += count(m_water_pass->flat_normal_texture());
    stats.textures += count(m_water_pass->neutral_flow_texture());
    stats.textures += count(m_water_pass->black_fallback_texture());
    stats.textures += count(m_water_pass->underwater_texture());
    stats.textures += count(m_water_pass->caustics_texture());
    stats.textures += count(m_gpu_sdf.terrain_noise_texture);
    stats.textures += count(m_gpu_sdf.cave_noise_texture);
    stats.textures += count(m_gpu_sdf.island_mask_texture);

    // Texture-array residency manager: one GL_TEXTURE_2D_ARRAY per size class
    // (T-I4-6). These register under the existing "texture" resource type, so
    // the render-health resource_types contract is unchanged.
    for (const TextureResidencyArray& array : m_texture_residency.arrays) {
        stats.textures += count(array.texture_id);
    }

    stats.renderbuffers += count(m_lighting_pass->lighting_fbo().depth_texture);
    stats.buffers += count(m_screen_quad_vbo);
    stats.buffers += count(m_skybox_pass->vbo());
    stats.buffers += count(m_gbuffer_pass->instance_matrix_vbo());
    stats.buffers += count(m_gbuffer_pass->joint_palette_ssbo());
    stats.buffers += count(m_gpu_sdf.sdf_buffer);
    stats.vertex_arrays += count(m_screen_quad_vao);
    stats.vertex_arrays += count(m_skybox_pass->vao());

    auto count_chunk_slot = [&stats](const ChunkRenderData& data) {
        stats.vertex_arrays += data.vao_id != 0 ? 1u : 0u;
        stats.buffers += data.vbo_id != 0 ? 1u : 0u;
        stats.buffers += data.ebo_id != 0 ? 1u : 0u;
    };
    auto count_water_slot = [&stats](const WaterRenderData& data) {
        stats.vertex_arrays += data.vao_id != 0 ? 1u : 0u;
        stats.buffers += data.vbo_id != 0 ? 1u : 0u;
        stats.buffers += data.ebo_id != 0 ? 1u : 0u;
    };

    stats.terrain_slots = m_chunk_render_data.size() + m_free_chunk_render_slots.size();
    stats.water_slots = m_water_render_data.size() + m_free_water_render_slots.size();
    for (const auto& [id, data] : m_chunk_render_data) {
        (void)id;
        count_chunk_slot(data);
    }
    for (const auto& data : m_free_chunk_render_slots) {
        count_chunk_slot(data);
    }

    // T-I4-16: live terrain geometry is now backed by the shared pool's blocks
    // (each block: 1 VAO + vertex VBO + index EBO) plus the per-frame MDI ring
    // (indirect command buffer + origin buffer per ring slot), rather than a
    // VAO/VBO/EBO per chunk. Count those here so the resource registry remains
    // accurate (debug-labelled, and empty after shutdown -- the pool/MDI buffers
    // are released in cleanup_gpu_resources()).
    for (const ChunkGeometryPool::Block& block : m_chunk_geometry_pool.blocks()) {
        stats.vertex_arrays += block.vao != 0 ? 1u : 0u;
        stats.buffers += block.vbo != 0 ? 1u : 0u;
        stats.buffers += block.ebo != 0 ? 1u : 0u;
    }
    for (const MdiFrameBuffers& frame : m_mdi_frames) {
        stats.buffers += frame.indirect_buffer != 0 ? 1u : 0u;
        stats.buffers += frame.origin_buffer != 0 ? 1u : 0u;
    }
    for (const auto& [id, data] : m_water_render_data) {
        (void)id;
        count_water_slot(data);
    }
    for (const auto& data : m_free_water_render_slots) {
        count_water_slot(data);
    }

    // Far-LOD region meshes (one VAO + VBO/EBO per resident region, T-I3-9).
    if (m_farlod) {
        stats.vertex_arrays += m_farlod->resident_vertex_array_count();
        stats.buffers += m_farlod->resident_vertex_array_count() * 2u;
    }

    for (const ShaderHealthEntry& shader : get_shader_health()) {
        if (shader.ok) {
            stats.shader_programs++;
        }
    }

    // GPU timer query objects are intentionally not counted here: the
    // resource-registry contract enumerates framebuffer/texture/renderbuffer/
    // buffer/vertex_array/shader_program types only, and timestamp queries are
    // transient profiling objects with no backing storage. They still receive
    // debug labels once created and are deleted in cleanup_gpu_resources(), so
    // the empty-after-shutdown contract holds.

    const size_t total_resources = stats.framebuffers + stats.textures + stats.renderbuffers +
        stats.buffers + stats.vertex_arrays + stats.shader_programs + stats.terrain_slots + stats.water_slots;
    stats.empty_after_shutdown = !m_started && total_resources == 0u;
    return stats;
}

RenderPipeline::RenderHealthSnapshot RenderPipeline::get_render_health_snapshot(bool drain_gl_errors) const {
    RenderHealthSnapshot snapshot;
    snapshot.runtime = get_runtime_render_stats();
    snapshot.resources = get_resource_registry_stats();
    snapshot.shaders = get_shader_health();
    snapshot.passes = m_last_render_pass_metadata;
    snapshot.started = m_started;

    auto fail = [&snapshot](std::string message) {
        snapshot.failures.push_back(std::move(message));
    };

    if (drain_gl_errors) {
        constexpr size_t kMaxDrainedGlErrors = 256;
        for (size_t drained = 0; drained < kMaxDrainedGlErrors; ++drained) {
            const GLenum error = glGetError();
            if (error == GL_NO_ERROR) {
                break;
            }
            ++snapshot.gl_debug_errors;
        }
        if (snapshot.gl_debug_errors == kMaxDrainedGlErrors) {
            fail("GL error drain reached the safety limit");
        }
    }

    if (snapshot.gl_debug_errors != 0u) {
        fail("GL debug error count is non-zero");
    }

    if (m_started) {
        if (m_screen_width == 0u || m_screen_height == 0u) {
            fail("render target dimensions are not initialized");
        }
        if (!snapshot.runtime.terrain_texture_array_ok) {
            fail("terrain texture array is not initialized");
        }
        if (!snapshot.runtime.material_lut_ok) {
            fail("material LUT is not initialized");
        }
        if (snapshot.runtime.terrain_texture_fallback_layers != 0u) {
            fail("terrain texture array used fallback layers");
        }

        for (const ShaderHealthEntry& shader : snapshot.shaders) {
            if (!shader.ok) {
                fail("shader health failed: " + shader.name);
            }
        }

        const std::array<const char*, 8> required_passes = {
            "shadow",
            "gbuffer",
            "ssao",
            "ssao_blur",
            "lighting",
            "water",
            "skybox",
            "final_blit",
        };
        for (const char* required_pass : required_passes) {
            const bool found = std::any_of(
                snapshot.passes.begin(),
                snapshot.passes.end(),
                [required_pass](const RenderPassMetadata& pass) {
                    return pass.name == required_pass;
                });
            if (!found) {
                fail(std::string("missing render pass metadata: ") + required_pass);
            }
        }

        if (snapshot.resources.framebuffers == 0u) {
            fail("resource registry has no framebuffers while started");
        }
        if (snapshot.resources.textures == 0u) {
            fail("resource registry has no textures while started");
        }
        if (snapshot.resources.shader_programs == 0u) {
            fail("resource registry has no shader programs while started");
        }
    } else if (!snapshot.resources.empty_after_shutdown) {
        fail("resource registry is not empty after shutdown");
    }

    snapshot.passed = snapshot.failures.empty();
    return snapshot;
}

void RenderPipeline::refresh_render_pass_metadata() {
    m_last_render_pass_metadata.clear();
    auto add_pass = [this](std::string name,
                           std::vector<std::string> inputs,
                           std::vector<std::string> outputs,
                           u32 width,
                           u32 height,
                           std::string clear,
                           std::string load_store,
                           size_t draw_count,
                           size_t dispatch_count = 0u) {
        RenderPassMetadata metadata;
        metadata.name = std::move(name);
        metadata.inputs = std::move(inputs);
        metadata.outputs = std::move(outputs);
        metadata.width = width;
        metadata.height = height;
        metadata.clear = std::move(clear);
        metadata.load_store = std::move(load_store);
        metadata.draw_count = draw_count;
        metadata.dispatch_count = dispatch_count;
        m_last_render_pass_metadata.push_back(std::move(metadata));
    };

    add_pass("shadow", {"terrain_depth"}, {"shadow.depth_texture_array"}, m_shadow_pass->shadow_map().resolution, m_shadow_pass->shadow_map().resolution,
             "depth", "store depth cascades", m_last_render_pass_stats.shadow_draws);
    add_pass("gbuffer", {"terrain_meshes", "farlod_region_meshes", "static_meshes", "skinned_meshes", "material_lut", "terrain_texture_array", "terrain_normal_array"},
             {"gbuffer.position", "gbuffer.normal_material", "gbuffer.albedo_roughness", "gbuffer.metallic_ao", "gbuffer.depth"},
             m_screen_width, m_screen_height, "color+depth", "store deferred attachments",
             m_last_render_pass_stats.terrain_draws + m_last_render_pass_stats.far_region_draws + m_last_render_pass_stats.skinned_draws);
    add_pass("ssao", {"gbuffer.position", "gbuffer.normal_material", "ssao.noise"}, {"ssao.raw"}, m_screen_width, m_screen_height,
             "color", "store ambient occlusion", m_last_render_pass_stats.ssao_draws);
    add_pass("ssao_blur", {"ssao.raw"}, {"ssao.blur"}, m_screen_width, m_screen_height,
             "color", "store blurred ambient occlusion", m_last_render_pass_stats.ssao_blur_draws);
    add_pass("lighting", {"gbuffer.*", "shadow.depth_texture_array", "ssao.blur", "terrain_texture_array", "material_lut", "water.fallback.black"},
             {"lighting.color", "lighting.depth"}, m_screen_width, m_screen_height,
             "color+depth", "store lit scene", m_last_render_pass_stats.lighting_draws);
    add_pass("water", {"lighting.opaque_color_copy", "gbuffer.depth", "water_meshes", "water.fallback.*"}, {"lighting.color"}, m_screen_width, m_screen_height,
             "load lighting", "blend water into lighting", m_last_render_pass_stats.water_draws);
    add_pass("skybox", {"skybox_vertices"}, {"lighting.color"}, m_screen_width, m_screen_height,
             "load lighting", "store sky contribution", m_last_render_pass_stats.skybox_draws);
    add_pass("final_blit", {"lighting.color"}, {"swapchain.color"}, m_screen_width, m_screen_height,
             "default color+depth", "present-ready color", m_last_render_pass_stats.final_blits);
}

// --- PER-PASS GPU TIMERS ---

// ===========================================================================
// T-I4-16: glMultiDrawElementsIndirect submission for live terrain chunks.
//
// The G-buffer and shadow passes share this path. Each visible pool-resident
// chunk becomes one DrawElementsIndirectCommand (firstIndex/baseVertex into the
// chunk's pool block) plus a chunk world origin written to the origin SSBO at
// the same draw index. Commands are grouped by pool block so each block's VAO
// is bound once and submitted with a single glMultiDrawElementsIndirect (the
// "per-bucket MDI" form from the dispatch). The vertex shaders read the origin
// by gl_DrawID from the SSBO instead of a per-draw model uniform.
//
// A small ring of {indirect buffer, origin SSBO} pairs avoids the GPU stalling
// on buffers it may still be reading from a prior frame (the passes run twice
// per frame -> shadow then gbuffer; the ring advances per submit).
// ===========================================================================
void RenderPipeline::init_mdi_buffers() {
    for (MdiFrameBuffers& frame : m_mdi_frames) {
        frame.indirect_buffer = 0;
        frame.origin_buffer = 0;
        frame.command_capacity = 0;
    }
    m_mdi_frame_cursor = 0;
}

void RenderPipeline::destroy_mdi_buffers() {
    for (MdiFrameBuffers& frame : m_mdi_frames) {
        if (frame.indirect_buffer) { glDeleteBuffers(1, &frame.indirect_buffer); frame.indirect_buffer = 0; }
        if (frame.origin_buffer) { glDeleteBuffers(1, &frame.origin_buffer); frame.origin_buffer = 0; }
        frame.command_capacity = 0;
    }
    m_mdi_frame_cursor = 0;
    m_mdi_command_scratch.clear();
    m_mdi_command_scratch.shrink_to_fit();
    m_mdi_origin_scratch.clear();
    m_mdi_origin_scratch.shrink_to_fit();
}

void RenderPipeline::ensure_mdi_capacity(MdiFrameBuffers& frame, std::size_t commands) {
    if (commands <= frame.command_capacity && frame.indirect_buffer != 0) {
        return;
    }
    // Grow with headroom so steady-state frames never reallocate.
    std::size_t new_capacity = std::max<std::size_t>(commands, 256u);
    new_capacity += new_capacity / 2u;

    if (frame.indirect_buffer) { glDeleteBuffers(1, &frame.indirect_buffer); frame.indirect_buffer = 0; }
    if (frame.origin_buffer) { glDeleteBuffers(1, &frame.origin_buffer); frame.origin_buffer = 0; }

    glGenBuffers(1, &frame.indirect_buffer);
    glBindBuffer(GL_DRAW_INDIRECT_BUFFER, frame.indirect_buffer);
    glBufferData(GL_DRAW_INDIRECT_BUFFER,
                 static_cast<GLsizeiptr>(new_capacity * sizeof(DrawElementsIndirectCommand)),
                 nullptr, GL_STREAM_DRAW);

    glGenBuffers(1, &frame.origin_buffer);
    glBindBuffer(GL_ARRAY_BUFFER, frame.origin_buffer);
    glBufferData(GL_ARRAY_BUFFER,
                 static_cast<GLsizeiptr>(new_capacity * sizeof(glm::vec4)),
                 nullptr, GL_STREAM_DRAW);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindBuffer(GL_DRAW_INDIRECT_BUFFER, 0);

    const std::size_t slot = static_cast<std::size_t>(&frame - m_mdi_frames.data());
    const std::string base = "terrain.mdi." + std::to_string(slot);
    label_gl_object(GL_BUFFER, frame.indirect_buffer, base + ".indirect");
    label_gl_object(GL_BUFFER, frame.origin_buffer, base + ".origins");

    frame.command_capacity = new_capacity;
}

void RenderPipeline::draw_chunks_mdi(const std::vector<const ChunkCullEntry*>& visible_chunks,
                                     std::size_t& out_draws, std::size_t& out_indices) {
    out_draws = 0;
    out_indices = 0;
    const std::size_t block_count = m_chunk_geometry_pool.block_count();
    if (visible_chunks.empty() || block_count == 0) {
        return;
    }

    // Bucket visible draws by pool block, laying out per-block contiguous runs
    // of commands in a single flat array. The chunk origin shares the same flat
    // index, and each command's baseInstance = its flat index, so the instanced
    // origin attribute (binding 1, divisor 1) fetches origins[baseInstance] for
    // that draw. This is portable to GL 4.3 (no gl_BaseInstance/gl_DrawID in the
    // shader required). One glMultiDrawElementsIndirect per block (per bucket).
    std::vector<std::size_t> per_block_count(block_count, 0u);
    for (const ChunkCullEntry* chunk : visible_chunks) {
        auto it = m_chunk_render_data.find(chunk->id);
        if (it == m_chunk_render_data.end()) continue;
        const ChunkRenderData& rd = it->second;
        if (rd.pool_handle == ChunkRenderData::kInvalidPoolHandle || rd.element_count == 0) continue;
        ++per_block_count[m_chunk_geometry_pool.allocation(rd.pool_handle).block_index];
    }
    std::vector<std::size_t> block_offset(block_count, 0u);
    std::size_t running = 0;
    for (std::size_t b = 0; b < block_count; ++b) {
        block_offset[b] = running;
        running += per_block_count[b];
    }
    if (running == 0) {
        return;
    }

    m_mdi_command_scratch.resize(running);
    m_mdi_origin_scratch.resize(running);

    std::vector<std::size_t> write_cursor = block_offset;
    for (const ChunkCullEntry* chunk : visible_chunks) {
        auto it = m_chunk_render_data.find(chunk->id);
        if (it == m_chunk_render_data.end()) continue;
        const ChunkRenderData& rd = it->second;
        if (rd.pool_handle == ChunkRenderData::kInvalidPoolHandle || rd.element_count == 0) continue;
        const ChunkGeometryPool::Allocation& a = m_chunk_geometry_pool.allocation(rd.pool_handle);

        const std::size_t dst = write_cursor[a.block_index]++;
        DrawElementsIndirectCommand& cmd = m_mdi_command_scratch[dst];
        cmd.count = a.index_count;
        cmd.instanceCount = 1u;
        cmd.firstIndex = a.index_offset;
        cmd.baseVertex = a.vertex_offset;
        cmd.baseInstance = static_cast<GLuint>(dst); // origins[baseInstance]

        const glm::ivec3 cc = chunk->coords;
        m_mdi_origin_scratch[dst] = glm::vec4(
            static_cast<float>(cc.x * CHUNK_SIZE_X),
            static_cast<float>(cc.y * CHUNK_SIZE_Y),
            static_cast<float>(cc.z * CHUNK_SIZE_Z),
            0.0f);

        out_indices += a.index_count;
    }
    out_draws = running;

    MdiFrameBuffers& frame = m_mdi_frames[m_mdi_frame_cursor];
    ensure_mdi_capacity(frame, running);

    glBindBuffer(GL_DRAW_INDIRECT_BUFFER, frame.indirect_buffer);
    glBufferSubData(GL_DRAW_INDIRECT_BUFFER, 0,
                    static_cast<GLsizeiptr>(running * sizeof(DrawElementsIndirectCommand)),
                    m_mdi_command_scratch.data());
    // The origin buffer is consumed as an instanced vertex attribute (binding 1,
    // vec4 stride), not an SSBO -- baseInstance indexing works without GLSL 4.6.
    glBindBuffer(GL_ARRAY_BUFFER, frame.origin_buffer);
    glBufferSubData(GL_ARRAY_BUFFER, 0,
                    static_cast<GLsizeiptr>(running * sizeof(glm::vec4)),
                    m_mdi_origin_scratch.data());
    glBindBuffer(GL_ARRAY_BUFFER, 0);

    const auto& blocks = m_chunk_geometry_pool.blocks();
    for (std::size_t b = 0; b < block_count; ++b) {
        const std::size_t count = per_block_count[b];
        if (count == 0) continue;
        const std::size_t first = block_offset[b];

        glBindVertexArray(blocks[b].vao);
        // Bind this frame's origin buffer to the VAO's instanced binding (1).
        // baseInstance is absolute into this buffer, so offset 0 is correct.
        glBindVertexBuffer(1, frame.origin_buffer, 0, sizeof(glm::vec4));
        glBindBuffer(GL_DRAW_INDIRECT_BUFFER, frame.indirect_buffer);
        glMultiDrawElementsIndirect(
            GL_TRIANGLES, GL_UNSIGNED_INT,
            reinterpret_cast<const void*>(first * sizeof(DrawElementsIndirectCommand)),
            static_cast<GLsizei>(count),
            static_cast<GLsizei>(sizeof(DrawElementsIndirectCommand)));
    }

    glBindVertexArray(0);
    glBindBuffer(GL_DRAW_INDIRECT_BUFFER, 0);

    m_mdi_frame_cursor = (m_mdi_frame_cursor + 1u) % kMdiRingFrames;
}

void RenderPipeline::init_gpu_pass_timers() {
    static_assert(sizeof(kGpuTimerPassNames) / sizeof(kGpuTimerPassNames[0]) == kGpuTimerPassCount,
                  "GPU timer pass name table must mirror GpuTimerPass");

    m_gpu_timers = {};

    // Capability gate: GL_TIMESTAMP queries require GL 3.3+ or
    // GL_ARB_timer_query, and the glad-resolved entry points must be non-null.
    const bool loader_ok =
        glGenQueries != nullptr &&
        glDeleteQueries != nullptr &&
        glQueryCounter != nullptr &&
        glGetQueryObjectiv != nullptr &&
        glGetQueryObjectui64v != nullptr;
    const bool capability_ok = GLAD_GL_VERSION_3_3 != 0 || gl_extension_present("GL_ARB_timer_query");
    if (!capability_ok || !loader_ok) {
        LUMINUMBRA_CORE_WARN("Per-pass GPU timers disabled: GL_TIMESTAMP queries are unsupported on this context.");
        return;
    }

    for (GpuTimerFrameSlot& slot : m_gpu_timers.slots) {
        glGenQueries(static_cast<GLsizei>(kGpuTimerPassCount), slot.begin_queries.data());
        glGenQueries(static_cast<GLsizei>(kGpuTimerPassCount), slot.end_queries.data());
    }
    m_gpu_timers.supported = true;
}

void RenderPipeline::destroy_gpu_pass_timers() {
    if (glDeleteQueries != nullptr) {
        for (GpuTimerFrameSlot& slot : m_gpu_timers.slots) {
            for (size_t pass = 0; pass < kGpuTimerPassCount; ++pass) {
                if (slot.begin_queries[pass] != 0u) {
                    glDeleteQueries(1, &slot.begin_queries[pass]);
                }
                if (slot.end_queries[pass] != 0u) {
                    glDeleteQueries(1, &slot.end_queries[pass]);
                }
            }
        }
    }
    m_gpu_timers = {};
}

void RenderPipeline::begin_gpu_pass_timer(GpuTimerPass pass) {
    if (!m_gpu_timers.supported) {
        return;
    }
    GpuTimerFrameSlot& slot = m_gpu_timers.slots[m_gpu_timers.frame_index % kGpuTimerFrameRing];
    glQueryCounter(slot.begin_queries[static_cast<size_t>(pass)], GL_TIMESTAMP);
}

void RenderPipeline::end_gpu_pass_timer(GpuTimerPass pass) {
    if (!m_gpu_timers.supported) {
        return;
    }
    GpuTimerFrameSlot& slot = m_gpu_timers.slots[m_gpu_timers.frame_index % kGpuTimerFrameRing];
    glQueryCounter(slot.end_queries[static_cast<size_t>(pass)], GL_TIMESTAMP);
    slot.issued[static_cast<size_t>(pass)] = true;
}

void RenderPipeline::collect_gpu_pass_timers() {
    m_last_render_pass_stats.gpu_timers_supported = m_gpu_timers.supported;
    bool resolved_sample_this_frame = false;
    if (m_gpu_timers.supported) {
        // Read the slot written two frames ago. The slot is not reused until
        // the next frame, so polling here never has to block the CPU.
        GpuTimerFrameSlot& read_slot = m_gpu_timers.slots[(m_gpu_timers.frame_index + 1u) % kGpuTimerFrameRing];
        for (size_t pass = 0; pass < kGpuTimerPassCount; ++pass) {
            if (!read_slot.issued[pass]) {
                continue;
            }
            GLint begin_available = GL_FALSE;
            GLint end_available = GL_FALSE;
            glGetQueryObjectiv(read_slot.begin_queries[pass], GL_QUERY_RESULT_AVAILABLE, &begin_available);
            glGetQueryObjectiv(read_slot.end_queries[pass], GL_QUERY_RESULT_AVAILABLE, &end_available);
            if (begin_available != GL_TRUE || end_available != GL_TRUE) {
                // Not resolved yet: keep the previous sample instead of stalling.
                continue;
            }
            GLuint64 begin_ns = 0;
            GLuint64 end_ns = 0;
            glGetQueryObjectui64v(read_slot.begin_queries[pass], GL_QUERY_RESULT, &begin_ns);
            glGetQueryObjectui64v(read_slot.end_queries[pass], GL_QUERY_RESULT, &end_ns);
            read_slot.issued[pass] = false;
            m_gpu_timers.last_gpu_ms[pass] = end_ns >= begin_ns
                ? static_cast<double>(end_ns - begin_ns) / 1.0e6
                : 0.0;
            resolved_sample_this_frame = true;
        }
    }

    m_last_render_pass_stats.shadow_gpu_ms = m_gpu_timers.last_gpu_ms[static_cast<size_t>(GpuTimerPass::Shadow)];
    m_last_render_pass_stats.gbuffer_gpu_ms = m_gpu_timers.last_gpu_ms[static_cast<size_t>(GpuTimerPass::GBuffer)];
    m_last_render_pass_stats.ssao_gpu_ms = m_gpu_timers.last_gpu_ms[static_cast<size_t>(GpuTimerPass::Ssao)];
    m_last_render_pass_stats.ssao_blur_gpu_ms = m_gpu_timers.last_gpu_ms[static_cast<size_t>(GpuTimerPass::SsaoBlur)];
    m_last_render_pass_stats.lighting_gpu_ms = m_gpu_timers.last_gpu_ms[static_cast<size_t>(GpuTimerPass::Lighting)];
    m_last_render_pass_stats.water_gpu_ms = m_gpu_timers.last_gpu_ms[static_cast<size_t>(GpuTimerPass::Water)];
    m_last_render_pass_stats.skybox_gpu_ms = m_gpu_timers.last_gpu_ms[static_cast<size_t>(GpuTimerPass::Skybox)];
    m_last_render_pass_stats.final_blit_gpu_ms = m_gpu_timers.last_gpu_ms[static_cast<size_t>(GpuTimerPass::FinalBlit)];

    // One-time diagnostic so smoke runs prove the ring resolves real samples.
    if (resolved_sample_this_frame && !m_gpu_timers.first_sample_logged) {
        m_gpu_timers.first_sample_logged = true;
        LUMINUMBRA_CORE_INFO(
            "Per-pass GPU timers active (ms): shadow={:.4f} gbuffer={:.4f} ssao={:.4f} ssao_blur={:.4f} lighting={:.4f} water={:.4f} skybox={:.4f} final_blit={:.4f}",
            m_last_render_pass_stats.shadow_gpu_ms,
            m_last_render_pass_stats.gbuffer_gpu_ms,
            m_last_render_pass_stats.ssao_gpu_ms,
            m_last_render_pass_stats.ssao_blur_gpu_ms,
            m_last_render_pass_stats.lighting_gpu_ms,
            m_last_render_pass_stats.water_gpu_ms,
            m_last_render_pass_stats.skybox_gpu_ms,
            m_last_render_pass_stats.final_blit_gpu_ms);
    }
}

void RenderPipeline::finish_gpu_pass_timer_frame() {
    if (!m_gpu_timers.supported) {
        return;
    }
    // GL query objects only become valid label targets after first use
    // (glGenQueries reserves names without creating the objects), so debug
    // labels are applied once every ring slot has issued its timestamps.
    if (!m_gpu_timers.labeled && m_gpu_timers.frame_index + 1u >= kGpuTimerFrameRing) {
        for (size_t slot_index = 0; slot_index < kGpuTimerFrameRing; ++slot_index) {
            const GpuTimerFrameSlot& slot = m_gpu_timers.slots[slot_index];
            for (size_t pass = 0; pass < kGpuTimerPassCount; ++pass) {
                const std::string base = "gpu_timer." + std::string(kGpuTimerPassNames[pass]) + "." + std::to_string(slot_index);
                label_gl_object(GL_QUERY, slot.begin_queries[pass], base + ".begin");
                label_gl_object(GL_QUERY, slot.end_queries[pass], base + ".end");
            }
        }
        m_gpu_timers.labeled = true;
    }
    ++m_gpu_timers.frame_index;
}

void RenderPipeline::gather_lights(entt::registry& registry) {
    m_point_lights_this_frame.clear();
    auto view = registry.view<const Components::TransformComponent, const Components::PointLightComponent>();
    
    for (auto entity : view) {
        if (m_point_lights_this_frame.size() >= MAX_POINT_LIGHTS) break;

        auto const& transform = view.get<const Components::TransformComponent>(entity);
        auto const& light_data = view.get<const Components::PointLightComponent>(entity);

        PointLight light;
        light.position = transform.position;
        light.color = light_data.color;
        light.radius = light_data.radius;
        light.intensity = light_data.intensity;
        m_point_lights_this_frame.push_back(light);
    }
}

std::vector<RenderPipeline::ChunkMeshSnapshot> RenderPipeline::build_chunk_snapshots(const std::vector<Chunk*>& renderable_chunks) const {
    std::vector<ChunkMeshSnapshot> snapshots;
    snapshots.reserve(renderable_chunks.size());

    for (const auto* chunk : renderable_chunks) {
        if (!chunk) {
            continue;
        }

        ChunkMeshSnapshot snapshot;
        snapshot.id = chunk->get_id();
        snapshot.coords = chunk->get_coords();
        snapshot.source_chunk = chunk;
        snapshot.mesh_version = chunk->mesh_version.load(std::memory_order_acquire);
        snapshot.water_mesh_version = chunk->water_mesh_version.load(std::memory_order_acquire);
        snapshot.terrain_vertex_count = chunk->mesh_vertices.size();
        snapshot.terrain_index_count = chunk->mesh_indices.size();
        snapshot.water_vertex_count = chunk->water_mesh_vertices.size();
        snapshot.water_index_count = chunk->water_mesh_indices.size();

        const u32 version_after_metadata = chunk->mesh_version.load(std::memory_order_acquire);
        const u32 water_version_after_metadata = chunk->water_mesh_version.load(std::memory_order_acquire);
        if (version_after_metadata != snapshot.mesh_version) {
            snapshot.mesh_version = version_after_metadata;
            snapshot.terrain_vertex_count = 0;
            snapshot.terrain_index_count = 0;
        }
        if (water_version_after_metadata != snapshot.water_mesh_version) {
            snapshot.water_mesh_version = water_version_after_metadata;
            snapshot.water_vertex_count = 0;
            snapshot.water_index_count = 0;
        }

        snapshots.push_back(std::move(snapshot));
    }

    return snapshots;
}

void RenderPipeline::ensure_terrain_culling_hierarchy(const std::vector<ChunkMeshSnapshot>& renderable_chunks) {
    std::vector<ChunkID> sorted_chunk_ids;
    sorted_chunk_ids.reserve(renderable_chunks.size());
    for (const auto& chunk : renderable_chunks) {
        sorted_chunk_ids.push_back(chunk.id);
    }
    std::sort(sorted_chunk_ids.begin(), sorted_chunk_ids.end());

    u64 signature = 1469598103934665603ull;
    for (const ChunkID id : sorted_chunk_ids) {
        signature ^= id;
        signature *= 1099511628211ull;
    }

    const bool needs_rebuild = !m_terrainCullingCache.valid ||
                               m_terrainCullingCache.chunk_count != renderable_chunks.size() ||
                               m_terrainCullingCache.chunk_set_signature != signature;
    if (!needs_rebuild) {
        m_last_render_pass_stats.culling_hierarchy_chunks = m_terrainCullingCache.chunk_count;
        return;
    }

    m_hierarchicalCuller.BuildHierarchy(renderable_chunks);
    m_terrainCullingCache.chunk_set_signature = signature;
    m_terrainCullingCache.chunk_count = renderable_chunks.size();
    m_terrainCullingCache.valid = true;
    m_last_render_pass_stats.culling_hierarchy_rebuilds++;
    m_last_render_pass_stats.culling_hierarchy_chunks = renderable_chunks.size();
}

void RenderPipeline::render_frame(entt::registry& registry, Systems::SHIELD_WorldSystem& world_system, const Camera& camera, float deltaTime, bool wireframe) {
    if (!m_started) {
        return;
    }

    update_time_of_day(deltaTime);
    gather_lights(registry);
    auto renderable_chunks = world_system.get_renderable_chunks();
    auto renderable_chunk_snapshots = build_chunk_snapshots(renderable_chunks);
    m_last_mesh_upload_stats = {};
    m_last_mesh_upload_stats.snapshot_count = renderable_chunk_snapshots.size();
    m_last_render_pass_stats = {};
    m_last_render_pass_stats.snapshot_count = renderable_chunk_snapshots.size();

    // Publish GPU timings recorded two frames ago without stalling, then
    // record this frame's passes into the current ring slot below.
    collect_gpu_pass_timers();

    manage_chunk_gpu_resources(renderable_chunk_snapshots, camera);
    manage_water_gpu_resources(renderable_chunk_snapshots, camera);
    ensure_terrain_culling_hierarchy(renderable_chunk_snapshots);

    // Far-LOD scheduling (T-I3-9): ring-diff the wanted region set, integrate
    // finished tile builds (mesh uploads), and evict. Draws happen inside the
    // G-buffer pass after the live chunks.
    if (m_farlod) {
        m_farlod->update(world_system, camera.Position);
    }

    // Ensure no VAO is bound at start to prevent artifacts
    glBindVertexArray(0);
    
    glEnable(GL_DEPTH_TEST);
    
    // Configure rendering mode
    if (wireframe) {
        glPolygonMode(GL_FRONT_AND_BACK, GL_LINE);
        glLineWidth(2.0f);
        glDisable(GL_CULL_FACE);  // Disable culling in wireframe for debugging
    } else {
        glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
        glEnable(GL_CULL_FACE);
        glCullFace(GL_BACK);
    }

    glm::mat4 projection = glm::perspective(glm::radians(camera.Zoom), (float)m_screen_width / (float)m_screen_height, camera.GetNearPlane(), camera.GetFarPlane());
    glm::mat4 view = camera.GetViewMatrix();
    
    // Cache frustum planes to avoid recalculation when camera hasn't changed significantly
    glm::vec4 frustum_planes[6];
    const float POSITION_THRESHOLD = 0.5f;  // Less sensitive to prevent cache thrashing
    const float ROTATION_THRESHOLD = 0.05f; // Reduced sensitivity for smooth movement
    
    bool needsUpdate = !m_frustumCache.valid ||
                      glm::distance(camera.Position, m_frustumCache.lastCameraPos) > POSITION_THRESHOLD ||
                      glm::distance(camera.Front, m_frustumCache.lastCameraFront) > ROTATION_THRESHOLD ||
                      std::abs(camera.Zoom - m_frustumCache.lastZoom) > 0.1f;
    
    if (needsUpdate) {
        ExtractFrustumPlanes(projection * view, m_frustumCache.planes);
        m_frustumCache.lastCameraPos = camera.Position;
        m_frustumCache.lastCameraFront = camera.Front;
        m_frustumCache.lastZoom = camera.Zoom;
        m_frustumCache.valid = true;
    }
    
    // Use cached planes
    std::memcpy(frustum_planes, m_frustumCache.planes, sizeof(frustum_planes));

     // 1. SHADOW PASS
    begin_gpu_pass_timer(GpuTimerPass::Shadow);
    m_shadow_pass->execute(*this, renderable_chunk_snapshots, camera);
    end_gpu_pass_timer(GpuTimerPass::Shadow);
    glViewport(0, 0, m_screen_width, m_screen_height);

    // 2. GEOMETRY / G-BUFFER PASS
    begin_gpu_pass_timer(GpuTimerPass::GBuffer);
    m_gbuffer_pass->execute(*this, registry, renderable_chunk_snapshots, camera, frustum_planes);
    end_gpu_pass_timer(GpuTimerPass::GBuffer);
    glBindVertexArray(0);  // Unbind after gbuffer pass
    glDisable(GL_CULL_FACE);

    // 3. SSAO PASS
    begin_gpu_pass_timer(GpuTimerPass::Ssao);
    m_ssao_pass->execute_ssao(*this, camera);
    end_gpu_pass_timer(GpuTimerPass::Ssao);
    glBindVertexArray(0);  // Unbind after SSAO
    begin_gpu_pass_timer(GpuTimerPass::SsaoBlur);
    m_ssao_pass->execute_blur(*this);
    end_gpu_pass_timer(GpuTimerPass::SsaoBlur);
    glBindVertexArray(0);  // Unbind after SSAO blur

    // 4. LIGHTING PASS (Renders to m_lighting_fbo)
    glEnable(GL_CULL_FACE);
    begin_gpu_pass_timer(GpuTimerPass::Lighting);
    m_lighting_pass->execute(*this, camera);
    end_gpu_pass_timer(GpuTimerPass::Lighting);
    glBindVertexArray(0);  // Unbind after lighting pass

    // 5. SNAPSHOT THE OPAQUE SCENE, THEN COPY DEPTH TO LIGHTING FBO FOR WATER DEPTH TEST
    m_lighting_pass->copy_lighting_color_to_opaque_texture(*this);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, m_gbuffer_pass->gbuffer().fbo_id);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, m_lighting_pass->lighting_fbo().fbo_id);
    glBlitFramebuffer(0, 0, m_screen_width, m_screen_height, 0, 0, m_screen_width, m_screen_height, GL_DEPTH_BUFFER_BIT, GL_NEAREST);
    
    // 6. WATER PASS (Renders to m_lighting_fbo, reads from it for refraction)
    glBindFramebuffer(GL_FRAMEBUFFER, m_lighting_pass->lighting_fbo().fbo_id);
    begin_gpu_pass_timer(GpuTimerPass::Water);
    m_water_pass->execute(*this, renderable_chunk_snapshots, camera);
    end_gpu_pass_timer(GpuTimerPass::Water);
    glBindVertexArray(0);  // Unbind after water pass

    // 7. SKYBOX PASS (Renders to m_lighting_fbo)
    begin_gpu_pass_timer(GpuTimerPass::Skybox);
    m_skybox_pass->execute(*this, camera);
    end_gpu_pass_timer(GpuTimerPass::Skybox);
    glBindVertexArray(0);  // Unbind after skybox pass

    // 8. FINAL BLIT TO SCREEN
    begin_gpu_pass_timer(GpuTimerPass::FinalBlit);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, m_lighting_pass->lighting_fbo().fbo_id);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0); // Default framebuffer

    // Clear the default framebuffer first to prevent artifacts
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    glBlitFramebuffer(0, 0, m_screen_width, m_screen_height, 0, 0, m_screen_width, m_screen_height, GL_COLOR_BUFFER_BIT, GL_LINEAR);
    m_last_render_pass_stats.final_blits++;
    end_gpu_pass_timer(GpuTimerPass::FinalBlit);

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    finish_gpu_pass_timer_frame();
    refresh_render_pass_metadata();
}

void RenderPipeline::on_resize(u32 new_width, u32 new_height) {
    if (new_width == 0 || new_height == 0 || (new_width == m_screen_width && new_height == m_screen_height)) return;
    m_screen_width = new_width;
    m_screen_height = new_height;
    // Reallocate the screen-sized targets, preserving formats (each init_*
    // rebuilds with the same internal formats it used at startup). The shadow
    // map is a fixed-resolution cascade array and the water caustics texture is
    // a fixed-resolution offscreen target, so neither resizes here; the water /
    // skybox / far-LOD passes read the resized G-buffer and lighting targets
    // through the shared pipeline state and pick up the new size automatically.
    m_lighting_pass->destroy_lighting_fbo();
    m_lighting_pass->init_lighting_fbo(new_width, new_height);
    m_gbuffer_pass->destroy_gbuffer();
    m_gbuffer_pass->init_gbuffer(new_width, new_height);
    m_ssao_pass->destroy_ssao();
    m_ssao_pass->init_ssao(new_width, new_height);
    m_frustumCache.valid = false;
    ++m_resize_generation;
    LUMINUMBRA_CORE_INFO("RenderPipeline resized targets to {}x{} (resize generation {})", new_width, new_height, m_resize_generation);
}

void RenderPipeline::clear_all_chunk_data() {
    // Force clear all cached chunk render data to ensure fresh uploads.
    // T-I4-16: terrain geometry lives in the shared pool; dropping the whole
    // pool releases every live slice at once. delete_chunk_slot still runs on
    // each record (a no-op for terrain since vao/vbo/ebo are 0, kept for safety).
    for (auto& [id, data] : m_chunk_render_data) {
        (void)id;
        delete_chunk_slot(data);
    }
    m_chunk_render_data.clear();
    for (auto& data : m_free_chunk_render_slots) {
        delete_chunk_slot(data);
    }
    m_free_chunk_render_slots.clear();
    m_chunk_geometry_pool.destroy();

    for (auto& [id, data] : m_water_render_data) {
        (void)id;
        delete_water_slot(data);
    }
    m_water_render_data.clear();
    for (auto& data : m_free_water_render_slots) {
        delete_water_slot(data);
    }
    m_free_water_render_slots.clear();
    LUMINUMBRA_CORE_INFO("Cleared chunk render data cache");
}

// --- INITIALIZATION ---

void RenderPipeline::init_shaders() {
    m_gbuffer_pass->init_geometry_shader(m_root_path);
    m_lighting_pass->init_shader(m_root_path);
    m_skybox_pass->init_shader(m_root_path);
    m_shadow_pass->init_shader(m_root_path);
    m_ssao_pass->init_shaders(m_root_path);
    m_water_pass->init_shader(m_root_path);
}

void RenderPipeline::init_screen_quad() {
    const float quadVertices[] = { -1.0f,  1.0f, 0.0f, 0.0f, 1.0f, -1.0f, -1.0f, 0.0f, 0.0f, 0.0f, 1.0f,  1.0f, 0.0f, 1.0f, 1.0f, 1.0f, -1.0f, 0.0f, 1.0f, 0.0f, };
    glGenVertexArrays(1, &m_screen_quad_vao);
    glGenBuffers(1, &m_screen_quad_vbo);
    label_gl_object(GL_VERTEX_ARRAY, m_screen_quad_vao, "screen_quad.vao");
    label_gl_object(GL_BUFFER, m_screen_quad_vbo, "screen_quad.vbo");
    glBindVertexArray(m_screen_quad_vao);
    glBindBuffer(GL_ARRAY_BUFFER, m_screen_quad_vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(quadVertices), quadVertices, GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 5 * sizeof(float), (void*)0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 5 * sizeof(float), (void*)(3 * sizeof(float)));
    glBindVertexArray(0);
}

// --- CLEANUP ---

void RenderPipeline::cleanup_gpu_resources() {
    if (m_farlod) {
        m_farlod->shutdown();
    }
    for (auto& [id, d] : m_chunk_render_data) {
        (void)id;
        delete_chunk_slot(d);
    }
    m_chunk_render_data.clear();
    for (auto& d : m_free_chunk_render_slots) {
        delete_chunk_slot(d);
    }
    m_free_chunk_render_slots.clear();
    // T-I4-16: release the shared terrain geometry pool (unmaps + deletes all
    // blocks) and the per-frame MDI scratch buffers so the resource registry is
    // empty after shutdown (RenderHealth empty_after_shutdown invariant).
    m_chunk_geometry_pool.destroy();
    destroy_mdi_buffers();

    for (auto& [id, d] : m_water_render_data) {
        (void)id;
        delete_water_slot(d);
    }
    m_water_render_data.clear();
    for (auto& d : m_free_water_render_slots) {
        delete_water_slot(d);
    }
    m_free_water_render_slots.clear();
    m_lighting_pass->destroy_lighting_fbo();
    m_gbuffer_pass->destroy_gbuffer();
    m_shadow_pass->destroy_shadow_map();
    m_ssao_pass->destroy_ssao();
    if (m_screen_quad_vao) { glDeleteVertexArrays(1, &m_screen_quad_vao); m_screen_quad_vao = 0; }
    if (m_screen_quad_vbo) { glDeleteBuffers(1, &m_screen_quad_vbo); m_screen_quad_vbo = 0; }
    m_skybox_pass->destroy_geometry();
    m_gbuffer_pass->destroy_instanced_static_mesh();
    m_gbuffer_pass->destroy_skinned_mesh();
    if (m_terrainTextureArray) { glDeleteTextures(1, &m_terrainTextureArray); m_terrainTextureArray = 0; }
    if (m_terrainNormalArray) { glDeleteTextures(1, &m_terrainNormalArray); m_terrainNormalArray = 0; }
    if (m_skinnedTextureArray) { glDeleteTextures(1, &m_skinnedTextureArray); m_skinnedTextureArray = 0; }
    if (m_materialLUT) { glDeleteTextures(1, &m_materialLUT); m_materialLUT = 0; }
    destroy_texture_residency();
    m_water_pass->destroy_water_fallback_textures();
    cleanup_gpu_sdf_system();
    destroy_gpu_pass_timers();
    m_gbuffer_pass->reset_shaders();
    m_lighting_pass->reset_shader();
    m_skybox_pass->reset_shader();
    m_shadow_pass->reset_shader();
    m_ssao_pass->reset_shaders();
    m_water_pass->reset_shader();
    m_last_render_pass_metadata.clear();
    m_terrain_texture_fallback_layers = 0;
    m_started = false;
}

// --- RESOURCE MANAGEMENT ---

void RenderPipeline::manage_chunk_gpu_resources(const std::vector<ChunkMeshSnapshot>& renderable_chunks, const Camera& camera) {
    // --- Configuration for the new logic ---
    constexpr std::size_t kMinTerrainUploadsPerFrame = 8;
    constexpr std::size_t kMaxTerrainUploadsPerFrame = 64;
    const u32 INACTIVE_FRAME_TTL = 15;   // Grace period: unload after 15 frames of inactivity

    // Step 1: Create a quick-lookup set of chunks that should be active this frame.
    std::unordered_set<ChunkID> active_chunk_ids;
    active_chunk_ids.reserve(renderable_chunks.size());
    for (const auto& chunk : renderable_chunks) {
        active_chunk_ids.insert(chunk.id);
    }

    // Step 2: Mark-and-sweep stale GPU resources.
    // Instead of unloading immediately, we mark chunks as inactive and give them a time-to-live (TTL).
    std::vector<ChunkID> gpu_chunks_to_unload;
    for (auto& [id, render_data] : m_chunk_render_data) {
        if (active_chunk_ids.count(id)) {
            // This chunk is active. Reset its inactive counter.
            render_data.frames_since_inactive = 0;
        } else {
            // This chunk is no longer in the active set. Increment its inactive counter.
            render_data.frames_since_inactive++;
            // If it's been inactive for too long, schedule it for deletion.
            if (render_data.frames_since_inactive > INACTIVE_FRAME_TTL) {
                gpu_chunks_to_unload.push_back(id);
            }
        }
    }

    // Now, perform the actual unloading of chunks that have expired.
    for (const ChunkID id : gpu_chunks_to_unload) {
        unload_chunk_resources(id);
    }

    struct TerrainUploadCandidate {
        ChunkMeshSnapshot chunk;
        bool is_new = false;
        bool is_stale = false;
        float distance_sq = 0.0f;
    };

    std::vector<TerrainUploadCandidate> upload_candidates;
    upload_candidates.reserve(renderable_chunks.size());
    for (const auto& chunk : renderable_chunks) {
        if (!chunk.has_terrain_mesh()) continue;

        auto it = m_chunk_render_data.find(chunk.id);

        bool is_new = (it == m_chunk_render_data.end());
        bool is_stale = !is_new && (it->second.mesh_version != chunk.mesh_version);

        if (is_new || is_stale) {
            const glm::vec3 center(
                chunk.coords.x * CHUNK_SIZE_X + CHUNK_SIZE_X * 0.5f,
                chunk.coords.y * CHUNK_SIZE_Y + CHUNK_SIZE_Y * 0.5f,
                chunk.coords.z * CHUNK_SIZE_Z + CHUNK_SIZE_Z * 0.5f
            );
            const glm::vec3 delta = center - camera.Position;
            upload_candidates.push_back({chunk, is_new, is_stale, glm::dot(delta, delta)});
        }
    }

    m_last_mesh_upload_stats.terrain_upload_candidates = upload_candidates.size();
    if (!upload_candidates.empty()) {
        float nearest_candidate = std::numeric_limits<float>::max();
        for (const TerrainUploadCandidate& candidate : upload_candidates) {
            nearest_candidate = std::min(nearest_candidate, candidate.distance_sq);
            if (candidate.is_new) {
                ++m_last_mesh_upload_stats.terrain_new_upload_candidates;
            } else if (candidate.is_stale) {
                ++m_last_mesh_upload_stats.terrain_stale_upload_candidates;
            }
        }
        m_last_mesh_upload_stats.terrain_nearest_candidate_distance_sq = nearest_candidate;
    }
    std::sort(upload_candidates.begin(), upload_candidates.end(), [](const TerrainUploadCandidate& a, const TerrainUploadCandidate& b) {
        if (a.distance_sq != b.distance_sq) {
            return a.distance_sq < b.distance_sq;
        }
        if (a.is_new != b.is_new) {
            return a.is_new;
        }
        return a.chunk.id < b.chunk.id;
    });

    std::size_t upload_budget_limit = kMinTerrainUploadsPerFrame;
    if (upload_candidates.size() > 2048u) {
        upload_budget_limit = kMaxTerrainUploadsPerFrame;
    } else if (upload_candidates.size() > 1024u) {
        upload_budget_limit = 48u;
    } else if (upload_candidates.size() > 512u) {
        upload_budget_limit = 32u;
    } else if (upload_candidates.size() > 128u) {
        upload_budget_limit = 16u;
    }
    const std::size_t upload_budget = std::min(upload_candidates.size(), upload_budget_limit);
    float farthest_selected_distance = 0.0f;
    for (std::size_t i = 0; i < upload_budget; ++i) {
        const TerrainUploadCandidate& candidate = upload_candidates[i];
        if (candidate.is_new) {
            ++m_last_mesh_upload_stats.terrain_new_uploads_selected;
        } else if (candidate.is_stale) {
            ++m_last_mesh_upload_stats.terrain_stale_uploads_selected;
        }
        farthest_selected_distance = std::max(farthest_selected_distance, candidate.distance_sq);
        const ChunkMeshSnapshot& chunk = upload_candidates[i].chunk;
        ChunkMeshPayload payload;
        if (!copy_terrain_mesh_payload(chunk, payload)) {
            m_last_mesh_upload_stats.terrain_upload_failures++;
            continue;
        }

        m_last_mesh_upload_stats.terrain_payload_copies++;
        m_last_mesh_upload_stats.terrain_payload_bytes +=
            payload.vertices.size() * sizeof(VoxelVertex) + payload.indices.size() * sizeof(u32);
        upload_chunk_mesh(chunk, payload);
        m_last_mesh_upload_stats.terrain_uploads++;
    }
    m_last_mesh_upload_stats.terrain_uploads_deferred = upload_candidates.size() - m_last_mesh_upload_stats.terrain_uploads;
    m_last_mesh_upload_stats.terrain_farthest_selected_distance_sq = farthest_selected_distance;
    if (upload_candidates.size() > upload_budget) {
        float nearest_deferred = std::numeric_limits<float>::max();
        for (std::size_t i = upload_budget; i < upload_candidates.size(); ++i) {
            const TerrainUploadCandidate& candidate = upload_candidates[i];
            if (candidate.is_new) {
                ++m_last_mesh_upload_stats.terrain_new_uploads_deferred;
            } else if (candidate.is_stale) {
                ++m_last_mesh_upload_stats.terrain_stale_uploads_deferred;
            }
            nearest_deferred = std::min(nearest_deferred, candidate.distance_sq);
            if (upload_budget > 0 && candidate.distance_sq < farthest_selected_distance) {
                ++m_last_mesh_upload_stats.terrain_deferred_nearer_than_selected;
            }
        }
        m_last_mesh_upload_stats.terrain_nearest_deferred_distance_sq = nearest_deferred;
    }
}

void RenderPipeline::manage_water_gpu_resources(const std::vector<ChunkMeshSnapshot>& renderable_chunks, const Camera& camera) {
    const int MAX_UPLOADS_PER_FRAME = 8; // Increased to match chunk upload budget
    const u32 INACTIVE_FRAME_TTL = 30; // A slightly longer TTL for water as it may be just off-screen

    // Step 1: Identify all chunks that should have active GPU resources
    std::unordered_set<ChunkID> active_chunk_ids;
    active_chunk_ids.reserve(renderable_chunks.size());
    for (const auto& chunk : renderable_chunks) {
        if (chunk.has_water_mesh()) {
            active_chunk_ids.insert(chunk.id);
        }
    }

    // Step 2: Mark-and-sweep stale GPU resources
    std::vector<ChunkID> gpu_chunks_to_unload;
    for (auto& [id, render_data] : m_water_render_data) {
        if (active_chunk_ids.count(id) == 0) {
            // This GPU resource is for a chunk that is no longer renderable
            render_data.frames_since_inactive++;
            if (render_data.frames_since_inactive > INACTIVE_FRAME_TTL) {
                gpu_chunks_to_unload.push_back(id);
            }
        } else {
            // This chunk is active, reset its timer
            render_data.frames_since_inactive = 0;
        }
    }

    // Step 3: Unload expired resources
    for (const ChunkID id : gpu_chunks_to_unload) {
        unload_water_resources(id);
    }

    struct WaterUploadCandidate {
        ChunkMeshSnapshot chunk;
        bool is_new = false;
        bool is_stale = false;
        float distance_sq = 0.0f;
    };

    std::vector<WaterUploadCandidate> upload_candidates;
    upload_candidates.reserve(renderable_chunks.size());
    for (const auto& chunk : renderable_chunks) {
        if (!chunk.has_water_mesh()) continue; // Skip chunks with no water

        auto it = m_water_render_data.find(chunk.id);
        bool is_new = (it == m_water_render_data.end());
        bool is_stale = !is_new && (it->second.mesh_version != chunk.water_mesh_version);

        if (is_new || is_stale) {
            const glm::vec3 center(
                chunk.coords.x * CHUNK_SIZE_X + CHUNK_SIZE_X * 0.5f,
                chunk.coords.y * CHUNK_SIZE_Y + CHUNK_SIZE_Y * 0.5f,
                chunk.coords.z * CHUNK_SIZE_Z + CHUNK_SIZE_Z * 0.5f
            );
            const glm::vec3 delta = center - camera.Position;
            upload_candidates.push_back({chunk, is_new, is_stale, glm::dot(delta, delta)});
        }
    }

    m_last_mesh_upload_stats.water_upload_candidates = upload_candidates.size();
    if (!upload_candidates.empty()) {
        float nearest_candidate = std::numeric_limits<float>::max();
        for (const WaterUploadCandidate& candidate : upload_candidates) {
            nearest_candidate = std::min(nearest_candidate, candidate.distance_sq);
            if (candidate.is_new) {
                ++m_last_mesh_upload_stats.water_new_upload_candidates;
            } else if (candidate.is_stale) {
                ++m_last_mesh_upload_stats.water_stale_upload_candidates;
            }
        }
        m_last_mesh_upload_stats.water_nearest_candidate_distance_sq = nearest_candidate;
    }
    std::sort(upload_candidates.begin(), upload_candidates.end(), [](const WaterUploadCandidate& a, const WaterUploadCandidate& b) {
        if (a.distance_sq != b.distance_sq) {
            return a.distance_sq < b.distance_sq;
        }
        if (a.is_new != b.is_new) {
            return a.is_new;
        }
        return a.chunk.id < b.chunk.id;
    });

    const std::size_t upload_budget = std::min(upload_candidates.size(), static_cast<std::size_t>(MAX_UPLOADS_PER_FRAME));
    float farthest_selected_distance = 0.0f;
    for (std::size_t i = 0; i < upload_budget; ++i) {
        const WaterUploadCandidate& candidate = upload_candidates[i];
        if (candidate.is_new) {
            ++m_last_mesh_upload_stats.water_new_uploads_selected;
        } else if (candidate.is_stale) {
            ++m_last_mesh_upload_stats.water_stale_uploads_selected;
        }
        farthest_selected_distance = std::max(farthest_selected_distance, candidate.distance_sq);
        const ChunkMeshSnapshot& chunk = upload_candidates[i].chunk;
        ChunkMeshPayload payload;
        if (!copy_water_mesh_payload(chunk, payload)) {
            m_last_mesh_upload_stats.water_upload_failures++;
            continue;
        }

        m_last_mesh_upload_stats.water_payload_copies++;
        m_last_mesh_upload_stats.water_payload_bytes +=
            payload.vertices.size() * sizeof(VoxelVertex) + payload.indices.size() * sizeof(u32);
        upload_water_mesh(chunk, payload);
        m_last_mesh_upload_stats.water_uploads++;
    }
    m_last_mesh_upload_stats.water_uploads_deferred = upload_candidates.size() - m_last_mesh_upload_stats.water_uploads;
    m_last_mesh_upload_stats.water_farthest_selected_distance_sq = farthest_selected_distance;
    if (upload_candidates.size() > upload_budget) {
        float nearest_deferred = std::numeric_limits<float>::max();
        for (std::size_t i = upload_budget; i < upload_candidates.size(); ++i) {
            const WaterUploadCandidate& candidate = upload_candidates[i];
            if (candidate.is_new) {
                ++m_last_mesh_upload_stats.water_new_uploads_deferred;
            } else if (candidate.is_stale) {
                ++m_last_mesh_upload_stats.water_stale_uploads_deferred;
            }
            nearest_deferred = std::min(nearest_deferred, candidate.distance_sq);
            if (upload_budget > 0 && candidate.distance_sq < farthest_selected_distance) {
                ++m_last_mesh_upload_stats.water_deferred_nearer_than_selected;
            }
        }
        m_last_mesh_upload_stats.water_nearest_deferred_distance_sq = nearest_deferred;
    }
}

bool RenderPipeline::copy_terrain_mesh_payload(const ChunkMeshSnapshot& chunk, ChunkMeshPayload& payload) const {
    if (!chunk.source_chunk || !chunk.has_terrain_mesh()) {
        return false;
    }

    const u32 version_before = chunk.source_chunk->mesh_version.load(std::memory_order_acquire);
    if (version_before != chunk.mesh_version) {
        return false;
    }

    payload.mesh_version = version_before;
    payload.vertices = chunk.source_chunk->mesh_vertices;
    payload.indices = chunk.source_chunk->mesh_indices;

    const u32 version_after = chunk.source_chunk->mesh_version.load(std::memory_order_acquire);
    if (version_after != version_before || !payload.has_mesh()) {
        payload.vertices.clear();
        payload.indices.clear();
        return false;
    }

    return true;
}

bool RenderPipeline::copy_water_mesh_payload(const ChunkMeshSnapshot& chunk, ChunkMeshPayload& payload) const {
    if (!chunk.source_chunk || !chunk.has_water_mesh()) {
        return false;
    }

    const u32 version_before = chunk.source_chunk->water_mesh_version.load(std::memory_order_acquire);
    if (version_before != chunk.water_mesh_version) {
        return false;
    }

    payload.mesh_version = version_before;
    payload.vertices = chunk.source_chunk->water_mesh_vertices;
    payload.indices = chunk.source_chunk->water_mesh_indices;

    const u32 version_after = chunk.source_chunk->water_mesh_version.load(std::memory_order_acquire);
    if (version_after != version_before || !payload.has_mesh()) {
        payload.vertices.clear();
        payload.indices.clear();
        return false;
    }

    return true;
}

void RenderPipeline::upload_water_mesh(const ChunkMeshSnapshot& chunk, const ChunkMeshPayload& payload) {
    if (!payload.has_mesh()) return;

    if (payload.vertices.size() > std::numeric_limits<u32>::max() ||
        payload.indices.size() > std::numeric_limits<u32>::max())
    {
        m_last_mesh_upload_stats.water_upload_failures++;
        return;
    }

    auto it = m_water_render_data.find(chunk.id);
    bool slot_created = false;
    bool slot_from_pool = false;
    if (it == m_water_render_data.end()) {
        WaterRenderData data;
        if (!m_free_water_render_slots.empty()) {
            data = m_free_water_render_slots.back();
            m_free_water_render_slots.pop_back();
            slot_from_pool = true;
        } else {
            glGenVertexArrays(1, &data.vao_id);
            glGenBuffers(1, &data.vbo_id);
            glGenBuffers(1, &data.ebo_id);
            const std::string label_prefix = "water.chunk." + std::to_string(chunk.id);
            label_gl_object(GL_VERTEX_ARRAY, data.vao_id, label_prefix + ".vao");
            label_gl_object(GL_BUFFER, data.vbo_id, label_prefix + ".vbo");
            label_gl_object(GL_BUFFER, data.ebo_id, label_prefix + ".ebo");
            slot_created = true;
        }
        it = m_water_render_data.emplace(chunk.id, data).first;
    }

    WaterRenderData& data = it->second;
    const u32 vertex_count = static_cast<u32>(payload.vertices.size());
    const u32 index_count = static_cast<u32>(payload.indices.size());
    const bool needs_growth = data.vertex_capacity < vertex_count || data.index_capacity < index_count;
    const bool version_reused = data.mesh_version != 0 && data.mesh_version != payload.mesh_version;

    glBindVertexArray(data.vao_id);
    glBindBuffer(GL_ARRAY_BUFFER, data.vbo_id);
    if (needs_growth) {
        glBufferData(GL_ARRAY_BUFFER, payload.vertices.size() * sizeof(VoxelVertex), payload.vertices.data(), GL_STATIC_DRAW);
        data.vertex_capacity = vertex_count;
    } else {
        glBufferSubData(GL_ARRAY_BUFFER, 0, payload.vertices.size() * sizeof(VoxelVertex), payload.vertices.data());
    }

    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, data.ebo_id);
    if (needs_growth) {
        glBufferData(GL_ELEMENT_ARRAY_BUFFER, payload.indices.size() * sizeof(u32), payload.indices.data(), GL_STATIC_DRAW);
        data.index_capacity = index_count;
    } else {
        glBufferSubData(GL_ELEMENT_ARRAY_BUFFER, 0, payload.indices.size() * sizeof(u32), payload.indices.data());
    }

    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(VoxelVertex), (void*)offsetof(VoxelVertex, position));
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, sizeof(VoxelVertex), (void*)offsetof(VoxelVertex, normal));
    glBindVertexArray(0);

    data.element_count = index_count;
    data.mesh_version = payload.mesh_version;
    data.frames_since_inactive = 0;

    if (slot_created) {
        m_last_mesh_upload_stats.water_slots_created++;
    } else if (needs_growth) {
        m_last_mesh_upload_stats.water_slots_grown++;
    } else if (slot_from_pool || version_reused) {
        m_last_mesh_upload_stats.water_slots_reused++;
    }
}

void RenderPipeline::upload_chunk_mesh(const ChunkMeshSnapshot& chunk, const ChunkMeshPayload& payload) {
    if (!payload.has_mesh()) { return; }

    if (payload.vertices.size() > std::numeric_limits<u32>::max() ||
        payload.indices.size() > std::numeric_limits<u32>::max())
    {
        m_last_mesh_upload_stats.terrain_upload_failures++;
        return;
    }

    // T-I4-16: terrain geometry now lives in the shared bucketed
    // persistent-mapped pool instead of a dedicated VBO/EBO/VAO per chunk.
    // ChunkRenderData stays the per-chunk LIFECYCLE record (TTL, mesh_version,
    // capacity for the distance-budgeted upload selection); it now carries a
    // pool handle (vao/vbo/ebo stay 0 for terrain). The free-slot list is no
    // longer GL-backed for terrain but is retained so the existing telemetry
    // (terrain_slots_reused) keeps meaning "record reused without a pool grow".
    auto it = m_chunk_render_data.find(chunk.id);
    bool record_created = false;
    bool record_from_pool = false;
    if (it == m_chunk_render_data.end()) {
        ChunkRenderData render_data;
        if (!m_free_chunk_render_slots.empty()) {
            render_data = m_free_chunk_render_slots.back();
            m_free_chunk_render_slots.pop_back();
            record_from_pool = true;
        } else {
            record_created = true;
        }
        // A recycled record never carries a stale pool handle: unload_chunk_resources
        // frees the pool slice and clears the handle before pushing to the free list.
        render_data.pool_handle = ChunkRenderData::kInvalidPoolHandle;
        it = m_chunk_render_data.emplace(chunk.id, render_data).first;
    }

    ChunkRenderData& render_data = it->second;
    const u32 vertex_count = static_cast<u32>(payload.vertices.size());
    const u32 index_count = static_cast<u32>(payload.indices.size());
    const bool had_allocation = render_data.pool_handle != ChunkRenderData::kInvalidPoolHandle;
    const u32 prior_vertex_capacity = render_data.vertex_capacity;
    const u32 prior_index_capacity = render_data.index_capacity;

    const std::string label_seed = std::to_string(chunk.id);
    const u32 new_handle = m_chunk_geometry_pool.update(
        render_data.pool_handle,
        payload.vertices.data(), vertex_count,
        payload.indices.data(), index_count,
        label_seed.c_str());
    if (new_handle == ChunkRenderData::kInvalidPoolHandle) {
        // Pool allocation failed (e.g. OOM): leave the record empty so the draw
        // loop skips it, and record the failure.
        render_data.element_count = 0;
        m_last_mesh_upload_stats.terrain_upload_failures++;
        if (record_created) m_last_mesh_upload_stats.terrain_slots_created++;
        return;
    }

    render_data.pool_handle = new_handle;
    const ChunkGeometryPool::Allocation& alloc = m_chunk_geometry_pool.allocation(new_handle);
    // Track the reserved slot capacity (not just the written count) so the
    // distance-budget "needs growth" intuition and the VRAM estimate match the
    // pool's actual reservation.
    render_data.vertex_capacity = alloc.vertex_slot;
    render_data.index_capacity = alloc.index_slot;
    render_data.element_count = index_count;
    render_data.mesh_version = payload.mesh_version;
    render_data.frames_since_inactive = 0;

    // Telemetry parity with the legacy slot accounting: a brand-new record is a
    // "slot created"; an existing chunk whose mesh outgrew its reserved slot
    // (so the pool reallocated, raising capacity) is a "slot grown"; an in-place
    // pool overwrite of a recycled/existing record is a "slot reused".
    const bool grew = had_allocation &&
        (render_data.vertex_capacity > prior_vertex_capacity || render_data.index_capacity > prior_index_capacity);
    if (record_created && !had_allocation) {
        m_last_mesh_upload_stats.terrain_slots_created++;
    } else if (grew) {
        m_last_mesh_upload_stats.terrain_slots_grown++;
    } else if (record_from_pool || had_allocation) {
        m_last_mesh_upload_stats.terrain_slots_reused++;
    }
}

void RenderPipeline::unload_water_resources(ChunkID chunk_id) {
    auto it = m_water_render_data.find(chunk_id);
    if (it != m_water_render_data.end()) {
        WaterRenderData data = it->second;
        data.element_count = 0;
        data.mesh_version = 0;
        data.frames_since_inactive = 0;
        if (m_free_water_render_slots.size() < kMaxFreeWaterRenderSlots) {
            m_free_water_render_slots.push_back(data);
        } else {
            delete_water_slot(data);
        }
        m_water_render_data.erase(it);
    }
}

void RenderPipeline::unload_chunk_resources(ChunkID chunk_id) {
    auto it = m_chunk_render_data.find(chunk_id);
    if (it != m_chunk_render_data.end()) {
        ChunkRenderData data = it->second;
        // T-I4-16: return the pool slice before recycling the record. The
        // recycled record must NOT carry a stale handle into the free list.
        if (data.pool_handle != ChunkRenderData::kInvalidPoolHandle) {
            m_chunk_geometry_pool.free(data.pool_handle);
            data.pool_handle = ChunkRenderData::kInvalidPoolHandle;
        }
        data.element_count = 0;
        data.mesh_version = 0;
        data.vertex_capacity = 0;
        data.index_capacity = 0;
        data.frames_since_inactive = 0;
        if (m_free_chunk_render_slots.size() < kMaxFreeChunkRenderSlots) {
            m_free_chunk_render_slots.push_back(data);
        } else {
            delete_chunk_slot(data);
        }
        m_chunk_render_data.erase(it);
    }
}

// --- HELPERS ---
void RenderPipeline::init_terrain_textures() {
    // T-I4-7: triplanar terrain fidelity. Two GL_TEXTURE_2D_ARRAY objects keyed
    // by the material-LUT layer indices (design §3/§10): an sRGB albedo array
    // and a linear tangent-space (OpenGL convention) normal-map array. The
    // committed 256x256 .ltex plates carry their own box-filtered mip chains, so
    // the whole terrain set stays far inside the 96 MB residency budget. Texture
    // arrays (not bindless) per design §10.
    //
    // Layer order matches the material LUT texture_layer/normal_layer columns:
    //   layer 0 Stone, 1 Soil, 2 Grass, 3 Sand, 4 Deepslate.
    struct TerrainLayerAssets {
        const char* albedo;
        const char* normal;
    };
    const std::array<TerrainLayerAssets, 5> assets = {{
        {"data/textures/terrain/rock/stone_albedo_256.ltex",        "data/textures/terrain/rock/stone_normal_256.ltex"},
        {"data/textures/terrain/soil/soil_albedo_256.ltex",         "data/textures/terrain/soil/soil_normal_256.ltex"},
        {"data/textures/terrain/grass/grass_albedo_256.ltex",       "data/textures/terrain/grass/grass_normal_256.ltex"},
        {"data/textures/terrain/sand/sand_albedo_256.ltex",         "data/textures/terrain/sand/sand_normal_256.ltex"},
        {"data/textures/terrain/deepslate/deepslate_albedo_256.ltex","data/textures/terrain/deepslate/deepslate_normal_256.ltex"},
    }};

    const int res = kTerrainTextureResolution;
    const int layer_count = static_cast<int>(assets.size());
    m_terrain_texture_fallback_layers = 0;
    m_material_texture_lut.terrain_layer_count = layer_count;

    // Uploads a .ltex (or a checker fallback) into one layer of a bound array,
    // mip level by mip level. Returns true if the .ltex loaded cleanly.
    auto upload_layer = [&](const std::filesystem::path& rel, int layer,
                            bool is_normal) -> bool {
        LtexCpuImage img;
        if (load_ltex_cpu_image(m_root_path / rel, img) &&
            img.width == static_cast<uint32_t>(res) &&
            img.height == static_cast<uint32_t>(res) && img.channels == 4u) {
            // Upload mip 0 only; the array is allocated with glTexImage3D (which
            // reserves level 0) and glGenerateMipmap below rebuilds the chain.
            // (The .ltex carries pre-built mips, but uploading them to an array
            // that only has level 0 storage is a GL error, and generated mips
            // are equivalent here.)
            glTexSubImage3D(GL_TEXTURE_2D_ARRAY, 0, 0, 0, layer,
                            static_cast<GLsizei>(img.width), static_cast<GLsizei>(img.height), 1,
                            GL_RGBA, GL_UNSIGNED_BYTE, img.bytes.data());
            return true;
        }
        // Fallback: magenta checker (albedo) or flat up-normal (normal map).
        if (is_normal) {
            std::vector<unsigned char> flat(static_cast<size_t>(res) * res * 4u);
            for (size_t p = 0; p < static_cast<size_t>(res) * res; ++p) {
                flat[p * 4 + 0] = 128; flat[p * 4 + 1] = 128;
                flat[p * 4 + 2] = 255; flat[p * 4 + 3] = 255;
            }
            glTexSubImage3D(GL_TEXTURE_2D_ARRAY, 0, 0, 0, layer, res, res, 1,
                            GL_RGBA, GL_UNSIGNED_BYTE, flat.data());
        } else {
            const std::vector<unsigned char> fallback = make_terrain_fallback_texture(res, res, layer);
            glTexSubImage3D(GL_TEXTURE_2D_ARRAY, 0, 0, 0, layer, res, res, 1,
                            GL_RGBA, GL_UNSIGNED_BYTE, fallback.data());
            ++m_terrain_texture_fallback_layers;
        }
        LUMINUMBRA_CORE_ERROR("Terrain texture: failed to load .ltex layer '{}'", rel.string());
        return false;
    };

    // --- Albedo array (sRGB) ---
    glGenTextures(1, &m_terrainTextureArray);
    label_gl_object(GL_TEXTURE, m_terrainTextureArray, "terrain.texture_array");
    glBindTexture(GL_TEXTURE_2D_ARRAY, m_terrainTextureArray);
    glTexImage3D(GL_TEXTURE_2D_ARRAY, 0, GL_SRGB8_ALPHA8, res, res, layer_count, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    for (int i = 0; i < layer_count; ++i) {
        upload_layer(assets[i].albedo, i, /*is_normal=*/false);
    }
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_T, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAX_LEVEL, 8); // 256 -> 1 is 9 levels
    glGenerateMipmap(GL_TEXTURE_2D_ARRAY); // build full mip chain from level 0

    // --- Normal-map array (linear RGBA8, tangent space) ---
    glGenTextures(1, &m_terrainNormalArray);
    label_gl_object(GL_TEXTURE, m_terrainNormalArray, "terrain.normal_array");
    glBindTexture(GL_TEXTURE_2D_ARRAY, m_terrainNormalArray);
    glTexImage3D(GL_TEXTURE_2D_ARRAY, 0, GL_RGBA8, res, res, layer_count, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    for (int i = 0; i < layer_count; ++i) {
        upload_layer(assets[i].normal, i, /*is_normal=*/true);
    }
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_T, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAX_LEVEL, 8);
    glGenerateMipmap(GL_TEXTURE_2D_ARRAY);
    glBindTexture(GL_TEXTURE_2D_ARRAY, 0);

    LUMINUMBRA_CORE_INFO("Terrain texture arrays loaded ({} albedo + {} normal layers, {}x{}).",
                         layer_count, layer_count, res, res);
}

void RenderPipeline::init_skinned_texture_array() {
    // T-I4-8 / T-I4-DR-split-lint: UV-mapped skinned-mesh texture array. Two
    // layers (0 = albedo, 1 = tangent-space normal). The engine allocates the
    // array with a flat fallback so a skinned mesh is always drawable; the
    // actual texture set is supplied later by the caller via
    // load_skinned_texture_set(), which reads paths from GAME data (the
    // scenario harness pulls them from the creature archetype JSON). No
    // creature/asset path is named in engine source.
    const int res = kSkinnedTextureResolution;
    constexpr int layer_count = 2;

    glGenTextures(1, &m_skinnedTextureArray);
    label_gl_object(GL_TEXTURE, m_skinnedTextureArray, "skinned.texture_array");
    glBindTexture(GL_TEXTURE_2D_ARRAY, m_skinnedTextureArray);
    glTexImage3D(GL_TEXTURE_2D_ARRAY, 0, GL_SRGB8_ALPHA8, res, res, layer_count, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, nullptr);

    // Flat fallback: layer 0 mid-grey albedo, layer 1 up-normal.
    for (int i = 0; i < layer_count; ++i) {
        const bool is_normal = (i == 1);
        std::vector<unsigned char> fill(static_cast<size_t>(res) * res * 4u);
        for (size_t p = 0; p < static_cast<size_t>(res) * res; ++p) {
            if (is_normal) {
                fill[p*4+0] = 128; fill[p*4+1] = 128; fill[p*4+2] = 255; fill[p*4+3] = 255;
            } else {
                fill[p*4+0] = 120; fill[p*4+1] = 150; fill[p*4+2] = 90; fill[p*4+3] = 255;
            }
        }
        glTexSubImage3D(GL_TEXTURE_2D_ARRAY, 0, 0, 0, i, res, res, 1,
                        GL_RGBA, GL_UNSIGNED_BYTE, fill.data());
    }
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_T, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAX_LEVEL, 8);
    glGenerateMipmap(GL_TEXTURE_2D_ARRAY);
    glBindTexture(GL_TEXTURE_2D_ARRAY, 0);

    // Layers are valid (flat fallback) even before a set is loaded.
    m_skinnedAlbedoLayer = 0;
    m_skinnedNormalLayer = 1;
    LUMINUMBRA_CORE_INFO("Skinned texture array allocated ({} layers, {}x{}, flat fallback).",
                         layer_count, res, res);
}

bool RenderPipeline::load_skinned_texture_set(const std::filesystem::path& albedo_path,
                                              const std::filesystem::path& normal_path,
                                              int& albedo_layer_out, int& normal_layer_out) {
    // Generic, data-driven loader (T-I4-DR-split-lint). Uploads the supplied
    // albedo into layer 0 and normal into layer 1; a missing/mismatched file
    // leaves that layer's existing fallback in place. Callers (scenario harness)
    // pass paths read from game data, so no content name lives in engine source.
    if (m_skinnedTextureArray == 0) {
        init_skinned_texture_array();
    }
    const int res = kSkinnedTextureResolution;
    glBindTexture(GL_TEXTURE_2D_ARRAY, m_skinnedTextureArray);

    struct SetLayer { const std::filesystem::path& path; int layer; };
    const std::array<SetLayer, 2> set = {{ {albedo_path, 0}, {normal_path, 1} }};
    bool albedo_ok = false;
    for (const auto& s : set) {
        if (s.path.empty()) continue;
        LtexCpuImage img;
        if (load_ltex_cpu_image(s.path, img) &&
            img.width == static_cast<uint32_t>(res) &&
            img.height == static_cast<uint32_t>(res) && img.channels == 4u) {
            glTexSubImage3D(GL_TEXTURE_2D_ARRAY, 0, 0, 0, s.layer,
                            static_cast<GLsizei>(img.width), static_cast<GLsizei>(img.height), 1,
                            GL_RGBA, GL_UNSIGNED_BYTE, img.bytes.data());
            if (s.layer == 0) albedo_ok = true;
        } else {
            LUMINUMBRA_CORE_WARN("Skinned texture set: failed to load '{}', keeping flat fallback.",
                                 s.path.string());
        }
    }
    glGenerateMipmap(GL_TEXTURE_2D_ARRAY);
    glBindTexture(GL_TEXTURE_2D_ARRAY, 0);

    m_skinnedAlbedoLayer = 0;
    m_skinnedNormalLayer = 1;
    albedo_layer_out = m_skinnedAlbedoLayer;
    normal_layer_out = m_skinnedNormalLayer;
    LUMINUMBRA_CORE_INFO("Skinned texture set loaded (albedo {}, {}x{}).",
                         albedo_ok ? "textured" : "fallback", res, res);
    return albedo_ok;
}

void RenderPipeline::load_material_texture_lut() {
    // Parse the texture_layer/normal_layer/tiling columns from materials.json
    // (design §3). Defaults (untextured/flat, tiling 4) are kept for any
    // material that omits the columns or for a missing file.
    m_material_texture_lut = MaterialTextureLut{};
    const std::filesystem::path path = m_root_path / "data/common/materials.json";
    std::ifstream in(path);
    if (!in) {
        LUMINUMBRA_CORE_WARN("Material texture LUT: materials.json not found at '{}', terrain stays untextured.", path.string());
        return;
    }
    try {
        nlohmann::json doc = nlohmann::json::parse(in);
        if (!doc.contains("materials") || !doc["materials"].is_array()) {
            LUMINUMBRA_CORE_WARN("Material texture LUT: materials.json missing 'materials' array.");
            return;
        }
        int textured = 0;
        for (const auto& mat : doc["materials"]) {
            if (!mat.contains("id")) continue;
            const int id = mat["id"].get<int>();
            if (id < 0 || id >= 256) continue;
            if (mat.contains("texture_layer")) {
                m_material_texture_lut.texture_layer[static_cast<size_t>(id)] = mat["texture_layer"].get<int>();
                if (m_material_texture_lut.texture_layer[static_cast<size_t>(id)] >= 0) ++textured;
            }
            if (mat.contains("normal_layer")) {
                m_material_texture_lut.normal_layer[static_cast<size_t>(id)] = mat["normal_layer"].get<int>();
            }
            if (mat.contains("tiling")) {
                const float t = mat["tiling"].get<float>();
                if (t > 0.0f) m_material_texture_lut.tiling[static_cast<size_t>(id)] = t;
            }
            if (mat.contains("emissive_intensity")) {
                m_material_texture_lut.emissive_intensity[static_cast<size_t>(id)] =
                    std::max(0.0f, mat["emissive_intensity"].get<float>());
            } else if (mat.contains("emission")) {
                // A material with authored emission but no explicit intensity
                // defaults to unit intensity so legacy emissive materials glow.
                m_material_texture_lut.emissive_intensity[static_cast<size_t>(id)] = 1.0f;
            }
            if (mat.contains("roughness")) {
                m_material_texture_lut.roughness[static_cast<size_t>(id)] =
                    glm::clamp(mat["roughness"].get<float>(), 0.0f, 1.0f);
                m_material_texture_lut.roughness_set[static_cast<size_t>(id)] = true;
            }
        }
        LUMINUMBRA_CORE_INFO("Material texture LUT parsed: {} textured material(s) from materials.json.", textured);
    } catch (const std::exception& e) {
        LUMINUMBRA_CORE_WARN("Material texture LUT: failed to parse materials.json ({}); terrain stays untextured.", e.what());
        m_material_texture_lut = MaterialTextureLut{};
    }
}

void RenderPipeline::init_material_lut() {
    // Material properties LUT (T-I4-7 two rows; T-I4-9 adds row 2). Sampled by
    // material id (u = id/255) at the row centers:
    //   row 0 (v=1/6): [R metallic, G roughness, B AO, A magical-flag]
    //   row 1 (v=1/2): [R texture_layer/255, G normal_layer/255, B tiling/64,
    //                   A has_texture]
    //   row 2 (v=5/6): [R emissive_intensity/kEmissiveLutScale, G/B/A reserved]
    // The texture/emissive columns come from materials.json
    // (load_material_texture_lut). The emissive_intensity column drives the
    // emission->lighting->glow chain (T-I4-9 calibration); it is stored
    // normalized by kEmissiveLutScale so the 0..1 RGBA8 LUT covers intensities
    // up to that ceiling, and the lighting pass rescales it back.
    const int MATERIAL_COUNT = 256;
    const int ROWS = 3;

    std::vector<glm::vec4> materialData(static_cast<size_t>(MATERIAL_COUNT) * ROWS, glm::vec4(0.1f, 0.8f, 1.0f, 0.0f));
    auto row0 = [&](int id) -> glm::vec4& { return materialData[static_cast<size_t>(id)]; };
    auto row1 = [&](int id) -> glm::vec4& { return materialData[static_cast<size_t>(MATERIAL_COUNT + id)]; };
    auto row2 = [&](int id) -> glm::vec4& { return materialData[static_cast<size_t>(2 * MATERIAL_COUNT + id)]; };

    // Row 0 (metallic / roughness / AO / magical). T-I4-10: the G (roughness)
    // channel is now DRIVEN by the materials.json roughness column (default 0.85)
    // via the parsed LUT; metallic/AO/magical keep their authored values. The
    // roughness feeds the G-buffer and the lighting specular response.
    row0(0) = glm::vec4(0.1f, 0.8f, 1.0f, 0.0f);   // Air/Default
    row0(1) = glm::vec4(0.05f, 0.85f, 1.0f, 0.0f); // Stone
    row0(2) = glm::vec4(0.0f, 0.9f, 1.0f, 0.0f);   // Soil
    row0(3) = glm::vec4(0.0f, 0.8f, 1.0f, 0.0f);   // Grass
    row0(4) = glm::vec4(0.0f, 0.75f, 1.0f, 0.0f);  // Sand
    row0(5) = glm::vec4(0.02f, 0.95f, 1.0f, 0.0f); // Deepslate
    row0(6) = glm::vec4(0.1f, 0.05f, 1.0f, 1.0f);  // Luminous Crystal (magical in alpha)
    row0(7) = glm::vec4(0.0f, 0.1f, 1.0f, 0.0f);   // Water
    // T-I4-DR-far-water-exposure: far-water sheet (id 200,
    // FarLodSystem::kFarWaterMaterialId). Without an explicit row this id
    // inherited the unknown-material defaults below (metallic 0.1, roughness
    // 0.85), so an up-facing sheet at noon picked up a broad white specular lobe
    // (F0 = mix(0.04, albedo, 0.1)) that, on top of the already-clipping diffuse,
    // washed the sheet toward warm-white. The deep-water albedo fix in
    // g_buffer.frag relies on a FULLY matte surface: metallic 0 (no metal F0
    // toward albedo), roughness 1.0, AO 1.0. Any glossier setting (0.10 and
    // 0.55 were both tried) turns the sheet into a sun-colored mirror at
    // grazing incidence - the stations that matter view the sea near-grazing,
    // Fresnel rises toward 1 there, and the white sun specular swamps the dark
    // blue diffuse. The sheet is a flat SKY-REFLECTION-TINT approximation by
    // design (see the g_buffer.frag case-200 comment), so it carries its look
    // entirely in the albedo. Set BEFORE the data-driven roughness override so
    // the override's "unknown -> 0.85" branch does not stomp the explicit
    // roughness (id 200 is render-only and never appears in materials.json).
    row0(200) = glm::vec4(0.0f, 1.0f, 1.0f, 0.0f); // Far-water sheet (matte sky-tint water)
    // Override the roughness channel from the data-driven column. Materials that
    // do not declare roughness keep the 0.85 default (matching the authored
    // values above for the common terrain ids).
    for (int id = 0; id < MATERIAL_COUNT; ++id) {
        if (id == 200) {
            // T-I4-DR-far-water-exposure: explicit far-water row authored above;
            // it is not a materials.json material, so skip the data-driven /
            // unknown-default roughness override that would force it to 0.85.
            continue;
        }
        if (m_material_texture_lut.roughness_set[static_cast<size_t>(id)]) {
            row0(id).g = m_material_texture_lut.roughness[static_cast<size_t>(id)];
        } else if (id != 0) {
            // Unknown materials default to 0.85 (design §3 roughness default).
            row0(id).g = 0.85f;
        }
    }

    // Rows 1 + 2 (texture + emissive columns) — baked from the parsed LUT.
    for (int id = 0; id < MATERIAL_COUNT; ++id) {
        const int tl = m_material_texture_lut.texture_layer[static_cast<size_t>(id)];
        const int nl = m_material_texture_lut.normal_layer[static_cast<size_t>(id)];
        const float tiling = m_material_texture_lut.tiling[static_cast<size_t>(id)];
        const bool has_tex = tl >= 0;
        row1(id) = glm::vec4(
            has_tex ? static_cast<float>(tl) / 255.0f : 0.0f,
            (nl >= 0) ? static_cast<float>(nl) / 255.0f : 0.0f,
            glm::clamp(tiling / 64.0f, 0.0f, 1.0f),
            has_tex ? 1.0f : 0.0f);
        const float ei = m_material_texture_lut.emissive_intensity[static_cast<size_t>(id)];
        row2(id) = glm::vec4(glm::clamp(ei / kEmissiveLutScale, 0.0f, 1.0f), 0.0f, 0.0f, 0.0f);
    }

    glGenTextures(1, &m_materialLUT);
    label_gl_object(GL_TEXTURE, m_materialLUT, "terrain.material_lut");
    glBindTexture(GL_TEXTURE_2D, m_materialLUT);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, MATERIAL_COUNT, ROWS, 0, GL_RGBA, GL_FLOAT, materialData.data());
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    LUMINUMBRA_CORE_INFO("Material LUT initialized ({} materials x {} rows).", MATERIAL_COUNT, ROWS);
}

// --- TEXTURE-ARRAY RESIDENCY MANAGER (T-I4-6) ---

namespace {

constexpr uint32_t kLtexMagic = 0x5845544C; // 'LTEX' little-endian
constexpr uint16_t kLtexVersion = 1;
// Layers reserved per size-class array. Over-provisioning is cheap (storage is
// allocated lazily per uploaded layer in this iteration; growth reallocates).
constexpr uint32_t kResidencyArrayInitialCapacity = 8;

template <typename T>
bool ReadPod(std::ifstream& in, T& value) {
    in.read(reinterpret_cast<char*>(&value), sizeof(T));
    return static_cast<bool>(in);
}

} // namespace

bool RenderPipeline::load_ltex_cpu_image(const std::filesystem::path& path, LtexCpuImage& out) const {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        LUMINUMBRA_CORE_ERROR("Texture residency: could not open .ltex '{}'", path.string());
        return false;
    }

    uint32_t magic = 0;
    uint16_t version = 0;
    uint16_t mip_count = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    uint8_t channels = 0;
    if (!ReadPod(in, magic) || !ReadPod(in, version) || !ReadPod(in, mip_count) ||
        !ReadPod(in, width) || !ReadPod(in, height) || !ReadPod(in, channels)) {
        LUMINUMBRA_CORE_ERROR("Texture residency: truncated .ltex header '{}'", path.string());
        return false;
    }
    if (magic != kLtexMagic) {
        LUMINUMBRA_CORE_ERROR("Texture residency: bad .ltex magic in '{}'", path.string());
        return false;
    }
    if (version != kLtexVersion) {
        LUMINUMBRA_CORE_ERROR("Texture residency: unsupported .ltex version {} in '{}'", version, path.string());
        return false;
    }
    if (width == 0 || height == 0 || channels == 0 || channels > 4 || mip_count == 0) {
        LUMINUMBRA_CORE_ERROR("Texture residency: invalid .ltex dimensions in '{}'", path.string());
        return false;
    }

    // Compute the total mip-chain byte count (dimensions halve, floored, min 1).
    size_t total_bytes = 0;
    {
        uint32_t w = width;
        uint32_t h = height;
        for (uint16_t level = 0; level < mip_count; ++level) {
            total_bytes += static_cast<size_t>(w) * h * channels;
            w = std::max(1u, w / 2u);
            h = std::max(1u, h / 2u);
        }
    }

    out.width = width;
    out.height = height;
    out.channels = channels;
    out.mip_count = mip_count;
    out.bytes.resize(total_bytes);
    in.read(reinterpret_cast<char*>(out.bytes.data()), static_cast<std::streamsize>(total_bytes));
    if (!in) {
        LUMINUMBRA_CORE_ERROR("Texture residency: truncated .ltex mip data '{}'", path.string());
        return false;
    }
    return true;
}

bool RenderPipeline::upload_ltex_to_residency(const std::string& name, const std::filesystem::path& path) {
    if (m_texture_residency.layer_by_name.count(name)) {
        LUMINUMBRA_CORE_WARN("Texture residency: '{}' already resident; skipping", name);
        return true;
    }

    LtexCpuImage image;
    if (!load_ltex_cpu_image(path, image)) {
        return false;
    }

    // Budget gate: reject an upload that would exceed the resident budget.
    const size_t upload_bytes = image.bytes.size();
    if (m_texture_residency.resident_bytes + upload_bytes > kTextureResidentBudgetBytes) {
        LUMINUMBRA_CORE_ERROR(
            "Texture residency: uploading '{}' ({} bytes) would exceed the {} MB budget (resident {} bytes); rejected",
            name, upload_bytes, kTextureResidentBudgetBytes / (1024u * 1024u), m_texture_residency.resident_bytes);
        return false;
    }

    const std::string size_class_key =
        std::to_string(image.width) + "x" + std::to_string(image.height) + "x" + std::to_string(image.channels);

    // Find or create the size-class array. A size class is keyed by
    // {width, height, channels}; mip_count is implied by the dimensions.
    size_t array_index = m_texture_residency.arrays.size();
    for (size_t i = 0; i < m_texture_residency.arrays.size(); ++i) {
        if (m_texture_residency.arrays[i].size_class_key == size_class_key) {
            array_index = i;
            break;
        }
    }

    const GLenum internal_format = (image.channels == 4) ? GL_SRGB8_ALPHA8 : GL_RGBA8;
    const GLenum upload_format = GL_RGBA;

    if (array_index == m_texture_residency.arrays.size()) {
        TextureResidencyArray array;
        array.width = image.width;
        array.height = image.height;
        array.channels = image.channels;
        array.mip_count = image.mip_count;
        array.layer_capacity = kResidencyArrayInitialCapacity;
        array.size_class_key = size_class_key;
        glGenTextures(1, &array.texture_id);
        label_gl_object(GL_TEXTURE, array.texture_id, "residency.array." + size_class_key);
        glBindTexture(GL_TEXTURE_2D_ARRAY, array.texture_id);
        glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
        glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_S, GL_REPEAT);
        glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_T, GL_REPEAT);
        glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_BASE_LEVEL, 0);
        glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAX_LEVEL, image.mip_count - 1);
        // Allocate immutable-style storage for the size class across all mips.
        uint32_t w = image.width;
        uint32_t h = image.height;
        for (uint16_t level = 0; level < image.mip_count; ++level) {
            glTexImage3D(GL_TEXTURE_2D_ARRAY, level, internal_format,
                         static_cast<GLsizei>(w), static_cast<GLsizei>(h),
                         static_cast<GLsizei>(array.layer_capacity), 0,
                         upload_format, GL_UNSIGNED_BYTE, nullptr);
            w = std::max(1u, w / 2u);
            h = std::max(1u, h / 2u);
        }
        m_texture_residency.arrays.push_back(array);
    }

    TextureResidencyArray& array = m_texture_residency.arrays[array_index];
    glBindTexture(GL_TEXTURE_2D_ARRAY, array.texture_id);

    const uint32_t layer = array.layer_count;
    // Upload each mip level of the new layer from the CPU mip chain.
    uint32_t w = image.width;
    uint32_t h = image.height;
    size_t offset = 0;
    for (uint16_t level = 0; level < image.mip_count; ++level) {
        const size_t level_bytes = static_cast<size_t>(w) * h * image.channels;
        glTexSubImage3D(GL_TEXTURE_2D_ARRAY, level, 0, 0, static_cast<GLint>(layer),
                        static_cast<GLsizei>(w), static_cast<GLsizei>(h), 1,
                        upload_format, GL_UNSIGNED_BYTE, image.bytes.data() + offset);
        offset += level_bytes;
        w = std::max(1u, w / 2u);
        h = std::max(1u, h / 2u);
    }

    array.layer_count += 1;
    array.resident_bytes += upload_bytes;
    m_texture_residency.resident_bytes += upload_bytes;
    m_texture_residency.layer_by_name[name] = TextureResidencyLayer{array_index, layer};
    return true;
}

void RenderPipeline::init_texture_residency() {
    // Load the committed iteration-4 .ltex test assets. Nothing samples these
    // layers yet; T-I4-7 wires shaders to the material-LUT layer indices.
    const std::array<std::pair<const char*, const char*>, 3> assets = {{
        {"test/checker", "data/textures/test/checker_16.ltex"},
        {"test/gradient", "data/textures/test/gradient_16.ltex"},
        {"test/framed", "data/textures/test/framed_32x8.ltex"},
    }};

    size_t loaded = 0;
    for (const auto& [name, rel_path] : assets) {
        const std::filesystem::path path = m_root_path / rel_path;
        if (!std::filesystem::exists(path)) {
            // Missing optional test asset is not fatal — the residency manager
            // stays empty and telemetry reports zero resident bytes.
            LUMINUMBRA_CORE_WARN("Texture residency: .ltex asset not found '{}'", path.string());
            continue;
        }
        if (upload_ltex_to_residency(name, path)) {
            ++loaded;
        }
    }

    LUMINUMBRA_CORE_INFO(
        "Texture residency: {} layer(s) across {} size-class array(s), {} resident bytes (budget {} MB).",
        m_texture_residency.layer_by_name.size(), m_texture_residency.arrays.size(),
        m_texture_residency.resident_bytes, kTextureResidentBudgetBytes / (1024u * 1024u));
    (void)loaded;
}

void RenderPipeline::destroy_texture_residency() {
    for (TextureResidencyArray& array : m_texture_residency.arrays) {
        if (array.texture_id) {
            glDeleteTextures(1, &array.texture_id);
            array.texture_id = 0;
        }
    }
    m_texture_residency.arrays.clear();
    m_texture_residency.layer_by_name.clear();
    m_texture_residency.resident_bytes = 0;
}

bool RenderPipeline::find_resident_texture_layer(const std::string& name, size_t& out_array_index, uint32_t& out_layer) const {
    const auto it = m_texture_residency.layer_by_name.find(name);
    if (it == m_texture_residency.layer_by_name.end()) {
        return false;
    }
    out_array_index = it->second.array_index;
    out_layer = it->second.layer;
    return true;
}

// --- GPU SDF GENERATION SYSTEM ---

GLuint create_compute_shader(const char* source) {
    GLuint shader = glCreateShader(GL_COMPUTE_SHADER);
    glShaderSource(shader, 1, &source, nullptr);
    glCompileShader(shader);
    
    GLint success;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &success);
    if (!success) {
        char infoLog[512];
        glGetShaderInfoLog(shader, 512, nullptr, infoLog);
        LUMINUMBRA_CORE_ERROR("Compute shader compilation failed: {}", infoLog);
        glDeleteShader(shader);
        return 0;
    }
    
    return shader;
}

GLuint create_compute_program(const char* compute_source) {
    GLuint compute_shader = create_compute_shader(compute_source);
    if (compute_shader == 0) return 0;
    
    GLuint program = glCreateProgram();
    glAttachShader(program, compute_shader);
    glLinkProgram(program);
    
    GLint success;
    glGetProgramiv(program, GL_LINK_STATUS, &success);
    if (!success) {
        char infoLog[512];
        glGetProgramInfoLog(program, 512, nullptr, infoLog);
        LUMINUMBRA_CORE_ERROR("Compute program linking failed: {}", infoLog);
        glDeleteProgram(program);
        program = 0;
    }
    
    glDeleteShader(compute_shader);
    return program;
}

void RenderPipeline::init_gpu_sdf_system() {
    // Load compute shader
    std::string compute_path = (m_root_path / "res/shaders/sdf_generation.compute").string();
    std::ifstream file(compute_path);
    if (!file.is_open()) {
        LUMINUMBRA_CORE_ERROR("Failed to load GPU SDF compute shader: {}", compute_path);
        return;
    }
    
    std::string compute_source((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    file.close();
    
    m_gpu_sdf.compute_program = create_compute_program(compute_source.c_str());
    if (m_gpu_sdf.compute_program == 0) {
        LUMINUMBRA_CORE_ERROR("Failed to create GPU SDF compute program");
        return;
    }
    label_gl_object(GL_PROGRAM, m_gpu_sdf.compute_program, "gpu_sdf.compute_program");
    
    // Create SDF buffer (17³ floats for chunk + padding)
    const size_t sdf_buffer_size = 17 * 17 * 17 * sizeof(float);
    glGenBuffers(1, &m_gpu_sdf.sdf_buffer);
    label_gl_object(GL_BUFFER, m_gpu_sdf.sdf_buffer, "gpu_sdf.sdf_buffer");
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, m_gpu_sdf.sdf_buffer);
    glBufferData(GL_SHADER_STORAGE_BUFFER, sdf_buffer_size, nullptr, GL_DYNAMIC_READ);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, m_gpu_sdf.sdf_buffer);
    
    // Generate noise textures
    generate_noise_textures();
    
    m_gpu_sdf.initialized = true;
    LUMINUMBRA_CORE_INFO("GPU SDF generation system initialized");
}

void RenderPipeline::generate_noise_textures() {
    // Generate 3D noise textures for hardware-accelerated sampling
    const int NOISE_SIZE = 128; // 128³ noise texture
    
    // Terrain noise texture (3D Simplex-like)
    std::vector<float> terrain_noise(NOISE_SIZE * NOISE_SIZE * NOISE_SIZE);
    for (int z = 0; z < NOISE_SIZE; ++z) {
        for (int y = 0; y < NOISE_SIZE; ++y) {
            for (int x = 0; x < NOISE_SIZE; ++x) {
                // Simple procedural noise - replace with FastNoise2 integration
                float nx = x / (float)NOISE_SIZE;
                float ny = y / (float)NOISE_SIZE;
                float nz = z / (float)NOISE_SIZE;
                
                // Multi-octave noise approximation
                float noise = 0.0f;
                float amplitude = 1.0f;
                float frequency = 1.0f;
                
                for (int octave = 0; octave < 4; ++octave) {
                    noise += sin(nx * frequency * 6.28f) * cos(ny * frequency * 6.28f) * sin(nz * frequency * 6.28f) * amplitude;
                    frequency *= 2.0f;
                    amplitude *= 0.5f;
                }
                
                terrain_noise[z * NOISE_SIZE * NOISE_SIZE + y * NOISE_SIZE + x] = noise * 0.5f + 0.5f; // [0,1]
            }
        }
    }
    
    glGenTextures(1, &m_gpu_sdf.terrain_noise_texture);
    label_gl_object(GL_TEXTURE, m_gpu_sdf.terrain_noise_texture, "gpu_sdf.terrain_noise");
    glBindTexture(GL_TEXTURE_3D, m_gpu_sdf.terrain_noise_texture);
    glTexImage3D(GL_TEXTURE_3D, 0, GL_R32F, NOISE_SIZE, NOISE_SIZE, NOISE_SIZE, 0, GL_RED, GL_FLOAT, terrain_noise.data());
    glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_T, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_R, GL_REPEAT);
    
    // Cave noise texture (3D Perlin-like)
    std::vector<float> cave_noise(NOISE_SIZE * NOISE_SIZE * NOISE_SIZE);
    for (int z = 0; z < NOISE_SIZE; ++z) {
        for (int y = 0; y < NOISE_SIZE; ++y) {
            for (int x = 0; x < NOISE_SIZE; ++x) {
                float nx = x / (float)NOISE_SIZE;
                float ny = y / (float)NOISE_SIZE;
                float nz = z / (float)NOISE_SIZE;
                
                // Different noise pattern for caves
                float noise = sin(nx * 4.0f) * cos(ny * 4.0f) * sin(nz * 4.0f);
                cave_noise[z * NOISE_SIZE * NOISE_SIZE + y * NOISE_SIZE + x] = noise * 0.5f + 0.5f;
            }
        }
    }
    
    glGenTextures(1, &m_gpu_sdf.cave_noise_texture);
    label_gl_object(GL_TEXTURE, m_gpu_sdf.cave_noise_texture, "gpu_sdf.cave_noise");
    glBindTexture(GL_TEXTURE_3D, m_gpu_sdf.cave_noise_texture);
    glTexImage3D(GL_TEXTURE_3D, 0, GL_R32F, NOISE_SIZE, NOISE_SIZE, NOISE_SIZE, 0, GL_RED, GL_FLOAT, cave_noise.data());
    glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_T, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_R, GL_REPEAT);
    
    // Island mask texture (2D)
    std::vector<float> island_mask(NOISE_SIZE * NOISE_SIZE);
    for (int y = 0; y < NOISE_SIZE; ++y) {
        for (int x = 0; x < NOISE_SIZE; ++x) {
            float nx = (x - NOISE_SIZE * 0.5f) / (NOISE_SIZE * 0.5f);
            float ny = (y - NOISE_SIZE * 0.5f) / (NOISE_SIZE * 0.5f);
            float dist = sqrt(nx * nx + ny * ny);
            float mask = 1.0f - glm::smoothstep(0.6f, 1.0f, dist); // Circular island
            island_mask[y * NOISE_SIZE + x] = mask;
        }
    }
    
    glGenTextures(1, &m_gpu_sdf.island_mask_texture);
    label_gl_object(GL_TEXTURE, m_gpu_sdf.island_mask_texture, "gpu_sdf.island_mask");
    glBindTexture(GL_TEXTURE_2D, m_gpu_sdf.island_mask_texture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_R32F, NOISE_SIZE, NOISE_SIZE, 0, GL_RED, GL_FLOAT, island_mask.data());
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
    
    LUMINUMBRA_CORE_INFO("Generated GPU noise textures ({}³)", NOISE_SIZE);
}

bool RenderPipeline::generate_chunk_sdf_gpu(const glm::ivec3& chunk_coords, const ::Luminumbra::Systems::TerrainGenParams& params, int seed, std::vector<float>& out_sdf) {
    if (!m_gpu_sdf.initialized) return false;
    
    // Calculate chunk world position
    glm::vec3 chunk_base_pos(
        chunk_coords.x * CHUNK_SIZE_X,
        chunk_coords.y * CHUNK_SIZE_Y,
        chunk_coords.z * CHUNK_SIZE_Z
    );
    
    // Use compute shader
    glUseProgram(m_gpu_sdf.compute_program);
    
    // Set uniforms
    glUniform3fv(glGetUniformLocation(m_gpu_sdf.compute_program, "u_chunkBasePos"), 1, &chunk_base_pos[0]);
    glUniform1f(glGetUniformLocation(m_gpu_sdf.compute_program, "u_voxelSize"), 1.0f);
    
    // Terrain parameters
    glUniform1f(glGetUniformLocation(m_gpu_sdf.compute_program, "u_baseFrequency"), params.base_frequency);
    glUniform1f(glGetUniformLocation(m_gpu_sdf.compute_program, "u_baseAmplitude"), params.base_amplitude);
    glUniform1f(glGetUniformLocation(m_gpu_sdf.compute_program, "u_heightOffset"), params.height_offset);
    glUniform1i(glGetUniformLocation(m_gpu_sdf.compute_program, "u_octaves"), params.octaves);
    glUniform1f(glGetUniformLocation(m_gpu_sdf.compute_program, "u_persistence"), params.persistence);
    glUniform1f(glGetUniformLocation(m_gpu_sdf.compute_program, "u_lacunarity"), params.lacunarity);
    
    // Cave parameters
    glUniform1i(glGetUniformLocation(m_gpu_sdf.compute_program, "u_cavesEnabled"), params.caves_enabled);
    glUniform1f(glGetUniformLocation(m_gpu_sdf.compute_program, "u_caveFrequency"), params.cave_frequency);
    glUniform1f(glGetUniformLocation(m_gpu_sdf.compute_program, "u_caveThreshold"), params.cave_threshold);
    glUniform1f(glGetUniformLocation(m_gpu_sdf.compute_program, "u_caveCarveValue"), params.cave_carve_value);
    
    // Island parameters
    glUniform1i(glGetUniformLocation(m_gpu_sdf.compute_program, "u_islandMaskEnabled"), params.island_mask_enabled);
    glUniform1f(glGetUniformLocation(m_gpu_sdf.compute_program, "u_islandMaskFrequency"), params.island_mask_frequency);
    
    // Seed
    glUniform1f(glGetUniformLocation(m_gpu_sdf.compute_program, "u_seedOffset"), static_cast<float>(seed));
    
    // Bind noise textures
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_3D, m_gpu_sdf.terrain_noise_texture);
    glUniform1i(glGetUniformLocation(m_gpu_sdf.compute_program, "u_terrainNoise"), 0);
    
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_3D, m_gpu_sdf.cave_noise_texture);
    glUniform1i(glGetUniformLocation(m_gpu_sdf.compute_program, "u_caveNoise"), 1);
    
    glActiveTexture(GL_TEXTURE2);
    glBindTexture(GL_TEXTURE_2D, m_gpu_sdf.island_mask_texture);
    glUniform1i(glGetUniformLocation(m_gpu_sdf.compute_program, "u_islandMask"), 2);
    
    // Bind SDF buffer
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, m_gpu_sdf.sdf_buffer);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, m_gpu_sdf.sdf_buffer);
    
    // Dispatch compute shader (17³ = 4913 threads, dispatch as 8x8x8 groups)
    glDispatchCompute(3, 3, 3); // ceil(17/8) = 3 for each dimension
    
    // Insert memory barrier
    glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT);
    
    // For async operation, create fence and return immediately
    if (m_gpu_sdf.compute_fence != nullptr) {
        glDeleteSync(m_gpu_sdf.compute_fence);
    }
    m_gpu_sdf.compute_fence = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
    
    // For now, do synchronous read (async version would check fence in a different frame)
    glClientWaitSync(m_gpu_sdf.compute_fence, GL_SYNC_FLUSH_COMMANDS_BIT, GL_TIMEOUT_IGNORED);
    
    // Read back results
    const size_t sdf_size = 17 * 17 * 17;
    out_sdf.resize(sdf_size);
    
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, m_gpu_sdf.sdf_buffer);
    float* mapped_data = (float*)glMapBuffer(GL_SHADER_STORAGE_BUFFER, GL_READ_ONLY);
    if (mapped_data) {
        std::memcpy(out_sdf.data(), mapped_data, sdf_size * sizeof(float));
        glUnmapBuffer(GL_SHADER_STORAGE_BUFFER);
    } else {
        LUMINUMBRA_CORE_ERROR("Failed to map GPU SDF buffer for readback");
        return false;
    }
    
    return true;
}

void RenderPipeline::cleanup_gpu_sdf_system() {
    if (m_gpu_sdf.compute_fence) {
        glDeleteSync(m_gpu_sdf.compute_fence);
        m_gpu_sdf.compute_fence = nullptr;
    }
    
    if (m_gpu_sdf.sdf_buffer) {
        glDeleteBuffers(1, &m_gpu_sdf.sdf_buffer);
        m_gpu_sdf.sdf_buffer = 0;
    }
    
    if (m_gpu_sdf.terrain_noise_texture) {
        glDeleteTextures(1, &m_gpu_sdf.terrain_noise_texture);
        m_gpu_sdf.terrain_noise_texture = 0;
    }
    
    if (m_gpu_sdf.cave_noise_texture) {
        glDeleteTextures(1, &m_gpu_sdf.cave_noise_texture);
        m_gpu_sdf.cave_noise_texture = 0;
    }
    
    if (m_gpu_sdf.island_mask_texture) {
        glDeleteTextures(1, &m_gpu_sdf.island_mask_texture);
        m_gpu_sdf.island_mask_texture = 0;
    }
    
    if (m_gpu_sdf.compute_program) {
        glDeleteProgram(m_gpu_sdf.compute_program);
        m_gpu_sdf.compute_program = 0;
    }
    
    m_gpu_sdf.initialized = false;
    m_gpu_sdf.callback_registered = false;
}

void RenderPipeline::update_time_of_day(float deltaTime) {
    m_timeOfDay += deltaTime / m_dayDurationSeconds;
    m_timeOfDay = fmod(m_timeOfDay, 1.0f);

    float sun_angle_rad = m_timeOfDay * 2.0f * glm::pi<float>();
    m_sun.direction = glm::normalize(glm::vec3(sin(sun_angle_rad), -cos(sun_angle_rad), -0.2f));

    float sun_up_factor = glm::dot(m_sun.direction, glm::vec3(0.0f, -1.0f, 0.0f));
    m_sun.intensity = glm::smoothstep(-0.1f, 0.15f, sun_up_factor);

    // T-I4-DR-tod-sky-balance: the sky dome's day/twilight/night blend is keyed
    // off the sun's RAW elevation, not the lighting intensity above.
    // m_sun.intensity saturates to 1 once the sun clears ~0.15 elevation, so a
    // sun only ~11 degrees up (dusk) still drove a full-midday dome and the
    // sub-horizon night dome stayed bright twilight-blue. This wider band keeps
    // the zenith at full day while the sun is high but falls off across the
    // low-sun arc, so dusk is a genuine partial-day value and night collapses to
    // ~0. Noon (sun_up ~0.95) stays pinned at 1.0, so the noon dome and the
    // albedo/ambient calibrations that depend on it are untouched.
    m_skyDayFactor = glm::smoothstep(-0.05f, 0.55f, sun_up_factor);

    glm::vec3 noonColor(1.0f, 0.95f, 0.85f);
    glm::vec3 horizonColor(1.0f, 0.6f, 0.2f);
    m_sun.color = glm::mix(horizonColor, noonColor, glm::smoothstep(0.0f, 0.25f, sun_up_factor)) * m_sun.intensity;
    
    m_moonDirection = -m_sun.direction;

    // Ambient scales by the same PI as SUN_IRRADIANCE_SCALE (lighting_pass
    // exposure audit): these values were tuned against the pre-audit sun, so
    // without the matching scale the sun:ambient balance collapses from ~30%
    // shadow luminance to ~4% (shadowed slopes read near-black, LodGround/
    // LodSeamRisk near_black_ratio regressions on DEM-realistic terrain).
    constexpr float kAmbientIrradianceScale = 3.14159265f;
    glm::vec3 dayAmbient = glm::vec3(0.1f, 0.15f, 0.2f) * kAmbientIrradianceScale;
    glm::vec3 nightAmbient = glm::vec3(0.01f, 0.02f, 0.04f) * kAmbientIrradianceScale;
    m_skyAmbientColor = glm::mix(nightAmbient, dayAmbient, m_sun.intensity);
}

u32 RenderPipeline::water_caustics_texture() const {
    return m_water_pass ? m_water_pass->caustics_texture() : 0u;
}

void RenderPipeline::set_time_of_day(float normalized_time) {
    m_timeOfDay = std::clamp(normalized_time, 0.0f, 1.0f);
}

void RenderPipeline::set_weather(WeatherType type, float intensity) {
    m_weather_type = type;
    m_weather_intensity = std::clamp(intensity, 0.0f, 1.0f);
}

std::vector<glm::mat4> RenderPipeline::get_light_space_matrices(const Camera& camera) {
    ShadowMap& shadow_map = m_shadow_pass->shadow_map();
    if (!has_valid_shadow_cascade_splits(shadow_map)) {
        LUMINUMBRA_CORE_ERROR("Shadow cascade splits were not initialized; restoring defaults.");
        set_default_shadow_cascade_splits(shadow_map);
    }

    std::vector<glm::mat4> matrices;
    matrices.reserve(ShadowMap::CASCADE_COUNT);
    for (int i = 0; i < ShadowMap::CASCADE_COUNT; i++) {
        float split_near = (i == 0) ? camera.GetNearPlane() : shadow_map.cascade_splits[i];
        float split_far = shadow_map.cascade_splits[i + 1];
        glm::mat4 proj = glm::perspective(glm::radians(camera.Zoom), (float)m_screen_width / (float)m_screen_height, split_near, split_far);
        glm::mat4 view = camera.GetViewMatrix();
        std::vector<glm::vec4> corners;
        for (int x = 0; x < 2; ++x) for (int y = 0; y < 2; ++y) for (int z = 0; z < 2; ++z) {
            const glm::vec4 pt = glm::inverse(proj * view) * glm::vec4(2.0f*x-1.0f, 2.0f*y-1.0f, 2.0f*z-1.0f, 1.0f);
            corners.push_back(pt / pt.w);
        }
        glm::vec3 center = glm::vec3(0.0f);
        for(const auto& v : corners) center += glm::vec3(v);
        center /= corners.size();
        glm::mat4 light_view = glm::lookAt(center - m_sun.direction, center, glm::vec3(0.0f, 1.0f, 0.0f));
        float minX = std::numeric_limits<float>::max(), maxX = std::numeric_limits<float>::lowest();
        float minY = std::numeric_limits<float>::max(), maxY = std::numeric_limits<float>::lowest();
        float minZ = std::numeric_limits<float>::max(), maxZ = std::numeric_limits<float>::lowest();
        for(const auto& v : corners) {
            const glm::vec4 trf = light_view * v;
            minX = std::min(minX, trf.x); maxX = std::max(maxX, trf.x);
            minY = std::min(minY, trf.y); maxY = std::max(maxY, trf.y);
            minZ = std::min(minZ, trf.z); maxZ = std::max(maxZ, trf.z);
        }
        constexpr float z_mult = 10.0f;
        minZ = (minZ < 0) ? minZ * z_mult : minZ / z_mult;
        maxZ = (maxZ < 0) ? maxZ / z_mult : maxZ * z_mult;
        glm::mat4 light_proj = glm::ortho(minX, maxX, minY, maxY, minZ, maxZ);
        matrices.push_back(light_proj * light_view);
    }
    return matrices;
}

} // namespace Luminumbra::Rendering
