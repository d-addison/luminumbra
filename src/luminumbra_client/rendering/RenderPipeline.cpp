#include "RenderPipeline.h"
#include "luminumbra_common/systems/SHIELD_WorldSystem.h"
#include "luminumbra_common/world/Chunk.h"
#include "core/Log.h"
#include "rendering/Shader.h"
#include "rendering/Camera.h"
#include <unordered_set>
#include <glm/gtc/matrix_transform.hpp>

namespace {

// Extracts 6 planes from a combined projection-view matrix. Each plane normalized as (xyz, w) with xyz being normal.
inline void ExtractFrustumPlanes(const glm::mat4& m, glm::vec4 planes[6]) {
    // m is projection * view
    // Left
    planes[0] = glm::vec4(m[0][3] + m[0][0], m[1][3] + m[1][0], m[2][3] + m[2][0], m[3][3] + m[3][0]);
    // Right
    planes[1] = glm::vec4(m[0][3] - m[0][0], m[1][3] - m[1][0], m[2][3] - m[2][0], m[3][3] - m[3][0]);
    // Bottom
    planes[2] = glm::vec4(m[0][3] + m[0][1], m[1][3] + m[1][1], m[2][3] + m[2][1], m[3][3] + m[3][1]);
    // Top
    planes[3] = glm::vec4(m[0][3] - m[0][1], m[1][3] - m[1][1], m[2][3] - m[2][1], m[3][3] - m[3][1]);
    // Near
    planes[4] = glm::vec4(m[0][3] + m[0][2], m[1][3] + m[1][2], m[2][3] + m[2][2], m[3][3] + m[3][2]);
    // Far
    planes[5] = glm::vec4(m[0][3] - m[0][2], m[1][3] - m[1][2], m[2][3] - m[2][2], m[3][3] - m[3][2]);

    for (int i = 0; i < 6; ++i) {
        float inv_len = 1.0f / glm::length(glm::vec3(planes[i]));
        planes[i] *= inv_len;
    }
}

// AABB vs plane test: return true if AABB is outside this plane
inline bool AABBOutsidePlane(const glm::vec3& minp, const glm::vec3& maxp, const glm::vec4& plane) {
    // get the most positive point for the plane normal (for outside test)
    glm::vec3 p = glm::vec3(
        plane.x >= 0 ? maxp.x : minp.x,
        plane.y >= 0 ? maxp.y : minp.y,
        plane.z >= 0 ? maxp.z : minp.z
    );
    float dist = plane.x * p.x + plane.y * p.y + plane.z * p.z + plane.w;
    return dist < 0.0f;
}

inline bool AABBFrustumCulled(const glm::vec3& minp, const glm::vec3& maxp, const glm::vec4 planes[6]) {
    for (int i = 0; i < 6; ++i)
        if (AABBOutsidePlane(minp, maxp, planes[i])) return true;
    return false;
}

} // anonymous

