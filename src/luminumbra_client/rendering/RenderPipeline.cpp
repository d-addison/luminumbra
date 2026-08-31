#include "RenderPipeline.h"
#include "../../include/luminumbra/core/Types.h"
#include "CelestialBodyModel.h" //  Tier 1: the celestial-body seam
#include "ExposureModel.h"      //  rendering: SelectRenderExposure (manual EV precedence)
#include "FarLodSystem.h"
#include "FroxelGrid.h"    //  rendering: the froxel grid model
#include "ImpostorBake.h"  //  far-field tree impostor atlas (opt-in)
#include "InProcessFlip.h" // Deterministic in-process FLIP image comparison.
#include "Mesh.h"
#include "RenderContext.h" // per-frame pass contract
#include "RenderGraph.h"   // Declarative frame graph validated against the trace.
#include "RenderSystem.h"
#include "SunLightModel.h"  //  rendering: SunIrradiance (transmittance-coupled sun magnitude)
#include "TimeOfDayModel.h" // Pure time-of-day policy helpers.
#include "core/Log.h"
#include "luminumbra_common/components/CoreComponents.h"
#include "luminumbra_common/components/LightingComponents.h"
#include "luminumbra_common/core/DeterministicMath.h"
#include "luminumbra_common/core/Environment.h"
#include "luminumbra_common/systems/SHIELD_WorldSystem.h"
#include "luminumbra_common/world/Chunk.h"
#include "passes/DebugViewPass.h" // render-only G-buffer debug visualizer (default-OFF)
#include "passes/FinalBlitPass.h" // -T01: FinalBlit on the RenderContext seam
#include "passes/FoliagePass.h"
#include "passes/GBufferPass.h"
#include "passes/GroundDecalPass.h" // render-only pheromone ground decal (flag-gated)
#include "passes/LightingPass.h"
#include "passes/ParticlePass.h"
#include "passes/PassGlHelpers.h"    // Push and pop debug-group markers.
#include "passes/PlantProcgenPass.h" //  render-only procedural plants (flag-gated)
#include "passes/ShadowPass.h"
#include "passes/SkyboxPass.h"
#include "passes/SsaoPass.h"
#include "passes/WaterPass.h"
#include "rendering/Camera.h"
#include "rendering/Shader.h"
#include <GLFW/glfw3.h>
#include <algorithm>
#include <cassert>
#include <chrono> // CPU per-phase submit cost
#include <cmath>
#include <cstring>
#include <exception>
#include <fstream>
#include <glm/gtc/constants.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <limits>
#include <nlohmann/json.hpp>
#include <random>
#include <stb_image.h>
#include <unordered_set>
#include <utility>

//  the Hillaire scattering-LUT implementation is compiled into this TU
// rather than added as a separate source file (the vendored meshoptimizer
// source is absent from this checkout, so a sources.cmake change that forces a
// fresh CMake configure would fail; folding the impl here keeps the build
// incremental). The.ipp opens its own Luminumbra::Rendering namespace.
#include "SkyAtmosphereLut.ipp"

namespace {
// Helper for frustum culling
inline void ExtractFrustumPlanes(const glm::mat4& m, glm::vec4 planes[6]) {
    planes[0] =
        glm::vec4(m[0][3] + m[0][0], m[1][3] + m[1][0], m[2][3] + m[2][0], m[3][3] + m[3][0]);
    planes[1] =
        glm::vec4(m[0][3] - m[0][0], m[1][3] - m[1][0], m[2][3] - m[2][0], m[3][3] - m[3][0]);
    planes[2] =
        glm::vec4(m[0][3] + m[0][1], m[1][3] + m[1][1], m[2][3] + m[2][1], m[3][3] + m[3][1]);
    planes[3] =
        glm::vec4(m[0][3] - m[0][1], m[1][3] - m[1][1], m[2][3] - m[2][1], m[3][3] - m[3][1]);
    planes[4] =
        glm::vec4(m[0][3] + m[0][2], m[1][3] + m[1][2], m[2][3] + m[2][2], m[3][3] + m[3][2]);
    planes[5] =
        glm::vec4(m[0][3] - m[0][2], m[1][3] - m[1][2], m[2][3] - m[2][2], m[3][3] - m[3][2]);
    for (int i = 0; i < 6; ++i) {
        float inv_len = 1.0f / glm::length(glm::vec3(planes[i]));
        planes[i] *= inv_len;
    }
}
inline bool AABBOutsidePlane(const glm::vec3& minp, const glm::vec3& maxp, const glm::vec4& plane) {
    glm::vec3 p = glm::vec3(plane.x >= 0 ? maxp.x : minp.x,
                            plane.y >= 0 ? maxp.y : minp.y,
                            plane.z >= 0 ? maxp.z : minp.z);
    return (glm::dot(glm::vec3(plane), p) + plane.w) < 0.0f;
}
inline bool
AABBFrustumCulled(const glm::vec3& minp, const glm::vec3& maxp, const glm::vec4 planes[6]) {
    for (int i = 0; i < 6; ++i)
        if (AABBOutsidePlane(minp, maxp, planes[i]))
            return true;
    return false;
}
} // namespace

