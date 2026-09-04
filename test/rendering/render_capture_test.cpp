#include "gtest/gtest.h"

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>
#include <glad/glad.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <system_error>
#include <vector>

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>

#include "systems/SHIELD_WorldSystem.h"
#include "systems/WaterSystem.h"
#include "world/Chunk.h"
#include "world/MarchingCubes.h"

// the capture-SDK trigger under test, plus the real RenderDoc in-app API
// header (used here only to build an injected API double).
#include "renderdoc/renderdoc_app.h"
#include "rendering/CaptureHooks.h"

//  rendering ( /  ): the shipping manual-exposure model
// under test (the SAME functions main_client.cpp + RenderPipeline.cpp call).
#include "rendering/ExposureModel.h"
#include "rendering/SunLightModel.h"
// the pure time-of-day policy facets under test.
#include "rendering/TimeOfDayModel.h"
// the declarative frame graph under test.
#include "rendering/CelestialBodyModel.h" //  Tier 1: the celestial-body seam
#include "rendering/FroxelGrid.h"         //  rendering: the froxel grid model
#include "rendering/GlassTintModel.h"     // Tinted transmission.
#include "rendering/OitModel.h"           // Weighted blended OIT model.
#include "rendering/RenderGraph.h"
#include "rendering/SnowCoverModel.h" // Render-only snow cover.

namespace fs = std::filesystem;

using namespace Luminumbra;
using namespace Luminumbra::Systems;

namespace {

#ifndef LUMINUMBRA_SOURCE_ROOT
#define LUMINUMBRA_SOURCE_ROOT "."
#endif

#ifndef LUMINUMBRA_TEST_ARTIFACT_DIR
#define LUMINUMBRA_TEST_ARTIFACT_DIR "."
#endif

constexpr int kCaptureWidth = 256;
constexpr int kCaptureHeight = 256;
constexpr int kSeed = 424242;

struct ImageMetrics {
    std::size_t nonblack_pixels = 0;
    std::size_t foreground_pixels = 0;
    std::size_t clipped_dark_pixels = 0;
    std::size_t clipped_bright_pixels = 0;
    double mean_luminance = 0.0;
    int min_luminance = 255;
    int max_luminance = 0;
};

struct CaptureScene {
    std::string name;
    std::vector<VoxelVertex> vertices;
    std::vector<u32> indices;
    Vec3 object_color{0.6f, 0.8f, 0.5f};
    glm::vec3 camera_position{18.0f, 18.0f, 32.0f};
    glm::vec3 camera_target{8.0f, 7.0f, 8.0f};
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

        m_window = glfwCreateWindow(64, 64, "render_capture_test", nullptr, nullptr);
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

    bool ready() const {
        return m_ready;
    }
    const std::string& error() const {
        return m_error;
    }

private:
    GLFWwindow* m_window = nullptr;
    bool m_glfw_initialized = false;
    bool m_ready = false;
    std::string m_error;
};

fs::path SourceRoot() {
    return fs::weakly_canonical(fs::path(LUMINUMBRA_SOURCE_ROOT));
}

fs::path ArtifactRoot() {
    return fs::path(LUMINUMBRA_TEST_ARTIFACT_DIR) / "render_captures";
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

std::string GetShaderInfoLog(GLuint shader) {
    GLint log_length = 0;
    glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &log_length);
    if (log_length <= 1) {
        return {};
    }
    std::string log(static_cast<std::size_t>(log_length), '\0');
    glGetShaderInfoLog(shader, log_length, nullptr, log.data());
    return log;
}

std::string GetProgramInfoLog(GLuint program) {
    GLint log_length = 0;
    glGetProgramiv(program, GL_INFO_LOG_LENGTH, &log_length);
    if (log_length <= 1) {
        return {};
    }
    std::string log(static_cast<std::size_t>(log_length), '\0');
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
        ADD_FAILURE() << "Shader failed to compile: " << path.string() << "\n"
                      << GetShaderInfoLog(shader);
        glDeleteShader(shader);
        return 0;
    }

    return shader;
}

GLuint LinkBasicProgram() {
    const fs::path shader_root = SourceRoot() / "res/shaders";
    GLuint vertex = CompileShader(shader_root / "basic.vert", GL_VERTEX_SHADER);
    GLuint fragment = CompileShader(shader_root / "basic.frag", GL_FRAGMENT_SHADER);
    if (vertex == 0 || fragment == 0) {
        if (vertex != 0)
            glDeleteShader(vertex);
        if (fragment != 0)
            glDeleteShader(fragment);
        return 0;
    }

    GLuint program = glCreateProgram();
    glAttachShader(program, vertex);
    glAttachShader(program, fragment);
    glLinkProgram(program);
    glDeleteShader(vertex);
    glDeleteShader(fragment);

    GLint success = GL_FALSE;
    glGetProgramiv(program, GL_LINK_STATUS, &success);
    if (success != GL_TRUE) {
        ADD_FAILURE() << "basic shader failed to link\n" << GetProgramInfoLog(program);
        glDeleteProgram(program);
        return 0;
    }

    return program;
}

void SetMat4(GLuint program, const char* name, const glm::mat4& value) {
    glUniformMatrix4fv(glGetUniformLocation(program, name), 1, GL_FALSE, glm::value_ptr(value));
}

void SetMat3(GLuint program, const char* name, const glm::mat3& value) {
    glUniformMatrix3fv(glGetUniformLocation(program, name), 1, GL_FALSE, glm::value_ptr(value));
}

TerrainGenParams CaptureTerrainParams() {
    TerrainGenParams params;
    params.base_frequency = 0.035f;
    params.base_amplitude = 8.0f;
    params.octaves = 5;
    params.persistence = 0.5f;
    params.lacunarity = 2.1f;
    params.height_offset = 10.0f;
    params.caves_enabled = true;
    params.cave_frequency = 0.15f;
    params.cave_threshold = 0.55f;
    params.cave_carve_value = 5.0f;
    params.island_mask_enabled = true;
    params.island_mask_frequency = 0.075f;
    return params;
}

TerrainGenParams CaptureWaterParams() {
    TerrainGenParams params;
    params.base_frequency = 0.02f;
    params.base_amplitude = 1.0f;
    params.height_offset = -4.0f;
    params.caves_enabled = false;
    params.island_mask_enabled = false;
    return params;
}

CaptureScene BuildTerrainScene() {
    SHIELD_WorldSystem world(nullptr, nullptr, CaptureTerrainParams(), kSeed);
    Chunk chunk({0, 0, 0});
    world.GenerateChunkData(chunk);
    World::MarchingCubes::PolygoniseTerrain(world, chunk, 0.0f, 1);

    CaptureScene scene;
    scene.name = "terrain_caves";
    scene.vertices = std::move(chunk.mesh_vertices);
    scene.indices = std::move(chunk.mesh_indices);
    scene.object_color = Vec3{0.44f, 0.72f, 0.38f};
    scene.camera_position = glm::vec3{21.0f, 19.0f, 31.0f};
    scene.camera_target = glm::vec3{8.0f, 7.0f, 8.0f};
    return scene;
}

CaptureScene BuildWaterScene() {
    SHIELD_WorldSystem world(nullptr, nullptr, CaptureWaterParams(), kSeed);
    WaterSystem water(nullptr, &world);
    Chunk chunk({0, 0, 0});
    world.GenerateChunkData(chunk);
    chunk.water_level_data.assign(static_cast<std::size_t>(WATER_SIM_RESOLUTION_X) *
                                      static_cast<std::size_t>(WATER_SIM_RESOLUTION_Z),
                                  SEA_LEVEL);
    chunk.has_water_sim.store(true);
    World::MarchingCubes::GenerateWaterMesh(water, world, chunk);

    CaptureScene scene;
    scene.name = "water_submerged";
    scene.vertices = std::move(chunk.water_mesh_vertices);
    scene.indices = std::move(chunk.water_mesh_indices);
    scene.object_color = Vec3{0.18f, 0.48f, 0.9f};
    scene.camera_position = glm::vec3{18.0f, 14.0f, 28.0f};
    scene.camera_target = glm::vec3{8.0f, 0.0f, 8.0f};
    return scene;
}

CaptureScene BuildFlatQuadScene(const std::string& name,
                                const Vec3& color,
                                float half_width,
                                float half_height) {
    CaptureScene scene;
    scene.name = name;
    scene.object_color = color;
    scene.camera_position = glm::vec3{0.0f, 0.0f, 9.0f};
    scene.camera_target = glm::vec3{0.0f, 0.0f, 0.0f};
    scene.vertices = {
        {Vec3{-half_width, -half_height, 0.0f}, Vec3{0.0f, 0.0f, 1.0f}, 3u},
        {Vec3{half_width, -half_height, 0.0f}, Vec3{0.0f, 0.0f, 1.0f}, 3u},
        {Vec3{half_width, half_height, 0.0f}, Vec3{0.0f, 0.0f, 1.0f}, 3u},
        {Vec3{-half_width, half_height, 0.0f}, Vec3{0.0f, 0.0f, 1.0f}, 3u},
    };
    scene.indices = {0u, 1u, 2u, 2u, 3u, 0u};
    return scene;
}

CaptureScene BuildSkyScene() {
    return BuildFlatQuadScene("sky_gradient", Vec3{0.18f, 0.38f, 0.95f}, 4.2f, 2.4f);
}

CaptureScene BuildUiOverlayScene() {
    return BuildFlatQuadScene("ui_overlay", Vec3{0.88f, 0.96f, 0.92f}, 2.8f, 1.2f);
}

