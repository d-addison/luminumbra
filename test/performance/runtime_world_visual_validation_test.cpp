#include "gtest/gtest.h"

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>
#include <glad/glad.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

#include "entt/entt.hpp"
#include "nlohmann/json.hpp"
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>

#include "core/JobSystem.h"
#include "systems/PhysicsSystem.h"
#include "systems/SHIELD_WorldSystem.h"
#include "world/Chunk.h"
#include "world/TerrainPresetLoader.h"

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

constexpr int kSeed = 424242;
constexpr int kSurfaceRadius = 12;
constexpr int kCollisionRadius = 4;
constexpr int kCaptureWidth = 384;
constexpr int kCaptureHeight = 384;
constexpr int kTileSize = 16;
constexpr float kDensityEpsilon = 1.0e-4f;

struct ScopedJobSystem {
    ScopedJobSystem() {
        jobs.startup();
    }

    ~ScopedJobSystem() {
        jobs.shutdown();
    }

    JobSystem jobs;
};

struct ScopedPhysicsSystem {
    ScopedPhysicsSystem() {
        physics.startup();
    }

    ~ScopedPhysicsSystem() {
        physics.shutdown();
    }

    PhysicsSystem physics;
};

struct HiddenGlContext {
    HiddenGlContext() {
        if (!glfwInit()) {
            error = "glfwInit failed";
            return;
        }
        glfw_initialized = true;

        glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
        glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 4);
        glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 5);
        glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);

        window = glfwCreateWindow(64, 64, "runtime_world_visual_validation_test", nullptr, nullptr);
        if (!window) {
            error = "glfwCreateWindow failed";
            return;
        }

        glfwMakeContextCurrent(window);
        if (!gladLoadGLLoader(reinterpret_cast<GLADloadproc>(glfwGetProcAddress))) {
            error = "gladLoadGLLoader failed";
            return;
        }

        ready = true;
    }

    ~HiddenGlContext() {
        if (window) {
            glfwDestroyWindow(window);
        }
        if (glfw_initialized) {
            glfwTerminate();
        }
    }

    GLFWwindow* window = nullptr;
    bool glfw_initialized = false;
    bool ready = false;
    std::string error;
};

struct HorizonMetrics {
    std::size_t renderable_chunks = 0;
    std::size_t mesh_chunks = 0;
    std::size_t collision_chunks = 0;
    std::size_t vertices = 0;
    std::size_t indices = 0;
    std::size_t triangles = 0;
    std::array<std::size_t, 3> lod_mesh_chunks{0u, 0u, 0u};
    int min_chunk_x = std::numeric_limits<int>::max();
    int max_chunk_x = std::numeric_limits<int>::lowest();
    int min_chunk_z = std::numeric_limits<int>::max();
    int max_chunk_z = std::numeric_limits<int>::lowest();
    std::array<std::size_t, 4> quadrant_mesh_chunks{0u, 0u, 0u, 0u};
};

struct ImageMetrics {
    std::size_t foreground_pixels = 0;
    std::size_t occupied_tiles = 0;
    std::size_t clipped_dark_pixels = 0;
    std::size_t clipped_bright_pixels = 0;
    double mean_luminance = 0.0;
    int min_luminance = 255;
    int max_luminance = 0;
};

struct CombinedMesh {
    std::vector<VoxelVertex> vertices;
    std::vector<u32> indices;
};

struct SeamMetrics {
    std::size_t adjacent_pairs = 0;
    std::size_t mixed_lod_pairs = 0;
    std::size_t boundary_sample_points = 0;
    std::size_t density_mismatches = 0;
    std::size_t transition_risk_samples = 0;
    std::size_t mitigated_transition_risk_samples = 0;
    std::size_t unmitigated_transition_risk_samples = 0;
    std::size_t mixed_lod_pairs_with_transition_skirt = 0;
    std::size_t mixed_lod_pairs_without_transition_skirt = 0;
    float max_density_delta = 0.0f;
    std::map<std::string, std::size_t> pair_type_counts;
    std::map<std::string, std::size_t> mitigated_pair_type_counts;
    std::map<std::string, std::size_t> unmitigated_pair_type_counts;
    std::map<std::string, std::size_t> transition_skirt_pair_type_counts;
    std::map<std::string, std::size_t> missing_transition_skirt_pair_type_counts;
    std::vector<std::string> missing_transition_pair_details;
};

fs::path SourceRoot() {
    return fs::weakly_canonical(fs::path(LUMINUMBRA_SOURCE_ROOT));
}

fs::path ArtifactRoot() {
    return fs::path(LUMINUMBRA_TEST_ARTIFACT_DIR) / "runtime_world_visual";
}

