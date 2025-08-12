#include "RenderPipeline.h"
#include "luminumbra_common/systems/SHIELD_WorldSystem.h"
#include "luminumbra_common/world/Chunk.h"
#include "core/Log.h"
#include "rendering/Shader.h"
#include "rendering/Camera.h"
#include <unordered_set>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/constants.hpp>
#include <random>
#include <GLFW/glfw3.h>
#include "Mesh.h"
#include "../../include/luminumbra/core/Types.h"
#include "luminumbra_common/components/CoreComponents.h"
#include <cmath>
#include "luminumbra_common/components/LightingComponents.h"

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

// --- CONSTRUCTOR / DESTRUCTOR ---

RenderPipeline::RenderPipeline() {}
RenderPipeline::~RenderPipeline() {
    cleanup_gpu_resources();
}

// --- PUBLIC INTERFACE ---

void RenderPipeline::startup(u32 screen_width, u32 screen_height, const std::filesystem::path& root_path) {
    m_screen_width = screen_width;
    m_screen_height = screen_height;
    m_root_path = root_path;

    init_shaders();
    init_gbuffer(screen_width, screen_height);
    init_shadow_map();
    init_ssao();
    init_screen_quad();
    init_skybox();
    init_terrain_textures();

    std::string instanced_vert_path = (m_root_path / "res/shaders/instanced_mesh.vert").string();
    std::string gbuffer_frag_path = (m_root_path / "res/shaders/g_buffer.frag").string();
    m_instanced_static_mesh_shader = std::make_unique<Shader>(instanced_vert_path.c_str(), gbuffer_frag_path.c_str());
    glGenBuffers(1, &m_instanceMatrixVBO);
    glBindBuffer(GL_ARRAY_BUFFER, m_instanceMatrixVBO);
    glBufferData(GL_ARRAY_BUFFER, 10000 * sizeof(glm::mat4), nullptr, GL_DYNAMIC_DRAW);
    glBindBuffer(GL_ARRAY_BUFFER, 0);

    LUMINUMBRA_CORE_INFO("Render Pipeline Initialized.");
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

void RenderPipeline::render_frame(entt::registry& registry, Systems::SHIELD_WorldSystem& world_system, const Camera& camera, float deltaTime) {
    update_time_of_day(deltaTime);
    gather_lights(registry);
    auto renderable_chunks = world_system.get_renderable_chunks();
    manage_chunk_gpu_resources(renderable_chunks);

    glEnable(GL_DEPTH_TEST);
    glEnable(GL_CULL_FACE);

    glm::mat4 projection = glm::perspective(glm::radians(camera.Zoom), (float)m_screen_width / (float)m_screen_height, camera.GetNearPlane(), camera.GetFarPlane());
    glm::mat4 view = camera.GetViewMatrix();
    glm::vec4 frustum_planes[6];
    ExtractFrustumPlanes(projection * view, frustum_planes);

    shadow_pass(renderable_chunks, camera);
    glViewport(0, 0, m_screen_width, m_screen_height);

    glBindFramebuffer(GL_FRAMEBUFFER, m_gbuffer.fbo_id);
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    geometry_pass_chunks(renderable_chunks, camera, frustum_planes);
    geometry_pass_static_meshes(registry, camera, frustum_planes);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glDisable(GL_CULL_FACE);
    ssao_pass(camera);
    ssao_blur_pass();

    lighting_pass(camera);

    glBindFramebuffer(GL_READ_FRAMEBUFFER, m_gbuffer.fbo_id);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
    glBlitFramebuffer(0, 0, m_screen_width, m_screen_height, 0, 0, m_screen_width, m_screen_height, GL_DEPTH_BUFFER_BIT, GL_NEAREST);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);

    skybox_pass(camera);
}

void RenderPipeline::on_resize(u32 new_width, u32 new_height) {
    if (new_width == 0 || new_height == 0 || (new_width == m_screen_width && new_height == m_screen_height)) return;
    m_screen_width = new_width;
    m_screen_height = new_height;
    destroy_gbuffer();
    init_gbuffer(new_width, new_height);
    destroy_ssao();
    init_ssao();
}

// --- RENDER PASSES ---

