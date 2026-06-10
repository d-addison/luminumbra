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
#include <stb_image.h>

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

GLuint make_solid_rgba_texture(const unsigned char rgba[4], const std::string& label) {
    GLuint texture = 0;
    glGenTextures(1, &texture);
    label_gl_object(GL_TEXTURE, texture, label);
    glBindTexture(GL_TEXTURE_2D, texture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
    glBindTexture(GL_TEXTURE_2D, 0);
    return texture;
}

} // namespace

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

RenderPipeline::RenderPipeline() {}
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
        set_default_shadow_cascade_splits(m_shadow_map);

        init_shaders();
        init_lighting_fbo(screen_width, screen_height);
        init_gbuffer(screen_width, screen_height);
        init_shadow_map();
        init_ssao();
        init_screen_quad();
        init_skybox();
        init_terrain_textures();
        init_material_lut();
        init_water_fallback_textures();
        init_gpu_sdf_system();
        init_gpu_pass_timers();

        std::string instanced_vert_path = (m_root_path / "res/shaders/instanced_mesh.vert").string();
        std::string gbuffer_frag_path = (m_root_path / "res/shaders/g_buffer.frag").string();
        m_instanced_static_mesh_shader = std::make_unique<Shader>(instanced_vert_path.c_str(), gbuffer_frag_path.c_str());
        label_gl_object(GL_PROGRAM, m_instanced_static_mesh_shader ? m_instanced_static_mesh_shader->Id() : 0u, "shader.instanced_static_mesh");
        glGenBuffers(1, &m_instanceMatrixVBO);
        label_gl_object(GL_BUFFER, m_instanceMatrixVBO, "static_mesh.instance_matrices");
        glBindBuffer(GL_ARRAY_BUFFER, m_instanceMatrixVBO);
        glBufferData(GL_ARRAY_BUFFER, 10000 * sizeof(glm::mat4), nullptr, GL_DYNAMIC_DRAW);
        glBindBuffer(GL_ARRAY_BUFFER, 0);

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
    if (m_lighting_fbo.color_texture) estimated_vram_bytes += pixel_count * 8u; // RGBA16F
    if (m_lighting_fbo.opaque_color_texture) estimated_vram_bytes += pixel_count * 8u; // RGBA16F
    if (m_lighting_fbo.depth_texture) estimated_vram_bytes += pixel_count * 4u;
    if (m_gbuffer.position_texture) estimated_vram_bytes += pixel_count * 6u; // RGB16F
    if (m_gbuffer.normal_texture) estimated_vram_bytes += pixel_count * 4u;
    if (m_gbuffer.albedo_texture) estimated_vram_bytes += pixel_count * 4u;
    if (m_gbuffer.material_texture) estimated_vram_bytes += pixel_count * 4u; // RG16F
    if (m_gbuffer.depth_texture) estimated_vram_bytes += pixel_count * 4u;
    if (m_shadow_map.depth_texture_array) {
        estimated_vram_bytes += static_cast<size_t>(m_shadow_map.resolution) *
                                static_cast<size_t>(m_shadow_map.resolution) *
                                static_cast<size_t>(ShadowMap::CASCADE_COUNT) * 4u;
    }
    if (m_ssao.ssaoColorBuffer) estimated_vram_bytes += pixel_count * 2u;
    if (m_ssao.ssaoColorBufferBlur) estimated_vram_bytes += pixel_count * 2u;
    if (m_ssao.noiseTexture) estimated_vram_bytes += 4u * 4u * 6u;
    if (m_screen_quad_vbo) estimated_vram_bytes += 20u * sizeof(float);
    if (m_skybox_vbo) estimated_vram_bytes += 108u * sizeof(float);
    if (m_instanceMatrixVBO) estimated_vram_bytes += 10000u * sizeof(glm::mat4);
    if (m_terrainTextureArray) estimated_vram_bytes += 2048u * 2048u * 5u * 4u;
    if (m_materialLUT) estimated_vram_bytes += 256u * 4u;
    if (m_water_flat_normal_texture) estimated_vram_bytes += 4u;
    if (m_water_neutral_flow_texture) estimated_vram_bytes += 4u;
    if (m_water_black_texture) estimated_vram_bytes += 4u;
    if (m_water_underwater_texture) estimated_vram_bytes += 4u;
    if (m_gpu_sdf.sdf_buffer) estimated_vram_bytes += 17u * 17u * 17u * sizeof(float);
    if (m_gpu_sdf.terrain_noise_texture) estimated_vram_bytes += 128u * 128u * 128u * sizeof(float);
    if (m_gpu_sdf.cave_noise_texture) estimated_vram_bytes += 128u * 128u * 128u * sizeof(float);
    if (m_gpu_sdf.island_mask_texture) estimated_vram_bytes += 128u * 128u * sizeof(float);
    stats.estimated_vram_bytes = estimated_vram_bytes;

    stats.geometry_shader_ok = m_geometry_shader && m_geometry_shader->IsValid();
    stats.lighting_shader_ok = m_lighting_shader && m_lighting_shader->IsValid();
    stats.skybox_shader_ok = m_skybox_shader && m_skybox_shader->IsValid();
    stats.shadow_shader_ok = m_shadow_shader && m_shadow_shader->IsValid();
    stats.ssao_shader_ok = m_ssao.ssaoShader && m_ssao.ssaoShader->IsValid();
    stats.ssao_blur_shader_ok = m_ssao.blurShader && m_ssao.blurShader->IsValid();
    stats.water_shader_ok = m_water_shader && m_water_shader->IsValid();
    stats.instanced_static_mesh_shader_ok = m_instanced_static_mesh_shader && m_instanced_static_mesh_shader->IsValid();
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

    add_shader("geometry", m_geometry_shader);
    add_shader("lighting", m_lighting_shader);
    add_shader("skybox", m_skybox_shader);
    add_shader("shadow", m_shadow_shader);
    add_shader("ssao", m_ssao.ssaoShader);
    add_shader("ssao_blur", m_ssao.blurShader);
    add_shader("water", m_water_shader);
    add_shader("instanced_static_mesh", m_instanced_static_mesh_shader);
    health.push_back({"gpu_sdf_compute", m_gpu_sdf.compute_program != 0, m_gpu_sdf.compute_program != 0 ? "" : "not initialized"});
    return health;
}

