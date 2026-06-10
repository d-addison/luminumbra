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
// calls.
//
// T-I2-16a: the pass also owns a small offscreen caustics generation target
// (kCausticsResolution^2 RGBA8 texture + FBO) rendered through
// caustics_generator.frag at the start of execute(), inside the Water GPU
// timer. The caustics feed is published through black_texture(): the lighting
// pass binds that accessor as its caustics sampler, so it picks up the
// generated texture (or the black fallback before the first generated frame /
// when the caustics shader is unavailable) without any LightingPass changes.
class WaterPass {
public:
    // Resolution of the offscreen caustics generation target.
    static constexpr int kCausticsResolution = 256;

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
    // Shared caustics feed accessor (named for its historical fallback): the
    // lighting pass binds this as u_causticsTexture and the water shader as
    // u_caustics_texture. Returns the generated caustics texture when the
    // offscreen pass is available, the black fallback otherwise.
    u32 black_texture() const { return m_caustics_texture != 0 ? m_caustics_texture : m_water_black_texture; }
    // Raw black fallback texture (resource-registry accounting + foam slot).
    u32 black_fallback_texture() const { return m_water_black_texture; }
    u32 underwater_texture() const { return m_water_underwater_texture; }
    u32 caustics_texture() const { return m_caustics_texture; }
    u32 caustics_fbo() const { return m_caustics_fbo; }

private:
    void generate_caustics(RenderPipeline& pipeline);

    std::unique_ptr<Shader> m_water_shader;
    std::unique_ptr<Shader> m_caustics_shader;
    u32 m_water_flat_normal_texture = 0;
    u32 m_water_neutral_flow_texture = 0;
    u32 m_water_black_texture = 0;
    u32 m_water_underwater_texture = 0;
    u32 m_caustics_texture = 0;
    u32 m_caustics_fbo = 0;
};

} // namespace Luminumbra::Rendering
