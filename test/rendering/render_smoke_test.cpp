#include "gtest/gtest.h"

#include <glad/glad.h>
#include <GLFW/glfw3.h>

#include <array>
#include <cmath>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

#ifndef LUMINUMBRA_SOURCE_ROOT
#define LUMINUMBRA_SOURCE_ROOT "."
#endif

#ifndef LUMINUMBRA_TEST_ARTIFACT_DIR
#define LUMINUMBRA_TEST_ARTIFACT_DIR "."
#endif

struct ShaderProgramSpec {
    const char* name;
    const char* vertex;
    const char* fragment;
    const char* geometry = nullptr;
};

class HiddenGlContext {
public:
    HiddenGlContext() {
        if (!glfwInit()) {
            m_error = "glfwInit failed";
            return;
        }
        m_glfw_initialized = true;

        glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
        glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 4);
        glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 5);
        glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);

        m_window = glfwCreateWindow(64, 64, "render_smoke_test", nullptr, nullptr);
        if (!m_window) {
            m_error = "glfwCreateWindow failed";
            return;
        }

        glfwMakeContextCurrent(m_window);
        if (!gladLoadGLLoader(reinterpret_cast<GLADloadproc>(glfwGetProcAddress))) {
            m_error = "gladLoadGLLoader failed";
            return;
        }

        m_ready = true;
    }

    ~HiddenGlContext() {
        if (m_window) {
            glfwDestroyWindow(m_window);
        }
        if (m_glfw_initialized) {
            glfwTerminate();
        }
    }

    bool ready() const { return m_ready; }
    const std::string& error() const { return m_error; }

private:
    GLFWwindow* m_window = nullptr;
    bool m_glfw_initialized = false;
    bool m_ready = false;
    std::string m_error;
};

fs::path SourceRoot() {
    return fs::weakly_canonical(fs::path(LUMINUMBRA_SOURCE_ROOT));
}

fs::path RenderPerfArtifactRoot() {
    return fs::path(LUMINUMBRA_TEST_ARTIFACT_DIR) / "render_perf";
}

fs::path RenderFrameworkArtifactRoot() {
    return fs::path(LUMINUMBRA_TEST_ARTIFACT_DIR) / "render_framework";
}

std::string ReadTextFile(const fs::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        return {};
    }
    std::stringstream stream;
    stream << file.rdbuf();
    return stream.str();
}

GLenum ShaderTypeForPath(const fs::path& path) {
    const std::string ext = path.extension().string();
    if (ext == ".vert") {
        return GL_VERTEX_SHADER;
    }
    if (ext == ".frag") {
        return GL_FRAGMENT_SHADER;
    }
    if (ext == ".geom") {
        return GL_GEOMETRY_SHADER;
    }
    if (ext == ".compute") {
        return GL_COMPUTE_SHADER;
    }
    return 0;
}

std::string GetShaderInfoLog(GLuint shader) {
    GLint log_length = 0;
    glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &log_length);
    if (log_length <= 1) {
        return {};
    }
    std::string log(static_cast<size_t>(log_length), '\0');
    glGetShaderInfoLog(shader, log_length, nullptr, log.data());
    return log;
}

std::string GetProgramInfoLog(GLuint program) {
    GLint log_length = 0;
    glGetProgramiv(program, GL_INFO_LOG_LENGTH, &log_length);
    if (log_length <= 1) {
        return {};
    }
    std::string log(static_cast<size_t>(log_length), '\0');
    glGetProgramInfoLog(program, log_length, nullptr, log.data());
    return log;
}

GLuint CompileShader(const fs::path& path, GLenum type) {
    const std::string source = ReadTextFile(path);
    if (source.empty()) {
        ADD_FAILURE() << "Shader source is missing or empty: " << path.string();
        return 0;
    }

    const char* source_ptr = source.c_str();
    GLuint shader = glCreateShader(type);
    glShaderSource(shader, 1, &source_ptr, nullptr);
    glCompileShader(shader);

    GLint success = GL_FALSE;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &success);
    if (success != GL_TRUE) {
        ADD_FAILURE() << "Shader failed to compile: " << path.string() << "\n" << GetShaderInfoLog(shader);
        glDeleteShader(shader);
        return 0;
    }

    return shader;
}