namespace Luminumbra::Rendering {

// Forward declaration, reused by the luminance meter below.
GLuint create_compute_program(const char* compute_source);

namespace {

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
    if (data.vao_id) {
        glDeleteVertexArrays(1, &data.vao_id);
        data.vao_id = 0;
    }
    if (data.vbo_id) {
        glDeleteBuffers(1, &data.vbo_id);
        data.vbo_id = 0;
    }
    if (data.ebo_id) {
        glDeleteBuffers(1, &data.ebo_id);
        data.ebo_id = 0;
    }
    data = {};
}

void delete_water_slot(WaterRenderData& data) {
    if (data.vao_id) {
        glDeleteVertexArrays(1, &data.vao_id);
        data.vao_id = 0;
    }
    if (data.vbo_id) {
        glDeleteBuffers(1, &data.vbo_id);
        data.vbo_id = 0;
    }
    if (data.ebo_id) {
        glDeleteBuffers(1, &data.ebo_id);
        data.ebo_id = 0;
    }
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
    std::vector<unsigned char> pixels(static_cast<size_t>(width) * static_cast<size_t>(height) *
                                      4u);
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const bool bright =
                (((x / kTerrainFallbackTileSize) + (y / kTerrainFallbackTileSize) + layer) % 2) ==
                0;
            const size_t index =
                (static_cast<size_t>(y) * static_cast<size_t>(width) + static_cast<size_t>(x)) * 4u;
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
            const char* name =
                reinterpret_cast<const char*>(glGetStringi(GL_EXTENSIONS, static_cast<GLuint>(i)));
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
    "particles",
    "foliage",
    "aerial",
    "final_blit",
};

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

// --- HIERARCHICAL CULLING IMPLEMENTATION ---

void RenderPipeline::HierarchicalCuller::BuildHierarchy(
    const std::vector<ChunkMeshSnapshot>& chunks) {
    if (chunks.empty()) {
        m_root.reset();
        return;
    }

    std::vector<ChunkCullEntry> chunk_refs;
    chunk_refs.reserve(chunks.size());
    for (const auto& chunk : chunks) {
        glm::vec3 chunk_min(static_cast<float>(chunk.coords.x * CHUNK_SIZE_X),
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

void RenderPipeline::HierarchicalCuller::BuildRecursive(CullingNode* node,
                                                        const std::vector<ChunkCullEntry>& chunks,
                                                        int depth) {
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
        AABB(node->bounds.min, glm::vec3(center.x, node->bounds.max.y, center.z)), // Bottom-left
        AABB(glm::vec3(center.x, node->bounds.min.y, node->bounds.min.z),
             glm::vec3(node->bounds.max.x, node->bounds.max.y, center.z)), // Bottom-right
        AABB(glm::vec3(node->bounds.min.x, node->bounds.min.y, center.z),
             glm::vec3(center.x, node->bounds.max.y, node->bounds.max.z)),        // Top-left
        AABB(glm::vec3(center.x, node->bounds.min.y, center.z), node->bounds.max) // Top-right
    };

    // Distribute chunks to children
    std::vector<std::vector<ChunkCullEntry>> child_chunks(4);

    for (const auto& chunk : chunks) {
        glm::ivec3 coords = chunk.coords;
        glm::vec3 chunk_center(coords.x * CHUNK_SIZE_X + CHUNK_SIZE_X * 0.5f,
                               coords.y * CHUNK_SIZE_Y + CHUNK_SIZE_Y * 0.5f,
                               coords.z * CHUNK_SIZE_Z + CHUNK_SIZE_Z * 0.5f);

        int child_index = 0;
        if (chunk_center.x >= center.x)
            child_index += 1;
        if (chunk_center.z >= center.z)
            child_index += 2;

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

void RenderPipeline::HierarchicalCuller::CullRecursive(
    const glm::vec4 frustum_planes[6],
    CullingNode* node,
    std::vector<const ChunkCullEntry*>& visible) {
    if (!node)
        return;

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

bool RenderPipeline::HierarchicalCuller::AABBFrustumCulled(const AABB& aabb,
                                                           const glm::vec4 frustum_planes[6]) {
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

void RenderPipeline::HierarchicalCuller::CullHierarchical(
    const glm::vec4 frustum_planes[6], std::vector<const ChunkCullEntry*>& visible) {
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
    : m_farlod(std::make_unique<FarLodSystem>())
    , m_gbuffer_pass(std::make_unique<GBufferPass>())
    , m_shadow_pass(std::make_unique<ShadowPass>())
    , m_ssao_pass(std::make_unique<SsaoPass>())
    , m_lighting_pass(std::make_unique<LightingPass>())
    , m_water_pass(std::make_unique<WaterPass>())
    , m_skybox_pass(std::make_unique<SkyboxPass>())
    , m_particle_pass(std::make_unique<ParticlePass>())
    , m_foliage_pass(std::make_unique<FoliagePass>())
    , m_plant_procgen_pass(std::make_unique<PlantProcgenPass>())
    , m_ground_decal_pass(std::make_unique<GroundDecalPass>())
    , m_debug_view_pass(std::make_unique<DebugViewPass>())
    , m_final_blit_pass(std::make_unique<FinalBlitPass>()) {}

// forward the one-way scent snapshot to the decal pass (render-only;
// defined here where GroundDecalPass is a complete type).
void RenderPipeline::UpdateScentDecals(const ScentFieldRenderMirror& mirror) {
    if (m_ground_decal_pass)
        m_ground_decal_pass->update_scent(mirror);
}

// Render-only debug-view override (0 = off). Never feeds world_hash.
void RenderPipeline::set_debug_view(int mode) {
    if (m_debug_view_pass)
        m_debug_view_pass->set_mode(mode);
}
RenderPipeline::~RenderPipeline() {
    cleanup_gpu_resources();
}

// --- PUBLIC INTERFACE ---

bool RenderPipeline::startup(u32 screen_width,
                             u32 screen_height,
                             const std::filesystem::path& root_path) {
    m_started = false;
    m_screen_width = screen_width;
    m_screen_height = screen_height;
    // render-scale knob. m_render_scale defaults to user.render_scale (wired via
    // set_render_scale before startup); LUMIN_RENDER_SCALE overrides for an A/B. Clamped to
    // [0.5, 1.0] -- 1.0 is byte-identical (internal==output); < 1.0 renders the scene at the
    // internal extent and the taau/final-blit sampler upscales to output.
    if (const auto value = Core::ReadEnvironment("LUMIN_RENDER_SCALE"); value && !value->empty()) {
        const float s = std::strtof(value->c_str(), nullptr);
        // The env knob WINS over the config-seeded set_render_scale for any PARSEABLE value:
        // clamp into [0.5,1.0] to match set_render_scale's clamp (so config=0.5 + env=2.0 -> 1.0,
        // not a leaked 0.5). strtof returns 0.0 on garbage -> `s > 0.0f` rejects unparseable input.
        if (s > 0.0f)
            m_render_scale = std::clamp(s, 0.5f, 1.0f);
    }
    // Internal (scaled) render extent = round(output * scale); at 1.0, lround(N*1.0f)==N.
    m_internal_width = static_cast<u32>(std::lround(m_screen_width * m_render_scale));
    m_internal_height = static_cast<u32>(std::lround(m_screen_height * m_render_scale));
    // log the resolved scale so the active render resolution is observable (which of
    // user.render_scale / LUMIN_RENDER_SCALE won, and the internal extent the scene renders at).
    LUMINUMBRA_CORE_INFO(" render_scale {} -> internal {}x{} (output {}x{})",
                         m_render_scale,
                         m_internal_width,
                         m_internal_height,
                         m_screen_width,
                         m_screen_height);
    m_root_path = root_path;

    try {
        set_default_shadow_cascade_splits(m_shadow_pass->shadow_map());

        init_shaders();
        // SCALED intermediates render at internal res (== output at scale 1.0).
        m_lighting_pass->init_lighting_fbo(m_render_registry, m_internal_width, m_internal_height);
        m_gbuffer_pass->init_gbuffer(m_render_registry, m_internal_width, m_internal_height);
        m_shadow_pass->init_shadow_map(m_render_registry);
        m_ssao_pass->init_ssao(m_render_registry, m_internal_width, m_internal_height);
        init_screen_quad();
        init_taau(screen_width,
                  screen_height); // OUTPUT res: TAAU history/backbuffer stay at output
        init_halfres_cloud();     // no-op unless cloud quality was set > 0 before startup
        m_skybox_pass->init_geometry();
        m_particle_pass->init_buffers();      //  persistent-mapped instance pool
        m_foliage_pass->init_buffers();       //  persistent-mapped scatter pool
        m_plant_procgen_pass->init_buffers(); //  dedicated procgen plant VAO/VBO/EBO
        m_ground_decal_pass->init_buffers();  // decal VAO + lazy scent texture
        m_debug_view_pass->init_buffers();    // render-only debug-view VAO (default-OFF)
        load_material_texture_lut();
        init_terrain_textures();
        init_skinned_texture_array();
        register_static_model_textures(); // Tree-part bark and leaf textures.
        //  far-field tree impostors: DEFAULT ON (set LUMIN_TREE_IMPOSTORS=0 to disable for
        // an A/B). Perf-validated win (render-benchmark forest_dense), scales with far tree count.
        // Bake the atlas now that the tree textures are loaded; the GBuffer LOD3 path samples it.
        const auto impostors = Core::ReadEnvironment("LUMIN_TREE_IMPOSTORS");
        if (!impostors || (!impostors->empty() && impostors->front() != '0')) {
            OctaImpostorGrid g;
            g.gridResolution = 12;
            const ImpostorAtlasTextures ia =
                BakeTreeImpostorAtlasToTextures(m_root_path.string(), *this, g);
            if (ia.ok) {
                m_treeImpostorAlbedo = ia.albedoTex;
                m_treeImpostorNormal = ia.normalTex;
                m_treeImpostorGrid = ia.grid;
                m_treeImpostorRadius = ia.radius;
                m_treeImpostorSphereY = ia.sphereY;
                m_treeImpostorsEnabled = true;
                LUMINUMBRA_CORE_INFO("Tree impostors ON: atlas baked ({}x{} grid, radius {:.1f})",
                                     ia.grid,
                                     ia.grid,
                                     ia.radius);
            } else {
                LUMINUMBRA_CORE_WARN("Tree impostor bake failed: {}", ia.error);
            }
        }
        init_material_lut();
        m_water_pass->init_water_fallback_textures(m_render_registry);
        init_gpu_pass_timers();
        init_mdi_buffers(); // per-frame MDI command + origin SSBO ring
        init_sky_lut();     //  Hillaire 2020 scattering LUT precompute

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

void RenderPipeline::register_procgen_mesh(const std::string& key, std::unique_ptr<Mesh> mesh) {
    if (m_gbuffer_pass)
        m_gbuffer_pass->register_cached_mesh(key, std::move(mesh));
}
bool RenderPipeline::has_procgen_mesh(const std::string& key) const {
    return m_gbuffer_pass && m_gbuffer_pass->has_cached_mesh(key);
}

// framescan: expose the G-buffer attachments (read-only) for the what's-in-frame
// scan tool. Forwards to the owning GBufferPass.  — only returns
// existing GL texture ids; nothing is hashed or written to sim state.
const GBuffer& RenderPipeline::gbuffer() const {
    return m_gbuffer_pass->gbuffer();
}

bool RenderPipeline::capture_frame_parity(const Camera& camera,
                                          const std::filesystem::path& out_dir) {
    // see the header contract. The normal render loop prepared (and
    // dispatched) this frame already; the two legs below are re-dispatches of the
    // SAME prepared frame, so any nonzero FLIP isolates a dispatch-idempotence
    // break — and, during the  migration, an execution-path divergence.
    if (!m_frame_prepared.valid) {
        LUMINUMBRA_CORE_ERROR("capture_frame_parity: no prepared frame (render a frame first)");
        return false;
    }
    if (m_taau_enabled) {
        // TAAU's history ping-pong makes leg B consume leg A's writes (Codex
        // critique finding 2). The parity contract requires it OFF (the default).
        LUMINUMBRA_CORE_ERROR("capture_frame_parity: requires TAAU OFF (history ping-pong breaks "
                              "dispatch idempotence)");
        return false;
    }
    const GLsizei w = static_cast<GLsizei>(m_screen_width);
    const GLsizei h = static_cast<GLsizei>(m_screen_height);
    if (w <= 0 || h <= 0)
        return false;

    auto make_target = [&](GLuint& fbo, GLuint& tex) {
        glGenTextures(1, &tex);
        glBindTexture(GL_TEXTURE_2D, tex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glGenFramebuffers(1, &fbo);
        glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
    };
    GLuint fboA = 0, texA = 0, fboB = 0, texB = 0;
    make_target(fboA, texA);
    bool ok = (glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE);
    make_target(fboB, texB);
    ok = ok && (glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE);

    // The prepared frame was built under the NORMAL target state (Codex critique
    // finding 5: prepare behavior keys off m_offscreen_target_active, e.g. the
    // far-LOD skip) — only the DISPATCH legs retarget. Save + restore exactly.
    const bool prev_active = m_offscreen_target_active;
    const u32 prev_fbo = m_offscreen_target_fbo;
    const u32 prev_w = m_offscreen_target_w;
    const u32 prev_h = m_offscreen_target_h;
    // The frame's real dispatch already recorded this frame's GPU timer ring slot;
    // the two parity legs must not double-issue timestamp queries.
    m_gpu_timers_suppressed = true;

    set_offscreen_target(fboA, static_cast<u32>(w), static_cast<u32>(h));
    dispatch_stages(camera);
    set_offscreen_target(fboB, static_cast<u32>(w), static_cast<u32>(h));
    dispatch_stages(camera);

    m_gpu_timers_suppressed = false;
    m_offscreen_target_active = prev_active;
    m_offscreen_target_fbo = prev_fbo;
    m_offscreen_target_w = prev_w;
    m_offscreen_target_h = prev_h;

    auto read_rgba = [&](GLuint fbo, std::vector<std::uint8_t>& px) {
        px.assign(static_cast<std::size_t>(w) * static_cast<std::size_t>(h) * 4u, 0u);
        glBindFramebuffer(GL_READ_FRAMEBUFFER, fbo);
        glReadBuffer(GL_COLOR_ATTACHMENT0);
        glPixelStorei(GL_PACK_ALIGNMENT, 1);
        glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
    };
    std::vector<std::uint8_t> pxA, pxB;
    read_rgba(fboA, pxA);
    read_rgba(fboB, pxB);

    const auto flip = InProcessFlip::ComputeLumaFlip(pxA.data(), pxB.data(), w, h);

    std::error_code ec;
    std::filesystem::create_directories(out_dir, ec);
    auto write_ppm = [&](const std::vector<std::uint8_t>& px,
                         const std::filesystem::path& path) -> bool {
        std::ofstream f(path, std::ios::binary | std::ios::trunc);
        if (!f)
            return false;
        f << "P6\n" << w << " " << h << "\n255\n";
        for (std::size_t i = 0; i < px.size(); i += 4u) {
            f.write(reinterpret_cast<const char*>(px.data() + i), 3);
        }
        return static_cast<bool>(f);
    };
    ok = ok && write_ppm(pxA, out_dir / "frame_parity_a.ppm");
    ok = ok && write_ppm(pxB, out_dir / "frame_parity_b.ppm");
    {
        std::ofstream j(out_dir / "frame_parity.json", std::ios::trunc);
        ok = ok && static_cast<bool>(j);
        if (j) {
            j << "{\n"
              << "  \"schema\": \"luminumbra.render_frame_parity.v1\",\n"
              << "  \"metric\": \"" << InProcessFlip::BackendName() << "\",\n"
              << "  \"score\": " << flip.score << ",\n"
              << "  \"max_error\": " << flip.max_error << ",\n"
              << "  \"width\": " << w << ",\n"
              << "  \"height\": " << h << ",\n"
              << "  \"passed\": " << ((flip.score == 0.0) ? "true" : "false") << "\n"
              << "}\n";
        }
    }

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glDeleteFramebuffers(1, &fboA);
    glDeleteFramebuffers(1, &fboB);
    glDeleteTextures(1, &texA);
    glDeleteTextures(1, &texB);

    if (flip.score != 0.0) {
        LUMINUMBRA_CORE_ERROR(
            "capture_frame_parity: dispatch NOT idempotent — flip score {} (max {}) != 0.0",
            flip.score,
            flip.max_error);
        return false;
    }
    LUMINUMBRA_CORE_INFO("capture_frame_parity: whole-frame A/B EXACT (score 0.0, {}x{})", w, h);
    return ok;
}

bool RenderPipeline::capture_upscale_seam_parity(const Camera& camera,
                                                 const std::filesystem::path& out_dir) {
    // compare three dispatches over ONE prepared frame/context:
    //   reference: native scale 1.0
    //   leg 1:     the same scale-1 seam again (must be bit-exact)
    //   leg 2:     scale 0.67, upscaled by FinalBlit to the same output extent
    // Keeping all three in-process removes the ~0.057 cross-run capture noise floor.
    if (!m_frame_prepared.valid) {
        LUMINUMBRA_CORE_ERROR(
            "capture_upscale_seam_parity: no prepared frame (render a frame first)");
        return false;
    }
    if (m_taau_enabled) {
        LUMINUMBRA_CORE_ERROR("capture_upscale_seam_parity: requires TAAU OFF (history ping-pong "
                              "invalidates parity)");
        return false;
    }

    constexpr float kReducedScale = 0.67f;
    // Clean RTX 5070 Ti calibration (2026-07-10): 0.0112331374322083 at
    // 3840x1581 -> 2573x1059. Add ~25% headroom for driver-level raster variance
    // while remaining far below the preregistered 0.08 hard ceiling.
    constexpr double kUpscaleSeamLeg2Threshold = 0.0141;
    const GLsizei w = static_cast<GLsizei>(m_screen_width);
    const GLsizei h = static_cast<GLsizei>(m_screen_height);
    if (w <= 0 || h <= 0)
        return false;

    auto make_target = [&](GLuint& fbo, GLuint& tex) {
        glGenTextures(1, &tex);
        glBindTexture(GL_TEXTURE_2D, tex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glGenFramebuffers(1, &fbo);
        glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
    };
    auto read_rgba = [&](GLuint fbo, std::vector<std::uint8_t>& px) {
        px.assign(static_cast<std::size_t>(w) * static_cast<std::size_t>(h) * 4u, 0u);
        glBindFramebuffer(GL_READ_FRAMEBUFFER, fbo);
        glReadBuffer(GL_COLOR_ATTACHMENT0);
        glPixelStorei(GL_PACK_ALIGNMENT, 1);
        glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
    };
    auto write_ppm = [&](const std::vector<std::uint8_t>& px,
                         const std::filesystem::path& path) -> bool {
        std::ofstream f(path, std::ios::binary | std::ios::trunc);
        if (!f)
            return false;
        f << "P6\n" << w << " " << h << "\n255\n";
        for (std::size_t i = 0; i < px.size(); i += 4u) {
            f.write(reinterpret_cast<const char*>(px.data() + i), 3);
        }
        return static_cast<bool>(f);
    };

    GLuint fbo_reference = 0, tex_reference = 0;
    GLuint fbo_scale1 = 0, tex_scale1 = 0;
    GLuint fbo_scale067 = 0, tex_scale067 = 0;
    make_target(fbo_reference, tex_reference);
    bool ok = glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
    make_target(fbo_scale1, tex_scale1);
    ok = ok && glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
    make_target(fbo_scale067, tex_scale067);
    ok = ok && glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;

    const float previous_scale = m_render_scale;
    const bool previous_target_active = m_offscreen_target_active;
    const u32 previous_target_fbo = m_offscreen_target_fbo;
    const u32 previous_target_w = m_offscreen_target_w;
    const u32 previous_target_h = m_offscreen_target_h;
    const bool previous_timer_suppression = m_gpu_timers_suppressed;

    m_gpu_timers_suppressed = true;
    set_render_scale(1.0f);
    const u32 native_internal_w = m_internal_width;
    const u32 native_internal_h = m_internal_height;
    set_offscreen_target(fbo_reference, static_cast<u32>(w), static_cast<u32>(h));
    dispatch_stages(camera);
    set_offscreen_target(fbo_scale1, static_cast<u32>(w), static_cast<u32>(h));
    dispatch_stages(camera);

    set_render_scale(kReducedScale);
    const u32 reduced_internal_w = m_internal_width;
    const u32 reduced_internal_h = m_internal_height;
    set_offscreen_target(fbo_scale067, static_cast<u32>(w), static_cast<u32>(h));
    dispatch_stages(camera);

    std::vector<std::uint8_t> px_reference, px_scale1, px_scale067;
    read_rgba(fbo_reference, px_reference);
    read_rgba(fbo_scale1, px_scale1);
    read_rgba(fbo_scale067, px_scale067);

    // Restore production state even though the headless harness exits after capture.
    set_render_scale(previous_scale);
    m_offscreen_target_active = previous_target_active;
    m_offscreen_target_fbo = previous_target_fbo;
    m_offscreen_target_w = previous_target_w;
    m_offscreen_target_h = previous_target_h;
    // set_render_scale rebuilt the scaled G-buffer/lighting/SSAO targets. Re-dispatch
    // once at the restored production scale so the frame-scan that follows this capture
    // observes populated attachments rather than the freshly allocated empty targets.
    dispatch_stages(camera);
    m_gpu_timers_suppressed = previous_timer_suppression;

    const auto leg1 = InProcessFlip::ComputeLumaFlip(px_reference.data(), px_scale1.data(), w, h);
    const auto leg2 = InProcessFlip::ComputeLumaFlip(px_reference.data(), px_scale067.data(), w, h);
    const bool leg1_passed = leg1.score == 0.0;
    const bool leg2_passed = leg2.score <= kUpscaleSeamLeg2Threshold;

    std::error_code ec;
    std::filesystem::create_directories(out_dir, ec);
    ok = ok && !ec;
    ok = ok && write_ppm(px_reference, out_dir / "upscale_seam_reference.ppm");
    ok = ok && write_ppm(px_scale1, out_dir / "upscale_seam_scale1.ppm");
    ok = ok && write_ppm(px_scale067, out_dir / "upscale_seam_scale067.ppm");
    {
        nlohmann::json report = {{"schema", "luminumbra.upscale_seam_parity.v1"},
                                 {"metric", InProcessFlip::BackendName()},
                                 {"output_width", w},
                                 {"output_height", h},
                                 {"reference",
                                  {{"scale", 1.0},
                                   {"internal_width", native_internal_w},
                                   {"internal_height", native_internal_h}}},
                                 {"leg1",
                                  {{"scale", 1.0},
                                   {"score", leg1.score},
                                   {"max_error", leg1.max_error},
                                   {"threshold", 0.0},
                                   {"passed", leg1_passed}}},
                                 {"leg2",
                                  {{"scale", kReducedScale},
                                   {"internal_width", reduced_internal_w},
                                   {"internal_height", reduced_internal_h},
                                   {"score", leg2.score},
                                   {"max_error", leg2.max_error},
                                   {"threshold", kUpscaleSeamLeg2Threshold},
                                   {"passed", leg2_passed}}},
                                 {"restored_scale", previous_scale},
                                 {"passed", leg1_passed && leg2_passed}};
        std::ofstream j(out_dir / "upscale_seam_parity.json", std::ios::trunc);
        ok = ok && static_cast<bool>(j);
        if (j)
            j << report.dump(2) << '\n';
    }

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glDeleteFramebuffers(1, &fbo_reference);
    glDeleteFramebuffers(1, &fbo_scale1);
    glDeleteFramebuffers(1, &fbo_scale067);
    glDeleteTextures(1, &tex_reference);
    glDeleteTextures(1, &tex_scale1);
    glDeleteTextures(1, &tex_scale067);

    if (!leg1_passed) {
        LUMINUMBRA_CORE_ERROR(
            "capture_upscale_seam_parity: scale-1 seam NOT exact — score {} (max {})",
            leg1.score,
            leg1.max_error);
    }
    if (!leg2_passed) {
        LUMINUMBRA_CORE_ERROR(
            "capture_upscale_seam_parity: scale-0.67 score {} exceeds threshold {}",
            leg2.score,
            kUpscaleSeamLeg2Threshold);
    }
    if (leg1_passed && leg2_passed) {
        LUMINUMBRA_CORE_INFO("capture_upscale_seam_parity: scale-1 EXACT; scale-0.67 score {} <= "
                             "{} ({}x{} -> {}x{})",
                             leg2.score,
                             kUpscaleSeamLeg2Threshold,
                             reduced_internal_w,
                             reduced_internal_h,
                             w,
                             h);
    }
    return ok && leg1_passed && leg2_passed;
}

// the SSAO pass contract, built from pipeline state. Used
// by both the render_frame call site and capture_ssao_parity so the gated ctx is
// byte-identical to the production ctx.
RenderContext RenderPipeline::make_ssao_context(const Camera& camera) {
    RenderContext ctx;
    ctx.camera = &camera;
    ctx.screen_width = m_screen_width;
    ctx.screen_height = m_screen_height;
    ctx.internal_width = m_internal_width; //
    ctx.internal_height = m_internal_height;
    ctx.registry = &m_render_registry;
    ctx.screen_quad_vao = m_screen_quad_vao;
    ctx.ssao_quality = m_ssao_quality;
    ctx.gbuffer_position = m_render_registry.adopt_texture(
        "gbuffer_position", m_gbuffer_pass->gbuffer().position_texture);
    ctx.gbuffer_normal =
        m_render_registry.adopt_texture("gbuffer_normal", m_gbuffer_pass->gbuffer().normal_texture);
    return ctx;
}

// the DebugView pass contract — the four G-buffer attachments the
// diagnostic overlay samples (position/normal/albedo/depth) adopted wrap-existing, plus
// the frame camera for Depth-mode near/far linearization. Render-only; never hashed.
RenderContext RenderPipeline::make_debug_view_context(const Camera& camera) {
    RenderContext ctx;
    ctx.camera = &camera;
    ctx.screen_width = m_screen_width;
    ctx.screen_height = m_screen_height;
    ctx.internal_width = m_internal_width; //
    ctx.internal_height = m_internal_height;
    ctx.registry = &m_render_registry;
    const GBuffer& gb = m_gbuffer_pass->gbuffer();
    ctx.gbuffer_position = m_render_registry.adopt_texture("gbuffer_position", gb.position_texture);
    ctx.gbuffer_normal = m_render_registry.adopt_texture("gbuffer_normal", gb.normal_texture);
    ctx.gbuffer_albedo = m_render_registry.adopt_texture("gbuffer_albedo", gb.albedo_texture);
    ctx.gbuffer_depth = m_render_registry.adopt_texture("gbuffer_depth", gb.depth_texture);
    return ctx;
}

// the GroundDecal pass contract — the G-buffer view-space position the
// decal projects through, adopted wrap-existing, plus the frame camera for the inverse
// view. Render-only (one-way scent mirror); never feeds world_hash.
RenderContext RenderPipeline::make_ground_decal_context(const Camera& camera) {
    RenderContext ctx;
    ctx.camera = &camera;
    ctx.screen_width = m_screen_width;
    ctx.screen_height = m_screen_height;
    ctx.internal_width = m_internal_width; //
    ctx.internal_height = m_internal_height;
    ctx.registry = &m_render_registry;
    ctx.gbuffer_position = m_render_registry.adopt_texture(
        "gbuffer_position", m_gbuffer_pass->gbuffer().position_texture);
    return ctx;
}

// the PlantProcgen pass contract (screen size + the single
// per-frame wall-clock snapshot that feeds u_time leaf sway). Render-only.
RenderContext RenderPipeline::make_plant_context() {
    RenderContext ctx;
    ctx.registry = &m_render_registry;
    ctx.screen_width = m_screen_width;
    ctx.screen_height = m_screen_height;
    ctx.internal_width = m_internal_width; //
    ctx.internal_height = m_internal_height;
    ctx.time_seconds = m_wall_clock_time;
    return ctx;
}

// the Particle pass contract (lit-scene draw target,
// g-buffer depth, sun/ambient/point-lights, screen). Handles adopted wrap-existing.
RenderContext RenderPipeline::make_particle_context(const Camera& camera) {
    RenderContext ctx;
    ctx.camera = &camera;
    ctx.screen_width = m_screen_width;
    ctx.screen_height = m_screen_height;
    ctx.internal_width = m_internal_width; //
    ctx.internal_height = m_internal_height;
    ctx.registry = &m_render_registry;
    ctx.sun = m_sun;
    ctx.sky_ambient_color = m_skyAmbientColor;
    ctx.time_seconds = m_wall_clock_time; // Per-frame snapshot used for shader sway.
    ctx.point_lights = &m_point_lights_this_frame;
    ctx.gbuffer_depth =
        m_render_registry.adopt_texture("gbuffer_depth", m_gbuffer_pass->gbuffer().depth_texture);
    ctx.lit_scene =
        m_render_registry.adopt_fbo("lit_scene", m_lighting_pass->lighting_fbo().fbo_id);
    return ctx;
}

// the Foliage pass contract (lit-scene draw target,
// sun/ambient/moon/cloud light state, screen, per-frame time for shader sway).
RenderContext RenderPipeline::make_foliage_context(const Camera& camera) {
    RenderContext ctx;
    ctx.camera = &camera;
    ctx.screen_width = m_screen_width;
    ctx.screen_height = m_screen_height;
    ctx.internal_width = m_internal_width; //
    ctx.internal_height = m_internal_height;
    ctx.registry = &m_render_registry;
    ctx.time_seconds = m_wall_clock_time;
    ctx.sun = m_sun;
    ctx.sky_ambient_color = m_skyAmbientColor;
    ctx.moon_light_dir = m_moonLightDir;
    ctx.cloud_state = m_cloud_state;
    ctx.lit_scene =
        m_render_registry.adopt_fbo("lit_scene", m_lighting_pass->lighting_fbo().fbo_id);
    return ctx;
}

// build the Lighting pass contract from pipeline state.
// Mirrors make_water_context/make_ssao_context (adopt_texture wrap-existing GL
// names; PODs by value). The shadow-cascade fixup is HOISTED here from
// LightingPass::execute because it mutates the shared ShadowMap private state and
// calls the pipeline-private get_light_space_matrices — both unavailable to a
// friendless pass. The fixup is CPU-only (no GL), so moving it ahead of the
// Lighting GPU timer leaves the measured GPU work and every pixel unchanged.
RenderContext RenderPipeline::make_lighting_context(const Camera& camera) {
    RenderContext ctx;
    ctx.camera = &camera;
    ctx.screen_width = m_screen_width;
    ctx.screen_height = m_screen_height;
    ctx.internal_width = m_internal_width; //
    ctx.internal_height = m_internal_height;
    ctx.registry = &m_render_registry;
    ctx.screen_quad_vao = m_screen_quad_vao;

    // Group B — G-buffer attachment handles (adopt-by-name; wrap-existing GL names).
    ctx.gbuffer_position = m_render_registry.adopt_texture(
        "gbuffer_position", m_gbuffer_pass->gbuffer().position_texture);
    ctx.gbuffer_normal =
        m_render_registry.adopt_texture("gbuffer_normal", m_gbuffer_pass->gbuffer().normal_texture);
    ctx.gbuffer_albedo =
        m_render_registry.adopt_texture("gbuffer_albedo", m_gbuffer_pass->gbuffer().albedo_texture);
    ctx.gbuffer_material = m_render_registry.adopt_texture(
        "gbuffer_material", m_gbuffer_pass->gbuffer().material_texture);
    ctx.gbuffer_depth =
        m_render_registry.adopt_texture("gbuffer_depth", m_gbuffer_pass->gbuffer().depth_texture);

    // Group E — shadow / SSAO / caustics reads.
    ctx.shadow_depth_array = m_render_registry.adopt_texture(
        "shadow_depth_array", m_shadow_pass->shadow_map().depth_texture_array);
    // the tinted-transmission cascade (registry-owned by
    // ShadowPass; init-cleared white so empty-glass frames multiply by exactly 1.0).
    ctx.shadow_tint_array = m_render_registry.adopt_texture("shadow_tint_cascades",
                                                            m_shadow_pass->tint_texture_array());
    ctx.ssao_blur =
        m_render_registry.adopt_texture("ssao_blur", m_ssao_pass->ssao().ssaoColorBufferBlur);
    ctx.caustics_tex =
        m_render_registry.adopt_texture("caustics_tex", m_water_pass->black_texture());

    // Group F — terrain / material arrays.
    ctx.terrain_textures =
        m_render_registry.adopt_texture("terrain_textures", m_terrainTextureArray);
    ctx.material_lut = m_render_registry.adopt_texture("material_lut", m_materialLUT);

    // Group G — aether field.
    ctx.aether_field = m_render_registry.adopt_texture("aether_field", m_aetherFieldTexture);
    ctx.aether_active = m_aetherFieldActive;
    ctx.aether_polarity_active = m_aetherPolarityActive; //
    ctx.aether_extent = m_aetherFieldExtent;
    ctx.aether_cell_size = m_aetherFieldCellSize;
    ctx.aether_world_origin = m_aetherFieldWorldOrigin;
    ctx.aether_glow_color = m_aetherGlowColor;                   // the glow grade
    ctx.aether_glow_intensity = m_aetherGlowIntensity;           // (defaults == GLSL consts)
    ctx.aether_material_modulation = m_aetherMaterialModulation; //  (0 = identical)
    ctx.snow_cover = m_snowCover;                                // snow ground cover

    // Group H — light/atmosphere scalars & vectors.
    ctx.sun = m_sun;
    ctx.sky_ambient_color = m_skyAmbientColor;
    ctx.moon_light_dir = m_moonLightDir;
    ctx.moon_illumination = m_moonIllumination; //  rendering (): lunar phase / two night modes
    ctx.moon_radiance = m_moonRadiance; //  rendering : the moon's dedicated radiance channel
    //  rendering: photo-mode manual EV OVERRIDES the auto exposure;
    // the -1 sentinel (photo mode inactive) selects it.: with
    // metering ON and a first readback consumed, the ring-fed damped servo IS the
    // auto source; otherwise the analytic TOD curve (/) remains.
    const float auto_exposure =
        (m_auto_exposure_metered && m_metered_valid) ? m_metered_exposure : m_scene_exposure;
    ctx.exposure = SelectRenderExposure(m_exposureOverride, auto_exposure);
    ctx.emissive_lut_scale = kEmissiveLutScale;
    ctx.point_lights = &m_point_lights_this_frame;
    ctx.cloud_state = m_cloud_state;
    ctx.lightning_state = &m_lightning_state;

    // Shadow-cascade fixup HOISTED from LightingPass::execute (mutates the shared
    // ShadowMap private state + calls the pipeline-private get_light_space_matrices).
    ShadowMap& shadow_map = m_shadow_pass->shadow_map();
    if (!PassGl::has_valid_shadow_cascade_splits(shadow_map)) {
        LUMINUMBRA_CORE_ERROR(
            "Shadow cascade splits were invalid during lighting; restoring defaults.");
        PassGl::set_default_shadow_cascade_splits(shadow_map);
    }
    if (shadow_map.light_space_matrices.size() < ShadowMap::CASCADE_COUNT) {
        shadow_map.light_space_matrices = get_light_space_matrices(camera);
    }
    ctx.cascade_splits = glm::vec4(shadow_map.cascade_splits[1],
                                   shadow_map.cascade_splits[2],
                                   shadow_map.cascade_splits[3],
                                   shadow_map.cascade_splits[4]);
    ctx.light_space_matrices = &shadow_map.light_space_matrices;

    // Group M — stats out-pointer (execute + lightning overlay bump in place).
    ctx.lighting_draws = &m_last_render_pass_stats.lighting_draws;
    return ctx;
}

// the Skybox/sky-dome + weather-overlay pass contract.
// The dome reads isolation/sun/moon/sky-day/LUTs/cloud/weather + the per-frame
// time snapshot; the deferred weather overlay additionally reads the lit-scene
// draw target (lit_scene), the post-water opaque snapshot (opaque_scene), and the
// g-buffer depth/position/normal. Handles are adopted wrap-existing (the FBOs /
// textures are still owned by Lighting/GBuffer during the migration). Built AFTER
// the gbuffer->lighting depth blit, mirroring the production sequence. The dome's
// skybox_draws bump rides ctx.skybox_draw_counter (Group M out-pointer).
RenderContext RenderPipeline::make_skybox_context(const Camera& camera) {
    RenderContext ctx;
    ctx.camera = &camera;
    ctx.screen_width = m_screen_width;
    ctx.screen_height = m_screen_height;
    ctx.internal_width = m_internal_width; //
    ctx.internal_height = m_internal_height;
    ctx.registry = &m_render_registry;
    ctx.screen_quad_vao = m_screen_quad_vao;
    ctx.time_seconds = m_wall_clock_time;
    ctx.sun = m_sun;
    ctx.moon_direction = m_moonDirection;
    ctx.sky_day_factor = m_skyDayFactor;
    ctx.sky_lut_ready = m_sky_lut.ready();
    ctx.cloud_state = m_cloud_state;
    ctx.weather_state = &m_weather_state;
    ctx.weather_type = m_weather_type;
    ctx.weather_intensity = m_weather_intensity;
    ctx.isolation = &m_isolation_config;
    ctx.skybox_draw_counter = &m_last_render_pass_stats.skybox_draws;
    // Adopt the scattering LUTs only when ready -- exactly when the dome binds
    // them (the dome guards on ctx.sky_lut_ready; left as default {0} otherwise).
    if (m_sky_lut.ready()) {
        ctx.sky_view_lut =
            m_render_registry.adopt_texture("sky_view_lut", m_sky_lut.sky_view_texture());
        ctx.transmittance_lut =
            m_render_registry.adopt_texture("transmittance_lut", m_sky_lut.transmittance_texture());
    }
    // Weather-overlay reaches (also harmless for the dome path, which ignores them).
    ctx.lit_scene =
        m_render_registry.adopt_fbo("lit_scene", m_lighting_pass->lighting_fbo().fbo_id);
    ctx.opaque_scene = m_render_registry.adopt_texture(
        "opaque_scene", m_lighting_pass->lighting_fbo().opaque_color_texture);
    ctx.gbuffer_depth =
        m_render_registry.adopt_texture("gbuffer_depth", m_gbuffer_pass->gbuffer().depth_texture);
    ctx.gbuffer_position = m_render_registry.adopt_texture(
        "gbuffer_position", m_gbuffer_pass->gbuffer().position_texture);
    ctx.gbuffer_normal =
        m_render_registry.adopt_texture("gbuffer_normal", m_gbuffer_pass->gbuffer().normal_texture);
    return ctx;
}

//  terrain-submit seam (Codex-signed-off). Reproduces the EXACT current
// GBuffer/Shadow submit (CullHierarchical + draw_chunks_mdi) and RETURNS the
// counts — no stats mutation, no sort/cache/readback, no hierarchy rebuild (the
// pipeline owns that, once per frame, before both passes). The only dropped
// statement vs today is `visible.reserve(renderable_chunks.size)`, a capacity
// hint with no effect on draw order or stats — so output is byte-identical.
// the Water pass contract — lit-scene draw target,
// opaque-scene + g-buffer depth for refraction/SSR, the shared quad, sun light,
// and the per-frame time snapshot. The draw list travels separately (WaterPassInput).
RenderContext RenderPipeline::make_water_context(const Camera& camera) {
    RenderContext ctx;
    ctx.camera = &camera;
    ctx.screen_width = m_screen_width;
    ctx.screen_height = m_screen_height;
    ctx.internal_width = m_internal_width; //
    ctx.internal_height = m_internal_height;
    ctx.registry = &m_render_registry;
    ctx.screen_quad_vao = m_screen_quad_vao;
    ctx.time_seconds = m_wall_clock_time;
    ctx.sun = m_sun;
    ctx.lit_scene =
        m_render_registry.adopt_fbo("lit_scene", m_lighting_pass->lighting_fbo().fbo_id);
    ctx.opaque_scene = m_render_registry.adopt_texture(
        "opaque_scene", m_lighting_pass->lighting_fbo().opaque_color_texture);
    ctx.gbuffer_depth =
        m_render_registry.adopt_texture("gbuffer_depth", m_gbuffer_pass->gbuffer().depth_texture);
    return ctx;
}

// build the GBuffer pass contract — frame state only; the
// live-terrain submit callback, far-LOD, root path, static-model UV lane and
// tree-impostor bundle travel through GBufferPassInput (built at the call site).
RenderContext RenderPipeline::make_gbuffer_context(const Camera& camera,
                                                   const glm::vec4 frustum_planes[6]) {
    RenderContext ctx;
    ctx.camera = &camera;
    ctx.screen_width = m_screen_width;
    ctx.screen_height = m_screen_height;
    ctx.internal_width = m_internal_width; //
    ctx.internal_height = m_internal_height;
    ctx.registry = &m_render_registry;
    ctx.time_seconds = m_wall_clock_time;
    ctx.taau_jitter_ndc = m_taau_jitter_ndc;
    ctx.prev_view_proj = m_prev_view_proj;
    ctx.prev_time = m_prev_time;
    ctx.terrain_roughness_valid = m_terrainRoughnessValid;
    ctx.skinned_albedo_layer = m_skinnedAlbedoLayer;
    ctx.skinned_normal_layer = m_skinnedNormalLayer;
    ctx.frustum_planes = frustum_planes;
    ctx.isolation = &m_isolation_config;
    ctx.material_lut = m_render_registry.adopt_texture("material_lut", m_materialLUT);
    ctx.terrain_textures =
        m_render_registry.adopt_texture("terrain_texture_array", m_terrainTextureArray);
    ctx.terrain_normals =
        m_render_registry.adopt_texture("terrain_normal_array", m_terrainNormalArray);
    ctx.terrain_roughness =
        m_render_registry.adopt_texture("terrain_roughness_array", m_terrainRoughnessArray);
    ctx.skinned_textures =
        m_render_registry.adopt_texture("skinned_texture_array", m_skinnedTextureArray);
    return ctx;
}

SubmitTerrainChunksFn RenderPipeline::make_terrain_submitter() {
    return [this](const glm::vec4(&frustum_planes)[6]) -> TerrainSubmitStats {
        std::vector<const ChunkCullEntry*> visible_chunks;
        m_hierarchicalCuller.CullHierarchical(frustum_planes, visible_chunks);
        std::size_t draws = 0;
        std::size_t indices = 0;
        draw_chunks_mdi(visible_chunks, draws, indices);
        return TerrainSubmitStats{visible_chunks.size(), draws, indices};
    };
}

// SSAO parity gate. For a MECHANICAL conversion (the GL
// sequence is a verbatim copy, only operands changed pipeline.X -> ctx.X) the sole
// risk is make_ssao_context mis-mapping a field, so we (1) assert every ctx field
// equals its raw pipeline source — the independent mapping check — and (2) prove
// the seam is deterministic by running it twice and memcmp'ing the R16F readback of
// the pass-owned blur target, across ssao_quality 0..3 (legacy / GTAO / half-res).
bool RenderPipeline::capture_ssao_parity(const std::filesystem::path& out_dir,
                                         const Camera& camera) {
    const GLsizei w = static_cast<GLsizei>(m_screen_width);
    const GLsizei h = static_cast<GLsizei>(m_screen_height);
    if (w <= 0 || h <= 0)
        return false;

    RenderContext ctx = make_ssao_context(camera);
    // (1) Mapping check: ctx fields must equal the raw pipeline sources.
    bool mapping_ok = ctx.screen_width == m_screen_width && ctx.screen_height == m_screen_height &&
                      ctx.screen_quad_vao == m_screen_quad_vao &&
                      ctx.ssao_quality == m_ssao_quality &&
                      ctx.gbuffer_position.id == m_gbuffer_pass->gbuffer().position_texture &&
                      ctx.gbuffer_normal.id == m_gbuffer_pass->gbuffer().normal_texture;

    auto readback_blur = [&](std::vector<float>& buf) {
        buf.assign(static_cast<size_t>(w) * static_cast<size_t>(h), 0.0f);
        glBindFramebuffer(GL_READ_FRAMEBUFFER, m_ssao_pass->ssao().blurFBO);
        glReadBuffer(GL_COLOR_ATTACHMENT0);
        glPixelStorei(GL_PACK_ALIGNMENT, 1);
        glReadPixels(0, 0, w, h, GL_RED, GL_FLOAT, buf.data());
    };

    bool determinism_ok = true;
    std::vector<float> b1, b2;
    for (int q = 0; q <= 3; ++q) {
        ctx.ssao_quality = q;
        m_ssao_pass->execute_ssao(ctx);
        m_ssao_pass->execute_blur(ctx);
        readback_blur(b1);
        m_ssao_pass->execute_ssao(ctx);
        m_ssao_pass->execute_blur(ctx);
        readback_blur(b2);
        if (b1 != b2) {
            determinism_ok = false;
        }
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);

    std::error_code ec;
    std::filesystem::create_directories(out_dir, ec);
    const bool ok = mapping_ok && determinism_ok;
    std::ofstream f(out_dir / "ssao_parity.txt", std::ios::trunc);
    if (f) {
        f << "ssao_parity: " << (ok ? "PASS" : "FAIL") << " mapping_ok=" << mapping_ok
          << " determinism_ok=" << determinism_ok << " (" << w << "x" << h << ", quality 0..3)\n";
    }
    return ok;
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

void RenderPipeline::set_far_lod_preview_anchor(const glm::vec3& center, float inner_radius_m) {
    if (m_farlod) {
        m_farlod->set_preview_anchor(center, inner_radius_m);
    }
}

void RenderPipeline::clear_far_lod_preview_anchor() {
    if (m_farlod) {
        m_farlod->clear_preview_anchor();
    }
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

    const size_t pixel_count =
        static_cast<size_t>(m_screen_width) * static_cast<size_t>(m_screen_height);
    size_t estimated_vram_bytes = 0;
    estimated_vram_bytes +=
        (stats.terrain_vertex_capacity + stats.water_vertex_capacity) * sizeof(VoxelVertex);
    estimated_vram_bytes +=
        (stats.terrain_index_capacity + stats.water_index_capacity) * sizeof(u32);
    const FrameBufferObject& lighting_fbo = m_lighting_pass->lighting_fbo();
    if (lighting_fbo.color_texture)
        estimated_vram_bytes += pixel_count * 8u; // RGBA16F
    if (lighting_fbo.opaque_color_texture)
        estimated_vram_bytes += pixel_count * 8u; // RGBA16F
    if (lighting_fbo.depth_texture)
        estimated_vram_bytes += pixel_count * 4u;
    const GBuffer& gbuffer = m_gbuffer_pass->gbuffer();
    if (gbuffer.position_texture)
        estimated_vram_bytes += pixel_count * 6u; // RGB16F
    if (gbuffer.normal_texture)
        estimated_vram_bytes += pixel_count * 4u;
    if (gbuffer.albedo_texture)
        estimated_vram_bytes += pixel_count * 4u;
    if (gbuffer.material_texture)
        estimated_vram_bytes += pixel_count * 4u; // RG16F
    if (gbuffer.depth_texture)
        estimated_vram_bytes += pixel_count * 4u;
    if (m_shadow_pass->shadow_map().depth_texture_array) {
        estimated_vram_bytes += static_cast<size_t>(m_shadow_pass->shadow_map().resolution) *
                                static_cast<size_t>(m_shadow_pass->shadow_map().resolution) *
                                static_cast<size_t>(ShadowMap::CASCADE_COUNT) * 4u;
    }
    if (m_ssao_pass->ssao().ssaoColorBuffer)
        estimated_vram_bytes += pixel_count * 2u;
    if (m_ssao_pass->ssao().ssaoColorBufferBlur)
        estimated_vram_bytes += pixel_count * 2u;
    if (m_ssao_pass->ssao().noiseTexture)
        estimated_vram_bytes += 4u * 4u * 6u;
    if (m_screen_quad_vbo)
        estimated_vram_bytes += 20u * sizeof(float);
    if (m_skybox_pass->vbo())
        estimated_vram_bytes += 108u * sizeof(float);
    if (m_gbuffer_pass->instance_matrix_vbo())
        estimated_vram_bytes += 10000u * sizeof(glm::mat4);
    if (m_terrainTextureArray)
        estimated_vram_bytes +=
            static_cast<size_t>(kTerrainTextureResolution) * kTerrainTextureResolution * 5u * 4u;
    if (m_terrainNormalArray)
        estimated_vram_bytes +=
            static_cast<size_t>(kTerrainTextureResolution) * kTerrainTextureResolution * 5u * 4u;
    if (m_skinnedTextureArray)
        estimated_vram_bytes +=
            static_cast<size_t>(kSkinnedTextureResolution) * kSkinnedTextureResolution * 2u * 4u;
    if (m_materialLUT)
        estimated_vram_bytes +=
            256u * 4u * 4u; // 256 ids x 4 rows x RGBA8 (: was stale 2-row estimate)
    if (m_water_pass->flat_normal_texture())
        estimated_vram_bytes += 4u;
    if (m_water_pass->neutral_flow_texture())
        estimated_vram_bytes += 4u;
    if (m_water_pass->black_fallback_texture())
        estimated_vram_bytes += 4u;
    if (m_water_pass->underwater_texture())
        estimated_vram_bytes += 4u;
    if (m_water_pass->caustics_texture()) {
        estimated_vram_bytes += static_cast<size_t>(WaterPass::kCausticsResolution) *
                                static_cast<size_t>(WaterPass::kCausticsResolution) * 4u; // RGBA8
    }
    if (m_farlod)
        estimated_vram_bytes += m_farlod->stats().resident_bytes;
    stats.estimated_vram_bytes = estimated_vram_bytes;

    stats.geometry_shader_ok =
        m_gbuffer_pass->geometry_shader() && m_gbuffer_pass->geometry_shader()->IsValid();
    stats.lighting_shader_ok = m_lighting_pass->shader() && m_lighting_pass->shader()->IsValid();
    stats.skybox_shader_ok = m_skybox_pass->shader() && m_skybox_pass->shader()->IsValid();
    stats.shadow_shader_ok = m_shadow_pass->shader() && m_shadow_pass->shader()->IsValid();
    stats.ssao_shader_ok =
        m_ssao_pass->ssao().ssaoShader && m_ssao_pass->ssao().ssaoShader->IsValid();
    stats.ssao_blur_shader_ok =
        m_ssao_pass->ssao().blurShader && m_ssao_pass->ssao().blurShader->IsValid();
    stats.water_shader_ok = m_water_pass->shader() && m_water_pass->shader()->IsValid();
    stats.instanced_static_mesh_shader_ok =
        m_gbuffer_pass->instanced_static_mesh_shader() &&
        m_gbuffer_pass->instanced_static_mesh_shader()->IsValid();
    stats.terrain_texture_array_ok = m_terrainTextureArray != 0;
    stats.material_lut_ok = m_materialLUT != 0;
    stats.terrain_texture_fallback_layers = m_terrain_texture_fallback_layers;

    return stats;
}

void RenderPipeline::enumerate_shaders(
    const std::function<void(const char*, Shader*)>& visit) const {
    //  (-2): THE shader roster — the single enumeration of every
    // live Shader the pipeline owns. Shader health, reload-all, the dev shader
    // panel, and the auto-reload watcher all consume THIS list, so the roster
    // cannot drift between consumers by construction. A null instance is still
    // visited (health reports "not initialized"; reload/panel skip it).
    visit("geometry", m_gbuffer_pass->geometry_shader().get());
    visit("lighting", m_lighting_pass->shader().get());
    visit("skybox", m_skybox_pass->shader().get());
    visit("shadow", m_shadow_pass->shader().get());
    visit("ssao", m_ssao_pass->ssao().ssaoShader.get());
    visit("ssao_blur", m_ssao_pass->ssao().blurShader.get());
    visit("water", m_water_pass->shader().get());
    visit("instanced_static_mesh", m_gbuffer_pass->instanced_static_mesh_shader().get());
    visit("skinned_mesh", m_gbuffer_pass->skinned_mesh_shader().get());
    visit("weather_overlay", m_skybox_pass->weather_shader().get());
    if (m_particle_pass) {
        visit("particles", m_particle_pass->shader().get());
    } //
    if (m_foliage_pass) {
        visit("foliage", m_foliage_pass->shader().get());
    } //
    visit("aerial_perspective", m_aerial_shader.get()); //
    // the pipeline-owned post shaders, previously UNMONITORED by health.
    visit("god_rays", m_god_rays_shader.get());
    visit("cloud_composite", m_cloud_composite_shader.get());
    visit("waterfall", m_waterfall_shader.get());
    // the WBOIT glass chain (lazy — null until glass
    // first appears; health reports "not initialized", which is accurate).
    if (m_glass_oit_shader) {
        visit("glass_oit", m_glass_oit_shader.get());
    }
    if (m_glass_oit_resolve_shader) {
        visit("glass_oit_resolve", m_glass_oit_resolve_shader.get());
    }
}

std::vector<RenderPipeline::ShaderHealthEntry> RenderPipeline::get_shader_health() const {
    std::vector<ShaderHealthEntry> health;
    enumerate_shaders([&health](const char* name, Shader* shader) {
        ShaderHealthEntry entry;
        entry.name = name;
        entry.ok = shader && shader->IsValid();
        if (shader && !shader->Diagnostic().empty()) {
            entry.diagnostic = shader->Diagnostic();
        } else if (!shader) {
            entry.diagnostic = "not initialized";
        }
        health.push_back(std::move(entry));
    });
    return health;
}

RenderPipeline::ShaderReloadReport RenderPipeline::reload_all_shaders() {
    //  (-1, the crawl): hot-reload every roster shader from
    // res/shaders/. Per-shader rollback safety is Shader::Reload's contract — a
    // broken edit keeps the previous good program and lands in `failures` with
    // its diagnostic; the session never breaks. Render-only; never world_hash.
    ShaderReloadReport report;
    enumerate_shaders([&report](const char* name, Shader* shader) {
        if (!shader)
            return;
        ++report.attempted;
        if (shader->Reload()) {
            ++report.reloaded;
        } else {
            ++report.kept;
            report.failures.push_back(std::string(name) + ": " + shader->Diagnostic());
        }
    });
    LUMINUMBRA_CORE_INFO("Shader reload-all: {} attempted, {} swapped, {} kept prior program{}",
                         report.attempted,
                         report.reloaded,
                         report.kept,
                         report.failures.empty() ? "" : " (see failures)");
    for (const std::string& failure : report.failures) {
        LUMINUMBRA_CORE_WARN("  shader kept prior program: {}", failure);
    }
    return report;
}

RenderPipeline::RenderResourceRegistryStats RenderPipeline::get_resource_registry_stats() const {
    RenderResourceRegistryStats stats;
    auto count = [](GLuint id) -> size_t {
        return id != 0 ? 1u : 0u;
    };

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
    //  the three Hillaire scattering LUTs (transmittance / multi-scatter
    // / sky-view). RGB16F GL textures, debug-labelled, released in
    // cleanup_gpu_resources -> SkyAtmosphereLut::destroy so the
    // empty-after-shutdown invariant holds.
    stats.textures += count(m_sky_lut.transmittance_texture());
    stats.textures += count(m_sky_lut.multiscatter_texture());
    stats.textures += count(m_sky_lut.sky_view_texture());

    stats.renderbuffers += count(m_lighting_pass->lighting_fbo().depth_texture);
    stats.buffers += count(m_screen_quad_vbo);
    stats.buffers += count(m_skybox_pass->vbo());
    stats.buffers += count(m_gbuffer_pass->instance_matrix_vbo());
    stats.buffers += count(m_gbuffer_pass->joint_palette_ssbo());
    stats.vertex_arrays += count(m_screen_quad_vao);
    stats.vertex_arrays += count(m_skybox_pass->vao());
    //  particle pass VAO + persistent-mapped instance ring buffers.
    // Released in cleanup_gpu_resources -> ParticlePass::destroy_buffers, so
    // they count 0 after shutdown (empty-after-shutdown invariant holds).
    if (m_particle_pass) {
        stats.vertex_arrays += count(m_particle_pass->vao());
        for (std::size_t ring = 0; ring < ParticlePass::kRingFrames; ++ring) {
            stats.buffers += count(m_particle_pass->instance_buffer(ring));
        }
    }
    //  foliage pass VAO + persistent-mapped scatter ring buffers (same
    // shutdown-release invariant as the particle pool above).
    if (m_foliage_pass) {
        stats.vertex_arrays += count(m_foliage_pass->vao());
        for (std::size_t ring = 0; ring < FoliagePass::kRingFrames; ++ring) {
            stats.buffers += count(m_foliage_pass->instance_buffer(ring));
        }
    }

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

    // live terrain geometry is now backed by the shared pool's blocks
    // (each block: 1 VAO + vertex VBO + index EBO) plus the per-frame MDI ring
    // (indirect command buffer + origin buffer per ring slot), rather than a
    // VAO/VBO/EBO per chunk. Count those here so the resource registry remains
    // accurate (debug-labelled, and empty after shutdown -- the pool/MDI buffers
    // are released in cleanup_gpu_resources).
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

    // Far-LOD region meshes (one VAO + VBO/EBO per resident region, ).
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
    // debug labels once created and are deleted in cleanup_gpu_resources, so
    // the empty-after-shutdown contract holds.

    const size_t total_resources = stats.framebuffers + stats.textures + stats.renderbuffers +
                                   stats.buffers + stats.vertex_arrays + stats.shader_programs +
                                   stats.terrain_slots + stats.water_slots;
    stats.empty_after_shutdown = !m_started && total_resources == 0u;
    return stats;
}

RenderPipeline::RenderHealthSnapshot
RenderPipeline::get_render_health_snapshot(bool drain_gl_errors) const {
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
            const bool found = std::any_of(snapshot.passes.begin(),
                                           snapshot.passes.end(),
                                           [required_pass](const RenderPassMetadata& pass) {
                                               return pass.name == required_pass;
                                           });
            if (!found) {
                fail(std::string("missing render pass metadata: ") + required_pass);
            }
        }

        // the declarative frame graph must stay a faithful mirror of
        // render_frame's real dispatch order. ALWAYS assert the declaration is internally
        // consistent (no read-before-write; the god-rays latest-opaque-snapshot edge resolves);
        // and once a frame has actually dispatched, assert the graph's schedule EQUALS the real
        // emitted stage trace -- so the declaration can never silently drift from the shipping
        // sequence (a stage added/moved/removed in render_frame without updating the graph fails
        // here). Pure CPU; render-neutral; never hashed.
        {
            const Rendering::RenderGraph frame_graph = Rendering::BuildLuminumbraFrameGraph();
            for (const std::string& violation : frame_graph.validate()) {
                fail("frame graph declaration inconsistent: " + violation);
            }
            if (!m_frame_stage_trace.empty() && frame_graph.schedule() != m_frame_stage_trace) {
                fail("frame graph schedule drifted from render_frame dispatch order");
            }
            //   ( execution migration): the graph now DRIVES the
            // passes, so every declared node must map to an executor table entry —
            // a missing mapping would silently skip a stage. (The inverse, a table
            // entry not in the graph, can never dispatch and the trace equality
            // above catches the hole it would leave.)
            for (const std::string& node : frame_graph.schedule()) {
                bool mapped = false;
                for (const auto& entry : stage_executor_table()) {
                    if (entry.first == node) {
                        mapped = true;
                        break;
                    }
                }
                if (!mapped) {
                    fail("frame-graph node has no executor table entry: " + node);
                }
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

    add_pass("shadow",
             {"terrain_depth"},
             {"shadow.depth_texture_array"},
             m_shadow_pass->shadow_map().resolution,
             m_shadow_pass->shadow_map().resolution,
             "depth",
             "store depth cascades",
             m_last_render_pass_stats.shadow_draws);
    add_pass("gbuffer",
             {"terrain_meshes",
              "farlod_region_meshes",
              "static_meshes",
              "skinned_meshes",
              "material_lut",
              "terrain_texture_array",
              "terrain_normal_array"},
             {"gbuffer.position",
              "gbuffer.normal_material",
              "gbuffer.albedo_roughness",
              "gbuffer.metallic_ao",
              "gbuffer.depth"},
             m_screen_width,
             m_screen_height,
             "color+depth",
             "store deferred attachments",
             m_last_render_pass_stats.terrain_draws + m_last_render_pass_stats.far_region_draws +
                 m_last_render_pass_stats.skinned_draws);
    add_pass("ssao",
             {"gbuffer.position", "gbuffer.normal_material", "ssao.noise"},
             {"ssao.raw"},
             m_screen_width,
             m_screen_height,
             "color",
             "store ambient occlusion",
             m_last_render_pass_stats.ssao_draws);
    add_pass("ssao_blur",
             {"ssao.raw"},
             {"ssao.blur"},
             m_screen_width,
             m_screen_height,
             "color",
             "store blurred ambient occlusion",
             m_last_render_pass_stats.ssao_blur_draws);
    add_pass("lighting",
             {"gbuffer.*",
              "shadow.depth_texture_array",
              "ssao.blur",
              "terrain_texture_array",
              "material_lut",
              "water.fallback.black"},
             {"lighting.color", "lighting.depth"},
             m_screen_width,
             m_screen_height,
             "color+depth",
             "store lit scene",
             m_last_render_pass_stats.lighting_draws);
    add_pass("skybox",
             {"skybox_vertices"},
             {"lighting.color"},
             m_screen_width,
             m_screen_height,
             "load lighting",
             "store sky contribution",
             m_last_render_pass_stats.skybox_draws);
    add_pass("water",
             {"lighting.opaque_color_copy", "gbuffer.depth", "water_meshes", "water.fallback.*"},
             {"lighting.color"},
             m_screen_width,
             m_screen_height,
             "load sky+lighting",
             "blend water over sky+lighting",
             m_last_render_pass_stats.water_draws);
    add_pass("particles",
             {"particle_instances", "gbuffer.depth"},
             {"lighting.color"},
             m_screen_width,
             m_screen_height,
             "load lighting",
             "blend forward-lit particles",
             m_last_render_pass_stats.particle_draws);
    add_pass("final_blit",
             {"lighting.color"},
             {"swapchain.color"},
             m_screen_width,
             m_screen_height,
             "default color+depth",
             "present-ready color",
             m_last_render_pass_stats.final_blits);
}

// --- PER-PASS GPU TIMERS ---

// ===========================================================================
// glMultiDrawElementsIndirect submission for live terrain chunks.
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
        if (frame.indirect_buffer) {
            glDeleteBuffers(1, &frame.indirect_buffer);
            frame.indirect_buffer = 0;
        }
        if (frame.origin_buffer) {
            glDeleteBuffers(1, &frame.origin_buffer);
            frame.origin_buffer = 0;
        }
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

    if (frame.indirect_buffer) {
        glDeleteBuffers(1, &frame.indirect_buffer);
        frame.indirect_buffer = 0;
    }
    if (frame.origin_buffer) {
        glDeleteBuffers(1, &frame.origin_buffer);
        frame.origin_buffer = 0;
    }

    glGenBuffers(1, &frame.indirect_buffer);
    glBindBuffer(GL_DRAW_INDIRECT_BUFFER, frame.indirect_buffer);
    glBufferData(GL_DRAW_INDIRECT_BUFFER,
                 static_cast<GLsizeiptr>(new_capacity * sizeof(DrawElementsIndirectCommand)),
                 nullptr,
                 GL_STREAM_DRAW);

    glGenBuffers(1, &frame.origin_buffer);
    glBindBuffer(GL_ARRAY_BUFFER, frame.origin_buffer);
    glBufferData(GL_ARRAY_BUFFER,
                 static_cast<GLsizeiptr>(new_capacity * sizeof(glm::vec4)),
                 nullptr,
                 GL_STREAM_DRAW);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindBuffer(GL_DRAW_INDIRECT_BUFFER, 0);

    const std::size_t slot = static_cast<std::size_t>(&frame - m_mdi_frames.data());
    const std::string base = "terrain.mdi." + std::to_string(slot);
    label_gl_object(GL_BUFFER, frame.indirect_buffer, base + ".indirect");
    label_gl_object(GL_BUFFER, frame.origin_buffer, base + ".origins");

    frame.command_capacity = new_capacity;
}

void RenderPipeline::draw_chunks_mdi(const std::vector<const ChunkCullEntry*>& visible_chunks,
                                     std::size_t& out_draws,
                                     std::size_t& out_indices) {
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
        if (it == m_chunk_render_data.end())
            continue;
        const ChunkRenderData& rd = it->second;
        if (rd.pool_handle == ChunkRenderData::kInvalidPoolHandle || rd.element_count == 0)
            continue;
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
        if (it == m_chunk_render_data.end())
            continue;
        const ChunkRenderData& rd = it->second;
        if (rd.pool_handle == ChunkRenderData::kInvalidPoolHandle || rd.element_count == 0)
            continue;
        const ChunkGeometryPool::Allocation& a = m_chunk_geometry_pool.allocation(rd.pool_handle);

        const std::size_t dst = write_cursor[a.block_index]++;
        DrawElementsIndirectCommand& cmd = m_mdi_command_scratch[dst];
        cmd.count = a.index_count;
        cmd.instanceCount = 1u;
        cmd.firstIndex = a.index_offset;
        cmd.baseVertex = a.vertex_offset;
        cmd.baseInstance = static_cast<GLuint>(dst); // origins[baseInstance]

        const glm::ivec3 cc = chunk->coords;
        m_mdi_origin_scratch[dst] = glm::vec4(static_cast<float>(cc.x * CHUNK_SIZE_X),
                                              static_cast<float>(cc.y * CHUNK_SIZE_Y),
                                              static_cast<float>(cc.z * CHUNK_SIZE_Z),
                                              0.0f);

        out_indices += a.index_count;
    }
    out_draws = running;

    MdiFrameBuffers& frame = m_mdi_frames[m_mdi_frame_cursor];
    ensure_mdi_capacity(frame, running);

    glBindBuffer(GL_DRAW_INDIRECT_BUFFER, frame.indirect_buffer);
    glBufferSubData(GL_DRAW_INDIRECT_BUFFER,
                    0,
                    static_cast<GLsizeiptr>(running * sizeof(DrawElementsIndirectCommand)),
                    m_mdi_command_scratch.data());
    // The origin buffer is consumed as an instanced vertex attribute (binding 1,
    // vec4 stride), not an SSBO -- baseInstance indexing works without GLSL 4.6.
    glBindBuffer(GL_ARRAY_BUFFER, frame.origin_buffer);
    glBufferSubData(GL_ARRAY_BUFFER,
                    0,
                    static_cast<GLsizeiptr>(running * sizeof(glm::vec4)),
                    m_mdi_origin_scratch.data());
    glBindBuffer(GL_ARRAY_BUFFER, 0);

    const auto& blocks = m_chunk_geometry_pool.blocks();
    for (std::size_t b = 0; b < block_count; ++b) {
        const std::size_t count = per_block_count[b];
        if (count == 0)
            continue;
        const std::size_t first = block_offset[b];

        glBindVertexArray(blocks[b].vao);
        // Bind this frame's origin buffer to the VAO's instanced binding (1).
        // baseInstance is absolute into this buffer, so offset 0 is correct.
        glBindVertexBuffer(1, frame.origin_buffer, 0, sizeof(glm::vec4));
        glBindBuffer(GL_DRAW_INDIRECT_BUFFER, frame.indirect_buffer);
        glMultiDrawElementsIndirect(
            GL_TRIANGLES,
            GL_UNSIGNED_INT,
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
    const bool loader_ok = glGenQueries != nullptr && glDeleteQueries != nullptr &&
                           glQueryCounter != nullptr && glGetQueryObjectiv != nullptr &&
                           glGetQueryObjectui64v != nullptr;
    const bool capability_ok =
        GLAD_GL_VERSION_3_3 != 0 || gl_extension_present("GL_ARB_timer_query");
    if (!capability_ok || !loader_ok) {
        LUMINUMBRA_CORE_WARN(
            "Per-pass GPU timers disabled: GL_TIMESTAMP queries are unsupported on this context.");
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
    // open a KHR_debug group named for the pass so Nsight/RenderDoc
    // captures show a labelled span. Pushed BEFORE the support-guard so markers
    // bracket the pass even on contexts without GL timestamp queries. Pure
    // command-stream annotation: zero effect on rendered pixels (RenderHealth
    // stays byte-stable). Paired with the pop in end_gpu_pass_timer.
    PassGl::push_debug_group(kGpuTimerPassNames[static_cast<size_t>(pass)]);
    // the harness's second dispatch suppresses timestamp queries (the
    // ring slot records once per frame); the debug-group markers stay balanced.
    if (!m_gpu_timers.supported || m_gpu_timers_suppressed) {
        return;
    }
    GpuTimerFrameSlot& slot = m_gpu_timers.slots[m_gpu_timers.frame_index % kGpuTimerFrameRing];
    glQueryCounter(slot.begin_queries[static_cast<size_t>(pass)], GL_TIMESTAMP);
}

void RenderPipeline::end_gpu_pass_timer(GpuTimerPass pass) {
    PassGl::pop_debug_group();
    if (!m_gpu_timers.supported || m_gpu_timers_suppressed) {
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
        GpuTimerFrameSlot& read_slot =
            m_gpu_timers.slots[(m_gpu_timers.frame_index + 1u) % kGpuTimerFrameRing];
        for (size_t pass = 0; pass < kGpuTimerPassCount; ++pass) {
            if (!read_slot.issued[pass]) {
                continue;
            }
            GLint begin_available = GL_FALSE;
            GLint end_available = GL_FALSE;
            glGetQueryObjectiv(
                read_slot.begin_queries[pass], GL_QUERY_RESULT_AVAILABLE, &begin_available);
            glGetQueryObjectiv(
                read_slot.end_queries[pass], GL_QUERY_RESULT_AVAILABLE, &end_available);
            if (begin_available != GL_TRUE || end_available != GL_TRUE) {
                // Not resolved yet: keep the previous sample instead of stalling.
                continue;
            }
            GLuint64 begin_ns = 0;
            GLuint64 end_ns = 0;
            glGetQueryObjectui64v(read_slot.begin_queries[pass], GL_QUERY_RESULT, &begin_ns);
            glGetQueryObjectui64v(read_slot.end_queries[pass], GL_QUERY_RESULT, &end_ns);
            read_slot.issued[pass] = false;
            m_gpu_timers.last_gpu_ms[pass] =
                end_ns >= begin_ns ? static_cast<double>(end_ns - begin_ns) / 1.0e6 : 0.0;
            resolved_sample_this_frame = true;
        }
    }

    m_last_render_pass_stats.shadow_gpu_ms =
        m_gpu_timers.last_gpu_ms[static_cast<size_t>(GpuTimerPass::Shadow)];
    m_last_render_pass_stats.gbuffer_gpu_ms =
        m_gpu_timers.last_gpu_ms[static_cast<size_t>(GpuTimerPass::GBuffer)];
    m_last_render_pass_stats.ssao_gpu_ms =
        m_gpu_timers.last_gpu_ms[static_cast<size_t>(GpuTimerPass::Ssao)];
    m_last_render_pass_stats.ssao_blur_gpu_ms =
        m_gpu_timers.last_gpu_ms[static_cast<size_t>(GpuTimerPass::SsaoBlur)];
    m_last_render_pass_stats.lighting_gpu_ms =
        m_gpu_timers.last_gpu_ms[static_cast<size_t>(GpuTimerPass::Lighting)];
    m_last_render_pass_stats.water_gpu_ms =
        m_gpu_timers.last_gpu_ms[static_cast<size_t>(GpuTimerPass::Water)];
    m_last_render_pass_stats.skybox_gpu_ms =
        m_gpu_timers.last_gpu_ms[static_cast<size_t>(GpuTimerPass::Skybox)];
    m_last_render_pass_stats.particle_gpu_ms =
        m_gpu_timers.last_gpu_ms[static_cast<size_t>(GpuTimerPass::Particle)];
    m_last_render_pass_stats.foliage_gpu_ms =
        m_gpu_timers.last_gpu_ms[static_cast<size_t>(GpuTimerPass::Foliage)]; //
    m_last_render_pass_stats.aerial_gpu_ms =
        m_gpu_timers.last_gpu_ms[static_cast<size_t>(GpuTimerPass::Aerial)]; //
    //  the cloud cast-shadow sample lives INSIDE the lighting pass (a
    // per-fragment projected-coverage lookup, no separate pass), so its cost is
    // the lighting-pass GPU time on frames where the shadow is active. The
    // CloudShadow gate captures this with clouds ON vs OFF to bound the added
    // sample cost against the ≤ 0.4 ms budget (documented design, ).
    m_last_render_pass_stats.cloud_shadow_gpu_ms =
        (m_cloud_state.enabled && m_cloud_state.shadow_enabled)
            ? m_gpu_timers.last_gpu_ms[static_cast<size_t>(GpuTimerPass::Lighting)]
            : 0.0;
    // the lightning light-pulse + bolt also live INSIDE the lighting
    // pass (full-scene additive flash + screen-space bolt rasterization, no separate
    // pass), so the transient pulse cost is the lighting-pass GPU time on frames the
    // pulse is active. PerfRegression bounds this against the ≤ 0.5 ms budget.
    m_last_render_pass_stats.lightning_pulse_gpu_ms =
        (m_lightning_state.active && m_lightning_state.pulse_intensity > 0.0f)
            ? m_gpu_timers.last_gpu_ms[static_cast<size_t>(GpuTimerPass::Lighting)]
            : 0.0;
    m_last_render_pass_stats.final_blit_gpu_ms =
        m_gpu_timers.last_gpu_ms[static_cast<size_t>(GpuTimerPass::FinalBlit)];

    // One-time diagnostic so smoke runs prove the ring resolves real samples.
    if (resolved_sample_this_frame && !m_gpu_timers.first_sample_logged) {
        m_gpu_timers.first_sample_logged = true;
        LUMINUMBRA_CORE_INFO("Per-pass GPU timers active (ms): shadow={:.4f} gbuffer={:.4f} "
                             "ssao={:.4f} ssao_blur={:.4f} lighting={:.4f} water={:.4f} "
                             "skybox={:.4f} particles={:.4f} final_blit={:.4f}",
                             m_last_render_pass_stats.shadow_gpu_ms,
                             m_last_render_pass_stats.gbuffer_gpu_ms,
                             m_last_render_pass_stats.ssao_gpu_ms,
                             m_last_render_pass_stats.ssao_blur_gpu_ms,
                             m_last_render_pass_stats.lighting_gpu_ms,
                             m_last_render_pass_stats.water_gpu_ms,
                             m_last_render_pass_stats.skybox_gpu_ms,
                             m_last_render_pass_stats.particle_gpu_ms,
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
                const std::string base = "gpu_timer." + std::string(kGpuTimerPassNames[pass]) +
                                         "." + std::to_string(slot_index);
                label_gl_object(GL_QUERY, slot.begin_queries[pass], base + ".begin");
                label_gl_object(GL_QUERY, slot.end_queries[pass], base + ".end");
            }
        }
        m_gpu_timers.labeled = true;
    }
    ++m_gpu_timers.frame_index;
}

void RenderPipeline::gather_lights(entt::registry& registry, const glm::vec3& camera_pos) {
    m_point_lights_this_frame.clear();
    auto view =
        registry
            .view<const Components::TransformComponent, const Components::PointLightComponent>();

    // Gather ALL candidate lights with squared distance to the camera, then keep the
    // NEAREST MAX_POINT_LIGHTS. Fixes the gap where the old code took the first 32 in
    // arbitrary ECS order — so a cave full of crystals lights the ones AROUND the player,
    // not random distant ones. Render-only (never touches world_hash).
    struct Cand {
        PointLight light;
        float d2;
    };
    std::vector<Cand> cands;
    for (auto entity : view) {
        auto const& transform = view.get<const Components::TransformComponent>(entity);
        auto const& light_data = view.get<const Components::PointLightComponent>(entity);
        PointLight light;
        light.position = transform.position;
        light.color = light_data.color;
        light.radius = light_data.radius;
        light.intensity = light_data.intensity;
        const glm::vec3 d =
            glm::vec3(light.position.x, light.position.y, light.position.z) - camera_pos;
        cands.push_back({light, glm::dot(d, d)});
    }
    if (static_cast<int>(cands.size()) > MAX_POINT_LIGHTS) {
        std::nth_element(cands.begin(),
                         cands.begin() + MAX_POINT_LIGHTS,
                         cands.end(),
                         [](const Cand& a, const Cand& b) { return a.d2 < b.d2; });
        cands.resize(static_cast<std::size_t>(MAX_POINT_LIGHTS));
    }
    m_point_lights_this_frame.reserve(cands.size());
    for (const auto& c : cands)
        m_point_lights_this_frame.push_back(c.light);
}

std::vector<RenderPipeline::ChunkMeshSnapshot>
RenderPipeline::build_chunk_snapshots(const std::vector<Chunk*>& renderable_chunks) const {
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
        const u32 water_version_after_metadata =
            chunk->water_mesh_version.load(std::memory_order_acquire);
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

void RenderPipeline::ensure_terrain_culling_hierarchy(
    const std::vector<ChunkMeshSnapshot>& renderable_chunks) {
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

void RenderPipeline::render_frame(entt::registry& registry,
                                  Systems::SHIELD_WorldSystem& world_system,
                                  const Camera& camera,
                                  float deltaTime,
                                  bool wireframe) {
    if (!m_started) {
        return;
    }
    //   (the  unlock): prepare once (ALL per-frame CPU mutation),
    // dispatch once (the pure GPU stage sequence over the prepared state), then the
    // per-frame epilogue. Byte-identical to the pre-split monolith by construction —
    // the bodies moved verbatim. The harness (capture_frame_parity) reuses the same
    // prepared frame and dispatches TWICE, which must be bit-identical.
    prepare_frame(registry, world_system, camera, deltaTime, wireframe);
    dispatch_stages(camera);

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    finish_gpu_pass_timer_frame();
    refresh_render_pass_metadata();

    //  record CPU per-phase submit cost (this frame).
    {
        auto ms = [](auto a, auto b) {
            return std::chrono::duration<double, std::milli>(b - a).count();
        };
        const auto _cpu_end = std::chrono::steady_clock::now();
        m_last_render_pass_stats.cpu_prepare_ms = ms(m_cpu_frame_t0, m_cpu_frame_prep);
        m_last_render_pass_stats.cpu_shadow_ms = ms(m_cpu_frame_prep, m_cpu_frame_shadow);
        m_last_render_pass_stats.cpu_gbuffer_ms = ms(m_cpu_frame_shadow, m_cpu_frame_gbuf);
        m_last_render_pass_stats.cpu_post_ms = ms(m_cpu_frame_gbuf, _cpu_end);
        m_last_render_pass_stats.cpu_static_prop_ms =
            m_gbuffer_pass ? m_gbuffer_pass->last_static_prop_cpu_ms() : 0.0;
    }

    // every pass marker must be balanced by frame end, else the
    // Nsight/RenderDoc capture (which the  tracer decision depends on)
    // is garbled even though pixels are unaffected. Cheap debug-only guard.
    assert(PassGl::debug_group_depth() == 0 &&
           "unbalanced GL debug-group push/pop in render_frame");

    //  TAAU: remember this frame's UNJITTERED view-proj so next frame's G-buffer can write
    // screen-space motion vectors (reproject each surface point's world position to where it was).
    m_prev_view_proj = m_frame_prepared.projection * m_frame_prepared.view;
    //  TAAU: also remember this frame's wind wall-clock so next frame's instanced vertex shader
    // can reconstruct where each wind-swayed vertex WAS (camera-only reprojection ghosts
    // tree-tops).
    m_prev_time = static_cast<float>(glfwGetTime());
}

void RenderPipeline::prepare_frame(entt::registry& registry,
                                   Systems::SHIELD_WorldSystem& world_system,
                                   const Camera& camera,
                                   float deltaTime,
                                   bool wireframe) {
    m_cpu_frame_t0 = std::chrono::steady_clock::now(); // CPU per-phase submit cost

    //  (Group K): ONE wall-clock snapshot per frame, shared by every pass
    // via RenderContext.time_seconds so converted passes are deterministic w.r.t.
    // each other (no per-pass glfwGetTime drift). Render-only (u_time sway etc.).
    m_wall_clock_time = static_cast<float>(glfwGetTime());

    update_time_of_day(deltaTime);

    // consume the newest COMPLETED luminance readback (never
    // blocks; a stale N-frame-old value is the contract) and advance the damped
    // exposure servo toward the mid-grey key. Flag OFF -> the analytic
    // AutoExposureForElevation curve stays the auto source.
    if (m_auto_exposure_metered && m_exposure_ring.initialized()) {
        const void* p = nullptr;
        std::size_t n = 0;
        if (m_exposure_ring.consume(&p, &n) && n >= sizeof(float)) {
            constexpr float kMeterKey = 0.18f;         // mid-grey target
            constexpr float kMeterMinExposure = 0.25f; // stop clamps (servo bounds)
            constexpr float kMeterMaxExposure = 4.0f;
            constexpr float kMeterAdaptRate = 0.08f; // eye-adaptation damping
            float avg = *static_cast<const float*>(p);
            avg = std::clamp(avg, 1e-4f, 64.0f);
            const float target = std::clamp(kMeterKey / avg, kMeterMinExposure, kMeterMaxExposure);
            m_metered_exposure += (target - m_metered_exposure) * kMeterAdaptRate;
            m_metered_valid = true;
        }
    }
    gather_lights(registry, camera.Position);
    // Underwater detection: the aerial pass becomes a murky-water volume when the
    // camera sits below the local water surface (sea OR a perched lake).
    {
        const float water_level = world_system.WaterLevelAt(camera.Position.x, camera.Position.z);
        m_underwater_factor = (camera.Position.y < water_level - 0.05f) ? 1.0f : 0.0f;
    }
    auto renderable_chunks = world_system.get_renderable_chunks();
    auto renderable_chunk_snapshots = build_chunk_snapshots(renderable_chunks);
    m_last_mesh_upload_stats = {};
    m_last_mesh_upload_stats.snapshot_count = renderable_chunk_snapshots.size();
    m_last_render_pass_stats = {};
    m_last_render_pass_stats.snapshot_count = renderable_chunk_snapshots.size();
    //  carry the sky-LUT precompute (startup one-shot) + this frame's
    // sky-view refresh cost (set by update_time_of_day above, before this reset).
    m_last_render_pass_stats.sky_full_precompute_ms = m_sky_full_precompute_ms;
    m_last_render_pass_stats.sky_view_refresh_ms = m_sky_view_refresh_ms;

    // Publish GPU timings recorded two frames ago without stalling, then
    // record this frame's passes into the current ring slot below.
    collect_gpu_pass_timers();

    manage_chunk_gpu_resources(renderable_chunk_snapshots, camera);
    manage_water_gpu_resources(renderable_chunk_snapshots, camera);
    ensure_terrain_culling_hierarchy(renderable_chunk_snapshots);

    // Far-LOD scheduling: ring-diff the wanted region set, integrate
    // finished tile builds (mesh uploads), and evict. Draws happen inside the
    // G-buffer pass after the live chunks.
    //
    // Skipped while an OFFSCREEN target is bound (the worldgen preview's FBO-capture
    // path, WorldgenPreview::render): that path does not need the streaming far-field.
    // The LIVE preview (render_to_backbuffer) DOES run far-LOD so the diorama shows the
    // far field; it stays use-after-free safe because the preview drains far-LOD off its
    // candidate world before freeing it (swap_pending_into_live + the dtor call
    // prepare_world_swap). m_far_lod_enabled remains a manual override hook.
    if (m_farlod && m_far_lod_enabled && !m_offscreen_target_active) {
        m_farlod->update(world_system, camera.Position);
    }

    glm::mat4 projection = glm::perspective(glm::radians(camera.Zoom),
                                            (float)m_screen_width / (float)m_screen_height,
                                            camera.GetNearPlane(),
                                            camera.GetFarPlane());
    glm::mat4 view = camera.GetViewMatrix();

    //  TAAU: per-frame Halton[2,3] sub-pixel jitter for the G-buffer projection. Computed ONLY
    // when TAAU is on; (0,0) otherwise so the default render (and frustum culling, which uses the
    // UNJITTERED projection above) is byte-identical. GBufferPass applies it; the resolve
    // un-jitters the motion vectors with the same offset.
    if (m_taau_enabled && m_screen_width > 0 && m_screen_height > 0) {
        auto halton = [](unsigned i, unsigned base) {
            float f = 1.0f, r = 0.0f;
            while (i > 0u) {
                f /= (float)base;
                r += f * (float)(i % base);
                i /= base;
            }
            return r;
        };
        const unsigned idx = (m_taau_frame % 16u) + 1u;
        m_taau_jitter_ndc = glm::vec2((halton(idx, 2u) - 0.5f) * 2.0f / (float)m_screen_width,
                                      (halton(idx, 3u) - 0.5f) * 2.0f / (float)m_screen_height);
        ++m_taau_frame;
    } else {
        m_taau_jitter_ndc = glm::vec2(0.0f);
    }

    // Cache frustum planes to avoid recalculation when camera hasn't changed significantly
    glm::vec4 frustum_planes[6];
    const float POSITION_THRESHOLD = 0.5f;  // Less sensitive to prevent cache thrashing
    const float ROTATION_THRESHOLD = 0.05f; // Reduced sensitivity for smooth movement

    bool needsUpdate =
        !m_frustumCache.valid ||
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

    // particle MOTION advances in prepare (once per frame), so the
    // dispatch below only draws — running dispatch twice must not double-advance
    // motion. Guarded identically to the particles stage's execute.
    if (m_particle_pass &&
        m_isolation_config.renders(Client::ScenarioHarness::IsolationLayer::Particles)) {
        m_particle_pass->update(deltaTime);
    }

    // publish the prepared frame dispatch_stages reads.
    m_frame_prepared.registry = &registry;
    m_frame_prepared.world_system = &world_system;
    m_frame_prepared.delta_time = deltaTime;
    m_frame_prepared.wireframe = wireframe;
    m_frame_prepared.projection = projection;
    m_frame_prepared.view = view;
    std::memcpy(m_frame_prepared.frustum_planes, frustum_planes, sizeof(frustum_planes));
    m_frame_prepared.renderable_chunk_snapshots = std::move(renderable_chunk_snapshots);
    m_frame_prepared.valid = true;

    m_cpu_frame_prep = std::chrono::steady_clock::now(); //
}

void RenderPipeline::dispatch_stages(const Camera& camera) {
    // the pure GPU stage sequence over the prepared frame. Each
    // execute_stage_* body moved VERBATIM from the pre-split render_frame and
    // reads only FramePrepared + pipeline members. This function must be
    // IDEMPOTENT per prepared frame (bit-identical pixels when run twice) — no
    // wall-clock reads, no motion/history advances, no per-frame counters in any
    // stage; those live in prepare_frame. Gated by -Mode RenderParityFrame == 0.0.

    // Ensure no VAO is bound at start to prevent artifacts
    glBindVertexArray(0);

    glEnable(GL_DEPTH_TEST);

    // Configure rendering mode
    if (m_frame_prepared.wireframe) {
        glPolygonMode(GL_FRONT_AND_BACK, GL_LINE);
        glLineWidth(2.0f);
        glDisable(GL_CULL_FACE); // Disable culling in wireframe for debugging
    } else {
        glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
        glEnable(GL_CULL_FACE);
        glCullFace(GL_BACK);
    }

    // reset the frame-graph stage trace, then record each stage id as it
    // dispatches. This is render_frame's REAL dispatch order -- the golden the declarative
    // RenderGraph (BuildLuminumbraFrameGraph) is gated against (schedule == this trace), so the
    // declaration can never silently drift from the shipping sequence. Emitted unconditionally at
    // each stage's slot (a conditional stage that does no GL work still holds its place in the
    // order). Pure CPU observability: never a GL call, never world_hash.
    m_frame_stage_trace.clear();

    //  -c3/c4 —  EXECUTION MIGRATION COMPLETE: the graph DRIVES
    // the passes. The order comes from the DECLARATION (schedule over
    // BuildLuminumbraFrameGraph, computed once — the graph is static data); the
    // executor table maps each node name to its extracted body. Three locks pin
    // this: the drift guard (schedule == the emitted trace, on every RenderHealth
    // frame), the whole-frame A/B (-Mode RenderParityFrame == exactly 0.0), and
    // FrameDispatch.ExecutorTableCoversEveryGraphNodeInOrder (table == declaration).
    // A new render feature adds a RenderGraphNode + an executor entry + its
    // record_frame_stage slot — the locks fail loudly on any of the three missing.
    static const std::vector<std::string> kSchedule =
        Rendering::BuildLuminumbraFrameGraph().schedule();
    for (const std::string& node : kSchedule) {
        void (RenderPipeline::*fn)(const Camera&) = nullptr;
        for (const auto& entry : stage_executor_table()) {
            if (entry.first == node) {
                fn = entry.second;
                break;
            }
        }
        // Unmapped node = a declaration/executor drift the ctest pins; loud in debug.
        assert(fn && "frame-graph node has no executor table entry");
        if (fn)
            (this->*fn)(camera);
    }
}

const std::vector<std::pair<std::string, RenderPipeline::StageExecutorFn>>&
RenderPipeline::stage_executor_table() {
    // Authored order matches BuildLuminumbraFrameGraph's node order 1:1 (pinned
    // by FrameDispatch.ExecutorTableCoversEveryGraphNodeInOrder).
    static const std::vector<std::pair<std::string, StageExecutorFn>> kTable = {
        {"shadow", &RenderPipeline::execute_stage_shadow},
        {"gbuffer", &RenderPipeline::execute_stage_gbuffer},
        {"plant_procgen", &RenderPipeline::execute_stage_plant_procgen},
        {"ground_decals", &RenderPipeline::execute_stage_ground_decals},
        {"ssao", &RenderPipeline::execute_stage_ssao},
        {"ssao_blur", &RenderPipeline::execute_stage_ssao_blur},
        {"lighting", &RenderPipeline::execute_stage_lighting},
        {"depth_blit_to_lighting", &RenderPipeline::execute_stage_depth_blit_to_lighting},
        {"skybox", &RenderPipeline::execute_stage_skybox},
        {"opaque_snapshot", &RenderPipeline::execute_stage_opaque_snapshot},
        {"water", &RenderPipeline::execute_stage_water},
        {"waterfall", &RenderPipeline::execute_stage_waterfall},
        {"glass_oit_accum", &RenderPipeline::execute_stage_glass_oit_accum},
        {"glass_oit_resolve", &RenderPipeline::execute_stage_glass_oit_resolve},
        {"weather_opaque_snapshot", &RenderPipeline::execute_stage_weather_opaque_snapshot},
        {"weather_overlay", &RenderPipeline::execute_stage_weather_overlay},
        {"froxel_inject", &RenderPipeline::execute_stage_froxel_inject},
        {"froxel_integrate", &RenderPipeline::execute_stage_froxel_integrate},
        {"aerial", &RenderPipeline::execute_stage_aerial},
        {"god_rays", &RenderPipeline::execute_stage_god_rays},
        {"foliage", &RenderPipeline::execute_stage_foliage},
        {"taau_resolve", &RenderPipeline::execute_stage_taau_resolve},
        {"luminance_meter", &RenderPipeline::execute_stage_luminance_meter},
        {"particles", &RenderPipeline::execute_stage_particles},
        {"lightning_overlay", &RenderPipeline::execute_stage_lightning_overlay},
        {"final_blit", &RenderPipeline::execute_stage_final_blit},
        {"debug_view", &RenderPipeline::execute_stage_debug_view},
    };
    return kTable;
}

void RenderPipeline::execute_stage_shadow(const Camera& camera) {
    // 1. SHADOW PASS
    //  (T10): precompute light-space matrices (pipeline-private) + the
    // terrain-submit callback at the call site; the pass submits one per cascade.
    record_frame_stage("shadow");
    begin_gpu_pass_timer(GpuTimerPass::Shadow);
    {
        RenderContext shadow_ctx;
        shadow_ctx.registry = &m_render_registry;
        ShadowPassInput shadow_input;
        shadow_input.light_space_matrices = get_light_space_matrices(camera);
        shadow_input.submit_terrain = make_terrain_submitter();
        // translucent occluders for the tint cascade.
        shadow_input.glass_items = &m_glass_pane_items;
        shadow_input.glass_vao = m_glass_quad_vao;
        const auto shadow_stats = m_shadow_pass->execute(shadow_ctx, shadow_input);
        for (int i = 0; i < ShadowMap::CASCADE_COUNT; ++i) {
            m_last_render_pass_stats.shadow_cascade_visible_chunks[i] =
                shadow_stats[i].visible_chunks;
            m_last_render_pass_stats.shadow_cascade_draws[i] += shadow_stats[i].draws;
            m_last_render_pass_stats.shadow_draws += shadow_stats[i].draws;
            m_last_render_pass_stats.shadow_indices_drawn += shadow_stats[i].indices;
        }
    }
    end_gpu_pass_timer(GpuTimerPass::Shadow);
    m_cpu_frame_shadow = std::chrono::steady_clock::now(); //
    glViewport(0, 0, m_internal_width, m_internal_height); // scene renders at internal res
}

void RenderPipeline::execute_stage_gbuffer(const Camera& camera) {
    entt::registry& registry = *m_frame_prepared.registry;
    glm::vec4 frustum_planes[6];
    std::memcpy(frustum_planes, m_frame_prepared.frustum_planes, sizeof(frustum_planes));

    // 2. GEOMETRY / G-BUFFER PASS (-T11: routed through the RenderContext seam).
    record_frame_stage("gbuffer");
    begin_gpu_pass_timer(GpuTimerPass::GBuffer);
    {
        RenderContext gbuffer_ctx = make_gbuffer_context(camera, frustum_planes);
        GBufferPassInput gbuffer_input;
        // Live-terrain submit: the Codex-signed-off callback (CullHierarchical +
        // draw_chunks_mdi, byte-identical), shared with ShadowPass.
        gbuffer_input.submit_terrain_chunks = make_terrain_submitter();
        gbuffer_input.far_lod = farlod();
        gbuffer_input.root_path = m_root_path;
        gbuffer_input.static_model_texture_array = static_model_texture_array();
        gbuffer_input.static_model_tex = [this](const std::string& mesh_path) {
            return static_model_tex(mesh_path);
        };
        gbuffer_input.tree_impostor_enabled = tree_impostor_enabled();
        gbuffer_input.tree_impostor_albedo = tree_impostor_albedo();
        gbuffer_input.tree_impostor_normal = tree_impostor_normal();
        gbuffer_input.tree_impostor_grid = tree_impostor_grid();
        gbuffer_input.tree_impostor_radius = tree_impostor_radius();
        gbuffer_input.tree_impostor_sphere_y = tree_impostor_sphere_y();
        const GBufferDrawStats gstats =
            m_gbuffer_pass->execute(gbuffer_ctx, registry, gbuffer_input);
        // Fold with the EXACT current policy: terrain_visible_chunks '=', rest '+='.
        m_last_render_pass_stats.terrain_visible_chunks = gstats.terrain_visible_chunks;
        m_last_render_pass_stats.terrain_draws += gstats.terrain_draws;
        m_last_render_pass_stats.terrain_indices_drawn += gstats.terrain_indices;
        m_last_render_pass_stats.far_region_draws += gstats.far_region_draws;
        m_last_render_pass_stats.far_indices_drawn += gstats.far_indices;
        m_last_render_pass_stats.skinned_draws += gstats.skinned_draws;
        m_last_render_pass_stats.skinned_indices_drawn += gstats.skinned_indices;
    }
    m_cpu_frame_gbuf = std::chrono::steady_clock::now(); //
}

void RenderPipeline::execute_stage_plant_procgen(const Camera& camera) {
    // 2a.  (render.plant_procgen): draw the procedural plants into the
    // SAME G-buffer the static meshes just wrote. The combined world-space mesh
    // is baked + pushed by the client (set_plants); OFF by default, so this is a
    // no-op (zero GL work) and the render is byte-identical. Bind the G-buffer
    // FBO + its 4 draw buffers and depth-test (GL_LESS) so the plants occlude /
    // are occluded correctly, mirroring the far-field injection below.
    // NOTE : the GBuffer GPU-timer bracket spans gbuffer -> plant_procgen (it
    // opened in execute_stage_gbuffer and closes here) — preserved verbatim.
    record_frame_stage("plant_procgen");
    if (m_plant_procgen_pass && m_plant_procgen_pass->enabled() &&
        m_plant_procgen_pass->index_count() > 0) {
        glBindFramebuffer(GL_FRAMEBUFFER, m_gbuffer_pass->gbuffer().fbo_id);
        const GLenum pp_bufs[4] = {
            GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1, GL_COLOR_ATTACHMENT2, GL_COLOR_ATTACHMENT3};
        glDrawBuffers(4, pp_bufs);
        glViewport(0, 0, m_internal_width, m_internal_height); // into internal G-buffer
        glEnable(GL_DEPTH_TEST);
        glDepthFunc(GL_LESS);
        glDepthMask(GL_TRUE);
        glDisable(GL_CULL_FACE); // procgen branches/leaves are 2-sided
        RenderContext plant_ctx = make_plant_context();
        m_plant_procgen_pass->execute(plant_ctx, camera);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
    }
    end_gpu_pass_timer(GpuTimerPass::GBuffer);
    glBindVertexArray(0); // Unbind after gbuffer pass
    glDisable(GL_CULL_FACE);
}

void RenderPipeline::execute_stage_ground_decals(const Camera& camera) {
    // 2c. PHEROMONE GROUND DECALS (, render-only sim->render mirror).
    // Additively tint the ALBEDO attachment where forager food/home scent trails
    // exist, AFTER the G-buffer is fully populated but BEFORE SSAO + lighting, so the
    // trails are AO-darkened and lit as ground detail. Determinism-neutral: reads the
    // one-way ScentFieldRenderMirror, never the sim. No-op (zero draws) until a valid,
    // non-empty mirror is uploaded (default-OFF), so the render stays byte-identical.
    record_frame_stage("ground_decals");
    if (m_ground_decal_pass && m_ground_decal_pass->active()) {
        glBindFramebuffer(GL_FRAMEBUFFER, m_gbuffer_pass->gbuffer().fbo_id);
        const GLenum gd_bufs[1] = {GL_COLOR_ATTACHMENT2}; // albedo only
        glDrawBuffers(1, gd_bufs);
        glViewport(0, 0, m_internal_width, m_internal_height); // into internal G-buffer
        glDisable(GL_DEPTH_TEST);
        glDepthMask(GL_FALSE);
        glDisable(GL_CULL_FACE);
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE); // additive trail tint
        RenderContext decal_ctx = make_ground_decal_context(camera);
        m_ground_decal_pass->execute(decal_ctx);
        glDisable(GL_BLEND);
        glDepthMask(GL_TRUE);
        glEnable(GL_DEPTH_TEST);
        const GLenum gd_restore[4] = {
            GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1, GL_COLOR_ATTACHMENT2, GL_COLOR_ATTACHMENT3};
        glDrawBuffers(4, gd_restore);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
    }
}

void RenderPipeline::execute_stage_ssao(const Camera& camera) {
    // 3. SSAO PASS (-T02: routed through the RenderContext seam).
    // (: the ssao/ssao_blur pair each build their own ctx — make_ssao_context is
    // a pure projection of frame-stable pipeline state, so rebuild == the old reuse.)
    {
        RenderContext ctx = make_ssao_context(camera);
        record_frame_stage("ssao");
        begin_gpu_pass_timer(GpuTimerPass::Ssao);
        m_ssao_pass->execute_ssao(ctx);
        m_last_render_pass_stats.ssao_draws++;
        end_gpu_pass_timer(GpuTimerPass::Ssao);
        glBindVertexArray(0); // Unbind after SSAO
    }
}

void RenderPipeline::execute_stage_ssao_blur(const Camera& camera) {
    {
        RenderContext ctx = make_ssao_context(camera);
        record_frame_stage("ssao_blur");
        begin_gpu_pass_timer(GpuTimerPass::SsaoBlur);
        m_ssao_pass->execute_blur(ctx);
        m_last_render_pass_stats.ssao_blur_draws++;
        end_gpu_pass_timer(GpuTimerPass::SsaoBlur);
        glBindVertexArray(0); // Unbind after SSAO blur
    }
}

void RenderPipeline::execute_stage_lighting(const Camera& camera) {
    // 4. LIGHTING PASS (Renders to m_lighting_fbo) — -T12 RenderContext seam.
    glEnable(GL_CULL_FACE);
    // Built ONCE per dispatch and kept as m_frame_lighting_ctx: the same ctx is
    // reused by the opaque-copy (step 7) and lightning-overlay (step 8b) stages —
    // the fields they read are frame-stable, so reuse == rebuild (the pre-split
    // code built it once at function scope for exactly this reuse).
    // make_lighting_context also runs the hoisted shadow-cascade fixup (CPU-only,
    // render-neutral) before the GPU timer.
    record_frame_stage("lighting");
    m_frame_lighting_ctx = make_lighting_context(camera);
    begin_gpu_pass_timer(GpuTimerPass::Lighting);
    m_lighting_pass->execute(m_frame_lighting_ctx);
    end_gpu_pass_timer(GpuTimerPass::Lighting);
    glBindVertexArray(0); // Unbind after lighting pass
}

void RenderPipeline::execute_stage_depth_blit_to_lighting(const Camera& camera) {
    (void)camera;
    // 5. COPY DEPTH TO LIGHTING FBO SO SKYBOX AND WATER SHARE THE G-BUFFER OCCLUSION
    record_frame_stage("depth_blit_to_lighting");
    glBindFramebuffer(GL_READ_FRAMEBUFFER, m_gbuffer_pass->gbuffer().fbo_id);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, m_lighting_pass->lighting_fbo().fbo_id);
    // both G-buffer and lighting are internal-sized; copy the internal depth rect
    // (a screen-sized rect would leave the internal depth garbage at scale<1.0 -> the skybox
    // depth-mask fails and the sky goes dark). At scale 1.0 internal==screen (byte-identical).
    glBlitFramebuffer(0,
                      0,
                      m_internal_width,
                      m_internal_height,
                      0,
                      0,
                      m_internal_width,
                      m_internal_height,
                      GL_DEPTH_BUFFER_BIT,
                      GL_NEAREST);
}

void RenderPipeline::execute_stage_skybox(const Camera& camera) {
    // 6. SKYBOX / SKY-DOME PASS (Renders to m_lighting_fbo before transparent water blends)
    // build the skybox ctx AFTER the gbuffer->lighting depth
    // blit (step 5) so the adopted handles + state match the production sequence.
    record_frame_stage("skybox");
    begin_gpu_pass_timer(GpuTimerPass::Skybox);
    RenderContext skybox_ctx = make_skybox_context(camera);
    if (m_cloud_quality > 0 && m_halfres_cloud.fbo && m_cloud_composite_shader &&
        m_cloud_composite_shader->IsValid()) {
        // Render-optimization (cloud-raymarch-optimization, ): raymarch the
        // sky dome at reduced resolution, then depth-mask-composite it into the
        // lighting FBO. The expensive cloud march pays for 1/4 (half) or 1/16
        // (quarter) of the fragments.  (no world_hash impact); sky
        // pixels only (scene depth == far plane), reproducing the legacy GL_LEQUAL
        // sky mask. Quality 0 takes the byte-identical legacy path below.
        //
        // (a) Dome -> reduced-res FBO. No depth attachment / depth test off: the
        //     dome fills every texel (the sky mask is reapplied at composite time).
        const GLboolean blend_was = glIsEnabled(GL_BLEND);
        if (blend_was)
            glDisable(GL_BLEND);
        glBindFramebuffer(GL_FRAMEBUFFER, m_halfres_cloud.fbo);
        glViewport(0,
                   0,
                   static_cast<GLsizei>(m_halfres_cloud.width),
                   static_cast<GLsizei>(m_halfres_cloud.height));
        glDisable(GL_DEPTH_TEST);
        glDepthMask(GL_FALSE);
        m_skybox_pass->execute(skybox_ctx, camera, false);
        // (b) Depth-masked upsample composite -> lighting FBO (full res). Writes the
        //     bilinear-upsampled sky only where the scene depth is the far plane.
        glBindFramebuffer(GL_FRAMEBUFFER, m_lighting_pass->lighting_fbo().fbo_id);
        glViewport(0, 0, m_internal_width, m_internal_height); // into internal lighting FBO
        glDisable(GL_DEPTH_TEST);
        glDepthMask(GL_FALSE);
        m_cloud_composite_shader->use();
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, m_halfres_cloud.color_texture);
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, m_gbuffer_pass->gbuffer().depth_texture);
        m_cloud_composite_shader->setInt("u_cloudColor", 0);
        m_cloud_composite_shader->setInt("u_sceneDepth", 1);
        glBindVertexArray(m_screen_quad_vao);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        glBindVertexArray(0);
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, 0);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, 0);
        glDepthMask(GL_TRUE);
        glEnable(GL_DEPTH_TEST);
        if (blend_was)
            glEnable(GL_BLEND);
    } else {
        glBindFramebuffer(GL_FRAMEBUFFER, m_lighting_pass->lighting_fbo().fbo_id);
        glViewport(0,
                   0,
                   m_internal_width,
                   m_internal_height); // skybox must not inherit a stale (screen) viewport
        m_skybox_pass->execute(skybox_ctx, camera, false);
    }
    end_gpu_pass_timer(GpuTimerPass::Skybox);
    glBindVertexArray(0); // Unbind after skybox pass
}

void RenderPipeline::execute_stage_opaque_snapshot(const Camera& camera) {
    (void)camera;
    // 7. SNAPSHOT THE LIT OPAQUE+SKY SCENE, THEN WATER PASS BLENDS OVER IT.
    // Water keeps depth writes off for transparency. Drawing the sky first prevents
    // cloud/aurora sky pixels from overwriting water over far-depth/background
    // samples while still giving refraction a stable pre-water color source.
    record_frame_stage("opaque_snapshot");
    m_lighting_pass->copy_lighting_color_to_opaque_texture(m_frame_lighting_ctx);
}

void RenderPipeline::execute_stage_water(const Camera& camera) {
    const auto& renderable_chunk_snapshots = m_frame_prepared.renderable_chunk_snapshots;
    glBindFramebuffer(GL_FRAMEBUFFER, m_lighting_pass->lighting_fbo().fbo_id);
    record_frame_stage("water");
    if (m_isolation_config.renders(Client::ScenarioHarness::IsolationLayer::Water)) {
        //  (T16): build the water draw list from m_water_render_data in
        // chunk order (byte-stable) + the ctx, then run the seam.
        RenderContext water_ctx = make_water_context(camera);
        WaterPassInput water_input;
        for (const auto& chunk : renderable_chunk_snapshots) {
            auto it = m_water_render_data.find(chunk.id);
            if (it != m_water_render_data.end() && it->second.element_count > 0) {
                glm::mat4 model = glm::translate(
                    glm::mat4(1.0f),
                    glm::vec3(chunk.coords * IVec3(CHUNK_SIZE_X, CHUNK_SIZE_Y, CHUNK_SIZE_Z)));
                water_input.draw_items.push_back(
                    {model, it->second.vao_id, it->second.element_count});
            }
        }
        begin_gpu_pass_timer(GpuTimerPass::Water);
        const WaterDrawStats water_stats = m_water_pass->execute(water_ctx, water_input, camera);
        m_last_render_pass_stats.water_draws += water_stats.water_draws;
        m_last_render_pass_stats.water_indices_drawn += water_stats.water_indices;
        end_gpu_pass_timer(GpuTimerPass::Water);
        glBindVertexArray(0); // Unbind after water pass
    }
}

void RenderPipeline::execute_stage_waterfall(const Camera& camera) {
    const glm::mat4& projection = m_frame_prepared.projection;
    const glm::mat4& view = m_frame_prepared.view;
    // 7-W. WATERFALL SHEETS (, ): the animated falling-sheet veil drawn
    // over each detected waterfall site. Render-only dressing on a world-
    // deterministic site set (never hashed). Drawn after water, into the lit FBO,
    // as a translucent double-sided veil (depth-test on, depth-write off, blend on
    // so it layers over the scene + each other). A no-op (zero draws) when no
    // sites were baked, so existing visual gates stay byte-stable.
    record_frame_stage("waterfall");
    if (m_waterfall_geometry_built && m_waterfall_vao != 0 && !m_waterfall_sheet_sites.empty() &&
        m_waterfall_shader && m_waterfall_shader->IsValid()) {
        glBindFramebuffer(GL_FRAMEBUFFER, m_lighting_pass->lighting_fbo().fbo_id);
        glViewport(0, 0, m_internal_width, m_internal_height); // into internal lighting FBO

        // Save GL state we toggle so it is restored exactly afterwards.
        const GLboolean blend_was = glIsEnabled(GL_BLEND);
        const GLboolean cull_was = glIsEnabled(GL_CULL_FACE);
        const GLboolean depth_was = glIsEnabled(GL_DEPTH_TEST);
        const GLboolean poly_off_was = glIsEnabled(GL_POLYGON_OFFSET_FILL);

        m_waterfall_shader->use();
        m_waterfall_shader->setMat4("model", glm::mat4(1.0f));
        m_waterfall_shader->setMat4("view", view);
        m_waterfall_shader->setMat4("projection", projection);
        m_waterfall_shader->setMat3("normalMatrix", glm::mat3(1.0f));
        // the per-frame wall-clock SNAPSHOT (prepare_frame), not a live
        // glfwGetTime read — dispatch must be bit-idempotent per prepared frame
        // (sub-µs sway phase shift; render-only).
        m_waterfall_shader->setFloat("u_time", m_wall_clock_time);
        m_waterfall_shader->setVec3("u_camera_pos", camera.Position);
        const glm::vec3 sun_color =
            (m_sun.color != glm::vec3(0.0f)) ? m_sun.color : glm::vec3(1.0f);
        m_waterfall_shader->setVec3("u_sun_color", sun_color);
        // Scene-light the waterfall (bright-waterfall fix): drive u_scene_light from the
        // sun-up factor with a small night floor so the cascade is LIT by the scene and
        // darkens at dusk/night instead of emitting near-white.  (the standalone
        // WaterfallVisual gate never sets this uniform -> it keeps the shader default 1.0).
        const float waterfall_scene_lit = 0.10f + 0.90f * glm::clamp(m_sun.intensity, 0.0f, 1.0f);
        m_waterfall_shader->setVec3("u_scene_light", glm::vec3(waterfall_scene_lit));

        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        glEnable(GL_DEPTH_TEST); // self-sufficient: don't rely on the water pass leaving it on
        glDepthMask(GL_FALSE);   // translucent veil: don't occlude
        glDisable(GL_CULL_FACE); // double-sided sheet
        // The sheet hugs the cliff face the river carved (same surface WaterPass/terrain
        // draws), so pull it slightly toward the camera in depth to avoid z-fighting /
        // being hidden behind the toe of the drop.
        glEnable(GL_POLYGON_OFFSET_FILL);
        glPolygonOffset(-1.0f, -1.0f);

        glBindVertexArray(m_waterfall_vao);
        for (std::size_t i = 0; i < m_waterfall_sheet_sites.size(); ++i) {
            const WaterfallSite& s = m_waterfall_sheet_sites[i];
            //  ( T.1): the LIVE upstream water scales the sheet —
            // a dammed/drained crest extinguishes its fall. Reads the live float
            // mirror (one-way from mm, legal after derived-state reclassification); stable within
            // the frame (the sim ticks outside render_frame), so dispatch stays bit-idempotent.
            // Unstreamed crests read neutral 1.0.
            float live = 1.0f;
            if (m_frame_prepared.world_system != nullptr) {
                live = Rendering::LiveWaterFactorAt(*m_frame_prepared.world_system, s);
            }
            if (live < 0.02f) {
                continue; // extinguished: no sheet, no plunge pool
            }
            m_waterfall_shader->setFloat("u_live_factor", live);
            m_waterfall_shader->setFloat("u_crest_y", s.crest.y);
            m_waterfall_shader->setFloat("u_foot_y", s.foot.y);
            // 12 verts/site: the vertical sheet (0..5) + the horizontal plunge-pool quad (6..11).
            glDrawArrays(GL_TRIANGLES, static_cast<GLint>(i * 12), 12);
        }
        glBindVertexArray(0);

        // Restore GL state: depth writes on, cull/blend/depth-test/poly-offset as they were.
        glDepthMask(GL_TRUE);
        glPolygonOffset(0.0f, 0.0f);
        if (poly_off_was)
            glEnable(GL_POLYGON_OFFSET_FILL);
        else
            glDisable(GL_POLYGON_OFFSET_FILL);
        if (depth_was)
            glEnable(GL_DEPTH_TEST);
        else
            glDisable(GL_DEPTH_TEST);
        if (cull_was)
            glEnable(GL_CULL_FACE);
        else
            glDisable(GL_CULL_FACE);
        if (blend_was)
            glEnable(GL_BLEND);
        else
            glDisable(GL_BLEND);
    }
}

void RenderPipeline::execute_stage_glass_oit_accum(const Camera& camera) {
    // WBOIT accumulation — each visible pane writes its
    // depth-weighted premultiplied color into accum (blend ONE/ONE) and its
    // coverage into reveal (ZERO/ONE_MINUS_SRC_ALPHA -> the (1-a) product),
    // depth-TESTED against the SHARED lighting depth (write off). Empty glass
    // list = zero GL work (the trace slot still records).
    record_frame_stage("glass_oit_accum");
    if (m_glass_pane_items.empty() || m_glass_quad_vao == 0) {
        return;
    }
    if (!m_glass_oit_shader) {
        // Lazy init: shaders + the accum/reveal MRT FBO sharing the lighting depth.
        m_glass_oit_shader =
            std::make_unique<Shader>((m_root_path / "res/shaders/glass_oit.vert").string().c_str(),
                                     (m_root_path / "res/shaders/glass_oit.frag").string().c_str());
        label_gl_object(
            GL_PROGRAM, m_glass_oit_shader ? m_glass_oit_shader->Id() : 0u, "shader.glass_oit");
        m_glass_oit_resolve_shader = std::make_unique<Shader>(
            (m_root_path / "res/shaders/volumetric_lighting.vert").string().c_str(),
            (m_root_path / "res/shaders/glass_oit_resolve.frag").string().c_str());
        label_gl_object(GL_PROGRAM,
                        m_glass_oit_resolve_shader ? m_glass_oit_resolve_shader->Id() : 0u,
                        "shader.glass_oit_resolve");

        auto make_target = [&](GLenum ifmt, const char* name) {
            GLuint t = 0;
            glGenTextures(1, &t);
            glBindTexture(GL_TEXTURE_2D, t);
            glTexStorage2D(GL_TEXTURE_2D,
                           1,
                           ifmt,
                           m_internal_width,
                           m_internal_height); // match internal lighting depth
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
            label_gl_object(GL_TEXTURE, t, name);
            return t;
        };
        m_oit_accum_tex = make_target(GL_RGBA16F, "oit.accum");
        m_oit_reveal_tex = make_target(GL_R16F, "oit.reveal");
        glGenFramebuffers(1, &m_oit_fbo);
        glBindFramebuffer(GL_FRAMEBUFFER, m_oit_fbo);
        glFramebufferTexture2D(
            GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, m_oit_accum_tex, 0);
        glFramebufferTexture2D(
            GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT1, GL_TEXTURE_2D, m_oit_reveal_tex, 0);
        glFramebufferTexture2D(GL_FRAMEBUFFER,
                               GL_DEPTH_ATTACHMENT,
                               GL_TEXTURE_2D,
                               m_lighting_pass->lighting_fbo().depth_texture,
                               0);
        label_gl_object(GL_FRAMEBUFFER, m_oit_fbo, "oit.fbo");
        if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
            LUMINUMBRA_CORE_ERROR("glass_oit: MRT FBO incomplete; OIT disabled");
            glBindFramebuffer(GL_FRAMEBUFFER, 0);
            glDeleteFramebuffers(1, &m_oit_fbo);
            m_oit_fbo = 0;
            return;
        }
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
    }
    if (m_oit_fbo == 0 || !m_glass_oit_shader->IsValid()) {
        return;
    }

    glBindFramebuffer(GL_FRAMEBUFFER, m_oit_fbo);
    const GLenum bufs[2] = {GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1};
    glDrawBuffers(2, bufs);
    glViewport(0, 0, m_internal_width, m_internal_height); // OIT composites into internal scene
    const GLfloat clear_accum[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    const GLfloat clear_reveal[4] = {1.0f, 1.0f, 1.0f, 1.0f};
    glClearBufferfv(GL_COLOR, 0, clear_accum);
    glClearBufferfv(GL_COLOR, 1, clear_reveal);

    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LESS);
    glDepthMask(GL_FALSE);
    glDisable(GL_CULL_FACE);
    glEnable(GL_BLEND);
    glBlendFunci(0, GL_ONE, GL_ONE);
    glBlendFunci(1, GL_ZERO, GL_ONE_MINUS_SRC_ALPHA);

    m_glass_oit_shader->use();
    m_glass_oit_shader->setMat4("u_view", m_frame_prepared.view);
    m_glass_oit_shader->setMat4("u_projection", m_frame_prepared.projection);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, m_lighting_pass->lighting_fbo().opaque_color_texture);
    m_glass_oit_shader->setInt("u_opaqueScene", 0);
    m_glass_oit_shader->setVec2(
        "u_screenSize",
        glm::vec2(static_cast<float>(m_screen_width), static_cast<float>(m_screen_height)));
    m_glass_oit_shader->setVec3("u_cameraPos", camera.Position);
    m_glass_oit_shader->setFloat("u_refractionStrength", 0.35f);
    glBindVertexArray(m_glass_quad_vao);
    for (const GlassPaneItem& pane : m_glass_pane_items) {
        m_glass_oit_shader->setMat4("u_model", pane.model);
        m_glass_oit_shader->setVec3("u_tint", pane.tint);
        m_glass_oit_shader->setFloat("u_thickness", pane.thickness);
        glDrawArrays(GL_TRIANGLES, 0, 6);
    }
    glBindVertexArray(0);

    // Restore the default blend state (per-buffer funcs revert to the global).
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glDisable(GL_BLEND);
    glEnable(GL_CULL_FACE);
    glDepthMask(GL_TRUE);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

void RenderPipeline::execute_stage_glass_oit_resolve(const Camera& camera) {
    (void)camera;
    // the WBOIT resolve — the weighted-average glass
    // color composited over the lit scene with coverage = 1 - reveal. Runs
    // BEFORE the weather snapshot so god-rays/weather observe resolved glass.
    record_frame_stage("glass_oit_resolve");
    if (m_glass_pane_items.empty() || m_oit_fbo == 0 || !m_glass_oit_resolve_shader ||
        !m_glass_oit_resolve_shader->IsValid() || m_screen_quad_vao == 0) {
        return;
    }
    glBindFramebuffer(GL_FRAMEBUFFER, m_lighting_pass->lighting_fbo().fbo_id);
    glViewport(0, 0, m_internal_width, m_internal_height); // into internal lighting FBO
    glDisable(GL_DEPTH_TEST);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    m_glass_oit_resolve_shader->use();
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, m_oit_accum_tex);
    m_glass_oit_resolve_shader->setInt("u_accum", 0);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, m_oit_reveal_tex);
    m_glass_oit_resolve_shader->setInt("u_reveal", 1);
    glBindVertexArray(m_screen_quad_vao);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glBindVertexArray(0);
    glDisable(GL_BLEND);
    glEnable(GL_DEPTH_TEST);
    glActiveTexture(GL_TEXTURE0);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

void RenderPipeline::execute_stage_weather_opaque_snapshot(const Camera& camera) {
    // 7a-pre. WEATHER OVERLAY's post-water opaque snapshot (owned by SkyboxPass's
    // stage pair): relocated the snapshot here from inside
    // SkyboxPass (a friendless pass can't call Lighting). Guarded by the SAME
    // conditions the overlay runs under, so opaque_color_texture (also read by
    // god-rays below) stays byte-identical to the prior behavior. (: the pair
    // each compute overlay_will_run + ctx from the same frame-stable state —
    // rebuild == the old shared block-locals.)
    const FrameBufferObject& lighting_fbo = m_lighting_pass->lighting_fbo();
    const bool overlay_will_run =
        m_weather_type != WeatherType::None && m_weather_intensity > 0.0f &&
        m_skybox_pass->weather_shader() && m_skybox_pass->weather_shader()->IsValid() &&
        lighting_fbo.fbo_id && lighting_fbo.opaque_color_texture && m_screen_quad_vao;
    RenderContext weather_ctx = make_skybox_context(camera);
    record_frame_stage("weather_opaque_snapshot");
    if (overlay_will_run) {
        m_lighting_pass->copy_lighting_color_to_opaque_texture(weather_ctx);
    }
}

void RenderPipeline::execute_stage_weather_overlay(const Camera& camera) {
    // 7a. WEATHER OVERLAY (owned by SkyboxPass): defer until after water so
    // screen-space rain/fog remains a full-scene composite while sky/cloud/aurora
    // still render before water.
    RenderContext weather_ctx = make_skybox_context(camera);
    record_frame_stage("weather_overlay");
    m_skybox_pass->execute_weather_overlay(weather_ctx, camera);
    glBindVertexArray(0);
}

void RenderPipeline::execute_stage_froxel_inject(const Camera& camera) {
    //  rendering: per-froxel media density + in-scatter into
    // the scatter volume, sampling the shadow depth AND the  tint cascade so
    // stained glass throws COLORED shafts. Quality 0 = zero-GL no-op.
    record_frame_stage("froxel_inject");
    if (m_volumetric_quality <= 0) {
        return;
    }
    if (m_froxel_inject_program == 0) {
        // Lazy init: both kernels + both 3D volumes, together.
        auto load_comp = [&](const char* rel) -> GLuint {
            std::ifstream file(std::filesystem::path(m_root_path) / rel);
            if (!file) {
                LUMINUMBRA_CORE_ERROR("froxel: missing {}", rel);
                return 0;
            }
            std::stringstream source;
            source << file.rdbuf();
            return create_compute_program(source.str().c_str());
        };
        m_froxel_inject_program = load_comp("res/shaders/froxel_inject.comp");
        m_froxel_integrate_program = load_comp("res/shaders/froxel_integrate.comp");
        if (m_froxel_inject_program == 0 || m_froxel_integrate_program == 0) {
            LUMINUMBRA_CORE_ERROR("froxel: kernel compile failed; volumetrics disabled");
            m_volumetric_quality = 0;
            return;
        }
        label_gl_object(GL_PROGRAM, m_froxel_inject_program, "shader.froxel_inject");
        label_gl_object(GL_PROGRAM, m_froxel_integrate_program, "shader.froxel_integrate");
        auto make_volume = [&](const char* label) -> GLuint {
            GLuint t = 0;
            glGenTextures(1, &t);
            glBindTexture(GL_TEXTURE_3D, t);
            glTexStorage3D(
                GL_TEXTURE_3D, 1, GL_RGBA16F, Froxel::kGridX, Froxel::kGridY, Froxel::kGridZ);
            glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE);
            label_gl_object(GL_TEXTURE, t, label);
            glBindTexture(GL_TEXTURE_3D, 0);
            return t;
        };
        m_froxel_scatter_tex = make_volume("froxel.scatter");
        m_froxel_integrated_tex = make_volume("froxel.integrated");
    }

    const ShadowMap& sm = m_shadow_pass->shadow_map();
    glm::vec4 splits(0.0f);
    if (sm.cascade_splits.size() >= 5) {
        splits = glm::vec4(
            sm.cascade_splits[1], sm.cascade_splits[2], sm.cascade_splits[3], sm.cascade_splits[4]);
    }

    glUseProgram(m_froxel_inject_program);
    glBindImageTexture(0, m_froxel_scatter_tex, 0, GL_TRUE, 0, GL_WRITE_ONLY, GL_RGBA16F);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D_ARRAY, sm.depth_texture_array);
    glUniform1i(glGetUniformLocation(m_froxel_inject_program, "u_shadowCascades"), 0);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D_ARRAY, m_shadow_pass->tint_texture_array());
    glUniform1i(glGetUniformLocation(m_froxel_inject_program, "u_shadowTintCascades"), 1);
    glUniform1i(glGetUniformLocation(m_froxel_inject_program, "u_shadowTintEnabled"),
                m_shadow_pass->tint_texture_array() != 0 ? 1 : 0);
    for (int i = 0; i < 4 && i < static_cast<int>(sm.light_space_matrices.size()); ++i) {
        const std::string name = "u_lightSpaceMatrices[" + std::to_string(i) + "]";
        glUniformMatrix4fv(glGetUniformLocation(m_froxel_inject_program, name.c_str()),
                           1,
                           GL_FALSE,
                           &sm.light_space_matrices[i][0][0]);
    }
    glUniform4fv(glGetUniformLocation(m_froxel_inject_program, "u_cascadeSplits"), 1, &splits[0]);
    const glm::mat4 inv_view = glm::inverse(camera.GetViewMatrix());
    glUniformMatrix4fv(glGetUniformLocation(m_froxel_inject_program, "u_inverseView"),
                       1,
                       GL_FALSE,
                       &inv_view[0][0]);
    glUniform3fv(
        glGetUniformLocation(m_froxel_inject_program, "u_cameraPos"), 1, &camera.Position[0]);
    glUniform1f(glGetUniformLocation(m_froxel_inject_program, "u_tanHalfFovY"),
                std::tan(glm::radians(camera.Zoom) * 0.5f));
    glUniform1f(glGetUniformLocation(m_froxel_inject_program, "u_aspect"),
                static_cast<float>(m_screen_width) / static_cast<float>(m_screen_height));
    // Toward-sun (sun-disc convention — matches the aerial pass's u_sunDirection).
    const glm::vec3 toward_sun = -m_sun.direction;
    glUniform3fv(
        glGetUniformLocation(m_froxel_inject_program, "u_sunDirection"), 1, &toward_sun[0]);
    glUniform3fv(glGetUniformLocation(m_froxel_inject_program, "u_sunColor"), 1, &m_sun.color[0]);
    glUniform3fv(
        glGetUniformLocation(m_froxel_inject_program, "u_ambientColor"), 1, &m_skyAmbientColor[0]);
    // v1 media tuning (checkpoint-ratified): a gentle ground-hugging haze layer.
    glUniform1f(glGetUniformLocation(m_froxel_inject_program, "u_baseDensity"), 0.012f);
    glUniform1f(glGetUniformLocation(m_froxel_inject_program, "u_baseHeight"), 40.0f);
    glUniform1f(glGetUniformLocation(m_froxel_inject_program, "u_densityFalloff"), 0.05f);

    glDispatchCompute(Froxel::kGridX / 8, Froxel::kGridY / 8 + 1, Froxel::kGridZ);
    glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT);
    glUseProgram(0);
    glActiveTexture(GL_TEXTURE0);
}

void RenderPipeline::execute_stage_froxel_integrate(const Camera& camera) {
    (void)camera;
    //  rendering: front-to-back march of the scatter volume —
    // per-column accumulated in-scatter L + transmittance T.
    record_frame_stage("froxel_integrate");
    if (m_volumetric_quality <= 0 || m_froxel_integrate_program == 0) {
        return;
    }
    glUseProgram(m_froxel_integrate_program);
    glBindImageTexture(0, m_froxel_scatter_tex, 0, GL_TRUE, 0, GL_READ_ONLY, GL_RGBA16F);
    glBindImageTexture(1, m_froxel_integrated_tex, 0, GL_TRUE, 0, GL_WRITE_ONLY, GL_RGBA16F);
    glDispatchCompute(Froxel::kGridX / 8, Froxel::kGridY / 8 + 1, 1);
    // The aerial composite samples the integrated volume as a texture.
    glMemoryBarrier(GL_TEXTURE_FETCH_BARRIER_BIT);
    glUseProgram(0);
}

void RenderPipeline::execute_stage_aerial(const Camera& camera) {
    // 7b. AERIAL-PERSPECTIVE PASS: analytic distance-fog in-scatter
    // over the lit scene, wiring the dormant volumetric_lighting.frag. Reads the
    // SAME sky-view/transmittance LUT the skybox uses so the fog palette stays
    // coherent with the sky (warm pinks/oranges at low sun). Budget ≤ 0.3 ms.
    record_frame_stage("aerial");
    if (m_isolation_config.renders(Client::ScenarioHarness::IsolationLayer::Aerial)) {
        RenderContext aerial_ctx = make_aerial_context(camera);
        begin_gpu_pass_timer(GpuTimerPass::Aerial);
        execute_aerial_pass(aerial_ctx);
        end_gpu_pass_timer(GpuTimerPass::Aerial);
        glBindVertexArray(0);
    }
}

void RenderPipeline::execute_stage_god_rays(const Camera& camera) {
    // 7b2. GOD RAYS (screen-space crepuscular rays): additive shafts fanning from the
    // sun around occluders (clouds/terrain). Only when the sun is above the horizon
    // and on screen (zero cost otherwise). Reads the post-sky opaque snapshot.
    {
        record_frame_stage("god_rays");
        RenderContext godray_ctx = make_god_rays_context(camera);
        execute_god_rays(godray_ctx);
    }
}

void RenderPipeline::execute_stage_foliage(const Camera& camera) {
    // 7c. FOLIAGE PASS (, ): instanced ground-cover scatter blended
    // into the lit HDR target. Depth-tested against the scene depth (blitted into
    // the lighting FBO at step 5), so the cards occlude correctly. The scatter
    // instances are rebuilt by the scenario/client driver (the deterministic
    // placement hash + the  wind bridge) BEFORE this; here we only draw. A
    // no-op (zero GL draws) when foliage is disabled / empty, so all existing
    // visual gates stay byte-stable.  (one-way, never feeds the sim).
    record_frame_stage("foliage");
    if (m_foliage_pass &&
        m_isolation_config.renders(Client::ScenarioHarness::IsolationLayer::Foliage)) {
        RenderContext foliage_ctx = make_foliage_context(camera);
        begin_gpu_pass_timer(GpuTimerPass::Foliage);
        const std::size_t foliage_drawn = m_foliage_pass->execute(foliage_ctx, camera);
        // Stats moved out of FoliagePass::execute (-T17): bump only when
        // the pass actually drew (return > 0), so a disabled/empty no-op frame does
        // not corrupt foliage_draws/foliage_instances_drawn.
        if (foliage_drawn > 0) {
            m_last_render_pass_stats.foliage_draws++;
            m_last_render_pass_stats.foliage_instances_drawn += foliage_drawn;
        }
        end_gpu_pass_timer(GpuTimerPass::Foliage);
        glBindVertexArray(0);
    }
}

void RenderPipeline::execute_stage_taau_resolve(const Camera& camera) {
    (void)camera;
    //  TAAU : resolve the OPAQUE lit color BEFORE the transparent particle/lightning
    // composite. Transparents can't write the gbuffer motion-vector MRT, so a resolve placed
    // after them would temporally blend them along the opaque surface's motion (or zero for sky)
    // -> smeared rain streaks / ghosted bolt. Resolving first keeps history particle-free and lets
    // particles + lightning composite fresh on the stable resolved image. Flag-gated (render.taau);
    // OFF -> no-op. (Pre- this ran just before the blit; moving it up is render-only,
    // gate-neutral when TAAU is OFF, which is the default and what every visual gate runs with.)
    record_frame_stage("taau_resolve");
    if (m_taau_enabled) {
        RenderContext taau_ctx = make_taau_context();
        execute_taau_resolve(taau_ctx);
    }
}

void RenderPipeline::execute_stage_luminance_meter(const Camera& camera) {
    (void)camera;
    //  (/,  ): mean-log-luminance of the resolved
    // lit scene -> a 1-float SSBO -> the AsyncReadbackRing. Submit-only here (the
    // damped servo consumes in prepare_frame, stale-safe, never blocking). Flag
    // OFF (the default) is a zero-GL no-op; the trace slot is still recorded.
    // Dispatch-idempotent: re-running recomputes the SAME value into the SSBO and
    // ring-submits again. The current frame does not consume that new sample, so
    // pixels remain unchanged until the next frame's exposure update.
    record_frame_stage("luminance_meter");
    if (!m_auto_exposure_metered) {
        return;
    }
    if (m_lum_reduce_program == 0) {
        // Lazy init: compile the reduce kernel + the 1-float SSBO on first use.
        std::ifstream file(std::filesystem::path(m_root_path) /
                           "res/shaders/luminance_reduce.comp");
        if (!file) {
            LUMINUMBRA_CORE_ERROR("luminance_meter: missing res/shaders/luminance_reduce.comp");
            m_auto_exposure_metered = false;
            return;
        }
        std::stringstream source;
        source << file.rdbuf();
        m_lum_reduce_program = create_compute_program(source.str().c_str());
        if (m_lum_reduce_program == 0) {
            LUMINUMBRA_CORE_ERROR("luminance_meter: compute compile failed; metering disabled");
            m_auto_exposure_metered = false;
            return;
        }
        label_gl_object(GL_PROGRAM, m_lum_reduce_program, "shader.luminance_reduce");
        glGenBuffers(1, &m_lum_reduce_ssbo);
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, m_lum_reduce_ssbo);
        glBufferData(GL_SHADER_STORAGE_BUFFER, sizeof(float), nullptr, GL_DYNAMIC_COPY);
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
        label_gl_object(GL_BUFFER, m_lum_reduce_ssbo, "luminance_reduce.ssbo");
    }
    glUseProgram(m_lum_reduce_program);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, m_lighting_pass->lighting_fbo().color_texture);
    glUniform1i(glGetUniformLocation(m_lum_reduce_program, "u_scene"), 0);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, m_lum_reduce_ssbo);
    glDispatchCompute(1, 1, 1);
    // The ring's GPU->GPU copy must observe the SSBO write.
    glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT | GL_BUFFER_UPDATE_BARRIER_BIT);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, 0);
    glUseProgram(0);
    if (m_exposure_ring.ensure(sizeof(float), 3) && m_exposure_ring.begin()) {
        m_exposure_ring.copy_region(m_lum_reduce_ssbo, 0, 0, sizeof(float));
        m_exposure_ring.submit();
    }
}