void RenderPipeline::geometry_pass_chunks(const std::vector<Luminumbra::Chunk*>& renderable_chunks, const Camera& camera, const glm::vec4 frustum_planes[6]) {
    m_geometry_shader->use();
    glm::mat4 projection = glm::perspective(glm::radians(camera.Zoom), (float)m_screen_width / (float)m_screen_height, camera.GetNearPlane(), camera.GetFarPlane());
    glm::mat4 view = camera.GetViewMatrix();

    m_geometry_shader->setMat4("projection", projection);
    m_geometry_shader->setMat4("view", view);

    for (auto* chunk : renderable_chunks) {
        if (m_chunk_render_data.find(chunk->get_id()) == m_chunk_render_data.end()) continue;

        const auto& render_data = m_chunk_render_data.at(chunk->get_id());
        if (render_data.element_count == 0) continue;
        
        glm::ivec3 cc = chunk->get_coords();
        glm::vec3 min_aabb(cc.x * CHUNK_SIZE_X, cc.y * CHUNK_SIZE_Y, cc.z * CHUNK_SIZE_Z);
        glm::vec3 max_aabb = min_aabb + glm::vec3(CHUNK_SIZE_X, CHUNK_SIZE_Y, CHUNK_SIZE_Z);
        
        if (AABBFrustumCulled(min_aabb, max_aabb, frustum_planes)) continue;

        glm::mat4 model = glm::translate(glm::mat4(1.0f), min_aabb);

        // ===================== FIX STARTS HERE =====================
        // Calculate the normal matrix on the CPU
        glm::mat3 normalMatrix = glm::transpose(glm::inverse(glm::mat3(view * model)));
        
        // Pass it to the shader as a uniform
        m_geometry_shader->setMat4("model", model);
        m_geometry_shader->setMat3("normalMatrix", normalMatrix); // <-- NEW UNIFORM
        // ====================== FIX ENDS HERE ======================

        glBindVertexArray(render_data.vao_id);
        glDrawElements(GL_TRIANGLES, render_data.element_count, GL_UNSIGNED_INT, 0);
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

void RenderPipeline::shadow_pass(const std::vector<Luminumbra::Chunk*>& renderable_chunks, const Camera& camera) {
    auto light_space_matrices = get_light_space_matrices(camera);
    m_shadow_map.light_space_matrices = light_space_matrices;
    glViewport(0, 0, m_shadow_map.resolution, m_shadow_map.resolution);
    glBindFramebuffer(GL_FRAMEBUFFER, m_shadow_map.fbo_id);
    glClear(GL_DEPTH_BUFFER_BIT);
    glCullFace(GL_FRONT);
    m_shadow_shader->use();
    for (int i = 0; i < ShadowMap::CASCADE_COUNT; ++i) {
        glFramebufferTextureLayer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, m_shadow_map.depth_texture_array, 0, i);
        m_shadow_shader->setMat4("u_lightSpaceMatrix", light_space_matrices[i]);
        for (auto* chunk : renderable_chunks) {
            if (m_chunk_render_data.count(chunk->get_id()) == 0) continue;
            const auto& render_data = m_chunk_render_data.at(chunk->get_id());
            glm::ivec3 cc = chunk->get_coords();
            glm::vec3 base(cc.x * CHUNK_SIZE_X, cc.y * CHUNK_SIZE_Y, cc.z * CHUNK_SIZE_Z);
            glm::mat4 model = glm::translate(glm::mat4(1.0f), base);
            m_shadow_shader->setMat4("u_model", model);
            glBindVertexArray(render_data.vao_id);
            glDrawElements(GL_TRIANGLES, render_data.element_count, GL_UNSIGNED_INT, 0);
        }
    }
    glCullFace(GL_BACK);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

void RenderPipeline::lighting_pass(const Camera& camera) {
    glViewport(0, 0, m_screen_width, m_screen_height);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    m_lighting_shader->use();
    glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D, m_gbuffer.position_texture);
    glActiveTexture(GL_TEXTURE1); glBindTexture(GL_TEXTURE_2D, m_gbuffer.normal_texture);
    glActiveTexture(GL_TEXTURE2); glBindTexture(GL_TEXTURE_2D, m_gbuffer.albedo_texture);
    glActiveTexture(GL_TEXTURE3); glBindTexture(GL_TEXTURE_2D, m_gbuffer.material_texture);
    glActiveTexture(GL_TEXTURE4); glBindTexture(GL_TEXTURE_2D_ARRAY, m_shadow_map.depth_texture_array);
    glActiveTexture(GL_TEXTURE5); glBindTexture(GL_TEXTURE_2D, m_ssao.ssaoColorBufferBlur);
    glActiveTexture(GL_TEXTURE6); glBindTexture(GL_TEXTURE_2D_ARRAY, m_terrainTextureArray);
    m_lighting_shader->setMat4("u_inverseView", glm::inverse(camera.GetViewMatrix()));
    m_lighting_shader->setInt("gPosition", 0);
    m_lighting_shader->setInt("gNormal", 1);
    m_lighting_shader->setInt("gAlbedo", 2);
    m_lighting_shader->setInt("gMaterial", 3);
    m_lighting_shader->setInt("u_shadowCascades", 4);
    m_lighting_shader->setInt("u_ssao", 5);
    m_lighting_shader->setInt("u_terrainTextures", 6);
    m_lighting_shader->setVec3("u_skyAmbientColor", m_skyAmbientColor);
    m_lighting_shader->setVec3("u_viewPos", camera.Position);
    m_lighting_shader->setVec3("u_sun.direction", m_sun.direction);
    m_lighting_shader->setVec3("u_sun.color", m_sun.color);
    m_lighting_shader->setInt("u_pointLightCount", static_cast<int>(m_point_lights_this_frame.size()));
    for(size_t i = 0; i < m_point_lights_this_frame.size(); ++i) {
        std::string prefix = "u_pointLights[" + std::to_string(i) + "].";
        m_lighting_shader->setVec3(prefix + "position", m_point_lights_this_frame[i].position);
        m_lighting_shader->setVec3(prefix + "color", m_point_lights_this_frame[i].color);
        m_lighting_shader->setFloat(prefix + "radius", m_point_lights_this_frame[i].radius);
        m_lighting_shader->setFloat(prefix + "intensity", m_point_lights_this_frame[i].intensity);
    }
    m_lighting_shader->setFloat("u_farPlane", camera.GetFarPlane());
    m_lighting_shader->setVec4("u_cascadeSplits", glm::vec4(m_shadow_map.cascade_splits[1], m_shadow_map.cascade_splits[2], m_shadow_map.cascade_splits[3], m_shadow_map.cascade_splits[4]));
    for (int i = 0; i < ShadowMap::CASCADE_COUNT; ++i) {
        m_lighting_shader->setMat4("u_lightSpaceMatrices[" + std::to_string(i) + "]", m_shadow_map.light_space_matrices[i]);
    }
    glm::vec3 terrainOrigin(floor(camera.Position.x / CHUNK_SIZE_X) * CHUNK_SIZE_X, 0.0f, floor(camera.Position.z / CHUNK_SIZE_Z) * CHUNK_SIZE_Z);
    m_lighting_shader->setVec3("u_terrainOrigin", terrainOrigin);
    glBindVertexArray(m_screen_quad_vao);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glBindVertexArray(0);
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
    m_ssao.ssaoShader->setInt("gNormal", 1);
    m_ssao.ssaoShader->setInt("u_noiseTexture", 2);
    for (unsigned int i = 0; i < 64; ++i)
        m_ssao.ssaoShader->setVec3("u_samples[" + std::to_string(i) + "]", m_ssao.kernel[i]);
    glm::mat4 projection = glm::perspective(glm::radians(camera.Zoom), (float)m_screen_width / (float)m_screen_height, camera.GetNearPlane(), camera.GetFarPlane());
    m_ssao.ssaoShader->setMat4("u_projection", projection);
    glBindVertexArray(m_screen_quad_vao);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
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
}

void RenderPipeline::init_gbuffer(u32 width, u32 height) {
    glGenFramebuffers(1, &m_gbuffer.fbo_id);
    glBindFramebuffer(GL_FRAMEBUFFER, m_gbuffer.fbo_id);
    glGenTextures(1, &m_gbuffer.position_texture);
    glBindTexture(GL_TEXTURE_2D, m_gbuffer.position_texture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, width, height, 0, GL_RGBA, GL_FLOAT, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, m_gbuffer.position_texture, 0);
    glGenTextures(1, &m_gbuffer.normal_texture);
    glBindTexture(GL_TEXTURE_2D, m_gbuffer.normal_texture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, width, height, 0, GL_RGBA, GL_FLOAT, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT1, GL_TEXTURE_2D, m_gbuffer.normal_texture, 0);
    glGenTextures(1, &m_gbuffer.albedo_texture);
    glBindTexture(GL_TEXTURE_2D, m_gbuffer.albedo_texture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT2, GL_TEXTURE_2D, m_gbuffer.albedo_texture, 0);
    glGenTextures(1, &m_gbuffer.material_texture);
    glBindTexture(GL_TEXTURE_2D, m_gbuffer.material_texture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT3, GL_TEXTURE_2D, m_gbuffer.material_texture, 0);
    const GLenum attachments[4] = { GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1, GL_COLOR_ATTACHMENT2, GL_COLOR_ATTACHMENT3 };
    glDrawBuffers(4, attachments);
    glGenRenderbuffers(1, &m_gbuffer.depth_rbo);
    glBindRenderbuffer(GL_RENDERBUFFER, m_gbuffer.depth_rbo);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, width, height);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, m_gbuffer.depth_rbo);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) LUMINUMBRA_CORE_ERROR("G-Buffer FBO not complete!");
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