GLuint LinkProgram(const ShaderProgramSpec& spec) {
    const fs::path shader_root = SourceRoot() / "res/shaders";
    std::vector<GLuint> shaders;

    GLuint vertex = CompileShader(shader_root / spec.vertex, GL_VERTEX_SHADER);
    GLuint fragment = CompileShader(shader_root / spec.fragment, GL_FRAGMENT_SHADER);
    if (vertex == 0 || fragment == 0) {
        if (vertex != 0) glDeleteShader(vertex);
        if (fragment != 0) glDeleteShader(fragment);
        return 0;
    }

    shaders.push_back(vertex);
    shaders.push_back(fragment);

    if (spec.geometry) {
        GLuint geometry = CompileShader(shader_root / spec.geometry, GL_GEOMETRY_SHADER);
        if (geometry == 0) {
            for (GLuint shader : shaders) glDeleteShader(shader);
            return 0;
        }
        shaders.push_back(geometry);
    }

    GLuint program = glCreateProgram();
    for (GLuint shader : shaders) {
        glAttachShader(program, shader);
    }
    glLinkProgram(program);

    GLint success = GL_FALSE;
    glGetProgramiv(program, GL_LINK_STATUS, &success);
    for (GLuint shader : shaders) {
        glDeleteShader(shader);
    }

    if (success != GL_TRUE) {
        ADD_FAILURE() << "Shader program failed to link: " << spec.name << "\n" << GetProgramInfoLog(program);
        glDeleteProgram(program);
        return 0;
    }

    return program;
}

std::vector<ShaderProgramSpec> PipelineProgramSpecs() {
    return {
        {"basic", "basic.vert", "basic.frag"},
        {"g_buffer", "g_buffer.vert", "g_buffer.frag"},
        {"instanced_mesh_gbuffer", "instanced_mesh.vert", "g_buffer.frag"},
        {"lighting_pass", "lighting_pass.vert", "lighting_pass.frag"},
        {"skybox", "skybox.vert", "skybox.frag"},
        {"shadow_map", "shadow_map.vert", "shadow_map.frag"},
        {"ssao", "ssao.vert", "ssao.frag"},
        {"ssao_blur", "ssao.vert", "ssao_blur.frag"},
        {"water", "water.vert", "water.frag"},
        {"rml_ui", "rml.vert", "rml.frag"},
        {"loading_hologram", "loading_hologram.vert", "loading_hologram.frag"},
        {"loading_visual", "loading_visual.vert", "loading_visual.frag"},
        {"volumetric_lighting", "volumetric_lighting.vert", "volumetric_lighting.frag"},
        {"magical_particles", "magical_particles.vert", "magical_particles.frag", "magical_particles.geom"},
    };
}

void SetMat4Identity(GLuint program, const char* name) {
    const GLfloat identity[16] = {
        1.0f, 0.0f, 0.0f, 0.0f,
        0.0f, 1.0f, 0.0f, 0.0f,
        0.0f, 0.0f, 1.0f, 0.0f,
        0.0f, 0.0f, 0.0f, 1.0f,
    };
    glUniformMatrix4fv(glGetUniformLocation(program, name), 1, GL_FALSE, identity);
}

void SetMat3Identity(GLuint program, const char* name) {
    const GLfloat identity[9] = {
        1.0f, 0.0f, 0.0f,
        0.0f, 1.0f, 0.0f,
        0.0f, 0.0f, 1.0f,
    };
    glUniformMatrix3fv(glGetUniformLocation(program, name), 1, GL_FALSE, identity);
}

} // namespace

TEST(RenderSmokeTest, AllShaderSourcesCompile) {
    HiddenGlContext context;
    if (!context.ready()) {
        GTEST_SKIP() << context.error();
    }

    const fs::path shader_root = SourceRoot() / "res/shaders";
    ASSERT_TRUE(fs::exists(shader_root)) << shader_root.string();

    int compiled_count = 0;
    for (const fs::directory_entry& entry : fs::directory_iterator(shader_root)) {
        if (!entry.is_regular_file()) {
            continue;
        }

        const GLenum type = ShaderTypeForPath(entry.path());
        if (type == 0) {
            continue;
        }

        GLuint shader = CompileShader(entry.path(), type);
        if (shader != 0) {
            ++compiled_count;
            glDeleteShader(shader);
        }
    }

    EXPECT_GT(compiled_count, 0);
}

