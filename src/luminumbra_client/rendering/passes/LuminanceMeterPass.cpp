#include "LuminanceMeterPass.h"

#include "PassGlHelpers.h"
#include "core/Log.h"

#include <fstream>
#include <sstream>

namespace Luminumbra::Rendering {

namespace {

GLuint create_compute_shader(const char* source) {
    GLuint shader = glCreateShader(GL_COMPUTE_SHADER);
    glShaderSource(shader, 1, &source, nullptr);
    glCompileShader(shader);

    GLint success;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &success);
    if (!success) {
        char infoLog[512];
        glGetShaderInfoLog(shader, 512, nullptr, infoLog);
        LUMINUMBRA_CORE_ERROR("Compute shader compilation failed: {}", infoLog);
        glDeleteShader(shader);
        return 0;
    }

    return shader;
}

} // namespace

GLuint create_compute_program(const char* compute_source) {
    GLuint compute_shader = create_compute_shader(compute_source);
    if (compute_shader == 0)
        return 0;

    GLuint program = glCreateProgram();
    glAttachShader(program, compute_shader);
    glLinkProgram(program);

    GLint success;
    glGetProgramiv(program, GL_LINK_STATUS, &success);
    if (!success) {
        char infoLog[512];
        glGetProgramInfoLog(program, 512, nullptr, infoLog);
        LUMINUMBRA_CORE_ERROR("Compute program linking failed: {}", infoLog);
        glDeleteProgram(program);
        program = 0;
    }

    glDeleteShader(compute_shader);
    return program;
}

LuminanceMeterPass::LuminanceMeterPass() = default;
LuminanceMeterPass::~LuminanceMeterPass() = default;

bool LuminanceMeterPass::execute(const RenderContext& ctx, const std::filesystem::path& root_path) {
    if (m_reduce_program == 0) {
        // Lazy init: compile the reduce kernel + the 1-float SSBO on first use.
        std::ifstream file(root_path / "res/shaders/luminance_reduce.comp");
        if (!file) {
            LUMINUMBRA_CORE_ERROR("luminance_meter: missing res/shaders/luminance_reduce.comp");
            return false;
        }
        std::stringstream source;
        source << file.rdbuf();
        m_reduce_program = create_compute_program(source.str().c_str());
        if (m_reduce_program == 0) {
            LUMINUMBRA_CORE_ERROR("luminance_meter: compute compile failed; metering disabled");
            return false;
        }
        PassGl::label_gl_object(GL_PROGRAM, m_reduce_program, "shader.luminance_reduce");
        glGenBuffers(1, &m_reduce_ssbo);
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, m_reduce_ssbo);
        glBufferData(GL_SHADER_STORAGE_BUFFER, sizeof(float), nullptr, GL_DYNAMIC_COPY);
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
        PassGl::label_gl_object(GL_BUFFER, m_reduce_ssbo, "luminance_reduce.ssbo");
    }
    glUseProgram(m_reduce_program);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, ctx.lit_scene_color.id);
    glUniform1i(glGetUniformLocation(m_reduce_program, "u_scene"), 0);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, m_reduce_ssbo);
    glDispatchCompute(1, 1, 1);
    // The ring's GPU->GPU copy must observe the SSBO write.
    glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT | GL_BUFFER_UPDATE_BARRIER_BIT);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, 0);
    glUseProgram(0);
    if (m_readback.ensure(sizeof(float), 3) && m_readback.begin()) {
        m_readback.copy_region(m_reduce_ssbo, 0, 0, sizeof(float));
        m_readback.submit();
    }
    return true;
}

void LuminanceMeterPass::destroy() {
    if (m_reduce_program) {
        glDeleteProgram(m_reduce_program);
        m_reduce_program = 0;
    }
    if (m_reduce_ssbo) {
        glDeleteBuffers(1, &m_reduce_ssbo);
        m_reduce_ssbo = 0;
    }
    m_readback.shutdown();
}

} // namespace Luminumbra::Rendering