CaptureScene BuildLoadingVisualizerScene() {
    return BuildFlatQuadScene("loading_visualizer", Vec3{0.95f, 0.62f, 0.22f}, 3.0f, 1.8f);
}

ImageMetrics CalculateImageMetrics(const std::vector<unsigned char>& rgba) {
    ImageMetrics metrics;
    double sum = 0.0;
    const std::size_t pixel_count = rgba.size() / 4u;
    constexpr int clear_r = 10;
    constexpr int clear_g = 15;
    constexpr int clear_b = 20;
    for (std::size_t i = 0; i < pixel_count; ++i) {
        const int r = rgba[i * 4u];
        const int g = rgba[i * 4u + 1u];
        const int b = rgba[i * 4u + 2u];
        const int luminance = static_cast<int>(std::lround(0.2126 * r + 0.7152 * g + 0.0722 * b));
        metrics.min_luminance = std::min(metrics.min_luminance, luminance);
        metrics.max_luminance = std::max(metrics.max_luminance, luminance);
        sum += static_cast<double>(luminance);
        if (r + g + b > 8) {
            ++metrics.nonblack_pixels;
        }
        if (std::abs(r - clear_r) + std::abs(g - clear_g) + std::abs(b - clear_b) > 24) {
            ++metrics.foreground_pixels;
        }
        if (luminance <= 2) {
            ++metrics.clipped_dark_pixels;
        }
        if (luminance >= 253) {
            ++metrics.clipped_bright_pixels;
        }
    }
    if (pixel_count > 0u) {
        metrics.mean_luminance = sum / static_cast<double>(pixel_count);
    }
    return metrics;
}

void WritePpm(const fs::path& path, int width, int height, const std::vector<unsigned char>& rgba) {
    std::ofstream output(path, std::ios::binary);
    ASSERT_TRUE(output) << path.string();
    output << "P6\n" << width << " " << height << "\n255\n";
    for (int y = height - 1; y >= 0; --y) {
        for (int x = 0; x < width; ++x) {
            const std::size_t pixel = (static_cast<std::size_t>(y) * width + x) * 4u;
            const unsigned char rgb[3] = {rgba[pixel], rgba[pixel + 1u], rgba[pixel + 2u]};
            output.write(reinterpret_cast<const char*>(rgb), 3);
        }
    }
}

void WriteMetricsJson(const fs::path& path,
                      const std::vector<std::pair<std::string, ImageMetrics>>& metrics) {
    std::ofstream output(path);
    ASSERT_TRUE(output) << path.string();
    output << "{\n";
    output << "  \"schema\": \"luminumbra.render_captures.v1\",\n";
    output << "  \"capture_ready_marker\": "
              "\"luminumbra.capture.ready:deterministic_render_capture:RenderDoc\",\n";
    output << "  \"captures\": [\n";
    for (std::size_t i = 0; i < metrics.size(); ++i) {
        const auto& [name, image] = metrics[i];
        output << "    {";
        output << "\"name\": \"" << name << "\", ";
        output << "\"width\": " << kCaptureWidth << ", ";
        output << "\"height\": " << kCaptureHeight << ", ";
        output << "\"nonblack_pixels\": " << image.nonblack_pixels << ", ";
        output << "\"foreground_pixels\": " << image.foreground_pixels << ", ";
        output << "\"clipped_dark_pixels\": " << image.clipped_dark_pixels << ", ";
        output << "\"clipped_bright_pixels\": " << image.clipped_bright_pixels << ", ";
        output << "\"min_luminance\": " << image.min_luminance << ", ";
        output << "\"max_luminance\": " << image.max_luminance << ", ";
        output << "\"mean_luminance\": " << image.mean_luminance << "}";
        output << (i + 1u == metrics.size() ? "\n" : ",\n");
    }
    output << "  ]\n}\n";
}

std::vector<unsigned char> RenderScene(GLuint program, const CaptureScene& scene) {
    GLuint color_texture = 0;
    GLuint depth_renderbuffer = 0;
    GLuint fbo = 0;
    glGenTextures(1, &color_texture);
    glBindTexture(GL_TEXTURE_2D, color_texture);
    glTexImage2D(GL_TEXTURE_2D,
                 0,
                 GL_RGBA8,
                 kCaptureWidth,
                 kCaptureHeight,
                 0,
                 GL_RGBA,
                 GL_UNSIGNED_BYTE,
                 nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);

    glGenRenderbuffers(1, &depth_renderbuffer);
    glBindRenderbuffer(GL_RENDERBUFFER, depth_renderbuffer);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, kCaptureWidth, kCaptureHeight);

    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, color_texture, 0);
    glFramebufferRenderbuffer(
        GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, depth_renderbuffer);
    EXPECT_EQ(glCheckFramebufferStatus(GL_FRAMEBUFFER), GL_FRAMEBUFFER_COMPLETE);

    GLuint vao = 0;
    GLuint vbo = 0;
    GLuint ebo = 0;
    glGenVertexArrays(1, &vao);
    glGenBuffers(1, &vbo);
    glGenBuffers(1, &ebo);
    glBindVertexArray(vao);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER,
                 static_cast<GLsizeiptr>(scene.vertices.size() * sizeof(VoxelVertex)),
                 scene.vertices.data(),
                 GL_STATIC_DRAW);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ebo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER,
                 static_cast<GLsizeiptr>(scene.indices.size() * sizeof(u32)),
                 scene.indices.data(),
                 GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0,
                          3,
                          GL_FLOAT,
                          GL_FALSE,
                          sizeof(VoxelVertex),
                          reinterpret_cast<void*>(offsetof(VoxelVertex, position)));
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1,
                          3,
                          GL_FLOAT,
                          GL_FALSE,
                          sizeof(VoxelVertex),
                          reinterpret_cast<void*>(offsetof(VoxelVertex, normal)));

    glViewport(0, 0, kCaptureWidth, kCaptureHeight);
    glEnable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glClearColor(0.04f, 0.06f, 0.08f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    const glm::mat4 model(1.0f);
    const glm::mat4 view =
        glm::lookAt(scene.camera_position, scene.camera_target, glm::vec3{0.0f, 1.0f, 0.0f});
    const glm::mat4 projection = glm::perspective(
        glm::radians(45.0f), static_cast<float>(kCaptureWidth) / kCaptureHeight, 0.1f, 128.0f);
    const glm::mat3 normal_matrix(1.0f);

    glUseProgram(program);
    SetMat4(program, "model", model);
    SetMat4(program, "view", view);
    SetMat4(program, "projection", projection);
    SetMat3(program, "normalMatrix", normal_matrix);
    glUniform3f(glGetUniformLocation(program, "lightPos"), 28.0f, 36.0f, 28.0f);
    glUniform3f(glGetUniformLocation(program, "viewPos"),
                scene.camera_position.x,
                scene.camera_position.y,
                scene.camera_position.z);
    glUniform3f(glGetUniformLocation(program, "lightColor"), 1.0f, 1.0f, 1.0f);
    glUniform3f(glGetUniformLocation(program, "objectColor"),
                scene.object_color.x,
                scene.object_color.y,
                scene.object_color.z);
    glDrawElements(
        GL_TRIANGLES, static_cast<GLsizei>(scene.indices.size()), GL_UNSIGNED_INT, nullptr);

    std::vector<unsigned char> pixels(static_cast<std::size_t>(kCaptureWidth) *
                                      static_cast<std::size_t>(kCaptureHeight) * 4u);
    glReadPixels(0, 0, kCaptureWidth, kCaptureHeight, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());

    glDeleteBuffers(1, &ebo);
    glDeleteBuffers(1, &vbo);
    glDeleteVertexArrays(1, &vao);
    glDeleteFramebuffers(1, &fbo);
    glDeleteRenderbuffers(1, &depth_renderbuffer);
    glDeleteTextures(1, &color_texture);
    return pixels;
}

// ---  rendering: exposure->luminance pixel-pair harness ---
// A fullscreen-triangle shader that applies the EXACT exposure + ACES filmic + gamma
// chain lighting_pass.frag uses (res/shaders/lighting_pass.frag:665-696), with the
// grade controls at identity so ONLY u_exposure varies. This lets the gate prove, on
// real GPU pixels, that a larger exposure multiplier yields a strictly brighter frame
// (the shipping lighting_pass.frag itself is goldened by the visual sweeps).
constexpr const char* kExposureVert = R"GLSL(
#version 450 core
out vec2 TexCoords;
void main() {
    vec2 p = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2));
    TexCoords = p;
    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}
)GLSL";

constexpr const char* kExposureFrag = R"GLSL(
#version 450 core
in vec2 TexCoords;
out vec4 FragColor;
uniform vec3 u_inColor;    // linear HDR input (pre-exposure)
uniform float u_exposure;  // the RenderContext.exposure multiplier under test
void main() {
    vec3 color = u_inColor;
    color *= u_exposure;                                                       // 665
    color = color * (2.51*color + 0.03) / (color*(2.43*color + 0.59) + 0.14);  // ACES:669
    color = clamp(color, 0.0, 1.0);                                            // 681
    color = pow(color, vec3(1.0/2.2));                                         // gamma:684
    color = max(color, vec3(4.0/255.0));                                       // black floor:694
    FragColor = vec4(color, 1.0);
}
)GLSL";

GLuint CompileShaderSource(const char* source, GLenum type) {
    GLuint shader = glCreateShader(type);
    glShaderSource(shader, 1, &source, nullptr);
    glCompileShader(shader);
    GLint success = GL_FALSE;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &success);
    if (success != GL_TRUE) {
        ADD_FAILURE() << "inline shader failed to compile\n" << GetShaderInfoLog(shader);
        glDeleteShader(shader);
        return 0;
    }
    return shader;
}