RenderPipeline::RenderResourceRegistryStats RenderPipeline::get_resource_registry_stats() const {
    RenderResourceRegistryStats stats;
    auto count = [](GLuint id) -> size_t { return id != 0 ? 1u : 0u; };

    stats.framebuffers += count(m_lighting_fbo.fbo_id);
    stats.framebuffers += count(m_gbuffer.fbo_id);
    stats.framebuffers += count(m_shadow_map.fbo_id);
    stats.framebuffers += count(m_ssao.fbo);
    stats.framebuffers += count(m_ssao.blurFBO);

    stats.textures += count(m_lighting_fbo.color_texture);
    stats.textures += count(m_lighting_fbo.opaque_color_texture);
    stats.textures += count(m_gbuffer.position_texture);
    stats.textures += count(m_gbuffer.normal_texture);
    stats.textures += count(m_gbuffer.albedo_texture);
    stats.textures += count(m_gbuffer.material_texture);
    stats.textures += count(m_gbuffer.depth_texture);
    stats.textures += count(m_shadow_map.depth_texture_array);
    stats.textures += count(m_ssao.ssaoColorBuffer);
    stats.textures += count(m_ssao.ssaoColorBufferBlur);
    stats.textures += count(m_ssao.noiseTexture);
    stats.textures += count(m_terrainTextureArray);
    stats.textures += count(m_materialLUT);
    stats.textures += count(m_water_flat_normal_texture);
    stats.textures += count(m_water_neutral_flow_texture);
    stats.textures += count(m_water_black_texture);
    stats.textures += count(m_water_underwater_texture);
    stats.textures += count(m_gpu_sdf.terrain_noise_texture);
    stats.textures += count(m_gpu_sdf.cave_noise_texture);
    stats.textures += count(m_gpu_sdf.island_mask_texture);

    stats.renderbuffers += count(m_lighting_fbo.depth_texture);
    stats.buffers += count(m_screen_quad_vbo);
    stats.buffers += count(m_skybox_vbo);
    stats.buffers += count(m_instanceMatrixVBO);
    stats.buffers += count(m_gpu_sdf.sdf_buffer);
    stats.vertex_arrays += count(m_screen_quad_vao);
    stats.vertex_arrays += count(m_skybox_vao);

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
    for (const auto& [id, data] : m_water_render_data) {
        (void)id;
        count_water_slot(data);
    }
    for (const auto& data : m_free_water_render_slots) {
        count_water_slot(data);
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

    add_pass("shadow", {"terrain_depth"}, {"shadow.depth_texture_array"}, m_shadow_map.resolution, m_shadow_map.resolution,
             "depth", "store depth cascades", m_last_render_pass_stats.shadow_draws);
    add_pass("gbuffer", {"terrain_meshes", "static_meshes", "material_lut"},
             {"gbuffer.position", "gbuffer.normal_material", "gbuffer.albedo_roughness", "gbuffer.metallic_ao", "gbuffer.depth"},
             m_screen_width, m_screen_height, "color+depth", "store deferred attachments", m_last_render_pass_stats.terrain_draws);
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
    shadow_pass(renderable_chunk_snapshots, camera);
    end_gpu_pass_timer(GpuTimerPass::Shadow);
    glViewport(0, 0, m_screen_width, m_screen_height);

    // 2. GEOMETRY / G-BUFFER PASS
    begin_gpu_pass_timer(GpuTimerPass::GBuffer);
    gbuffer_pass(registry, renderable_chunk_snapshots, camera, frustum_planes);
    end_gpu_pass_timer(GpuTimerPass::GBuffer);
    glBindVertexArray(0);  // Unbind after gbuffer pass
    glDisable(GL_CULL_FACE);

    // 3. SSAO PASS
    begin_gpu_pass_timer(GpuTimerPass::Ssao);
    ssao_pass(camera);
    end_gpu_pass_timer(GpuTimerPass::Ssao);
    glBindVertexArray(0);  // Unbind after SSAO
    begin_gpu_pass_timer(GpuTimerPass::SsaoBlur);
    ssao_blur_pass();
    end_gpu_pass_timer(GpuTimerPass::SsaoBlur);
    glBindVertexArray(0);  // Unbind after SSAO blur

    // 4. LIGHTING PASS (Renders to m_lighting_fbo)
    glEnable(GL_CULL_FACE);
    begin_gpu_pass_timer(GpuTimerPass::Lighting);
    lighting_pass(camera);
    end_gpu_pass_timer(GpuTimerPass::Lighting);
    glBindVertexArray(0);  // Unbind after lighting pass

    // 5. SNAPSHOT THE OPAQUE SCENE, THEN COPY DEPTH TO LIGHTING FBO FOR WATER DEPTH TEST
    copy_lighting_color_to_opaque_texture();
    glBindFramebuffer(GL_READ_FRAMEBUFFER, m_gbuffer.fbo_id);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, m_lighting_fbo.fbo_id);
    glBlitFramebuffer(0, 0, m_screen_width, m_screen_height, 0, 0, m_screen_width, m_screen_height, GL_DEPTH_BUFFER_BIT, GL_NEAREST);
    
    // 6. WATER PASS (Renders to m_lighting_fbo, reads from it for refraction)
    glBindFramebuffer(GL_FRAMEBUFFER, m_lighting_fbo.fbo_id);
    begin_gpu_pass_timer(GpuTimerPass::Water);
    water_pass(renderable_chunk_snapshots, camera);
    end_gpu_pass_timer(GpuTimerPass::Water);
    glBindVertexArray(0);  // Unbind after water pass

    // 7. SKYBOX PASS (Renders to m_lighting_fbo)
    begin_gpu_pass_timer(GpuTimerPass::Skybox);
    skybox_pass(camera);
    end_gpu_pass_timer(GpuTimerPass::Skybox);
    glBindVertexArray(0);  // Unbind after skybox pass

    // 8. FINAL BLIT TO SCREEN
    begin_gpu_pass_timer(GpuTimerPass::FinalBlit);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, m_lighting_fbo.fbo_id);
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

void RenderPipeline::gbuffer_pass(entt::registry& registry, const std::vector<ChunkMeshSnapshot>& renderable_chunks, const Camera& camera, const glm::vec4 frustum_planes[6]) {
    glBindFramebuffer(GL_FRAMEBUFFER, m_gbuffer.fbo_id);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    
    // Pass 1: Render all the terrain chunks
    geometry_pass_chunks(renderable_chunks, camera, frustum_planes);

    // Pass 2: Render all instanced static meshes
    geometry_pass_static_meshes(registry, camera, frustum_planes);

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

void RenderPipeline::init_lighting_fbo(u32 width, u32 height) {
    glGenFramebuffers(1, &m_lighting_fbo.fbo_id);
    label_gl_object(GL_FRAMEBUFFER, m_lighting_fbo.fbo_id, "lighting.fbo");
    glBindFramebuffer(GL_FRAMEBUFFER, m_lighting_fbo.fbo_id);

    // Color attachment (for the final lit scene)
    glGenTextures(1, &m_lighting_fbo.color_texture);
    label_gl_object(GL_TEXTURE, m_lighting_fbo.color_texture, "lighting.color");
    glBindTexture(GL_TEXTURE_2D, m_lighting_fbo.color_texture);
    // Use RGBA16F for HDR lighting to avoid clamping colors between 0 and 1
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, width, height, 0, GL_RGBA, GL_FLOAT, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, m_lighting_fbo.color_texture, 0);

    glGenTextures(1, &m_lighting_fbo.opaque_color_texture);
    label_gl_object(GL_TEXTURE, m_lighting_fbo.opaque_color_texture, "lighting.opaque_color_copy");
    glBindTexture(GL_TEXTURE_2D, m_lighting_fbo.opaque_color_texture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, width, height, 0, GL_RGBA, GL_FLOAT, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    // We will blit the depth from the G-Buffer later, so we only need a renderbuffer object for depth testing.
    // However, if you wanted to do post-processing on this FBO that needs depth, you would use a depth texture.
    glGenRenderbuffers(1, &m_lighting_fbo.depth_texture); // Note: this is a renderbuffer ID, not a texture ID
    label_gl_object(GL_RENDERBUFFER, m_lighting_fbo.depth_texture, "lighting.depth");
    glBindRenderbuffer(GL_RENDERBUFFER, m_lighting_fbo.depth_texture);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, width, height);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, m_lighting_fbo.depth_texture);

    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
        LUMINUMBRA_CORE_ERROR("Lighting FBO not complete!");

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

void RenderPipeline::destroy_lighting_fbo() {
    if (m_lighting_fbo.fbo_id) { glDeleteFramebuffers(1, &m_lighting_fbo.fbo_id); m_lighting_fbo.fbo_id = 0; }
    if (m_lighting_fbo.color_texture) { glDeleteTextures(1, &m_lighting_fbo.color_texture); m_lighting_fbo.color_texture = 0; }
    if (m_lighting_fbo.opaque_color_texture) { glDeleteTextures(1, &m_lighting_fbo.opaque_color_texture); m_lighting_fbo.opaque_color_texture = 0; }
    if (m_lighting_fbo.depth_texture) { glDeleteRenderbuffers(1, &m_lighting_fbo.depth_texture); m_lighting_fbo.depth_texture = 0; }
}

void RenderPipeline::copy_lighting_color_to_opaque_texture() {
    if (!m_lighting_fbo.fbo_id || !m_lighting_fbo.color_texture || !m_lighting_fbo.opaque_color_texture) {
        return;
    }

    glBindFramebuffer(GL_READ_FRAMEBUFFER, m_lighting_fbo.fbo_id);
    glReadBuffer(GL_COLOR_ATTACHMENT0);
    glBindTexture(GL_TEXTURE_2D, m_lighting_fbo.opaque_color_texture);
    glCopyTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 0, 0, m_screen_width, m_screen_height);
    glBindTexture(GL_TEXTURE_2D, 0);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
}