namespace Luminumbra::Rendering {

RenderPipeline::RenderPipeline() {}
RenderPipeline::~RenderPipeline() { // NEW
    cleanup_gpu_resources();
}

void RenderPipeline::startup(u32 screen_width, u32 screen_height, const std::string& root_path) {
    m_screen_width = screen_width;
    m_screen_height = screen_height;
    m_root_path = root_path;

    init_shaders();
    init_gbuffer(screen_width, screen_height);
    init_screen_quad();

    LUMINUMBRA_CORE_INFO("Render Pipeline Initialized.");
}

void RenderPipeline::render_frame(Systems::SHIELD_WorldSystem& world_system, const Rendering::Camera& camera) {
    geometry_pass(world_system, camera);
    lighting_pass(camera);
    skybox_pass(camera);
    composite_pass();
}

void RenderPipeline::on_resize(u32 new_width, u32 new_height) {
    if (new_width == 0 || new_height == 0) return;
    if (new_width == m_screen_width && new_height == m_screen_height) return;

    m_screen_width = new_width;
    m_screen_height = new_height;

    destroy_gbuffer();
    init_gbuffer(new_width, new_height);
}

void RenderPipeline::geometry_pass(Systems::SHIELD_WorldSystem& world_system, const Camera& camera) {
    LUMINUMBRA_CORE_TRACE("--- Frame Start: Geometry Pass ---");

    glBindFramebuffer(GL_FRAMEBUFFER, m_gbuffer.fbo_id);
    glViewport(0, 0, m_screen_width, m_screen_height);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glEnable(GL_DEPTH_TEST);

    m_geometry_shader->use();
    glm::mat4 projection = glm::perspective(glm::radians(camera.Zoom),
                                        (float)m_screen_width / (float)m_screen_height,
                                        0.1f, 1000.0f);
    glm::mat4 view = camera.GetViewMatrix();
    glm::mat4 vp = projection * view; // NEW
    glm::vec4 frustum_planes[6];
    ExtractFrustumPlanes(vp, frustum_planes);
    m_geometry_shader->setMat4("projection", projection);
    m_geometry_shader->setMat4("view", view);

    auto renderable_chunks = world_system.get_renderable_chunks();
    LUMINUMBRA_CORE_TRACE("Renderable chunks: {}", renderable_chunks.size());

    std::unordered_set<ChunkID> active_chunk_ids;
    active_chunk_ids.reserve(renderable_chunks.size());
    for (const auto* chunk : renderable_chunks) {
        active_chunk_ids.insert(chunk->get_id());
    }

    std::vector<ChunkID> gpu_chunks_to_unload;
    gpu_chunks_to_unload.reserve(m_chunk_render_data.size());
    for (const auto& [id, _] : m_chunk_render_data) {
        if (active_chunk_ids.find(id) == active_chunk_ids.end()) {
            gpu_chunks_to_unload.push_back(id);
        }
    }
    for (const ChunkID id : gpu_chunks_to_unload) {
        unload_chunk_resources(id);
    }

    for (auto* chunk : renderable_chunks) {
        if (chunk->mesh_vertices.empty() || chunk->mesh_indices.empty()) continue;

        if (m_chunk_render_data.find(chunk->get_id()) == m_chunk_render_data.end()) {
            ChunkRenderData data{};
            glGenVertexArrays(1, &data.vao_id);
            glGenBuffers(1, &data.vbo_id);
            glGenBuffers(1, &data.ebo_id);

            glBindVertexArray(data.vao_id);

            glBindBuffer(GL_ARRAY_BUFFER, data.vbo_id);
            glBufferData(GL_ARRAY_BUFFER,
                         chunk->mesh_vertices.size() * sizeof(Luminumbra::VoxelVertex),
                         chunk->mesh_vertices.data(), GL_STATIC_DRAW);

            glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, data.ebo_id);
            glBufferData(GL_ELEMENT_ARRAY_BUFFER,
                         chunk->mesh_indices.size() * sizeof(unsigned int),
                         chunk->mesh_indices.data(), GL_STATIC_DRAW);

            glEnableVertexAttribArray(0);
            glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(Luminumbra::VoxelVertex), (void*)0);

            glEnableVertexAttribArray(1);
            glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, sizeof(Luminumbra::VoxelVertex),
                                  (void*)offsetof(Luminumbra::VoxelVertex, normal));

            glEnableVertexAttribArray(2);
            glVertexAttribIPointer(2, 1, GL_UNSIGNED_INT, sizeof(Luminumbra::VoxelVertex),
                                   (void*)offsetof(Luminumbra::VoxelVertex, material_id));

            glBindVertexArray(0);

            data.element_count = static_cast<u32>(chunk->mesh_indices.size());
            m_chunk_render_data[chunk->get_id()] = data;
        }

        const auto& render_data = m_chunk_render_data.at(chunk->get_id());

        glm::ivec3 cc = chunk->get_coords();
        glm::vec3 base = glm::vec3(cc.x * CHUNK_SIZE_X, cc.y * CHUNK_SIZE_Y, cc.z * CHUNK_SIZE_Z);
        glm::vec3 minp = base;
        glm::vec3 maxp = base + glm::vec3(CHUNK_SIZE_X, CHUNK_SIZE_Y, CHUNK_SIZE_Z);

        if (AABBFrustumCulled(minp, maxp, frustum_planes)) {
            continue; // culled
        }

        glm::mat4 model = glm::translate(glm::mat4(1.0f), base);
        m_geometry_shader->setMat4("model", model);

        glBindVertexArray(render_data.vao_id);
        glDrawElements(GL_TRIANGLES, render_data.element_count, GL_UNSIGNED_INT, 0);
        glBindVertexArray(0);
    }

    LUMINUMBRA_CORE_TRACE("--- Frame End: Geometry Pass ---");
}

void RenderPipeline::lighting_pass(const Rendering::Camera& camera) {
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, m_screen_width, m_screen_height);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    m_lighting_shader->use();
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, m_gbuffer.position_texture);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, m_gbuffer.normal_texture);
    glActiveTexture(GL_TEXTURE2);
    glBindTexture(GL_TEXTURE_2D, m_gbuffer.albedo_texture);

    m_lighting_shader->setInt("gPosition", 0);
    m_lighting_shader->setInt("gNormal", 1);
    m_lighting_shader->setInt("gAlbedoSpec", 2);
    m_lighting_shader->setVec3("viewPos", camera.Position);

    glBindVertexArray(m_screen_quad_vao);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glBindVertexArray(0);
}

