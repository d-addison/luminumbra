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
        init_terrain_textures();
        init_material_lut();
        m_water_pass->init_water_fallback_textures();
        init_gpu_sdf_system();
        init_gpu_pass_timers();

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
    if (m_terrainTextureArray) estimated_vram_bytes += 2048u * 2048u * 5u * 4u;
    if (m_materialLUT) estimated_vram_bytes += 256u * 4u;
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
    stats.textures += count(m_materialLUT);
    stats.textures += count(m_water_pass->flat_normal_texture());
    stats.textures += count(m_water_pass->neutral_flow_texture());
    stats.textures += count(m_water_pass->black_fallback_texture());
    stats.textures += count(m_water_pass->underwater_texture());
    stats.textures += count(m_water_pass->caustics_texture());
    stats.textures += count(m_gpu_sdf.terrain_noise_texture);
    stats.textures += count(m_gpu_sdf.cave_noise_texture);
    stats.textures += count(m_gpu_sdf.island_mask_texture);

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
    add_pass("gbuffer", {"terrain_meshes", "farlod_region_meshes", "static_meshes", "skinned_meshes", "material_lut"},
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
    m_lighting_pass->destroy_lighting_fbo();
    m_lighting_pass->init_lighting_fbo(new_width, new_height);
    m_gbuffer_pass->destroy_gbuffer();
    m_gbuffer_pass->init_gbuffer(new_width, new_height);
    m_ssao_pass->destroy_ssao();
    m_ssao_pass->init_ssao(new_width, new_height);
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
    if (m_materialLUT) { glDeleteTextures(1, &m_materialLUT); m_materialLUT = 0; }
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