void RenderPipeline::water_pass(const std::vector<ChunkMeshSnapshot>& renderable_chunks, const Camera& camera) {
    // --- 1. Set OpenGL State ---
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glDepthMask(GL_FALSE);
    glDisable(GL_CULL_FACE);

    // --- 2. Activate Shader and Set Uniforms ---
    m_water_shader->use();

    glm::mat4 projection = glm::perspective(glm::radians(camera.Zoom), (float)m_screen_width / (float)m_screen_height, camera.GetNearPlane(), camera.GetFarPlane());
    glm::mat4 view = camera.GetViewMatrix();

    // Set matrices
    m_water_shader->setMat4("u_view", view);
    m_water_shader->setMat4("u_projection", projection);
    m_water_shader->setMat4("u_inverse_view", glm::inverse(view));
    m_water_shader->setMat4("u_inverse_projection", glm::inverse(projection));
    // <<< OPTIMIZATION: Set the new pre-combined matrix for the SSR loop
    m_water_shader->setMat4("u_view_projection", projection * view);
    
    // Set scene and material properties (as before)
    m_water_shader->setVec3("u_camera_pos", camera.Position);
    m_water_shader->setVec2("u_screen_size", glm::vec2(m_screen_width, m_screen_height));
    m_water_shader->setFloat("u_time", static_cast<float>(glfwGetTime()));
    m_water_shader->setVec3("u_sun_direction", m_sun.direction);
    m_water_shader->setVec3("u_sun_color", m_sun.color);
    m_water_shader->setVec3("u_shallow_color", glm::vec3(0.3, 0.8, 0.7));
    m_water_shader->setVec3("u_deep_color", glm::vec3(0.02, 0.18, 0.34));
    m_water_shader->setFloat("u_water_depth_scaler", 0.2f);
    m_water_shader->setFloat("u_reflection_power", 0.7f);

    // Bind textures (as before)
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, m_lighting_fbo.opaque_color_texture);
    m_water_shader->setInt("u_opaque_scene_color", 0);

    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, m_gbuffer.depth_texture);
    m_water_shader->setInt("u_opaque_depth", 1);

    glActiveTexture(GL_TEXTURE2);
    glBindTexture(GL_TEXTURE_2D, m_water_flat_normal_texture);
    m_water_shader->setInt("u_normal_map", 2);

    glActiveTexture(GL_TEXTURE3);
    glBindTexture(GL_TEXTURE_2D, m_water_neutral_flow_texture);
    m_water_shader->setInt("u_flow_map", 3);

    glActiveTexture(GL_TEXTURE4);
    glBindTexture(GL_TEXTURE_2D, m_water_black_texture);
    m_water_shader->setInt("u_caustics_texture", 4);

    glActiveTexture(GL_TEXTURE5);
    glBindTexture(GL_TEXTURE_2D, m_water_underwater_texture);
    m_water_shader->setInt("u_underwater_texture", 5);

    glActiveTexture(GL_TEXTURE6);
    glBindTexture(GL_TEXTURE_2D, m_water_black_texture);
    m_water_shader->setInt("u_foam_texture", 6);
    
    // --- 3. Draw Water Meshes ---
    for (const auto& chunk : renderable_chunks) {
        auto it = m_water_render_data.find(chunk.id);
        if (it != m_water_render_data.end() && it->second.element_count > 0) {
            const auto& render_data = it->second;
            
            glm::mat4 model = glm::translate(glm::mat4(1.0f), glm::vec3(chunk.coords * IVec3(CHUNK_SIZE_X, CHUNK_SIZE_Y, CHUNK_SIZE_Z)));
            m_water_shader->setMat4("u_model", model);
            
            glBindVertexArray(render_data.vao_id);
            glDrawElements(GL_TRIANGLES, render_data.element_count, GL_UNSIGNED_INT, 0);
            m_last_render_pass_stats.water_draws++;
            m_last_render_pass_stats.water_indices_drawn += render_data.element_count;
        }
    }

    // --- 4. Restore OpenGL State ---
    glBindVertexArray(0);
    glDepthMask(GL_TRUE);
    glDisable(GL_BLEND);
    glEnable(GL_CULL_FACE);
}


void RenderPipeline::on_resize(u32 new_width, u32 new_height) {
    if (new_width == 0 || new_height == 0 || (new_width == m_screen_width && new_height == m_screen_height)) return;
    m_screen_width = new_width;
    m_screen_height = new_height;
    destroy_lighting_fbo();
    init_lighting_fbo(new_width, new_height);
    destroy_gbuffer();
    init_gbuffer(new_width, new_height);
    destroy_ssao();
    init_ssao();
    m_frustumCache.valid = false;
}

void RenderPipeline::clear_all_chunk_data() {
    // Force clear all cached chunk render data to ensure fresh uploads
    for (auto& [id, data] : m_chunk_render_data) {
        (void)id;
        delete_chunk_slot(data);
    }
    m_chunk_render_data.clear();
    for (auto& data : m_free_chunk_render_slots) {
        delete_chunk_slot(data);
    }
    m_free_chunk_render_slots.clear();

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

// --- RENDER PASSES ---

void RenderPipeline::geometry_pass_chunks(const std::vector<RenderPipeline::ChunkMeshSnapshot>& renderable_chunks, const Camera& camera, const glm::vec4 frustum_planes[6]) {
    m_geometry_shader->use();
    glm::mat4 projection = glm::perspective(glm::radians(camera.Zoom), (float)m_screen_width / (float)m_screen_height, camera.GetNearPlane(), camera.GetFarPlane());
    glm::mat4 view = camera.GetViewMatrix();

    m_geometry_shader->setMat4("projection", projection);
    m_geometry_shader->setMat4("view", view);
    
    // Bind material LUT for G-Buffer pass
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, m_materialLUT);
    m_geometry_shader->setInt("u_materialLUT", 0);
    
    // Perform hierarchical frustum culling
    std::vector<const ChunkCullEntry*> visible_chunks;
    visible_chunks.reserve(renderable_chunks.size());
    m_hierarchicalCuller.CullHierarchical(frustum_planes, visible_chunks);
    m_last_render_pass_stats.terrain_visible_chunks = visible_chunks.size();

    // Render visible chunks
    for (const auto* chunk : visible_chunks) {
        if (m_chunk_render_data.find(chunk->id) == m_chunk_render_data.end()) continue;

        const auto& render_data = m_chunk_render_data.at(chunk->id);
        if (render_data.element_count == 0) continue;
        
        glm::ivec3 cc = chunk->coords;
        glm::vec3 min_aabb(cc.x * CHUNK_SIZE_X, cc.y * CHUNK_SIZE_Y, cc.z * CHUNK_SIZE_Z);

        glm::mat4 model = glm::translate(glm::mat4(1.0f), min_aabb);
        glm::mat3 normalMatrix = glm::transpose(glm::inverse(glm::mat3(view * model)));
        m_geometry_shader->setMat4("model", model);
        m_geometry_shader->setMat3("normalMatrix", normalMatrix);

        glBindVertexArray(render_data.vao_id);
        glDrawElements(GL_TRIANGLES, render_data.element_count, GL_UNSIGNED_INT, 0);
        m_last_render_pass_stats.terrain_draws++;
        m_last_render_pass_stats.terrain_indices_drawn += render_data.element_count;
    }
    
    glBindVertexArray(0);
}

