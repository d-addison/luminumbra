#pragma once

#include "../../include/luminumbra/core/Types.h"
#include <glad/glad.h>
#include <array>
#include <vector>
#include <memory>
#include <unordered_map>
#include <string>
#include <glm/glm.hpp>
#include "Mesh.h"
#include <map>
#include "core/AssetManager.h"
#include <filesystem>
#include "luminumbra_common/components/LightingComponents.h"
#include "luminumbra_common/world/Chunk.h"

// Forward declarations
namespace Luminumbra { class Chunk; }
namespace Luminumbra::Systems { class SHIELD_WorldSystem; struct TerrainGenParams; }
namespace Luminumbra::Rendering { class Shader; class Camera; class ShadowPass; class GBufferPass; }

namespace Luminumbra::Rendering {

struct DirectionalLight {
    glm::vec3 direction = glm::normalize(glm::vec3(0.5f, -1.0f, -0.5f));
    glm::vec3 color = glm::vec3(1.0f, 0.95f, 0.85f);
    float intensity = 1.0f;
};

struct PointLight {
    glm::vec3 position;
    float radius; // Using std140 layout padding for future UBO compatibility
    glm::vec3 color;
    float intensity;
};

struct GBuffer {
    u32 fbo_id = 0;
    u32 position_texture = 0;
    u32 normal_texture = 0;
    u32 albedo_texture = 0;
    u32 material_texture = 0;
    u32 depth_texture = 0; 
};

struct ShadowMap {
    u32 fbo_id = 0;
    u32 depth_texture_array = 0;
    u32 resolution = 2048;
    static constexpr int CASCADE_COUNT = 4;
    std::vector<glm::mat4> light_space_matrices;
    std::vector<float> cascade_splits;
};

struct ChunkRenderData {
    u32 vao_id = 0;
    u32 vbo_id = 0;
    u32 ebo_id = 0;
    u32 element_count = 0;
    u32 mesh_version = 0;
    u32 vertex_capacity = 0;
    u32 index_capacity = 0;
    u32 frames_since_inactive = 0;
};

struct WaterRenderData {
    u32 vao_id = 0;
    u32 vbo_id = 0;
    u32 ebo_id = 0;
    u32 element_count = 0;
    u32 mesh_version = 0;
    u32 vertex_capacity = 0;
    u32 index_capacity = 0;
    u32 frames_since_inactive = 0;
};

struct FrameBufferObject {
    u32 fbo_id = 0;
    u32 color_texture = 0;
    u32 opaque_color_texture = 0;
    u32 depth_texture = 0;
};

struct SSAOData {
    GLuint fbo = 0, blurFBO = 0;
    GLuint ssaoColorBuffer = 0, ssaoColorBufferBlur = 0;
    GLuint noiseTexture = 0;
    std::vector<glm::vec3> kernel;
    std::unique_ptr<Shader> ssaoShader;
    std::unique_ptr<Shader> blurShader;
};

class RenderPipeline {
public:
    struct MeshUploadFrameStats {
        size_t snapshot_count = 0;
        size_t terrain_upload_candidates = 0;
        size_t terrain_uploads = 0;
        size_t terrain_payload_copies = 0;
        size_t terrain_payload_bytes = 0;
        size_t terrain_uploads_deferred = 0;
        size_t terrain_new_upload_candidates = 0;
        size_t terrain_stale_upload_candidates = 0;
        size_t terrain_new_uploads_selected = 0;
        size_t terrain_stale_uploads_selected = 0;
        size_t terrain_new_uploads_deferred = 0;
        size_t terrain_stale_uploads_deferred = 0;
        size_t terrain_deferred_nearer_than_selected = 0;
        float terrain_nearest_candidate_distance_sq = 0.0f;
        float terrain_farthest_selected_distance_sq = 0.0f;
        float terrain_nearest_deferred_distance_sq = 0.0f;
        size_t terrain_slots_created = 0;
        size_t terrain_slots_reused = 0;
        size_t terrain_slots_grown = 0;
        size_t terrain_upload_failures = 0;
        size_t water_upload_candidates = 0;
        size_t water_uploads = 0;
        size_t water_payload_copies = 0;
        size_t water_payload_bytes = 0;
        size_t water_uploads_deferred = 0;
        size_t water_new_upload_candidates = 0;
        size_t water_stale_upload_candidates = 0;
        size_t water_new_uploads_selected = 0;
        size_t water_stale_uploads_selected = 0;
        size_t water_new_uploads_deferred = 0;
        size_t water_stale_uploads_deferred = 0;
        size_t water_deferred_nearer_than_selected = 0;
        float water_nearest_candidate_distance_sq = 0.0f;
        float water_farthest_selected_distance_sq = 0.0f;
        float water_nearest_deferred_distance_sq = 0.0f;
        size_t water_slots_created = 0;
        size_t water_slots_reused = 0;
        size_t water_slots_grown = 0;
        size_t water_upload_failures = 0;
    };