void RenderPipeline::init_shadow_map() {
    glGenFramebuffers(1, &m_shadow_map.fbo_id);
    glGenTextures(1, &m_shadow_map.depth_texture_array);
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
    m_shadow_map.cascade_splits = { 0.1f, 15.0f, 40.0f, 100.0f, 250.0f };
}

void RenderPipeline::init_ssao() {
    glGenFramebuffers(1, &m_ssao.fbo);
    glGenFramebuffers(1, &m_ssao.blurFBO);
    glBindFramebuffer(GL_FRAMEBUFFER, m_ssao.fbo);
    glGenTextures(1, &m_ssao.ssaoColorBuffer);
    glBindTexture(GL_TEXTURE_2D, m_ssao.ssaoColorBuffer);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_R16F, m_screen_width, m_screen_height, 0, GL_RED, GL_FLOAT, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, m_ssao.ssaoColorBuffer, 0);
    glBindFramebuffer(GL_FRAMEBUFFER, m_ssao.blurFBO);
    glGenTextures(1, &m_ssao.ssaoColorBufferBlur);
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
    if (m_gbuffer.depth_rbo) { glDeleteRenderbuffers(1, &m_gbuffer.depth_rbo); m_gbuffer.depth_rbo = 0; }
}

void RenderPipeline::destroy_shadow_map() {
    if (m_shadow_map.fbo_id) { glDeleteFramebuffers(1, &m_shadow_map.fbo_id); m_shadow_map.fbo_id = 0; }
    if (m_shadow_map.depth_texture_array) { glDeleteTextures(1, &m_shadow_map.depth_texture_array); m_shadow_map.depth_texture_array = 0; }
}

