#include "FroxelPass.h"

#include "../FroxelGrid.h"
#include "../ShaderReflection.h"
#include "PassGlHelpers.h"
#include "core/Log.h"
#include "rendering/Camera.h"

#include <cmath>
#include <fstream>
#include <glm/gtc/matrix_transform.hpp>
#include <sstream>
#include <string>

namespace Luminumbra::Rendering {

// Shared with the pipeline's luminance-meter compute stage.
GLuint create_compute_program(const char* compute_source);

namespace {

bool ValidateLayout(GLuint program, const ExpectedLayout& expected) {
    const ValidationResult result =
        ValidateReflectedLayout(ReflectProgramLayout(program), expected);
    if (!result.ok) {
        LUMINUMBRA_CORE_ERROR("[shader-reflect] {} -- {}", expected.pass_name, result.diagnostic);
    } else if (result.had_warning) {
        LUMINUMBRA_CORE_WARN("[shader-reflect] {} -- {}", expected.pass_name, result.diagnostic);
    }
    return result.ok;
}

} // namespace

FroxelPass::FroxelPass() = default;
FroxelPass::~FroxelPass() = default;

bool FroxelPass::execute_inject(const RenderContext& ctx, const std::filesystem::path& root_path) {
    //  rendering: per-froxel media density + in-scatter into
    // the scatter volume, sampling the shadow depth AND the  tint cascade so
    // stained glass throws COLORED shafts. Quality 0 = zero-GL no-op.
    if (ctx.volumetric_quality <= 0) {
        return true;
    }
    if (m_inject_program == 0) {
        // Lazy init: both kernels + both 3D volumes, together.
        auto load_comp = [&](const char* rel) -> GLuint {
            std::ifstream file(std::filesystem::path(root_path) / rel);
            if (!file) {
                LUMINUMBRA_CORE_ERROR("froxel: missing {}", rel);
                return 0;
            }
            std::stringstream source;
            source << file.rdbuf();
            return create_compute_program(source.str().c_str());
        };
        m_inject_program = load_comp("res/shaders/froxel_inject.comp");
        m_integrate_program = load_comp("res/shaders/froxel_integrate.comp");
        if (m_inject_program == 0 || m_integrate_program == 0) {
            LUMINUMBRA_CORE_ERROR("froxel: kernel compile failed; volumetrics disabled");
            return false;
        }
        PassGl::label_gl_object(GL_PROGRAM, m_inject_program, "shader.froxel_inject");
        PassGl::label_gl_object(GL_PROGRAM, m_integrate_program, "shader.froxel_integrate");
        ExpectedLayout inject_layout;
        inject_layout.pass_name = "froxel_inject";
        inject_layout.samplers = {{"u_shadowCascades", GL_SAMPLER_2D_ARRAY, -1},
                                  {"u_shadowTintCascades", GL_SAMPLER_2D_ARRAY, -1}};
        ValidateLayout(m_inject_program, inject_layout);
        auto make_volume = [&](const char* label) -> GLuint {
            GLuint t = 0;
            glGenTextures(1, &t);
            glBindTexture(GL_TEXTURE_3D, t);
            glTexStorage3D(
                GL_TEXTURE_3D, 1, GL_RGBA16F, Froxel::kGridX, Froxel::kGridY, Froxel::kGridZ);
            glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE);
            PassGl::label_gl_object(GL_TEXTURE, t, label);
            glBindTexture(GL_TEXTURE_3D, 0);
            return t;
        };
        m_scatter_tex = make_volume("froxel.scatter");
        m_integrated_tex = make_volume("froxel.integrated");
    }