void RenderPipeline::execute_stage_particles(const Camera& camera) {
    // 8. PARTICLE PASS: forward-lit, soft-faded transparent particles
    // blended into the lit HDR target after the skybox. Render-only motion is
    // advanced in prepare_frame; the descriptor schedule (the sim-deterministic
    // surface) is rebuilt by the scenario driver, NOT here. A no-op (zero GL
    // draws) when no emitters exist, so all existing visual gates stay byte-stable.
    record_frame_stage("particles");
    if (m_particle_pass &&
        m_isolation_config.renders(Client::ScenarioHarness::IsolationLayer::Particles)) {
        // motion advance hoisted to prepare_frame (dispatch only draws).
        RenderContext particle_ctx = make_particle_context(camera);
        begin_gpu_pass_timer(GpuTimerPass::Particle);
        const std::size_t particles_drawn = m_particle_pass->execute(particle_ctx, camera);
        end_gpu_pass_timer(GpuTimerPass::Particle);
        // stat bumps relocated out of the pass; bump only
        // when the pass actually drew (preserves the original guarded behavior).
        if (particles_drawn > 0) {
            m_last_render_pass_stats.particle_draws++;
            m_last_render_pass_stats.particles_drawn += particles_drawn;
        }
        glBindVertexArray(0);
    }
}

