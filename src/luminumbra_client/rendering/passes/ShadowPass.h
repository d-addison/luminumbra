#pragma once

#include "../RenderPipeline.h"

#include <filesystem>
#include <memory>
#include <vector>

namespace Luminumbra::Rendering {

class Camera;
class Shader;

// Cascaded shadow-map render pass extracted from RenderPipeline (T-I2-11b).
// Owns the shadow depth FBO/texture array and the shadow shader. The pipeline
// keeps orchestration order, the shared chunk GPU slots, the culling
// hierarchy, stats collection, and the GPU timer issue/collect calls.
class ShadowPass {
public:
    ShadowPass();
    ~ShadowPass();

    void init_shader(const std::filesystem::path& root_path);
    void init_shadow_map();
    void destroy_shadow_map();
    void reset_shader();

    void execute(RenderPipeline& pipeline,
                 const std::vector<RenderPipeline::ChunkMeshSnapshot>& renderable_chunks,
                 const Camera& camera);

    ShadowMap& shadow_map() { return m_shadow_map; }
    const ShadowMap& shadow_map() const { return m_shadow_map; }
    const std::unique_ptr<Shader>& shader() const { return m_shadow_shader; }

private:
    ShadowMap m_shadow_map;
    std::unique_ptr<Shader> m_shadow_shader;
};

} // namespace Luminumbra::Rendering
