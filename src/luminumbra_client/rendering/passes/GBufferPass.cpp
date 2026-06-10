#include "GBufferPass.h"

#include "PassGlHelpers.h"
#include "core/Log.h"
#include "rendering/Camera.h"
#include "rendering/Shader.h"
#include "rendering/Mesh.h"
#include "luminumbra_common/components/CoreComponents.h"
#include "luminumbra_common/world/Chunk.h"

#include <glm/gtc/matrix_transform.hpp>

namespace Luminumbra::Rendering {

GBufferPass::GBufferPass() = default;
GBufferPass::~GBufferPass() = default;

void GBufferPass::init_geometry_shader(const std::filesystem::path& root_path) {
    m_geometry_shader = std::make_unique<Shader>((root_path / "res/shaders/g_buffer.vert").string().c_str(), (root_path / "res/shaders/g_buffer.frag").string().c_str());
    PassGl::label_gl_object(GL_PROGRAM, m_geometry_shader ? m_geometry_shader->Id() : 0u, "shader.geometry");
}

void GBufferPass::init_instanced_static_mesh(const std::filesystem::path& root_path) {
    std::string instanced_vert_path = (root_path / "res/shaders/instanced_mesh.vert").string();
    std::string gbuffer_frag_path = (root_path / "res/shaders/g_buffer.frag").string();
    m_instanced_static_mesh_shader = std::make_unique<Shader>(instanced_vert_path.c_str(), gbuffer_frag_path.c_str());
    PassGl::label_gl_object(GL_PROGRAM, m_instanced_static_mesh_shader ? m_instanced_static_mesh_shader->Id() : 0u, "shader.instanced_static_mesh");
    glGenBuffers(1, &m_instanceMatrixVBO);
    PassGl::label_gl_object(GL_BUFFER, m_instanceMatrixVBO, "static_mesh.instance_matrices");
    glBindBuffer(GL_ARRAY_BUFFER, m_instanceMatrixVBO);
    glBufferData(GL_ARRAY_BUFFER, 10000 * sizeof(glm::mat4), nullptr, GL_DYNAMIC_DRAW);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
}

void GBufferPass::init_gbuffer(u32 width, u32 height) {
    glGenFramebuffers(1, &m_gbuffer.fbo_id);
    PassGl::label_gl_object(GL_FRAMEBUFFER, m_gbuffer.fbo_id, "gbuffer.fbo");
    glBindFramebuffer(GL_FRAMEBUFFER, m_gbuffer.fbo_id);

    // Position: full view-space position for deferred lighting, SSAO, and material projection.
    glGenTextures(1, &m_gbuffer.position_texture);
    PassGl::label_gl_object(GL_TEXTURE, m_gbuffer.position_texture, "gbuffer.position");
    glBindTexture(GL_TEXTURE_2D, m_gbuffer.position_texture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB16F, width, height, 0, GL_RGB, GL_FLOAT, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, m_gbuffer.position_texture, 0);

    // Normal/Material: RGBA8 (4 bytes/pixel -> octahedral normal + material ID)
    glGenTextures(1, &m_gbuffer.normal_texture);
    PassGl::label_gl_object(GL_TEXTURE, m_gbuffer.normal_texture, "gbuffer.normal_material");
    glBindTexture(GL_TEXTURE_2D, m_gbuffer.normal_texture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT1, GL_TEXTURE_2D, m_gbuffer.normal_texture, 0);

    // Albedo/Roughness: RGBA8 (4 bytes/pixel -> RGB albedo + roughness)
    glGenTextures(1, &m_gbuffer.albedo_texture);
    PassGl::label_gl_object(GL_TEXTURE, m_gbuffer.albedo_texture, "gbuffer.albedo_roughness");
    glBindTexture(GL_TEXTURE_2D, m_gbuffer.albedo_texture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT2, GL_TEXTURE_2D, m_gbuffer.albedo_texture, 0);

    // Metallic/AO: RG16F (4 bytes/pixel -> metallic + ambient occlusion)
    glGenTextures(1, &m_gbuffer.material_texture);
    PassGl::label_gl_object(GL_TEXTURE, m_gbuffer.material_texture, "gbuffer.metallic_ao");
    glBindTexture(GL_TEXTURE_2D, m_gbuffer.material_texture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RG16F, width, height, 0, GL_RG, GL_FLOAT, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT3, GL_TEXTURE_2D, m_gbuffer.material_texture, 0);

    const GLenum attachments[4] = { GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1, GL_COLOR_ATTACHMENT2, GL_COLOR_ATTACHMENT3 };
    glDrawBuffers(4, attachments);

    // Depth texture (unchanged)
    glGenTextures(1, &m_gbuffer.depth_texture);
    PassGl::label_gl_object(GL_TEXTURE, m_gbuffer.depth_texture, "gbuffer.depth");
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

void GBufferPass::destroy_gbuffer() {
    if (m_gbuffer.fbo_id) { glDeleteFramebuffers(1, &m_gbuffer.fbo_id); m_gbuffer.fbo_id = 0; }
    if (m_gbuffer.position_texture) { glDeleteTextures(1, &m_gbuffer.position_texture); m_gbuffer.position_texture = 0; }
    if (m_gbuffer.normal_texture) { glDeleteTextures(1, &m_gbuffer.normal_texture); m_gbuffer.normal_texture = 0; }
    if (m_gbuffer.albedo_texture) { glDeleteTextures(1, &m_gbuffer.albedo_texture); m_gbuffer.albedo_texture = 0; }
    if (m_gbuffer.material_texture) { glDeleteTextures(1, &m_gbuffer.material_texture); m_gbuffer.material_texture = 0; }
    if (m_gbuffer.depth_texture) { glDeleteTextures(1, &m_gbuffer.depth_texture); m_gbuffer.depth_texture = 0; }
}

void GBufferPass::destroy_instanced_static_mesh() {
    if (m_instanceMatrixVBO) { glDeleteBuffers(1, &m_instanceMatrixVBO); m_instanceMatrixVBO = 0; }
}

void GBufferPass::reset_shaders() {
    m_geometry_shader.reset();
    m_instanced_static_mesh_shader.reset();
}

void GBufferPass::execute(RenderPipeline& pipeline,
                          entt::registry& registry,
                          const std::vector<RenderPipeline::ChunkMeshSnapshot>& renderable_chunks,
                          const Camera& camera,
                          const glm::vec4 frustum_planes[6]) {
    glBindFramebuffer(GL_FRAMEBUFFER, m_gbuffer.fbo_id);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    // Pass 1: Render all the terrain chunks
    geometry_pass_chunks(pipeline, renderable_chunks, camera, frustum_planes);

    // Pass 2: Render all instanced static meshes
    geometry_pass_static_meshes(pipeline, registry, camera, frustum_planes);

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

void GBufferPass::geometry_pass_chunks(RenderPipeline& pipeline,
                                       const std::vector<RenderPipeline::ChunkMeshSnapshot>& renderable_chunks,
                                       const Camera& camera,
                                       const glm::vec4 frustum_planes[6]) {
    m_geometry_shader->use();
    glm::mat4 projection = glm::perspective(glm::radians(camera.Zoom), (float)pipeline.m_screen_width / (float)pipeline.m_screen_height, camera.GetNearPlane(), camera.GetFarPlane());
    glm::mat4 view = camera.GetViewMatrix();

    m_geometry_shader->setMat4("projection", projection);
    m_geometry_shader->setMat4("view", view);

    // Bind material LUT for G-Buffer pass
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, pipeline.m_materialLUT);
    m_geometry_shader->setInt("u_materialLUT", 0);

    // Perform hierarchical frustum culling
    std::vector<const RenderPipeline::ChunkCullEntry*> visible_chunks;
    visible_chunks.reserve(renderable_chunks.size());
    pipeline.m_hierarchicalCuller.CullHierarchical(frustum_planes, visible_chunks);
    pipeline.m_last_render_pass_stats.terrain_visible_chunks = visible_chunks.size();

    // Render visible chunks
    for (const auto* chunk : visible_chunks) {
        if (pipeline.m_chunk_render_data.find(chunk->id) == pipeline.m_chunk_render_data.end()) continue;

        const auto& render_data = pipeline.m_chunk_render_data.at(chunk->id);
        if (render_data.element_count == 0) continue;

        glm::ivec3 cc = chunk->coords;
        glm::vec3 min_aabb(cc.x * CHUNK_SIZE_X, cc.y * CHUNK_SIZE_Y, cc.z * CHUNK_SIZE_Z);

        glm::mat4 model = glm::translate(glm::mat4(1.0f), min_aabb);
        glm::mat3 normalMatrix = glm::transpose(glm::inverse(glm::mat3(view * model)));
        m_geometry_shader->setMat4("model", model);
        m_geometry_shader->setMat3("normalMatrix", normalMatrix);

        glBindVertexArray(render_data.vao_id);
        glDrawElements(GL_TRIANGLES, render_data.element_count, GL_UNSIGNED_INT, 0);
        pipeline.m_last_render_pass_stats.terrain_draws++;
        pipeline.m_last_render_pass_stats.terrain_indices_drawn += render_data.element_count;
    }

    glBindVertexArray(0);
}

void GBufferPass::geometry_pass_static_meshes(RenderPipeline& pipeline,
                                              entt::registry& registry,
                                              const Camera& camera,
                                              const glm::vec4 frustum_planes[6]) {
    m_instanced_static_mesh_shader->use();
    m_instanced_static_mesh_shader->setMat4("projection", glm::perspective(glm::radians(camera.Zoom), (float)pipeline.m_screen_width / (float)pipeline.m_screen_height, camera.GetNearPlane(), camera.GetFarPlane()));
    m_instanced_static_mesh_shader->setMat4("view", camera.GetViewMatrix());
    auto view = registry.view<const Components::TransformComponent, const Components::StaticMeshComponent>();
    std::map<std::string, std::vector<glm::mat4>> visible_instance_groups;
    for (auto entity : view) {
        auto const& transform = view.get<const Components::TransformComponent>(entity);
        auto const& mesh_info = view.get<const Components::StaticMeshComponent>(entity);
        if (m_meshCache.find(mesh_info.meshPath) == m_meshCache.end()) {
            std::string full_mesh_path = (pipeline.m_root_path / mesh_info.meshPath).string();
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

} // namespace Luminumbra::Rendering
