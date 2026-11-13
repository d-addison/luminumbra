#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>
#include <glad/glad.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <ios>
#include <string>
#include <utility>

// A standalone syntax check needs no configured binary identity.
#ifndef LUMINUMBRA_SHADER_SCENE_INPUT_HASH
#define LUMINUMBRA_SHADER_SCENE_INPUT_HASH ""
#endif

namespace {
constexpr int Size = 128;
// Fixed fixture: six RGBA float attachments and one RGB readback, below 2 MiB.
constexpr std::size_t PixelCount = static_cast<std::size_t>(Size) * static_cast<std::size_t>(Size);

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
    static std::string Source(const std::string& filename) {
        std::ifstream file(std::filesystem::path(LUMINUMBRA_SOURCE_ROOT) / filename,
                           std::ios::binary);
        if (!file) {
            ADD_FAILURE() << filename;
            return {};
        }
        // Shader inputs are fixed repository files, capped before compilation.
        constexpr std::streamsize MaxShaderBytes = 262144;
        std::array<char, static_cast<std::size_t>(MaxShaderBytes) + 1> sourceBytes{};
        file.read(sourceBytes.data(), static_cast<std::streamsize>(sourceBytes.size()));
        if (file.bad() || file.gcount() > MaxShaderBytes) {
            ADD_FAILURE() << filename << ": unreadable or oversized shader";
            return {};
        }
        const std::string source(sourceBytes.data(), static_cast<std::size_t>(file.gcount()));
        const char* output = std::getenv("LUMINUMBRA_SHADER_SCENE_OUTPUT");
        if (output == nullptr) {
            ADD_FAILURE() << "Missing scene output directory";
            return {};
        }
        std::ofstream snapshot(std::filesystem::path(output) /
                                   std::filesystem::path(filename).filename(),
                               std::ios::binary);
        snapshot.write(source.data(), static_cast<std::streamsize>(source.size()));
        snapshot.close();
        if (!snapshot) {
            ADD_FAILURE() << "Cannot record consumed shader source";
            return {};
        }
        return source;
    }
    static GLuint CompileSource(GLenum stage, const std::string& source, const std::string& label) {
        if (source.empty()) {
            ADD_FAILURE() << label << ": empty shader source";
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
            ADD_FAILURE() << label << ": " << log.data();
            glDeleteShader(shader);
            return 0;
        }
        return shader;
    }
    static GLuint Compile(GLenum stage, const std::string& filename) {
        return CompileSource(stage, Source("res/shaders/" + filename), filename);
    }
    static GLuint Link(GLuint vertex, GLuint fragment = 0) {
        const GLuint linkedProgram = glCreateProgram();
        glAttachShader(linkedProgram, vertex);
        if (fragment != 0) {
            glAttachShader(linkedProgram, fragment);
        }
        glLinkProgram(linkedProgram);
        glDeleteShader(vertex);
        glDeleteShader(fragment);
        GLint linked = GL_FALSE;
        glGetProgramiv(linkedProgram, GL_LINK_STATUS, &linked);
        if (linked != GL_TRUE) {
            std::array<char, 8192> log{};
            glGetProgramInfoLog(
                linkedProgram, static_cast<GLsizei>(log.size()), nullptr, log.data());
            ADD_FAILURE() << "Scene program did not link: " << log.data();
            glDeleteProgram(linkedProgram);
            return 0;
        }
        return linkedProgram;
    }
    GLint Uniform(const char* name) const {
        const GLint location = glGetUniformLocation(program, name);
        EXPECT_GE(location, 0) << name;
        return location;
    }
    // Three fixed production compute kernels, at most 192*108*3 floats of
    // scratch. Their march bounds are 40, 16*20 and 30 samples per texel.
    void SkyLuts() {
        const std::string source = Source("src/luminumbra_client/rendering/SkyAtmosphereLut.ipp");
        ASSERT_FALSE(source.empty());
        constexpr std::array<const char*, 3> kernels{
            "kTransmittanceComputeSrc", "kMultiscatterComputeSrc", "kSkyViewComputeSrc"};
        constexpr std::array<int, 3> widths{256, 32, 192};
        constexpr std::array<int, 3> heights{64, 32, 108};
        glGenTextures(static_cast<GLsizei>(textures.size()), textures.data());
        glGenBuffers(1, &vbo);
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, vbo);
        glBufferData(
            GL_SHADER_STORAGE_BUFFER, 192 * 108 * 3 * sizeof(float), nullptr, GL_STATIC_COPY);
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, vbo);
        for (std::size_t index = 0; index < kernels.size(); ++index) {
            const auto declaration = source.find(std::string("const char* ") + kernels[index]);
            ASSERT_NE(declaration, std::string::npos);
            const auto begin = source.find("R\"GLSL(", declaration);
            ASSERT_NE(begin, std::string::npos);
            const auto end = source.find(")GLSL\"", begin + 7);
            ASSERT_NE(end, std::string::npos);
            const GLuint shader = CompileSource(
                GL_COMPUTE_SHADER, source.substr(begin + 7, end - begin - 7), kernels[index]);
            ASSERT_NE(shader, 0u);
            const GLuint compute = Link(shader);
            ASSERT_NE(compute, 0u);
            glUseProgram(compute);
            if (index > 0) {
                const GLint transmittance = glGetUniformLocation(compute, "u_transmittance");
                ASSERT_GE(transmittance, 0);
                glUniform1i(transmittance, 0);
            }
            if (index == 2) {
                const GLint multiscatter = glGetUniformLocation(compute, "u_multiscatter");
                const GLint sun = glGetUniformLocation(compute, "u_sunDir");
                ASSERT_GE(multiscatter, 0);
                ASSERT_GE(sun, 0);
                glUniform1i(multiscatter, 1);
                glUniform3f(sun, 0, -.08f, .9967949f);
            }
            glDispatchCompute(static_cast<GLuint>((widths[index] + 7) / 8),
                              static_cast<GLuint>((heights[index] + 7) / 8),
                              1);
            glMemoryBarrier(GL_ALL_BARRIER_BITS);
            glActiveTexture(GL_TEXTURE0 + static_cast<GLenum>(index));
            glBindTexture(GL_TEXTURE_2D, textures[index]);
            glBindBuffer(GL_PIXEL_UNPACK_BUFFER, vbo);
            glTexImage2D(GL_TEXTURE_2D,
                         0,
                         GL_RGB16F,
                         widths[index],
                         heights[index],
                         0,
                         GL_RGB,
                         GL_FLOAT,
                         nullptr);
            glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            glMemoryBarrier(GL_ALL_BARRIER_BITS);
            glDeleteProgram(compute);
        }
    }
    void Sky(bool dusk) {
        // Point rasterization supplies one exact direction per pixel to the real
        // fragment shader; no dome interpolation or projection seam is added.
        const std::string vertexSource = R"GLSL(#version 450 core
out vec3 WorldPos;
uniform int dusk;
void main() {
    int x = gl_VertexID % 128, y = gl_VertexID / 128;
    gl_Position = vec4((vec2(x, y) + 0.5) / 64.0 - 1.0, 0.0, 1.0);
    gl_PointSize = 1.0;
    float az = (float(x) + 0.5) * 6.28318530718 / 128.0;
    float elevation = dusk != 0 ? 0.001 + float(y) * 0.018 / 127.0
                               : float(y - 63) * 0.5 / 63.0;
    WorldPos = vec3(cos(elevation) * sin(az), sin(elevation), cos(elevation) * cos(az));
    if (dusk == 0 && y >= 64) {
        // Four independently seeded stars close to the radius-320 view sphere.
        const vec3 centres[4] = vec3[4](
            vec3(-197.36940163, 101.71339470, 230.46139854),
            vec3(-196.43707782, 101.36761558, 231.39882773),
            vec3(-49.56314933, 106.34229138, 297.72593805),
            vec3(-118.53462833, 111.27755010, 275.63571492));
        vec3 ray = normalize(centres[x / 32]);
        vec3 right = normalize(cross(ray, vec3(0, 1, 0)));
        vec3 up = cross(right, ray);
        vec2 offset = vec2(float(x % 32) - 16.3, float(y) - 96.2);
        WorldPos = normalize(ray + (right * offset.x + up * offset.y) * (0.02 / 320.0));
    }
}
)GLSL";
        if (dusk) {
            SkyLuts();
            ASSERT_FALSE(HasFatalFailure());
        } else {
            glGenTextures(static_cast<GLsizei>(textures.size()), textures.data());
            glBindTexture(GL_TEXTURE_2D, textures[0]);
            constexpr std::array<float, 3> black{};
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB32F, 1, 1, 0, GL_RGB, GL_FLOAT, black.data());
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        }
        const GLuint vertex = CompileSource(GL_VERTEX_SHADER, vertexSource, "sky directions");
        const GLuint fragment = Compile(GL_FRAGMENT_SHADER, "enhanced_skybox.frag");
        ASSERT_NE(vertex, 0u);
        ASSERT_NE(fragment, 0u);
        program = Link(vertex, fragment);
        ASSERT_NE(program, 0u);
        glUseProgram(program);
        glUniform1i(Uniform("dusk"), dusk ? 1 : 0);
        glUniform3f(Uniform("u_sunDirection"), 0, dusk ? -.08f : -1.f, dusk ? .9967949f : 0.f);
        glUniform3f(Uniform("u_moonDirection"), 0, 0, 0);
        glUniform1f(Uniform("u_sunCosZenith"), dusk ? -.08f : -1.f);
        glUniform1f(Uniform("u_skyDayFactor"), dusk ? .43f : 0.f);
        glUniform1f(Uniform("u_time"), 0);
        glUniform1f(Uniform("u_atmosDensity"), dusk ? 1.f : 0.f);
        glUniform1f(Uniform("u_cloudCoverageAmount"), 0);
        glUniform1f(Uniform("u_cloudBiomeVariation"), 0);
        glUniform1f(Uniform("u_cloudPlaneHeight"), 900);
        glUniform2f(Uniform("u_cloudScrollOffset"), 0, 0);
        glUniform1f(Uniform("u_auroraStrength"), 0);
        glUniform1f(Uniform("u_stormSkyFloor"), 0);
        glUniform1i(Uniform("u_backdropMode"), 0);
        glUniform1i(Uniform("u_useSkyLut"), dusk ? 1 : 0);
        glUniform1f(Uniform("u_skyExposure"), 38);
        glUniform1i(Uniform("u_transmittanceLut"), 0);
        glUniform1i(Uniform("u_skyViewLut"), dusk ? 2 : 0);
        glGenVertexArrays(1, &vao);
        glBindVertexArray(vao);
        glGenFramebuffers(1, &framebuffer);
        glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
        glGenTextures(1, attachments.data());
        // Keep sampler units intact while allocating the bounded float target.
        glActiveTexture(GL_TEXTURE4);
        glBindTexture(GL_TEXTURE_2D, attachments[0]);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB32F, Size, Size, 0, GL_RGB, GL_FLOAT, nullptr);
        glFramebufferTexture2D(
            GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, attachments[0], 0);
        ASSERT_EQ(glCheckFramebufferStatus(GL_FRAMEBUFFER), GL_FRAMEBUFFER_COMPLETE);
        glViewport(0, 0, Size, Size);
        glDrawArrays(GL_POINTS, 0, static_cast<GLsizei>(PixelCount));
        std::array<float, PixelCount * 3> pixels{};
        glReadPixels(0, 0, Size, Size, GL_RGB, GL_FLOAT, pixels.data());
        for (float& value : pixels) {
            ASSERT_TRUE(std::isfinite(value));
            ASSERT_GE(value, 0);
            // Apply the display clamp and undo the shader's explicit gamma.
            value = std::pow(std::min(value, 1.f), 2.2f);
        }
        ASSERT_EQ(glGetError(), GL_NO_ERROR);
        ASSERT_FALSE(HasFailure());
        Save(dusk ? "dusk-sky" : "night-sky", pixels);
    }
    static void Save(const char* name, const std::array<float, PixelCount * 3>& pixels) {
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
        std::ofstream image(directory / (std::string(name) + ".pfm"), std::ios::binary);
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
        for (const auto& [filename, glName] : metadataFiles) {
            std::ofstream metadata(directory / filename, std::ios::binary);
            metadata << reinterpret_cast<const char*>(glGetString(glName)) << '\n';
            metadata.close();
            ASSERT_TRUE(metadata);
        }
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
    Save("material", pixels);
}

