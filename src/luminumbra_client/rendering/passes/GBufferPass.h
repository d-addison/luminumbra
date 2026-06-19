#pragma once

#include "../RenderPipeline.h"

#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace Luminumbra::Rendering {

class Camera;
class Shader;

// Deferred geometry (G-Buffer) render pass extracted from RenderPipeline
// (T-I2-11c). Owns the G-Buffer FBO/attachments, the terrain geometry
// shader, the instanced static-mesh shader, the static-mesh cache, and the
// instance matrix VBO. The pipeline keeps orchestration order, the shared
// chunk GPU slots, the culling hierarchy, the material LUT, stats
// collection, and the GPU timer issue/collect calls.
class GBufferPass {
public:
    GBufferPass();
    ~GBufferPass();

    void init_geometry_shader(const std::filesystem::path& root_path);
    void init_instanced_static_mesh(const std::filesystem::path& root_path);
    // T-I3-16: non-instanced skinned draw stage (skinned_mesh.vert +
    // g_buffer.frag) with a joint-palette SSBO fed by the T-I3-15 runtime.
    void init_skinned_mesh(const std::filesystem::path& root_path);
    void init_gbuffer(u32 width, u32 height);
    void destroy_gbuffer();
    void destroy_instanced_static_mesh();
    void destroy_skinned_mesh();
    void reset_shaders();

    void execute(RenderPipeline& pipeline,
                 entt::registry& registry,
                 const std::vector<RenderPipeline::ChunkMeshSnapshot>& renderable_chunks,
                 const Camera& camera,
                 const glm::vec4 frustum_planes[6]);

    GBuffer& gbuffer() { return m_gbuffer; }
    const GBuffer& gbuffer() const { return m_gbuffer; }
    const std::unique_ptr<Shader>& geometry_shader() const { return m_geometry_shader; }
    const std::unique_ptr<Shader>& instanced_static_mesh_shader() const { return m_instanced_static_mesh_shader; }
    const std::unique_ptr<Shader>& skinned_mesh_shader() const { return m_skinned_mesh_shader; }
    GLuint instance_matrix_vbo() const { return m_instanceMatrixVBO; }
    GLuint joint_palette_ssbo() const { return m_jointPaletteSSBO; }

    // Register a runtime-built mesh under a synthetic key (e.g. "procgen://tree_3_leaf") so the
    // instanced static-mesh path + Track-B LOD can draw it -- the vast procedural-tree palette.
    void register_cached_mesh(const std::string& key, std::unique_ptr<Mesh> mesh);
    [[nodiscard]] bool has_cached_mesh(const std::string& key) const;

private:
    void geometry_pass_chunks(RenderPipeline& pipeline,
                              const std::vector<RenderPipeline::ChunkMeshSnapshot>& renderable_chunks,
                              const Camera& camera,
                              const glm::vec4 frustum_planes[6]);
    void geometry_pass_static_meshes(RenderPipeline& pipeline,
                                     entt::registry& registry,
                                     const Camera& camera,
                                     const glm::vec4 frustum_planes[6]);
    void geometry_pass_skinned_meshes(RenderPipeline& pipeline,
                                      entt::registry& registry,
                                      const Camera& camera,
                                      const glm::vec4 frustum_planes[6]);

    GBuffer m_gbuffer;
    std::unique_ptr<Shader> m_geometry_shader;
    std::unique_ptr<Shader> m_instanced_static_mesh_shader;
    std::unique_ptr<Shader> m_skinned_mesh_shader;
    std::map<std::string, std::unique_ptr<Mesh>> m_meshCache;
    std::map<std::string, std::unique_ptr<Mesh>> m_skinnedMeshCache;
    GLuint m_instanceMatrixVBO = 0;
    GLuint m_instanceTintVBO = 0;  // per-instance albedo tint (vast-forest colour variation)
    GLuint m_jointPaletteSSBO = 0;
    std::size_t m_jointPaletteSSBOCapacityBytes = 0;
};

} // namespace Luminumbra::Rendering