GLuint LinkInlineProgram(const char* vs_src, const char* fs_src) {
    GLuint vs = CompileShaderSource(vs_src, GL_VERTEX_SHADER);
    GLuint fs = CompileShaderSource(fs_src, GL_FRAGMENT_SHADER);
    if (vs == 0 || fs == 0) {
        if (vs)
            glDeleteShader(vs);
        if (fs)
            glDeleteShader(fs);
        return 0;
    }
    GLuint program = glCreateProgram();
    glAttachShader(program, vs);
    glAttachShader(program, fs);
    glLinkProgram(program);
    glDeleteShader(vs);
    glDeleteShader(fs);
    GLint success = GL_FALSE;
    glGetProgramiv(program, GL_LINK_STATUS, &success);
    if (success != GL_TRUE) {
        ADD_FAILURE() << "inline program failed to link\n" << GetProgramInfoLog(program);
        glDeleteProgram(program);
        return 0;
    }
    return program;
}

// Render a full frame of the fixed input color at one exposure and read it back.
std::vector<unsigned char>
RenderFullscreenExposure(GLuint program, const glm::vec3& in_color, float exposure) {
    GLuint color_texture = 0, depth_rb = 0, fbo = 0, vao = 0;
    glGenTextures(1, &color_texture);
    glBindTexture(GL_TEXTURE_2D, color_texture);
    glTexImage2D(GL_TEXTURE_2D,
                 0,
                 GL_RGBA8,
                 kCaptureWidth,
                 kCaptureHeight,
                 0,
                 GL_RGBA,
                 GL_UNSIGNED_BYTE,
                 nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glGenRenderbuffers(1, &depth_rb);
    glBindRenderbuffer(GL_RENDERBUFFER, depth_rb);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, kCaptureWidth, kCaptureHeight);
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, color_texture, 0);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, depth_rb);
    EXPECT_EQ(glCheckFramebufferStatus(GL_FRAMEBUFFER), GL_FRAMEBUFFER_COMPLETE);

    glGenVertexArrays(1, &vao); // core profile requires a bound VAO for attribute-less draw
    glBindVertexArray(vao);
    glViewport(0, 0, kCaptureWidth, kCaptureHeight);
    glDisable(GL_DEPTH_TEST);
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glUseProgram(program);
    glUniform3f(glGetUniformLocation(program, "u_inColor"), in_color.x, in_color.y, in_color.z);
    glUniform1f(glGetUniformLocation(program, "u_exposure"), exposure);
    glDrawArrays(GL_TRIANGLES, 0, 3);

    std::vector<unsigned char> pixels(static_cast<std::size_t>(kCaptureWidth) *
                                      static_cast<std::size_t>(kCaptureHeight) * 4u);
    glReadPixels(0, 0, kCaptureWidth, kCaptureHeight, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());

    glDeleteVertexArrays(1, &vao);
    glDeleteFramebuffers(1, &fbo);
    glDeleteRenderbuffers(1, &depth_rb);
    glDeleteTextures(1, &color_texture);
    return pixels;
}

} // namespace

TEST(RenderCaptureTest, DeterministicMeshScenesProduceStableImages) {
    HiddenGlContext context;
    if (!context.ready()) {
        GTEST_SKIP() << context.error();
    }

    fs::create_directories(ArtifactRoot());
    GLuint program = LinkBasicProgram();
    ASSERT_NE(program, 0u);

    std::vector<CaptureScene> scenes;
    scenes.push_back(BuildTerrainScene());
    scenes.push_back(BuildWaterScene());
    scenes.push_back(BuildSkyScene());
    scenes.push_back(BuildUiOverlayScene());
    scenes.push_back(BuildLoadingVisualizerScene());

    std::vector<std::pair<std::string, ImageMetrics>> all_metrics;
    for (const CaptureScene& scene : scenes) {
        ASSERT_FALSE(scene.vertices.empty()) << scene.name;
        ASSERT_FALSE(scene.indices.empty()) << scene.name;
        ASSERT_EQ(scene.indices.size() % 3u, 0u) << scene.name;

        const std::vector<unsigned char> pixels = RenderScene(program, scene);
        const ImageMetrics metrics = CalculateImageMetrics(pixels);
        WritePpm(ArtifactRoot() / (scene.name + ".ppm"), kCaptureWidth, kCaptureHeight, pixels);
        all_metrics.push_back({scene.name, metrics});

        EXPECT_GT(metrics.foreground_pixels,
                  static_cast<std::size_t>(kCaptureWidth * kCaptureHeight / 80))
            << scene.name;
        EXPECT_GT(metrics.max_luminance, 20) << scene.name;
        EXPECT_LT(metrics.clipped_bright_pixels,
                  static_cast<std::size_t>(kCaptureWidth * kCaptureHeight / 10))
            << scene.name;
    }

    WriteMetricsJson(ArtifactRoot() / "render_captures.json", all_metrics);
    glDeleteProgram(program);
}

//  ( ; contract ): the capture hooks were marker-only
// (capture_started always false). They now load the RenderDoc in-app API at
// runtime and drive a real StartFrameCapture/EndFrameCapture bracket. RenderDoc is
// not installed on the gate box, so instead of skipping the "SDK present" clause
// (as 's Diligent leg had to), these tests inject a RENDERDOC_API double
// through the production seam and run the EXACT production Begin/End code path
// against it -- capture_started=true and a real.rdc file both get asserted with
// zero skips. The untested remainder is only the real-DLL discovery success
// branch (needs a live RenderDoc), which is documented, not skipped.
namespace {

struct FakeRenderDocState {
    int start_calls = 0;
    int end_calls = 0;
    std::string path_template;
    std::string last_capture_path;
    std::uint32_t num_captures = 0;
};

FakeRenderDocState& FakeRenderDoc() {
    static FakeRenderDocState state;
    return state;
}

void RENDERDOC_CC FakeSetCaptureFilePathTemplate(const char* pathtemplate) {
    FakeRenderDoc().path_template = (pathtemplate != nullptr) ? pathtemplate : "";
}

void RENDERDOC_CC FakeStartFrameCapture(RENDERDOC_DevicePointer, RENDERDOC_WindowHandle) {
    ++FakeRenderDoc().start_calls;
}

std::uint32_t RENDERDOC_CC FakeEndFrameCapture(RENDERDOC_DevicePointer, RENDERDOC_WindowHandle) {
    ++FakeRenderDoc().end_calls;
    std::string path = FakeRenderDoc().path_template.empty() ? std::string("capture")
                                                             : FakeRenderDoc().path_template;
    path += "_frame0.rdc";
    // Write an explicit test-double artifact so file discovery and lifecycle handling run
    // against the filesystem without representing it as a real RenderDoc capture.
    std::error_code ec;
    fs::create_directories(fs::path(path).parent_path(), ec);
    std::ofstream out(path, std::ios::binary);
    out << "LUMINUMBRA_RENDERDOC_TEST_DOUBLE";
    out.close();
    FakeRenderDoc().last_capture_path = path;
    ++FakeRenderDoc().num_captures;
    return 1u;
}

std::uint32_t RENDERDOC_CC FakeGetNumCaptures() {
    return FakeRenderDoc().num_captures;
}

std::uint32_t RENDERDOC_CC FakeGetCapture(std::uint32_t idx,
                                          char* filename,
                                          std::uint32_t* pathlength,
                                          std::uint64_t* timestamp) {
    if (idx >= FakeRenderDoc().num_captures) {
        return 0u;
    }
    const std::string& p = FakeRenderDoc().last_capture_path;
    if (pathlength != nullptr) {
        *pathlength =
            static_cast<std::uint32_t>(p.size() + 1u); // include the null, RenderDoc's convention
    }
    if (filename != nullptr) {
        std::memcpy(filename, p.c_str(), p.size() + 1u);
    }
    if (timestamp != nullptr) {
        *timestamp = 0u;
    }
    return 1u;
}

RENDERDOC_API_1_6_0 MakeFakeRenderDocApi() {
    RENDERDOC_API_1_6_0 api;
    std::memset(&api, 0, sizeof(api)); // trivial C struct of function pointers
    api.SetCaptureFilePathTemplate = &FakeSetCaptureFilePathTemplate;
    api.StartFrameCapture = &FakeStartFrameCapture;
    api.EndFrameCapture = &FakeEndFrameCapture;
    api.GetNumCaptures = &FakeGetNumCaptures;
    api.GetCapture = &FakeGetCapture;
    return api;
}

} // namespace

