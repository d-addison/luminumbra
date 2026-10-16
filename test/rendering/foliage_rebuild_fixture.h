#pragma once

#include "luminumbra_client/rendering/passes/FoliagePass.h"

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>
#include <glad/glad.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <vector>

namespace FoliageTest {
using Luminumbra::Rendering::FoliagePass;
namespace Attribution = Luminumbra::Client::Attribution;

inline const glm::vec3 kCamera{16, 12, 16};
inline constexpr float kColumnExtent = static_cast<float>(Luminumbra::CHUNK_SIZE_X);

// All GL storage is released before the fixture releases its context, including
// when an assertion aborts a test. No production test seams are required.
struct ScopedPass {
    FoliagePass pass;
    ~ScopedPass() {
        pass.destroy_compute();
        pass.destroy_buffers();
    }
};

class FoliageRebuildTest : public ::testing::Test {
protected:
    void SetUp() override {
        initialized = glfwInit() == GLFW_TRUE;
        ASSERT_TRUE(initialized);
        glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
        glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 4);
        glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 5);
        glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
        window = glfwCreateWindow(64, 64, "foliage_rebuild_test", nullptr, nullptr);
        ASSERT_NE(window, nullptr);
        glfwMakeContextCurrent(window);
        ASSERT_NE(gladLoadGLLoader(reinterpret_cast<GLADloadproc>(glfwGetProcAddress)), 0);
    }
    void TearDown() override {
        if (window) {
            if (glad_glGetError) {
                EXPECT_EQ(glGetError(), static_cast<GLenum>(GL_NO_ERROR));
            }
            glfwDestroyWindow(window);
        }
        if (initialized) {
            glfwTerminate();
        }
    }
    static void Initialize(FoliagePass& pass, bool gpu = true) {
        pass.init_buffers();
        pass.set_readback_enabled(false);
        pass.set_fade_distances(48, 92);
        pass.set_density_scale(1.6f);
        pass.set_wind({0, 0});
        ASSERT_TRUE(pass.load_scatter_set(std::filesystem::path(LUMINUMBRA_SOURCE_ROOT) /
                                          "data/common/foliage/scatter_set.json"));
        if (gpu) {
            pass.init_compute(LUMINUMBRA_SOURCE_ROOT);
            ASSERT_TRUE(pass.gpu_scatter_active());
        }
    }
    static std::vector<FoliagePass::ChunkScatter> Chunks(std::size_t count) {
        // Small fixtures: at most three nearby columns, no pool saturation.
        std::vector<FoliagePass::ChunkScatter> result;
        for (std::size_t index = 0; index < count; ++index) {
            const auto x = static_cast<int>(index);
            FoliagePass::ChunkScatter chunk;
            chunk.chunk_xz = {x, 0};
            chunk.origin = {static_cast<float>(x) * 32.0f, 0, 0};
            chunk.biome_id = 1;
            chunk.density = 0.5f;
            result.push_back(chunk);
        }
        return result;
    }
    static void Rebuild(FoliagePass& pass,
                        const std::vector<FoliagePass::ChunkScatter>& chunks,
                        std::uint64_t frame,
                        const glm::vec3& camera = kCamera) {
        pass.set_evidence_frame(frame, 1);
        pass.rebuild_instances(
            chunks,
            +[](void*, float x, float z) {
                return FoliagePass::SurfaceSample{10.0f + x * 0.01f + z * 0.02f, 0.1f, 0.7f, true};
            },
            nullptr,
            camera);
    }
    static Attribution::FoliageRebuild
    RebuildAttributed(FoliagePass& pass,
                      const std::vector<FoliagePass::ChunkScatter>& chunks,
                      std::uint64_t frame_id,
                      const glm::vec3& camera = kCamera) {
        Attribution::FoliageFrameScope frame(true);
        Rebuild(pass, chunks, frame_id, camera);
        return frame.snapshot().rebuild;
    }
    static void ExpectReused(const Attribution::FoliageRebuild& rebuild, bool gpu) {
        EXPECT_EQ(rebuild.reasons, 0u);
        EXPECT_EQ(rebuild.path,
                  gpu ? Attribution::FoliagePath::ReusedGpu : Attribution::FoliagePath::ReusedCpu);
        EXPECT_EQ(rebuild.surface_misses, 0u);
        EXPECT_EQ(rebuild.uploaded_bytes, 0u);
        EXPECT_EQ(rebuild.hashed_bytes, 0u);
        EXPECT_FALSE(rebuild.backlog);
    }
    static std::vector<Luminumbra::VoxelVertex> GroundVertices(float height = 8.0f) {
        return {{{0, height, 0}, {0, 1, 0}, 3},
                {{kColumnExtent, height + 1.6f, 0}, {0, 1, 0}, 3},
                {{0, height + 1.6f, kColumnExtent}, {0, 1, 0}, 3},
                {{kColumnExtent, height, kColumnExtent}, {0, 1, 0}, 3}};
    }
    static std::vector<FoliagePass::ChunkScatter> SupportedChunks(std::size_t count) {
        auto result = Chunks(count);
        for (auto& chunk : result) {
            chunk.origin.x = static_cast<float>(chunk.chunk_xz.x) * kColumnExtent;
            chunk.extent_m = kColumnExtent;
        }
        return result;
    }
    static void UploadGround(FoliagePass& pass,
                             Luminumbra::ChunkID id,
                             const glm::ivec3& coords,
                             float height = 8.0f) {
        pass.update_ground_mesh(id, coords, GroundVertices(height), {0, 2, 3, 0, 3, 1});
    }
    static std::vector<FoliagePass::InstanceRecord> Records(const FoliagePass& pass, bool gpu) {
        return gpu ? GpuRecords() : pass.instances();
    }
    static GLuint Binding(GLuint index) {
        GLint buffer = 0;
        glGetIntegeri_v(GL_SHADER_STORAGE_BUFFER_BINDING, index, &buffer);
        return static_cast<GLuint>(buffer);
    }
    static std::vector<FoliagePass::InstanceRecord> GpuRecords() {
        std::array<GLuint, 5> command{};
        glGetNamedBufferSubData(Binding(3), 0, sizeof(command), command.data());
        EXPECT_LE(command[2], FoliagePass::kMaxInstances);
        // Bound before allocating even if a broken command exceeds the pool.
        const auto count = std::min<std::size_t>(command[2], FoliagePass::kMaxInstances);
        std::vector<FoliagePass::InstanceRecord> records(count);
        if (!records.empty()) {
            glGetNamedBufferSubData(Binding(2),
                                    0,
                                    static_cast<GLsizeiptr>(records.size() * sizeof(records[0])),
                                    records.data());
        }
        return records;
    }
    static void ExpectSame(const std::vector<FoliagePass::InstanceRecord>& first,
                           const std::vector<FoliagePass::InstanceRecord>& second) {
        ASSERT_EQ(first.size(), second.size());
        if (!first.empty()) {
            EXPECT_EQ(std::memcmp(first.data(), second.data(), first.size() * sizeof(first[0])), 0);
        }
    }
    static std::uint64_t
    Fnv(const void* data, std::size_t size, std::uint64_t hash = 1469598103934665603ull) {
        const auto* bytes = static_cast<const unsigned char*>(data);
        for (std::size_t offset = 0; offset < size; ++offset) {
            hash = (hash ^ bytes[offset]) * 1099511628211ull;
        }
        return hash;
    }
    // Independent copy of the pre-optimization recipe, using actual uploaded
    // bytes rather than the implementation's retained CPU scratch.
    static std::uint64_t LegacyUploadedHash(const FoliagePass& pass) {
        std::uint64_t hash = 1469598103934665603ull;
        for (const GLuint binding : {0u, 1u, 4u}) {
            GLint64 size = 0;
            const auto buffer = Binding(binding);
            glGetNamedBufferParameteri64v(buffer, GL_BUFFER_SIZE, &size);
            // These fixtures have <= 3 columns and <= 6 archetypes.
            EXPECT_GT(size, 0);
            EXPECT_LE(size, 3 * 8192 * 16);
            if (size <= 0 || size > 3 * 8192 * 16) {
                return 0;
            }
            std::vector<unsigned char> bytes(static_cast<std::size_t>(size));
            glGetNamedBufferSubData(buffer, 0, static_cast<GLsizeiptr>(size), bytes.data());
            hash = Fnv(bytes.data(), bytes.size(), hash);
        }
        float total_weight = 0;
        for (std::size_t index = 0; index < pass.archetype_count(); ++index) {
            total_weight += pass.archetype(index).density_weight;
        }
        if (total_weight <= 0) {
            total_weight = 1;
        }
        for (const float value :
             {kCamera.x, kCamera.z, pass.density_scale(), pass.fade_end_m(), total_weight}) {
            hash = Fnv(&value, sizeof(value), hash);
        }
        return hash;
    }

private:
    bool initialized = false;
    GLFWwindow* window = nullptr;
};

} // namespace FoliageTest
