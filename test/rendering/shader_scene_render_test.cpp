#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>
#include <glad/glad.h>
#include <gtest/gtest.h>

#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <utility>

// A standalone syntax check needs no configured binary identity.
#ifndef LUMINUMBRA_SHADER_SCENE_INPUT_HASH
#define LUMINUMBRA_SHADER_SCENE_INPUT_HASH ""
#endif

namespace {
constexpr int Size = 128;
// Fixed fixture: six RGBA float attachments and one RGB readback, below 2 MiB.
constexpr std::size_t PixelCount = Size * Size;

class ShaderScene : public testing::Test {
protected:
    GLFWwindow* window = nullptr;
    bool initialized = false;
    GLuint program = 0;
    GLuint framebuffer = 0;
    GLuint vao = 0;
    GLuint vbo = 0;
    std::array<GLuint, 6> attachments{};
    std::array<GLuint, 5> textures{};

    void SetUp() override {
        initialized = glfwInit() == GLFW_TRUE;
        ASSERT_TRUE(initialized);
        glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
        glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 4);
        glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 5);
        glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
        window = glfwCreateWindow(Size, Size, "shader scenes", nullptr, nullptr);
        ASSERT_NE(window, nullptr);
        glfwMakeContextCurrent(window);
        ASSERT_NE(gladLoadGLLoader(reinterpret_cast<GLADloadproc>(glfwGetProcAddress)), 0);
        ASSERT_NE(glGetString(GL_RENDERER), nullptr);
        const std::string renderer(reinterpret_cast<const char*>(glGetString(GL_RENDERER)));
        ASSERT_NE(renderer.find("llvmpipe"), std::string::npos) << renderer;
        RecordProperty("renderer", renderer);
        glDisable(GL_DITHER);
        glDisable(GL_BLEND);
        glDisable(GL_FRAMEBUFFER_SRGB);
        glDisable(GL_DEPTH_TEST);
    }
    void TearDown() override {
        if (glad_glDeleteProgram != nullptr && window != nullptr) {
            glDeleteProgram(program);
            glDeleteFramebuffers(1, &framebuffer);
            glDeleteVertexArrays(1, &vao);
            glDeleteBuffers(1, &vbo);
            glDeleteTextures(static_cast<GLsizei>(attachments.size()), attachments.data());
            glDeleteTextures(static_cast<GLsizei>(textures.size()), textures.data());
        }
        if (window != nullptr) {
            glfwDestroyWindow(window);
        }
        if (initialized) {
            glfwTerminate();
        }
    }
    static GLuint Compile(GLenum stage, const std::string& filename) {
        std::ifstream file(std::filesystem::path(LUMINUMBRA_SOURCE_ROOT) / "res/shaders" / filename,
                           std::ios::binary);
        if (!file) {
            ADD_FAILURE() << filename;
            return 0;
        }
        // Shader inputs are fixed repository files, capped before compilation.
        std::array<char, 256 * 1024 + 1> sourceBytes{};
        file.read(sourceBytes.data(), static_cast<std::streamsize>(sourceBytes.size()));
        if (file.bad() || file.gcount() > 256 * 1024) {
            ADD_FAILURE() << filename << ": unreadable or oversized shader";
            return 0;
        }
        const std::string source(sourceBytes.data(), static_cast<std::size_t>(file.gcount()));
        const char* output = std::getenv("LUMINUMBRA_SHADER_SCENE_OUTPUT");
        if (output == nullptr) {
            ADD_FAILURE() << "Missing scene output directory";
            return 0;
        }
        std::ofstream snapshot(std::filesystem::path(output) / filename, std::ios::binary);
        snapshot.write(source.data(), static_cast<std::streamsize>(source.size()));
        snapshot.close();
        if (!snapshot) {
            ADD_FAILURE() << "Cannot record consumed shader source";
            return 0;
        }
        const char* data = source.c_str();
        const GLuint shader = glCreateShader(stage);
        glShaderSource(shader, 1, &data, nullptr);
        glCompileShader(shader);
        GLint compiled = GL_FALSE;
        glGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
        std::array<char, 8192> log{};
        glGetShaderInfoLog(shader, static_cast<GLsizei>(log.size()), nullptr, log.data());
        if (compiled != GL_TRUE) {
            ADD_FAILURE() << filename << ": " << log.data();
            glDeleteShader(shader);
            return 0;
        }
        return shader;
    }
    GLint Uniform(const char* name) const {
        const GLint location = glGetUniformLocation(program, name);
        EXPECT_GE(location, 0) << name;
        return location;
    }
};