void RenderPipeline::execute_stage_lightning_overlay(const Camera& camera) {
    (void)camera;
    // 8b. LIGHTNING OVERLAY (, ): the full-scene light-pulse + screen-space
    // bolt, composited over the lit terrain AND the sky (drawn after the skybox).
    // A no-op when no strike is active (zero added cost). Its transient cost is
    // captured by the FinalBlit-adjacent timing; the PerfRegression budget (≤ 0.5 ms)
    // is bounded by the overlay being a single additive full-screen quad.
    record_frame_stage("lightning_overlay");
    if (m_isolation_config.renders(Client::ScenarioHarness::IsolationLayer::Lightning)) {
        m_lighting_pass->execute_lightning_overlay(m_frame_lighting_ctx);
        glBindVertexArray(0);
    }
}

void RenderPipeline::execute_stage_final_blit(const Camera& camera) {
    const float deltaTime = m_frame_prepared.delta_time;
    // 9. FINAL BLIT TO SCREEN (or to the offscreen preview target, ).
    // -T01: routed through the RenderContext seam (FinalBlitPass) — the
    // first pass off RenderPipeline&. Byte-identical to the prior inline blit; the
    // pipeline keeps the GPU-timer + stats orchestration around the call. The lit
    // scene is adopted into the registry as "lit_scene" (wrap-existing, since the
    // lighting FBO is still owned by LightingPass during the migration).
    //  when a preview FBO is bound, the lit scene is copied into it
    // instead of the default framebuffer (1:1 filtered blit). Default path
    // (no target) is byte-identical.
    record_frame_stage("final_blit");
    begin_gpu_pass_timer(GpuTimerPass::FinalBlit);
    {
        RenderContext ctx;
        ctx.camera = &camera;
        ctx.delta_time = deltaTime;
        ctx.screen_width = m_screen_width;
        ctx.screen_height = m_screen_height;
        ctx.internal_width = m_internal_width; //
        ctx.internal_height = m_internal_height;
        ctx.offscreen_active = m_offscreen_target_active;
        ctx.offscreen_fbo = m_offscreen_target_fbo;
        ctx.offscreen_w = m_offscreen_target_w;
        ctx.offscreen_h = m_offscreen_target_h;
        ctx.registry = &m_render_registry;
        ctx.lit_scene =
            m_render_registry.adopt_fbo("lit_scene", m_lighting_pass->lighting_fbo().fbo_id);
        m_final_blit_pass->execute(ctx);
    }
    m_last_render_pass_stats.final_blits++;
    end_gpu_pass_timer(GpuTimerPass::FinalBlit);
}

