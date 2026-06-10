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
    void init_gbuffer(u32 width, u32 height);
    void destroy_gbuffer();
    void destroy_instanced_static_mesh();
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
    GLuint instance_matrix_vbo() const { return m_instanceMatrixVBO; }

private:
    void geometry_pass_chunks(RenderPipeline& pipeline,
                              const std::vector<RenderPipeline::ChunkMeshSnapshot>& renderable_chunks,
                              const Camera& camera,
                              const glm::vec4 frustum_planes[6]);
    void geometry_pass_static_meshes(RenderPipeline& pipeline,
                                     entt::registry& registry,
                                     const Camera& camera,
                                     const glm::vec4 frustum_planes[6]);

    GBuffer m_gbuffer;
    std::unique_ptr<Shader> m_geometry_shader;
    std::unique_ptr<Shader> m_instanced_static_mesh_shader;
    std::map<std::string, std::unique_ptr<Mesh>> m_meshCache;
    GLuint m_instanceMatrixVBO = 0;
};

} // namespace Luminumbra::Rendering