void RenderPipeline::geometry_pass_static_meshes(entt::registry& registry, const Camera& camera, const glm::vec4 frustum_planes[6]) {
    m_instanced_static_mesh_shader->use();
    m_instanced_static_mesh_shader->setMat4("projection", glm::perspective(glm::radians(camera.Zoom), (float)m_screen_width / (float)m_screen_height, camera.GetNearPlane(), camera.GetFarPlane()));
    m_instanced_static_mesh_shader->setMat4("view", camera.GetViewMatrix());
    auto view = registry.view<const Components::TransformComponent, const Components::StaticMeshComponent>();
    std::map<std::string, std::vector<glm::mat4>> visible_instance_groups;
    for (auto entity : view) {
        auto const& transform = view.get<const Components::TransformComponent>(entity);
        auto const& mesh_info = view.get<const Components::StaticMeshComponent>(entity);
        if (m_meshCache.find(mesh_info.meshPath) == m_meshCache.end()) {
            std::string full_mesh_path = (m_root_path / mesh_info.meshPath).string();
            m_meshCache[mesh_info.meshPath] = MeshLoader::Load(full_mesh_path);
        }
        Mesh* mesh = m_meshCache[mesh_info.meshPath].get();
        if (!mesh) continue;
        glm::vec3 world_sphere_center = transform.position + glm::vec3(mesh->boundingSphere);
        float radius = mesh->boundingSphere.w * glm::max(glm::max(transform.scale.x, transform.scale.y), transform.scale.z);
        bool culled = false;
        for (int i = 0; i < 6; i++) {
            if (glm::dot(glm::vec4(world_sphere_center, 1.0f), frustum_planes[i]) < -radius) {
                culled = true;
                break;
            }
        }
        if (!culled) {
            glm::mat4 model = glm::translate(glm::mat4(1.0f), transform.position);
            model = glm::scale(model, transform.scale);
            visible_instance_groups[mesh_info.meshPath].push_back(model);
        }
    }
    for (const auto& [meshPath, matrices] : visible_instance_groups) {
        Mesh* mesh = m_meshCache[meshPath].get();
        if (!mesh || matrices.empty()) continue;
        glBindBuffer(GL_ARRAY_BUFFER, m_instanceMatrixVBO);
        glBufferSubData(GL_ARRAY_BUFFER, 0, matrices.size() * sizeof(glm::mat4), matrices.data());
        glBindVertexArray(mesh->vao);
        for (int i = 0; i < 4; i++) {
            glEnableVertexAttribArray(3 + i);
            glVertexAttribPointer(3 + i, 4, GL_FLOAT, GL_FALSE, sizeof(glm::mat4), (void*)(sizeof(glm::vec4) * i));
            glVertexAttribDivisor(3 + i, 1);
        }
        glDrawElementsInstanced(GL_TRIANGLES, mesh->indexCount, GL_UNSIGNED_INT, 0, matrices.size());
        glBindVertexArray(0);
    }
}

void RenderPipeline::shadow_pass(const std::vector<RenderPipeline::ChunkMeshSnapshot>& renderable_chunks, const Camera& camera) {
    if (!m_shadow_shader || m_shadow_map.fbo_id == 0 || m_shadow_map.depth_texture_array == 0) {
        LUMINUMBRA_CORE_ERROR("Shadow pass skipped because shadow resources are not initialized.");
        return;
    }

    auto light_space_matrices = get_light_space_matrices(camera);
    if (light_space_matrices.size() < ShadowMap::CASCADE_COUNT) {
        LUMINUMBRA_CORE_ERROR("Shadow pass skipped because light-space matrices could not be generated.");
        return;
    }

    m_shadow_map.light_space_matrices = light_space_matrices;
    glViewport(0, 0, m_shadow_map.resolution, m_shadow_map.resolution);
    glBindFramebuffer(GL_FRAMEBUFFER, m_shadow_map.fbo_id);
    glClear(GL_DEPTH_BUFFER_BIT);
    glCullFace(GL_FRONT);
    m_shadow_shader->use();
    for (int i = 0; i < ShadowMap::CASCADE_COUNT; ++i) {
        glFramebufferTextureLayer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, m_shadow_map.depth_texture_array, 0, i);
        m_shadow_shader->setMat4("u_lightSpaceMatrix", light_space_matrices[i]);

        glm::vec4 cascade_planes[6];
        ExtractFrustumPlanes(light_space_matrices[i], cascade_planes);
        std::vector<const ChunkCullEntry*> visible_chunks;
        visible_chunks.reserve(renderable_chunks.size());
        m_hierarchicalCuller.CullHierarchical(cascade_planes, visible_chunks);
        m_last_render_pass_stats.shadow_cascade_visible_chunks[i] = visible_chunks.size();

        for (const auto* chunk : visible_chunks) {
            if (m_chunk_render_data.count(chunk->id) == 0) continue;
            const auto& render_data = m_chunk_render_data.at(chunk->id);
            if (render_data.element_count == 0) continue;

            glm::ivec3 cc = chunk->coords;
            glm::vec3 base(cc.x * CHUNK_SIZE_X, cc.y * CHUNK_SIZE_Y, cc.z * CHUNK_SIZE_Z);
            glm::mat4 model = glm::translate(glm::mat4(1.0f), base);
            m_shadow_shader->setMat4("u_model", model);
            glBindVertexArray(render_data.vao_id);
            glDrawElements(GL_TRIANGLES, render_data.element_count, GL_UNSIGNED_INT, 0);
            m_last_render_pass_stats.shadow_cascade_draws[i]++;
            m_last_render_pass_stats.shadow_draws++;
            m_last_render_pass_stats.shadow_indices_drawn += render_data.element_count;
        }
    }
    glCullFace(GL_BACK);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

void RenderPipeline::lighting_pass(const Camera& camera) {
    glBindFramebuffer(GL_FRAMEBUFFER, m_lighting_fbo.fbo_id);
    glViewport(0, 0, m_screen_width, m_screen_height);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    m_lighting_shader->use();
    glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D, m_gbuffer.position_texture);    // View-space position
    glActiveTexture(GL_TEXTURE1); glBindTexture(GL_TEXTURE_2D, m_gbuffer.normal_texture);      // Octahedral normal + material
    glActiveTexture(GL_TEXTURE2); glBindTexture(GL_TEXTURE_2D, m_gbuffer.albedo_texture);      // Albedo + roughness
    glActiveTexture(GL_TEXTURE3); glBindTexture(GL_TEXTURE_2D, m_gbuffer.material_texture);    // Metallic + AO
    glActiveTexture(GL_TEXTURE4); glBindTexture(GL_TEXTURE_2D, m_gbuffer.depth_texture);
    glActiveTexture(GL_TEXTURE5); glBindTexture(GL_TEXTURE_2D_ARRAY, m_shadow_map.depth_texture_array);
    glActiveTexture(GL_TEXTURE6); glBindTexture(GL_TEXTURE_2D, m_ssao.ssaoColorBufferBlur);
    glActiveTexture(GL_TEXTURE7); glBindTexture(GL_TEXTURE_2D_ARRAY, m_terrainTextureArray);
    glActiveTexture(GL_TEXTURE8); glBindTexture(GL_TEXTURE_2D, m_materialLUT);
    glActiveTexture(GL_TEXTURE9); glBindTexture(GL_TEXTURE_2D, m_water_black_texture);
    m_lighting_shader->setMat4("u_inverseView", glm::inverse(camera.GetViewMatrix()));
    m_lighting_shader->setInt("gPosition", 0);
    m_lighting_shader->setInt("gNormalMaterial", 1);    // Octahedral normal + material
    m_lighting_shader->setInt("gAlbedoRoughness", 2);   // Albedo + roughness
    m_lighting_shader->setInt("gMetallicAO", 3);        // Metallic + AO
    m_lighting_shader->setInt("gDepth", 4);
    m_lighting_shader->setInt("u_shadowCascades", 5);
    m_lighting_shader->setInt("u_ssao", 6);
    m_lighting_shader->setInt("u_terrainTextures", 7);
    m_lighting_shader->setInt("u_materialLUT", 8);
    m_lighting_shader->setInt("u_causticsTexture", 9);
    m_lighting_shader->setVec3("u_skyAmbientColor", m_skyAmbientColor);
    m_lighting_shader->setVec3("u_viewPos", camera.Position);
    m_lighting_shader->setVec3("u_sun.direction", m_sun.direction);
    m_lighting_shader->setVec3("u_sun.color", m_sun.color);
    m_lighting_shader->setFloat("u_sea_level", SEA_LEVEL);
    m_lighting_shader->setInt("u_pointLightCount", static_cast<int>(m_point_lights_this_frame.size()));
    for(size_t i = 0; i < m_point_lights_this_frame.size(); ++i) {
        std::string prefix = "u_pointLights[" + std::to_string(i) + "].";
        m_lighting_shader->setVec3(prefix + "position", m_point_lights_this_frame[i].position);
        m_lighting_shader->setVec3(prefix + "color", m_point_lights_this_frame[i].color);
        m_lighting_shader->setFloat(prefix + "radius", m_point_lights_this_frame[i].radius);
        m_lighting_shader->setFloat(prefix + "intensity", m_point_lights_this_frame[i].intensity);
    }
    m_lighting_shader->setFloat("u_farPlane", camera.GetFarPlane());
    if (!has_valid_shadow_cascade_splits(m_shadow_map)) {
        LUMINUMBRA_CORE_ERROR("Shadow cascade splits were invalid during lighting; restoring defaults.");
        set_default_shadow_cascade_splits(m_shadow_map);
    }
    if (m_shadow_map.light_space_matrices.size() < ShadowMap::CASCADE_COUNT) {
        m_shadow_map.light_space_matrices = get_light_space_matrices(camera);
    }
    m_lighting_shader->setVec4("u_cascadeSplits", glm::vec4(m_shadow_map.cascade_splits[1], m_shadow_map.cascade_splits[2], m_shadow_map.cascade_splits[3], m_shadow_map.cascade_splits[4]));
    for (int i = 0; i < ShadowMap::CASCADE_COUNT; ++i) {
        m_lighting_shader->setMat4("u_lightSpaceMatrices[" + std::to_string(i) + "]", m_shadow_map.light_space_matrices[i]);
    }
    glm::vec3 terrainOrigin(floor(camera.Position.x / CHUNK_SIZE_X) * CHUNK_SIZE_X, 0.0f, floor(camera.Position.z / CHUNK_SIZE_Z) * CHUNK_SIZE_Z);
    m_lighting_shader->setVec3("u_terrainOrigin", terrainOrigin);
    glBindVertexArray(m_screen_quad_vao);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    m_last_render_pass_stats.lighting_draws++;
    glBindVertexArray(0);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