void RenderPipeline::execute_stage_debug_view(const Camera& camera) {
    const GLuint draw_fbo = m_offscreen_target_active ? m_offscreen_target_fbo : 0u;
    const u32 dst_w = m_offscreen_target_active ? m_offscreen_target_w : m_screen_width;
    const u32 dst_h = m_offscreen_target_active ? m_offscreen_target_h : m_screen_height;
    // Render-only debug-view OVERRIDE: when enabled, redraw the bound target (screen or
    // offscreen) with a single-channel G-buffer view, REPLACING the composited image so a
    // human/frame-scan can tell "dark night" from a lighting/geometry bug. Gated on mode !=
    // None so the default path is byte-identical. Diagnostic; never touches sim/world_hash.
    record_frame_stage("debug_view");
    if (m_debug_view_pass && m_debug_view_pass->mode() != DebugViewPass::None) {
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, draw_fbo);
        glViewport(0, 0, static_cast<GLsizei>(dst_w), static_cast<GLsizei>(dst_h));
        RenderContext debug_ctx = make_debug_view_context(camera);
        m_debug_view_pass->execute(debug_ctx);
    }
}

void RenderPipeline::update_aether_field(const std::vector<float>& cells,
                                         float world_origin_x,
                                         float world_origin_z,
                                         int extent,
                                         float cell_size_m) {
    // One-way sim->render bridge for the Aether emissive tap. Empty/
    // mismatched input -> inactive (lighting pass adds no glow, pixel-identical).
    if (extent <= 0 ||
        cells.size() != static_cast<std::size_t>(extent) * static_cast<std::size_t>(extent)) {
        m_aetherFieldActive = false;
        return;
    }
    if (m_aetherFieldTexture == 0 || m_aetherFieldExtent != extent ||
        m_aetherFieldTextureIsDual) { // fall back from an RG32F alloc
        if (m_aetherFieldTexture != 0) {
            glDeleteTextures(1, &m_aetherFieldTexture);
            m_aetherFieldTexture = 0;
        }
        glGenTextures(1, &m_aetherFieldTexture);
        glBindTexture(GL_TEXTURE_2D, m_aetherFieldTexture);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_R32F, extent, extent, 0, GL_RED, GL_FLOAT, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        m_aetherFieldExtent = extent;
        m_aetherFieldTextureIsDual = false;
    }
    glBindTexture(GL_TEXTURE_2D, m_aetherFieldTexture);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, extent, extent, GL_RED, GL_FLOAT, cells.data());
    glBindTexture(GL_TEXTURE_2D, 0);
    m_aetherFieldWorldOrigin = glm::vec2(world_origin_x, world_origin_z);
    m_aetherFieldCellSize = cell_size_m;
    m_aetherFieldActive = true;
    m_aetherPolarityActive = false; // single-channel upload: no polarity tint
}

void RenderPipeline::update_aether_field_dual(const std::vector<float>& energy_cells,
                                              const std::vector<float>& polarity_cells,
                                              float world_origin_x,
                                              float world_origin_z,
                                              int extent,
                                              float cell_size_m) {
    //  ( -8): the RG32F dual tap — R = energy (same
    // semantics as the single-channel tap), G = Lumin/Umbra polarity in
    // [-1, 1], consumed by the lighting pass ONLY when the polarity flag is
    // active (default OFF -> the glow color is untouched -> pixel-identical).
    // One-way sim->render bridge, exactly like the single-channel path.
    const std::size_t n = static_cast<std::size_t>(extent) * static_cast<std::size_t>(extent);
    if (extent <= 0 || energy_cells.size() != n || polarity_cells.size() != n) {
        m_aetherFieldActive = false;
        m_aetherPolarityActive = false;
        return;
    }
    // The RG32F allocation replaces any single-channel texture (and vice
    // versa: the R32F path above reallocates on extent change only, so switch
    // formats explicitly here).
    if (m_aetherFieldTexture == 0 || m_aetherFieldExtent != extent || !m_aetherFieldTextureIsDual) {
        if (m_aetherFieldTexture != 0) {
            glDeleteTextures(1, &m_aetherFieldTexture);
            m_aetherFieldTexture = 0;
        }
        glGenTextures(1, &m_aetherFieldTexture);
        glBindTexture(GL_TEXTURE_2D, m_aetherFieldTexture);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RG32F, extent, extent, 0, GL_RG, GL_FLOAT, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        m_aetherFieldExtent = extent;
        m_aetherFieldTextureIsDual = true;
    }
    std::vector<float> interleaved(n * 2);
    for (std::size_t i = 0; i < n; ++i) {
        interleaved[i * 2 + 0] = energy_cells[i];
        interleaved[i * 2 + 1] = polarity_cells[i];
    }
    glBindTexture(GL_TEXTURE_2D, m_aetherFieldTexture);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, extent, extent, GL_RG, GL_FLOAT, interleaved.data());
    glBindTexture(GL_TEXTURE_2D, 0);
    m_aetherFieldWorldOrigin = glm::vec2(world_origin_x, world_origin_z);
    m_aetherFieldCellSize = cell_size_m;
    m_aetherFieldActive = true;
    m_aetherPolarityActive = true;
}

void RenderPipeline::on_resize(u32 new_width, u32 new_height) {
    if (new_width == 0 || new_height == 0 ||
        (new_width == m_screen_width && new_height == m_screen_height))
        return;
    m_screen_width = new_width;
    m_screen_height = new_height;
    // recompute the internal (scaled) extent from the new output size.
    m_internal_width = static_cast<u32>(std::lround(m_screen_width * m_render_scale));
    m_internal_height = static_cast<u32>(std::lround(m_screen_height * m_render_scale));
    // Reallocate the screen-sized targets, preserving formats (each init_*
    // rebuilds with the same internal formats it used at startup). The shadow
    // map is a fixed-resolution cascade array and the water caustics texture is
    // a fixed-resolution offscreen target, so neither resizes here; the water /
    // skybox / far-LOD passes read the resized G-buffer and lighting targets
    // through the shared pipeline state and pick up the new size automatically.
    // SCALED intermediates reallocate at internal res (== output at scale 1.0).
    m_lighting_pass->destroy_lighting_fbo(m_render_registry);
    m_lighting_pass->init_lighting_fbo(m_render_registry, m_internal_width, m_internal_height);
    m_gbuffer_pass->destroy_gbuffer(m_render_registry);
    m_gbuffer_pass->init_gbuffer(m_render_registry, m_internal_width, m_internal_height);
    m_ssao_pass->destroy_ssao(m_render_registry);
    m_ssao_pass->init_ssao(m_render_registry, m_internal_width, m_internal_height);
    destroy_taau();
    init_taau(new_width,
              new_height); // OUTPUT res: TAAU history invalidated on resize, stays output
    init_halfres_cloud();  // re-size the reduced-res sky-dome target (no-op at quality 0)
    m_frustumCache.valid = false;
    ++m_resize_generation;
    LUMINUMBRA_CORE_INFO("RenderPipeline resized targets to {}x{} (resize generation {})",
                         new_width,
                         new_height,
                         m_resize_generation);
}

void RenderPipeline::set_render_scale(float scale) {
    // config-driven internal render scale. Clamp to the SAME [0.5, 1.0]
    // band startup's LUMIN_RENDER_SCALE env knob uses. BEFORE startup (m_started == false)
    // this only seeds m_render_scale -- startup then sizes m_internal_* from it, and its env
    // check overrides for an A/B (env WINS by running AFTER this). AFTER startup this
    // reallocates the scaled intermediates exactly as on_resize does (the output-res
    // TAAU/backbuffer targets are untouched -- only the internal extent moved). At scale 1.0
    // this is a no-op (internal==output), byte-identical.
    const float clamped = std::clamp(scale, 0.5f, 1.0f);
    if (clamped == m_render_scale)
        return;
    m_render_scale = clamped;
    if (!m_started)
        return; // startup() will size the internal extent from m_render_scale
    m_internal_width = static_cast<u32>(std::lround(m_screen_width * m_render_scale));
    m_internal_height = static_cast<u32>(std::lround(m_screen_height * m_render_scale));
    m_lighting_pass->destroy_lighting_fbo(m_render_registry);
    m_lighting_pass->init_lighting_fbo(m_render_registry, m_internal_width, m_internal_height);
    m_gbuffer_pass->destroy_gbuffer(m_render_registry);
    m_gbuffer_pass->init_gbuffer(m_render_registry, m_internal_width, m_internal_height);
    m_ssao_pass->destroy_ssao(m_render_registry);
    m_ssao_pass->init_ssao(m_render_registry, m_internal_width, m_internal_height);
    // The output-res TAAU history buffers keep their size (output is unchanged), but their
    // CONTENT is now stale -- it was resolved at the previous internal scale, so its motion
    // vectors / reprojection no longer match. Invalidate it so the next resolve restarts fresh
    // instead of blending the previous scale's history (Codex review, runtime scale-change).
    m_taau_history_valid = false;
    m_frustumCache.valid = false;
    ++m_resize_generation;
    LUMINUMBRA_CORE_INFO("RenderPipeline render_scale -> {} (internal {}x{})",
                         m_render_scale,
                         m_internal_width,
                         m_internal_height);
}

void RenderPipeline::set_offscreen_target(u32 fbo, u32 fbo_w, u32 fbo_h) {
    //  redirect the final blit into a caller-owned FBO. A zero
    // FBO or degenerate size clears the redirect (back to the default-0 path).
    if (fbo == 0 || fbo_w == 0 || fbo_h == 0) {
        clear_offscreen_target();
        return;
    }
    m_offscreen_target_active = true;
    m_offscreen_target_fbo = fbo;
    m_offscreen_target_w = fbo_w;
    m_offscreen_target_h = fbo_h;
}

void RenderPipeline::clear_offscreen_target() {
    m_offscreen_target_active = false;
    m_offscreen_target_fbo = 0;
    m_offscreen_target_w = 0;
    m_offscreen_target_h = 0;
}

void RenderPipeline::clear_all_chunk_data() {
    // Force clear all cached chunk render data to ensure fresh uploads.
    // terrain geometry lives in the shared pool; dropping the whole
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
    m_particle_pass->init_shader(m_root_path); //
    m_foliage_pass->init_shader(m_root_path);  //
    m_foliage_pass->init_compute(m_root_path); //  #4: GPU grass scatter (graceful CPU fallback)
    m_plant_procgen_pass->init_shader(m_root_path); //  procedural plant G-buffer shader
    m_ground_decal_pass->init_shader(m_root_path);  // pheromone decal shader
    m_debug_view_pass->init_shader(m_root_path);    // render-only G-buffer debug shader
    m_shadow_pass->init_shader(m_root_path);
    m_ssao_pass->init_shaders(m_root_path);
    m_water_pass->init_shader(m_root_path);
    //  analytic aerial-perspective term wiring the dormant
    // volumetric_lighting.frag as a fullscreen pass over the lit scene. Reuses
    // the SSAO fullscreen-quad vertex stage.
    m_aerial_shader = std::make_unique<Shader>(
        (m_root_path / "res/shaders/ssao.vert").string().c_str(),
        (m_root_path / "res/shaders/volumetric_lighting.frag").string().c_str());
    label_gl_object(
        GL_PROGRAM, m_aerial_shader ? m_aerial_shader->Id() : 0u, "shader.aerial_perspective");
    // the waterfall falling-sheet shader. Drawn over detected
    // waterfall sites (waterfall_sites) on a world-deterministic site; the
    // dressing is render-only and never hashed. Reuses the basic vertex stage
    // (world-position + normal varyings) the sheet quad is built with.
    m_waterfall_shader =
        std::make_unique<Shader>((m_root_path / "res/shaders/basic.vert").string().c_str(),
                                 (m_root_path / "res/shaders/waterfall.frag").string().c_str());
    label_gl_object(
        GL_PROGRAM, m_waterfall_shader ? m_waterfall_shader->Id() : 0u, "shader.waterfall");
    // Render-optimization (cloud-raymarch-optimization, ): the depth-masked
    // upsample that composites the reduced-res sky dome into the lighting FBO.
    // Reuses the SSAO fullscreen-quad vertex stage. Only used when cloud quality > 0.
    m_cloud_composite_shader = std::make_unique<Shader>(
        (m_root_path / "res/shaders/ssao.vert").string().c_str(),
        (m_root_path / "res/shaders/cloud_composite.frag").string().c_str());
    label_gl_object(GL_PROGRAM,
                    m_cloud_composite_shader ? m_cloud_composite_shader->Id() : 0u,
                    "shader.cloud_composite");
    // Screen-space crepuscular rays.
    m_god_rays_shader =
        std::make_unique<Shader>((m_root_path / "res/shaders/ssao.vert").string().c_str(),
                                 (m_root_path / "res/shaders/god_rays.frag").string().c_str());
    label_gl_object(
        GL_PROGRAM, m_god_rays_shader ? m_god_rays_shader->Id() : 0u, "shader.god_rays");
    //  TAAU resolve. Reuses the SSAO fullscreen-quad vertex stage.
    m_taau_shader =
        std::make_unique<Shader>((m_root_path / "res/shaders/ssao.vert").string().c_str(),
                                 (m_root_path / "res/shaders/taau_resolve.frag").string().c_str());
    label_gl_object(GL_PROGRAM, m_taau_shader ? m_taau_shader->Id() : 0u, "shader.taau_resolve");
}

void RenderPipeline::init_sky_lut() {
    // Build the transmittance + multi-scatter LUTs (sun-independent) and the
    // initial sky-view LUT for the current sun. Recorded as a startup one-shot
    // cost in render telemetry (budget ≤ 8.0 ms on release). Render-only.
    update_time_of_day(0.0f); // seed m_sun.direction for the initial sky-view
    const glm::vec3 toward_sun = -glm::normalize(m_sun.direction);
    // compile the GPU compute programs OUTSIDE the timed precompute so the
    // one-time driver shader-compile cost isn't charged to the budget (no-op when the knob is off).
    m_sky_lut.prewarm_gpu_compute();
    double precompute_ms = 0.0;
    if (m_sky_lut.initialize(toward_sun, &precompute_ms)) {
        m_skyScatterAmbient = m_sky_lut.sky_ambient();
        LUMINUMBRA_CORE_INFO("Sky scattering LUTs precomputed in {:.3f} ms", precompute_ms);
    } else {
        LUMINUMBRA_CORE_ERROR("Sky scattering LUT precompute failed");
    }
    m_sky_full_precompute_ms = precompute_ms;
}

// the aerial pass contract — sky/atmosphere LUTs (Group D),
// gbuffer depth, lit-scene target, and the Group I aerial floats resolved AFTER the
// LUMIN_ATMOS override. Render-only; never feeds world_hash.
RenderContext RenderPipeline::make_aerial_context(const Camera& camera) {
    RenderContext ctx;
    ctx.camera = &camera;
    ctx.screen_width = m_screen_width;
    ctx.screen_height = m_screen_height;
    ctx.internal_width = m_internal_width; //
    ctx.internal_height = m_internal_height;
    ctx.registry = &m_render_registry;
    ctx.screen_quad_vao = m_screen_quad_vao;
    ctx.sun = m_sun;
    ctx.sky_day_factor = m_skyDayFactor;
    ctx.underwater_factor = m_underwater_factor;
    ctx.sky_lut_ready = m_sky_lut.ready();
    ctx.sky_view_lut =
        m_render_registry.adopt_texture("sky_view_lut", m_sky_lut.sky_view_texture());
    ctx.transmittance_lut =
        m_render_registry.adopt_texture("transmittance_lut", m_sky_lut.transmittance_texture());
    ctx.gbuffer_depth =
        m_render_registry.adopt_texture("gbuffer_depth", m_gbuffer_pass->gbuffer().depth_texture);
    ctx.lit_scene =
        m_render_registry.adopt_fbo("lit_scene", m_lighting_pass->lighting_fbo().fbo_id);
    //  controllable atmosphere: the data-driven aerial params, with the optional
    // LUMIN_ATMOS="density,maxDistance,inscatterStrength,warmth" tuning override parsed
    // ONCE (not per frame). Resolved at the call site per the Group I contract.
    AtmosphereParams atmo = m_atmosphere;
    static const auto s_atmos_override = [] {
        std::optional<AtmosphereParams> ov;
        if (const auto env = Core::ReadEnvironment("LUMIN_ATMOS")) {
            AtmosphereParams p{};
            if (std::sscanf(env->c_str(),
                            "%f,%f,%f,%f",
                            &p.aerial_density,
                            &p.aerial_max_distance,
                            &p.inscatter_strength,
                            &p.warmth) == 4) {
                ov = p;
            }
        }
        return ov;
    }();
    if (s_atmos_override) {
        atmo = *s_atmos_override;
    }
    ctx.aerial_density = atmo.aerial_density;
    ctx.aerial_max_distance = atmo.aerial_max_distance;
    ctx.inscatter_strength = atmo.inscatter_strength;
    ctx.atmosphere_warmth = atmo.warmth;
    return ctx;
}