    struct RenderPassFrameStats {
        size_t snapshot_count = 0;
        size_t culling_hierarchy_rebuilds = 0;
        size_t culling_hierarchy_chunks = 0;
        size_t terrain_visible_chunks = 0;
        size_t terrain_draws = 0;
        size_t terrain_indices_drawn = 0;
        std::array<size_t, ShadowMap::CASCADE_COUNT> shadow_cascade_visible_chunks{};
        std::array<size_t, ShadowMap::CASCADE_COUNT> shadow_cascade_draws{};
        size_t shadow_draws = 0;
        size_t shadow_indices_drawn = 0;
        size_t ssao_draws = 0;
        size_t ssao_blur_draws = 0;
        size_t lighting_draws = 0;
        size_t water_draws = 0;
        size_t water_indices_drawn = 0;
        size_t skybox_draws = 0;
        size_t final_blits = 0;
        // Per-pass GPU timings sampled from a GL_TIMESTAMP query ring
        // (frame N publishes the timings recorded at frame N-2). Values stay
        // 0.0 when timers are unsupported or no sample has resolved yet.
        bool gpu_timers_supported = false;
        double shadow_gpu_ms = 0.0;
        double gbuffer_gpu_ms = 0.0;
        double ssao_gpu_ms = 0.0;
        double ssao_blur_gpu_ms = 0.0;
        double lighting_gpu_ms = 0.0;
        double water_gpu_ms = 0.0;
        double skybox_gpu_ms = 0.0;
        double final_blit_gpu_ms = 0.0;
    };

    struct RenderPassMetadata {
        std::string name;
        std::vector<std::string> inputs;
        std::vector<std::string> outputs;
        u32 width = 0;
        u32 height = 0;
        std::string clear;
        std::string load_store;
        size_t draw_count = 0;
        size_t dispatch_count = 0;
    };

    struct RenderResourceRegistryStats {
        size_t framebuffers = 0;
        size_t textures = 0;
        size_t renderbuffers = 0;
        size_t buffers = 0;
        size_t vertex_arrays = 0;
        size_t shader_programs = 0;
        size_t terrain_slots = 0;
        size_t water_slots = 0;
        bool empty_after_shutdown = false;
    };

    struct ShaderHealthEntry {
        std::string name;
        bool ok = false;
        std::string diagnostic;
    };

    struct RuntimeRenderStats {
        size_t terrain_gpu_chunks = 0;
        size_t water_gpu_chunks = 0;
        size_t free_terrain_slots = 0;
        size_t free_water_slots = 0;
        size_t terrain_vertex_capacity = 0;
        size_t terrain_index_capacity = 0;
        size_t water_vertex_capacity = 0;
        size_t water_index_capacity = 0;
        size_t estimated_vram_bytes = 0;
        bool started = false;
        bool geometry_shader_ok = false;
        bool lighting_shader_ok = false;
        bool skybox_shader_ok = false;
        bool shadow_shader_ok = false;
        bool ssao_shader_ok = false;
        bool ssao_blur_shader_ok = false;
        bool water_shader_ok = false;
        bool instanced_static_mesh_shader_ok = false;
        bool gpu_sdf_initialized = false;
        bool gpu_sdf_compile_time_enabled = false;
        bool gpu_sdf_runtime_requested = false;
        bool gpu_sdf_runtime_allowed = false;
        bool gpu_sdf_callback_registered = false;
        bool gpu_sdf_cpu_fallback_active = true;
        bool terrain_texture_array_ok = false;
        bool material_lut_ok = false;
        size_t terrain_texture_fallback_layers = 0;
    };