void RenderPipeline::skybox_pass(const Rendering::Camera& camera) {
    glDepthFunc(GL_LEQUAL);
    m_skybox_shader->use();
    glm::mat4 projection = glm::perspective(glm::radians(camera.Zoom), (float)m_screen_width / (float)m_screen_height, camera.GetNearPlane(), camera.GetFarPlane());
    glm::mat4 view = glm::mat4(glm::mat3(camera.GetViewMatrix())); // remove translation
    m_skybox_shader->setMat4("view", view);
    m_skybox_shader->setMat4("projection", projection);
    m_skybox_shader->setVec3("u_sunDirection", m_sun.direction);
    m_skybox_shader->setVec3("u_moonDirection", m_moonDirection);
    m_skybox_shader->setFloat("u_sunIntensity", m_sun.intensity);
    m_skybox_shader->setFloat("u_time", (float)glfwGetTime());
    glBindVertexArray(m_skybox_vao);
    glDrawArrays(GL_TRIANGLES, 0, 36);
    m_last_render_pass_stats.skybox_draws++;
    glBindVertexArray(0);
    glDepthFunc(GL_LESS);
}

void RenderPipeline::ssao_pass(const Camera& camera) {
    glBindFramebuffer(GL_FRAMEBUFFER, m_ssao.fbo);
    glClear(GL_COLOR_BUFFER_BIT);
    m_ssao.ssaoShader->use();
    glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D, m_gbuffer.position_texture);
    glActiveTexture(GL_TEXTURE1); glBindTexture(GL_TEXTURE_2D, m_gbuffer.normal_texture);
    glActiveTexture(GL_TEXTURE2); glBindTexture(GL_TEXTURE_2D, m_ssao.noiseTexture);
    m_ssao.ssaoShader->setInt("gPosition", 0);
    m_ssao.ssaoShader->setInt("gNormalMaterial", 1);
    m_ssao.ssaoShader->setInt("u_noiseTexture", 2);
    for (unsigned int i = 0; i < 64; ++i)
        m_ssao.ssaoShader->setVec3("u_samples[" + std::to_string(i) + "]", m_ssao.kernel[i]);
    glm::mat4 projection = glm::perspective(glm::radians(camera.Zoom), (float)m_screen_width / (float)m_screen_height, camera.GetNearPlane(), camera.GetFarPlane());
    m_ssao.ssaoShader->setMat4("u_projection", projection);
    m_ssao.ssaoShader->setVec2("u_screenSize", glm::vec2(m_screen_width, m_screen_height));
    glBindVertexArray(m_screen_quad_vao);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    m_last_render_pass_stats.ssao_draws++;
    glBindVertexArray(0);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

void RenderPipeline::ssao_blur_pass() {
    glBindFramebuffer(GL_FRAMEBUFFER, m_ssao.blurFBO);
    glClear(GL_COLOR_BUFFER_BIT);
    m_ssao.blurShader->use();
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, m_ssao.ssaoColorBuffer);
    m_ssao.blurShader->setInt("u_ssaoInput", 0);
    glBindVertexArray(m_screen_quad_vao);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    m_last_render_pass_stats.ssao_blur_draws++;
    glBindVertexArray(0);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

// --- INITIALIZATION ---

void RenderPipeline::init_shaders() {
    m_geometry_shader = std::make_unique<Shader>((m_root_path / "res/shaders/g_buffer.vert").string().c_str(), (m_root_path / "res/shaders/g_buffer.frag").string().c_str());
    m_lighting_shader = std::make_unique<Shader>((m_root_path / "res/shaders/lighting_pass.vert").string().c_str(), (m_root_path / "res/shaders/lighting_pass.frag").string().c_str());
    m_skybox_shader = std::make_unique<Shader>((m_root_path / "res/shaders/skybox.vert").string().c_str(), (m_root_path / "res/shaders/skybox.frag").string().c_str());
    m_shadow_shader = std::make_unique<Shader>((m_root_path / "res/shaders/shadow_map.vert").string().c_str(), (m_root_path / "res/shaders/shadow_map.frag").string().c_str());
    m_ssao.ssaoShader = std::make_unique<Shader>((m_root_path / "res/shaders/ssao.vert").string().c_str(), (m_root_path / "res/shaders/ssao.frag").string().c_str());
    m_ssao.blurShader = std::make_unique<Shader>((m_root_path / "res/shaders/ssao.vert").string().c_str(), (m_root_path / "res/shaders/ssao_blur.frag").string().c_str());
    m_water_shader = std::make_unique<Shader>((m_root_path / "res/shaders/water.vert").string().c_str(), (m_root_path / "res/shaders/water.frag").string().c_str());
    label_gl_object(GL_PROGRAM, m_geometry_shader ? m_geometry_shader->Id() : 0u, "shader.geometry");
    label_gl_object(GL_PROGRAM, m_lighting_shader ? m_lighting_shader->Id() : 0u, "shader.lighting");
    label_gl_object(GL_PROGRAM, m_skybox_shader ? m_skybox_shader->Id() : 0u, "shader.skybox");
    label_gl_object(GL_PROGRAM, m_shadow_shader ? m_shadow_shader->Id() : 0u, "shader.shadow");
    label_gl_object(GL_PROGRAM, m_ssao.ssaoShader ? m_ssao.ssaoShader->Id() : 0u, "shader.ssao");
    label_gl_object(GL_PROGRAM, m_ssao.blurShader ? m_ssao.blurShader->Id() : 0u, "shader.ssao_blur");
    label_gl_object(GL_PROGRAM, m_water_shader ? m_water_shader->Id() : 0u, "shader.water");
}