TEST_F(ShaderScene, MaterialLinearReferenceAndOcclusionBlend) {
    const GLuint vertex = Compile(GL_VERTEX_SHADER, "static_asset.vert");
    const GLuint fragment = Compile(GL_FRAGMENT_SHADER, "static_asset.frag");
    program = glCreateProgram();
    if (vertex != 0) {
        glAttachShader(program, vertex);
    }
    if (fragment != 0) {
        glAttachShader(program, fragment);
    }
    glLinkProgram(program);
    glDeleteShader(vertex);
    glDeleteShader(fragment);
    ASSERT_NE(vertex, 0u);
    ASSERT_NE(fragment, 0u);
    GLint linked = GL_FALSE;
    glGetProgramiv(program, GL_LINK_STATUS, &linked);
    std::array<char, 8192> log{};
    glGetProgramInfoLog(program, static_cast<GLsizei>(log.size()), nullptr, log.data());
    ASSERT_EQ(linked, GL_TRUE) << log.data();
    glUseProgram(program);
    constexpr std::array<float, 16> identity4{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    constexpr std::array<float, 9> identity3{1, 0, 0, 0, 1, 0, 0, 0, 1};
    for (const auto* name : {"u_model", "u_view", "u_projection"}) {
        glUniformMatrix4fv(Uniform(name), 1, GL_FALSE, identity4.data());
    }
    for (const auto* name : {"u_normal", "u_normalView"}) {
        glUniformMatrix3fv(Uniform(name), 1, GL_FALSE, identity3.data());
    }
    for (int index = 0; index < 5; ++index) {
        const std::string name = "u_uv[" + std::to_string(index) + "]";
        glUniformMatrix3fv(Uniform(name.c_str()), 1, GL_FALSE, identity3.data());
    }
    glUniform4f(Uniform("u_baseFactor"), .5f, .5f, .5f, 1);
    glUniform3f(Uniform("u_emissiveFactor"), 0, 0, 0);
    glUniform1f(Uniform("u_metallicFactor"), .5f);
    glUniform1f(Uniform("u_roughnessFactor"), .5f);
    glUniform1f(Uniform("u_normalScale"), 1);
    glUniform1f(Uniform("u_occlusionStrength"), .5f);
    glUniform1f(Uniform("u_alphaCutoff"), .5f);
    glUniform1i(Uniform("u_mask"), 0);
    glUniform1i(Uniform("u_doubleSided"), 0);
    glUniform1i(Uniform("u_hasNormal"), 0);
    constexpr std::array<const char*, 5> samplers{
        "u_baseColor", "u_metallicRoughness", "u_normalMap", "u_occlusion", "u_emissive"};
    constexpr std::array<float, 4> texel{.25f, .5f, .75f, 1};
    glGenTextures(static_cast<GLsizei>(textures.size()), textures.data());
    for (std::size_t index = 0; index < textures.size(); ++index) {
        glActiveTexture(GL_TEXTURE0 + static_cast<GLenum>(index));
        glBindTexture(GL_TEXTURE_2D, textures[index]);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA32F, 1, 1, 0, GL_RGBA, GL_FLOAT, texel.data());
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glUniform1i(Uniform(samplers[index]), static_cast<GLint>(index));
    }
    glGenFramebuffers(1, &framebuffer);
    glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
    glGenTextures(static_cast<GLsizei>(attachments.size()), attachments.data());
    std::array<GLenum, 6> buffers{};
    for (std::size_t index = 0; index < attachments.size(); ++index) {
        glBindTexture(GL_TEXTURE_2D, attachments[index]);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA32F, Size, Size, 0, GL_RGBA, GL_FLOAT, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        buffers[index] = GL_COLOR_ATTACHMENT0 + static_cast<GLenum>(index);
        glFramebufferTexture2D(
            GL_FRAMEBUFFER, buffers[index], GL_TEXTURE_2D, attachments[index], 0);
    }
    // The framebuffer setup used the last sampler unit; restore its fixed input.
    glBindTexture(GL_TEXTURE_2D, textures.back());
    glDrawBuffers(static_cast<GLsizei>(buffers.size()), buffers.data());
    ASSERT_EQ(glCheckFramebufferStatus(GL_FRAMEBUFFER), GL_FRAMEBUFFER_COMPLETE);
    constexpr std::array<float, 24> vertices{-1, -1, 0, 0, 0,  1, 0, 0, 3, -1, 0, 0,
                                             0,  1,  2, 0, -1, 3, 0, 0, 0, 1,  0, 2};
    glGenVertexArrays(1, &vao);
    glBindVertexArray(vao);
    glGenBuffers(1, &vbo);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices.data(), GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 8 * sizeof(float), nullptr);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1,
                          3,
                          GL_FLOAT,
                          GL_FALSE,
                          8 * sizeof(float),
                          reinterpret_cast<const void*>(3 * sizeof(float)));
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(2,
                          2,
                          GL_FLOAT,
                          GL_FALSE,
                          8 * sizeof(float),
                          reinterpret_cast<const void*>(6 * sizeof(float)));
    glViewport(0, 0, Size, Size);
    constexpr std::array<float, 4> clear{-1, -1, -1, -1};
    for (int index = 0; index < 6; ++index) {
        glClearBufferfv(GL_COLOR, index, clear.data());
    }
    glDrawArrays(GL_TRIANGLES, 0, 3);
    std::array<float, PixelCount * 3> pixels{};
    glReadBuffer(GL_COLOR_ATTACHMENT2);
    glReadPixels(0, 0, Size, Size, GL_RGB, GL_FLOAT, pixels.data());
    for (std::size_t index = 0; index < pixels.size(); ++index) {
        ASSERT_TRUE(std::isfinite(pixels[index]));
        EXPECT_FLOAT_EQ(pixels[index], texel[index % 3] * .5f);
    }
    std::array<float, PixelCount * 2> material{};
    glReadBuffer(GL_COLOR_ATTACHMENT3);
    glReadPixels(0, 0, Size, Size, GL_RG, GL_FLOAT, material.data());
    for (std::size_t index = 0; index < PixelCount; ++index) {
        EXPECT_FLOAT_EQ(material[index * 2], .375f);
        EXPECT_FLOAT_EQ(material[index * 2 + 1], .625f);
    }
    ASSERT_EQ(glGetError(), GL_NO_ERROR);
    ASSERT_FALSE(HasFailure());
    const char* output = std::getenv("LUMINUMBRA_SHADER_SCENE_OUTPUT");
    ASSERT_NE(output, nullptr);
    ASSERT_NE(glGetString(GL_VERSION), nullptr);
    const std::filesystem::path directory(output);
    const std::string fixtureHash = LUMINUMBRA_SHADER_SCENE_INPUT_HASH;
    ASSERT_EQ(fixtureHash.size(), 64u) << "Build the registered scene target";
    std::ofstream identity(directory / "fixture-sha256.txt", std::ios::binary);
    identity << fixtureHash << '\n';
    identity.close();
    ASSERT_TRUE(identity);
    std::ofstream image(directory / "material.pfm", std::ios::binary);
    image << "PF\n128 128\n-1.0\n";
    for (const float value : pixels) {
        const auto bits = std::bit_cast<std::uint32_t>(value);
        const std::array<char, 4> bytes{static_cast<char>(bits),
                                        static_cast<char>(bits >> 8),
                                        static_cast<char>(bits >> 16),
                                        static_cast<char>(bits >> 24)};
        image.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    }
    image.close();
    ASSERT_TRUE(image);
    constexpr std::array<std::pair<const char*, GLenum>, 2> metadataFiles{
        std::pair<const char*, GLenum>{"renderer.txt", GL_RENDERER},
        std::pair<const char*, GLenum>{"version.txt", GL_VERSION}};
    for (const auto& [filename, name] : metadataFiles) {
        std::ofstream metadata(directory / filename, std::ios::binary);
        metadata << reinterpret_cast<const char*>(glGetString(name)) << '\n';
        metadata.close();
        ASSERT_TRUE(metadata);
    }
}
} // namespace