    struct GpuSdfRuntimeToggleState {
        bool compile_time_enabled = false;
        bool runtime_requested = false;
        bool runtime_allowed = false;
        bool callback_registered = false;
        bool cpu_fallback_active = true;
    };

    struct RenderHealthSnapshot {
        RuntimeRenderStats runtime;
        RenderResourceRegistryStats resources;
        std::vector<ShaderHealthEntry> shaders;
        std::vector<RenderPassMetadata> passes;
        size_t gl_debug_errors = 0;
        bool started = false;
        bool passed = false;
        std::vector<std::string> failures;
    };

    RenderPipeline();
    ~RenderPipeline();

    bool startup(u32 screen_width, u32 screen_height, const std::filesystem::path& root_path);
    void shutdown();
    void render_frame(entt::registry& registry, Systems::SHIELD_WorldSystem& world_system, const Camera& camera, float deltaTime, bool wireframe = false);
    void on_resize(u32 new_width, u32 new_height);
    void clear_all_chunk_data(); // Force clear all cached chunk render data
    const MeshUploadFrameStats& get_last_mesh_upload_stats() const { return m_last_mesh_upload_stats; }
    const RenderPassFrameStats& get_last_render_pass_stats() const { return m_last_render_pass_stats; }
    const std::vector<RenderPassMetadata>& get_last_render_pass_metadata() const { return m_last_render_pass_metadata; }
    RuntimeRenderStats get_runtime_render_stats() const;
    RenderResourceRegistryStats get_resource_registry_stats() const;
    std::vector<ShaderHealthEntry> get_shader_health() const;
    RenderHealthSnapshot get_render_health_snapshot(bool drain_gl_errors = false) const;
    void set_time_of_day(float normalized_time);
    
    // GPU SDF integration
    void set_gpu_sdf_runtime_enabled(bool enabled);
    GpuSdfRuntimeToggleState get_gpu_sdf_runtime_toggle_state() const;
    void SetupGPUSDFIntegration(Systems::SHIELD_WorldSystem& world_system);

private:
    // Extracted render pass classes (T-I2-11). Passes own their GL resources
    // (FBOs/textures/shaders); the pipeline keeps orchestration order, shared
    // state, stats collection, and GPU timer issue/collect calls.
    friend class ShadowPass;
    friend class GBufferPass;

    struct ChunkMeshSnapshot {
        ChunkID id = 0;
        IVec3 coords{};
        const Chunk* source_chunk = nullptr;
        u32 mesh_version = 0;
        u32 water_mesh_version = 0;
        size_t terrain_vertex_count = 0;
        size_t terrain_index_count = 0;
        size_t water_vertex_count = 0;
        size_t water_index_count = 0;

        bool has_terrain_mesh() const { return terrain_vertex_count > 0 && terrain_index_count > 0; }
        bool has_water_mesh() const { return water_vertex_count > 0 && water_index_count > 0; }
    };

    struct ChunkMeshPayload {
        u32 mesh_version = 0;
        std::vector<VoxelVertex> vertices;
        std::vector<u32> indices;

        bool has_mesh() const { return !vertices.empty() && !indices.empty(); }
    };

    std::vector<ChunkMeshSnapshot> build_chunk_snapshots(const std::vector<Chunk*>& renderable_chunks) const;

    void ensure_terrain_culling_hierarchy(const std::vector<ChunkMeshSnapshot>& renderable_chunks);
    void manage_chunk_gpu_resources(const std::vector<ChunkMeshSnapshot>& renderable_chunks, const Camera& camera);
    bool copy_terrain_mesh_payload(const ChunkMeshSnapshot& chunk, ChunkMeshPayload& payload) const;
    void upload_chunk_mesh(const ChunkMeshSnapshot& chunk, const ChunkMeshPayload& payload);
    void unload_chunk_resources(ChunkID chunk_id);

