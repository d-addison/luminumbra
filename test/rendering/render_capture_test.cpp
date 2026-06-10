#include "gtest/gtest.h"

#include <glad/glad.h>
#include <GLFW/glfw3.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>

#include "systems/SHIELD_WorldSystem.h"
#include "systems/WaterSystem.h"
#include "world/Chunk.h"
#include "world/MarchingCubes.h"

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
        ADD_FAILURE() << "Shader failed to compile: " << path.string() << "\n" << GetShaderInfoLog(shader);
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
        if (vertex != 0) glDeleteShader(vertex);
        if (fragment != 0) glDeleteShader(fragment);
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
    chunk.water_level_data.assign(
        static_cast<std::size_t>(WATER_SIM_RESOLUTION_X) * static_cast<std::size_t>(WATER_SIM_RESOLUTION_Z),
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

CaptureScene BuildFlatQuadScene(const std::string& name, const Vec3& color, float half_width, float half_height) {
    CaptureScene scene;
    scene.name = name;
    scene.object_color = color;
    scene.camera_position = glm::vec3{0.0f, 0.0f, 9.0f};
    scene.camera_target = glm::vec3{0.0f, 0.0f, 0.0f};
    scene.vertices = {
        {Vec3{-half_width, -half_height, 0.0f}, Vec3{0.0f, 0.0f, 1.0f}, 3u},
        {Vec3{ half_width, -half_height, 0.0f}, Vec3{0.0f, 0.0f, 1.0f}, 3u},
        {Vec3{ half_width,  half_height, 0.0f}, Vec3{0.0f, 0.0f, 1.0f}, 3u},
        {Vec3{-half_width,  half_height, 0.0f}, Vec3{0.0f, 0.0f, 1.0f}, 3u},
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

void WriteMetricsJson(const fs::path& path, const std::vector<std::pair<std::string, ImageMetrics>>& metrics) {
    std::ofstream output(path);
    ASSERT_TRUE(output) << path.string();
    output << "{\n";
    output << "  \"schema\": \"luminumbra.render_captures.v1\",\n";
    output << "  \"capture_ready_marker\": \"luminumbra.capture.ready:deterministic_render_capture:RenderDoc\",\n";
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
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, kCaptureWidth, kCaptureHeight, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);

    glGenRenderbuffers(1, &depth_renderbuffer);
    glBindRenderbuffer(GL_RENDERBUFFER, depth_renderbuffer);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, kCaptureWidth, kCaptureHeight);

    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, color_texture, 0);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, depth_renderbuffer);
    EXPECT_EQ(glCheckFramebufferStatus(GL_FRAMEBUFFER), GL_FRAMEBUFFER_COMPLETE);

    GLuint vao = 0;
    GLuint vbo = 0;
    GLuint ebo = 0;
    glGenVertexArrays(1, &vao);
    glGenBuffers(1, &vbo);
    glGenBuffers(1, &ebo);
    glBindVertexArray(vao);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, static_cast<GLsizeiptr>(scene.vertices.size() * sizeof(VoxelVertex)), scene.vertices.data(), GL_STATIC_DRAW);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ebo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, static_cast<GLsizeiptr>(scene.indices.size() * sizeof(u32)), scene.indices.data(), GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(VoxelVertex), reinterpret_cast<void*>(offsetof(VoxelVertex, position)));
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, sizeof(VoxelVertex), reinterpret_cast<void*>(offsetof(VoxelVertex, normal)));

    glViewport(0, 0, kCaptureWidth, kCaptureHeight);
    glEnable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glClearColor(0.04f, 0.06f, 0.08f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    const glm::mat4 model(1.0f);
    const glm::mat4 view = glm::lookAt(scene.camera_position, scene.camera_target, glm::vec3{0.0f, 1.0f, 0.0f});
    const glm::mat4 projection = glm::perspective(glm::radians(45.0f), static_cast<float>(kCaptureWidth) / kCaptureHeight, 0.1f, 128.0f);
    const glm::mat3 normal_matrix(1.0f);

    glUseProgram(program);
    SetMat4(program, "model", model);
    SetMat4(program, "view", view);
    SetMat4(program, "projection", projection);
    SetMat3(program, "normalMatrix", normal_matrix);
    glUniform3f(glGetUniformLocation(program, "lightPos"), 28.0f, 36.0f, 28.0f);
    glUniform3f(glGetUniformLocation(program, "viewPos"), scene.camera_position.x, scene.camera_position.y, scene.camera_position.z);
    glUniform3f(glGetUniformLocation(program, "lightColor"), 1.0f, 1.0f, 1.0f);
    glUniform3f(glGetUniformLocation(program, "objectColor"), scene.object_color.x, scene.object_color.y, scene.object_color.z);
    glDrawElements(GL_TRIANGLES, static_cast<GLsizei>(scene.indices.size()), GL_UNSIGNED_INT, nullptr);

    std::vector<unsigned char> pixels(static_cast<std::size_t>(kCaptureWidth) * static_cast<std::size_t>(kCaptureHeight) * 4u);
    glReadPixels(0, 0, kCaptureWidth, kCaptureHeight, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());

    glDeleteBuffers(1, &ebo);
    glDeleteBuffers(1, &vbo);
    glDeleteVertexArrays(1, &vao);
    glDeleteFramebuffers(1, &fbo);
    glDeleteRenderbuffers(1, &depth_renderbuffer);
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

        EXPECT_GT(metrics.foreground_pixels, static_cast<std::size_t>(kCaptureWidth * kCaptureHeight / 80)) << scene.name;
        EXPECT_GT(metrics.max_luminance, 20) << scene.name;
        EXPECT_LT(metrics.clipped_bright_pixels, static_cast<std::size_t>(kCaptureWidth * kCaptureHeight / 10)) << scene.name;
    }

    WriteMetricsJson(ArtifactRoot() / "render_captures.json", all_metrics);
    glDeleteProgram(program);
}