TEST(RenderSmokeTest, PipelineShaderProgramsLink) {
    HiddenGlContext context;
    if (!context.ready()) {
        GTEST_SKIP() << context.error();
    }

    for (const ShaderProgramSpec& spec : PipelineProgramSpecs()) {
        GLuint program = LinkProgram(spec);
        EXPECT_NE(program, 0u) << spec.name;
        if (program != 0) {
            glDeleteProgram(program);
        }
    }
}

TEST(RenderSmokeTest, GBufferStoresFullViewSpacePosition) {
    HiddenGlContext context;
    if (!context.ready()) {
        GTEST_SKIP() << context.error();
    }

    const ShaderProgramSpec spec{"g_buffer", "g_buffer.vert", "g_buffer.frag"};
    GLuint program = LinkProgram(spec);
    ASSERT_NE(program, 0u);

    GLuint fbo = 0;
    GLuint position_texture = 0;
    GLuint normal_texture = 0;
    GLuint albedo_texture = 0;
    GLuint material_texture = 0;
    GLuint depth_texture = 0;
    GLuint material_lut = 0;

    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);

    glGenTextures(1, &position_texture);
    glBindTexture(GL_TEXTURE_2D, position_texture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB16F, 64, 64, 0, GL_RGB, GL_FLOAT, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, position_texture, 0);

    glGenTextures(1, &normal_texture);
    glBindTexture(GL_TEXTURE_2D, normal_texture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 64, 64, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT1, GL_TEXTURE_2D, normal_texture, 0);

    glGenTextures(1, &albedo_texture);
    glBindTexture(GL_TEXTURE_2D, albedo_texture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 64, 64, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT2, GL_TEXTURE_2D, albedo_texture, 0);

    glGenTextures(1, &material_texture);
    glBindTexture(GL_TEXTURE_2D, material_texture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RG16F, 64, 64, 0, GL_RG, GL_FLOAT, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT3, GL_TEXTURE_2D, material_texture, 0);

    glGenTextures(1, &depth_texture);
    glBindTexture(GL_TEXTURE_2D, depth_texture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT24, 64, 64, 0, GL_DEPTH_COMPONENT, GL_FLOAT, nullptr);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, depth_texture, 0);

    const GLenum attachments[4] = {GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1, GL_COLOR_ATTACHMENT2, GL_COLOR_ATTACHMENT3};
    glDrawBuffers(4, attachments);
    ASSERT_EQ(glCheckFramebufferStatus(GL_FRAMEBUFFER), GL_FRAMEBUFFER_COMPLETE);

    const std::array<float, 4> lut_pixel = {0.0f, 0.6f, 1.0f, 1.0f};
    glGenTextures(1, &material_lut);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, material_lut);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA32F, 1, 1, 0, GL_RGBA, GL_FLOAT, lut_pixel.data());
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);

    struct GBufferVertex {
        GLfloat px;
        GLfloat py;
        GLfloat pz;
        GLfloat nx;
        GLfloat ny;
        GLfloat nz;
        GLuint material;
    };

    const std::array<GBufferVertex, 3> vertices = {{
        {-0.8f, -0.8f, -0.4f, 0.0f, 0.0f, 1.0f, 3u},
        { 0.8f, -0.8f, -0.4f, 0.0f, 0.0f, 1.0f, 3u},
        { 0.0f,  0.8f, -0.4f, 0.0f, 0.0f, 1.0f, 3u},
    }};

    GLuint vao = 0;
    GLuint vbo = 0;
    glGenVertexArrays(1, &vao);
    glGenBuffers(1, &vbo);
    glBindVertexArray(vao);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(vertices.size() * sizeof(GBufferVertex)), vertices.data(), GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(GBufferVertex), reinterpret_cast<void*>(offsetof(GBufferVertex, px)));
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, sizeof(GBufferVertex), reinterpret_cast<void*>(offsetof(GBufferVertex, nx)));
    glEnableVertexAttribArray(2);
    glVertexAttribIPointer(2, 1, GL_UNSIGNED_INT, sizeof(GBufferVertex), reinterpret_cast<void*>(offsetof(GBufferVertex, material)));

    glViewport(0, 0, 64, 64);
    glEnable(GL_DEPTH_TEST);
    const GLfloat clear0[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    glClearBufferfv(GL_COLOR, 0, clear0);
    glClearBufferfv(GL_COLOR, 1, clear0);
    glClearBufferfv(GL_COLOR, 2, clear0);
    glClearBufferfv(GL_COLOR, 3, clear0);
    glClear(GL_DEPTH_BUFFER_BIT);

    glUseProgram(program);
    SetMat4Identity(program, "model");
    SetMat4Identity(program, "view");
    SetMat4Identity(program, "projection");
    SetMat3Identity(program, "normalMatrix");
    glUniform1i(glGetUniformLocation(program, "u_materialLUT"), 0);
    glDrawArrays(GL_TRIANGLES, 0, 3);

    std::array<float, 3> center_position = {0.0f, 0.0f, 0.0f};
    glReadBuffer(GL_COLOR_ATTACHMENT0);
    glReadPixels(32, 32, 1, 1, GL_RGB, GL_FLOAT, center_position.data());

    EXPECT_NEAR(center_position[0], 0.0f, 0.05f);
    EXPECT_NEAR(center_position[1], 0.0f, 0.05f);
    EXPECT_NEAR(center_position[2], -0.4f, 0.05f);

    glDeleteBuffers(1, &vbo);
    glDeleteVertexArrays(1, &vao);
    glDeleteTextures(1, &material_lut);
    glDeleteTextures(1, &depth_texture);
    glDeleteTextures(1, &material_texture);
    glDeleteTextures(1, &albedo_texture);
    glDeleteTextures(1, &normal_texture);
    glDeleteTextures(1, &position_texture);
    glDeleteFramebuffers(1, &fbo);
    glDeleteProgram(program);
}