void RenderPipeline::init_gbuffer(u32 width, u32 height) {
    glGenFramebuffers(1, &m_gbuffer.fbo_id);
    label_gl_object(GL_FRAMEBUFFER, m_gbuffer.fbo_id, "gbuffer.fbo");
    glBindFramebuffer(GL_FRAMEBUFFER, m_gbuffer.fbo_id);
    
    // Position: full view-space position for deferred lighting, SSAO, and material projection.
    glGenTextures(1, &m_gbuffer.position_texture);
    label_gl_object(GL_TEXTURE, m_gbuffer.position_texture, "gbuffer.position");
    glBindTexture(GL_TEXTURE_2D, m_gbuffer.position_texture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB16F, width, height, 0, GL_RGB, GL_FLOAT, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, m_gbuffer.position_texture, 0);
    
    // Normal/Material: RGBA8 (4 bytes/pixel -> octahedral normal + material ID)
    glGenTextures(1, &m_gbuffer.normal_texture);
    label_gl_object(GL_TEXTURE, m_gbuffer.normal_texture, "gbuffer.normal_material");
    glBindTexture(GL_TEXTURE_2D, m_gbuffer.normal_texture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT1, GL_TEXTURE_2D, m_gbuffer.normal_texture, 0);
    
    // Albedo/Roughness: RGBA8 (4 bytes/pixel -> RGB albedo + roughness)
    glGenTextures(1, &m_gbuffer.albedo_texture);
    label_gl_object(GL_TEXTURE, m_gbuffer.albedo_texture, "gbuffer.albedo_roughness");
    glBindTexture(GL_TEXTURE_2D, m_gbuffer.albedo_texture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT2, GL_TEXTURE_2D, m_gbuffer.albedo_texture, 0);
    
    // Metallic/AO: RG16F (4 bytes/pixel -> metallic + ambient occlusion)
    glGenTextures(1, &m_gbuffer.material_texture);
    label_gl_object(GL_TEXTURE, m_gbuffer.material_texture, "gbuffer.metallic_ao");
    glBindTexture(GL_TEXTURE_2D, m_gbuffer.material_texture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RG16F, width, height, 0, GL_RG, GL_FLOAT, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT3, GL_TEXTURE_2D, m_gbuffer.material_texture, 0);
    
    const GLenum attachments[4] = { GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1, GL_COLOR_ATTACHMENT2, GL_COLOR_ATTACHMENT3 };
    glDrawBuffers(4, attachments);
    
    // Depth texture (unchanged)
    glGenTextures(1, &m_gbuffer.depth_texture);
    label_gl_object(GL_TEXTURE, m_gbuffer.depth_texture, "gbuffer.depth");
    glBindTexture(GL_TEXTURE_2D, m_gbuffer.depth_texture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT24, width, height, 0, GL_DEPTH_COMPONENT, GL_FLOAT, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_BORDER);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_BORDER);
    float borderColor[] = { 1.0f, 1.0f, 1.0f, 1.0f };
    glTexParameterfv(GL_TEXTURE_2D, GL_TEXTURE_BORDER_COLOR, borderColor);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, m_gbuffer.depth_texture, 0);

    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) LUMINUMBRA_CORE_ERROR("G-Buffer FBO not complete!");
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

void RenderPipeline::init_shadow_map() {
    glGenFramebuffers(1, &m_shadow_map.fbo_id);
    glGenTextures(1, &m_shadow_map.depth_texture_array);
    label_gl_object(GL_FRAMEBUFFER, m_shadow_map.fbo_id, "shadow.fbo");
    label_gl_object(GL_TEXTURE, m_shadow_map.depth_texture_array, "shadow.depth_cascades");
    glBindTexture(GL_TEXTURE_2D_ARRAY, m_shadow_map.depth_texture_array);
    glTexImage3D(GL_TEXTURE_2D_ARRAY, 0, GL_DEPTH_COMPONENT32F, m_shadow_map.resolution, m_shadow_map.resolution, ShadowMap::CASCADE_COUNT, 0, GL_DEPTH_COMPONENT, GL_FLOAT, nullptr);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_BORDER);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_BORDER);
    float borderColor[] = { 1.0f, 1.0f, 1.0f, 1.0f };
    glTexParameterfv(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_BORDER_COLOR, borderColor);
    glBindFramebuffer(GL_FRAMEBUFFER, m_shadow_map.fbo_id);
    glFramebufferTexture(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, m_shadow_map.depth_texture_array, 0);
    glDrawBuffer(GL_NONE);
    glReadBuffer(GL_NONE);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) LUMINUMBRA_CORE_ERROR("Shadow Map FBO not complete!");
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    set_default_shadow_cascade_splits(m_shadow_map);
}

void RenderPipeline::init_ssao() {
    glGenFramebuffers(1, &m_ssao.fbo);
    glGenFramebuffers(1, &m_ssao.blurFBO);
    label_gl_object(GL_FRAMEBUFFER, m_ssao.fbo, "ssao.fbo");
    label_gl_object(GL_FRAMEBUFFER, m_ssao.blurFBO, "ssao.blur_fbo");
    glBindFramebuffer(GL_FRAMEBUFFER, m_ssao.fbo);
    glGenTextures(1, &m_ssao.ssaoColorBuffer);
    label_gl_object(GL_TEXTURE, m_ssao.ssaoColorBuffer, "ssao.raw");
    glBindTexture(GL_TEXTURE_2D, m_ssao.ssaoColorBuffer);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_R16F, m_screen_width, m_screen_height, 0, GL_RED, GL_FLOAT, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, m_ssao.ssaoColorBuffer, 0);
    glBindFramebuffer(GL_FRAMEBUFFER, m_ssao.blurFBO);
    glGenTextures(1, &m_ssao.ssaoColorBufferBlur);
    label_gl_object(GL_TEXTURE, m_ssao.ssaoColorBufferBlur, "ssao.blur");
    glBindTexture(GL_TEXTURE_2D, m_ssao.ssaoColorBufferBlur);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_R16F, m_screen_width, m_screen_height, 0, GL_RED, GL_FLOAT, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, m_ssao.ssaoColorBufferBlur, 0);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    std::uniform_real_distribution<float> randomFloats(0.0, 1.0);
    std::default_random_engine generator;
    for (unsigned int i = 0; i < 64; ++i) {
        glm::vec3 sample(randomFloats(generator) * 2.0 - 1.0, randomFloats(generator) * 2.0 - 1.0, randomFloats(generator));
        sample = glm::normalize(sample);
        sample *= randomFloats(generator);
        float scale = (float)i / 64.0f;
        scale = std::lerp(0.1f, 1.0f, scale * scale);
        sample *= scale;
        m_ssao.kernel.push_back(sample);
    }
    std::vector<glm::vec3> ssaoNoise;
    for (unsigned int i = 0; i < 16; i++) {
        ssaoNoise.push_back(glm::vec3(randomFloats(generator) * 2.0 - 1.0, randomFloats(generator) * 2.0 - 1.0, 0.0f));
    }
    glGenTextures(1, &m_ssao.noiseTexture);
    label_gl_object(GL_TEXTURE, m_ssao.noiseTexture, "ssao.noise");
    glBindTexture(GL_TEXTURE_2D, m_ssao.noiseTexture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB16F, 4, 4, 0, GL_RGB, GL_FLOAT, &ssaoNoise[0]);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
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

void RenderPipeline::init_skybox() {
    float skyboxVertices[] = { -1.0f,1.0f,-1.0f,-1.0f,-1.0f,-1.0f,1.0f,-1.0f,-1.0f,1.0f,-1.0f,-1.0f,1.0f,1.0f,-1.0f,-1.0f,1.0f,-1.0f,-1.0f,-1.0f,1.0f,-1.0f,-1.0f,-1.0f,-1.0f,1.0f,-1.0f,-1.0f,1.0f,-1.0f,-1.0f,1.0f,1.0f,-1.0f,-1.0f,1.0f,1.0f,-1.0f,-1.0f,1.0f,-1.0f,1.0f,1.0f,1.0f,1.0f,1.0f,1.0f,1.0f,1.0f,1.0f,-1.0f,1.0f,-1.0f,-1.0f,-1.0f,-1.0f,1.0f,-1.0f,1.0f,1.0f,1.0f,1.0f,1.0f,1.0f,1.0f,-1.0f,1.0f,1.0f,-1.0f,-1.0f,1.0f,-1.0f,1.0f,-1.0f,1.0f,1.0f,-1.0f,1.0f,1.0f,1.0f,1.0f,1.0f,-1.0f,1.0f,1.0f,-1.0f,1.0f,-1.0f,-1.0f,-1.0f,-1.0f,-1.0f,-1.0f,1.0f,1.0f,-1.0f,-1.0f,1.0f,-1.0f,-1.0f,-1.0f,-1.0f,1.0f,1.0f,-1.0f,1.0f };
    glGenVertexArrays(1, &m_skybox_vao);
    glGenBuffers(1, &m_skybox_vbo);
    label_gl_object(GL_VERTEX_ARRAY, m_skybox_vao, "skybox.vao");
    label_gl_object(GL_BUFFER, m_skybox_vbo, "skybox.vbo");
    glBindVertexArray(m_skybox_vao);
    glBindBuffer(GL_ARRAY_BUFFER, m_skybox_vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(skyboxVertices), &skyboxVertices, GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 3 * sizeof(float), (void*)0);
    glBindVertexArray(0);
}