TEST_F(ShaderScene, TreeImpostorAtlasCutout) {
    const GLuint vertex = Compile(GL_VERTEX_SHADER, "tree_impostor.vert");
    const GLuint fragment = Compile(GL_FRAGMENT_SHADER, "tree_impostor.frag");
    ASSERT_NE(vertex, 0u);
    ASSERT_NE(fragment, 0u);
    program = glCreateProgram();
    glAttachShader(program, vertex);
    glAttachShader(program, fragment);
    glLinkProgram(program);
    glDeleteShader(vertex);
    glDeleteShader(fragment);
    GLint linked = GL_FALSE;
    glGetProgramiv(program, GL_LINK_STATUS, &linked);
    ASSERT_EQ(linked, GL_TRUE);
    glUseProgram(program);
    constexpr std::array<float, 16> identity{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    glUniformMatrix4fv(Uniform("u_view"), 1, GL_FALSE, identity.data());
    glUniformMatrix4fv(Uniform("u_proj"), 1, GL_FALSE, identity.data());
    glUniform3f(Uniform("u_cameraPos"), 0, 0, 2);
    glUniform3f(Uniform("u_center"), 0, 0, 0);
    glUniform1f(Uniform("u_radius"), 1);
    glUniform1f(Uniform("u_grid"), 2);
    glUniform1f(Uniform("u_materialId"), 4.f / 255.f);
    glGenVertexArrays(1, &vao);
    glBindVertexArray(vao);
    // Disabled matrix attributes supply the one fixed instance without buffers.
    for (GLuint column = 0; column < 4; ++column) {
        glVertexAttrib4fv(column, identity.data() + static_cast<std::size_t>(column) * 4);
    }
    glVertexAttrib4f(4, 1, 1, 1, 1);
    glGenTextures(static_cast<GLsizei>(textures.size()), textures.data());
    // 2x2 tiles, each 4x4 texels. Only the selected (+Z) tile (1,1) has
    // transparent, just-below-cutoff, exact-cutoff and opaque texels. Other
    // tiles are opaque red, making a wrong octahedral tile selection visible.
    std::array<float, 256> atlas{};
    std::array<float, 256> normals{};
    constexpr std::array<float, 4> alpha{0, .499f, .5f, 1};
    for (int y = 0; y < 8; ++y) {
        for (int x = 0; x < 8; ++x) {
            const auto offset = (static_cast<std::size_t>(y) * 8 + static_cast<std::size_t>(x)) * 4;
            const bool selected = x >= 4 && y >= 4;
            const float coverage = selected ? alpha[static_cast<std::size_t>((x + y) % 4)] : 1;
            atlas[offset] = (selected ? .25f : 1.f) * coverage;
            atlas[offset + 1] = selected ? .5f * coverage : 0;
            atlas[offset + 2] = selected ? .75f * coverage : 0;
            atlas[offset + 3] = coverage;
            normals[offset] = .5f * coverage;
            normals[offset + 1] = .5f * coverage;
            normals[offset + 2] = .75f * coverage;
            normals[offset + 3] = coverage;
        }
    }
    for (int unit = 0; unit < 2; ++unit) {
        glActiveTexture(GL_TEXTURE0 + static_cast<GLenum>(unit));
        glBindTexture(GL_TEXTURE_2D, textures[static_cast<std::size_t>(unit)]);
        glTexImage2D(GL_TEXTURE_2D,
                     0,
                     GL_RGBA32F,
                     8,
                     8,
                     0,
                     GL_RGBA,
                     GL_FLOAT,
                     unit == 0 ? atlas.data() : normals.data());
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glUniform1i(Uniform(unit == 0 ? "u_albedo" : "u_normal"), unit);
    }
    glGenFramebuffers(1, &framebuffer);
    glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
    glGenTextures(static_cast<GLsizei>(attachments.size()), attachments.data());
    std::array<GLenum, 6> buffers{};
    for (std::size_t index = 0; index < attachments.size(); ++index) {
        glBindTexture(GL_TEXTURE_2D, attachments[index]);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA32F, Size, Size, 0, GL_RGBA, GL_FLOAT, nullptr);
        buffers[index] = GL_COLOR_ATTACHMENT0 + static_cast<GLenum>(index);
        glFramebufferTexture2D(
            GL_FRAMEBUFFER, buffers[index], GL_TEXTURE_2D, attachments[index], 0);
    }
    glBindTexture(GL_TEXTURE_2D, textures[1]);
    glDrawBuffers(static_cast<GLsizei>(buffers.size()), buffers.data());
    ASSERT_EQ(glCheckFramebufferStatus(GL_FRAMEBUFFER), GL_FRAMEBUFFER_COMPLETE);
    constexpr std::array<float, 4> background{.0625f, .125f, .1875f, .25f};
    for (int index = 0; index < 6; ++index) {
        glClearBufferfv(GL_COLOR, index, background.data());
    }
    glViewport(0, 0, Size, Size);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    // Check all six attachments: discard must preserve even outputs assigned
    // before the discard, including gAuthoredSurface. Coverage is exactly 50%.
    std::array<float, PixelCount * 4> values{};
    std::array<float, PixelCount * 3> pixels{};
    constexpr std::array<std::size_t, 6> outputComponents{3, 4, 4, 2, 2, 4};
    for (int attachment = 0; attachment < 6; ++attachment) {
        glReadBuffer(GL_COLOR_ATTACHMENT0 + static_cast<GLenum>(attachment));
        glReadPixels(0, 0, Size, Size, GL_RGBA, GL_FLOAT, values.data());
        for (int y = 0; y < Size; ++y) {
            for (int x = 0; x < Size; ++x) {
                const auto pixel = static_cast<std::size_t>(y) * static_cast<std::size_t>(Size) +
                                   static_cast<std::size_t>(x);
                const bool discarded = (x / 32 + y / 32) % 4 < 2;
                for (std::size_t channel = 0; channel < 4; ++channel) {
                    const float value = values[pixel * 4 + channel];
                    // Surviving vec2/vec3 outputs do not define the unused
                    // RGBA components. Discard preserves all cleared channels.
                    if (discarded ||
                        channel < outputComponents[static_cast<std::size_t>(attachment)]) {
                        ASSERT_TRUE(std::isfinite(value));
                    }
                    if (discarded) {
                        EXPECT_FLOAT_EQ(value, background[channel]);
                    } else if (attachment == 2) {
                        constexpr std::array<float, 4> expected{.25f, .5f, .75f, .75f};
                        EXPECT_FLOAT_EQ(value, expected[channel]);
                    }
                    if (attachment == 2 && channel < 3) {
                        pixels[pixel * 3 + channel] = value;
                    }
                }
            }
        }
    }
    ASSERT_EQ(glGetError(), GL_NO_ERROR);
    ASSERT_FALSE(HasFailure());
    Save("tree-cutout", pixels);
}
TEST_F(ShaderScene, TerrainMaterialTransition) {
    const GLuint vertex = Compile(GL_VERTEX_SHADER, "g_buffer.vert");
    const GLuint fragment = Compile(GL_FRAGMENT_SHADER, "g_buffer.frag");
    ASSERT_NE(vertex, 0u);
    ASSERT_NE(fragment, 0u);
    program = Link(vertex, fragment);
    ASSERT_NE(program, 0u);
    glUseProgram(program);
    constexpr std::array<float, 16> identity{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    constexpr std::array<float, 9> rotation{1, 0, 0, 0, 1, 0, 0, 0, 1};
    for (const char* name : {"model", "view", "projection", "u_prev_view_proj"}) {
        glUniformMatrix4fv(Uniform(name), 1, GL_FALSE, identity.data());
    }
    for (const char* name : {"normalMatrix", "u_normalViewMatrix"}) {
        glUniformMatrix3fv(Uniform(name), 1, GL_FALSE, rotation.data());
    }
    glUniform1i(Uniform("u_materialLUT"), 0);
    for (const char* name : {"u_terrainTextures",
                             "u_terrainNormals",
                             "u_terrainRoughness",
                             "u_skinnedTextures",
                             "u_staticSurface"}) {
        glUniform1i(Uniform(name), 1);
    }
    glUniform1i(Uniform("u_skinnedAlbedoLayer"), -1);
    glUniform1i(Uniform("u_skinnedNormalLayer"), -1);
    glUniform1i(Uniform("u_macroRockOverlay"), 0);
    // Suppress the later macro mottle; isolate interpolated terrain coverage.
    glUniform1i(Uniform("u_staticSurfaceLayer"), 0);
    glUniform1i(Uniform("u_forceFlat"), 0);
    glGenTextures(static_cast<GLsizei>(textures.size()), textures.data());
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, textures[0]);
    // Zero has_texture selects the shader's constant Stone/Sand reflectances.
    // Four rows, 256 entries; no external assets or filtering dependencies.
    std::array<float, 256 * 4 * 4> lut{};
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA32F, 256, 4, 0, GL_RGBA, GL_FLOAT, lut.data());
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D_ARRAY, textures[1]);
    constexpr std::array<float, 4> unused{};
    glTexImage3D(GL_TEXTURE_2D_ARRAY, 0, GL_RGBA32F, 1, 1, 1, 0, GL_RGBA, GL_FLOAT, unused.data());
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    struct Vertex {
        std::array<float, 3> position;
        std::uint32_t material;
    };
    constexpr std::array<Vertex, 4> vertices{
        Vertex{{-1, -1, 0}, 1}, Vertex{{1, -1, 0}, 4}, Vertex{{-1, 1, 0}, 1}, Vertex{{1, 1, 0}, 4}};
    glGenVertexArrays(1, &vao);
    glBindVertexArray(vao);
    glGenBuffers(1, &vbo);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices.data(), GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(Vertex), nullptr);
    glVertexAttrib3f(1, 0, 0, 1);
    glEnableVertexAttribArray(2);
    glVertexAttribIPointer(2,
                           1,
                           GL_UNSIGNED_INT,
                           sizeof(Vertex),
                           reinterpret_cast<const void*>(offsetof(Vertex, material)));
    glGenFramebuffers(1, &framebuffer);
    glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
    glGenTextures(static_cast<GLsizei>(attachments.size()), attachments.data());
    std::array<GLenum, 6> buffers{};
    glActiveTexture(GL_TEXTURE2);
    for (std::size_t index = 0; index < attachments.size(); ++index) {
        glBindTexture(GL_TEXTURE_2D, attachments[index]);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA32F, Size, Size, 0, GL_RGBA, GL_FLOAT, nullptr);
        buffers[index] = GL_COLOR_ATTACHMENT0 + static_cast<GLenum>(index);
        glFramebufferTexture2D(
            GL_FRAMEBUFFER, buffers[index], GL_TEXTURE_2D, attachments[index], 0);
    }
    glDrawBuffers(static_cast<GLsizei>(buffers.size()), buffers.data());
    ASSERT_EQ(glCheckFramebufferStatus(GL_FRAMEBUFFER), GL_FRAMEBUFFER_COMPLETE);
    glViewport(0, 0, Size, Size);
    constexpr std::array<float, 4> clear{-1, -1, -1, -1};
    glClearBufferfv(GL_COLOR, 2, clear.data());
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    std::array<float, PixelCount * 3> pixels{};
    glReadBuffer(GL_COLOR_ATTACHMENT2);
    glReadPixels(0, 0, Size, Size, GL_RGB, GL_FLOAT, pixels.data());
    for (const float value : pixels) {
        ASSERT_TRUE(std::isfinite(value));
    }
    ASSERT_EQ(glGetError(), GL_NO_ERROR);
    ASSERT_FALSE(HasFailure());
    Save("terrain-blend", pixels);
}

TEST_F(ShaderScene, NightSkyHorizonAndRadialStars) {
    Sky(false);
}

TEST_F(ShaderScene, DuskSkyAzimuthContinuity) {
    Sky(true);
}
} // namespace