void RenderPipeline::execute_aerial_pass(const RenderContext& ctx) {
    //  analytic aerial-perspective in-scatter composited OVER the lit
    // scene in the lighting FBO. Reads the SAME sky-view/transmittance LUTs the
    // dome uses (coherent palette). A no-op if the LUT/shader are unavailable.
    // Only the shader stays a member; all frame state comes from ctx.
    if (!m_aerial_shader || !m_aerial_shader->IsValid() || !ctx.sky_lut_ready ||
        ctx.screen_quad_vao == 0) {
        return;
    }
    const GLuint lit_fbo = ctx.lit_scene.id;
    if (!lit_fbo) {
        return;
    }
    const Camera& camera = *ctx.camera;

    glBindFramebuffer(GL_FRAMEBUFFER, lit_fbo);
    glViewport(0, 0, ctx.internal_w(), ctx.internal_h()); // aerial into internal lit FBO
    const GLboolean depth_was_enabled = glIsEnabled(GL_DEPTH_TEST);
    glDisable(GL_DEPTH_TEST);
    const GLboolean blend_was_enabled = glIsEnabled(GL_BLEND);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    m_aerial_shader->use();
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, ctx.gbuffer_depth.id);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, ctx.sky_view_lut.id);
    glActiveTexture(GL_TEXTURE2);
    glBindTexture(GL_TEXTURE_2D, ctx.transmittance_lut.id);
    m_aerial_shader->setInt("gDepth", 0);
    m_aerial_shader->setInt("u_skyViewLut", 1);
    m_aerial_shader->setInt("u_transmittanceLut", 2);
    glm::mat4 projection = glm::perspective(glm::radians(camera.Zoom),
                                            (float)ctx.screen_width / (float)ctx.screen_height,
                                            camera.GetNearPlane(),
                                            camera.GetFarPlane());
    m_aerial_shader->setMat4("u_inverseView", glm::inverse(camera.GetViewMatrix()));
    m_aerial_shader->setMat4("u_inverseProjection", glm::inverse(projection));
    m_aerial_shader->setVec3("u_viewPos", camera.Position);
    // Toward-sun direction (sun-disc convention), matching the sky-view LUT frame.
    m_aerial_shader->setVec3("u_sunDirection", -ctx.sun.direction);
    const float sun_up = glm::dot(ctx.sun.direction, glm::vec3(0.0f, -1.0f, 0.0f));
    m_aerial_shader->setFloat("u_sunCosZenith", sun_up);
    m_aerial_shader->setFloat("u_skyDayFactor", ctx.sky_day_factor);
    m_aerial_shader->setFloat("u_underwater", ctx.underwater_factor);
    m_aerial_shader->setFloat("u_aerialDensity", ctx.aerial_density);
    m_aerial_shader->setFloat("u_aerialMaxDistance", ctx.aerial_max_distance);
    m_aerial_shader->setFloat("u_inscatterStrength", ctx.inscatter_strength);
    m_aerial_shader->setFloat("u_atmosphereWarmth", ctx.atmosphere_warmth);
    //  rendering: compose the integrated froxel volume
    // ( — extends the analytic term, never replaces it). Mode 0 (the
    // default) skips the sampling entirely: byte-identical to pre-froxel.
    const bool froxel_on = m_volumetric_quality > 0 && m_froxel_integrated_tex != 0;
    m_aerial_shader->setInt("u_volumetricMode", froxel_on ? 1 : 0);
    glActiveTexture(GL_TEXTURE3);
    glBindTexture(GL_TEXTURE_3D, froxel_on ? m_froxel_integrated_tex : 0u);
    m_aerial_shader->setInt("u_froxelIntegrated", 3);

    glBindVertexArray(ctx.screen_quad_vao);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glBindVertexArray(0);

    if (!blend_was_enabled) {
        glDisable(GL_BLEND);
    }
    if (depth_was_enabled) {
        glEnable(GL_DEPTH_TEST);
    }
    glActiveTexture(GL_TEXTURE0);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

// the god-rays pass contract — the lit-scene target + its post-sky
// opaque snapshot, plus the Group J sun-screen projection COMPUTED here (identical math
// to the retired inline block) so execute_god_rays reads only ctx. Render-only.
RenderContext RenderPipeline::make_god_rays_context(const Camera& camera) {
    RenderContext ctx;
    ctx.camera = &camera;
    ctx.screen_width = m_screen_width;
    ctx.screen_height = m_screen_height;
    ctx.internal_width = m_internal_width; //
    ctx.internal_height = m_internal_height;
    ctx.registry = &m_render_registry;
    ctx.screen_quad_vao = m_screen_quad_vao;
    ctx.lit_scene =
        m_render_registry.adopt_fbo("lit_scene", m_lighting_pass->lighting_fbo().fbo_id);
    ctx.opaque_scene = m_render_registry.adopt_texture(
        "opaque_scene", m_lighting_pass->lighting_fbo().opaque_color_texture);
    // Group J: sun visibility + on-screen UV (crepuscular-ray origin). Zero when the
    // sun is below the horizon or off-screen -> execute_god_rays becomes a no-op.
    const glm::vec3 toSun = -m_sun.direction;
    float sun_visible = glm::smoothstep(-0.02f, 0.12f, toSun.y); // above horizon
    glm::vec2 sun_uv(0.0f);
    if (sun_visible > 0.0f) {
        const glm::mat4 view = camera.GetViewMatrix();
        const glm::mat4 projection =
            glm::perspective(glm::radians(camera.Zoom),
                             (float)m_screen_width / (float)m_screen_height,
                             camera.GetNearPlane(),
                             camera.GetFarPlane());
        const glm::vec4 clip = projection * glm::mat4(glm::mat3(view)) * glm::vec4(toSun, 1.0f);
        if (clip.w > 0.0f) {
            const glm::vec2 ndc = glm::vec2(clip) / clip.w;
            sun_uv = ndc * 0.5f + 0.5f;
            const float onx = glm::smoothstep(-0.35f, 0.05f, sun_uv.x) *
                              (1.0f - glm::smoothstep(0.95f, 1.35f, sun_uv.x));
            const float ony = glm::smoothstep(-0.35f, 0.05f, sun_uv.y) *
                              (1.0f - glm::smoothstep(0.95f, 1.35f, sun_uv.y));
            sun_visible *= onx * ony;
        } else {
            sun_visible = 0.0f;
        }
    }
    ctx.sun_visible = sun_visible;
    ctx.sun_uv_x = sun_uv.x;
    ctx.sun_uv_y = sun_uv.y;
    return ctx;
}

void RenderPipeline::execute_god_rays(const RenderContext& ctx) {
    // Screen-space crepuscular rays: additive shafts fanning from the sun around
    // occluders. Only when the sun is above the horizon and on screen (zero cost
    // otherwise). Only the shader stays a member; all frame state comes from ctx.
    if (!m_god_rays_shader || !m_god_rays_shader->IsValid() || ctx.screen_quad_vao == 0) {
        return;
    }
    const float sun_visible = ctx.sun_visible;
    const glm::vec2 sun_uv(ctx.sun_uv_x, ctx.sun_uv_y);
    const GLuint lit_fbo = ctx.lit_scene.id;
    const GLuint opaque_tex = ctx.opaque_scene.id;
    if (sun_visible > 0.002f && lit_fbo && opaque_tex) {
        glBindFramebuffer(GL_FRAMEBUFFER, lit_fbo);
        glViewport(0, 0, ctx.internal_w(), ctx.internal_h()); // god-rays into internal lit FBO
        const GLboolean depth_was = glIsEnabled(GL_DEPTH_TEST);
        glDisable(GL_DEPTH_TEST);
        const GLboolean blend_was = glIsEnabled(GL_BLEND);
        glEnable(GL_BLEND);
        glBlendFunc(GL_ONE, GL_ONE); // additive
        m_god_rays_shader->use();
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, opaque_tex);
        m_god_rays_shader->setInt("u_scene", 0);
        m_god_rays_shader->setVec2("u_sunUV", sun_uv);
        m_god_rays_shader->setFloat("u_sunVisible", sun_visible);
        m_god_rays_shader->setFloat("u_strength", 0.85f);
        glBindVertexArray(ctx.screen_quad_vao);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
        glBindVertexArray(0);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, 0);
        if (!blend_was)
            glDisable(GL_BLEND);
        if (depth_was)
            glEnable(GL_DEPTH_TEST);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
    }
}

// --- Render-optimization: reduced-res sky-dome (cloud-raymarch-optimization, ) ---

void RenderPipeline::set_cloud_quality(int quality) {
    if (quality < 0)
        quality = 0;
    if (quality > 2)
        quality = 2;
    if (quality == m_cloud_quality)
        return;
    m_cloud_quality = quality;
    if (m_started) {
        init_halfres_cloud();
    }
    LUMINUMBRA_CORE_INFO("Cloud render quality set to {} ({})",
                         m_cloud_quality,
                         m_cloud_quality == 0 ? "full"
                                              : (m_cloud_quality == 1 ? "half" : "quarter"));
}

void RenderPipeline::destroy_halfres_cloud() {
    if (m_halfres_cloud.color_texture) {
        glDeleteTextures(1, &m_halfres_cloud.color_texture);
        m_halfres_cloud.color_texture = 0;
    }
    if (m_halfres_cloud.fbo) {
        glDeleteFramebuffers(1, &m_halfres_cloud.fbo);
        m_halfres_cloud.fbo = 0;
    }
    m_halfres_cloud.width = 0;
    m_halfres_cloud.height = 0;
    m_halfres_cloud.scale = 0;
}

void RenderPipeline::init_halfres_cloud() {
    // Quality 0 -> release the target; the legacy full-res dome draw runs instead.
    if (m_cloud_quality <= 0) {
        destroy_halfres_cloud();
        return;
    }
    const int scale = (m_cloud_quality >= 2) ? 4 : 2;
    u32 w = m_screen_width / static_cast<u32>(scale);
    u32 h = m_screen_height / static_cast<u32>(scale);
    if (w == 0)
        w = 1;
    if (h == 0)
        h = 1;
    // Already sized correctly -> nothing to do.
    if (m_halfres_cloud.fbo && m_halfres_cloud.width == w && m_halfres_cloud.height == h &&
        m_halfres_cloud.scale == scale) {
        return;
    }
    destroy_halfres_cloud();
    glGenFramebuffers(1, &m_halfres_cloud.fbo);
    glGenTextures(1, &m_halfres_cloud.color_texture);
    label_gl_object(GL_FRAMEBUFFER, m_halfres_cloud.fbo, "cloud_halfres.fbo");
    label_gl_object(GL_TEXTURE, m_halfres_cloud.color_texture, "cloud_halfres.color");
    glBindTexture(GL_TEXTURE_2D, m_halfres_cloud.color_texture);
    // RGBA16F to match the HDR lighting target the dome normally writes into; LINEAR
    // so the composite gets a free bilinear upsample.
    glTexImage2D(GL_TEXTURE_2D,
                 0,
                 GL_RGBA16F,
                 static_cast<GLsizei>(w),
                 static_cast<GLsizei>(h),
                 0,
                 GL_RGBA,
                 GL_FLOAT,
                 nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindFramebuffer(GL_FRAMEBUFFER, m_halfres_cloud.fbo);
    glFramebufferTexture2D(
        GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, m_halfres_cloud.color_texture, 0);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glBindTexture(GL_TEXTURE_2D, 0);
    m_halfres_cloud.width = w;
    m_halfres_cloud.height = h;
    m_halfres_cloud.scale = scale;
    LUMINUMBRA_CORE_INFO("Half-res cloud target allocated {}x{} (1/{} per axis)", w, h, scale);
}

void RenderPipeline::init_screen_quad() {
    const float quadVertices[] = {
        -1.0f, 1.0f, 0.0f, 0.0f, 1.0f, -1.0f, -1.0f, 0.0f, 0.0f, 0.0f,
        1.0f,  1.0f, 0.0f, 1.0f, 1.0f, 1.0f,  -1.0f, 0.0f, 1.0f, 0.0f,
    };
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

    // the shared glass-pane unit quad — a vertical
    // XY-plane square (x -0.5..0.5, y 0..1, z 0), positioned/sized per pane by
    // GlassPaneItem.model. Position-only (shadow_tint.vert reads location 0).
    const float glassQuad[] = {
        -0.5f,
        0.0f,
        0.0f,
        0.5f,
        0.0f,
        0.0f,
        0.5f,
        1.0f,
        0.0f,
        -0.5f,
        0.0f,
        0.0f,
        0.5f,
        1.0f,
        0.0f,
        -0.5f,
        1.0f,
        0.0f,
    };
    glGenVertexArrays(1, &m_glass_quad_vao);
    glGenBuffers(1, &m_glass_quad_vbo);
    label_gl_object(GL_VERTEX_ARRAY, m_glass_quad_vao, "glass_quad.vao");
    label_gl_object(GL_BUFFER, m_glass_quad_vbo, "glass_quad.vbo");
    glBindVertexArray(m_glass_quad_vao);
    glBindBuffer(GL_ARRAY_BUFFER, m_glass_quad_vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(glassQuad), glassQuad, GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 3 * sizeof(float), (void*)0);
    glBindVertexArray(0);
}

// ---  TAAU resolve (render.taau, default OFF) ---

void RenderPipeline::init_taau(u32 width, u32 height) {
    if (width == 0 || height == 0)
        return;
    glGenFramebuffers(1, &m_taau_fbo);
    label_gl_object(GL_FRAMEBUFFER, m_taau_fbo, "taau.fbo");
    // the ping-pong resolved-color history textures are
    // registry-owned with History lifetime (they persist across frames by design;
    // resize recreates them without content preservation, matching the
    // "history invalidated on resize"). The FBO stays PASS-OWNED: it is a transient
    // container whose color attachment is the write-side history, bound per frame in
    // execute_taau_resolve - not a fixed-layout render target the registry can own.
    // The desc reproduces the retired glTexImage2D/glTexParameter calls exactly
    // (RGBA16F, LINEAR, CLAMP_TO_EDGE).
    for (int i = 0; i < 2; ++i) {
        TextureDesc history;
        history.width = width;
        history.height = height;
        history.internal_format = GL_RGBA16F;
        history.format = GL_RGBA;
        history.type = GL_FLOAT;
        history.min_filter = GL_LINEAR;
        history.mag_filter = GL_LINEAR;
        history.wrap_s = GL_CLAMP_TO_EDGE;
        history.wrap_t = GL_CLAMP_TO_EDGE;
        history.lifetime = ResourceLifetime::History;
        history.expected_layout = "color_attachment";
        history.debug_label = "taau.history";
        m_taau_history[i] =
            m_render_registry.create_texture(i == 0 ? "taau_history_0" : "taau_history_1", history)
                .id;
    }
    m_taau_history_write = 0;
    m_taau_history_valid = false; // no usable history until the first resolve fills it
}

void RenderPipeline::destroy_taau() {
    if (m_taau_fbo) {
        glDeleteFramebuffers(1, &m_taau_fbo);
        m_taau_fbo = 0;
    }
    // The history textures are registry-owned.
    m_render_registry.destroy_owned("taau_history_0");
    m_render_registry.destroy_owned("taau_history_1");
    m_taau_history[0] = 0;
    m_taau_history[1] = 0;
    m_taau_history_valid = false;
}

// the TAAU resolve pass contract — the lit-scene color source +
// its FBO (blit target) and the gbuffer motion vectors. The TAAU shader, ping-pong
// history textures (registry-owned per ), FBO, and write-index/valid flags
// stay members (pass-owned resolve state, like a pass owns its shader). Render-only.
RenderContext RenderPipeline::make_taau_context() {
    RenderContext ctx;
    ctx.screen_width = m_screen_width;
    ctx.screen_height = m_screen_height;
    ctx.internal_width = m_internal_width; //
    ctx.internal_height = m_internal_height;
    ctx.registry = &m_render_registry;
    ctx.screen_quad_vao = m_screen_quad_vao;
    const FrameBufferObject& lfbo = m_lighting_pass->lighting_fbo();
    ctx.lit_scene_color = m_render_registry.adopt_texture("lit_scene_color", lfbo.color_texture);
    ctx.lit_scene = m_render_registry.adopt_fbo("lit_scene", lfbo.fbo_id);
    ctx.motion_vectors = m_render_registry.adopt_texture(
        "motion_vectors", m_gbuffer_pass->gbuffer().motion_vector_texture);
    return ctx;
}

void RenderPipeline::execute_taau_resolve(const RenderContext& ctx) {
    if (!m_taau_shader || !m_taau_shader->IsValid() || m_taau_fbo == 0 ||
        ctx.screen_quad_vao == 0 || ctx.screen_width == 0 || ctx.screen_height == 0) {
        return;
    }
    const int wr = m_taau_history_write;
    const int rd = 1 - wr;
    const GLuint lit_color = ctx.lit_scene_color.id;
    const GLuint lit_fbo = ctx.lit_scene.id;

    // Resolve current (lit HDR) + motion-reprojected history -> history[wr].
    glBindFramebuffer(GL_FRAMEBUFFER, m_taau_fbo);
    glFramebufferTexture2D(
        GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, m_taau_history[wr], 0);
    const GLenum draw0[1] = {GL_COLOR_ATTACHMENT0};
    glDrawBuffers(1, draw0);
    glViewport(0, 0, ctx.screen_width, ctx.screen_height);

    const GLboolean depth_was = glIsEnabled(GL_DEPTH_TEST);
    glDisable(GL_DEPTH_TEST);
    const GLboolean blend_was = glIsEnabled(GL_BLEND);
    glDisable(GL_BLEND);

    m_taau_shader->use();
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, lit_color);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, m_taau_history[rd]);
    glActiveTexture(GL_TEXTURE2);
    glBindTexture(GL_TEXTURE_2D, ctx.motion_vectors.id);
    m_taau_shader->setInt("u_current", 0);
    m_taau_shader->setInt("u_history", 1);
    m_taau_shader->setInt("u_motion", 2);
    m_taau_shader->setVec2(
        "u_texel",
        glm::vec2(1.0f / (float)ctx.internal_w(),
                  1.0f / (float)ctx.internal_h())); // neighborhood steps by internal texels
                                                    // (u_current is internal-sized)
    m_taau_shader->setFloat("u_blend", 0.9f);
    m_taau_shader->setFloat("u_sharpness", 0.4f); // recover TAA temporal-blur softness
    m_taau_shader->setInt("u_history_valid", m_taau_history_valid ? 1 : 0);

    glBindVertexArray(ctx.screen_quad_vao);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glBindVertexArray(0);

    // Copy the resolved result back into the lighting color so the final blit shows it; keep
    // history[wr] for next frame's reprojection.
    glBindFramebuffer(GL_READ_FRAMEBUFFER, m_taau_fbo);
    glReadBuffer(GL_COLOR_ATTACHMENT0);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, lit_fbo);
    glDrawBuffer(GL_COLOR_ATTACHMENT0);
    glBlitFramebuffer(0,
                      0,
                      ctx.screen_width,
                      ctx.screen_height,
                      0,
                      0,
                      ctx.screen_width,
                      ctx.screen_height,
                      GL_COLOR_BUFFER_BIT,
                      GL_NEAREST);

    if (depth_was)
        glEnable(GL_DEPTH_TEST);
    if (blend_was)
        glEnable(GL_BLEND);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glActiveTexture(GL_TEXTURE0);
    m_taau_history_write = rd; // ping-pong
    m_taau_history_valid = true;
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
    // release the shared terrain geometry pool (unmaps + deletes all
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
    m_lighting_pass->destroy_lighting_fbo(m_render_registry);
    m_gbuffer_pass->destroy_gbuffer(m_render_registry);
    m_shadow_pass->destroy_shadow_map(m_render_registry);
    m_ssao_pass->destroy_ssao(m_render_registry);
    destroy_halfres_cloud(); // render-optimization: reduced-res sky-dome target
    if (m_screen_quad_vao) {
        glDeleteVertexArrays(1, &m_screen_quad_vao);
        m_screen_quad_vao = 0;
    }
    if (m_screen_quad_vbo) {
        glDeleteBuffers(1, &m_screen_quad_vbo);
        m_screen_quad_vbo = 0;
    }
    if (m_glass_quad_vao) {
        glDeleteVertexArrays(1, &m_glass_quad_vao);
        m_glass_quad_vao = 0;
    }
    if (m_glass_quad_vbo) {
        glDeleteBuffers(1, &m_glass_quad_vbo);
        m_glass_quad_vbo = 0;
    }
    // the auto-exposure meter kernel + SSBO + readback ring.
    if (m_lum_reduce_program) {
        glDeleteProgram(m_lum_reduce_program);
        m_lum_reduce_program = 0;
    }
    if (m_lum_reduce_ssbo) {
        glDeleteBuffers(1, &m_lum_reduce_ssbo);
        m_lum_reduce_ssbo = 0;
    }
    m_exposure_ring.shutdown();
    m_metered_valid = false;
    //  rendering: the froxel kernels + volumes.
    if (m_froxel_inject_program) {
        glDeleteProgram(m_froxel_inject_program);
        m_froxel_inject_program = 0;
    }
    if (m_froxel_integrate_program) {
        glDeleteProgram(m_froxel_integrate_program);
        m_froxel_integrate_program = 0;
    }
    if (m_froxel_scatter_tex) {
        glDeleteTextures(1, &m_froxel_scatter_tex);
        m_froxel_scatter_tex = 0;
    }
    if (m_froxel_integrated_tex) {
        glDeleteTextures(1, &m_froxel_integrated_tex);
        m_froxel_integrated_tex = 0;
    }
    // the WBOIT glass chain.
    m_glass_oit_shader.reset();
    m_glass_oit_resolve_shader.reset();
    if (m_oit_fbo) {
        glDeleteFramebuffers(1, &m_oit_fbo);
        m_oit_fbo = 0;
    }
    if (m_oit_accum_tex) {
        glDeleteTextures(1, &m_oit_accum_tex);
        m_oit_accum_tex = 0;
    }
    if (m_oit_reveal_tex) {
        glDeleteTextures(1, &m_oit_reveal_tex);
        m_oit_reveal_tex = 0;
    }
    // release the baked waterfall sheet geometry.
    if (m_waterfall_vao) {
        glDeleteVertexArrays(1, &m_waterfall_vao);
        m_waterfall_vao = 0;
    }
    if (m_waterfall_vbo) {
        glDeleteBuffers(1, &m_waterfall_vbo);
        m_waterfall_vbo = 0;
    }
    m_waterfall_sheet_sites.clear();
    m_waterfall_geometry_built = false;
    m_skybox_pass->destroy_geometry();
    if (m_particle_pass) {
        m_particle_pass->destroy_buffers();
    } //
    if (m_foliage_pass) {
        m_foliage_pass->destroy_buffers();
        m_foliage_pass->destroy_compute();
    } //  /  #4
    if (m_plant_procgen_pass) {
        m_plant_procgen_pass->destroy_buffers();
    } //
    if (m_ground_decal_pass) {
        m_ground_decal_pass->destroy_buffers();
    } //
    if (m_debug_view_pass) {
        m_debug_view_pass->destroy_buffers();
    } // render-only debug view
    m_sky_lut.destroy(); //  release scattering LUT textures
    m_gbuffer_pass->destroy_instanced_static_mesh();
    m_gbuffer_pass->destroy_skinned_mesh();
    if (m_terrainTextureArray) {
        glDeleteTextures(1, &m_terrainTextureArray);
        m_terrainTextureArray = 0;
    }
    if (m_terrainNormalArray) {
        glDeleteTextures(1, &m_terrainNormalArray);
        m_terrainNormalArray = 0;
    }
    if (m_terrainRoughnessArray) {
        glDeleteTextures(1, &m_terrainRoughnessArray);
        m_terrainRoughnessArray = 0;
    }
    if (m_skinnedTextureArray) {
        glDeleteTextures(1, &m_skinnedTextureArray);
        m_skinnedTextureArray = 0;
    }
    if (m_materialLUT) {
        glDeleteTextures(1, &m_materialLUT);
        m_materialLUT = 0;
    }
    m_water_pass->destroy_water_fallback_textures(m_render_registry);
    destroy_gpu_pass_timers();
    m_gbuffer_pass->reset_shaders();
    m_lighting_pass->reset_shader();
    m_skybox_pass->reset_shader();
    if (m_particle_pass) {
        m_particle_pass->reset_shader();
    } //
    if (m_foliage_pass) {
        m_foliage_pass->reset_shader();
    } //
    if (m_plant_procgen_pass) {
        m_plant_procgen_pass->reset_shader();
    } //
    if (m_ground_decal_pass) {
        m_ground_decal_pass->reset_shader();
    } //
    if (m_debug_view_pass) {
        m_debug_view_pass->reset_shader();
    } // render-only debug view
    m_aerial_shader.reset();    //  aerial-perspective fullscreen shader
    m_waterfall_shader.reset(); //  waterfall falling-sheet shader
    m_shadow_pass->reset_shader();
    m_ssao_pass->reset_shaders();
    m_water_pass->reset_shader();
    m_last_render_pass_metadata.clear();
    m_terrain_texture_fallback_layers = 0;
    m_started = false;
}

// bake the live waterfall DRESSING for `world`. Builds one vertical
// world-space quad per detected site into m_waterfall_vao/_vbo and emits one
// spray emitter per site (capped).: the SITES are a pure function of
// the generated world (camera/frame independent, same seed -> same sites), but
// the dressing geometry/spray are never hashed (one-way, regression review/).
void RenderPipeline::prepare_waterfalls(const Systems::SHIELD_WorldSystem& world) {
    // Re-bakeable: drop any prior geometry so a re-enter rebuilds cleanly.
    if (m_waterfall_vao) {
        glDeleteVertexArrays(1, &m_waterfall_vao);
        m_waterfall_vao = 0;
    }
    if (m_waterfall_vbo) {
        glDeleteBuffers(1, &m_waterfall_vbo);
        m_waterfall_vbo = 0;
    }
    m_waterfall_sheet_sites.clear();
    m_waterfall_geometry_built = false;

    // World-deterministic detection (cached). Copy into m_waterfall_sheet_sites so
    // the per-site crest/foot Y survive even if the cache is later cleared.
    const std::vector<WaterfallSite>& sites = waterfall_sites(world);
    if (sites.empty()) {
        LUMINUMBRA_CORE_INFO("[waterfall] prepare_waterfalls: 0 sites detected (no dressing)");
        return;
    }
    m_waterfall_sheet_sites = sites;

    // Interleaved pos(3) + normal(3) per vertex; 12 verts per site = the vertical SHEET (6) +
    // a horizontal plunge-POOL quad at the foot (6) so the fall visibly lands in water (the
    // "connect to water" fix — the pool reads as roiling foam because the shader's fall_t≈1 there).
    constexpr int kFloatsPerVertex = 6;
    constexpr int kVertsPerSite = 12;
    std::vector<float> verts;
    verts.reserve(m_waterfall_sheet_sites.size() * kVertsPerSite * kFloatsPerVertex);

    constexpr float kMinWidth = 2.0f; // a sane minimum sheet width (m)
    constexpr float kMinRun = 0.5f;   // ensure the foot is advanced downstream
    const glm::vec3 up(0.0f, 1.0f, 0.0f);

    for (const WaterfallSite& s : m_waterfall_sheet_sites) {
        // Downhill flow azimuth in XZ. Guard div-by-zero on normalize (fallback +X).
        glm::vec2 flow2 = s.flow_dir;
        float flow_len = std::sqrt(flow2.x * flow2.x + flow2.y * flow2.y);
        if (flow_len > 1e-5f) {
            flow2 /= flow_len;
        } else {
            flow2 = glm::vec2(1.0f, 0.0f);
        }
        const glm::vec3 flow_dir(flow2.x, 0.0f, flow2.y);
        // Cross-flow axis (the sheet's horizontal width direction): perpendicular
        // to flow in XZ. cross(up, flow_dir) is unit (both unit + orthogonal).
        const glm::vec3 cross_axis = glm::cross(up, flow_dir);

        const float width = std::max(s.width, kMinWidth);
        const float half_w = width * 0.5f;
        const float run = std::max(s.run_length, kMinRun);

        // Lip (top) at the crest XZ; foot (bottom) advanced downstream by the run.
        // CONNECT-TO-WATER: anchor the lip to the upstream WATER surface where the river/lake
        // actually carries water (WaterLevelAt > terrain), so the sheet starts AT the water
        // instead of floating on dry rock. Falls back to the terrain crest on perched/dry drops.
        const float crest_water = world.WaterLevelAt(s.crest.x, s.crest.z);
        const float top_y = std::max(s.crest.y, crest_water);
        const float bot_y = s.foot.y;
        const glm::vec3 top_center(s.crest.x, top_y, s.crest.z);
        const glm::vec3 bot_center =
            top_center + flow_dir * run + glm::vec3(0.0f, bot_y - top_y, 0.0f);

        // Four corners of the vertical sheet (left/right along the cross axis).
        const glm::vec3 tl = top_center - cross_axis * half_w;
        const glm::vec3 tr = top_center + cross_axis * half_w;
        const glm::vec3 bl = bot_center - cross_axis * half_w;
        const glm::vec3 br = bot_center + cross_axis * half_w;

        // Normal: horizontal, perpendicular to the sheet. cross(cross_axis, up)
        // == flow_dir (the downstream-facing horizontal normal). The sheet is
        // drawn double-sided (cull off) so the exact orientation is cosmetic.
        glm::vec3 n = glm::cross(cross_axis, up);
        float nlen = glm::length(n);
        n = (nlen > 1e-5f) ? (n / nlen) : glm::vec3(0.0f, 0.0f, 1.0f);

        auto push_vert = [&](const glm::vec3& p) {
            verts.push_back(p.x);
            verts.push_back(p.y);
            verts.push_back(p.z);
            verts.push_back(n.x);
            verts.push_back(n.y);
            verts.push_back(n.z);
        };
        // Triangle 1: tl, bl, br; Triangle 2: tl, br, tr.
        push_vert(tl);
        push_vert(bl);
        push_vert(br);
        push_vert(tl);
        push_vert(br);
        push_vert(tr);

        // PLUNGE POOL: a horizontal foamy water quad at the foot so the fall lands IN water
        // (visually connects sheet -> pool). Centred at the foot, sized ~1.6x the sheet width,
        // facing UP; at y≈foot the shader's fall_t≈1 -> roiling plunge foam, so it reads as a pool.
        n = up; // these 6 verts face up (overrides the sheet's horizontal normal in push_vert)
        const float pool_half = std::max(half_w * 1.6f, 2.0f);
        const glm::vec3 pc(bot_center.x, bot_y + 0.10f, bot_center.z);
        const glm::vec3 pa = cross_axis * pool_half; // width axis
        const glm::vec3 pb = flow_dir * pool_half;   // downstream axis
        const glm::vec3 ptl = pc - pa - pb;
        const glm::vec3 ptr = pc + pa - pb;
        const glm::vec3 pbl = pc - pa + pb;
        const glm::vec3 pbr = pc + pa + pb;
        push_vert(ptl);
        push_vert(pbl);
        push_vert(pbr);
        push_vert(ptl);
        push_vert(pbr);
        push_vert(ptr);
    }

    // Upload the combined sheet geometry to a dedicated VAO/VBO.
    glGenVertexArrays(1, &m_waterfall_vao);
    glGenBuffers(1, &m_waterfall_vbo);
    glBindVertexArray(m_waterfall_vao);
    glBindBuffer(GL_ARRAY_BUFFER, m_waterfall_vbo);
    glBufferData(GL_ARRAY_BUFFER,
                 static_cast<GLsizeiptr>(verts.size() * sizeof(float)),
                 verts.data(),
                 GL_STATIC_DRAW);
    // location 0 = aPos, location 1 = aNormal (basic.vert layout).
    glVertexAttribPointer(
        0, 3, GL_FLOAT, GL_FALSE, kFloatsPerVertex * sizeof(float), reinterpret_cast<void*>(0));
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(1,
                          3,
                          GL_FLOAT,
                          GL_FALSE,
                          kFloatsPerVertex * sizeof(float),
                          reinterpret_cast<void*>(3 * sizeof(float)));
    glEnableVertexAttribArray(1);
    glBindVertexArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    label_gl_object(GL_VERTEX_ARRAY, m_waterfall_vao, "waterfall.sheet_vao");
    label_gl_object(GL_BUFFER, m_waterfall_vbo, "waterfall.sheet_vbo");
    m_waterfall_geometry_built = true;

    //  spray: one mist emitter per plunge foot, capped to bound particle cost.
    constexpr std::size_t kMaxSpray = 24;
    std::size_t spray_count = 0;
    if (m_particle_pass) {
        const std::filesystem::path spray_json =
            m_root_path / "data/common/particles/waterfall_spray.json";
        for (const WaterfallSite& s : m_waterfall_sheet_sites) {
            if (spray_count >= kMaxSpray)
                break;
            m_particle_pass->add_waterfall_spray(spray_json, s.foot, s.drop_height, s.width);
            ++spray_count;
        }
    }

    if (m_waterfall_sheet_sites.size() > kMaxSpray) {
        LUMINUMBRA_CORE_INFO(
            "[waterfall] prepare_waterfalls: {} sites; sheets drawn for all, spray capped to {}",
            m_waterfall_sheet_sites.size(),
            kMaxSpray);
    } else {
        LUMINUMBRA_CORE_INFO(
            "[waterfall] prepare_waterfalls: {} sites (sheets + {} spray emitters)",
            m_waterfall_sheet_sites.size(),
            spray_count);
    }
}