TEST(RenderCaptureSdkTrigger, LiveCaptureViaInjectedApi) {
    namespace R = Luminumbra::Rendering;
    FakeRenderDoc() = FakeRenderDocState{};
    RENDERDOC_API_1_6_0 fake = MakeFakeRenderDocApi();
    R::detail::SetRenderDocApiForTesting(&fake);

    const bool available = R::IsCaptureSdkAvailable(R::CaptureBackend::RenderDoc);

    R::CaptureRequest request;
    request.scenario = "sdk trigger live";
    request.preferred_backend = R::CaptureBackend::RenderDoc;

    const fs::path capture_dir = ArtifactRoot() / "sdk_trigger";
    R::FrameCaptureSession session = R::BeginFrameCapture(request, capture_dir.string());
    const bool began_active = session.active;
    const std::string began_backend = session.backend;
    R::FrameCaptureResult result = R::EndFrameCapture(session);

    // Reset the injection while `fake` is still alive (it is a stack local); every
    // assertion below runs against captured values, so no early return can leave a
    // dangling injected pointer.
    R::detail::SetRenderDocApiForTesting(nullptr);

    EXPECT_TRUE(available) << "injected API should report the SDK available";
    EXPECT_TRUE(began_active);
    EXPECT_EQ(began_backend, "RenderDoc");
    EXPECT_TRUE(result.capture_started);
    EXPECT_EQ(result.backend, "RenderDoc");
    EXPECT_FALSE(result.capture_file.empty());
    EXPECT_TRUE(fs::exists(result.capture_file)) << result.capture_file;
    EXPECT_EQ(FakeRenderDoc().start_calls, 1);
    EXPECT_EQ(FakeRenderDoc().end_calls, 1);
    EXPECT_NE(FakeRenderDoc().path_template.find("sdk_trigger_live"), std::string::npos)
        << FakeRenderDoc().path_template;
}

TEST(RenderCaptureSdkTrigger, MarkerOnlyFallbackWhenNoSdk) {
    namespace R = Luminumbra::Rendering;
    R::detail::SetRenderDocApiForTesting(nullptr);
    // With no injection this exercises the REAL load path, which returns null
    // headless (renderdoc.dll not injected into this process) -- an honest false,
    // not a skip.
    EXPECT_FALSE(R::IsCaptureSdkAvailable(R::CaptureBackend::RenderDoc));

    R::CaptureRequest request;
    request.scenario = "headless fallback";
    R::FrameCaptureSession session = R::BeginFrameCapture(request, ArtifactRoot().string());
    EXPECT_FALSE(session.active);
    EXPECT_NE(session.marker.find("luminumbra.capture.ready:headless_fallback:RenderDoc"),
              std::string::npos)
        << session.marker;

    R::FrameCaptureResult result = R::EndFrameCapture(session);
    EXPECT_FALSE(result.capture_started);
    EXPECT_TRUE(result.capture_file.empty());
    EXPECT_NE(result.diagnostic.find("not linked"), std::string::npos) << result.diagnostic;
}

TEST(RenderCaptureSdkTrigger, UnsupportedBackendsReportNotLinked) {
    namespace R = Luminumbra::Rendering;
    R::detail::SetRenderDocApiForTesting(nullptr);
    EXPECT_FALSE(R::IsCaptureSdkAvailable(R::CaptureBackend::PIX));
    EXPECT_FALSE(R::IsCaptureSdkAvailable(R::CaptureBackend::Nsight));

    // The marker-only handshake is still emitted for any backend (headless-safe).
    R::CaptureRequest request;
    request.scenario = "nsight";
    request.preferred_backend = R::CaptureBackend::Nsight;
    R::CaptureResult marker = R::BuildCaptureReadyMarker(request);
    EXPECT_FALSE(marker.capture_started);
    EXPECT_EQ(marker.marker, "luminumbra.capture.ready:nsight:Nsight");
}

//  rendering ( /  ): the photo-mode MANUAL exposure model.
// These gates exercise the SHIPPING functions (ExposureModel.h) that both
// main_client.cpp (the push site) and RenderPipeline.cpp (the ctx assembly) call, so
// they are non-vacuous by construction.

TEST(ExposureModel, ManualMultiplierMapsLensEvAndPrecedenceSelects) {
    namespace R = Luminumbra::Rendering;
    using luminumbra::game::LensSettings;

    // Default lens -> EXACTLY the day/noon anchor: entering photo mode at the default
    // lens is continuous with the noon exposure (self-calibrated EV_ref).
    const LensSettings def{};
    const float base = R::ManualExposureMultiplier(def);
    EXPECT_NEAR(base, R::kManualExposureM0, 1e-4f);

    // Stopping DOWN (higher f-number -> higher EV) DARKENS; opening UP BRIGHTENS.
    LensSettings stopped = def;
    stopped.aperture_f = 5.6f; // +2 stops from f/2.8
    LensSettings opened = def;
    opened.aperture_f = 1.4f; // -2 stops
    EXPECT_LT(R::ManualExposureMultiplier(stopped), base);
    EXPECT_GT(R::ManualExposureMultiplier(opened), base);

    // Faster shutter DARKENS; slower BRIGHTENS.
    LensSettings fast = def;
    fast.shutter_s = def.shutter_s * 0.25f; // 2 stops faster
    LensSettings slow = def;
    slow.shutter_s = def.shutter_s * 4.0f; // 2 stops slower
    EXPECT_LT(R::ManualExposureMultiplier(fast), base);
    EXPECT_GT(R::ManualExposureMultiplier(slow), base);

    // Higher ISO (more sensitive -> lower required EV) BRIGHTENS; lower DARKENS.
    LensSettings hi_iso = def;
    hi_iso.iso = 400.0f; // +2 stops
    LensSettings lo_iso = def;
    lo_iso.iso = 50.0f; // -1 stop
    EXPECT_GT(R::ManualExposureMultiplier(hi_iso), base);
    EXPECT_LT(R::ManualExposureMultiplier(lo_iso), base);

    // The extremes CLAMP to the usable band (never pure black / pure white).
    LensSettings darkest = def;
    darkest.aperture_f = 32.0f;
    darkest.shutter_s = 1.0f / 4000.0f;
    darkest.iso = 50.0f;
    LensSettings brightest = def;
    brightest.aperture_f = 1.0f;
    brightest.shutter_s = 30.0f;
    brightest.iso = 25600.0f;
    EXPECT_FLOAT_EQ(R::ManualExposureMultiplier(darkest), R::kManualExposureMin);
    EXPECT_FLOAT_EQ(R::ManualExposureMultiplier(brightest), R::kManualExposureMax);

    // Precedence (the exact rule at RenderPipeline.cpp's ctx.exposure assignment): a
    // positive manual override wins; the -1 sentinel (photo mode inactive) falls back.
    EXPECT_FLOAT_EQ(R::SelectRenderExposure(0.3f, 1.5f), 0.3f);
    EXPECT_FLOAT_EQ(R::SelectRenderExposure(-1.0f, 1.5f), 1.5f);
    EXPECT_FLOAT_EQ(R::SelectRenderExposure(-1.0f, 1.02f), 1.02f);
    // Both branches are always > 0, so the lighting pass's `ctx.exposure > 0` sentinel
    // wire always fires (no accidental fall-through to the static LUMIN_GRADE exposure).
    EXPECT_GT(R::SelectRenderExposure(base, 1.02f), 0.0f);
}

// the AUTOMATIC time-of-day exposure curve extracted to
// ExposureModel.h (Rendering::AutoExposureForElevation). Byte-exact vs the canonical-primitive
// rebuild (glm::smoothstep/mix with the kManualExposureM0 day anchor), plus anchors: high sun ==
// the day anchor (photo-mode continuity), deep night lifted, the golden band dips below day.
// GPU-free.
TEST(ExposureModel, AutoExposureCurveMatchesPrimitivesAndAnchors) {
    namespace R = Luminumbra::Rendering;
    for (float up : {-1.0f, -0.2f, -0.05f, 0.0f, 0.1f, 0.2f, 0.22f, 0.3f, 0.5f, 0.9f, 1.0f}) {
        const float got = R::AutoExposureForElevation(up);
        const float day = glm::smoothstep(-0.05f, 0.30f, up);
        const float golden = day * (1.0f - glm::smoothstep(0.18f, 0.45f, up));
        float ref = glm::mix(1.75f, R::kManualExposureM0, day);
        ref = glm::mix(ref, 1.02f, golden);
        EXPECT_EQ(got, ref) << "up=" << up;
    }
    EXPECT_FLOAT_EQ(R::AutoExposureForElevation(1.0f),
                    R::kManualExposureM0); // high sun == day anchor
    EXPECT_GT(R::AutoExposureForElevation(-0.5f),
              R::AutoExposureForElevation(1.0f)); // night lifted above day
    EXPECT_LT(R::AutoExposureForElevation(0.22f),
              R::kManualExposureM0); // golden band dips below day
}

