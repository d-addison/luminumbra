#include "ShadowPass.h"

#include "PassGlHelpers.h"
#include "core/Log.h"
#include "rendering/Camera.h"
#include "rendering/Shader.h"
#include "luminumbra_common/world/Chunk.h"

#include <glm/gtc/matrix_transform.hpp>

namespace Luminumbra::Rendering {

ShadowPass::ShadowPass() = default;
ShadowPass::~ShadowPass() = default;

void ShadowPass::init_shader(const std::filesystem::path& root_path) {
    m_shadow_shader = std::make_unique<Shader>((root_path / "res/shaders/shadow_map.vert").string().c_str(), (root_path / "res/shaders/shadow_map.frag").string().c_str());
    PassGl::label_gl_object(GL_PROGRAM, m_shadow_shader ? m_shadow_shader->Id() : 0u, "shader.shadow");
}

void ShadowPass::init_shadow_map() {
    glGenFramebuffers(1, &m_shadow_map.fbo_id);
    glGenTextures(1, &m_shadow_map.depth_texture_array);
    PassGl::label_gl_object(GL_FRAMEBUFFER, m_shadow_map.fbo_id, "shadow.fbo");
    PassGl::label_gl_object(GL_TEXTURE, m_shadow_map.depth_texture_array, "shadow.depth_cascades");
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
    PassGl::set_default_shadow_cascade_splits(m_shadow_map);
}

void ShadowPass::destroy_shadow_map() {
    if (m_shadow_map.fbo_id) { glDeleteFramebuffers(1, &m_shadow_map.fbo_id); m_shadow_map.fbo_id = 0; }
    if (m_shadow_map.depth_texture_array) { glDeleteTextures(1, &m_shadow_map.depth_texture_array); m_shadow_map.depth_texture_array = 0; }
    m_shadow_map.light_space_matrices.clear();
    m_shadow_map.cascade_splits.clear();
}

void ShadowPass::reset_shader() {
    m_shadow_shader.reset();
}

void ShadowPass::execute(RenderPipeline& pipeline,
                         const std::vector<RenderPipeline::ChunkMeshSnapshot>& renderable_chunks,
                         const Camera& camera) {
    if (!m_shadow_shader || m_shadow_map.fbo_id == 0 || m_shadow_map.depth_texture_array == 0) {
        LUMINUMBRA_CORE_ERROR("Shadow pass skipped because shadow resources are not initialized.");
        return;
    }

    auto light_space_matrices = pipeline.get_light_space_matrices(camera);
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
    // T-I4-16: the shadow cascades draw the SAME live terrain chunks as the
    // G-buffer pass, now via glMultiDrawElementsIndirect from the shared pool.
    // The chunk world origin reaches shadow_map.vert through the instanced
    // aOrigin attribute (u_useInstanceOrigin == 1); the legacy per-chunk u_model
    // uniform path is left compiled but unused for live chunks.
    m_shadow_shader->setInt("u_useInstanceOrigin", 1);
    for (int i = 0; i < ShadowMap::CASCADE_COUNT; ++i) {
        glFramebufferTextureLayer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, m_shadow_map.depth_texture_array, 0, i);
        m_shadow_shader->setMat4("u_lightSpaceMatrix", light_space_matrices[i]);

        glm::vec4 cascade_planes[6];
        PassGl::ExtractFrustumPlanes(light_space_matrices[i], cascade_planes);
        std::vector<const RenderPipeline::ChunkCullEntry*> visible_chunks;
        visible_chunks.reserve(renderable_chunks.size());
        pipeline.m_hierarchicalCuller.CullHierarchical(cascade_planes, visible_chunks);
        pipeline.m_last_render_pass_stats.shadow_cascade_visible_chunks[i] = visible_chunks.size();

        std::size_t cascade_draws = 0;
        std::size_t cascade_indices = 0;
        pipeline.draw_chunks_mdi(visible_chunks, cascade_draws, cascade_indices);
        pipeline.m_last_render_pass_stats.shadow_cascade_draws[i] += cascade_draws;
        pipeline.m_last_render_pass_stats.shadow_draws += cascade_draws;
        pipeline.m_last_render_pass_stats.shadow_indices_drawn += cascade_indices;
    }
    glCullFace(GL_BACK);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

} // namespace Luminumbra::Rendering