// --- CLEANUP ---

void RenderPipeline::destroy_gbuffer() {
    if (m_gbuffer.fbo_id) { glDeleteFramebuffers(1, &m_gbuffer.fbo_id); m_gbuffer.fbo_id = 0; }
    if (m_gbuffer.position_texture) { glDeleteTextures(1, &m_gbuffer.position_texture); m_gbuffer.position_texture = 0; }
    if (m_gbuffer.normal_texture) { glDeleteTextures(1, &m_gbuffer.normal_texture); m_gbuffer.normal_texture = 0; }
    if (m_gbuffer.albedo_texture) { glDeleteTextures(1, &m_gbuffer.albedo_texture); m_gbuffer.albedo_texture = 0; }
    if (m_gbuffer.material_texture) { glDeleteTextures(1, &m_gbuffer.material_texture); m_gbuffer.material_texture = 0; }
    if (m_gbuffer.depth_texture) { glDeleteTextures(1, &m_gbuffer.depth_texture); m_gbuffer.depth_texture = 0; }
}

void RenderPipeline::destroy_shadow_map() {
    if (m_shadow_map.fbo_id) { glDeleteFramebuffers(1, &m_shadow_map.fbo_id); m_shadow_map.fbo_id = 0; }
    if (m_shadow_map.depth_texture_array) { glDeleteTextures(1, &m_shadow_map.depth_texture_array); m_shadow_map.depth_texture_array = 0; }
    m_shadow_map.light_space_matrices.clear();
    m_shadow_map.cascade_splits.clear();
}

void RenderPipeline::destroy_ssao() {
    if (m_ssao.fbo) { glDeleteFramebuffers(1, &m_ssao.fbo); m_ssao.fbo = 0; }
    if (m_ssao.blurFBO) { glDeleteFramebuffers(1, &m_ssao.blurFBO); m_ssao.blurFBO = 0; }
    if (m_ssao.ssaoColorBuffer) { glDeleteTextures(1, &m_ssao.ssaoColorBuffer); m_ssao.ssaoColorBuffer = 0; }
    if (m_ssao.ssaoColorBufferBlur) { glDeleteTextures(1, &m_ssao.ssaoColorBufferBlur); m_ssao.ssaoColorBufferBlur = 0; }
    if (m_ssao.noiseTexture) { glDeleteTextures(1, &m_ssao.noiseTexture); m_ssao.noiseTexture = 0; }
}

void RenderPipeline::destroy_water_fallback_textures() {
    if (m_water_flat_normal_texture) { glDeleteTextures(1, &m_water_flat_normal_texture); m_water_flat_normal_texture = 0; }
    if (m_water_neutral_flow_texture) { glDeleteTextures(1, &m_water_neutral_flow_texture); m_water_neutral_flow_texture = 0; }
    if (m_water_black_texture) { glDeleteTextures(1, &m_water_black_texture); m_water_black_texture = 0; }
    if (m_water_underwater_texture) { glDeleteTextures(1, &m_water_underwater_texture); m_water_underwater_texture = 0; }
}

void RenderPipeline::cleanup_gpu_resources() {
    for (auto& [id, d] : m_chunk_render_data) {
        (void)id;
        delete_chunk_slot(d);
    }
    m_chunk_render_data.clear();
    for (auto& d : m_free_chunk_render_slots) {
        delete_chunk_slot(d);
    }
    m_free_chunk_render_slots.clear();

    for (auto& [id, d] : m_water_render_data) {
        (void)id;
        delete_water_slot(d);
    }
    m_water_render_data.clear();
    for (auto& d : m_free_water_render_slots) {
        delete_water_slot(d);
    }
    m_free_water_render_slots.clear();
    destroy_lighting_fbo();
    destroy_gbuffer();
    destroy_shadow_map();
    destroy_ssao();
    if (m_screen_quad_vao) { glDeleteVertexArrays(1, &m_screen_quad_vao); m_screen_quad_vao = 0; }
    if (m_screen_quad_vbo) { glDeleteBuffers(1, &m_screen_quad_vbo); m_screen_quad_vbo = 0; }
    if (m_skybox_vao) { glDeleteVertexArrays(1, &m_skybox_vao); m_skybox_vao = 0; }
    if (m_skybox_vbo) { glDeleteBuffers(1, &m_skybox_vbo); m_skybox_vbo = 0; }
    if (m_instanceMatrixVBO) { glDeleteBuffers(1, &m_instanceMatrixVBO); m_instanceMatrixVBO = 0; }
    if (m_terrainTextureArray) { glDeleteTextures(1, &m_terrainTextureArray); m_terrainTextureArray = 0; }
    if (m_materialLUT) { glDeleteTextures(1, &m_materialLUT); m_materialLUT = 0; }
    destroy_water_fallback_textures();
    cleanup_gpu_sdf_system();
    destroy_gpu_pass_timers();
    m_geometry_shader.reset();
    m_lighting_shader.reset();
    m_skybox_shader.reset();
    m_shadow_shader.reset();
    m_ssao.ssaoShader.reset();
    m_ssao.blurShader.reset();
    m_water_shader.reset();
    m_instanced_static_mesh_shader.reset();
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

    auto it = m_chunk_render_data.find(chunk.id);
    bool slot_created = false;
    bool slot_from_pool = false;
    if (it == m_chunk_render_data.end()) {
        ChunkRenderData render_data;
        if (!m_free_chunk_render_slots.empty()) {
            render_data = m_free_chunk_render_slots.back();
            m_free_chunk_render_slots.pop_back();
            slot_from_pool = true;
        } else {
            glGenVertexArrays(1, &render_data.vao_id);
            glGenBuffers(1, &render_data.vbo_id);
            glGenBuffers(1, &render_data.ebo_id);
            const std::string label_prefix = "terrain.chunk." + std::to_string(chunk.id);
            label_gl_object(GL_VERTEX_ARRAY, render_data.vao_id, label_prefix + ".vao");
            label_gl_object(GL_BUFFER, render_data.vbo_id, label_prefix + ".vbo");
            label_gl_object(GL_BUFFER, render_data.ebo_id, label_prefix + ".ebo");
            slot_created = true;
        }
        it = m_chunk_render_data.emplace(chunk.id, render_data).first;
    }

    ChunkRenderData& render_data = it->second;
    const u32 vertex_count = static_cast<u32>(payload.vertices.size());
    const u32 index_count = static_cast<u32>(payload.indices.size());
    const bool needs_growth = render_data.vertex_capacity < vertex_count || render_data.index_capacity < index_count;
    const bool version_reused = render_data.mesh_version != 0 && render_data.mesh_version != payload.mesh_version;

    glBindVertexArray(render_data.vao_id);
    glBindBuffer(GL_ARRAY_BUFFER, render_data.vbo_id);
    if (needs_growth) {
        glBufferData(GL_ARRAY_BUFFER, payload.vertices.size() * sizeof(VoxelVertex), payload.vertices.data(), GL_STATIC_DRAW);
        render_data.vertex_capacity = vertex_count;
    } else {
        glBufferSubData(GL_ARRAY_BUFFER, 0, payload.vertices.size() * sizeof(VoxelVertex), payload.vertices.data());
    }

    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, render_data.ebo_id);
    if (needs_growth) {
        glBufferData(GL_ELEMENT_ARRAY_BUFFER, payload.indices.size() * sizeof(u32), payload.indices.data(), GL_STATIC_DRAW);
        render_data.index_capacity = index_count;
    } else {
        glBufferSubData(GL_ELEMENT_ARRAY_BUFFER, 0, payload.indices.size() * sizeof(u32), payload.indices.data());
    }

    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(VoxelVertex), (void*)offsetof(VoxelVertex, position));
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, sizeof(VoxelVertex), (void*)offsetof(VoxelVertex, normal));
    glEnableVertexAttribArray(2);
    glVertexAttribIPointer(2, 1, GL_UNSIGNED_INT, sizeof(VoxelVertex), (void*)offsetof(VoxelVertex, material_id));
    glBindVertexArray(0);

    render_data.element_count = index_count;
    render_data.mesh_version = payload.mesh_version;
    render_data.frames_since_inactive = 0;

    if (slot_created) {
        m_last_mesh_upload_stats.terrain_slots_created++;
    } else if (needs_growth) {
        m_last_mesh_upload_stats.terrain_slots_grown++;
    } else if (slot_from_pool || version_reused) {
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
        data.element_count = 0;
        data.mesh_version = 0;
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
    // List of textures to load in order. This order MUST match the MaterialID enum,
    // starting from MaterialID = 1.
    const std::vector<std::string> texture_paths = {
        "data/textures/terrain/rock/Rock028_2K-PNG_Color.png",      // MaterialID = 1 (Stone) -> Index 0
        "data/textures/terrain/soil/Ground048_2K-PNG_Color.png",       // MaterialID = 2 (Soil) -> Index 1
        "data/textures/terrain/grass/Grass003_2K-PNG_Color.png",      // MaterialID = 3 (Grass) -> Index 2
        "data/textures/terrain/sand/Ground087_2K-PNG_Color.png",       // MaterialID = 4 (Sand) -> Index 3
        "data/textures/terrain/deepslate/Gravel040_2K-PNG_Color.png" // MaterialID = 5 (Deepslate) -> Index 4
    };

    const int texture_width = 2048;  // Assuming all textures are 2K
    const int texture_height = 2048;
    const int layer_count = texture_paths.size();
    m_terrain_texture_fallback_layers = 0;

    glGenTextures(1, &m_terrainTextureArray);
    label_gl_object(GL_TEXTURE, m_terrainTextureArray, "terrain.texture_array");
    glBindTexture(GL_TEXTURE_2D_ARRAY, m_terrainTextureArray);

    // Allocate storage for the entire texture array
    glTexImage3D(GL_TEXTURE_2D_ARRAY, 0, GL_SRGB8_ALPHA8, texture_width, texture_height, layer_count, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);

    for (int i = 0; i < layer_count; ++i) {
        std::string full_path = (m_root_path / texture_paths[i]).string();
        int width, height, channels;
        stbi_set_flip_vertically_on_load(true);
        unsigned char* data = stbi_load(full_path.c_str(), &width, &height, &channels, 4); // Force 4 channels
        bool uploaded = false;

        if (data) {
            if (width != texture_width || height != texture_height) {
                 LUMINUMBRA_CORE_ERROR("Texture '{}' has wrong dimensions!", texture_paths[i]);
                 stbi_image_free(data);
                 data = nullptr;
            } else {
                // Upload data to the i-th layer of the array
                glTexSubImage3D(GL_TEXTURE_2D_ARRAY, 0, 0, 0, i, width, height, 1, GL_RGBA, GL_UNSIGNED_BYTE, data);
                stbi_image_free(data);
                uploaded = true;
            }
        }

        if (!uploaded) {
            LUMINUMBRA_CORE_ERROR("Failed to load texture array layer: {}", texture_paths[i]);
            const std::vector<unsigned char> fallback = make_terrain_fallback_texture(texture_width, texture_height, i);
            glTexSubImage3D(GL_TEXTURE_2D_ARRAY, 0, 0, 0, i, texture_width, texture_height, 1, GL_RGBA, GL_UNSIGNED_BYTE, fallback.data());
            ++m_terrain_texture_fallback_layers;
        }
    }

    // Set texture parameters
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_T, GL_REPEAT);
    
    glGenerateMipmap(GL_TEXTURE_2D_ARRAY); // Generate mipmaps for the entire array

    LUMINUMBRA_CORE_INFO("Terrain texture array loaded with {} layers.", layer_count);
}

