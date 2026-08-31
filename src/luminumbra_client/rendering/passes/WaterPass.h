#pragma once

#include "../RenderContext.h"
#include "../RenderInputs.h"

#include <filesystem>
#include <memory>
#include <vector>

namespace Luminumbra::Rendering {

class Camera;
class Shader;
class RenderResourceRegistry;

// Forward water render pass extracted from RenderPipeline.
// Owns the water shader and the solid-color fallback textures (flat normal,
// neutral flow, black, underwater). The pipeline keeps orchestration order,
// the shared water mesh GPU slots, lighting/G-Buffer inputs (read through
// the pass accessors), stats collection, and the GPU timer issue/collect
// calls.
//
// the pass also owns a small offscreen caustics generation target
// (kCausticsResolution^2 RGBA8 texture + FBO) rendered through
// caustics_generator.frag at the start of execute, inside the Water GPU
// timer. The caustics feed is published through black_texture: the lighting
// pass binds that accessor as its caustics sampler, so it picks up the
// generated texture (or the black fallback before the first generated frame /
// when the caustics shader is unavailable) without any LightingPass changes.
class WaterPass {
public:
    // Resolution of the offscreen caustics generation target.
    static constexpr int kCausticsResolution = 256;

    // Approximate sky reflection color for SSR rays that leave the screen
    // without hitting geometry. Tracks the enhanced skybox day
    // gradient, faded toward the night sky by the sun intensity. Exposed
    // statically so the water visual analysis can use the exact same
    // reference when correlating reflected water hue against the sky.
    static glm::vec3 approximate_sky_reflection_color(float sun_intensity) {
        const glm::vec3 day_sky(0.45f, 0.68f, 0.95f);
        const glm::vec3 night_sky(0.02f, 0.04f, 0.10f);
        const float t = sun_intensity < 0.0f ? 0.0f : (sun_intensity > 1.0f ? 1.0f : sun_intensity);
        return night_sky + (day_sky - night_sky) * t;
    }

    WaterPass();
    ~WaterPass();

    void init_shader(const std::filesystem::path& root_path);
    // the offscreen caustics generation target (256^2 RGBA8
    // texture + FBO) is REGISTRY-OWNED. The four 1x1 solid-color fallback
    // textures stay pass-owned (uploaded data, static inputs). The pass caches
    // the owned caustics ids so every reader (black_texture accessor) is unchanged.
    void init_water_fallback_textures(RenderResourceRegistry& registry);
    void destroy_water_fallback_textures(RenderResourceRegistry& registry);
    void reset_shader();

    // RenderContext seam. Draw list (WaterPassInput) is
    // built at the call site from m_water_render_data in chunk order (byte-stable);
    // returns water draw/index counts the caller folds into stats.
    WaterDrawStats
    execute(const RenderContext& ctx, const WaterPassInput& input, const Camera& camera);

    const std::unique_ptr<Shader>& shader() const {
        return m_water_shader;
    }
    u32 flat_normal_texture() const {
        return m_water_flat_normal_texture;
    }
    u32 neutral_flow_texture() const {
        return m_water_neutral_flow_texture;
    }
    // Shared caustics feed accessor (named for its historical fallback): the
    // lighting pass binds this as u_causticsTexture and the water shader as
    // u_caustics_texture. Returns the generated caustics texture when the
    // offscreen pass is available, the black fallback otherwise.
    u32 black_texture() const {
        return m_caustics_texture != 0 ? m_caustics_texture : m_water_black_texture;
    }
    // Raw black fallback texture (resource-registry accounting + foam slot).
    u32 black_fallback_texture() const {
        return m_water_black_texture;
    }
    u32 underwater_texture() const {
        return m_water_underwater_texture;
    }
    u32 caustics_texture() const {
        return m_caustics_texture;
    }
    u32 caustics_fbo() const {
        return m_caustics_fbo;
    }

private:
    void generate_caustics(const RenderContext& ctx);

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
