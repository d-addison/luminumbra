#pragma once

#include "../RenderContext.h"
#include "../RenderInputs.h"
#include "../ShadowMap.h"

#include <array>
#include <filesystem>
#include <memory>
#include <vector>

namespace Luminumbra::Rendering {

class Camera;
class Shader;
class RenderResourceRegistry;

// Cascaded shadow-map render pass extracted from RenderPipeline.
// Owns the shadow depth FBO/texture array and the shadow shader. The pipeline
// keeps orchestration order, the shared chunk GPU slots, the culling
// hierarchy, stats collection, and the GPU timer issue/collect calls.
class ShadowPass {
public:
    ShadowPass();
    ~ShadowPass();

    void init_shader(const std::filesystem::path& root_path);
    // the cascaded shadow atlas (layered depth array + no-color
    // FBO) is REGISTRY-OWNED (allocated/destroyed by the registry, the
    // pilot-gate ownership leg). The ShadowMap struct caches the owned GL ids so
    // every downstream reader is unchanged.
    void init_shadow_map(RenderResourceRegistry& registry);
    void destroy_shadow_map(RenderResourceRegistry& registry);
    void reset_shader();

    // RenderContext seam. Terrain submission goes through
    // input.submit_terrain (make_terrain_submitter) once per cascade; light-space
    // matrices are precomputed at the call site. Returns per-cascade submit stats.
    std::array<TerrainSubmitStats, ShadowMap::CASCADE_COUNT> execute(const RenderContext& ctx,
                                                                     const ShadowPassInput& input);

    ShadowMap& shadow_map() {
        return m_shadow_map;
    }
    const ShadowMap& shadow_map() const {
        return m_shadow_map;
    }
    const std::unique_ptr<Shader>& shader() const {
        return m_shadow_shader;
    }
    const std::unique_ptr<Shader>& tint_shader() const {
        return m_tint_shader;
    }
    // the tinted-transmission cascade array (RGBA8,
    // CASCADE_COUNT layers). Init-cleared to WHITE, so with no glass the lighting
    // pass multiplies by 1.0 — pixel-identical to the pre-C1 render.
    unsigned int tint_texture_array() const {
        return m_tint_texture_array;
    }

private:
    ShadowMap m_shadow_map;
    std::unique_ptr<Shader> m_shadow_shader;
    //  the tint cascade + its per-cascade FBO (color = tint layer,
    // depth = the SAME opaque depth layer, depth-test on / write off) + shader.
    std::unique_ptr<Shader> m_tint_shader;
    unsigned int m_tint_texture_array = 0;
    unsigned int m_tint_fbo = 0;
    // True while the tint cascades hold non-white texels — an empty glass frame
    // pays one clear to return to identity, then zero GPU cost until glass returns.
    bool m_tint_dirty = false;
};

} // namespace Luminumbra::Rendering
