#include "ShadowPass.h"

#include "../RenderResourceRegistry.h"
#include "PassGlHelpers.h"
#include "core/Log.h"
#include "rendering/Shader.h"

#include <glm/gtc/matrix_transform.hpp>

namespace Luminumbra::Rendering {

ShadowPass::ShadowPass() = default;
ShadowPass::~ShadowPass() = default;

void ShadowPass::init_shader(const std::filesystem::path& root_path) {
    m_shadow_shader = std::make_unique<Shader>((root_path / "res/shaders/shadow_map.vert").string().c_str(), (root_path / "res/shaders/shadow_map.frag").string().c_str());
    PassGl::label_gl_object(GL_PROGRAM, m_shadow_shader ? m_shadow_shader->Id() : 0u, "shader.shadow");
}

void ShadowPass::init_shadow_map(RenderResourceRegistry& registry) {
    // RENDER-12/GPU-12: allocate the layered depth array + no-color FBO THROUGH
    // the registry. The desc reproduces the exact GL parameters of the retired
    // glTexImage3D/glTexParameter calls (DEPTH_COMPONENT32F, CASCADE_COUNT layers,
    // NEAREST, clamp-to-border white border, NO compare mode) so the object is
    // parameter-identical to the pass code it replaces; the struct caches the ids.
    TextureDesc depth_desc;
    depth_desc.width = m_shadow_map.resolution;
    depth_desc.height = m_shadow_map.resolution;
    depth_desc.layers = ShadowMap::CASCADE_COUNT;
    depth_desc.internal_format = GL_DEPTH_COMPONENT32F;
    depth_desc.format = GL_DEPTH_COMPONENT;
    depth_desc.type = GL_FLOAT;
    depth_desc.min_filter = GL_NEAREST;
    depth_desc.mag_filter = GL_NEAREST;
    depth_desc.wrap_s = GL_CLAMP_TO_BORDER;
    depth_desc.wrap_t = GL_CLAMP_TO_BORDER;
    depth_desc.has_border_color = true;
    depth_desc.border_color[0] = 1.0f;
    depth_desc.border_color[1] = 1.0f;
    depth_desc.border_color[2] = 1.0f;
    depth_desc.border_color[3] = 1.0f;
    depth_desc.expected_layout = "depth_attachment";
    depth_desc.debug_label = "shadow.depth_cascades";
    m_shadow_map.depth_texture_array =
        registry.create_texture("shadow_depth_cascades", depth_desc).id;

    FboDesc fbo_desc;
    fbo_desc.attachments = {{GL_DEPTH_ATTACHMENT, "shadow_depth_cascades"}};
    fbo_desc.no_color = true;
    fbo_desc.debug_label = "shadow.fbo";
    m_shadow_map.fbo_id = registry.create_fbo("shadow_fbo", fbo_desc).id;
    if (m_shadow_map.fbo_id == 0) {
        LUMINUMBRA_CORE_ERROR("Shadow Map FBO not complete!");
    }
    PassGl::set_default_shadow_cascade_splits(m_shadow_map);
}

void ShadowPass::destroy_shadow_map(RenderResourceRegistry& registry) {
    // Ownership contract: the registry deletes the owned GL objects.
    registry.destroy_owned("shadow_fbo");
    registry.destroy_owned("shadow_depth_cascades");
    m_shadow_map.fbo_id = 0;
    m_shadow_map.depth_texture_array = 0;
    m_shadow_map.light_space_matrices.clear();
    m_shadow_map.cascade_splits.clear();
}

void ShadowPass::reset_shader() {
    m_shadow_shader.reset();
}

std::array<TerrainSubmitStats, ShadowMap::CASCADE_COUNT> ShadowPass::execute(const RenderContext& ctx,
                                                                            const ShadowPassInput& input) {
    (void)ctx; // Shadow reads no RenderContext fields; its inputs are m_shadow_map + ShadowPassInput.
    std::array<TerrainSubmitStats, ShadowMap::CASCADE_COUNT> cascade_stats{};
    if (!m_shadow_shader || m_shadow_map.fbo_id == 0 || m_shadow_map.depth_texture_array == 0) {
        LUMINUMBRA_CORE_ERROR("Shadow pass skipped because shadow resources are not initialized.");
        return cascade_stats;
    }

    const std::vector<glm::mat4>& light_space_matrices = input.light_space_matrices;
    if (light_space_matrices.size() < ShadowMap::CASCADE_COUNT) {
        LUMINUMBRA_CORE_ERROR("Shadow pass skipped because light-space matrices could not be generated.");
        return cascade_stats;
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
        // Spec 016: ONE submit per cascade via the Codex-signed-off callback
        // (reproduces CullHierarchical + draw_chunks_mdi exactly). Returns the
        // per-cascade counts; the call site folds them into stats with =/+=.
        cascade_stats[i] = input.submit_terrain(cascade_planes);
    }
    glCullFace(GL_BACK);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    return cascade_stats;
}

} // namespace Luminumbra::Rendering