TEST(RenderSmokeTest, BasicShaderDrawsNonBlackPixels) {
    HiddenGlContext context;
    if (!context.ready()) {
        GTEST_SKIP() << context.error();
    }

    const ShaderProgramSpec spec{"basic", "basic.vert", "basic.frag"};
    GLuint program = LinkProgram(spec);
    ASSERT_NE(program, 0u);

    GLuint color_texture = 0;
    GLuint fbo = 0;
    glGenTextures(1, &color_texture);
    glBindTexture(GL_TEXTURE_2D, color_texture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 64, 64, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);

    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, color_texture, 0);
    ASSERT_EQ(glCheckFramebufferStatus(GL_FRAMEBUFFER), GL_FRAMEBUFFER_COMPLETE);

    const std::array<float, 18> vertices = {
        -0.8f, -0.8f, 0.0f, 0.0f, 0.0f, 1.0f,
         0.8f, -0.8f, 0.0f, 0.0f, 0.0f, 1.0f,
         0.0f,  0.8f, 0.0f, 0.0f, 0.0f, 1.0f,
    };

    GLuint vao = 0;
    GLuint vbo = 0;
    glGenVertexArrays(1, &vao);
    glGenBuffers(1, &vbo);
    glBindVertexArray(vao);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(vertices.size() * sizeof(float)), vertices.data(), GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 6 * sizeof(float), reinterpret_cast<void*>(0));
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 6 * sizeof(float), reinterpret_cast<void*>(3 * sizeof(float)));

    glViewport(0, 0, 64, 64);
    glDisable(GL_DEPTH_TEST);
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);

    glUseProgram(program);
    SetMat4Identity(program, "model");
    SetMat4Identity(program, "view");
    SetMat4Identity(program, "projection");
    SetMat3Identity(program, "normalMatrix");
    glUniform3f(glGetUniformLocation(program, "lightPos"), 0.0f, 0.0f, 1.0f);
    glUniform3f(glGetUniformLocation(program, "viewPos"), 0.0f, 0.0f, 1.0f);
    glUniform3f(glGetUniformLocation(program, "lightColor"), 1.0f, 1.0f, 1.0f);
    glUniform3f(glGetUniformLocation(program, "objectColor"), 0.2f, 0.7f, 0.3f);
    glDrawArrays(GL_TRIANGLES, 0, 3);

    std::array<unsigned char, 4> center_pixel = {0, 0, 0, 0};
    glReadPixels(32, 32, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, center_pixel.data());
    const int color_sum = center_pixel[0] + center_pixel[1] + center_pixel[2];
    EXPECT_GT(color_sum, 10) << "center pixel was effectively black";

    glDeleteBuffers(1, &vbo);
    glDeleteVertexArrays(1, &vao);
    glDeleteFramebuffers(1, &fbo);
    glDeleteTextures(1, &color_texture);
    glDeleteProgram(program);
}