void RenderPipeline::far_field_pass(const Rendering::Camera& /*camera*/) {}
void RenderPipeline::composite_pass() {}

void RenderPipeline::skybox_pass(const Rendering::Camera& /*camera*/) {
    glBindFramebuffer(GL_READ_FRAMEBUFFER, m_gbuffer.fbo_id);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
    glBlitFramebuffer(0, 0, m_screen_width, m_screen_height,
                      0, 0, m_screen_width, m_screen_height,
                      GL_DEPTH_BUFFER_BIT, GL_NEAREST);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
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

    const GLenum attachments[3] = { GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1, GL_COLOR_ATTACHMENT2 };
    glDrawBuffers(3, attachments);

    glGenRenderbuffers(1, &m_gbuffer.depth_texture);
    glBindRenderbuffer(GL_RENDERBUFFER, m_gbuffer.depth_texture);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, width, height);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, m_gbuffer.depth_texture);

    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
        LUMINUMBRA_CORE_ERROR("G-Buffer framebuffer not complete!");

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

void RenderPipeline::destroy_gbuffer() { // NEW
    if (m_gbuffer.position_texture) { glDeleteTextures(1, &m_gbuffer.position_texture); m_gbuffer.position_texture = 0; }
    if (m_gbuffer.normal_texture)   { glDeleteTextures(1, &m_gbuffer.normal_texture);   m_gbuffer.normal_texture   = 0; }
    if (m_gbuffer.albedo_texture)   { glDeleteTextures(1, &m_gbuffer.albedo_texture);   m_gbuffer.albedo_texture   = 0; }
    if (m_gbuffer.depth_texture)    { glDeleteRenderbuffers(1, &m_gbuffer.depth_texture); m_gbuffer.depth_texture  = 0; }
    if (m_gbuffer.fbo_id)           { glDeleteFramebuffers(1, &m_gbuffer.fbo_id);       m_gbuffer.fbo_id          = 0; }
}

void RenderPipeline::init_shaders() {
    std::string gbuffer_vert_path = m_root_path + "res/shaders/g_buffer.vert";
    std::string gbuffer_frag_path = m_root_path + "res/shaders/g_buffer.frag";
    std::string lighting_vert_path = m_root_path + "res/shaders/lighting_pass.vert";
    std::string lighting_frag_path = m_root_path + "res/shaders/lighting_pass.frag";

    m_geometry_shader = std::make_unique<Shader>(gbuffer_vert_path.c_str(), gbuffer_frag_path.c_str());
    m_lighting_shader = std::make_unique<Shader>(lighting_vert_path.c_str(), lighting_frag_path.c_str());
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

void RenderPipeline::init_screen_quad() {
    const float quadVertices[] = {
        -1.0f,  1.0f, 0.0f, 0.0f, 1.0f,
        -1.0f, -1.0f, 0.0f, 0.0f, 0.0f,
         1.0f,  1.0f, 0.0f, 1.0f, 1.0f,
         1.0f, -1.0f, 0.0f, 1.0f, 0.0f,
    };

    glGenVertexArrays(1, &m_screen_quad_vao);
    glGenBuffers(1, &m_screen_quad_vbo); // store handle

    glBindVertexArray(m_screen_quad_vao);
    glBindBuffer(GL_ARRAY_BUFFER, m_screen_quad_vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(quadVertices), quadVertices, GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 5 * sizeof(float), (void*)0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 5 * sizeof(float), (void*)(3 * sizeof(float)));
    glBindVertexArray(0);
}

void RenderPipeline::cleanup_gpu_resources() {
    // chunks
    for (auto& [id, d] : m_chunk_render_data) {
        if (d.vao_id) glDeleteVertexArrays(1, &d.vao_id);
        if (d.vbo_id) glDeleteBuffers(1, &d.vbo_id);
        if (d.ebo_id) glDeleteBuffers(1, &d.ebo_id);
    }
    m_chunk_render_data.clear();

    // gbuffer
    destroy_gbuffer();

    // screen quad
    if (m_screen_quad_vao) { glDeleteVertexArrays(1, &m_screen_quad_vao); m_screen_quad_vao = 0; }
    if (m_screen_quad_vbo) { glDeleteBuffers(1, &m_screen_quad_vbo); m_screen_quad_vbo = 0; }

    // far field
    if (m_far_field_texture) { glDeleteTextures(1, &m_far_field_texture); m_far_field_texture = 0; }
    if (m_far_field_fbo)     { glDeleteFramebuffers(1, &m_far_field_fbo); m_far_field_fbo = 0; }

    // shaders
    m_geometry_shader.reset();
    m_lighting_shader.reset();
    m_far_field_shader.reset();
    m_composite_shader.reset();
    m_skybox_shader.reset();
}

} // namespace Luminumbra::Rendering