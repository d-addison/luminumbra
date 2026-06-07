#pragma once

#include "../../include/luminumbra/core/Types.h"
#include <glad/glad.h>
#include <vector>
#include <memory>
#include <unordered_map>
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
namespace Luminumbra::Rendering { class Shader; class Camera; }

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
    u32 frames_since_inactive = 0;
};

struct WaterRenderData {
    u32 vao_id = 0;
    u32 vbo_id = 0;
    u32 ebo_id = 0;
    u32 element_count = 0;
    u32 mesh_version = 0;
    u32 frames_since_inactive = 0;
};

struct FrameBufferObject {
    u32 fbo_id = 0;
    u32 color_texture = 0;
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
    RenderPipeline();
    ~RenderPipeline();

    bool startup(u32 screen_width, u32 screen_height, const std::filesystem::path& root_path);
    void render_frame(entt::registry& registry, Systems::SHIELD_WorldSystem& world_system, const Camera& camera, float deltaTime, bool wireframe = false);
    void on_resize(u32 new_width, u32 new_height);
    void clear_all_chunk_data(); // Force clear all cached chunk render data
    
    // GPU SDF integration
    void SetupGPUSDFIntegration(Systems::SHIELD_WorldSystem& world_system);

private:
    struct ChunkMeshSnapshot {
        ChunkID id = 0;
        IVec3 coords{};
        u32 mesh_version = 0;
        std::vector<VoxelVertex> mesh_vertices;
        std::vector<u32> mesh_indices;
        std::vector<VoxelVertex> water_mesh_vertices;
        std::vector<u32> water_mesh_indices;

        bool has_terrain_mesh() const { return !mesh_vertices.empty() && !mesh_indices.empty(); }
        bool has_water_mesh() const { return !water_mesh_vertices.empty() && !water_mesh_indices.empty(); }
    };

    std::vector<ChunkMeshSnapshot> build_chunk_snapshots(const std::vector<Chunk*>& renderable_chunks) const;

    void manage_chunk_gpu_resources(const std::vector<ChunkMeshSnapshot>& renderable_chunks);
    void upload_chunk_mesh(const ChunkMeshSnapshot& chunk);
    void unload_chunk_resources(ChunkID chunk_id);

    void shadow_pass(const std::vector<ChunkMeshSnapshot>& renderable_chunks, const Camera& camera);
    void ssao_pass(const Camera& camera);
    void ssao_blur_pass();
    void lighting_pass(const Camera& camera);
    void skybox_pass(const Camera& camera);

    void manage_water_gpu_resources(const std::vector<ChunkMeshSnapshot>& renderable_chunks);
    void upload_water_mesh(const ChunkMeshSnapshot& chunk);
    void unload_water_resources(ChunkID chunk_id);

    void init_gbuffer(u32 width, u32 height);
    void destroy_gbuffer();
    void init_shadow_map();
    void destroy_shadow_map();
    void init_shaders();
    void init_skybox();
    void init_screen_quad();
    void init_ssao();
    void destroy_ssao();
    void cleanup_gpu_resources();

    void init_lighting_fbo(u32 width, u32 height);
    void gbuffer_pass(entt::registry& registry, const std::vector<ChunkMeshSnapshot>& renderable_chunks, const Camera& camera, const glm::vec4 frustum_planes[6]);
    void water_pass(const std::vector<ChunkMeshSnapshot>& renderable_chunks, const Camera& camera);
    FrameBufferObject m_lighting_fbo;
    
    std::vector<glm::mat4> get_light_space_matrices(const Camera& camera);

    void update_time_of_day(float deltaTime);
    float m_timeOfDay = 0.5f; // Start at sunrise
    float m_dayDurationSeconds = 60.0f;

    u32 m_screen_width = 0;
    u32 m_screen_height = 0;
    std::filesystem::path m_root_path;

    std::unique_ptr<Shader> m_geometry_shader;
    std::unique_ptr<Shader> m_lighting_shader;
    std::unique_ptr<Shader> m_skybox_shader;
    std::unique_ptr<Shader> m_shadow_shader;
    std::unique_ptr<Shader> m_water_shader;

    DirectionalLight m_sun;
    glm::vec3 m_moonDirection;
    glm::vec3 m_skyAmbientColor;
    GBuffer m_gbuffer;
    ShadowMap m_shadow_map;
    SSAOData m_ssao;

    std::unordered_map<ChunkID, ChunkRenderData> m_chunk_render_data;
    std::unordered_map<ChunkID, WaterRenderData> m_water_render_data;

    u32 m_screen_quad_vao = 0;
    u32 m_screen_quad_vbo = 0;
    u32 m_skybox_vao = 0;
    u32 m_skybox_vbo = 0;

    void geometry_pass_chunks(const std::vector<ChunkMeshSnapshot>& renderable_chunks, const Camera& camera, const glm::vec4 frustum_planes[6]);
    void geometry_pass_static_meshes(entt::registry& registry, const Camera& camera, const glm::vec4 frustum_planes[6]);
    
    std::unique_ptr<Shader> m_instanced_static_mesh_shader;
    
    std::map<std::string, std::unique_ptr<Mesh>> m_meshCache;
    GLuint m_instanceMatrixVBO = 0;

    u32 m_terrainTextureArray = 0;
    u32 m_materialLUT = 0;

    void init_terrain_textures();
    void init_material_lut();
    
    // --- GPU SDF Generation ---
    struct GPUSDFSystem {
        GLuint compute_program = 0;
        GLuint sdf_buffer = 0;
        GLuint terrain_noise_texture = 0;
        GLuint cave_noise_texture = 0;
        GLuint island_mask_texture = 0;
        bool initialized = false;
        
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
    
    struct CullingNode {
        AABB bounds;
        std::vector<const ChunkMeshSnapshot*> chunks;
        std::unique_ptr<CullingNode> children[4]; // Quadtree (X-Z plane)
        bool is_leaf = true;
        
        CullingNode() = default;
        CullingNode(const AABB& bounds) : bounds(bounds) {}
    };
    
    class HierarchicalCuller {
    public:
        void BuildHierarchy(const std::vector<ChunkMeshSnapshot>& chunks);
        void CullRecursive(const glm::vec4 frustum_planes[6], CullingNode* node, std::vector<const ChunkMeshSnapshot*>& visible);
        void CullHierarchical(const glm::vec4 frustum_planes[6], std::vector<const ChunkMeshSnapshot*>& visible);
        void Clear();
        
        std::unique_ptr<CullingNode> m_root; // Made public for access
        
    private:
        static constexpr int MAX_CHUNKS_PER_NODE = 8;
        static constexpr int MAX_DEPTH = 4;
        
        void BuildRecursive(CullingNode* node, const std::vector<const ChunkMeshSnapshot*>& chunks, int depth);
        bool AABBFrustumCulled(const AABB& aabb, const glm::vec4 frustum_planes[6]);
    };
    
    HierarchicalCuller m_hierarchicalCuller;

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