// the SEASON facet extracted from update_time_of_day
// (TimeOfDayModel::ComputeSeason) — the SAME function the frame runs. Byte-exact extraction
// guard: each output is asserted == the same expression rebuilt from the CANONICAL primitive
// (DeterministicMath::Sin) directly here — NOT a re-typed copy of the season arithmetic, so a
// wrong-primitive swap (DM::Sin -> std::sin) or a reassociation diverges — plus neutral-state
// neutrality, exact period wrap, and code-independent analytical solstice anchors. GPU-free.
TEST(TimeOfDayModel, SeasonIsPureTickFunctionOfCanonicalPrimitives) {
    namespace R = Luminumbra::Rendering;
    namespace DM = Luminumbra::DeterministicMath;
    constexpr std::uint64_t kCycle =
        432000ull; // == RenderPipeline::kTicksPerSeasonCycle (4 h @ 30 Hz)

    // neutral-state NEUTRALITY: tick 0 (the default every non-season scenario sees, because it
    // never calls set_season_tick) must be EXACTLY season-neutral, or the whole non-season path
    // drifts.
    const R::SeasonState s0 = R::ComputeSeason(0, kCycle);
    EXPECT_EQ(s0.phase, 0.0f);
    EXPECT_EQ(s0.wave, DM::Sin(0.0f)); // exactly the neutral primitive value (0)
    EXPECT_EQ(s0.sunDeclination, R::kSeasonalTiltAmplitude * s0.wave);

    // Dense sweep: every output bit-exact against the canonical primitive expression. DM::Sin
    // here is the library's ground-truth primitive (not a copy of the season math), so this is
    // not a closed loop — a trig swap, a changed cycle constant, or a reassociation all fail.
    for (std::uint64_t t : {0ull,
                            1ull,
                            108000ull,
                            216000ull,
                            324000ull,
                            431999ull,
                            432000ull,
                            540000ull,
                            999999ull,
                            12345678ull}) {
        const R::SeasonState s = R::ComputeSeason(t, kCycle);
        const std::uint64_t tick_in_year = t % kCycle;
        const float phase =
            static_cast<float>(static_cast<double>(tick_in_year) / static_cast<double>(kCycle));
        EXPECT_EQ(s.phase, phase) << "tick=" << t;
        EXPECT_EQ(s.wave, DM::Sin(phase * DM::kTwoPi)) << "tick=" << t;
        EXPECT_EQ(s.sunDeclination, R::kSeasonalTiltAmplitude * DM::Sin(phase * DM::kTwoPi))
            << "tick=" << t;
    }

    // WRAP: the period is exactly kCycle ticks — tick 0 == tick kCycle, byte for byte.
    const R::SeasonState w = R::ComputeSeason(kCycle, kCycle);
    EXPECT_EQ(w.phase, s0.phase);
    EXPECT_EQ(w.wave, s0.wave);
    EXPECT_EQ(w.sunDeclination, s0.sunDeclination);

    // ANALYTICAL anchors (independent of the implementation): the solstices sit at the quarter /
    // three-quarter year, reach the tilt amplitude, and carry the summer-positive / winter-
    // negative sign convention the sun-arc code depends on.
    const R::SeasonState summer = R::ComputeSeason(kCycle / 4, kCycle);       // .25
    const R::SeasonState winter = R::ComputeSeason((kCycle * 3) / 4, kCycle); // .75
    EXPECT_FLOAT_EQ(summer.phase, 0.25f);
    EXPECT_FLOAT_EQ(winter.phase, 0.75f);
    EXPECT_NEAR(summer.wave, 1.0f, 1e-3f);    // summer solstice ~ +1 (highest arc)
    EXPECT_NEAR(winter.wave, -1.0f, 1e-3f);   // winter solstice ~ -1 (lowest arc)
    EXPECT_GT(summer.sunDeclination, 0.40f);  // ~ +0.410 rad
    EXPECT_LT(winter.sunDeclination, -0.40f); // ~ -0.410 rad
}

// the SUN GEOMETRY facet (TimeOfDayModel::ComputeSunGeometry) —
// the SAME function the frame runs. Byte-exact extraction guard: every output == the same
// expression rebuilt from the canonical primitives here — UNQUALIFIED sin/cos exactly as the
// sun-direction site resolves them (this TU has <cmath> and no `using namespace std`, matching
// the pipeline TU's global::sin), glm::normalize/dot, std::asin, glm::smoothstep — so a std::
// qualification of the sun trig, a reassociation, or a changed band constant diverges. Plus
// code-independent noon / midnight / horizon / season anchors on the sign + saturation. GPU-free.
TEST(TimeOfDayModel, SunGeometryMatchesCanonicalPrimitivesAndAnchors) {
    namespace R = Luminumbra::Rendering;
    namespace DM = Luminumbra::DeterministicMath;

    for (float tod : {0.0f, 0.1f, 0.2f, 0.25f, 0.3f, 0.5f, 0.6f, 0.75f, 0.9f, 0.99f}) {
        for (float decl : {0.0f, 0.41015237f, -0.41015237f, 0.2f, -0.15f}) {
            const R::SunGeometry g = R::ComputeSunGeometry(tod, decl);
            const float angle = tod * 2.0f * glm::pi<float>();
            const float tiltZ = DM::Sin(decl) - 0.2f;
            // UNQUALIFIED sin/cos — global::sin, matching the sun-direction site (not a closed
            // loop: :sin is the library primitive, not a re-typed copy of the arithmetic).
            const glm::vec3 dir = glm::normalize(glm::vec3(sin(angle), -cos(angle), tiltZ));
            const float up = glm::dot(dir, glm::vec3(0.0f, -1.0f, 0.0f));
            EXPECT_EQ(g.angleRad, angle);
            EXPECT_EQ(g.tiltZ, tiltZ);
            EXPECT_EQ(g.direction.x, dir.x);
            EXPECT_EQ(g.direction.y, dir.y);
            EXPECT_EQ(g.direction.z, dir.z);
            EXPECT_EQ(g.upFactor, up);
            EXPECT_EQ(g.elevationRad, std::asin(glm::clamp(up, -1.0f, 1.0f)));
            EXPECT_EQ(g.sunIntensity, glm::smoothstep(-0.1f, 0.15f, up));
            EXPECT_EQ(g.skyDomeDayFactor, glm::smoothstep(-0.22f, 0.34f, up));
        }
    }

    // ANALYTICAL anchors: timeOfDay 0 is NOON (sun overhead, full day-factors); 0.5 is MIDNIGHT
    // (sun below, day-factors 0); the season-neutral horizon sits at timeOfDay 0.25 (up ~ 0).
    const R::SunGeometry noon = R::ComputeSunGeometry(0.0f, 0.0f);
    EXPECT_GT(noon.upFactor, 0.97f); // ~0.981 overhead
    EXPECT_FLOAT_EQ(noon.sunIntensity, 1.0f);
    EXPECT_FLOAT_EQ(noon.skyDomeDayFactor, 1.0f);
    const R::SunGeometry midnight = R::ComputeSunGeometry(0.5f, 0.0f);
    EXPECT_LT(midnight.upFactor, -0.97f);
    EXPECT_FLOAT_EQ(midnight.sunIntensity, 0.0f);
    EXPECT_FLOAT_EQ(midnight.skyDomeDayFactor, 0.0f);
    const R::SunGeometry horizon = R::ComputeSunGeometry(0.25f, 0.0f);
    EXPECT_NEAR(horizon.upFactor, 0.0f, 0.01f);
    // Season raises/lowers the noon arc: a summer declination lifts the noon sun above a winter
    // one.
    EXPECT_GT(R::ComputeSunGeometry(0.0f, 0.41015237f).upFactor,
              R::ComputeSunGeometry(0.0f, -0.41015237f).upFactor);
}

// the MOON + SEASON-PALETTE facets extracted from
// update_time_of_day. Byte-exact guards: each output == the same expression rebuilt from the
// canonical primitives here — season tint via the VERBATIM manual luma sum (not a dot); the
// moon via std::sin/std::cos (the FLOAT overload, distinct from the sun's::sin); the lunar cycle
// via std::cos — so a reassociation or a wrong trig/overload diverges. Plus code-independent
// anchors: winter-warm/summer-cool + luma preserved; moon overhead at midnight; full vs new-moon
// illumination; override precedence + clamp. GPU-free.
TEST(TimeOfDayModel, MoonAndSeasonPaletteMatchCanonicalPrimitivesAndAnchors) {
    namespace R = Luminumbra::Rendering;
    namespace DM = Luminumbra::DeterministicMath;
    auto luma = [](const glm::vec3& c) {
        return c.r * 0.2126f + c.g * 0.7152f + c.b * 0.0722f;
    };
    constexpr std::uint64_t kLunar =
        54000ull; // == RenderPipeline::kTicksPerLunarCycle (30 min @ 30 Hz)

    // SEASON PALETTE tint: bit-exact vs the verbatim construction + manual luma normalize.
    for (float wave : {-1.0f, -0.5f, 0.0f, 0.3f, 1.0f}) {
        const glm::vec3 t = R::SeasonPaletteTint(wave);
        constexpr float k = 0.06f;
        glm::vec3 ref(1.0f - k * wave, 1.0f, 1.0f + k * wave);
        const float lum = ref.r * 0.2126f + ref.g * 0.7152f + ref.b * 0.0722f;
        if (lum > 1e-6f)
            ref /= lum;
        EXPECT_EQ(t.r, ref.r) << "wave=" << wave;
        EXPECT_EQ(t.g, ref.g) << "wave=" << wave;
        EXPECT_EQ(t.b, ref.b) << "wave=" << wave;
        EXPECT_NEAR(luma(t), 1.0f, 1e-5f); // luminance preserved -> ~1 after normalization
    }
    EXPECT_GT(R::SeasonPaletteTint(-1.0f).r, R::SeasonPaletteTint(-1.0f).b); // winter warm (R>B)
    EXPECT_LT(R::SeasonPaletteTint(1.0f).r, R::SeasonPaletteTint(1.0f).b);   // summer cool (B>R)
    EXPECT_FLOAT_EQ(R::SeasonPaletteTint(0.0f).r, R::SeasonPaletteTint(0.0f).b); // neutral at

    // MOON GEOMETRY: bit-exact vs the std::sin/std::cos (FLOAT overload) rebuild.
    for (float tod : {0.0f, 0.25f, 0.5f, 0.75f}) {
        for (float decl : {0.0f, 0.41015237f, -0.41015237f}) {
            const R::SunGeometry sg = R::ComputeSunGeometry(tod, decl);
            const R::MoonGeometry mg = R::ComputeMoonGeometry(sg.angleRad, sg.tiltZ, sg.direction);
            const glm::vec3 ldir =
                glm::normalize(glm::vec3(-std::sin(sg.angleRad), std::cos(sg.angleRad), sg.tiltZ));
            EXPECT_EQ(mg.direction.x, -sg.direction.x);
            EXPECT_EQ(mg.direction.y, -sg.direction.y);
            EXPECT_EQ(mg.direction.z, -sg.direction.z);
            EXPECT_EQ(mg.lightDir.x, ldir.x);
            EXPECT_EQ(mg.lightDir.y, ldir.y);
            EXPECT_EQ(mg.lightDir.z, ldir.z);
            EXPECT_EQ(mg.upFactor, glm::dot(ldir, glm::vec3(0.0f, -1.0f, 0.0f)));
        }
    }
    { // the moon is OVERHEAD at midnight (sun below the horizon) — high moon up-factor at tod 0.5.
        const R::SunGeometry sg = R::ComputeSunGeometry(0.5f, 0.0f);
        const R::MoonGeometry mg = R::ComputeMoonGeometry(sg.angleRad, sg.tiltZ, sg.direction);
        EXPECT_GT(mg.upFactor, 0.97f);
    }

    // LUNAR illumination: bit-exact vs the std::cos rebuild; full at tick 0, floor at half-cycle.
    for (std::uint64_t t :
         {0ull, 1ull, 13500ull, 27000ull, 40500ull, 53999ull, 54000ull, 123456ull}) {
        const float got = R::LunarIllumination(t, kLunar);
        const std::uint64_t til = t % kLunar;
        const float lt = static_cast<float>(static_cast<double>(til) / static_cast<double>(kLunar));
        const float full = 0.5f + 0.5f * std::cos(lt * 2.0f * glm::pi<float>());
        EXPECT_EQ(got, glm::mix(R::kNewMoonFloor, 1.0f, full)) << "tick=" << t;
    }
    EXPECT_FLOAT_EQ(R::LunarIllumination(0, kLunar), 1.0f); // tick 0 = full moon
    EXPECT_NEAR(
        R::LunarIllumination(kLunar / 2, kLunar), R::kNewMoonFloor, 1e-5f); // half = new moon
    // Override precedence + clamp (ComputeMoonIllumination): a forced >= 0 wins, clamped to [0,1].
    EXPECT_FLOAT_EQ(R::ComputeMoonIllumination(0, 0.42f, kLunar), 0.42f);
    EXPECT_FLOAT_EQ(R::ComputeMoonIllumination(0, 5.0f, kLunar), 1.0f); // clamped high
    EXPECT_FLOAT_EQ(R::ComputeMoonIllumination(0, -1.0f, kLunar),
                    R::LunarIllumination(0, kLunar)); // <0 falls back to cycle
}

