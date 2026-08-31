#pragma once

#include "../GBuffer.h"       // GBuffer attachments (value member)
#include "../RenderContext.h" // per-frame pass contract (RenderContext)
#include "../RenderInputs.h"  // GBufferPassInput / GBufferDrawStats

#include <entt/entt.hpp> // entt::registry& execute param (un-fwd-declarable typedef)
#include <glad/glad.h>   // GLuint for the owned VBO/VAO/SSBO members
#include <glm/glm.hpp>   // glm::vec3/vec4/mat4 in the cached-prop structs

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace Luminumbra::Rendering {

class Camera;
class Shader;
struct Mesh;
class RenderResourceRegistry;

// Deferred geometry (G-Buffer) render pass extracted from RenderPipeline
//. Owns the G-Buffer FBO/attachments, the terrain geometry
// shader, the instanced static-mesh shader, the static-mesh cache, and the
// instance matrix VBO. The pipeline keeps orchestration order, the shared
// chunk GPU slots, the culling hierarchy, the material LUT, stats
// collection, and the GPU timer issue/collect calls.
//
// converted to the RenderContext seam. Frame state
// (camera/screen/time/taau jitter/prev-view-proj/prev-time/material+terrain+
// skinned arrays/isolation/frustum planes) arrives via RenderContext; the
// live-terrain submit callback, far-LOD system, root path, static-model UV
// texture lane and tree-impostor bundle arrive via GBufferPassInput. No
// RenderPipeline& and no `friend` reach. entt::registry& is a separate param
// (its type is an un-fwd-declarable typedef).
class GBufferPass {
public:
    GBufferPass();
    ~GBufferPass();

    void init_geometry_shader(const std::filesystem::path& root_path);
    void init_instanced_static_mesh(const std::filesystem::path& root_path);
    // non-instanced skinned draw stage (skinned_mesh.vert +
    // g_buffer.frag) with a joint-palette SSBO fed by the  runtime.
    void init_skinned_mesh(const std::filesystem::path& root_path);
    // the G-buffer FBO + attachments are REGISTRY-OWNED
    // (allocated/resized/destroyed by the registry, the rendering pilot-gate
    // ownership leg). The GBuffer struct caches the owned GL ids so every
    // downstream reader is unchanged.
    void init_gbuffer(RenderResourceRegistry& registry, u32 width, u32 height);
    void destroy_gbuffer(RenderResourceRegistry& registry);
    void destroy_instanced_static_mesh();
    void destroy_skinned_mesh();
    void reset_shaders();

    // RenderContext seam. Returns the draw stats the
    // call site folds into RenderPassFrameStats with the exact =/+= policy
    // (terrain_visible_chunks '=', all other counters '+=').
    GBufferDrawStats
    execute(const RenderContext& ctx, entt::registry& registry, const GBufferPassInput& input);

    GBuffer& gbuffer() {
        return m_gbuffer;
    }
    const GBuffer& gbuffer() const {
        return m_gbuffer;
    }
    const std::unique_ptr<Shader>& geometry_shader() const {
        return m_geometry_shader;
    }
    const std::unique_ptr<Shader>& instanced_static_mesh_shader() const {
        return m_instanced_static_mesh_shader;
    }
    const std::unique_ptr<Shader>& skinned_mesh_shader() const {
        return m_skinned_mesh_shader;
    }
    GLuint instance_matrix_vbo() const {
        return m_instanceMatrixVBO;
    }
    // CPU submit cost of the static-prop pass on the last frame (ms).
    double last_static_prop_cpu_ms() const {
        return m_last_static_prop_cpu_ms;
    }
    GLuint joint_palette_ssbo() const {
        return m_jointPaletteSSBO;
    }

    // Register a runtime-built mesh under a synthetic key (e.g. "procgen://tree_3_leaf") so the
    // instanced static-mesh path + tree rendering LOD can draw it -- the vast procedural-tree
    // palette.
    void register_cached_mesh(const std::string& key, std::unique_ptr<Mesh> mesh);
    [[nodiscard]] bool has_cached_mesh(const std::string& key) const;

private:
    void geometry_pass_chunks(const RenderContext& ctx,
                              const GBufferPassInput& input,
                              const Camera& camera,
                              const glm::vec4 (&frustum_planes)[6],
                              GBufferDrawStats& stats);
    void geometry_pass_static_meshes(const RenderContext& ctx,
                                     const GBufferPassInput& input,
                                     entt::registry& registry,
                                     const Camera& camera,
                                     const glm::vec4 (&frustum_planes)[6]);
    void geometry_pass_skinned_meshes(const RenderContext& ctx,
                                      const GBufferPassInput& input,
                                      entt::registry& registry,
                                      const Camera& camera,
                                      const glm::vec4 (&frustum_planes)[6],
                                      GBufferDrawStats& stats);

    GBuffer m_gbuffer;
    std::unique_ptr<Shader> m_geometry_shader;
    std::unique_ptr<Shader> m_instanced_static_mesh_shader;
    std::unique_ptr<Shader> m_skinned_mesh_shader;
    std::map<std::string, std::unique_ptr<Mesh>> m_meshCache;
    std::map<std::string, std::unique_ptr<Mesh>> m_skinnedMeshCache;
    GLuint m_instanceMatrixVBO = 0;
    GLuint m_instanceTintVBO = 0; // per-instance albedo tint (vast-forest colour variation)
    //  far-field tree impostors (opt-in). One camera-facing quad per far tree, sampling the
    // RenderPipeline impostor atlas. Lazily initialised on first use when impostors are enabled.
    std::unique_ptr<Shader> m_tree_impostor_shader;
    GLuint m_impostorInstanceVBO = 0; // per-instance vec4 (xyz=tree base pos, w=scale)
    GLuint m_impostorVAO = 0;
    std::vector<glm::vec4> m_impostorInstances; // reused per frame (keep capacity, no realloc)
    GLuint m_jointPaletteSSBO = 0;
    std::size_t m_jointPaletteSSBOCapacityBytes = 0;

    // cached static-prop instance data. Props are scattered
    // ONCE and never move, so their model matrix + albedo tint + base-mesh hash
    // are precomputed once instead of rebuilt for ALL ~84k instances every frame
    // (the measured CPU-submit bottleneck: lines that built translate*rotate*scale
    // + the leaf/bark tint hash per instance per frame). Per frame we only do
    // distance->LOD + frustum cull (still using the per-LOD resolved mesh sphere,
    // so the visible set is identical) + append into the reused per-group buffers.
    //; rebuilt when the static-mesh population changes. This flat array
    // is also the upload-once source 's GPU compute cull will consume.
    struct CachedStaticProp {
        glm::mat4 model;                // precomputed translate * rotate * scale
        glm::vec3 position;             // transform.position (distance + sphere center)
        glm::vec3 tint;                 // precomputed per-instance albedo tint
        float maxScale = 1.0f;          // max(scale.xyz) for the cull radius
        std::uint64_t baseMeshHash = 0; // fnv64(meshPath) — resolve memo / group key seed
        std::uint32_t pathIndex = 0;    // index into m_propMeshPaths (stable storage)
        std::uint32_t materialId = 0;
        //  impostors: classified ONCE at cache build (not per-frame string search). 1 = this prop
        // is a tree part replaced by the LOD3 impostor; impostorLeaf = the per-tree representative.
        bool impostorTree = false;
        bool impostorLeaf = false;
    };
    std::vector<CachedStaticProp> m_staticPropCache;
    std::vector<std::string> m_propMeshPaths;           // interned unique mesh paths
    std::size_t m_staticPropCachePopulation = SIZE_MAX; // invalidation signal
    void build_static_prop_cache(entt::registry& registry);

    // Reused across frames (avoid per-frame unordered_map / vector reallocation).
    // A group's mesh / drawPath / basePath / material are fixed by its key, so we
    // keep the metadata and only clear the per-frame mats/tints vectors.
    struct InstanceBatchCached {
        Mesh* mesh = nullptr;
        std::string drawPath;
        std::string basePath;
        std::uint32_t materialId = 0;
        std::vector<glm::mat4> mats;
        std::vector<glm::vec3> tints;
        bool active = false; // appeared this frame
    };
    std::unordered_map<std::uint64_t, InstanceBatchCached> m_visibleGroups;
    std::unordered_map<std::uint64_t, std::pair<Mesh*, std::string>> m_resolveMemo; // persistent
    double m_last_static_prop_cpu_ms = 0.0; // CPU submit cost, last frame
};

} // namespace Luminumbra::Rendering
