#pragma once

#include "../RenderPipeline.h"

#include <filesystem>
#include <memory>
#include <vector>

namespace Luminumbra::Rendering {

class Camera;
class Shader;

// Forward water render pass extracted from RenderPipeline (T-I2-11f).
// Owns the water shader and the solid-color fallback textures (flat normal,
// neutral flow, black, underwater). The pipeline keeps orchestration order,
// the shared water mesh GPU slots, lighting/G-Buffer inputs (read through
// the pass accessors), stats collection, and the GPU timer issue/collect
// calls. The black fallback texture is also consumed by the lighting pass
// as the caustics texture.
class WaterPass {
public:
    WaterPass();
    ~WaterPass();

    void init_shader(const std::filesystem::path& root_path);
    void init_water_fallback_textures();
    void destroy_water_fallback_textures();
    void reset_shader();

    void execute(RenderPipeline& pipeline,
                 const std::vector<RenderPipeline::ChunkMeshSnapshot>& renderable_chunks,
                 const Camera& camera);

    const std::unique_ptr<Shader>& shader() const { return m_water_shader; }
    u32 flat_normal_texture() const { return m_water_flat_normal_texture; }
    u32 neutral_flow_texture() const { return m_water_neutral_flow_texture; }
    u32 black_texture() const { return m_water_black_texture; }
    u32 underwater_texture() const { return m_water_underwater_texture; }

private:
    std::unique_ptr<Shader> m_water_shader;
    u32 m_water_flat_normal_texture = 0;
    u32 m_water_neutral_flow_texture = 0;
    u32 m_water_black_texture = 0;
    u32 m_water_underwater_texture = 0;
};

} // namespace Luminumbra::Rendering