void RenderPipeline::destroy_ssao() {
    if (m_ssao.fbo) { glDeleteFramebuffers(1, &m_ssao.fbo); m_ssao.fbo = 0; }
    if (m_ssao.blurFBO) { glDeleteFramebuffers(1, &m_ssao.blurFBO); m_ssao.blurFBO = 0; }
    if (m_ssao.ssaoColorBuffer) { glDeleteTextures(1, &m_ssao.ssaoColorBuffer); m_ssao.ssaoColorBuffer = 0; }
    if (m_ssao.ssaoColorBufferBlur) { glDeleteTextures(1, &m_ssao.ssaoColorBufferBlur); m_ssao.ssaoColorBufferBlur = 0; }
    if (m_ssao.noiseTexture) { glDeleteTextures(1, &m_ssao.noiseTexture); m_ssao.noiseTexture = 0; }
}

void RenderPipeline::cleanup_gpu_resources() {
    for (auto& [id, d] : m_chunk_render_data) {
        if (d.vao_id) glDeleteVertexArrays(1, &d.vao_id);
        if (d.vbo_id) glDeleteBuffers(1, &d.vbo_id);
        if (d.ebo_id) glDeleteBuffers(1, &d.ebo_id);
    }
    m_chunk_render_data.clear();
    destroy_gbuffer();
    destroy_shadow_map();
    destroy_ssao();
    if (m_screen_quad_vao) { glDeleteVertexArrays(1, &m_screen_quad_vao); m_screen_quad_vao = 0; }
    if (m_screen_quad_vbo) { glDeleteBuffers(1, &m_screen_quad_vbo); m_screen_quad_vbo = 0; }
    if (m_skybox_vao) { glDeleteVertexArrays(1, &m_skybox_vao); m_skybox_vao = 0; }
    if (m_skybox_vbo) { glDeleteBuffers(1, &m_skybox_vbo); m_skybox_vbo = 0; }
    m_geometry_shader.reset();
    m_lighting_shader.reset();
    m_skybox_shader.reset();
    m_shadow_shader.reset();
    m_ssao.ssaoShader.reset();
    m_ssao.blurShader.reset();
}

// --- RESOURCE MANAGEMENT ---