//  rendering: the direct-sun magnitude is derived from the atmosphere
// transmittance (SunLightModel::SunIrradiance) — the SAME function RenderPipeline uses to
// set m_sun.color. This pins the contract: overhead sun preserved, low sun dims AND
// reddens from that one physical term, below-horizon goes dark. GPU-free (pure logic).
TEST(SunLightModel, NoonPinnedLowSunDimsAndReddensHorizonCutoff) {
    namespace R = Luminumbra::Rendering;
    auto luma = [](const glm::vec3& c) {
        return 0.2126f * c.r + 0.7152f * c.g + 0.0722f * c.b;
    };

    // A physically-plausible transmittance toward the sun as a function of elevation
    // (cos of the zenith-ish angle == sun_up_factor). Overhead: short path, near-white and
    // relatively blue-rich. Low sun: long path, heavily red-shifted (blue eaten first).
    // Modeled as Beer-Lambert with an air-mass ~ 1/max(cos, eps) and per-channel optical
    // depth increasing toward blue — the qualitative shape the real Rayleigh LUT produces.
    auto transmittance = [](float cos_up) {
        const float mu = std::max(cos_up, 0.02f);
        const float air_mass = 1.0f / mu;         // grows sharply as the sun lowers
        const glm::vec3 tau(0.12f, 0.20f, 0.35f); // R < G < B optical depth (blue eaten first)
        return glm::vec3(
            std::exp(-tau.r * air_mass), std::exp(-tau.g * air_mass), std::exp(-tau.b * air_mass));
    };
    const glm::vec3 t_ref = transmittance(1.0f); // overhead reference

    // Overhead sun (noon): irradiance is pinned to unit white by the 1/t_ref solar
    // constant, so m_sun.color == SunBaseHue there (the noon image is preserved).
    const glm::vec3 noon = R::SunIrradiance(transmittance(1.0f), t_ref, 1.0f);
    EXPECT_NEAR(noon.r, 1.0f, 1e-4f);
    EXPECT_NEAR(noon.g, 1.0f, 1e-4f);
    EXPECT_NEAR(noon.b, 1.0f, 1e-4f);

    // Sweeping the sun DOWN from overhead to the horizon: luminance is monotonically
    // NON-INCREASING (the sun dims), and the red/blue ratio strictly RISES (it reddens).
    float prev_luma = luma(noon) + 1e-3f;
    float prev_rb = noon.r / std::max(noon.b, 1e-6f);
    for (float cos_up = 0.95f; cos_up >= 0.06f; cos_up -= 0.05f) {
        const glm::vec3 irr = R::SunIrradiance(transmittance(cos_up), t_ref, cos_up);
        EXPECT_LE(luma(irr), prev_luma + 1e-4f)
            << "sun should not brighten as it lowers, cos=" << cos_up;
        const float rb = irr.r / std::max(irr.b, 1e-6f);
        EXPECT_GT(rb, prev_rb - 1e-4f) << "sun should redden as it lowers, cos=" << cos_up;
        prev_luma = luma(irr);
        prev_rb = rb;
    }

    // A genuinely low sun is both dimmer AND redder than noon (: golden hour
    // reddens AND dims from ONE model).
    const glm::vec3 golden = R::SunIrradiance(transmittance(0.08f), t_ref, 0.08f);
    EXPECT_LT(luma(golden), luma(noon));
    EXPECT_GT(golden.r / std::max(golden.b, 1e-6f), noon.r / std::max(noon.b, 1e-6f));

    // Below the horizon the physical disc is gone (the moon path lights the night).
    const glm::vec3 below = R::SunIrradiance(transmittance(-0.2f), t_ref, -0.2f);
    EXPECT_NEAR(luma(below), 0.0f, 1e-5f);
}

TEST(ExposureModel, ExposureScalesLuminanceMonotonicOnGpu) {
    HiddenGlContext context;
    if (!context.ready()) {
        GTEST_SKIP() << context.error();
    }
    namespace R = Luminumbra::Rendering;

    GLuint program = LinkInlineProgram(kExposureVert, kExposureFrag);
    ASSERT_NE(program, 0u);

    // A fixed mid-grey linear HDR input; three exposures chosen to stay below ACES
    // saturation so the tonemapped luminance rises monotonically with exposure.
    const glm::vec3 in_color{0.18f, 0.18f, 0.18f};
    const float exposures[3] = {
        R::kManualExposureMin * 3.0f, // ~0.3, a dark exposure
        R::kManualExposureM0,         // 1.12, the noon anchor
        3.0f,                         // bright but sub-saturation
    };
    double luma[3];
    for (int i = 0; i < 3; ++i) {
        luma[i] = CalculateImageMetrics(RenderFullscreenExposure(program, in_color, exposures[i]))
                      .mean_luminance;
    }
    // The genuine two(+)-exposure capture pair: a larger exposure -> a strictly brighter
    // frame on real GPU pixels.
    EXPECT_LT(luma[0], luma[1]) << luma[0] << " !< " << luma[1];
    EXPECT_LT(luma[1], luma[2]) << luma[1] << " !< " << luma[2];

    // And the value a real photo would push: a stopped-down lens (f/8) reads strictly
    // DARKER than the noon anchor -- the effect the feature promises, end to end from
    // LensSettings through the shipping mapping to captured luminance.
    using luminumbra::game::LensSettings;
    LensSettings stopped{};
    stopped.aperture_f = 8.0f;
    const float stop_mult = R::ManualExposureMultiplier(stopped);
    const double stop_luma =
        CalculateImageMetrics(RenderFullscreenExposure(program, in_color, stop_mult))
            .mean_luminance;
    const double noon_luma =
        CalculateImageMetrics(RenderFullscreenExposure(program, in_color, R::kManualExposureM0))
            .mean_luminance;
    EXPECT_LT(stop_luma, noon_luma) << "stopping down (" << stop_mult << ") should darken vs noon";

    glDeleteProgram(program);
}

// ===========================================================================
//   ( , ): the DECLARATIVE FRAME GRAPH.
//
// These are pure-CPU tests of the ordering/validation logic (no GL context). The
// separate DRIFT GUARD -- that the graph's schedule equals render_frame's ACTUAL
// emitted stage trace, so the declaration can never silently diverge from the
// shipping order -- lives in the render-health snapshot check and the
// FrameStageTraceMatchesDeclaredGraph capture test below; here we prove the graph
// is internally sound and that the one dynamic dependency the advisor flagged (the
// god-rays "latest opaque snapshot") resolves correctly under BOTH branches.
// ===========================================================================

