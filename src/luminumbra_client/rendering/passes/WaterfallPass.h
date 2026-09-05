#pragma once

#include "../RenderContext.h"
#include "../WaterfallDetect.h"

#include <filesystem>
#include <memory>
#include <vector>

namespace Luminumbra::Rendering {

class ParticlePass;
class Shader;

// Waterfall dressing pass extracted from RenderPipeline. The world-deterministic
// site cache, baked sheet geometry, shader, bake, and draw stay together under
// one owner; per-frame state arrives through RenderContext.
class WaterfallPass {
public:
    WaterfallPass();
    ~WaterfallPass();

    void init_shader(const std::filesystem::path& root_path);
    void reset_shader();
    void destroy_geometry();
    void execute(const RenderContext& ctx);

    // Bake the live waterfall DRESSING for `world` (call once after the world
    // is entered). Queries waterfall_sites(world), bakes one vertical
    // world-space quad per site into m_vao/m_vbo (drawn by the falling-sheet
    // shader), records the per-site crest/foot Y for the shader's
    // height-down-the-fall normalization, and emits a spray emitter at each
    // plunge foot (capped to bound particle cost). Dressing on a
    // world-deterministic site set — never hashed (one-way).
    void prepare(const Systems::SHIELD_WorldSystem& world,
                 ParticlePass* particle_pass,
                 const std::filesystem::path& root_path);

    const std::vector<WaterfallSite>& waterfall_sites(const Systems::SHIELD_WorldSystem& world,
                                                      const WaterfallDetectParams& params = {}) {
        return m_sites.sites_for(world, params);
    }
    WaterfallSiteCache& waterfall_cache() {
        return m_sites;
    }
    const std::unique_ptr<Shader>& shader() const {
        return m_shader;
    }

private:
    WaterfallSiteCache m_sites;
    std::unique_ptr<Shader> m_shader;
    // Baked waterfall sheet geometry. One vertical world-space quad plus one
    // plunge-pool quad (12 verts, interleaved pos+normal) per detected site,
    // all packed into one VBO. Render-only, never hashed.
    u32 m_vao = 0;
    u32 m_vbo = 0;
    std::vector<WaterfallSite> m_sheet_sites;
    bool m_geometry_built = false;
};

} // namespace Luminumbra::Rendering
