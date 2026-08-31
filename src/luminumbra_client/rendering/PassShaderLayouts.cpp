#include "PassShaderLayouts.h"

#include <glad/glad.h>

namespace Luminumbra::Rendering {

// The single source of truth ( + ). One entry per render-pass SHADER
// that binds samplers, listing ONLY the samplers the fragment stage actually samples
// (the GL linker strips declared-but-unused uniforms; see the two NB exclusions).
//
// Sampler GL types were introspected from the res/shaders/*.frag declarations and
// cross-checked against the C++ glActiveTexture+glBindTexture bindings each pass issues
// in execute. unit=-1 everywhere: the bindings are imperative (glActiveTexture), not
// layout(binding=), so there is no post-link unit to check -- only the TYPE, which is
// what guards the "sampler2DArray bound where the shader wants sampler2D -> garbage" bug.
const std::vector<PassShaderLayout>& AllPassShaderLayouts() {
    static const std::vector<PassShaderLayout> kLayouts = [] {
        std::vector<PassShaderLayout> v;

        // --- GBufferPass ----------------------------------------------------
        // g_buffer.frag is shared by three programs (chunk / static-mesh / skinned-
        // mesh verts); the sampler set is fragment-determined and identical across
        // them, so one entry validates the layout for all three.
        v.push_back(
            {"gbuffer_geometry", "res/shaders/g_buffer.vert", "res/shaders/g_buffer.frag", [] {
                 ExpectedLayout e;
                 e.pass_name = "gbuffer_geometry";
                 e.samplers = {
                     {"u_materialLUT", GL_SAMPLER_2D, -1},
                     {"u_terrainTextures", GL_SAMPLER_2D_ARRAY, -1},
                     {"u_terrainNormals", GL_SAMPLER_2D_ARRAY, -1},
                     {"u_skinnedTextures", GL_SAMPLER_2D_ARRAY, -1},
                     {"u_terrainRoughness", GL_SAMPLER_2D_ARRAY, -1},
                 };
                 return e;
             }()});
        v.push_back({"gbuffer_impostor",
                     "res/shaders/tree_impostor.vert",
                     "res/shaders/tree_impostor.frag",
                     [] {
                         ExpectedLayout e;
                         e.pass_name = "gbuffer_impostor";
                         e.samplers = {
                             {"u_albedo", GL_SAMPLER_2D, -1},
                             {"u_normal", GL_SAMPLER_2D, -1},
                         };
                         return e;
                     }()});

        // --- SsaoPass (four programs, all off ssao.vert) ---------------------
        v.push_back({"ssao", "res/shaders/ssao.vert", "res/shaders/ssao.frag", [] {
                         ExpectedLayout e;
                         e.pass_name = "ssao";
                         e.samplers = {
                             {"gPosition", GL_SAMPLER_2D, -1},
                             {"gNormalMaterial", GL_SAMPLER_2D, -1},
                             {"u_noiseTexture", GL_SAMPLER_2D, -1},
                         };
                         return e;
                     }()});
        v.push_back({"ssao_gtao", "res/shaders/ssao.vert", "res/shaders/ssao_gtao.frag", [] {
                         ExpectedLayout e;
                         e.pass_name = "ssao_gtao";
                         e.samplers = {
                             {"gPosition", GL_SAMPLER_2D, -1},
                             {"gNormalMaterial", GL_SAMPLER_2D, -1},
                         };
                         return e;
                     }()});
        v.push_back({"ssao_blur", "res/shaders/ssao.vert", "res/shaders/ssao_blur.frag", [] {
                         ExpectedLayout e;
                         e.pass_name = "ssao_blur";
                         e.samplers = {
                             {"u_ssaoInput", GL_SAMPLER_2D, -1},
                         };
                         return e;
                     }()});
        v.push_back({"ssao_bilateral_upsample",
                     "res/shaders/ssao.vert",
                     "res/shaders/ssao_bilateral_upsample.frag",
                     [] {
                         ExpectedLayout e;
                         e.pass_name = "ssao_bilateral_upsample";
                         e.samplers = {
                             {"u_aoHalf", GL_SAMPLER_2D, -1},
                             {"gPosition", GL_SAMPLER_2D, -1},
                         };
                         return e;
                     }()});

        // --- LightingPass ---------------------------------------------------
        // NB: u_terrainTextures is bound by execute at unit 7 but the shader no
        // longer samples it (legacy tri-planar override removed) -> the GL linker
        // strips it -> intentionally NOT declared (a stripped-unused sampler would
        // make the non-vacuity assertion fail).
        v.push_back(
            {"lighting", "res/shaders/lighting_pass.vert", "res/shaders/lighting_pass.frag", [] {
                 ExpectedLayout e;
                 e.pass_name = "lighting";
                 e.samplers = {
                     {"gPosition", GL_SAMPLER_2D, -1},
                     {"gNormalMaterial", GL_SAMPLER_2D, -1},
                     {"gAlbedoRoughness", GL_SAMPLER_2D, -1},
                     {"gMetallicAO", GL_SAMPLER_2D, -1},
                     {"u_ssao", GL_SAMPLER_2D, -1},
                     {"u_materialLUT", GL_SAMPLER_2D, -1},
                     {"u_aetherField", GL_SAMPLER_2D, -1},
                     {"u_causticsTexture", GL_SAMPLER_2D, -1},
                     {"u_shadowCascades", GL_SAMPLER_2D_ARRAY, -1},
                 };
                 return e;
             }()});
        // The lazily-created lightning overlay program.
        v.push_back({"lightning_overlay",
                     "res/shaders/lightning_overlay.vert",
                     "res/shaders/lightning_overlay.frag",
                     [] {
                         ExpectedLayout e;
                         e.pass_name = "lightning_overlay";
                         e.samplers = {
                             {"u_scene", GL_SAMPLER_2D, -1},
                         };
                         return e;
                     }()});

        // --- WaterPass (the caustics_generator program binds no samplers) ----
        v.push_back({"water", "res/shaders/water.vert", "res/shaders/water.frag", [] {
                         ExpectedLayout e;
                         e.pass_name = "water";
                         e.samplers = {
                             {"u_opaque_scene_color", GL_SAMPLER_2D, -1},
                             {"u_opaque_depth", GL_SAMPLER_2D, -1},
                             {"u_normal_map", GL_SAMPLER_2D, -1},
                             {"u_flow_map", GL_SAMPLER_2D, -1},
                             {"u_caustics_texture", GL_SAMPLER_2D, -1},
                             {"u_underwater_texture", GL_SAMPLER_2D, -1},
                         };
                         return e;
                     }()});

        // --- SkyboxPass -----------------------------------------------------
        v.push_back({"skybox", "res/shaders/skybox.vert", "res/shaders/enhanced_skybox.frag", [] {
                         ExpectedLayout e;
                         e.pass_name = "skybox";
                         e.samplers = {
                             {"u_skyViewLut", GL_SAMPLER_2D, -1},
                             {"u_transmittanceLut", GL_SAMPLER_2D, -1},
                         };
                         return e;
                     }()});
        // The weather overlay reuses ssao.vert. NB: gPosition is declared + bound at
        // unit 2 but never sampled in weather_system.frag -> stripped -> excluded.
        v.push_back(
            {"weather_overlay", "res/shaders/ssao.vert", "res/shaders/weather_system.frag", [] {
                 ExpectedLayout e;
                 e.pass_name = "weather_overlay";
                 e.samplers = {
                     {"u_sceneColor", GL_SAMPLER_2D, -1},
                     {"u_sceneDepth", GL_SAMPLER_2D, -1},
                     {"gNormal", GL_SAMPLER_2D, -1},
                 };
                 return e;
             }()});

        // --- ParticlePass ---------------------------------------------------
        v.push_back({"magical_particles",
                     "res/shaders/magical_particles.vert",
                     "res/shaders/magical_particles.frag",
                     [] {
                         ExpectedLayout e;
                         e.pass_name = "magical_particles";
                         e.samplers = {
                             {"u_sceneDepth", GL_SAMPLER_2D, -1},
                         };
                         return e;
                     }()});

        // --- DebugViewPass --------------------------------------------------
        v.push_back(
            {"debug_view", "res/shaders/fullscreen_tri.vert", "res/shaders/debug_view.frag", [] {
                 ExpectedLayout e;
                 e.pass_name = "debug_view";
                 e.samplers = {
                     {"gPosition", GL_SAMPLER_2D, -1},
                     {"gNormalMaterial", GL_SAMPLER_2D, -1},
                     {"gAlbedo", GL_SAMPLER_2D, -1},
                     {"gDepth", GL_SAMPLER_2D, -1},
                 };
                 return e;
             }()});

        // --- GroundDecalPass ------------------------------------------------
        v.push_back(
            {"scent_decal", "res/shaders/fullscreen_tri.vert", "res/shaders/scent_decal.frag", [] {
                 ExpectedLayout e;
                 e.pass_name = "scent_decal";
                 e.samplers = {
                     {"gPosition", GL_SAMPLER_2D, -1},
                     {"u_scentField", GL_SAMPLER_2D, -1},
                 };
                 return e;
             }()});

        return v;
    }();
    return kLayouts;
}

const PassShaderLayout* FindPassShaderLayout(std::string_view layout_name) {
    for (const auto& entry : AllPassShaderLayouts()) {
        if (entry.layout_name == layout_name)
            return &entry;
    }
    return nullptr;
}

const ExpectedLayout* FindPassExpectedLayout(std::string_view layout_name) {
    const PassShaderLayout* entry = FindPassShaderLayout(layout_name);
    return entry ? &entry->expected : nullptr;
}

} // namespace Luminumbra::Rendering
