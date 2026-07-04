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

// Cascaded shadow-map render pass extracted from RenderPipeline (T-I2-11b).
// Owns the shadow depth FBO/texture array and the shadow shader. The pipeline
// keeps orchestration order, the shared chunk GPU slots, the culling
// hierarchy, stats collection, and the GPU timer issue/collect calls.
class ShadowPass {
public:
    ShadowPass();
    ~ShadowPass();

    void init_shader(const std::filesystem::path& root_path);
    // RENDER-12/GPU-12: the cascaded shadow atlas (layered depth array + no-color
    // FBO) is REGISTRY-OWNED (allocated/destroyed by the registry, the 014
    // pilot-gate ownership leg). The ShadowMap struct caches the owned GL ids so
    // every downstream reader is unchanged.
    void init_shadow_map(RenderResourceRegistry& registry);
    void destroy_shadow_map(RenderResourceRegistry& registry);
    void reset_shader();

    // Spec 016 (016-P2-T10): RenderContext seam. Terrain submission goes through
    // input.submit_terrain (make_terrain_submitter) once per cascade; light-space
    // matrices are precomputed at the call site. Returns per-cascade submit stats.
    std::array<TerrainSubmitStats, ShadowMap::CASCADE_COUNT> execute(const RenderContext& ctx,
                                                                     const ShadowPassInput& input);

    ShadowMap& shadow_map() { return m_shadow_map; }
    const ShadowMap& shadow_map() const { return m_shadow_map; }
    const std::unique_ptr<Shader>& shader() const { return m_shadow_shader; }

private:
    ShadowMap m_shadow_map;
    std::unique_ptr<Shader> m_shadow_shader;
};

} // namespace Luminumbra::Rendering