    glUseProgram(m_inject_program);
    glBindImageTexture(0, m_scatter_tex, 0, GL_TRUE, 0, GL_WRITE_ONLY, GL_RGBA16F);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D_ARRAY, ctx.shadow_depth_array.id);
    glUniform1i(glGetUniformLocation(m_inject_program, "u_shadowCascades"), 0);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D_ARRAY, ctx.shadow_tint_array.id);
    glUniform1i(glGetUniformLocation(m_inject_program, "u_shadowTintCascades"), 1);
    glUniform1i(glGetUniformLocation(m_inject_program, "u_shadowTintEnabled"),
                ctx.shadow_tint_array.id != 0 ? 1 : 0);
    if (ctx.light_space_matrices) {
        for (int i = 0; i < 4 && i < static_cast<int>(ctx.light_space_matrices->size()); ++i) {
            const std::string name = "u_lightSpaceMatrices[" + std::to_string(i) + "]";
            glUniformMatrix4fv(glGetUniformLocation(m_inject_program, name.c_str()),
                               1,
                               GL_FALSE,
                               &(*ctx.light_space_matrices)[i][0][0]);
        }
    }
    glUniform4fv(
        glGetUniformLocation(m_inject_program, "u_cascadeSplits"), 1, &ctx.cascade_splits[0]);
    const Camera& camera = *ctx.camera;
    const glm::mat4 inv_view = glm::inverse(camera.GetViewMatrix());
    glUniformMatrix4fv(
        glGetUniformLocation(m_inject_program, "u_inverseView"), 1, GL_FALSE, &inv_view[0][0]);
    glUniform3fv(glGetUniformLocation(m_inject_program, "u_cameraPos"), 1, &camera.Position[0]);
    glUniform1f(glGetUniformLocation(m_inject_program, "u_tanHalfFovY"),
                std::tan(glm::radians(camera.Zoom) * 0.5f));
    glUniform1f(glGetUniformLocation(m_inject_program, "u_aspect"),
                static_cast<float>(ctx.screen_width) / static_cast<float>(ctx.screen_height));
    // Toward-sun (sun-disc convention — matches the aerial pass's u_sunDirection).
    const glm::vec3 toward_sun = -ctx.sun.direction;
    glUniform3fv(glGetUniformLocation(m_inject_program, "u_sunDirection"), 1, &toward_sun[0]);
    glUniform3fv(glGetUniformLocation(m_inject_program, "u_sunColor"), 1, &ctx.sun.color[0]);
    glUniform3fv(
        glGetUniformLocation(m_inject_program, "u_ambientColor"), 1, &ctx.sky_ambient_color[0]);
    // v1 media tuning (checkpoint-ratified): a gentle ground-hugging haze layer.
    glUniform1f(glGetUniformLocation(m_inject_program, "u_baseDensity"), 0.012f);
    glUniform1f(glGetUniformLocation(m_inject_program, "u_baseHeight"), 40.0f);
    glUniform1f(glGetUniformLocation(m_inject_program, "u_densityFalloff"), 0.05f);

    glDispatchCompute(Froxel::kGridX / 8, Froxel::kGridY / 8 + 1, Froxel::kGridZ);
    glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT);
    glUseProgram(0);
    glActiveTexture(GL_TEXTURE0);
    return true;
}

void FroxelPass::execute_integrate(const RenderContext& ctx) {
    //  rendering: front-to-back march of the scatter volume —
    // per-column accumulated in-scatter L + transmittance T.
    if (ctx.volumetric_quality <= 0 || m_integrate_program == 0) {
        return;
    }
    glUseProgram(m_integrate_program);
    glBindImageTexture(0, m_scatter_tex, 0, GL_TRUE, 0, GL_READ_ONLY, GL_RGBA16F);
    glBindImageTexture(1, m_integrated_tex, 0, GL_TRUE, 0, GL_WRITE_ONLY, GL_RGBA16F);
    glDispatchCompute(Froxel::kGridX / 8, Froxel::kGridY / 8 + 1, 1);
    // The aerial composite samples the integrated volume as a texture.
    glMemoryBarrier(GL_TEXTURE_FETCH_BARRIER_BIT);
    glUseProgram(0);
}

void FroxelPass::destroy() {
    if (m_inject_program) {
        glDeleteProgram(m_inject_program);
        m_inject_program = 0;
    }
    if (m_integrate_program) {
        glDeleteProgram(m_integrate_program);
        m_integrate_program = 0;
    }
    if (m_scatter_tex) {
        glDeleteTextures(1, &m_scatter_tex);
        m_scatter_tex = 0;
    }
    if (m_integrated_tex) {
        glDeleteTextures(1, &m_integrated_tex);
        m_integrated_tex = 0;
    }
}

} // namespace Luminumbra::Rendering