void RenderPipeline::manage_chunk_gpu_resources(const std::vector<Chunk*>& renderable_chunks) {
    // --- Configuration for the new logic ---
    const int MAX_UPLOADS_PER_FRAME = 4; // Your existing upload budget
    const u32 INACTIVE_FRAME_TTL = 15;   // Grace period: unload after 15 frames of inactivity

    // Step 1: Create a quick-lookup set of chunks that should be active this frame.
    std::unordered_set<ChunkID> active_chunk_ids;
    active_chunk_ids.reserve(renderable_chunks.size());
    for (const auto* chunk : renderable_chunks) {
        active_chunk_ids.insert(chunk->get_id());
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

    // Step 3: Upload new and updated chunk meshes, respecting the budget.
    int uploads_this_frame = 0;
    for (auto* chunk : renderable_chunks) {
        // Skip chunks that don't have a mesh ready yet.
        if (chunk->mesh_vertices.empty() || chunk->mesh_indices.empty()) continue;

        auto it = m_chunk_render_data.find(chunk->get_id());

        bool is_new = (it == m_chunk_render_data.end());
        bool is_stale = !is_new && (it->second.mesh_version != chunk->mesh_version.load());

        if (is_new || is_stale) {
            if (uploads_this_frame >= MAX_UPLOADS_PER_FRAME) {
                // Stop if we've hit our per-frame upload limit to prevent stuttering.
                break;
            }
            
            // If the mesh is stale, we must first free the old GPU resources before uploading the new version.
            if (is_stale) {
                unload_chunk_resources(chunk->get_id());
            }

            upload_chunk_mesh(*chunk);
            uploads_this_frame++;
        }
    }
}

void RenderPipeline::upload_chunk_mesh(const Chunk& chunk) {
    if (chunk.mesh_vertices.empty() || chunk.mesh_indices.empty()) { return; }
    ChunkRenderData render_data;
    render_data.element_count = static_cast<u32>(chunk.mesh_indices.size());
    render_data.mesh_version = chunk.mesh_version.load();
    glGenVertexArrays(1, &render_data.vao_id);
    glGenBuffers(1, &render_data.vbo_id);
    glGenBuffers(1, &render_data.ebo_id);
    glBindVertexArray(render_data.vao_id);
    glBindBuffer(GL_ARRAY_BUFFER, render_data.vbo_id);
    glBufferData(GL_ARRAY_BUFFER, chunk.mesh_vertices.size() * sizeof(VoxelVertex), chunk.mesh_vertices.data(), GL_STATIC_DRAW);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, render_data.ebo_id);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, chunk.mesh_indices.size() * sizeof(u32), chunk.mesh_indices.data(), GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(VoxelVertex), (void*)offsetof(VoxelVertex, position));
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, sizeof(VoxelVertex), (void*)offsetof(VoxelVertex, normal));
    glEnableVertexAttribArray(2);
    glVertexAttribIPointer(2, 1, GL_UNSIGNED_INT, sizeof(VoxelVertex), (void*)offsetof(VoxelVertex, material_id));
    glBindVertexArray(0);
    m_chunk_render_data[chunk.get_id()] = render_data;
}

void RenderPipeline::unload_chunk_resources(ChunkID chunk_id) {
    auto it = m_chunk_render_data.find(chunk_id);
    if (it != m_chunk_render_data.end()) {
        const auto& d = it->second;
        if (d.vao_id) glDeleteVertexArrays(1, &d.vao_id);
        if (d.vbo_id) glDeleteBuffers(1, &d.vbo_id);
        if (d.ebo_id) glDeleteBuffers(1, &d.ebo_id);
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

    glGenTextures(1, &m_terrainTextureArray);
    glBindTexture(GL_TEXTURE_2D_ARRAY, m_terrainTextureArray);

    // Allocate storage for the entire texture array
    glTexImage3D(GL_TEXTURE_2D_ARRAY, 0, GL_SRGB8_ALPHA8, texture_width, texture_height, layer_count, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);

    for (int i = 0; i < layer_count; ++i) {
        std::string full_path = (m_root_path / texture_paths[i]).string();
        int width, height, channels;
        stbi_set_flip_vertically_on_load(true);
        unsigned char* data = stbi_load(full_path.c_str(), &width, &height, &channels, 4); // Force 4 channels

        if (data) {
            if (width != texture_width || height != texture_height) {
                 LUMINUMBRA_CORE_ERROR("Texture '{}' has wrong dimensions!", texture_paths[i]);
                 stbi_image_free(data);
                 continue;
            }
            // Upload data to the i-th layer of the array
            glTexSubImage3D(GL_TEXTURE_2D_ARRAY, 0, 0, 0, i, width, height, 1, GL_RGBA, GL_UNSIGNED_BYTE, data);
            stbi_image_free(data);
        } else {
            LUMINUMBRA_CORE_ERROR("Failed to load texture array layer: {}", texture_paths[i]);
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

std::vector<glm::mat4> RenderPipeline::get_light_space_matrices(const Camera& camera) {
    std::vector<glm::mat4> matrices;
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