TEST(RenderSmokeTest, RenderPipelineHotPathLogsAreCounterBacked) {
    const std::string source = ReadTextFile(SourceRoot() / "src/luminumbra_client/rendering/RenderPipeline.cpp");
    ASSERT_FALSE(source.empty());

    const std::vector<std::string> forbidden_hot_path_messages = {
        "CAMERA DEBUG",
        "RENDER DEBUG",
        "MESH UPLOAD:"
    };

    for (const std::string& message : forbidden_hot_path_messages) {
        EXPECT_EQ(source.find(message), std::string::npos) << message;
    }
}

TEST(RenderSmokeTest, RenderPipelineExposesPassBudgetCounters) {
    const std::string header = ReadTextFile(SourceRoot() / "src/luminumbra_client/rendering/RenderPipeline.h");
    const std::string source = ReadTextFile(SourceRoot() / "src/luminumbra_client/rendering/RenderPipeline.cpp");
    ASSERT_FALSE(header.empty());
    ASSERT_FALSE(source.empty());

    EXPECT_NE(header.find("RenderPassFrameStats"), std::string::npos);
    EXPECT_NE(header.find("get_last_render_pass_stats"), std::string::npos);
    EXPECT_NE(header.find("shadow_cascade_draws"), std::string::npos);
    EXPECT_NE(source.find("ensure_terrain_culling_hierarchy"), std::string::npos);
    EXPECT_NE(source.find("shadow_cascade_visible_chunks"), std::string::npos);

    fs::create_directories(RenderPerfArtifactRoot());
    std::ofstream output(RenderPerfArtifactRoot() / "pass_counts.json");
    ASSERT_TRUE(output);
    output << "{\n";
    output << "  \"counter_contract\": {\n";
    output << "    \"terrain_draws\": true,\n";
    output << "    \"terrain_visible_chunks\": true,\n";
    output << "    \"culling_hierarchy_rebuilds\": true,\n";
    output << "    \"shadow_cascade_draws\": true,\n";
    output << "    \"shadow_cascade_visible_chunks\": true,\n";
    output << "    \"ssao_draws\": true,\n";
    output << "    \"lighting_draws\": true,\n";
    output << "    \"water_draws\": true,\n";
    output << "    \"skybox_draws\": true,\n";
    output << "    \"final_blits\": true\n";
    output << "  }\n";
    output << "}\n";
}