void RenderPipeline::init_material_lut() {
    // Create material properties lookup texture (256x4 RGBA8 = 1KB)
    const int MATERIAL_COUNT = 256;
    
    // Material properties: [R: Metallic, G: Roughness, B: AO, A: Reserved]
    std::vector<glm::vec4> materialData(MATERIAL_COUNT, glm::vec4(0.1f, 0.8f, 1.0f, 0.0f)); // Default values
    
    // Define specific materials matching the G-Buffer shader
    materialData[0] = glm::vec4(0.1f, 0.8f, 1.0f, 0.0f);   // Air/Default
    materialData[1] = glm::vec4(0.05f, 0.85f, 1.0f, 0.0f); // Stone
    materialData[2] = glm::vec4(0.0f, 0.9f, 1.0f, 0.0f);   // Soil
    materialData[3] = glm::vec4(0.0f, 0.8f, 1.0f, 0.0f);   // Grass
    materialData[4] = glm::vec4(0.0f, 0.75f, 1.0f, 0.0f);  // Sand
    materialData[5] = glm::vec4(0.02f, 0.95f, 1.0f, 0.0f); // Deepslate
    materialData[6] = glm::vec4(0.1f, 0.05f, 1.0f, 1.0f);  // Luminous Crystal (marked as magical in alpha)
    materialData[7] = glm::vec4(0.0f, 0.1f, 1.0f, 0.0f);   // Water
    
    // Additional materials can be added here for future expansion
    // materialData[8] = glm::vec4(...);  // Wood
    // materialData[9] = glm::vec4(...);  // Metal
    // etc.
    
    glGenTextures(1, &m_materialLUT);
    label_gl_object(GL_TEXTURE, m_materialLUT, "terrain.material_lut");
    glBindTexture(GL_TEXTURE_2D, m_materialLUT);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, MATERIAL_COUNT, 1, 0, GL_RGBA, GL_FLOAT, materialData.data());
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    
    LUMINUMBRA_CORE_INFO("Material LUT initialized with {} materials.", MATERIAL_COUNT);
}

void RenderPipeline::init_water_fallback_textures() {
    const unsigned char flat_normal[4] = {128, 128, 255, 255};
    const unsigned char neutral_flow[4] = {128, 128, 0, 0};
    const unsigned char black[4] = {0, 0, 0, 255};
    const unsigned char underwater[4] = {5, 28, 48, 255};

    m_water_flat_normal_texture = make_solid_rgba_texture(flat_normal, "water.fallback.flat_normal");
    m_water_neutral_flow_texture = make_solid_rgba_texture(neutral_flow, "water.fallback.neutral_flow");
    m_water_black_texture = make_solid_rgba_texture(black, "water.fallback.black");
    m_water_underwater_texture = make_solid_rgba_texture(underwater, "water.fallback.underwater");
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

    glm::vec3 noonColor(1.0f, 0.95f, 0.85f);
    glm::vec3 horizonColor(1.0f, 0.6f, 0.2f);
    m_sun.color = glm::mix(horizonColor, noonColor, glm::smoothstep(0.0f, 0.25f, sun_up_factor)) * m_sun.intensity;
    
    m_moonDirection = -m_sun.direction;

    glm::vec3 dayAmbient(0.1f, 0.15f, 0.2f);
    glm::vec3 nightAmbient(0.01f, 0.02f, 0.04f);
    m_skyAmbientColor = glm::mix(nightAmbient, dayAmbient, m_sun.intensity);
}

void RenderPipeline::set_time_of_day(float normalized_time) {
    m_timeOfDay = std::clamp(normalized_time, 0.0f, 1.0f);
}

std::vector<glm::mat4> RenderPipeline::get_light_space_matrices(const Camera& camera) {
    if (!has_valid_shadow_cascade_splits(m_shadow_map)) {
        LUMINUMBRA_CORE_ERROR("Shadow cascade splits were not initialized; restoring defaults.");
        set_default_shadow_cascade_splits(m_shadow_map);
    }

    std::vector<glm::mat4> matrices;
    matrices.reserve(ShadowMap::CASCADE_COUNT);
    for (int i = 0; i < ShadowMap::CASCADE_COUNT; i++) {
        float split_near = (i == 0) ? camera.GetNearPlane() : m_shadow_map.cascade_splits[i];
        float split_far = m_shadow_map.cascade_splits[i + 1];
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
