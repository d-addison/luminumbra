#include "FroxelPass.h"
#include "../FarWorldReplay.h"
#include "core/Profiler.h"
#include "luminumbra_common/core/CaptureContentPaths.h"
#include "rendering/AttributedGl.h"

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
#include <utility>

namespace Luminumbra::Rendering {

// Shared with the pipeline's luminance-meter compute stage.
AttributedGl::Program create_compute_program(const char* compute_source);

namespace {

// One initialization owns exactly two programs and two fixed-grid volumes.
// Unwind and every refusal release only resources created by this attempt.
struct PendingFroxelResources {
    AttributedGl::Program inject{};
    AttributedGl::Program integrate{};
    GLuint scatter = 0;
    GLuint integrated = 0;

    ~PendingFroxelResources() {
        if (inject)
            AttributedGl::DeleteProgram(glDeleteProgram, inject);
        if (integrate)
            AttributedGl::DeleteProgram(glDeleteProgram, integrate);
        if (scatter)
            glDeleteTextures(1, &scatter);
        if (integrated)
            glDeleteTextures(1, &integrated);
    }
};

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
    LUMIN_PROFILE_ZONE_N("GL FroxelPass.execute_inject");
    //  rendering: per-froxel media density + in-scatter into
    // the scatter volume, sampling the shadow depth AND the  tint cascade so
    // stained glass throws COLORED shafts. Quality 0 = zero-GL no-op.
    if (ctx.volumetric_quality <= 0) {
        return true;
    }
    if (m_initialization_state == InitializationState::Failed)
        return false;
    if (m_initialization_state == InitializationState::Uninitialized) {
        if (ctx.frozen_replay)
            return false;
        // Latch even an exception; only an explicit destroy permits a retry.
        m_initialization_state = InitializationState::Failed;
        PendingFroxelResources pending;
        // Lazy init: both kernels + both 3D volumes, together.
        auto load_comp = [&](const char* rel) -> AttributedGl::Program {
            Filesystem::ContentInput file(std::filesystem::path(root_path) / rel);
            if (!file) {
                LUMINUMBRA_CORE_ERROR("froxel: missing {}", rel);
                return {};
            }
            std::stringstream source;
            source << file.rdbuf();
            if (file.bad() || source.fail())
                return {};
            return create_compute_program(source.str().c_str());
        };
        pending.inject = load_comp("res/shaders/froxel_inject.comp");
        pending.integrate = load_comp("res/shaders/froxel_integrate.comp");
        if (pending.inject == 0 || pending.integrate == 0) {
            LUMINUMBRA_CORE_ERROR("froxel: kernel compile failed; volumetrics disabled");
            return false;
        }
        PassGl::label_gl_object(GL_PROGRAM, pending.inject, "shader.froxel_inject");
        PassGl::label_gl_object(GL_PROGRAM, pending.integrate, "shader.froxel_integrate");
        ExpectedLayout inject_layout;
        inject_layout.pass_name = "froxel_inject";
        inject_layout.samplers = {{"u_shadowCascades", GL_SAMPLER_2D_ARRAY, -1},
                                  {"u_shadowTintCascades", GL_SAMPLER_2D_ARRAY, -1}};
        if (!ValidateLayout(pending.inject, inject_layout))
            return false;
        auto make_volume = [](GLuint& t, const char* label) -> bool {
            // The pending owner holds the name before labeling can allocate.
            glGenTextures(1, &t);
            if (t == 0)
                return false;
            glBindTexture(GL_TEXTURE_3D, t);
            glTexStorage3D(
                GL_TEXTURE_3D, 1, GL_RGBA16F, Froxel::kGridX, Froxel::kGridY, Froxel::kGridZ);
            glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE);
            GLint immutable = GL_FALSE;
            glGetTexParameteriv(GL_TEXTURE_3D, GL_TEXTURE_IMMUTABLE_FORMAT, &immutable);
            PassGl::label_gl_object(GL_TEXTURE, t, label);
            glBindTexture(GL_TEXTURE_3D, 0);
            return immutable == GL_TRUE;
        };
        if (!make_volume(pending.scatter, "froxel.scatter") ||
            !make_volume(pending.integrated, "froxel.integrated")) {
            LUMINUMBRA_CORE_ERROR("froxel: volume allocation failed; volumetrics disabled");
            return false;
        }
        m_inject_program = std::exchange(pending.inject, AttributedGl::Program{});
        m_integrate_program = std::exchange(pending.integrate, AttributedGl::Program{});
        m_scatter_tex = std::exchange(pending.scatter, 0);
        m_integrated_tex = std::exchange(pending.integrated, 0);
        m_initialization_state = InitializationState::Ready;
    }

    PreserveReplayObject(ReplayObjectKind::Program, m_inject_program);
    Luminumbra::Rendering::AttributedGl::UseProgram(glUseProgram, m_inject_program);
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
                               &(*ctx.light_space_matrices)[static_cast<std::size_t>(i)][0][0]);
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

    Luminumbra::Rendering::AttributedGl::Submit(
        glDispatchCompute, Froxel::kGridX / 8, Froxel::kGridY / 8 + 1, Froxel::kGridZ);
    glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT);
    Luminumbra::Rendering::AttributedGl::UseProgram(glUseProgram, 0);
    glActiveTexture(GL_TEXTURE0);
    return true;
}

void FroxelPass::execute_integrate(const RenderContext& ctx) {
    LUMIN_PROFILE_ZONE_N("GL FroxelPass.execute_integrate");
    //  rendering: front-to-back march of the scatter volume —
    // per-column accumulated in-scatter L + transmittance T.
    if (ctx.volumetric_quality <= 0 || m_initialization_state != InitializationState::Ready) {
        return;
    }
    PreserveReplayObject(ReplayObjectKind::Program, m_integrate_program);
    Luminumbra::Rendering::AttributedGl::UseProgram(glUseProgram, m_integrate_program);
    glBindImageTexture(0, m_scatter_tex, 0, GL_TRUE, 0, GL_READ_ONLY, GL_RGBA16F);
    glBindImageTexture(1, m_integrated_tex, 0, GL_TRUE, 0, GL_WRITE_ONLY, GL_RGBA16F);
    Luminumbra::Rendering::AttributedGl::Submit(
        glDispatchCompute, Froxel::kGridX / 8, Froxel::kGridY / 8 + 1, 1);
    // The aerial composite samples the integrated volume as a texture.
    glMemoryBarrier(GL_TEXTURE_FETCH_BARRIER_BIT);
    Luminumbra::Rendering::AttributedGl::UseProgram(glUseProgram, 0);
}

void FroxelPass::destroy() {
    if (m_inject_program) {
        Luminumbra::Rendering::AttributedGl::DeleteProgram(glDeleteProgram, m_inject_program);
        m_inject_program = {};
    }
    if (m_integrate_program) {
        Luminumbra::Rendering::AttributedGl::DeleteProgram(glDeleteProgram, m_integrate_program);
        m_integrate_program = {};
    }
    if (m_scatter_tex) {
        glDeleteTextures(1, &m_scatter_tex);
        m_scatter_tex = 0;
    }
    if (m_integrated_tex) {
        glDeleteTextures(1, &m_integrated_tex);
        m_integrated_tex = 0;
    }
    m_initialization_state = InitializationState::Uninitialized;
}

} // namespace Luminumbra::Rendering