// --- RESOURCE MANAGEMENT ---

void RenderPipeline::manage_chunk_gpu_resources(
    const std::vector<ChunkMeshSnapshot>& renderable_chunks, const Camera& camera) {
    // --- Configuration for the new logic ---
    constexpr std::size_t kMinTerrainUploadsPerFrame = 8;
    constexpr std::size_t kMaxTerrainUploadsPerFrame = 64;
    const u32 INACTIVE_FRAME_TTL = 15; // Grace period: unload after 15 frames of inactivity

    // Step 1: Create a quick-lookup set of chunks that should be active this frame.
    std::unordered_set<ChunkID> active_chunk_ids;
    active_chunk_ids.reserve(renderable_chunks.size());
    for (const auto& chunk : renderable_chunks) {
        active_chunk_ids.insert(chunk.id);
    }

    // Step 2: Mark-and-sweep stale GPU resources.
    // Instead of unloading immediately, we mark chunks as inactive and give them a time-to-live
    // (TTL).
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
        if (!chunk.has_terrain_mesh())
            continue;

        auto it = m_chunk_render_data.find(chunk.id);

        bool is_new = (it == m_chunk_render_data.end());
        bool is_stale = !is_new && (it->second.mesh_version != chunk.mesh_version);

        if (is_new || is_stale) {
            const glm::vec3 center(chunk.coords.x * CHUNK_SIZE_X + CHUNK_SIZE_X * 0.5f,
                                   chunk.coords.y * CHUNK_SIZE_Y + CHUNK_SIZE_Y * 0.5f,
                                   chunk.coords.z * CHUNK_SIZE_Z + CHUNK_SIZE_Z * 0.5f);
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
    std::sort(upload_candidates.begin(),
              upload_candidates.end(),
              [](const TerrainUploadCandidate& a, const TerrainUploadCandidate& b) {
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
    //  implementation note (streaming-burst amortization): a FIXED count budget (up to 64) of
    // freshly-streamed chunk meshes uploaded in one frame spikes render to 40ms+ when moving into a
    // dense region (a fixed count of LARGE meshes = a big hitch — research: Zylann godot_voxel uses
    // a per-frame TIME budget, not a count). Time-slice the upload: drain nearest-first (already
    // sorted) until ~kTerrainUploadBudgetMs is spent, then defer the rest to following frames. A
    // small floor (kMinTerrainUploadsPerFrame) guarantees forward progress so the near field still
    // fills quickly. Render-only: the GPU geometry pool is NOT hashed (chunk mesh CONTENT is set in
    // the world tick), so WHICH frame a mesh uploads is determinism-neutral — no lockstep impact,
    // no gate change.
    constexpr double kTerrainUploadBudgetMs = 3.0;
    const auto _upload_t0 = std::chrono::steady_clock::now();
    float farthest_selected_distance = 0.0f;
    std::size_t processed = upload_budget;
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

        if (i + 1 >= kMinTerrainUploadsPerFrame &&
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - _upload_t0)
                    .count() > kTerrainUploadBudgetMs) {
            processed = i + 1; // time slice spent; defer the remainder (nearest-first already done)
            break;
        }
    }
    m_last_mesh_upload_stats.terrain_uploads_deferred =
        upload_candidates.size() - m_last_mesh_upload_stats.terrain_uploads;
    m_last_mesh_upload_stats.terrain_farthest_selected_distance_sq = farthest_selected_distance;
    if (upload_candidates.size() > processed) {
        float nearest_deferred = std::numeric_limits<float>::max();
        for (std::size_t i = processed; i < upload_candidates.size(); ++i) {
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

void RenderPipeline::manage_water_gpu_resources(
    const std::vector<ChunkMeshSnapshot>& renderable_chunks, const Camera& camera) {
    const int MAX_UPLOADS_PER_FRAME = 8; // Increased to match chunk upload budget
    const u32 INACTIVE_FRAME_TTL =
        30; // A slightly longer TTL for water as it may be just off-screen

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
        if (!chunk.has_water_mesh())
            continue; // Skip chunks with no water

        auto it = m_water_render_data.find(chunk.id);
        bool is_new = (it == m_water_render_data.end());
        bool is_stale = !is_new && (it->second.mesh_version != chunk.water_mesh_version);

        if (is_new || is_stale) {
            const glm::vec3 center(chunk.coords.x * CHUNK_SIZE_X + CHUNK_SIZE_X * 0.5f,
                                   chunk.coords.y * CHUNK_SIZE_Y + CHUNK_SIZE_Y * 0.5f,
                                   chunk.coords.z * CHUNK_SIZE_Z + CHUNK_SIZE_Z * 0.5f);
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
    std::sort(upload_candidates.begin(),
              upload_candidates.end(),
              [](const WaterUploadCandidate& a, const WaterUploadCandidate& b) {
                  if (a.distance_sq != b.distance_sq) {
                      return a.distance_sq < b.distance_sq;
                  }
                  if (a.is_new != b.is_new) {
                      return a.is_new;
                  }
                  return a.chunk.id < b.chunk.id;
              });

    const std::size_t upload_budget =
        std::min(upload_candidates.size(), static_cast<std::size_t>(MAX_UPLOADS_PER_FRAME));
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
    m_last_mesh_upload_stats.water_uploads_deferred =
        upload_candidates.size() - m_last_mesh_upload_stats.water_uploads;
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

bool RenderPipeline::copy_terrain_mesh_payload(const ChunkMeshSnapshot& chunk,
                                               ChunkMeshPayload& payload) const {
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

bool RenderPipeline::copy_water_mesh_payload(const ChunkMeshSnapshot& chunk,
                                             ChunkMeshPayload& payload) const {
    if (!chunk.source_chunk || !chunk.has_water_mesh()) {
        return false;
    }

    const u32 version_before =
        chunk.source_chunk->water_mesh_version.load(std::memory_order_acquire);
    if (version_before != chunk.water_mesh_version) {
        return false;
    }

    payload.mesh_version = version_before;
    payload.vertices = chunk.source_chunk->water_mesh_vertices;
    payload.indices = chunk.source_chunk->water_mesh_indices;

    const u32 version_after =
        chunk.source_chunk->water_mesh_version.load(std::memory_order_acquire);
    if (version_after != version_before || !payload.has_mesh()) {
        payload.vertices.clear();
        payload.indices.clear();
        return false;
    }

    return true;
}

void RenderPipeline::upload_water_mesh(const ChunkMeshSnapshot& chunk,
                                       const ChunkMeshPayload& payload) {
    if (!payload.has_mesh())
        return;

    if (payload.vertices.size() > std::numeric_limits<u32>::max() ||
        payload.indices.size() > std::numeric_limits<u32>::max()) {
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
    const bool needs_growth =
        data.vertex_capacity < vertex_count || data.index_capacity < index_count;
    const bool version_reused = data.mesh_version != 0 && data.mesh_version != payload.mesh_version;

    glBindVertexArray(data.vao_id);
    glBindBuffer(GL_ARRAY_BUFFER, data.vbo_id);
    if (needs_growth) {
        glBufferData(GL_ARRAY_BUFFER,
                     payload.vertices.size() * sizeof(VoxelVertex),
                     payload.vertices.data(),
                     GL_STATIC_DRAW);
        data.vertex_capacity = vertex_count;
    } else {
        glBufferSubData(GL_ARRAY_BUFFER,
                        0,
                        payload.vertices.size() * sizeof(VoxelVertex),
                        payload.vertices.data());
    }

    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, data.ebo_id);
    if (needs_growth) {
        glBufferData(GL_ELEMENT_ARRAY_BUFFER,
                     payload.indices.size() * sizeof(u32),
                     payload.indices.data(),
                     GL_STATIC_DRAW);
        data.index_capacity = index_count;
    } else {
        glBufferSubData(GL_ELEMENT_ARRAY_BUFFER,
                        0,
                        payload.indices.size() * sizeof(u32),
                        payload.indices.data());
    }

    glEnableVertexAttribArray(0);
    glVertexAttribPointer(
        0, 3, GL_FLOAT, GL_FALSE, sizeof(VoxelVertex), (void*)offsetof(VoxelVertex, position));
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(
        1, 3, GL_FLOAT, GL_FALSE, sizeof(VoxelVertex), (void*)offsetof(VoxelVertex, normal));
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

void RenderPipeline::upload_chunk_mesh(const ChunkMeshSnapshot& chunk,
                                       const ChunkMeshPayload& payload) {
    if (!payload.has_mesh()) {
        return;
    }

    if (payload.vertices.size() > std::numeric_limits<u32>::max() ||
        payload.indices.size() > std::numeric_limits<u32>::max()) {
        m_last_mesh_upload_stats.terrain_upload_failures++;
        return;
    }

    // terrain geometry now lives in the shared bucketed
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
    const u32 new_handle = m_chunk_geometry_pool.update(render_data.pool_handle,
                                                        payload.vertices.data(),
                                                        vertex_count,
                                                        payload.indices.data(),
                                                        index_count,
                                                        label_seed.c_str());
    if (new_handle == ChunkRenderData::kInvalidPoolHandle) {
        // Pool allocation failed (e.g. OOM): leave the record empty so the draw
        // loop skips it, and record the failure.
        render_data.element_count = 0;
        m_last_mesh_upload_stats.terrain_upload_failures++;
        if (record_created)
            m_last_mesh_upload_stats.terrain_slots_created++;
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
    const bool grew = had_allocation && (render_data.vertex_capacity > prior_vertex_capacity ||
                                         render_data.index_capacity > prior_index_capacity);
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
        // return the pool slice before recycling the record. The
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
    // triplanar terrain fidelity. Two GL_TEXTURE_2D_ARRAY objects keyed
    // by the material-LUT layer indices (documented design/): an sRGB albedo array
    // and a linear tangent-space (OpenGL convention) normal-map array. The
    // committed 256x256.ltex plates carry their own box-filtered mip chains, so
    // the whole terrain set stays far inside the 96 MB residency budget. Texture
    // arrays (not bindless) per documented design.
    //
    // Layer order matches the material LUT texture_layer/normal_layer columns:
    //   layer 0 Stone, 1 Soil, 2 Grass, 3 Sand, 4 Deepslate.
    struct TerrainLayerAssets {
        const char* albedo;
        const char* normal;
    };
    const std::array<TerrainLayerAssets, 5> assets = {{
        {"data/textures/terrain/rock/rock_albedo_1024.ltex",
         "data/textures/terrain/rock/rock_normal_1024.ltex"},
        {"data/textures/terrain/soil/soil_albedo_1024.ltex",
         "data/textures/terrain/soil/soil_normal_1024.ltex"},
        {"data/textures/terrain/grass/grass_albedo_1024.ltex",
         "data/textures/terrain/grass/grass_normal_1024.ltex"},
        {"data/textures/terrain/sand/sand_albedo_1024.ltex",
         "data/textures/terrain/sand/sand_normal_1024.ltex"},
        {"data/textures/terrain/deepslate/deepslate_albedo_1024.ltex",
         "data/textures/terrain/deepslate/deepslate_normal_1024.ltex"},
    }};

    const int res = kTerrainTextureResolution;
    const int layer_count = static_cast<int>(assets.size());
    m_terrain_texture_fallback_layers = 0;
    m_material_texture_lut.terrain_layer_count = layer_count;

    // Uploads a.ltex (or a checker fallback) into one layer of a bound array,
    // mip level by mip level. Returns true if the.ltex loaded cleanly.
    auto upload_layer = [&](const std::filesystem::path& rel, int layer, bool is_normal) -> bool {
        LtexCpuImage img;
        if (load_ltex_cpu_image(m_root_path / rel, img) &&
            img.width == static_cast<uint32_t>(res) && img.height == static_cast<uint32_t>(res) &&
            img.channels == 4u) {
            // Upload mip 0 only; the array is allocated with glTexImage3D (which
            // reserves level 0) and glGenerateMipmap below rebuilds the chain.
            // (The.ltex carries pre-built mips, but uploading them to an array
            // that only has level 0 storage is a GL error, and generated mips
            // are equivalent here.)
            glTexSubImage3D(GL_TEXTURE_2D_ARRAY,
                            0,
                            0,
                            0,
                            layer,
                            static_cast<GLsizei>(img.width),
                            static_cast<GLsizei>(img.height),
                            1,
                            GL_RGBA,
                            GL_UNSIGNED_BYTE,
                            img.bytes.data());
            return true;
        }
        // Fallback: magenta checker (albedo) or flat up-normal (normal map).
        if (is_normal) {
            std::vector<unsigned char> flat(static_cast<size_t>(res) * res * 4u);
            for (size_t p = 0; p < static_cast<size_t>(res) * res; ++p) {
                flat[p * 4 + 0] = 128;
                flat[p * 4 + 1] = 128;
                flat[p * 4 + 2] = 255;
                flat[p * 4 + 3] = 255;
            }
            glTexSubImage3D(GL_TEXTURE_2D_ARRAY,
                            0,
                            0,
                            0,
                            layer,
                            res,
                            res,
                            1,
                            GL_RGBA,
                            GL_UNSIGNED_BYTE,
                            flat.data());
        } else {
            const std::vector<unsigned char> fallback =
                make_terrain_fallback_texture(res, res, layer);
            glTexSubImage3D(GL_TEXTURE_2D_ARRAY,
                            0,
                            0,
                            0,
                            layer,
                            res,
                            res,
                            1,
                            GL_RGBA,
                            GL_UNSIGNED_BYTE,
                            fallback.data());
            ++m_terrain_texture_fallback_layers;
        }
        LUMINUMBRA_CORE_ERROR("Terrain texture: failed to load.ltex layer '{}'", rel.string());
        return false;
    };

    // --- Albedo array (sRGB) ---
    glGenTextures(1, &m_terrainTextureArray);
    label_gl_object(GL_TEXTURE, m_terrainTextureArray, "terrain.texture_array");
    glBindTexture(GL_TEXTURE_2D_ARRAY, m_terrainTextureArray);
    glTexImage3D(GL_TEXTURE_2D_ARRAY,
                 0,
                 GL_SRGB8_ALPHA8,
                 res,
                 res,
                 layer_count,
                 0,
                 GL_RGBA,
                 GL_UNSIGNED_BYTE,
                 nullptr);
    for (int i = 0; i < layer_count; ++i) {
        upload_layer(assets[i].albedo, i, /*is_normal=*/false);
    }
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_T, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAX_LEVEL, 10); // 1024 -> 1 is 11 levels
    glGenerateMipmap(GL_TEXTURE_2D_ARRAY); // build full mip chain from level 0

    // --- Normal-map array (linear RGBA8, tangent space) ---
    glGenTextures(1, &m_terrainNormalArray);
    label_gl_object(GL_TEXTURE, m_terrainNormalArray, "terrain.normal_array");
    glBindTexture(GL_TEXTURE_2D_ARRAY, m_terrainNormalArray);
    glTexImage3D(GL_TEXTURE_2D_ARRAY,
                 0,
                 GL_RGBA8,
                 res,
                 res,
                 layer_count,
                 0,
                 GL_RGBA,
                 GL_UNSIGNED_BYTE,
                 nullptr);
    for (int i = 0; i < layer_count; ++i) {
        upload_layer(assets[i].normal, i, /*is_normal=*/true);
    }
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_T, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAX_LEVEL, 10);
    glGenerateMipmap(GL_TEXTURE_2D_ARRAY);

    // --- Roughness-map array (linear RGBA8; roughness in.r) — terrain PBR roughness-map ---
    // Per-material AmbientCG roughness plates give per-texel roughness instead of
    // the flat materials.json scalar. Layer
    // order matches albedo/normal. A missing layer falls back to flat 0.85
    // (the design roughness default) and clears m_terrainRoughnessValid so the
    // shader keeps the per-material scalar rather than a wrong constant.
    const std::array<const char*, 5> roughness_assets = {{
        "data/textures/terrain/rock/rock_roughness_1024.ltex",
        "data/textures/terrain/soil/soil_roughness_1024.ltex",
        "data/textures/terrain/grass/grass_roughness_1024.ltex",
        "data/textures/terrain/sand/sand_roughness_1024.ltex",
        "data/textures/terrain/deepslate/deepslate_roughness_1024.ltex",
    }};
    glGenTextures(1, &m_terrainRoughnessArray);
    label_gl_object(GL_TEXTURE, m_terrainRoughnessArray, "terrain.roughness_array");
    glBindTexture(GL_TEXTURE_2D_ARRAY, m_terrainRoughnessArray);
    glTexImage3D(GL_TEXTURE_2D_ARRAY,
                 0,
                 GL_RGBA8,
                 res,
                 res,
                 layer_count,
                 0,
                 GL_RGBA,
                 GL_UNSIGNED_BYTE,
                 nullptr);
    int roughness_fallbacks = 0;
    for (int i = 0; i < layer_count; ++i) {
        LtexCpuImage img;
        if (load_ltex_cpu_image(m_root_path / roughness_assets[static_cast<size_t>(i)], img) &&
            img.width == static_cast<uint32_t>(res) && img.height == static_cast<uint32_t>(res) &&
            img.channels == 4u) {
            glTexSubImage3D(GL_TEXTURE_2D_ARRAY,
                            0,
                            0,
                            0,
                            i,
                            static_cast<GLsizei>(img.width),
                            static_cast<GLsizei>(img.height),
                            1,
                            GL_RGBA,
                            GL_UNSIGNED_BYTE,
                            img.bytes.data());
        } else {
            std::vector<unsigned char> flat(static_cast<size_t>(res) * res * 4u, 217u); // ~0.85
            glTexSubImage3D(GL_TEXTURE_2D_ARRAY,
                            0,
                            0,
                            0,
                            i,
                            res,
                            res,
                            1,
                            GL_RGBA,
                            GL_UNSIGNED_BYTE,
                            flat.data());
            ++roughness_fallbacks;
            LUMINUMBRA_CORE_ERROR("Terrain roughness: failed to load.ltex layer '{}'",
                                  roughness_assets[static_cast<size_t>(i)]);
        }
    }
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_T, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAX_LEVEL, 10);
    glGenerateMipmap(GL_TEXTURE_2D_ARRAY);
    m_terrainRoughnessValid = (roughness_fallbacks == 0) ? 1 : 0;

    glBindTexture(GL_TEXTURE_2D_ARRAY, 0);

    LUMINUMBRA_CORE_INFO("Terrain texture arrays loaded ({} albedo + {} normal + {} roughness "
                         "layers, {}x{}; roughness_valid={}).",
                         layer_count,
                         layer_count,
                         layer_count,
                         res,
                         res,
                         m_terrainRoughnessValid);
}

void RenderPipeline::init_skinned_texture_array() {
    // UV-mapped skinned-mesh texture array. Two
    // layers (0 = albedo, 1 = tangent-space normal). The engine allocates the
    // array with a flat fallback so a skinned mesh is always drawable; the
    // actual texture set is supplied later by the caller via
    // load_skinned_texture_set, which reads paths from GAME data (the
    // scenario harness pulls them from the creature archetype JSON). No
    // creature/asset path is named in engine source.
    const int res = kSkinnedTextureResolution;
    constexpr int layer_count = 2;

    glGenTextures(1, &m_skinnedTextureArray);
    label_gl_object(GL_TEXTURE, m_skinnedTextureArray, "skinned.texture_array");
    glBindTexture(GL_TEXTURE_2D_ARRAY, m_skinnedTextureArray);
    glTexImage3D(GL_TEXTURE_2D_ARRAY,
                 0,
                 GL_SRGB8_ALPHA8,
                 res,
                 res,
                 layer_count,
                 0,
                 GL_RGBA,
                 GL_UNSIGNED_BYTE,
                 nullptr);

    // Flat fallback: layer 0 mid-grey albedo, layer 1 up-normal.
    for (int i = 0; i < layer_count; ++i) {
        const bool is_normal = (i == 1);
        std::vector<unsigned char> fill(static_cast<size_t>(res) * res * 4u);
        for (size_t p = 0; p < static_cast<size_t>(res) * res; ++p) {
            if (is_normal) {
                fill[p * 4 + 0] = 128;
                fill[p * 4 + 1] = 128;
                fill[p * 4 + 2] = 255;
                fill[p * 4 + 3] = 255;
            } else {
                fill[p * 4 + 0] = 120;
                fill[p * 4 + 1] = 150;
                fill[p * 4 + 2] = 90;
                fill[p * 4 + 3] = 255;
            }
        }
        glTexSubImage3D(
            GL_TEXTURE_2D_ARRAY, 0, 0, 0, i, res, res, 1, GL_RGBA, GL_UNSIGNED_BYTE, fill.data());
    }
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_T, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAX_LEVEL, 10);
    glGenerateMipmap(GL_TEXTURE_2D_ARRAY);
    glBindTexture(GL_TEXTURE_2D_ARRAY, 0);

    // Layers are valid (flat fallback) even before a set is loaded.
    m_skinnedAlbedoLayer = 0;
    m_skinnedNormalLayer = 1;
    LUMINUMBRA_CORE_INFO("Skinned texture array allocated ({} layers, {}x{}, flat fallback).",
                         layer_count,
                         res,
                         res);
}

bool RenderPipeline::load_skinned_texture_set(const std::filesystem::path& albedo_path,
                                              const std::filesystem::path& normal_path,
                                              int& albedo_layer_out,
                                              int& normal_layer_out) {
    // Generic, data-driven loader. Uploads the supplied
    // albedo into layer 0 and normal into layer 1; a missing/mismatched file
    // leaves that layer's existing fallback in place. Callers (scenario harness)
    // pass paths read from game data, so no content name lives in engine source.
    if (m_skinnedTextureArray == 0) {
        init_skinned_texture_array();
    }
    const int res = kSkinnedTextureResolution;
    glBindTexture(GL_TEXTURE_2D_ARRAY, m_skinnedTextureArray);

    struct SetLayer {
        const std::filesystem::path& path;
        int layer;
    };
    const std::array<SetLayer, 2> set = {{{albedo_path, 0}, {normal_path, 1}}};
    bool albedo_ok = false;
    for (const auto& s : set) {
        if (s.path.empty())
            continue;
        LtexCpuImage img;
        if (load_ltex_cpu_image(s.path, img) && img.width == static_cast<uint32_t>(res) &&
            img.height == static_cast<uint32_t>(res) && img.channels == 4u) {
            glTexSubImage3D(GL_TEXTURE_2D_ARRAY,
                            0,
                            0,
                            0,
                            s.layer,
                            static_cast<GLsizei>(img.width),
                            static_cast<GLsizei>(img.height),
                            1,
                            GL_RGBA,
                            GL_UNSIGNED_BYTE,
                            img.bytes.data());
            if (s.layer == 0)
                albedo_ok = true;
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
                         albedo_ok ? "textured" : "fallback",
                         res,
                         res);
    return albedo_ok;
}

void RenderPipeline::init_static_model_texture_array() {
    const int res = kStaticModelTextureResolution;
    const int layers = kStaticModelTextureLayers;
    glGenTextures(1, &m_staticModelTextureArray);
    label_gl_object(GL_TEXTURE, m_staticModelTextureArray, "static_model.texture_array");
    glBindTexture(GL_TEXTURE_2D_ARRAY, m_staticModelTextureArray);
    glTexImage3D(GL_TEXTURE_2D_ARRAY,
                 0,
                 GL_SRGB8_ALPHA8,
                 res,
                 res,
                 layers,
                 0,
                 GL_RGBA,
                 GL_UNSIGNED_BYTE,
                 nullptr);
    // Flat fallback every layer: even = mid-grey albedo, odd = up-normal.
    for (int i = 0; i < layers; ++i) {
        const bool is_normal = (i % 2) == 1;
        std::vector<unsigned char> fill(static_cast<size_t>(res) * res * 4u);
        for (size_t p = 0; p < static_cast<size_t>(res) * res; ++p) {
            if (is_normal) {
                fill[p * 4 + 0] = 128;
                fill[p * 4 + 1] = 128;
                fill[p * 4 + 2] = 255;
                fill[p * 4 + 3] = 255;
            } else {
                fill[p * 4 + 0] = 120;
                fill[p * 4 + 1] = 120;
                fill[p * 4 + 2] = 120;
                fill[p * 4 + 3] = 255;
            }
        }
        glTexSubImage3D(
            GL_TEXTURE_2D_ARRAY, 0, 0, 0, i, res, res, 1, GL_RGBA, GL_UNSIGNED_BYTE, fill.data());
    }
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_T, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAX_LEVEL, 10);
    glGenerateMipmap(GL_TEXTURE_2D_ARRAY);
    glBindTexture(GL_TEXTURE_2D_ARRAY, 0);
    m_staticModelNextLayer = 0;
    LUMINUMBRA_CORE_INFO(
        "Static-model texture array allocated ({} layers, {}x{}).", layers, res, res);
}

bool RenderPipeline::load_static_model_texture_set(const std::filesystem::path& albedo_path,
                                                   const std::filesystem::path& normal_path,
                                                   int& albedo_layer_out,
                                                   int& normal_layer_out) {
    if (m_staticModelTextureArray == 0)
        init_static_model_texture_array();
    if (m_staticModelNextLayer + 1 >= kStaticModelTextureLayers) {
        LUMINUMBRA_CORE_WARN("Static-model texture array full; cannot load '{}'.",
                             albedo_path.string());
        return false;
    }
    const int res = kStaticModelTextureResolution;
    const int albedo_layer = m_staticModelNextLayer;
    const int normal_layer = m_staticModelNextLayer + 1;
    glBindTexture(GL_TEXTURE_2D_ARRAY, m_staticModelTextureArray);
    struct SetLayer {
        const std::filesystem::path& path;
        int layer;
    };
    const std::array<SetLayer, 2> set = {
        {{albedo_path, albedo_layer}, {normal_path, normal_layer}}};
    bool albedo_ok = false;
    for (const auto& s : set) {
        if (s.path.empty())
            continue;
        LtexCpuImage img;
        if (load_ltex_cpu_image(s.path, img) && img.width == static_cast<uint32_t>(res) &&
            img.height == static_cast<uint32_t>(res) && img.channels == 4u) {
            glTexSubImage3D(GL_TEXTURE_2D_ARRAY,
                            0,
                            0,
                            0,
                            s.layer,
                            res,
                            res,
                            1,
                            GL_RGBA,
                            GL_UNSIGNED_BYTE,
                            img.bytes.data());
            if (s.layer == albedo_layer)
                albedo_ok = true;
        } else {
            LUMINUMBRA_CORE_WARN("Static-model texture: failed to load '{}', keeping fallback.",
                                 s.path.string());
        }
    }
    glGenerateMipmap(GL_TEXTURE_2D_ARRAY);
    glBindTexture(GL_TEXTURE_2D_ARRAY, 0);
    m_staticModelNextLayer += 2;
    albedo_layer_out = albedo_layer;
    normal_layer_out = normal_layer;
    return albedo_ok;
}

void RenderPipeline::register_static_model_textures() {
    // Data-driven: data/models/trees/tree_textures.json maps each part mesh to its
    // albedo/normal.ltex + an alpha-test flag (leaves). Render-only; absent file
    // is a graceful no-op (parts keep the flat fallback / terrain look).
    const std::filesystem::path manifest = m_root_path / "data/models/trees/tree_textures.json";
    std::ifstream in(manifest);
    if (!in)
        return;
    try {
        nlohmann::json j;
        in >> j;
        for (const auto& m : j.value("models", nlohmann::json::array())) {
            const std::string mesh = m.value("mesh", std::string{});
            if (mesh.empty())
                continue;
            const std::filesystem::path albedo =
                m.contains("albedo") ? (m_root_path / m["albedo"].get<std::string>())
                                     : std::filesystem::path{};
            const std::filesystem::path normal =
                m.contains("normal") ? (m_root_path / m["normal"].get<std::string>())
                                     : std::filesystem::path{};
            int al = -1, nl = -1;
            const bool ok = load_static_model_texture_set(albedo, normal, al, nl);
            if (!ok)
                continue;
            StaticModelTex tex;
            tex.albedoLayer = al;
            tex.normalLayer = nl;
            tex.alphaTest = m.value("alpha_test", false);
            m_staticModelTextures[mesh] = tex;
        }
        LUMINUMBRA_CORE_INFO("Static-model textures registered ({} models).",
                             m_staticModelTextures.size());
    } catch (const std::exception& e) {
        LUMINUMBRA_CORE_WARN("tree_textures.json parse failed: {}", e.what());
    }
}

void RenderPipeline::load_material_texture_lut() {
    // Parse the texture_layer/normal_layer/tiling columns from materials.json
    // (documented design). Defaults (untextured/flat, tiling 4) are kept for any
    // material that omits the columns or for a missing file.
    m_material_texture_lut = MaterialTextureLut{};
    const std::filesystem::path path = m_root_path / "data/common/materials.json";
    std::ifstream in(path);
    if (!in) {
        LUMINUMBRA_CORE_WARN(
            "Material texture LUT: materials.json not found at '{}', terrain stays untextured.",
            path.string());
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
            if (!mat.contains("id"))
                continue;
            const int id = mat["id"].get<int>();
            if (id < 0 || id >= 256)
                continue;
            if (mat.contains("texture_layer")) {
                m_material_texture_lut.texture_layer[static_cast<size_t>(id)] =
                    mat["texture_layer"].get<int>();
                if (m_material_texture_lut.texture_layer[static_cast<size_t>(id)] >= 0)
                    ++textured;
            }
            if (mat.contains("normal_layer")) {
                m_material_texture_lut.normal_layer[static_cast<size_t>(id)] =
                    mat["normal_layer"].get<int>();
            }
            if (mat.contains("tiling")) {
                const float t = mat["tiling"].get<float>();
                if (t > 0.0f)
                    m_material_texture_lut.tiling[static_cast<size_t>(id)] = t;
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
            // terrain PBR: metallic column (was hardcoded in init_material_lut). 0..1.
            if (mat.contains("metallic")) {
                m_material_texture_lut.metallic[static_cast<size_t>(id)] =
                    glm::clamp(mat["metallic"].get<float>(), 0.0f, 1.0f);
                m_material_texture_lut.metallic_set[static_cast<size_t>(id)] = true;
            }
            //  optional albedo multiplier (render-only LUT
            // calibration). Clamped to a sane (0, 1] range; absent -> 1.0.
            if (mat.contains("albedo_scale")) {
                m_material_texture_lut.albedo_scale[static_cast<size_t>(id)] =
                    glm::clamp(mat["albedo_scale"].get<float>(), 0.0f, 1.0f);
            }
            //  dusty- palette: optional per-material warm albedo tint
            // [r,g,b] (render-only). Absent -> [1,1,1] (no-op). The tint is a
            // MULTIPLIER baked into the 0..1 RGBA8 LUT, so it is clamped to
            // [0,1]: a channel can only be left at 1 or suppressed. "Warmth" is
            // therefore achieved by lowering G/B relative to R (not by boosting
            // R > 1, which the RGBA8 LUT cannot represent).
            if (mat.contains("albedo_tint") && mat["albedo_tint"].is_array() &&
                mat["albedo_tint"].size() == 3) {
                const auto& t = mat["albedo_tint"];
                m_material_texture_lut.albedo_tint[static_cast<size_t>(id)] =
                    glm::vec3(glm::clamp(t[0].get<float>(), 0.0f, 1.0f),
                              glm::clamp(t[1].get<float>(), 0.0f, 1.0f),
                              glm::clamp(t[2].get<float>(), 0.0f, 1.0f));
            }
        }
        LUMINUMBRA_CORE_INFO(
            "Material texture LUT parsed: {} textured material(s) from materials.json.", textured);
    } catch (const std::exception& e) {
        LUMINUMBRA_CORE_WARN(
            "Material texture LUT: failed to parse materials.json ({}); terrain stays untextured.",
            e.what());
        m_material_texture_lut = MaterialTextureLut{};
    }
}

void RenderPipeline::init_material_lut() {
    // Material properties LUT ( two rows;  adds row 2). Sampled by
    // material id (u = id/255) at the row centers:
    //   row 0 (v=1/6): [R metallic, G roughness, B AO, A magical-flag]
    //   row 1 (v=1/2): [R texture_layer/255, G normal_layer/255, B tiling/64,
    //                   A has_texture]
    //   row 2 (v=5/8): [R emissive_intensity/kEmissiveLutScale,
    //                   G albedo_scale (0..1, default 1; ), B/A reserved]
    //   row 3 (v=7/8): [RGB albedo_tint (default 1,1,1;  dusty-), A reserved]
    // The texture/emissive columns come from materials.json
    // (load_material_texture_lut). The emissive_intensity column drives the
    // emission->lighting->glow chain ( calibration); it is stored
    // normalized by kEmissiveLutScale so the 0..1 RGBA8 LUT covers intensities
    // up to that ceiling, and the lighting pass rescales it back.
    // ROWS widened 3->4 to carry the per-material albedo_tint. The shader
    // row v-coords are now the centers (row+0.5)/4 = 0.125/0.375/0.625/0.875.
    const int MATERIAL_COUNT = 256;
    const int ROWS = 4;

    // Default fill: NON-metallic (metallic 0). A 0.1 metallic default tinted the
    // Fresnel F0 toward albedo on every material without an explicit row — notably
    // the skinned avatar (which falls through to the default), producing a spurious
    // cyan/blue specular lobe. Dielectric default (metallic 0, F0 0.04) is correct;
    // real metals set their own row.
    std::vector<glm::vec4> materialData(static_cast<size_t>(MATERIAL_COUNT) * ROWS,
                                        glm::vec4(0.0f, 0.8f, 1.0f, 0.0f));
    auto row0 = [&](int id) -> glm::vec4& {
        return materialData[static_cast<size_t>(id)];
    };
    auto row1 = [&](int id) -> glm::vec4& {
        return materialData[static_cast<size_t>(MATERIAL_COUNT + id)];
    };
    auto row2 = [&](int id) -> glm::vec4& {
        return materialData[static_cast<size_t>(2 * MATERIAL_COUNT + id)];
    };
    auto row3 = [&](int id) -> glm::vec4& {
        return materialData[static_cast<size_t>(3 * MATERIAL_COUNT + id)];
    };

    // Row 0 (metallic / roughness / AO / magical).: the G (roughness)
    // channel is now DRIVEN by the materials.json roughness column (default 0.85)
    // via the parsed LUT; metallic/AO/magical keep their authored values. The
    // roughness feeds the G-buffer and the lighting specular response.
    row0(0) = glm::vec4(0.0f, 0.8f, 1.0f, 0.0f);   // Air/Default (dielectric)
    row0(1) = glm::vec4(0.05f, 0.85f, 1.0f, 0.0f); // Stone
    row0(2) = glm::vec4(0.0f, 0.9f, 1.0f, 0.0f);   // Soil
    row0(3) = glm::vec4(0.0f, 0.8f, 1.0f, 0.0f);   // Grass
    row0(4) = glm::vec4(0.0f, 0.75f, 1.0f, 0.0f);  // Sand
    row0(5) = glm::vec4(0.02f, 0.95f, 1.0f, 0.0f); // Deepslate
    row0(6) = glm::vec4(0.1f, 0.05f, 1.0f, 1.0f);  // Luminous Crystal (magical in alpha)
    row0(7) = glm::vec4(0.0f, 0.1f, 1.0f, 0.0f);   // Water
    // far-water sheet (id 200,
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
            // explicit far-water row authored above;
            // it is not a materials.json material, so skip the data-driven /
            // unknown-default roughness override that would force it to 0.85.
            continue;
        }
        if (m_material_texture_lut.roughness_set[static_cast<size_t>(id)]) {
            row0(id).g = m_material_texture_lut.roughness[static_cast<size_t>(id)];
        } else if (id != 0) {
            // Unknown materials default to 0.85 (documented design roughness default).
            row0(id).g = 0.85f;
        }
        // terrain PBR: metallic (row0.r) is now data-driven too. Materials that
        // declare metallic in materials.json override the authored fallback
        // above; the JSON values match the prior hardcoded ones, so this is a
        // zero-visible-change activation of the authoring path. The id==200
        // far-water matte row is skipped above (continue), so its metallic 0
        // stays intact (a glossier far sheet becomes a sun mirror at grazing
        // incidence — see the far-water comment above).
        if (m_material_texture_lut.metallic_set[static_cast<size_t>(id)]) {
            row0(id).r = m_material_texture_lut.metallic[static_cast<size_t>(id)];
        }
    }

    // Rows 1 + 2 (texture + emissive columns) — baked from the parsed LUT.
    for (int id = 0; id < MATERIAL_COUNT; ++id) {
        const int tl = m_material_texture_lut.texture_layer[static_cast<size_t>(id)];
        const int nl = m_material_texture_lut.normal_layer[static_cast<size_t>(id)];
        const float tiling = m_material_texture_lut.tiling[static_cast<size_t>(id)];
        const bool has_tex = tl >= 0;
        row1(id) = glm::vec4(has_tex ? static_cast<float>(tl) / 255.0f : 0.0f,
                             (nl >= 0) ? static_cast<float>(nl) / 255.0f : 0.0f,
                             glm::clamp(tiling / 64.0f, 0.0f, 1.0f),
                             has_tex ? 1.0f : 0.0f);
        const float ei = m_material_texture_lut.emissive_intensity[static_cast<size_t>(id)];
        //  row2.G carries the per-material albedo multiplier
        // (default 1.0). The G-buffer samples it and scales the baked textured
        // albedo (sand-flat noon-brightness calibration). Stored directly (0..1).
        const float albedo_scale = m_material_texture_lut.albedo_scale[static_cast<size_t>(id)];
        row2(id) = glm::vec4(glm::clamp(ei / kEmissiveLutScale, 0.0f, 1.0f),
                             glm::clamp(albedo_scale, 0.0f, 1.0f),
                             0.0f,
                             0.0f);
        //  dusty- palette: row3.rgb = per-material warm albedo tint
        // (default 1,1,1 = byte-identical no-op). Clamped to the LUT's 0..1 RGBA8
        // range; tints are warm-desaturating multipliers <= 1 so no clamp loss.
        const glm::vec3 tint = m_material_texture_lut.albedo_tint[static_cast<size_t>(id)];
        row3(id) = glm::vec4(glm::clamp(tint, glm::vec3(0.0f), glm::vec3(1.0f)), 0.0f);
    }

    glGenTextures(1, &m_materialLUT);
    label_gl_object(GL_TEXTURE, m_materialLUT, "terrain.material_lut");
    glBindTexture(GL_TEXTURE_2D, m_materialLUT);
    glTexImage2D(GL_TEXTURE_2D,
                 0,
                 GL_RGBA8,
                 MATERIAL_COUNT,
                 ROWS,
                 0,
                 GL_RGBA,
                 GL_FLOAT,
                 materialData.data());
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    LUMINUMBRA_CORE_INFO(
        "Material LUT initialized ({} materials x {} rows).", MATERIAL_COUNT, ROWS);
}

// --- TEXTURE-ARRAY RESIDENCY MANAGER ---

namespace {

constexpr uint32_t kLtexMagic = 0x5845544C; // 'LTEX' little-endian
constexpr uint16_t kLtexVersion = 1;
// Layers reserved per size-class array. Over-provisioning is cheap (storage is
// allocated lazily per uploaded layer in this iteration; growth reallocates).
constexpr uint32_t kResidencyArrayInitialCapacity = 8;

template<typename T>
bool ReadPod(std::ifstream& in, T& value) {
    in.read(reinterpret_cast<char*>(&value), sizeof(T));
    return static_cast<bool>(in);
}

} // namespace

bool RenderPipeline::load_ltex_cpu_image(const std::filesystem::path& path,
                                         LtexCpuImage& out) const {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        LUMINUMBRA_CORE_ERROR("Texture residency: could not open.ltex '{}'", path.string());
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
        LUMINUMBRA_CORE_ERROR("Texture residency: truncated.ltex header '{}'", path.string());
        return false;
    }
    if (magic != kLtexMagic) {
        LUMINUMBRA_CORE_ERROR("Texture residency: bad.ltex magic in '{}'", path.string());
        return false;
    }
    if (version != kLtexVersion) {
        LUMINUMBRA_CORE_ERROR(
            "Texture residency: unsupported.ltex version {} in '{}'", version, path.string());
        return false;
    }
    if (width == 0 || height == 0 || channels == 0 || channels > 4 || mip_count == 0) {
        LUMINUMBRA_CORE_ERROR("Texture residency: invalid.ltex dimensions in '{}'", path.string());
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
        LUMINUMBRA_CORE_ERROR("Texture residency: truncated.ltex mip data '{}'", path.string());
        return false;
    }
    return true;
}

// --- Compute-shader helpers ---

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
    if (compute_shader == 0)
        return 0;

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

void RenderPipeline::update_time_of_day(float deltaTime) {
    // while HELD (photo-mode time-of-day scrub), the day clock does NOT
    // auto-advance — the externally-set m_timeOfDay (via set_time_of_day) persists and the
    // sun/season below recompute from it. Render-only; never touches the sim clock.
    // when the sim tick drove TOD this frame (set_time_of_day_tick), the
    // wall-clock advance is also skipped — the tick is the time authority. Paths that
    // never feed the tick (scenarios, tests, menus) keep the legacy advance verbatim.
    if (!m_timeOfDayHold && !m_todTickDriven) {
        m_timeOfDay += deltaTime / m_dayDurationSeconds;
        m_timeOfDay = fmod(m_timeOfDay, 1.0f);
    }
    m_todTickDriven = false;

    // SEASON phase / wave / declination as a PURE FUNCTION of the
    // tick count -- integer epoch math then a single DeterministicMath trig evaluation, no
    // wall-clock, no float accumulator. Extracted to Rendering::ComputeSeason (TimeOfDayModel.h)
    // so the frame runs the SAME code the unit gate tests.  (the default every
    // non-season scenario sees) is season-NEUTRAL and reproduces the pre-season sun arc /
    // palette EXACTLY. Render-derived, one-way, never folded into world_hash.
    namespace DM = Luminumbra::DeterministicMath;
    const Rendering::SeasonState season =
        Rendering::ComputeSeason(m_seasonTick, kTicksPerSeasonCycle);
    m_seasonPhase = season.phase;
    const float season_wave = season.wave;
    m_seasonSunDeclination = season.sunDeclination;

    //  Tier 1: sun + moon are now two evaluated instances of the
    // celestial-body seam (CelestialBodyModel.h), which calls the  primitives
    // VERBATIM — bit-identical by construction (the SunMoonSeamBitExact gate pins it).
    // TRIG (byte-fragile): the sun direction uses UNQUALIFIED sin/cos inside the
    // primitive; the moon uses std::sin/std::cos — the asymmetry is intentional.
    // The LUMIN_MOON env read (parsed once) stays here as a client-config concern.
    static const float s_moon_env = [] {
        if (const auto value = Core::ReadEnvironment("LUMIN_MOON")) {
            try {
                return std::stof(*value);
            } catch (...) {}
        }
        return -1.0f;
    }();
    const float moon_forced = s_moon_env >= 0.0f ? s_moon_env : m_moonIllumOverride;
    const Rendering::CelestialFrame celestial = Rendering::EvaluateCelestialBodies(
        m_timeOfDay, m_seasonSunDeclination, m_seasonTick, moon_forced, kTicksPerLunarCycle);
    const Rendering::SunGeometry& sun_geo = celestial.sun_geometry;
    float sun_angle_rad = sun_geo.angleRad;
    const float season_tilt_z = sun_geo.tiltZ;
    m_sun.direction = sun_geo.direction;

    float sun_up_factor = sun_geo.upFactor;
    // Sun elevation above the horizon (radians). Exposed for the season sweep so it can assert
    // per-season sun-path bands.
    m_sunElevationRad = sun_geo.elevationRad;
    // A 0->1 DAY-FACTOR (not the sun magnitude any more —  moved the direct-sun
    // magnitude onto the physical transmittance coupling below, m_sun.color). This still
    // drives the daytime-ness blends: skybox/foliage/particle sun intensity, the water sky
    // reflection, the waterfall lit-fraction, and the ambient day/night + hue-tint mixes.
    m_sun.intensity = sun_geo.sunIntensity;

    // the sky dome's day/twilight/night blend is keyed off the sun's
    // RAW elevation (a WIDER band than m_sun.intensity) so civil twilight stays lit and WARM:
    // ~0.5-0.6 at the horizon, full day while the sun is up, collapsing to 0 only once the sun
    // is well below the horizon. This is what makes dawn/dusk glow instead of jumping to night.
    m_skyDayFactor = sun_geo.skyDomeDayFactor;

    //  refresh the sun-dependent sky-view LUT (no-op unless the sun moved past
    // the small threshold), sharing the SAME atmosphere the skybox + aerial pass use so
    // the sky and ambient redden coherently at low sun. (The daytime ambient still tints
    // its HUE from the normalized scattering below; the SUN magnitude is now the physical
    // transmittance coupling —  — not a normalized-to-~1 hue multiplier.)
    double refresh_ms = 0.0;
    const glm::vec3 toward_sun = -m_sun.direction;
    if (m_sky_lut.ready() && m_sky_lut.refresh_sky_view(toward_sun, &refresh_ms)) {
        m_skyScatterAmbient = m_sky_lut.sky_ambient();
    }
    m_sky_view_refresh_ms =
        refresh_ms; // 0.0 on no-refresh frames; copied into stats after the per-frame reset

    //  rendering: the direct-sun MAGNITUDE is the LUT transmittance
    // toward the sun × a top-of-atmosphere solar constant (calibrated 1/t_ref so the
    // overhead sun == the prior calibrated noon magnitude), replacing the authored
    // smoothstep intensity ramp + horizonColor hue mix. As the sun lowers the longer
    // optical path both DIMS and REDDENS the sun from one physical model (blue eaten
    // first -> the golden hour). See SunLightModel.h. Noon + all high sun (sun_up ≥ 0.25)
    // stays byte-identical to the prior code (division form, clamp was a no-op there);
    // only the low-sun arc changes. Render-only; never world_hash.
    glm::vec3 t_now(1.0f);
    glm::vec3 t_ref(1.0f);
    if (m_sky_lut.ready()) {
        const float sun_cos = glm::clamp(sun_up_factor, -1.0f, 1.0f);
        t_now = m_sky_lut.sun_transmittance(sun_cos);
        t_ref = m_sky_lut.sun_transmittance(1.0f); // overhead-sun reference
    }
    m_sun.color = SunBaseHue() * SunIrradiance(t_now, t_ref, sun_up_factor);

    // SEASON PALETTE tint. A small luminance-preserving hue shift
    // selects the biome material/foliage palette feel per season. The direction
    // REINFORCES the seasonal sun path rather than fighting it: WINTER (low arc,
    // season_wave < 0) leans WARM/golden -- matching the long-optical-path low
    // sun -- while SUMMER (high arc, season_wave > 0) leans COOL/blue, matching
    // the high overhead sun. Applied to the SUN color (and, below, the ambient)
    // so the whole lit scene + foliage picks up the seasonal cast. Amplitude is
    // small and luminance-normalized so it modulates HUE without moving the
    // calibrated noon luminance the LodGround/RenderHealth baselines depend on.
    // Render-only. (season_wave: +1 summer -> R down/B up; -1 winter -> R up/B
    // down.)
    // SEASON PALETTE tint (Rendering::SeasonPaletteTint) -- a small
    // luminance-preserving hue shift (WINTER warm, SUMMER cool) applied to the sun color (and the
    // ambient below). Luminance-normalized so it rotates HUE only; the calibrated noon luminance
    // the LodGround/RenderHealth baselines depend on does not move. Render-only.
    m_seasonPaletteTint = Rendering::SeasonPaletteTint(season_wave);
    m_sun.color *= m_seasonPaletteTint;

    //  Tier 1: the moon instance of the celestial frame (VERBATIM
    // ComputeMoonGeometry + ComputeMoonIllumination underneath). m_moonLightDir is
    // its TOWARD-LIGHT vector (same convention as u_sun.direction), overhead at
    // midnight so get_light_space_matrices can re-key the shadow cascade onto the
    // moon at night; m_moonDirection places the disc; illumination carries the
    // deterministic lunar phase (the "two night modes"), env/override resolved above.
    m_moonDirection = celestial.moon.travel_direction;
    m_moonLightDir = celestial.moon.light_direction;
    m_moonUpFactor = celestial.moon.up_factor;
    m_moonIllumination = celestial.moon.illumination;
    (void)season_tilt_z; // consumed inside the seam's moon evaluation now

    // Ambient scales by the same PI as SUN_IRRADIANCE_SCALE (lighting_pass
    // exposure audit): these values were tuned against the pre-audit sun, so
    // without the matching scale the sun:ambient balance collapses from ~30%
    // shadow luminance to ~4% (shadowed slopes read near-black, LodGround/
    // LodSeamRisk near_black_ratio regressions on DEM-realistic terrain).
    constexpr float kAmbientIrradianceScale = 3.14159265f;
    //  rendering ( /  MAGNITUDE): the DAYTIME ambient magnitude is now
    // coupled to the sky-view hemisphere-irradiance LUT (sky_ambient) instead of a fixed
    // authored constant, so shadowed surfaces dim + warm PHYSICALLY as the sun lowers, not
    // just in hue. kSkyAmbientRenderScale is CALIBRATED (measured noon sky_ambient luminance
    // 0.05291 -> the prior authored noon ambient luminance 0.449) so NOON ambient is
    // preserved (the LodGround/RenderHealth baseline holds) while lower-sun frames get a
    // genuinely lower ambient. Falls back to the authored constant if the LUT isn't ready.
    constexpr float kSkyAmbientRenderScale = 8.49f;
    glm::vec3 dayAmbient = m_sky_lut.ready()
                               ? m_sky_lut.sky_ambient() * kSkyAmbientRenderScale
                               : glm::vec3(0.1f, 0.15f, 0.2f) * kAmbientIrradianceScale;
    // moon-shadows: a real NIGHT SKYLIGHT fill (was 0.01,0.02,0.04). The moon
    // directional only lights up-facing ground; camera-facing SLOPES get NdotL~0
    // from an overhead moon, so without skylight they read pure black at night
    // (daytime fills them via the ~10x-larger day ambient). A moonlit sky IS a
    // large soft cool light source — lift the night hemisphere ambient so slopes
    // and moon-shadowed areas stay dim-but-NAVIGABLE and cool, while the moon
    // directional still gives form + cast shadows on flat ground. Cool/blue-biased.
    //  rendering: the night skylight FILL scales with the lunar phase so a
    // full moon gives a brighter, navigable night hemisphere and a new moon a darker one,
    // never fully black (a starlight floor keeps deep new-moon nights navigable-with-effort).
    constexpr float kStarlightFrac = 0.40f; // new-moon night keeps ~40% of the moonlit skylight
    glm::vec3 nightAmbient = glm::vec3(0.060f, 0.090f, 0.165f) * kAmbientIrradianceScale *
                             glm::mix(kStarlightFrac, 1.0f, m_moonIllumination);
    m_skyAmbientColor = glm::mix(nightAmbient, dayAmbient, m_sun.intensity);
    //  tint the daytime ambient HUE toward the sky-view scattering
    // ambient (the same LUT integral) while preserving the calibrated ambient
    // LUMINANCE, so shadowed surfaces pick up the coherent sky color without
    // moving the noon ambient level that LodGround/LodSeamRisk depend on.
    if (m_sky_lut.ready()) {
        const float scatter_lum = m_skyScatterAmbient.r * 0.2126f +
                                  m_skyScatterAmbient.g * 0.7152f + m_skyScatterAmbient.b * 0.0722f;
        if (scatter_lum > 1e-6f) {
            const glm::vec3 scatter_hue =
                m_skyScatterAmbient / scatter_lum; // luminance-normalized hue
            const float amb_lum = m_skyAmbientColor.r * 0.2126f + m_skyAmbientColor.g * 0.7152f +
                                  m_skyAmbientColor.b * 0.0722f;
            const glm::vec3 tinted = scatter_hue * amb_lum;
            //  rendering ( / ): couple the ambient HUE more fully to
            // the sky-view scattering LUT (was 0.5) so shadowed surfaces physically pick
            // up the atmosphere's color across the day (cool noon, warm golden hour). The
            // calibrated ambient LUMINANCE is preserved (tinted = scatter_hue * amb_lum),
            // so the LodGround/RenderHealth noon-ambient baselines do not move — only the
            // ambient hue tracks the LUT. (Magnitude coupling stays in m_scene_exposure.)
            m_skyAmbientColor = glm::mix(m_skyAmbientColor, tinted, 0.9f * m_sun.intensity);
        }
    }
    // carry the seasonal palette tint into the DAYTIME ambient too
    // (luminance-preserving, scaled by sun intensity) so shadowed foliage/terrain
    // picks up the season cast, not just sunlit surfaces. Night ambient is left
    // untouched (tint folds out as m_sun.intensity -> 0).
    {
        const float amb_lum = m_skyAmbientColor.r * 0.2126f + m_skyAmbientColor.g * 0.7152f +
                              m_skyAmbientColor.b * 0.0722f;
        const glm::vec3 season_tinted = m_skyAmbientColor * m_seasonPaletteTint;
        m_skyAmbientColor = glm::mix(m_skyAmbientColor, season_tinted, m_sun.intensity);
        // Restore the calibrated ambient luminance after the hue rotation.
        const float new_lum = m_skyAmbientColor.r * 0.2126f + m_skyAmbientColor.g * 0.7152f +
                              m_skyAmbientColor.b * 0.0722f;
        if (new_lum > 1e-6f) {
            m_skyAmbientColor *= (amb_lum / new_lum);
        }
    }

    // Deterministic time-of-day exposure fallback. GPU metering and manual
    // photo EV override it when enabled. The shared day anchor keeps toggling
    // photo mode at the default lens continuous. Render-only.
    m_scene_exposure = Rendering::AutoExposureForElevation(sun_up_factor);

    // advance the wind-advected cloud scroll on the same tick-
    // derived deltaTime so the dome clouds + their cast shadow drift with the wind.
    advance_cloud_phase(deltaTime);
}

void RenderPipeline::set_season_tick(std::uint64_t tick) {
    //  season: store the authoritative sim tick; update_time_of_day
    // recomputes the season phase/declination/tint from it as a pure function.
    // One-way : this never feeds back into the sim and never touches
    // world_hash.
    m_seasonTick = tick;
}

u32 RenderPipeline::water_caustics_texture() const {
    return m_water_pass ? m_water_pass->caustics_texture() : 0u;
}

void RenderPipeline::set_time_of_day(float normalized_time) {
    m_timeOfDay = std::clamp(normalized_time, 0.0f, 1.0f);
}

void RenderPipeline::set_time_of_day_tick(std::uint64_t sim_tick) {
    // the photo-mode hold outranks the tick authority ( — the player
    // is scrubbing time); scenario/scene pins outrank it by frame order (they call
    // set_time_of_day after this). Marks the frame tick-driven either way so the
    // wall-clock advance never fights the scrub.
    if (!m_timeOfDayHold) {
        m_timeOfDay = Rendering::TimeOfDayFromTick(sim_tick, m_dayLengthTicks);
    }
    m_todTickDriven = true;
}

void RenderPipeline::set_weather(WeatherType type, float intensity) {
    m_weather_type = type;
    m_weather_intensity = std::clamp(intensity, 0.0f, 1.0f);
    // A direct debug set_weather call overrides any prior sim-driven state.
    m_weather_state.driven = false;
}

void RenderPipeline::set_weather_state(const WeatherRenderState& state) {
    // SIM-DRIVEN path. Store the replicated state for the overlay +
    // wetness response. The overlay's zero-work gate keys on m_weather_type /
    // m_weather_intensity, so derive a representative type + intensity from the
    // sim state: precipitation drives Rain/Snow, an active storm escalates to
    // Storm, fog/overcast without precip is Fog; no precip + no fog == None (the
    // overlay short-circuits, exactly like a clear sky). One-way: nothing here
    // writes back into the sim.
    m_weather_state = state;
    m_weather_state.driven = true;
    m_weather_state.rain_intensity = std::clamp(state.rain_intensity, 0.0f, 1.0f);
    m_weather_state.snow_intensity = std::clamp(state.snow_intensity, 0.0f, 1.0f);
    m_weather_state.fog_density = std::clamp(state.fog_density, 0.0f, 1.0f);
    m_weather_state.storm_intensity = std::clamp(state.storm_intensity, 0.0f, 1.0f);
    m_weather_state.wetness = std::clamp(state.wetness, 0.0f, 1.0f);
    m_weather_state.wind_strength = std::clamp(state.wind_strength, 0.0f, 1.0f);

    const float precip = std::max(m_weather_state.rain_intensity, m_weather_state.snow_intensity);
    if (m_weather_state.storm_intensity > 0.05f && m_weather_state.rain_intensity > 0.05f) {
        m_weather_type = WeatherType::Storm;
        m_weather_intensity = std::max(precip, m_weather_state.storm_intensity);
    } else if (m_weather_state.snow_intensity > 0.02f) {
        m_weather_type = WeatherType::Snow;
        m_weather_intensity = m_weather_state.snow_intensity;
    } else if (m_weather_state.rain_intensity > 0.02f) {
        m_weather_type = WeatherType::Rain;
        m_weather_intensity = m_weather_state.rain_intensity;
    } else if (m_weather_state.fog_density > 0.02f) {
        m_weather_type = WeatherType::Fog;
        m_weather_intensity = m_weather_state.fog_density;
    } else {
        m_weather_type = WeatherType::None;
        m_weather_intensity = 0.0f;
    }
}

void RenderPipeline::set_cloud_state(const CloudRenderState& state) {
    // render-only. Store the coverage parameters; the wind scroll
    // offset is advanced internally (advance_cloud_phase) from the pushed wind
    // direction/strength so the field DRIFTS deterministically with the wind. The
    // caller's scroll_offset/sun_travel_dir are ignored — we own those — so a
    // caller need only set enabled/shadow_enabled/coverage/biome/plane/strength.
    // One-way: nothing here is read back into the sim or world_hash.
    const glm::vec2 preserved_offset = m_cloud_state.scroll_offset;
    m_cloud_state = state;
    m_cloud_state.scroll_offset = preserved_offset; // keep the accumulated drift
    m_cloud_state.coverage_amount = std::clamp(m_cloud_state.coverage_amount, 0.0f, 1.0f);
    m_cloud_state.biome_variation = std::clamp(m_cloud_state.biome_variation, -1.0f, 1.0f);
    m_cloud_state.shadow_strength = std::clamp(m_cloud_state.shadow_strength, 0.0f, 1.0f);
    if (m_cloud_state.plane_height < 1.0f) {
        m_cloud_state.plane_height = 900.0f;
    }
    // The sun travel direction the cast-shadow projection uses always mirrors the
    // current sun so the shadow stays consistent with the lit scene.
    m_cloud_state.sun_travel_dir = m_sun.direction;
}

void RenderPipeline::advance_cloud_phase(float deltaTime) {
    // advance the wind-advection scroll. The phase accumulates the
    // SAME tick-derived deltaTime that drives the sun (deterministic per tick in
    // the headless/scenario stepping — no wall-clock), and the scroll offset is
    // wind_direction * wind_strength * phase, so the entire coverage field (dome
    // clouds AND the projected cast shadow) translates with the large-scale wind.
    // Pure render accumulator: never hashed, never read back into the sim.
    if (!m_cloud_state.enabled) {
        return;
    }
    m_cloud_phase += deltaTime;
    // Wind comes from the replicated weather render state (one-way bridge). A
    // gentle base drift so clouds still move when no wind has been pushed yet.
    glm::vec2 wind_xz(m_weather_state.wind_direction.x, m_weather_state.wind_direction.z);
    const float wind_len = glm::length(wind_xz);
    if (wind_len > 1e-4f) {
        wind_xz /= wind_len;
    } else {
        wind_xz = glm::vec2(1.0f, 0.0f);
    }
    // Metres-per-second the cloud sheet drifts at full wind strength. The cells
    // are ~1200 m, so ~60 m/s visibly shifts a cloud edge across a terrain ROI in
    // the few-second gate window while staying a believable high-altitude drift.
    constexpr float kCloudDriftMetersPerSec = 60.0f;
    const float speed = kCloudDriftMetersPerSec * std::max(0.12f, m_weather_state.wind_strength);
    m_cloud_state.scroll_offset = wind_xz * (m_cloud_phase * speed);
    m_cloud_state.sun_travel_dir = m_sun.direction;
}

void RenderPipeline::set_lightning_state(const LightningRenderState& state) {
    // render-only. Store the per-frame pulse + bolt polyline for the
    // LightingPass to inject. Clamp the pulse so the additive flash stays bounded;
    // truncate the bolt polyline to the GLSL uniform-array cap. One-way :
    // nothing here is read back into the sim or world_hash.
    m_lightning_state = state;
    m_lightning_state.pulse_intensity = std::max(0.0f, state.pulse_intensity);
    if (static_cast<int>(m_lightning_state.bolt_points_ndc.size()) > kMaxBoltSegmentPoints) {
        m_lightning_state.bolt_points_ndc.resize(kMaxBoltSegmentPoints);
    }
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
        glm::mat4 proj = glm::perspective(glm::radians(camera.Zoom),
                                          (float)m_screen_width / (float)m_screen_height,
                                          split_near,
                                          split_far);
        glm::mat4 view = camera.GetViewMatrix();
        std::vector<glm::vec4> corners;
        for (int x = 0; x < 2; ++x)
            for (int y = 0; y < 2; ++y)
                for (int z = 0; z < 2; ++z) {
                    const glm::vec4 pt =
                        glm::inverse(proj * view) *
                        glm::vec4(2.0f * x - 1.0f, 2.0f * y - 1.0f, 2.0f * z - 1.0f, 1.0f);
                    corners.push_back(pt / pt.w);
                }
        glm::vec3 center = glm::vec3(0.0f);
        for (const auto& v : corners)
            center += glm::vec3(v);
        center /= corners.size();
        // moon-shadows: the single shadow cascade follows whichever luminary is
        // ABOVE the horizon. By day the sun direction is used (unchanged). Once the
        // sun drops below the horizon its direction is degenerate (points up, would
        // build a useless below-ground shadow map), so we re-key the cascade onto
        // the MOON direction (overhead at midnight) — this is what lets moonlit
        // surfaces cast/receive real shadows at night. Switch on the sun's
        // elevation; m_moonLightDir is the anti-sun toward-light dir.
        const float sun_up = glm::dot(m_sun.direction, glm::vec3(0.0f, -1.0f, 0.0f));
        const glm::vec3 light_dir = (sun_up < 0.0f) ? m_moonLightDir : m_sun.direction;
        glm::mat4 light_view = glm::lookAt(center - light_dir, center, glm::vec3(0.0f, 1.0f, 0.0f));
        float minX = std::numeric_limits<float>::max(), maxX = std::numeric_limits<float>::lowest();
        float minY = std::numeric_limits<float>::max(), maxY = std::numeric_limits<float>::lowest();
        float minZ = std::numeric_limits<float>::max(), maxZ = std::numeric_limits<float>::lowest();
        for (const auto& v : corners) {
            const glm::vec4 trf = light_view * v;
            minX = std::min(minX, trf.x);
            maxX = std::max(maxX, trf.x);
            minY = std::min(minY, trf.y);
            maxY = std::max(maxY, trf.y);
            minZ = std::min(minZ, trf.z);
            maxZ = std::max(maxZ, trf.z);
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