    void ssao_pass(const Camera& camera);
    void ssao_blur_pass();
    void lighting_pass(const Camera& camera);
    void skybox_pass(const Camera& camera);

    void manage_water_gpu_resources(const std::vector<ChunkMeshSnapshot>& renderable_chunks, const Camera& camera);
    bool copy_water_mesh_payload(const ChunkMeshSnapshot& chunk, ChunkMeshPayload& payload) const;
    void upload_water_mesh(const ChunkMeshSnapshot& chunk, const ChunkMeshPayload& payload);
    void unload_water_resources(ChunkID chunk_id);

    void init_shaders();
    void init_skybox();
    void init_screen_quad();
    void init_ssao();
    void destroy_ssao();
    void cleanup_gpu_resources();

    // --- Per-pass GPU timers (GL_TIMESTAMP query pairs) ---
    enum class GpuTimerPass : size_t {
        Shadow = 0,
        GBuffer,
        Ssao,
        SsaoBlur,
        Lighting,
        Water,
        Skybox,
        FinalBlit,
        Count,
    };
    static constexpr size_t kGpuTimerPassCount = static_cast<size_t>(GpuTimerPass::Count);
    // Ring of 3 frame slots so frame N polls the queries issued at frame N-2
    // without ever stalling on GL_QUERY_RESULT_AVAILABLE.
    static constexpr size_t kGpuTimerFrameRing = 3;

    struct GpuTimerFrameSlot {
        std::array<GLuint, kGpuTimerPassCount> begin_queries{};
        std::array<GLuint, kGpuTimerPassCount> end_queries{};
        std::array<bool, kGpuTimerPassCount> issued{};
    };

    struct GpuPassTimers {
        bool supported = false;
        bool labeled = false;
        bool first_sample_logged = false;
        u64 frame_index = 0;
        std::array<GpuTimerFrameSlot, kGpuTimerFrameRing> slots{};
        std::array<double, kGpuTimerPassCount> last_gpu_ms{};
    };
    GpuPassTimers m_gpu_timers;

    void init_gpu_pass_timers();
    void destroy_gpu_pass_timers();
    void begin_gpu_pass_timer(GpuTimerPass pass);
    void end_gpu_pass_timer(GpuTimerPass pass);
    void collect_gpu_pass_timers();
    void finish_gpu_pass_timer_frame();

    void init_lighting_fbo(u32 width, u32 height);
    void destroy_lighting_fbo();
    void copy_lighting_color_to_opaque_texture();
    void refresh_render_pass_metadata();
    void water_pass(const std::vector<ChunkMeshSnapshot>& renderable_chunks, const Camera& camera);
    FrameBufferObject m_lighting_fbo;
    
    std::vector<glm::mat4> get_light_space_matrices(const Camera& camera);

    void update_time_of_day(float deltaTime);
    float m_timeOfDay = 0.5f; // Start at sunrise
    float m_dayDurationSeconds = 60.0f;

    u32 m_screen_width = 0;
    u32 m_screen_height = 0;
    std::filesystem::path m_root_path;

    std::unique_ptr<Shader> m_lighting_shader;
    std::unique_ptr<Shader> m_skybox_shader;
    std::unique_ptr<Shader> m_water_shader;

    DirectionalLight m_sun;
    glm::vec3 m_moonDirection;
    glm::vec3 m_skyAmbientColor;
    std::unique_ptr<GBufferPass> m_gbuffer_pass;
    std::unique_ptr<ShadowPass> m_shadow_pass;
    SSAOData m_ssao;

    std::unordered_map<ChunkID, ChunkRenderData> m_chunk_render_data;
    std::unordered_map<ChunkID, WaterRenderData> m_water_render_data;
    std::vector<ChunkRenderData> m_free_chunk_render_slots;
    std::vector<WaterRenderData> m_free_water_render_slots;
    MeshUploadFrameStats m_last_mesh_upload_stats;
    RenderPassFrameStats m_last_render_pass_stats;
    std::vector<RenderPassMetadata> m_last_render_pass_metadata;