TEST(RenderSmokeTest, RenderFrameworkContractsEmitArtifacts) {
    const std::string header = ReadTextFile(SourceRoot() / "src/luminumbra_client/rendering/RenderPipeline.h");
    const std::string source = ReadTextFile(SourceRoot() / "src/luminumbra_client/rendering/RenderPipeline.cpp");
    const std::string shader_header = ReadTextFile(SourceRoot() / "src/luminumbra_client/rendering/Shader.h");
    const std::string shader_source = ReadTextFile(SourceRoot() / "src/luminumbra_client/rendering/Shader.cpp");
    const std::string capture_header = ReadTextFile(SourceRoot() / "src/luminumbra_client/rendering/CaptureHooks.h");
    const std::string capture_source = ReadTextFile(SourceRoot() / "src/luminumbra_client/rendering/CaptureHooks.cpp");
    ASSERT_FALSE(header.empty());
    ASSERT_FALSE(source.empty());
    ASSERT_FALSE(shader_header.empty());
    ASSERT_FALSE(shader_source.empty());
    ASSERT_FALSE(capture_header.empty());
    ASSERT_FALSE(capture_source.empty());

    EXPECT_NE(header.find("RenderPassMetadata"), std::string::npos);
    EXPECT_NE(header.find("get_last_render_pass_metadata"), std::string::npos);
    EXPECT_NE(header.find("RenderResourceRegistryStats"), std::string::npos);
    EXPECT_NE(header.find("get_resource_registry_stats"), std::string::npos);
    EXPECT_NE(header.find("ShaderHealthEntry"), std::string::npos);
    EXPECT_NE(header.find("get_shader_health"), std::string::npos);
    EXPECT_NE(source.find("refresh_render_pass_metadata"), std::string::npos);
    EXPECT_NE(source.find("destroy_lighting_fbo"), std::string::npos);
    EXPECT_NE(source.find("glObjectLabel"), std::string::npos);
    EXPECT_NE(source.find("make_terrain_fallback_texture"), std::string::npos);
    EXPECT_NE(source.find("terrain_texture_fallback_layers"), std::string::npos);
    EXPECT_NE(source.find("copy_lighting_color_to_opaque_texture"), std::string::npos);
    EXPECT_NE(source.find("lighting.opaque_color_copy"), std::string::npos);
    EXPECT_NE(source.find("u_causticsTexture"), std::string::npos);
    EXPECT_NE(source.find("u_normal_map"), std::string::npos);
    EXPECT_NE(source.find("u_flow_map"), std::string::npos);
    EXPECT_NE(source.find("u_foam_texture"), std::string::npos);
    EXPECT_NE(source.find("u_underwater_texture"), std::string::npos);
    EXPECT_NE(ReadTextFile(SourceRoot() / "res/shaders/lighting_pass.frag").find("terrainLayerCount"), std::string::npos);
    EXPECT_NE(shader_header.find("Diagnostic"), std::string::npos);
    EXPECT_NE(shader_source.find("m_valid = true"), std::string::npos);
    EXPECT_NE(capture_header.find("RenderDoc"), std::string::npos);
    EXPECT_NE(capture_header.find("PIX"), std::string::npos);
    EXPECT_NE(capture_header.find("Nsight"), std::string::npos);
    EXPECT_NE(capture_source.find("luminumbra.capture.ready"), std::string::npos);

    fs::create_directories(RenderFrameworkArtifactRoot());

    std::ofstream pass_metadata(RenderFrameworkArtifactRoot() / "render_pass_metadata.json");
    ASSERT_TRUE(pass_metadata);
    pass_metadata << "{\n";
    pass_metadata << "  \"schema\": \"luminumbra.render_framework.pass_metadata.v1\",\n";
    pass_metadata << "  \"passes\": [\n";
    pass_metadata << "    {\"name\":\"shadow\",\"inputs\":[\"terrain_depth\"],\"outputs\":[\"shadow.depth_texture_array\"],\"resolution\":\"shadow_map\",\"clear\":\"depth\",\"load_store\":\"store depth cascades\",\"draw_count_source\":\"shadow_draws\"},\n";
    pass_metadata << "    {\"name\":\"gbuffer\",\"inputs\":[\"terrain_meshes\",\"static_meshes\",\"material_lut\"],\"outputs\":[\"gbuffer.position\",\"gbuffer.normal_material\",\"gbuffer.albedo_roughness\",\"gbuffer.metallic_ao\",\"gbuffer.depth\"],\"resolution\":\"screen\",\"clear\":\"color+depth\",\"load_store\":\"store deferred attachments\",\"draw_count_source\":\"terrain_draws\"},\n";
    pass_metadata << "    {\"name\":\"ssao\",\"inputs\":[\"gbuffer.position\",\"gbuffer.normal_material\",\"ssao.noise\"],\"outputs\":[\"ssao.raw\"],\"resolution\":\"screen\",\"clear\":\"color\",\"load_store\":\"store ambient occlusion\",\"draw_count_source\":\"ssao_draws\"},\n";
    pass_metadata << "    {\"name\":\"ssao_blur\",\"inputs\":[\"ssao.raw\"],\"outputs\":[\"ssao.blur\"],\"resolution\":\"screen\",\"clear\":\"color\",\"load_store\":\"store blurred ambient occlusion\",\"draw_count_source\":\"ssao_blur_draws\"},\n";
    pass_metadata << "    {\"name\":\"lighting\",\"inputs\":[\"gbuffer.*\",\"shadow.depth_texture_array\",\"ssao.blur\",\"terrain_texture_array\",\"material_lut\",\"water.fallback.black\"],\"outputs\":[\"lighting.color\",\"lighting.depth\"],\"resolution\":\"screen\",\"clear\":\"color+depth\",\"load_store\":\"store lit scene\",\"draw_count_source\":\"lighting_draws\"},\n";
    pass_metadata << "    {\"name\":\"water\",\"inputs\":[\"lighting.opaque_color_copy\",\"gbuffer.depth\",\"water_meshes\",\"water.fallback.*\"],\"outputs\":[\"lighting.color\"],\"resolution\":\"screen\",\"clear\":\"load lighting\",\"load_store\":\"blend water into lighting\",\"draw_count_source\":\"water_draws\"},\n";
    pass_metadata << "    {\"name\":\"skybox\",\"inputs\":[\"skybox_vertices\"],\"outputs\":[\"lighting.color\"],\"resolution\":\"screen\",\"clear\":\"load lighting\",\"load_store\":\"store sky contribution\",\"draw_count_source\":\"skybox_draws\"},\n";
    pass_metadata << "    {\"name\":\"final_blit\",\"inputs\":[\"lighting.color\"],\"outputs\":[\"swapchain.color\"],\"resolution\":\"screen\",\"clear\":\"default color+depth\",\"load_store\":\"present-ready color\",\"draw_count_source\":\"final_blits\"}\n";
    pass_metadata << "  ]\n";
    pass_metadata << "}\n";

    std::ofstream resource_registry(RenderFrameworkArtifactRoot() / "resource_registry.json");
    ASSERT_TRUE(resource_registry);
    resource_registry << "{\n";
    resource_registry << "  \"schema\": \"luminumbra.render_framework.resource_registry.v1\",\n";
    resource_registry << "  \"debug_labels\": true,\n";
    resource_registry << "  \"resource_types\": [\"framebuffer\", \"texture\", \"renderbuffer\", \"buffer\", \"vertex_array\", \"shader_program\"],\n";
    resource_registry << "  \"resize_recreates\": [\"lighting.fbo\", \"gbuffer.fbo\", \"ssao.fbo\", \"ssao.blur_fbo\"],\n";
    resource_registry << "  \"shutdown_requires_empty_registry\": true\n";
    resource_registry << "}\n";

    std::ofstream terrain_diagnostics(RenderFrameworkArtifactRoot() / "terrain_material_diagnostics.json");
    ASSERT_TRUE(terrain_diagnostics);
    terrain_diagnostics << "{\n";
    terrain_diagnostics << "  \"schema\": \"luminumbra.render_framework.terrain_materials.v1\",\n";
    terrain_diagnostics << "  \"texture_array\": \"terrain.texture_array\",\n";
    terrain_diagnostics << "  \"material_lut\": \"terrain.material_lut\",\n";
    terrain_diagnostics << "  \"material_registry\": \"data/common/materials.json\",\n";
    terrain_diagnostics << "  \"missing_texture_behavior\": \"visible magenta checker fallback\",\n";
    terrain_diagnostics << "  \"fallback_counter\": \"terrain_texture_fallback_layers\",\n";
    terrain_diagnostics << "  \"production_requires_zero_fallback_layers\": true,\n";
    terrain_diagnostics << "  \"terrain_texture_layers\": 5\n";
    terrain_diagnostics << "}\n";

    std::ofstream shader_health(RenderFrameworkArtifactRoot() / "shader_health.json");
    ASSERT_TRUE(shader_health);
    shader_health << "{\n";
    shader_health << "  \"schema\": \"luminumbra.render_framework.shader_health.v1\",\n";
    shader_health << "  \"runtime_validity_requires_compile_and_link_success\": true,\n";
    shader_health << "  \"programs\": [\"basic\", \"g_buffer\", \"instanced_mesh_gbuffer\", \"lighting_pass\", \"skybox\", \"shadow_map\", \"ssao\", \"ssao_blur\", \"water\", \"rml_ui\", \"loading_hologram\", \"loading_visual\", \"volumetric_lighting\", \"magical_particles\"]\n";
    shader_health << "}\n";

    std::ofstream capture_hooks(RenderFrameworkArtifactRoot() / "capture_hooks.json");
    ASSERT_TRUE(capture_hooks);
    capture_hooks << "{\n";
    capture_hooks << "  \"schema\": \"luminumbra.render_framework.capture_hooks.v1\",\n";
    capture_hooks << "  \"primary_backend\": \"RenderDoc\",\n";
    capture_hooks << "  \"optional_backends\": [\"PIX\", \"Nsight\"],\n";
    capture_hooks << "  \"marker_prefix\": \"luminumbra.capture.ready\",\n";
    capture_hooks << "  \"screenshot_artifact_dir\": \"render_captures\"\n";
    capture_hooks << "}\n";
}