TerrainGenParams LoadPresetParams(const fs::path& path) {
    // delegate to the canonical engine preset parser.
    const Luminumbra::world::TerrainPresetLoadResult result =
        Luminumbra::world::LoadTerrainPreset(path);
    EXPECT_TRUE(result.ok) << path.string();
    for (const std::string& error : result.errors) {
        ADD_FAILURE() << "preset parse error: " << error;
    }
    return result.params;
}

Vec3 ChunkBase(const Chunk& chunk) {
    return Vec3(chunk.get_coords() * IVec3(CHUNK_SIZE_X, CHUNK_SIZE_Y, CHUNK_SIZE_Z));
}

nlohmann::json VecToJson(const Vec3& value) {
    return {
        {"x", value.x},
        {"y", value.y},
        {"z", value.z},
    };
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
        if (vertex != 0) {
            glDeleteShader(vertex);
        }
        if (fragment != 0) {
            glDeleteShader(fragment);
        }
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

HorizonMetrics CalculateHorizonMetrics(const std::vector<Chunk*>& chunks,
                                       const IVec3& spawn_chunk) {
    HorizonMetrics metrics;
    metrics.renderable_chunks = chunks.size();

    for (const Chunk* chunk : chunks) {
        if (chunk->mesh_vertices.empty() || chunk->mesh_indices.empty()) {
            continue;
        }

        ++metrics.mesh_chunks;
        metrics.vertices += chunk->mesh_vertices.size();
        metrics.indices += chunk->mesh_indices.size();
        metrics.triangles += chunk->mesh_indices.size() / 3u;
        if (chunk->has_collision.load(std::memory_order_acquire)) {
            ++metrics.collision_chunks;
        }

        const int lod = std::clamp(chunk->current_lod.load(std::memory_order_acquire), 0, 2);
        ++metrics.lod_mesh_chunks[static_cast<std::size_t>(lod)];

        const IVec3 coords = chunk->get_coords();
        metrics.min_chunk_x = std::min(metrics.min_chunk_x, coords.x);
        metrics.max_chunk_x = std::max(metrics.max_chunk_x, coords.x);
        metrics.min_chunk_z = std::min(metrics.min_chunk_z, coords.z);
        metrics.max_chunk_z = std::max(metrics.max_chunk_z, coords.z);

        const int dx = coords.x - spawn_chunk.x;
        const int dz = coords.z - spawn_chunk.z;
        const int quadrant = (dx < 0 ? 0 : 1) + (dz < 0 ? 0 : 2);
        ++metrics.quadrant_mesh_chunks[static_cast<std::size_t>(quadrant)];
    }

    if (metrics.mesh_chunks == 0u) {
        metrics.min_chunk_x = 0;
        metrics.max_chunk_x = 0;
        metrics.min_chunk_z = 0;
        metrics.max_chunk_z = 0;
    }
    return metrics;
}

CombinedMesh BuildCombinedTerrainMesh(const std::vector<Chunk*>& chunks) {
    CombinedMesh mesh;
    std::size_t vertex_count = 0;
    std::size_t index_count = 0;
    for (const Chunk* chunk : chunks) {
        vertex_count += chunk->mesh_vertices.size();
        index_count += chunk->mesh_indices.size();
    }
    mesh.vertices.reserve(vertex_count);
    mesh.indices.reserve(index_count);

    for (const Chunk* chunk : chunks) {
        if (chunk->mesh_vertices.empty() || chunk->mesh_indices.empty()) {
            continue;
        }

        const u32 base_index = static_cast<u32>(mesh.vertices.size());
        const Vec3 base = ChunkBase(*chunk);
        for (VoxelVertex vertex : chunk->mesh_vertices) {
            vertex.position += base;
            mesh.vertices.push_back(vertex);
        }
        for (const u32 index : chunk->mesh_indices) {
            mesh.indices.push_back(base_index + index);
        }
    }

    return mesh;
}

ImageMetrics CalculateImageMetrics(const std::vector<unsigned char>& rgba) {
    ImageMetrics metrics;
    double luminance_sum = 0.0;
    const std::size_t pixel_count = rgba.size() / 4u;
    constexpr int clear_r = 8;
    constexpr int clear_g = 10;
    constexpr int clear_b = 14;

    const int tiles_x = kCaptureWidth / kTileSize;
    const int tiles_y = kCaptureHeight / kTileSize;
    std::vector<bool> occupied_tiles(static_cast<std::size_t>(tiles_x * tiles_y), false);

    for (std::size_t i = 0; i < pixel_count; ++i) {
        const int r = rgba[i * 4u];
        const int g = rgba[i * 4u + 1u];
        const int b = rgba[i * 4u + 2u];
        const int luminance = static_cast<int>(std::lround(0.2126 * r + 0.7152 * g + 0.0722 * b));
        metrics.min_luminance = std::min(metrics.min_luminance, luminance);
        metrics.max_luminance = std::max(metrics.max_luminance, luminance);
        luminance_sum += static_cast<double>(luminance);

        if (std::abs(r - clear_r) + std::abs(g - clear_g) + std::abs(b - clear_b) > 28) {
            ++metrics.foreground_pixels;
            const int x = static_cast<int>(i % kCaptureWidth);
            const int y = static_cast<int>(i / kCaptureWidth);
            const int tile_x = std::clamp(x / kTileSize, 0, tiles_x - 1);
            const int tile_y = std::clamp(y / kTileSize, 0, tiles_y - 1);
            occupied_tiles[static_cast<std::size_t>(tile_y * tiles_x + tile_x)] = true;
        }
        if (luminance <= 2) {
            ++metrics.clipped_dark_pixels;
        }
        if (luminance >= 253) {
            ++metrics.clipped_bright_pixels;
        }
    }

    if (pixel_count > 0u) {
        metrics.mean_luminance = luminance_sum / static_cast<double>(pixel_count);
    }
    metrics.occupied_tiles =
        static_cast<std::size_t>(std::count(occupied_tiles.begin(), occupied_tiles.end(), true));
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

std::vector<unsigned char>
RenderCombinedMesh(GLuint program, const CombinedMesh& mesh, const Vec3& spawn) {
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
                 static_cast<GLsizeiptr>(mesh.vertices.size() * sizeof(VoxelVertex)),
                 mesh.vertices.data(),
                 GL_STATIC_DRAW);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ebo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER,
                 static_cast<GLsizeiptr>(mesh.indices.size() * sizeof(u32)),
                 mesh.indices.data(),
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
    glClearColor(0.031f, 0.039f, 0.055f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    const glm::vec3 camera_target(spawn.x, spawn.y - 4.0f, spawn.z);
    const glm::vec3 camera_position(spawn.x - 180.0f, spawn.y + 118.0f, spawn.z + 180.0f);
    const glm::mat4 model(1.0f);
    const glm::mat4 view = glm::lookAt(camera_position, camera_target, glm::vec3{0.0f, 1.0f, 0.0f});
    const glm::mat4 projection = glm::perspective(
        glm::radians(58.0f), static_cast<float>(kCaptureWidth) / kCaptureHeight, 0.1f, 800.0f);
    const glm::mat3 normal_matrix(1.0f);

    glUseProgram(program);
    SetMat4(program, "model", model);
    SetMat4(program, "view", view);
    SetMat4(program, "projection", projection);
    SetMat3(program, "normalMatrix", normal_matrix);
    glUniform3f(glGetUniformLocation(program, "lightPos"),
                spawn.x - 80.0f,
                spawn.y + 160.0f,
                spawn.z + 120.0f);
    glUniform3f(glGetUniformLocation(program, "viewPos"),
                camera_position.x,
                camera_position.y,
                camera_position.z);
    glUniform3f(glGetUniformLocation(program, "lightColor"), 1.0f, 0.98f, 0.92f);
    glUniform3f(glGetUniformLocation(program, "objectColor"), 0.46f, 0.74f, 0.36f);
    glDrawElements(
        GL_TRIANGLES, static_cast<GLsizei>(mesh.indices.size()), GL_UNSIGNED_INT, nullptr);

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

Chunk* FindChunkByXZ(const std::vector<Chunk*>& chunks, int x, int z) {
    for (Chunk* chunk : chunks) {
        const IVec3 coords = chunk->get_coords();
        if (coords.x == x && coords.z == z && !chunk->mesh_vertices.empty()) {
            return chunk;
        }
    }
    return nullptr;
}

float BoundaryDensityDelta(const SHIELD_WorldSystem& world, const Vec3& world_pos) {
    const Vec3 nudged_x = world_pos + Vec3(kDensityEpsilon, 0.0f, 0.0f);
    const Vec3 nudged_z = world_pos + Vec3(0.0f, 0.0f, kDensityEpsilon);
    const float center = world.get_density_at(world_pos);
    return std::max(std::abs(center - world.get_density_at(nudged_x)),
                    std::abs(center - world.get_density_at(nudged_z)));
}

enum class SeamBoundaryFace {
    MinX,
    MaxX,
    MinZ,
    MaxZ,
};

Vec3 SeamBoundaryNormal(SeamBoundaryFace face) {
    switch (face) {
        case SeamBoundaryFace::MinX:
            return Vec3(-1.0f, 0.0f, 0.0f);
        case SeamBoundaryFace::MaxX:
            return Vec3(1.0f, 0.0f, 0.0f);
        case SeamBoundaryFace::MinZ:
            return Vec3(0.0f, 0.0f, -1.0f);
        case SeamBoundaryFace::MaxZ:
            return Vec3(0.0f, 0.0f, 1.0f);
    }
    return Vec3(0.0f, 0.0f, 0.0f);
}

bool VertexOnSeamFace(const VoxelVertex& vertex, SeamBoundaryFace face) {
    switch (face) {
        case SeamBoundaryFace::MinX:
            return std::abs(vertex.position.x) <= kDensityEpsilon;
        case SeamBoundaryFace::MaxX:
            return std::abs(vertex.position.x - static_cast<float>(CHUNK_SIZE_X)) <=
                   kDensityEpsilon;
        case SeamBoundaryFace::MinZ:
            return std::abs(vertex.position.z) <= kDensityEpsilon;
        case SeamBoundaryFace::MaxZ:
            return std::abs(vertex.position.z - static_cast<float>(CHUNK_SIZE_Z)) <=
                   kDensityEpsilon;
    }
    return false;
}

SeamBoundaryFace SharedFaceFor(const Chunk& chunk, const Chunk& neighbor, bool along_x) {
    const IVec3 coords = chunk.get_coords();
    const IVec3 neighbor_coords = neighbor.get_coords();
    if (along_x) {
        return coords.x < neighbor_coords.x ? SeamBoundaryFace::MaxX : SeamBoundaryFace::MinX;
    }
    return coords.z < neighbor_coords.z ? SeamBoundaryFace::MaxZ : SeamBoundaryFace::MinZ;
}

bool HasTransitionSkirtOnFace(const Chunk& chunk, SeamBoundaryFace face) {
    const Vec3 face_normal = SeamBoundaryNormal(face);
    std::size_t aligned_boundary_vertices = 0;
    for (const VoxelVertex& vertex : chunk.mesh_vertices) {
        if (VertexOnSeamFace(vertex, face) && std::abs(vertex.normal.y) <= 0.1f &&
            glm::dot(vertex.normal, face_normal) >= 0.98f) {
            ++aligned_boundary_vertices;
        }
    }
    return aligned_boundary_vertices >= 2u;
}

// Per-cell surface ownership splits a column's coarse mesh (and therefore its
// boundary coverage) across vertically adjacent chunks, so transition
// mitigation is a property of the column: the chunk of the pair may carry no
// surface at the shared face while its vertical sibling carries the skirt.
bool ColumnHasTransitionSkirtOnFace(const std::vector<Chunk*>& chunks,
                                    const Chunk& column_chunk,
                                    SeamBoundaryFace face) {
    const IVec3 coords = column_chunk.get_coords();
    for (Chunk* chunk : chunks) {
        const IVec3 candidate = chunk->get_coords();
        if (candidate.x == coords.x && candidate.z == coords.z &&
            HasTransitionSkirtOnFace(*chunk, face)) {
            return true;
        }
    }
    return false;
}

std::string SeamFaceName(SeamBoundaryFace face) {
    switch (face) {
        case SeamBoundaryFace::MinX:
            return "min_x";
        case SeamBoundaryFace::MaxX:
            return "max_x";
        case SeamBoundaryFace::MinZ:
            return "min_z";
        case SeamBoundaryFace::MaxZ:
            return "max_z";
    }
    return "unknown";
}

std::string ChunkCoordString(const Chunk& chunk) {
    const IVec3 coords = chunk.get_coords();
    return "(" + std::to_string(coords.x) + "," + std::to_string(coords.y) + "," +
           std::to_string(coords.z) + ")";
}

SeamMetrics CalculateSeamMetrics(const SHIELD_WorldSystem& world,
                                 const std::vector<Chunk*>& chunks) {
    SeamMetrics metrics;
    std::set<std::pair<ChunkID, ChunkID>> visited_pairs;

    auto add_pair = [&](Chunk* lhs, Chunk* rhs, bool along_x) {
        if (!lhs || !rhs || lhs == rhs) {
            return;
        }
        const ChunkID a = std::min(lhs->get_id(), rhs->get_id());
        const ChunkID b = std::max(lhs->get_id(), rhs->get_id());
        if (!visited_pairs.insert({a, b}).second) {
            return;
        }

        ++metrics.adjacent_pairs;
        const int lhs_lod = std::clamp(lhs->current_lod.load(std::memory_order_acquire), 0, 2);
        const int rhs_lod = std::clamp(rhs->current_lod.load(std::memory_order_acquire), 0, 2);
        const std::string pair_key = std::to_string(std::min(lhs_lod, rhs_lod)) + "-" +
                                     std::to_string(std::max(lhs_lod, rhs_lod));
        ++metrics.pair_type_counts[pair_key];
        if (lhs_lod == rhs_lod) {
            return;
        }

        ++metrics.mixed_lod_pairs;
        const Chunk* coarse_chunk = lhs_lod > rhs_lod ? lhs : rhs;
        const Chunk* fine_chunk = lhs_lod > rhs_lod ? rhs : lhs;
        const SeamBoundaryFace coarse_face = SharedFaceFor(*coarse_chunk, *fine_chunk, along_x);
        const bool vertical_offset = coarse_chunk->get_coords().y != fine_chunk->get_coords().y;
        const bool coarse_has_transition_skirt =
            ColumnHasTransitionSkirtOnFace(chunks, *coarse_chunk, coarse_face);
        const bool fine_has_transition_skirt =
            vertical_offset &&
            ColumnHasTransitionSkirtOnFace(
                chunks, *fine_chunk, SharedFaceFor(*fine_chunk, *coarse_chunk, along_x));
        const bool transition_is_covered = coarse_has_transition_skirt || fine_has_transition_skirt;
        if (transition_is_covered) {
            ++metrics.mixed_lod_pairs_with_transition_skirt;
            ++metrics.transition_skirt_pair_type_counts[pair_key + ":" + SeamFaceName(coarse_face)];
        } else {
            ++metrics.mixed_lod_pairs_without_transition_skirt;
            ++metrics.missing_transition_skirt_pair_type_counts[pair_key + ":" +
                                                                SeamFaceName(coarse_face)];
            std::size_t face_vertices = 0;
            std::size_t stamped_vertices = 0;
            const Vec3 coarse_face_normal = SeamBoundaryNormal(coarse_face);
            for (const VoxelVertex& vertex : coarse_chunk->mesh_vertices) {
                if (!VertexOnSeamFace(vertex, coarse_face)) {
                    continue;
                }
                ++face_vertices;
                if (std::abs(vertex.normal.y) <= 0.1f &&
                    glm::dot(vertex.normal, coarse_face_normal) >= 0.98f) {
                    ++stamped_vertices;
                }
            }
            float min_abs_face_sdf = std::numeric_limits<float>::max();
            if (!coarse_chunk->sdf_data.empty()) {
                for (int major = 0; major <= CHUNK_SIZE_X; ++major) {
                    for (int local_y = 0; local_y <= CHUNK_SIZE_Y; ++local_y) {
                        int sx = 0;
                        int sz = 0;
                        switch (coarse_face) {
                            case SeamBoundaryFace::MinX:
                                sx = 0;
                                sz = major;
                                break;
                            case SeamBoundaryFace::MaxX:
                                sx = CHUNK_SIZE_X;
                                sz = major;
                                break;
                            case SeamBoundaryFace::MinZ:
                                sx = major;
                                sz = 0;
                                break;
                            case SeamBoundaryFace::MaxZ:
                                sx = major;
                                sz = CHUNK_SIZE_Z;
                                break;
                        }
                        const std::size_t idx =
                            static_cast<std::size_t>(sx) +
                            static_cast<std::size_t>(local_y) * (CHUNK_SIZE_X + 1) +
                            static_cast<std::size_t>(sz) * (CHUNK_SIZE_X + 1) * (CHUNK_SIZE_Y + 1);
                        if (idx < coarse_chunk->sdf_data.size()) {
                            min_abs_face_sdf =
                                std::min(min_abs_face_sdf, std::abs(coarse_chunk->sdf_data[idx]));
                        }
                    }
                }
            }
            metrics.missing_transition_pair_details.push_back(
                pair_key + ":" + SeamFaceName(coarse_face) + ":coarse=" +
                ChunkCoordString(*coarse_chunk) + ":fine=" + ChunkCoordString(*fine_chunk) +
                ":coarse_lod=" + std::to_string(coarse_chunk->current_lod.load()) +
                ":mesh_verts=" + std::to_string(coarse_chunk->mesh_vertices.size()) +
                ":face_verts=" + std::to_string(face_vertices) +
                ":stamped=" + std::to_string(stamped_vertices) +
                ":sdf_size=" + std::to_string(coarse_chunk->sdf_data.size()) + ":min_face_sdf=" +
                (coarse_chunk->sdf_data.empty() ? std::string("none")
                                                : std::to_string(min_abs_face_sdf)));
        }
        const int coarse_step = lhs_lod > rhs_lod ? (lhs_lod == 1 ? 2 : 4) : (rhs_lod == 1 ? 2 : 4);
        const IVec3 lhs_coords = lhs->get_coords();
        const IVec3 rhs_coords = rhs->get_coords();
        const int boundary_x = along_x ? std::max(lhs_coords.x, rhs_coords.x) * CHUNK_SIZE_X
                                       : lhs_coords.x * CHUNK_SIZE_X;
        const int boundary_z = along_x ? lhs_coords.z * CHUNK_SIZE_Z
                                       : std::max(lhs_coords.z, rhs_coords.z) * CHUNK_SIZE_Z;
        const int min_y = std::min(lhs_coords.y, rhs_coords.y) * CHUNK_SIZE_Y;
        const int max_y = (std::max(lhs_coords.y, rhs_coords.y) + 1) * CHUNK_SIZE_Y;

        for (int major = 0; major <= CHUNK_SIZE_X; ++major) {
            for (int y = min_y; y <= max_y; ++y) {
                const Vec3 sample_pos =
                    along_x ? Vec3(static_cast<float>(boundary_x),
                                   static_cast<float>(y),
                                   static_cast<float>(lhs_coords.z * CHUNK_SIZE_Z + major))
                            : Vec3(static_cast<float>(lhs_coords.x * CHUNK_SIZE_X + major),
                                   static_cast<float>(y),
                                   static_cast<float>(boundary_z));
                ++metrics.boundary_sample_points;

                const float delta = BoundaryDensityDelta(world, sample_pos);
                metrics.max_density_delta = std::max(metrics.max_density_delta, delta);
                if (delta > 0.05f) {
                    ++metrics.density_mismatches;
                }

                const int local_major = major % coarse_step;
                const int local_y = std::abs(y - min_y) % coarse_step;
                const bool off_coarse_grid = local_major != 0 || local_y != 0;
                if (off_coarse_grid && std::abs(world.get_density_at(sample_pos)) <= 0.65f) {
                    ++metrics.transition_risk_samples;
                    if (transition_is_covered) {
                        ++metrics.mitigated_transition_risk_samples;
                        ++metrics.mitigated_pair_type_counts[pair_key];
                    } else {
                        ++metrics.unmitigated_transition_risk_samples;
                        ++metrics.unmitigated_pair_type_counts[pair_key];
                    }
                }
            }
        }
    };

    for (Chunk* chunk : chunks) {
        const IVec3 coords = chunk->get_coords();
        add_pair(chunk, FindChunkByXZ(chunks, coords.x + 1, coords.z), true);
        add_pair(chunk, FindChunkByXZ(chunks, coords.x, coords.z + 1), false);
    }

    return metrics;
}

void WriteJson(const fs::path& path, const nlohmann::json& data) {
    std::ofstream output(path);
    ASSERT_TRUE(output) << path.string();
    output << std::setw(2) << data << "\n";
}

nlohmann::json HorizonMetricsToJson(const HorizonMetrics& metrics) {
    return {
        {"renderable_chunks", metrics.renderable_chunks},
        {"mesh_chunks", metrics.mesh_chunks},
        {"collision_chunks", metrics.collision_chunks},
        {"vertices", metrics.vertices},
        {"indices", metrics.indices},
        {"triangles", metrics.triangles},
        {"lod_mesh_chunks",
         {
             {"lod0", metrics.lod_mesh_chunks[0]},
             {"lod1", metrics.lod_mesh_chunks[1]},
             {"lod2", metrics.lod_mesh_chunks[2]},
         }},
        {"chunk_extent",
         {
             {"min_x", metrics.min_chunk_x},
             {"max_x", metrics.max_chunk_x},
             {"min_z", metrics.min_chunk_z},
             {"max_z", metrics.max_chunk_z},
         }},
        {"quadrant_mesh_chunks",
         {
             metrics.quadrant_mesh_chunks[0],
             metrics.quadrant_mesh_chunks[1],
             metrics.quadrant_mesh_chunks[2],
             metrics.quadrant_mesh_chunks[3],
         }},
    };
}

nlohmann::json ImageMetricsToJson(const ImageMetrics& metrics) {
    return {
        {"width", kCaptureWidth},
        {"height", kCaptureHeight},
        {"foreground_pixels", metrics.foreground_pixels},
        {"occupied_tiles", metrics.occupied_tiles},
        {"clipped_dark_pixels", metrics.clipped_dark_pixels},
        {"clipped_bright_pixels", metrics.clipped_bright_pixels},
        {"min_luminance", metrics.min_luminance},
        {"max_luminance", metrics.max_luminance},
        {"mean_luminance", metrics.mean_luminance},
    };
}

nlohmann::json SeamMetricsToJson(const SeamMetrics& metrics) {
    nlohmann::json pair_counts = nlohmann::json::object();
    for (const auto& [key, value] : metrics.pair_type_counts) {
        pair_counts[key] = value;
    }
    nlohmann::json mitigated_pair_counts = nlohmann::json::object();
    for (const auto& [key, value] : metrics.mitigated_pair_type_counts) {
        mitigated_pair_counts[key] = value;
    }
    nlohmann::json unmitigated_pair_counts = nlohmann::json::object();
    for (const auto& [key, value] : metrics.unmitigated_pair_type_counts) {
        unmitigated_pair_counts[key] = value;
    }
    nlohmann::json transition_skirt_pair_counts = nlohmann::json::object();
    for (const auto& [key, value] : metrics.transition_skirt_pair_type_counts) {
        transition_skirt_pair_counts[key] = value;
    }
    nlohmann::json missing_transition_skirt_pair_counts = nlohmann::json::object();
    for (const auto& [key, value] : metrics.missing_transition_skirt_pair_type_counts) {
        missing_transition_skirt_pair_counts[key] = value;
    }

    return {
        {"adjacent_pairs", metrics.adjacent_pairs},
        {"mixed_lod_pairs", metrics.mixed_lod_pairs},
        {"mixed_lod_pairs_with_transition_skirt", metrics.mixed_lod_pairs_with_transition_skirt},
        {"mixed_lod_pairs_without_transition_skirt",
         metrics.mixed_lod_pairs_without_transition_skirt},
        {"boundary_sample_points", metrics.boundary_sample_points},
        {"density_mismatches", metrics.density_mismatches},
        {"transition_risk_samples", metrics.transition_risk_samples},
        {"mitigated_transition_risk_samples", metrics.mitigated_transition_risk_samples},
        {"unmitigated_transition_risk_samples", metrics.unmitigated_transition_risk_samples},
        {"max_density_delta", metrics.max_density_delta},
        {"pair_type_counts", pair_counts},
        {"mitigated_pair_type_counts", mitigated_pair_counts},
        {"unmitigated_pair_type_counts", unmitigated_pair_counts},
        {"transition_skirt_pair_type_counts", transition_skirt_pair_counts},
        {"missing_transition_skirt_pair_type_counts", missing_transition_skirt_pair_counts},
        {"missing_transition_pair_details", metrics.missing_transition_pair_details},
    };
}

struct ReadyWorld {
    ScopedJobSystem jobs;
    ScopedPhysicsSystem physics;
    TerrainGenParams params;
    SHIELD_WorldSystem world;
    Vec3 spawn;

    ReadyWorld()
        : params(LoadPresetParams(SourceRoot() / "worlds/atlas/presets/default.json"))
        , world(&jobs.jobs, nullptr, params, kSeed)
        , spawn(8.0f, world.GetTerrainHeightAt(8.0f, 8.0f) + 1.95f, 8.0f) {
        EXPECT_TRUE(world.EnsureSurfaceReadyNear(
            spawn, &physics.physics, kSurfaceRadius, kCollisionRadius));
    }
};

} // namespace

TEST(RuntimeWorldVisualValidationTest, SpawnHorizonRendersBroadVisibleCoverage) {
    fs::create_directories(ArtifactRoot());
    ReadyWorld ready_world;

    std::vector<Chunk*> renderable_chunks = ready_world.world.get_renderable_chunks();
    const IVec3 spawn_chunk = SHIELD_WorldSystem::world_to_chunk_coords(ready_world.spawn);
    const HorizonMetrics horizon = CalculateHorizonMetrics(renderable_chunks, spawn_chunk);
    const CombinedMesh combined_mesh = BuildCombinedTerrainMesh(renderable_chunks);

    HiddenGlContext context;
    if (!context.ready) {
        GTEST_SKIP() << context.error;
    }

    GLuint program = LinkBasicProgram();
    ASSERT_NE(program, 0u);

    const std::vector<unsigned char> pixels =
        RenderCombinedMesh(program, combined_mesh, ready_world.spawn);
    const ImageMetrics image = CalculateImageMetrics(pixels);
    WritePpm(ArtifactRoot() / "spawn_horizon.ppm", kCaptureWidth, kCaptureHeight, pixels);
    glDeleteProgram(program);

    const nlohmann::json report = {
        {"schema", "luminumbra.runtime_world_visual.v1"},
        {"seed", kSeed},
        {"spawn", VecToJson(ready_world.spawn)},
        {"horizon", HorizonMetricsToJson(horizon)},
        {"image", ImageMetricsToJson(image)},
        {"thresholds",
         {
             {"minimum_mesh_chunks", 600},
             {"minimum_collision_chunks", 81},
             {"minimum_foreground_pixels", kCaptureWidth * kCaptureHeight / 32},
             {"minimum_occupied_tiles", 45},
             {"minimum_quadrants_with_mesh", 4},
         }},
    };
    WriteJson(ArtifactRoot() / "runtime_world_visual.json", report);

    const std::size_t quadrants_with_mesh =
        static_cast<std::size_t>(std::count_if(horizon.quadrant_mesh_chunks.begin(),
                                               horizon.quadrant_mesh_chunks.end(),
                                               [](std::size_t count) { return count > 0u; }));

    EXPECT_GE(horizon.mesh_chunks, 600u);
    EXPECT_GE(horizon.collision_chunks, 81u);
    EXPECT_GT(horizon.lod_mesh_chunks[0], 0u);
    EXPECT_GT(horizon.lod_mesh_chunks[1], 0u);
    EXPECT_GT(horizon.lod_mesh_chunks[2], 0u);
    EXPECT_EQ(quadrants_with_mesh, 4u);
    EXPECT_LE(horizon.min_chunk_x, spawn_chunk.x - kSurfaceRadius);
    EXPECT_GE(horizon.max_chunk_x, spawn_chunk.x + kSurfaceRadius);
    EXPECT_LE(horizon.min_chunk_z, spawn_chunk.z - kSurfaceRadius);
    EXPECT_GE(horizon.max_chunk_z, spawn_chunk.z + kSurfaceRadius);
    EXPECT_GT(image.foreground_pixels,
              static_cast<std::size_t>(kCaptureWidth * kCaptureHeight / 32));
    EXPECT_GT(image.occupied_tiles, 45u);
    EXPECT_GT(image.max_luminance, 20);
    EXPECT_LT(image.clipped_bright_pixels,
              static_cast<std::size_t>(kCaptureWidth * kCaptureHeight / 8));
}

TEST(RuntimeWorldVisualValidationTest, MixedLodBoundariesAreContinuousAndReported) {
    fs::create_directories(ArtifactRoot());
    ReadyWorld ready_world;
    std::vector<Chunk*> renderable_chunks = ready_world.world.get_renderable_chunks();

    const SeamMetrics seams = CalculateSeamMetrics(ready_world.world, renderable_chunks);
    const nlohmann::json report = {
        {"schema", "luminumbra.lod_seams.v1"},
        {"seed", kSeed},
        {"spawn", VecToJson(ready_world.spawn)},
        {"metrics", SeamMetricsToJson(seams)},
        {"thresholds",
         {
             {"maximum_density_mismatches", 0},
             {"maximum_density_delta", 0.05},
             {"maximum_unmitigated_transition_risk_samples", 0},
             {"mixed_lod_pairs_must_be_reported", true},
         }},
    };
    WriteJson(ArtifactRoot() / "lod_seams.json", report);

    EXPECT_GT(seams.adjacent_pairs, 0u);
    EXPECT_GT(seams.mixed_lod_pairs, 0u);
    EXPECT_GT(seams.boundary_sample_points, 0u);
    EXPECT_EQ(seams.density_mismatches, 0u);
    EXPECT_EQ(seams.unmitigated_transition_risk_samples, 0u);
    EXPECT_EQ(seams.transition_risk_samples, seams.mitigated_transition_risk_samples);
    EXPECT_LT(seams.max_density_delta, 0.05f);
}

TEST(RuntimeWorldVisualValidationTest, JoinReadinessKeepsCollisionBudgetNearField) {
    fs::create_directories(ArtifactRoot());
    ReadyWorld ready_world;
    const std::vector<Chunk*> renderable_chunks = ready_world.world.get_renderable_chunks();
    const IVec3 spawn_chunk = SHIELD_WorldSystem::world_to_chunk_coords(ready_world.spawn);
    const HorizonMetrics horizon = CalculateHorizonMetrics(renderable_chunks, spawn_chunk);

    const nlohmann::json report = {
        {"schema", "luminumbra.physics_water_budget.v1"},
        {"seed", kSeed},
        {"spawn", VecToJson(ready_world.spawn)},
        {"collision_radius", kCollisionRadius},
        {"surface_radius", kSurfaceRadius},
        {"horizon", HorizonMetricsToJson(horizon)},
        {"thresholds",
         {
             {"expected_collision_chunks", 81},
             {"maximum_collision_chunks", 81},
             {"non_collision_horizon_chunks_allowed", true},
         }},
    };
    WriteJson(ArtifactRoot() / "physics_water_budget.json", report);

    EXPECT_EQ(horizon.collision_chunks, 81u);
    EXPECT_GE(horizon.mesh_chunks, 600u);
    EXPECT_GT(horizon.mesh_chunks, horizon.collision_chunks);
}