    u32 m_screen_quad_vao = 0;
    u32 m_screen_quad_vbo = 0;
    u32 m_skybox_vao = 0;
    u32 m_skybox_vbo = 0;

    u32 m_terrainTextureArray = 0;
    u32 m_materialLUT = 0;
    u32 m_water_flat_normal_texture = 0;
    u32 m_water_neutral_flow_texture = 0;
    u32 m_water_black_texture = 0;
    u32 m_water_underwater_texture = 0;
    size_t m_terrain_texture_fallback_layers = 0;

    void init_terrain_textures();
    void init_material_lut();
    void init_water_fallback_textures();
    void destroy_water_fallback_textures();
    
    // --- GPU SDF Generation ---
    struct GPUSDFSystem {
        GLuint compute_program = 0;
        GLuint sdf_buffer = 0;
        GLuint terrain_noise_texture = 0;
        GLuint cave_noise_texture = 0;
        GLuint island_mask_texture = 0;
        bool initialized = false;
        bool runtime_requested = false;
        bool callback_registered = false;
        
        // Async compute fence for non-blocking operation
        GLsync compute_fence = nullptr;
    };
    GPUSDFSystem m_gpu_sdf;
    
    void init_gpu_sdf_system();
    void generate_noise_textures();
    bool generate_chunk_sdf_gpu(const glm::ivec3& chunk_coords, const ::Luminumbra::Systems::TerrainGenParams& params, int seed, std::vector<float>& out_sdf);
    void cleanup_gpu_sdf_system();

    std::vector<PointLight> m_point_lights_this_frame;
    const int MAX_POINT_LIGHTS = 32;

    void gather_lights(entt::registry& registry);

    bool m_started = false;
    
    // Hierarchical frustum culling system
    struct AABB {
        glm::vec3 min;
        glm::vec3 max;
        
        AABB() = default;
        AABB(const glm::vec3& min, const glm::vec3& max) : min(min), max(max) {}
    };

    struct ChunkCullEntry {
        ChunkID id = 0;
        IVec3 coords{};
        AABB bounds;
    };
    
    struct CullingNode {
        AABB bounds;
        std::vector<ChunkCullEntry> chunks;
        std::unique_ptr<CullingNode> children[4]; // Quadtree (X-Z plane)
        bool is_leaf = true;
        
        CullingNode() = default;
        CullingNode(const AABB& bounds) : bounds(bounds) {}
    };
    
    class HierarchicalCuller {
    public:
        void BuildHierarchy(const std::vector<ChunkMeshSnapshot>& chunks);
        void CullRecursive(const glm::vec4 frustum_planes[6], CullingNode* node, std::vector<const ChunkCullEntry*>& visible);
        void CullHierarchical(const glm::vec4 frustum_planes[6], std::vector<const ChunkCullEntry*>& visible);
        void Clear();
        
        std::unique_ptr<CullingNode> m_root; // Made public for access
        
    private:
        static constexpr int MAX_CHUNKS_PER_NODE = 8;
        static constexpr int MAX_DEPTH = 4;
        
        void BuildRecursive(CullingNode* node, const std::vector<ChunkCullEntry>& chunks, int depth);
        bool AABBFrustumCulled(const AABB& aabb, const glm::vec4 frustum_planes[6]);
    };
    
    HierarchicalCuller m_hierarchicalCuller;

    struct TerrainCullingCache {
        u64 chunk_set_signature = 0;
        size_t chunk_count = 0;
        bool valid = false;
    } m_terrainCullingCache;

    // Frustum culling cache
    struct FrustumCache {
        glm::vec4 planes[6];
        glm::vec3 lastCameraPos;
        glm::vec3 lastCameraFront;
        float lastZoom = 0.0f;
        bool valid = false;
    } m_frustumCache;
};

} // namespace Luminumbra::Rendering