// The canonical frame graph topo-sorts to its authored linearization (deps are
// consistent + acyclic -- no node dropped) and passes internal validation.
TEST(RenderGraph, CanonicalGraphSchedulesToAuthoredOrderAndValidatesClean) {
    namespace R = Luminumbra::Rendering;
    const R::RenderGraph g = R::BuildLuminumbraFrameGraph();

    // The authored order is derived FROM the graph (not a re-typed list): the
    // schedule must reproduce it, proving the declared resource dependencies form a
    // DAG whose only stable linearization is the one render_frame dispatches.
    std::vector<std::string> authored;
    for (const R::RenderGraphNode& node : g.nodes())
        authored.push_back(node.name);

    const std::vector<std::string> scheduled = g.schedule();
    EXPECT_EQ(scheduled.size(), g.size()) << "topo sort dropped a node -> a dependency cycle";
    EXPECT_EQ(scheduled, authored) << "schedule diverged from the authored linearization";

    const std::vector<std::string> violations = g.validate();
    EXPECT_TRUE(violations.empty()) << "frame graph is internally inconsistent; first: "
                                    << (violations.empty() ? std::string{} : violations.front());

    // Spot-check the load-bearing accumulation invariant: every stage that writes the
    // lit color target is ordered exactly as authored (sky -> water ->... -> the
    // final composite readers), so the blend order is machine-declared, not implicit.
    auto index_of = [&](const char* n) {
        return std::find(scheduled.begin(), scheduled.end(), n) - scheduled.begin();
    };
    EXPECT_LT(index_of("skybox"), index_of("water"));
    EXPECT_LT(index_of("opaque_snapshot"), index_of("water"));
    EXPECT_LT(index_of("lighting"), index_of("skybox"));
    EXPECT_LT(index_of("god_rays"), index_of("final_blit"));
    // the tint cascade flows shadow -> lighting.
    EXPECT_LT(index_of("shadow"), index_of("lighting"));
}

// the tinted-transmission model (GlassTintModel.h) is the
// ONE definition the shadow_tint shader mirrors (T(d) = tint^d, Beer-Lambert with
// the unit-thickness tint as the authored coefficient). Anchor its algebra.
TEST(ColoredShadow, BeerLambertTintModelAnchors) {
    namespace R = Luminumbra::Rendering;
    const glm::vec3 tint(0.9f, 0.4f, 0.1f);
    // d=1 returns the authored unit tint exactly.
    EXPECT_EQ(R::GlassTransmission(tint, 1.0f), tint);
    // d=0 is identity (no glass) — the init-cleared-white cascade convention.
    EXPECT_EQ(R::GlassTransmission(tint, 0.0f), glm::vec3(1.0f));
    // Two stacked unit panes == one 2x-thickness pane (the multiply-blend contract
    // the GL_DST_COLOR/GL_ZERO accumulation in the tint sub-pass relies on).
    const glm::vec3 stacked = R::GlassTransmission(tint, 1.0f) * R::GlassTransmission(tint, 1.0f);
    const glm::vec3 twice = R::GlassTransmission(tint, 2.0f);
    EXPECT_NEAR(stacked.r, twice.r, 1e-6f);
    EXPECT_NEAR(stacked.g, twice.g, 1e-6f);
    EXPECT_NEAR(stacked.b, twice.b, 1e-6f);
    // Out-of-range inputs clamp: negative thickness -> identity; tint -> [0,1].
    EXPECT_EQ(R::GlassTransmission(tint, -3.0f), glm::vec3(1.0f));
    EXPECT_EQ(R::GlassTransmission(glm::vec3(2.0f, -1.0f, 0.5f), 1.0f),
              glm::vec3(1.0f, 0.0f, 0.5f));
}

//  rendering: the froxel grid model — the exponential slice
// distribution's anchors, monotonicity, and the depth<->slice round trip. The
// froxel_inject/froxel_integrate GLSL kernels bake the SAME constants; these
// anchors pin the C++ half, FroxelUniformMediumMatchesAnalyticTransmittance
// (render_smoke_test) pins the GLSL half end to end.
TEST(FroxelModel, SliceDistributionAndWorldMappingAnchors) {
    namespace F = Luminumbra::Rendering::Froxel;
    // The GLSL mirrors bake exactly these values.
    EXPECT_EQ(F::kGridX, 160);
    EXPECT_EQ(F::kGridY, 90);
    EXPECT_EQ(F::kGridZ, 64);
    EXPECT_FLOAT_EQ(F::kNearDepth, 0.5f);
    EXPECT_FLOAT_EQ(F::kFarDepth, 160.0f);
    // Exponential boundaries hit both ends exactly.
    EXPECT_FLOAT_EQ(F::SliceBoundaryDepth(0), F::kNearDepth);
    EXPECT_NEAR(F::SliceBoundaryDepth(F::kGridZ), F::kFarDepth, 1e-3f);
    // Monotonic boundaries + the depth->slice round trip at every slice centre.
    for (int i = 0; i < F::kGridZ; ++i) {
        EXPECT_LT(F::SliceBoundaryDepth(i), F::SliceBoundaryDepth(i + 1));
        const float mid = 0.5f * (F::SliceBoundaryDepth(i) + F::SliceBoundaryDepth(i + 1));
        EXPECT_EQ(F::DepthToSlice(mid), i) << "slice " << i;
    }
    // The composite's texture-W mapping clamps and spans [0,1].
    EXPECT_FLOAT_EQ(F::DepthToTextureW(0.0f), 0.0f);
    EXPECT_FLOAT_EQ(F::DepthToTextureW(F::kFarDepth * 2.0f), 1.0f);
    EXPECT_GT(F::DepthToTextureW(10.0f), F::DepthToTextureW(5.0f));
}

// the WBOIT model — the depth weight's shape and the
// composite algebra. Order independence is BY CONSTRUCTION (the accumulation is
// a commutative sum), pinned here algebraically; the glass_oit shaders mirror
// DepthWeight exactly.
TEST(OitModel, WeightFunctionMonotoneAndCompositeAlgebra) {
    namespace O = Luminumbra::Rendering::Oit;
    // Near surfaces outweigh far ones, monotonically.
    float prev = O::DepthWeight(0.5f, 1.0f);
    for (float z : {2.0f, 8.0f, 30.0f, 90.0f, 300.0f}) {
        const float w = O::DepthWeight(z, 1.0f);
        EXPECT_LT(w, prev) << "depth weight must fall with distance (z=" << z << ")";
        prev = w;
    }
    // Weight scales linearly with alpha; zero alpha contributes nothing.
    EXPECT_FLOAT_EQ(O::DepthWeight(10.0f, 0.5f), 0.5f * O::DepthWeight(10.0f, 1.0f));
    EXPECT_FLOAT_EQ(O::DepthWeight(10.0f, 0.0f), 0.0f);
    // The clamps hold at the extremes.
    EXPECT_LE(O::DepthWeight(1e6f, 1.0f), 1e-2f + 1e-6f);
    EXPECT_LE(O::DepthWeight(1e-6f, 1.0f), 3e3f);
    // Composite algebra: the accumulation is a SUM and reveal is a PRODUCT —
    // both commutative, so pane order cannot change the resolve. Two panes,
    // both orders, identical resolve inputs.
    struct Pane {
        float z, a;
        float c;
    };
    const Pane p1{3.0f, 0.85f, 0.9f}, p2{7.0f, 0.85f, 0.2f};
    auto accumulate = [](const Pane& first, const Pane& second) {
        const float w1 = O::DepthWeight(first.z, first.a);
        const float w2 = O::DepthWeight(second.z, second.a);
        const float accum_c = first.c * first.a * w1 + second.c * second.a * w2;
        const float accum_w = first.a * w1 + second.a * w2;
        const float reveal = (1.0f - first.a) * (1.0f - second.a);
        return std::array<float, 3>{accum_c, accum_w, reveal};
    };
    const auto ab = accumulate(p1, p2);
    const auto ba = accumulate(p2, p1);
    EXPECT_FLOAT_EQ(ab[0], ba[0]);
    EXPECT_FLOAT_EQ(ab[1], ba[1]);
    EXPECT_FLOAT_EQ(ab[2], ba[2]);
    // Coverage: no glass -> 0; a full-alpha pane -> 1.
    EXPECT_FLOAT_EQ(O::ResolveCoverage(1.0f), 0.0f);
    EXPECT_FLOAT_EQ(O::ResolveCoverage(0.0f), 1.0f);
}

// the render-only snow-cover model — accumulates under
// snowfall, melts under sun, clamps [0,1], and 0-input stays 0 (the byte-identical
// default). The lighting_pass u_snowCover blend consumes exactly this scalar.
TEST(SnowCoverModel, AccumulatesUnderSnowMeltsUnderSunAndClamps) {
    namespace S = Luminumbra::Rendering::SnowCover;
    S::State s;
    // No snow, no sun: only the (slow) ambient thaw — a zero cover stays zero.
    S::Advance(s, 0.0f, 0.0f, 60.0f);
    EXPECT_FLOAT_EQ(s.cover01, 0.0f);
    // Heavy snow accumulates monotonically and saturates at 1.
    float prev = 0.0f;
    for (int i = 0; i < 10; ++i) {
        S::Advance(s, 1.0f, 0.0f, 20.0f);
        EXPECT_GE(s.cover01, prev);
        prev = s.cover01;
    }
    EXPECT_GT(s.cover01, 0.9f);
    S::Advance(s, 1.0f, 0.0f, 600.0f);
    EXPECT_FLOAT_EQ(s.cover01, 1.0f); // clamped
    // Full sun melts it back down monotonically to bare.
    for (int i = 0; i < 10; ++i) {
        const float before = s.cover01;
        S::Advance(s, 0.0f, 1.0f, 30.0f);
        EXPECT_LE(s.cover01, before);
    }
    S::Advance(s, 0.0f, 1.0f, 600.0f);
    EXPECT_FLOAT_EQ(s.cover01, 0.0f);
    // Negative dt is defended (no time travel).
    S::Advance(s, 1.0f, 0.0f, -5.0f);
    EXPECT_FLOAT_EQ(s.cover01, 0.0f);
}

// TIME AUTHORITY — time-of-day is a pure function of the
// sim tick. Same tick -> same TOD, bit-for-bit, no matter how many times or in
// what order it is evaluated (there is no accumulator to drift with frame
// pacing); the default day length reproduces the legacy 60 s/day wall pacing.
TEST(TodTickPurity, SameTickSameTodIndependentOfFramePacing) {
    namespace R = Luminumbra::Rendering;
    EXPECT_EQ(R::kDefaultDayLengthTicks, 1800u) << "60 s/day at 30 Hz — the legacy pacing";
    // Anchors: tick 0 == noon (tod 0); half a day == midnight (tod 0.5); wraps.
    EXPECT_FLOAT_EQ(R::TimeOfDayFromTick(0, 1800), 0.0f);
    EXPECT_FLOAT_EQ(R::TimeOfDayFromTick(900, 1800), 0.5f);
    EXPECT_FLOAT_EQ(R::TimeOfDayFromTick(1800, 1800), 0.0f);
    EXPECT_FLOAT_EQ(R::TimeOfDayFromTick(1800ull * 1000ull + 450ull, 1800), 0.25f);
    // Purity: evaluating out of order / repeatedly yields bit-identical values —
    // the property a wall-clock accumulator can never have.
    const std::uint64_t ticks[] = {7ull, 12345ull, 99999999ull, 3ull, 7ull};
    const float first_seven = R::TimeOfDayFromTick(7, 1800);
    for (std::uint64_t t : ticks) {
        const float a = R::TimeOfDayFromTick(t, 1800);
        const float b = R::TimeOfDayFromTick(t, 1800);
        EXPECT_EQ(a, b);
        if (t == 7ull) {
            EXPECT_EQ(a, first_seven);
        }
        EXPECT_GE(a, 0.0f);
        EXPECT_LT(a, 1.0f);
    }
    // Zero day length is defended (clamped to 1).
    EXPECT_FLOAT_EQ(R::TimeOfDayFromTick(42, 0), 0.0f);
}

// the LIVE season is the same pure function of the sim
// tick the sweeps use — feeding the authoritative tick per frame yields one
// season trajectory reproducible from the tick alone (no wall-clock, no drift).
TEST(LiveSeasonTick, SeasonPhaseIsPureFunctionOfSimTick) {
    namespace R = Luminumbra::Rendering;
    constexpr std::uint64_t kCycle = 216000ull; // any cycle: purity is the subject
    for (std::uint64_t t : {0ull, 1ull, 54000ull, 108000ull, 215999ull, 216000ull, 999999999ull}) {
        const R::SeasonState a = R::ComputeSeason(t, kCycle);
        const R::SeasonState b = R::ComputeSeason(t, kCycle);
        EXPECT_EQ(a.phase, b.phase);
        EXPECT_EQ(a.wave, b.wave);
        EXPECT_EQ(a.sunDeclination, b.sunDeclination);
    }
    // The wrap is exact: one full cycle later is the same season, bit-for-bit.
    const R::SeasonState s0 = R::ComputeSeason(1234, kCycle);
    const R::SeasonState s1 = R::ComputeSeason(1234 + kCycle, kCycle);
    EXPECT_EQ(s0.phase, s1.phase);
    EXPECT_EQ(s0.wave, s1.wave);
    //  is the season-NEUTRAL default every pre- live session sat at.
    EXPECT_FLOAT_EQ(R::ComputeSeason(0, kCycle).wave, 0.0f);
}

//  Tier 1: the celestial-body seam is PLUMBING, not math —
// every state field must be BIT-EQUAL to the direct TimeOfDayModel primitive
// calls across a tod x declination x tick sweep (the  technique). Any
// drift means the seam added math of its own, which Tier 1 forbids (-2).
TEST(CelestialBodyModel, SunMoonSeamBitExactAgainstPrimitives) {
    namespace R = Luminumbra::Rendering;
    constexpr std::uint64_t kLunar = 54000ull;
    const float tods[] = {0.0f, 0.04f, 0.13f, 0.25f, 0.5f, 0.77f, 0.999f};
    const float decls[] = {-0.41f, -0.1f, 0.0f, 0.2f, 0.41f};
    const std::uint64_t ticks[] = {0ull, 1234ull, 27000ull, 53999ull, 999999ull};
    const float overrides[] = {-1.0f, 0.0f, 0.5f, 1.0f};
    for (float tod : tods) {
        for (float decl : decls) {
            for (std::uint64_t tick : ticks) {
                for (float ov : overrides) {
                    const R::CelestialFrame f =
                        R::EvaluateCelestialBodies(tod, decl, tick, ov, kLunar);
                    const R::SunGeometry sg = R::ComputeSunGeometry(tod, decl);
                    // Bit-equality (memcmp): the seam may not perturb a single ULP.
                    EXPECT_EQ(0, std::memcmp(&f.sun_geometry, &sg, sizeof(sg)));
                    EXPECT_EQ(
                        0, std::memcmp(&f.sun.travel_direction, &sg.direction, sizeof(glm::vec3)));
                    EXPECT_EQ(
                        0, std::memcmp(&f.sun.light_direction, &sg.direction, sizeof(glm::vec3)));
                    EXPECT_EQ(f.sun.up_factor, sg.upFactor);
                    EXPECT_EQ(f.sun.elevation_rad, sg.elevationRad);
                    EXPECT_EQ(f.sun.day_factor, sg.sunIntensity);
                    EXPECT_EQ(f.sun.sky_dome_day_factor, sg.skyDomeDayFactor);
                    const R::MoonGeometry mg =
                        R::ComputeMoonGeometry(sg.angleRad, sg.tiltZ, sg.direction);
                    EXPECT_EQ(
                        0, std::memcmp(&f.moon.travel_direction, &mg.direction, sizeof(glm::vec3)));
                    EXPECT_EQ(
                        0, std::memcmp(&f.moon.light_direction, &mg.lightDir, sizeof(glm::vec3)));
                    EXPECT_EQ(f.moon.up_factor, mg.upFactor);
                    EXPECT_EQ(f.moon.illumination, R::ComputeMoonIllumination(tick, ov, kLunar));
                }
            }
        }
    }
}

// The flagged dynamic edge: god rays sample whichever opaque snapshot executed most
// recently -- snapshot #2 (weather) when the weather overlay ran, else snapshot #1.
// The declaration models this as a latest_writer_read; pruning the (conditional)
// weather snapshot -- exactly what happens on a clear-weather frame -- must shift the
// resolution back to snapshot #1, and the graph must still be sound.
TEST(RenderGraph, GodRaysReadLatestOpaqueSnapshotUnderBothBranches) {
    namespace R = Luminumbra::Rendering;

    R::RenderGraph with_weather = R::BuildLuminumbraFrameGraph();
    EXPECT_EQ(with_weather.latest_writer_of("lighting.opaque_color", "god_rays"),
              "weather_opaque_snapshot");
    EXPECT_TRUE(with_weather.validate().empty());

    R::RenderGraph clear_weather = R::BuildLuminumbraFrameGraph();
    clear_weather.prune("weather_opaque_snapshot"); // no weather overlay this frame
    EXPECT_EQ(clear_weather.latest_writer_of("lighting.opaque_color", "god_rays"),
              "opaque_snapshot");
    EXPECT_TRUE(clear_weather.validate().empty())
        << "god rays must still resolve a snapshot when the weather one is absent";
    // and the schedule stays complete + authored-consistent after pruning.
    EXPECT_EQ(clear_weather.schedule().size(), clear_weather.size());
}

// The validator has teeth: an unresolvable latest-writer and a plain read-before-write
// are both reported. (This is the "missing-edge negative test" -- proof the clean
// verdict above is not vacuous.)
TEST(RenderGraph, ValidatorReportsMisdeclaredDependencies) {
    namespace R = Luminumbra::Rendering;

    // (1) Remove BOTH opaque snapshots: god rays' latest-writer read can no longer
    // resolve -> a violation, and latest_writer_of returns "" (nothing to sample).
    R::RenderGraph no_snapshot = R::BuildLuminumbraFrameGraph();
    no_snapshot.prune("opaque_snapshot");
    no_snapshot.prune("weather_opaque_snapshot");
    EXPECT_FALSE(no_snapshot.validate().empty());
    EXPECT_EQ(no_snapshot.latest_writer_of("lighting.opaque_color", "god_rays"), std::string{});

    // (2) A hand-built read-before-write: the reader is authored BEFORE the only
    // producer of "R" -> a violation the topo order alone would silently accept.
    R::RenderGraph bad;
    bad.add({"reader", {"R"}, {}, {}, false});
    bad.add({"writer", {}, {"R"}, {}, false});
    const std::vector<std::string> v = bad.validate();
    EXPECT_FALSE(v.empty());
}

// The scheduler's edge logic in isolation: write-after-write keeps two writers of an
// accumulation target in authored order, and the reader lands after both.
TEST(RenderGraph, WriteAfterWriteAndReadAfterWriteOrderingHold) {
    namespace R = Luminumbra::Rendering;
    R::RenderGraph g;
    g.add({"a", {}, {"T"}, {}, false}); // first writer of T
    g.add({"b", {}, {"T"}, {}, false}); // second writer of T (WAW: after a)
    g.add({"c", {"T"}, {}, {}, false}); // reader of T (RAW: after the latest writer)

    const std::vector<std::string> s = g.schedule();
    const std::vector<std::string> expected = {"a", "b", "c"};
    EXPECT_EQ(s, expected);
    EXPECT_TRUE(g.validate().empty());
    EXPECT_EQ(g.latest_writer_of("T", "c"), "b");
}
