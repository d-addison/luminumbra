#include "../support/DeterministicWorkBudget.h"
#include "luminumbra/core/Types.h"
#include "luminumbra_client/app/FrameAttribution.h"
#include "luminumbra_client/rendering/Camera.h"
#include "luminumbra_client/rendering/FoliageGroundMesh.h"
#include "luminumbra_client/rendering/FoliageSurfaceSlots.h"
#include "luminumbra_client/rendering/RenderContext.h"
#include "luminumbra_client/rendering/RenderResourceHandles.h"
#include "luminumbra_client/rendering/Shader.h"
#include "luminumbra_client/rendering/passes/FoliagePass.h"
#include "luminumbra_common/world/Chunk.h"

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>
#include <glad/glad.h>
#include <glm/ext/vector_float3.hpp>
#include <glm/ext/vector_float4.hpp>
#include <glm/ext/vector_int3.hpp>
#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <vector>

namespace {
using Luminumbra::Rendering::FoliagePass;
namespace Attribution = Luminumbra::Client::Attribution;

const glm::vec3 kCamera{16, 12, 16};
constexpr float kColumnExtent = static_cast<float>(Luminumbra::CHUNK_SIZE_X);

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
        EXPECT_EQ(rebuild.dirty_columns, 0u);
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
    static std::uint64_t LegacyUploadedHash(const FoliagePass& pass, std::size_t count) {
        std::uint64_t hash = 1469598103934665603ull;
        // The fixtures supply the admitted logical count (at most three), since
        // retained buffer capacity includes unused slots and dispatch unbinds
        // the compute program before returning.
        EXPECT_GT(count, 0u);
        EXPECT_LE(count, 3u);
        if (count == 0 || count > 3)
            return 0;
        const auto storage_bytes = [](GLuint binding) {
            GLint64 size = 0;
            glGetNamedBufferParameteri64v(Binding(binding), GL_BUFFER_SIZE, &size);
            return size;
        };
        EXPECT_EQ(storage_bytes(0), GLint64{512} * (8 * static_cast<GLint64>(sizeof(float)) + 4));
        EXPECT_EQ(storage_bytes(7), storage_bytes(0));
        EXPECT_GT(storage_bytes(1), 0);
        EXPECT_LE(storage_bytes(1), 4 * 8192 * static_cast<GLint64>(sizeof(glm::vec4)));
        EXPECT_GT(pass.archetype_count(), 0u);
        EXPECT_LE(pass.archetype_count(), 6u);
        EXPECT_EQ(storage_bytes(4),
                  static_cast<GLint64>(pass.archetype_count() * 8 * sizeof(float)));
        std::vector<unsigned char> params(count * 8 * sizeof(float));
        glGetNamedBufferSubData(
            Binding(0), 0, static_cast<GLsizeiptr>(params.size()), params.data());
        hash = Fnv(params.data(), params.size(), hash);
        std::vector<std::uint32_t> slots(count);
        glGetNamedBufferSubData(Binding(7),
                                std::size_t{512} * 8 * sizeof(float),
                                static_cast<GLsizeiptr>(count * sizeof(slots[0])),
                                slots.data());
        std::vector<unsigned char> surface(FoliagePass::kMaxCandidatesPerChunk * sizeof(glm::vec4));
        for (const auto slot : slots) {
            EXPECT_LT(slot, Luminumbra::Rendering::FoliageSurfaceSlots::kCapacity);
            if (slot >= Luminumbra::Rendering::FoliageSurfaceSlots::kCapacity)
                return 0;
            glGetNamedBufferSubData(Binding(1),
                                    static_cast<GLintptr>(slot * surface.size()),
                                    static_cast<GLsizeiptr>(surface.size()),
                                    surface.data());
            hash = Fnv(surface.data(), surface.size(), hash);
        }
        std::vector<float> palette(pass.archetype_count() * 8);
        glGetNamedBufferSubData(
            Binding(4), 0, static_cast<GLsizeiptr>(palette.size() * sizeof(float)), palette.data());
        hash = Fnv(palette.data(), palette.size() * sizeof(float), hash);
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

// Count the public GL entry points, forwarding every call to the real context.
class RebuildGlCalls {
public:
    RebuildGlCalls() {
        current = this;
        glad_glBufferData = BufferData;
        glad_glBufferSubData = BufferSubData;
        glad_glGetUniformLocation = UniformLocation;
        glad_glFenceSync = FenceSync;
    }
    ~RebuildGlCalls() {
        glad_glBufferData = original_buffer_data;
        glad_glBufferSubData = original_buffer_sub_data;
        glad_glGetUniformLocation = original_uniform_location;
        glad_glFenceSync = original_fence_sync;
        current = nullptr;
    }
    RebuildGlCalls(const RebuildGlCalls&) = delete;
    RebuildGlCalls& operator=(const RebuildGlCalls&) = delete;
    std::size_t scratch_allocations = 0;
    std::size_t uniform_queries = 0;
    std::size_t uploaded_bytes = 0;
    std::size_t uploads = 0;
    std::size_t surface_upload_bytes = 0;
    std::size_t surface_uploads = 0;
    std::size_t fences = 0;

private:
    static void GLAPIENTRY BufferData(GLenum target,
                                      GLsizeiptr size,
                                      const void* data,
                                      GLenum usage) {
        if (target == GL_SHADER_STORAGE_BUFFER && data == nullptr) {
            ++current->scratch_allocations;
        }
        if (data != nullptr && size > 0) {
            current->uploaded_bytes += static_cast<std::size_t>(size);
            ++current->uploads;
        }
        current->original_buffer_data(target, size, data, usage);
    }
    static void GLAPIENTRY BufferSubData(GLenum target,
                                         GLintptr offset,
                                         GLsizeiptr size,
                                         const void* data) {
        current->uploaded_bytes += static_cast<std::size_t>(size);
        if (size > 0)
            ++current->uploads;
        if (size ==
            static_cast<GLsizeiptr>(FoliagePass::kMaxCandidatesPerChunk * sizeof(glm::vec4))) {
            current->surface_upload_bytes += static_cast<std::size_t>(size);
            ++current->surface_uploads;
        }
        current->original_buffer_sub_data(target, offset, size, data);
    }
    static GLint GLAPIENTRY UniformLocation(GLuint program, const GLchar* name) {
        ++current->uniform_queries;
        return current->original_uniform_location(program, name);
    }
    static GLsync GLAPIENTRY FenceSync(GLenum condition, GLbitfield flags) {
        ++current->fences;
        return current->original_fence_sync(condition, flags);
    }
    inline static RebuildGlCalls* current = nullptr;
    PFNGLBUFFERDATAPROC original_buffer_data = glad_glBufferData;
    PFNGLBUFFERSUBDATAPROC original_buffer_sub_data = glad_glBufferSubData;
    PFNGLGETUNIFORMLOCATIONPROC original_uniform_location = glad_glGetUniformLocation;
    PFNGLFENCESYNCPROC original_fence_sync = glad_glFenceSync;
};

// Inject readiness failures at the public GL boundary without changing the
// ring's zero-timeout polling or relying on device speed. Storage stays real.
class ScopedReadbackNotReady {
public:
    explicit ScopedReadbackNotReady(GLenum result)
        : m_result(result) {
        current = this;
        glad_glClientWaitSync = Wait;
    }
    ~ScopedReadbackNotReady() {
        glad_glClientWaitSync = m_original_wait;
        current = nullptr;
    }
    ScopedReadbackNotReady(const ScopedReadbackNotReady&) = delete;
    ScopedReadbackNotReady& operator=(const ScopedReadbackNotReady&) = delete;
    std::size_t polls = 0;

private:
    static GLenum GLAPIENTRY Wait(GLsync, GLbitfield, GLuint64 timeout) {
        EXPECT_EQ(timeout, 0u);
        ++current->polls;
        return current->m_result;
    }
    inline static ScopedReadbackNotReady* current = nullptr;
    PFNGLCLIENTWAITSYNCPROC m_original_wait = glad_glClientWaitSync;
    GLenum m_result;
};

TEST_F(FoliageRebuildTest, CompletedCountIsUnknownAcrossOrdinaryAndFrozenGenerationRollover) {
    for (const bool frozen : {false, true}) {
        SCOPED_TRACE(frozen);
        ScopedPass scoped;
        auto& pass = scoped.pass;
        ASSERT_NO_FATAL_FAILURE(Initialize(pass));
        pass.set_readback_enabled(true);
        pass.set_evidence_enabled(true);
        const auto chunks = Chunks(1);
        Rebuild(pass, chunks, 1);
        glFinish();
        Rebuild(pass, chunks, 2);
        ASSERT_TRUE(pass.frame_instance_count_known());
        ASSERT_GT(pass.frame_instance_count(), 0u);
        const auto first_generation = pass.build_generation();
        const auto first_count = pass.frame_instance_count();
        {
            Attribution::FoliageFrameScope frame(true);
            if (frozen) {
                pass.set_evidence_frame(3, 1);
                ASSERT_TRUE(pass.begin_qualification());
            } else {
                pass.set_wind({1, 0});
                Rebuild(pass, chunks, 3);
            }
            EXPECT_EQ(pass.build_generation(), first_generation + 1);
            EXPECT_FALSE(pass.frame_instance_count_known());
            EXPECT_EQ(pass.frame_instance_count(), 0u);
            EXPECT_FALSE(frame.snapshot().count_known);
            EXPECT_EQ(frame.snapshot().generated_count, 0u);
            EXPECT_EQ(frame.snapshot().rebuild.rebuild_calls, 1u);
        }
        // The old blade snapshot is still identifiable for qualification, but
        // cannot stand in for the current command's as-yet-unknown count.
        EXPECT_EQ(pass.instance_generation(), first_generation);
        EXPECT_EQ(pass.instances().size(), first_count);
        glFinish();
        std::uint64_t frame_id = 4;
        for (const auto readiness : {GL_TIMEOUT_EXPIRED, GL_WAIT_FAILED}) {
            ScopedReadbackNotReady unavailable(readiness);
            Attribution::FoliageFrameScope frame(true);
            Rebuild(pass, chunks, frame_id++);
            EXPECT_GT(unavailable.polls, 0u);
            EXPECT_EQ(pass.build_generation(), first_generation + 1);
            EXPECT_FALSE(pass.frame_instance_count_known());
            EXPECT_EQ(pass.frame_instance_count(), 0u);
            EXPECT_FALSE(frame.snapshot().count_known);
            EXPECT_EQ(frame.snapshot().rebuild.copied_bytes, 0u);
        }
        {
            Attribution::FoliageFrameScope frame(true);
            Rebuild(pass, chunks, frame_id++);
            EXPECT_TRUE(pass.frame_instance_count_known());
            EXPECT_EQ(pass.frame_instance_count(), first_count);
            EXPECT_EQ(pass.frame_instance_count_generation(), pass.build_generation());
            EXPECT_TRUE(frame.snapshot().count_known);
            EXPECT_EQ(frame.snapshot().generated_count, first_count);
            EXPECT_EQ(frame.snapshot().count_generation, pass.build_generation());
        }
        if (frozen) {
            // The other frozen caller rebuilds on wind changes after the
            // independent qualification rebuild has already completed.
            ASSERT_TRUE(pass.qualification_rebuild_complete());
            pass.set_wind({1, 0});
            ScopedReadbackNotReady unavailable(GL_TIMEOUT_EXPIRED);
            Attribution::FoliageFrameScope frame(true);
            Rebuild(pass, chunks, frame_id);
            EXPECT_GT(unavailable.polls, 0u);
            EXPECT_EQ(pass.build_generation(), first_generation + 2);
            EXPECT_FALSE(pass.frame_instance_count_known());
            EXPECT_FALSE(frame.snapshot().count_known);
        }
    }
}

TEST_F(FoliageRebuildTest, EmptyAndInactiveGenerationsCannotReadBackThePreviousCommand) {
    for (const int empty_kind : {0, 1, 2, 3}) {
        SCOPED_TRACE(empty_kind);
        ScopedPass scoped;
        auto& pass = scoped.pass;
        ASSERT_NO_FATAL_FAILURE(Initialize(pass));
        pass.set_readback_enabled(true);
        auto chunks = Chunks(1);
        Rebuild(pass, chunks, 1);
        glFinish();
        Rebuild(pass, chunks, 2);
        ASSERT_TRUE(pass.frame_instance_count_known());
        ASSERT_GT(pass.frame_instance_count(), 0u);
        const auto old_generation = pass.build_generation();
        if (empty_kind == 0)
            chunks.clear();
        else if (empty_kind == 1)
            chunks.front().density = 0;
        else if (empty_kind == 2)
            pass.set_enabled(false);
        else
            pass.set_wind({1, 0}); // Force the null-query build through admission.
        for (std::uint64_t frame_id = 3; frame_id <= 5; ++frame_id) {
            Attribution::FoliageFrameScope frame(true);
            if (empty_kind == 3) {
                pass.set_evidence_frame(frame_id, 1);
                pass.rebuild_instances(chunks, nullptr, nullptr, kCamera);
            } else {
                Rebuild(pass, chunks, frame_id);
            }
            EXPECT_EQ(pass.build_generation(), old_generation + 1);
            EXPECT_TRUE(pass.frame_instance_count_known());
            EXPECT_EQ(pass.frame_instance_count(), 0u);
            EXPECT_EQ(pass.frame_instance_count_generation(), pass.build_generation());
            EXPECT_TRUE(frame.snapshot().count_known);
            EXPECT_EQ(frame.snapshot().generated_count, 0u);
            EXPECT_TRUE(pass.instances().empty());
            glFinish();
        }
        chunks = Chunks(1);
        pass.set_enabled(true);
        pass.set_wind({2, 0});
        Rebuild(pass, chunks, 6);
        EXPECT_FALSE(pass.frame_instance_count_known());
        glFinish();
        Rebuild(pass, chunks, 7);
        EXPECT_TRUE(pass.frame_instance_count_known());
        EXPECT_GT(pass.frame_instance_count(), 0u);
        EXPECT_EQ(pass.frame_instance_count_generation(), pass.build_generation());
    }
}

TEST_F(FoliageRebuildTest, RecreationAndWorldClearRetireCountAndRebuildUnchangedInputs) {
    for (const int reset_kind : {0, 1, 2}) {
        SCOPED_TRACE(reset_kind);
        ScopedPass scoped;
        auto& pass = scoped.pass;
        ASSERT_NO_FATAL_FAILURE(Initialize(pass));
        pass.set_readback_enabled(true);
        const auto chunks = Chunks(1);
        Rebuild(pass, chunks, 1);
        glFinish();
        Rebuild(pass, chunks, 2);
        ASSERT_TRUE(pass.frame_instance_count_known());
        ASSERT_GT(pass.frame_instance_count(), 0u);
        const auto generation = pass.build_generation();
        if (reset_kind == 2) {
            pass.clear_ground_meshes();
        } else {
            pass.destroy_compute();
            if (reset_kind == 1) {
                pass.destroy_buffers();
                pass.init_buffers();
            }
            pass.init_compute(LUMINUMBRA_SOURCE_ROOT);
            ASSERT_TRUE(pass.gpu_scatter_active());
        }
        EXPECT_TRUE(pass.frame_instance_count_known());
        EXPECT_EQ(pass.frame_instance_count(), 0u);
        EXPECT_TRUE(pass.instances().empty());
        // Same phase, wind, camera and chunks: resource retirement itself must
        // invalidate the scatter cache rather than requiring a settings change.
        Rebuild(pass, chunks, 3);
        EXPECT_EQ(pass.build_generation(), generation + 1);
        EXPECT_FALSE(pass.frame_instance_count_known());
        glFinish();
        Rebuild(pass, chunks, 4);
        EXPECT_TRUE(pass.frame_instance_count_known());
        EXPECT_GT(pass.frame_instance_count(), 0u);
        EXPECT_EQ(pass.frame_instance_count_generation(), pass.build_generation());
    }
}

TEST_F(FoliageRebuildTest, ReenabledReadbackRetriesOrdinaryAndFrozenSnapshots) {
    for (const int mode : {0, 1, 2}) {
        SCOPED_TRACE(mode);
        ScopedPass scoped;
        auto& pass = scoped.pass;
        ASSERT_NO_FATAL_FAILURE(Initialize(pass));
        pass.set_readback_enabled(true);
        pass.set_evidence_enabled(true);
        const auto chunks = Chunks(1);
        Rebuild(pass, chunks, 1);
        glFinish();
        Rebuild(pass, chunks, 2);
        ASSERT_TRUE(pass.frame_instance_count_known());
        ASSERT_GT(pass.frame_instance_count(), 0u);
        if (mode != 0) {
            ASSERT_TRUE(pass.begin_qualification());
            glFinish();
            Rebuild(pass, chunks, 3);
            ASSERT_TRUE(pass.frame_instance_count_known());
        }
        auto generation = pass.build_generation();
        pass.set_readback_enabled(false);
        EXPECT_FALSE(pass.frame_instance_count_known());
        if (mode == 2) {
            // Frozen wind rebuilds and subsequent retries must obey the
            // disabled readback policy as well as ordinary GPU builds do.
            RebuildGlCalls calls;
            pass.set_wind({1, 0});
            Rebuild(pass, chunks, 4);
            Rebuild(pass, chunks, 5);
            EXPECT_EQ(calls.fences, 0u);
            EXPECT_EQ(pass.build_generation(), generation + 1);
            ++generation;
        }
        pass.set_readback_enabled(true);
        {
            ScopedReadbackNotReady unavailable(GL_TIMEOUT_EXPIRED);
            RebuildGlCalls calls;
            Rebuild(pass, chunks, 6);
            EXPECT_EQ(calls.fences, 1u);
            EXPECT_GT(unavailable.polls, 0u);
            EXPECT_FALSE(pass.frame_instance_count_known());
            EXPECT_EQ(pass.build_generation(), generation);
        }
        glFinish();
        Rebuild(pass, chunks, 7);
        EXPECT_TRUE(pass.frame_instance_count_known());
        EXPECT_GT(pass.frame_instance_count(), 0u);
        EXPECT_EQ(pass.frame_instance_count_generation(), generation);
    }
}

TEST_F(FoliageRebuildTest, EvidenceIsExplicitAndPreservesLegacyHashAndGpuInstanceBytes) {
    ScopedPass scoped;
    auto& pass = scoped.pass;
    ASSERT_NO_FATAL_FAILURE(Initialize(pass));
    const auto chunks = Chunks(2);
    // Default evidence is off, even while attribution is explicitly active.
    {
        Attribution::FoliageFrameScope frame(true);
        Rebuild(pass, chunks, 1);
        EXPECT_FALSE(pass.uploaded_input_hash());
        EXPECT_EQ(frame.snapshot().rebuild.hashed_bytes, 0u);
        EXPECT_DOUBLE_EQ(frame.snapshot().rebuild.hash_ms, 0);
    }
    const auto reference = GpuRecords();
    ASSERT_FALSE(reference.empty());
    const auto expected_hash = LegacyUploadedHash(pass, chunks.size());
    const auto generation = pass.build_generation();

    // Enabling evidence on cached inputs hashes once without a new build/upload.
    pass.set_evidence_enabled(true);
    {
        Attribution::FoliageFrameScope frame(true);
        Rebuild(pass, chunks, 2);
        const auto cached_hash = pass.uploaded_input_hash();
        ASSERT_TRUE(cached_hash);
        if (!cached_hash) {
            return;
        }
        EXPECT_EQ(*cached_hash, expected_hash);
        EXPECT_EQ(frame.snapshot().rebuild.hashed_bytes,
                  2u * (8u * sizeof(float) + 8192u * sizeof(glm::vec4)) +
                      pass.archetype_count() * 8u * sizeof(float) + 5u * sizeof(float));
        EXPECT_EQ(frame.snapshot().rebuild.uploaded_bytes, 0u);
    }
    EXPECT_EQ(pass.build_generation(), generation);
    ExpectSame(reference, GpuRecords());

    // A fresh rebuild with evidence on produces the same bytes and legacy hash.
    pass.set_evidence_frame(3, 2);
    pass.rebuild_instances(
        chunks,
        +[](void*, float x, float z) {
            return FoliagePass::SurfaceSample{10.0f + x * 0.01f + z * 0.02f, 0.1f, 0.7f, true};
        },
        nullptr,
        kCamera);
    const auto rebuilt_hash = pass.uploaded_input_hash();
    ASSERT_TRUE(rebuilt_hash);
    if (!rebuilt_hash) {
        return;
    }
    EXPECT_EQ(*rebuilt_hash, expected_hash);
    ExpectSame(reference, GpuRecords());

    // No attribution collector is required to collect evidence.
    pass.set_evidence_enabled(false);
    EXPECT_FALSE(pass.uploaded_input_hash());
    pass.set_evidence_enabled(true);
    Rebuild(pass, chunks, 4);
    EXPECT_EQ(pass.uploaded_input_hash(), expected_hash);
    // Readback alone also cannot enable the full surface hash.
    pass.set_evidence_enabled(false);
    pass.set_readback_enabled(true);
    pass.set_evidence_frame(5, 2);
    {
        Attribution::FoliageFrameScope frame(true);
        pass.rebuild_instances(
            chunks,
            +[](void*, float, float) { return FoliagePass::SurfaceSample{10, 0.1f, 0.7f, true}; },
            nullptr,
            kCamera);
        EXPECT_FALSE(pass.uploaded_input_hash());
        EXPECT_EQ(frame.snapshot().rebuild.hashed_bytes, 0u);
        EXPECT_DOUBLE_EQ(frame.snapshot().rebuild.hash_ms, 0);
    }
}

TEST_F(FoliageRebuildTest, ReusedScratchMatchesFreshBuildsAcrossGrowthShrinkAndEmpty) {
    ScopedPass reused;
    ASSERT_NO_FATAL_FAILURE(Initialize(reused.pass));
    reused.pass.set_evidence_enabled(true);
    std::size_t high_water_columns = 0;
    std::size_t surface_capacity = 0;
    GLuint reused_count_buffer = 0;
    std::uint64_t frame_id = 1;
    for (const std::size_t columns : {1u, 3u, 2u, 0u, 1u, 3u}) {
        SCOPED_TRACE(columns);
        const auto chunks = Chunks(columns);
        {
            RebuildGlCalls calls;
            Attribution::FoliageFrameScope frame(true);
            Rebuild(reused.pass, chunks, frame_id++);
            const bool grows = columns > high_water_columns;
            const bool surface_grows = columns > surface_capacity;
            if (surface_grows) {
                surface_capacity = 1;
                while (surface_capacity < columns)
                    surface_capacity *= 2;
            }
            EXPECT_EQ(calls.scratch_allocations, (grows ? 2u : 0u) + (surface_grows ? 1u : 0u));
            EXPECT_EQ(calls.uniform_queries, 0u);
            EXPECT_EQ(frame.snapshot().rebuild.scratch_bytes,
                      (grows ? 2u * columns * 128u * sizeof(GLuint) : 0u) +
                          (surface_grows ? surface_capacity * 8192u * sizeof(glm::vec4) : 0u));
        }
        high_water_columns = std::max(high_water_columns, columns);
        if (columns == 0) {
            EXPECT_EQ(reused.pass.frame_instance_count(), 0u);
            EXPECT_FALSE(reused.pass.uploaded_input_hash());
            std::array<GLuint, 5> command{};
            glGetNamedBufferSubData(reused_count_buffer, 0, sizeof(command), command.data());
            EXPECT_EQ(command, (std::array<GLuint, 5>{0, 12, 0, 0, 0}));
            continue;
        }
        reused_count_buffer = Binding(3);
        const auto reused_records = GpuRecords();
        const auto reused_hash = reused.pass.uploaded_input_hash();
        EXPECT_EQ(reused_hash, LegacyUploadedHash(reused.pass, chunks.size()));
        GLint64 storage_size = 0;
        glGetNamedBufferParameteri64v(Binding(5), GL_BUFFER_SIZE, &storage_size);
        EXPECT_EQ(storage_size, static_cast<GLint64>(high_water_columns * 128u * sizeof(GLuint)));
        ScopedPass fresh;
        ASSERT_NO_FATAL_FAILURE(Initialize(fresh.pass));
        fresh.pass.set_evidence_enabled(true);
        Rebuild(fresh.pass, chunks, frame_id);
        EXPECT_EQ(reused_hash, fresh.pass.uploaded_input_hash());
        ExpectSame(reused_records, GpuRecords());
    }
}

TEST_F(FoliageRebuildTest, ComputeRecreationRefreshesUniformsAndScratchStorage) {
    ScopedPass scoped;
    auto& pass = scoped.pass;
    ASSERT_NO_FATAL_FAILURE(Initialize(pass));
    pass.set_evidence_enabled(true);
    const auto chunks = Chunks(2);
    Rebuild(pass, chunks, 1);
    const auto reference = GpuRecords();
    ASSERT_FALSE(reference.empty());
    const auto hash = pass.uploaded_input_hash();
    ASSERT_TRUE(hash.has_value());
    pass.destroy_compute();
    EXPECT_FALSE(pass.uploaded_input_hash());
    {
        RebuildGlCalls calls;
        pass.init_compute(LUMINUMBRA_SOURCE_ROOT);
        ASSERT_TRUE(pass.gpu_scatter_active());
        EXPECT_EQ(calls.uniform_queries, 12u);
        calls.scratch_allocations = 0;
        calls.uploaded_bytes = 0;
        Attribution::FoliageFrameScope frame(true);
        pass.set_evidence_frame(2, 1);
        pass.rebuild_instances(
            chunks,
            +[](void*, float, float) { return FoliagePass::SurfaceSample{0, 0, 0, false}; },
            nullptr,
            kCamera); // Cached exact surface inputs must be reused.
        EXPECT_EQ(calls.scratch_allocations, 3u);
        EXPECT_EQ(calls.uniform_queries, 12u);
        EXPECT_EQ(calls.surface_uploads, 2u);
        EXPECT_EQ(frame.snapshot().rebuild.surface_misses, 0u);
        EXPECT_EQ(frame.snapshot().rebuild.dirty_columns, 2u);
        EXPECT_EQ(frame.snapshot().rebuild.uploaded_bytes, calls.uploaded_bytes);
    }
    EXPECT_EQ(pass.uploaded_input_hash(), hash);
    ExpectSame(reference, GpuRecords());
}

TEST_F(FoliageRebuildTest, FullResourceRecreationRestoresSupportAndMatchesFreshBuild) {
    ScopedPass scoped;
    auto& pass = scoped.pass;
    ASSERT_NO_FATAL_FAILURE(Initialize(pass));
    pass.use_rendered_ground();
    pass.set_evidence_enabled(true);
    const auto chunks = SupportedChunks(2);
    UploadGround(pass, 1, {0, 0, 0});
    UploadGround(pass, 2, {1, 0, 0}, 6);
    Rebuild(pass, chunks, 1);
    const auto original = GpuRecords();
    ASSERT_FALSE(original.empty());
    const auto original_hash = pass.uploaded_input_hash();
    ASSERT_TRUE(original_hash.has_value());

    pass.destroy_compute();
    pass.destroy_buffers();
    EXPECT_FALSE(pass.uploaded_input_hash());
    pass.init_buffers();
    pass.init_compute(LUMINUMBRA_SOURCE_ROOT);
    ASSERT_TRUE(pass.gpu_scatter_active());
    // Full teardown clears ground support and cached surfaces. Restore CPU
    // authority before rebuilding with the valid sampler supplied by Rebuild.
    UploadGround(pass, 1, {0, 0, 0});
    UploadGround(pass, 2, {1, 0, 0}, 6);
    {
        RebuildGlCalls calls;
        const auto rebuilt = RebuildAttributed(pass, chunks, 2);
        EXPECT_EQ(rebuilt.path, Attribution::FoliagePath::Gpu);
        EXPECT_EQ(rebuilt.admitted_columns, 2u);
        EXPECT_EQ(rebuilt.surface_misses, 2u);
        EXPECT_EQ(rebuilt.dirty_columns, 2u);
        EXPECT_EQ(calls.surface_uploads, 2u);
        EXPECT_EQ(calls.surface_upload_bytes, sizeof(glm::vec4) * 2u * 8192u);
        EXPECT_EQ(calls.scratch_allocations, 3u);
        EXPECT_EQ(calls.uploaded_bytes,
                  sizeof(glm::vec4) * 2u * 8192u +
                      512u * (8u * sizeof(float) + sizeof(std::uint32_t)) +
                      pass.archetype_count() * 8u * sizeof(float) + 5u * sizeof(GLuint));
        EXPECT_EQ(rebuilt.uploaded_bytes, calls.uploaded_bytes);
    }
    const auto restored = GpuRecords();
    ASSERT_FALSE(restored.empty());
    const auto restored_hash = pass.uploaded_input_hash();
    ASSERT_TRUE(restored_hash.has_value());
    EXPECT_EQ(restored_hash, original_hash);
    EXPECT_EQ(restored_hash, LegacyUploadedHash(pass, chunks.size()));
    ExpectSame(original, restored);

    ScopedPass fresh;
    ASSERT_NO_FATAL_FAILURE(Initialize(fresh.pass));
    fresh.pass.use_rendered_ground();
    fresh.pass.set_evidence_enabled(true);
    UploadGround(fresh.pass, 1, {0, 0, 0});
    UploadGround(fresh.pass, 2, {1, 0, 0}, 6);
    Rebuild(fresh.pass, chunks, 2);
    const auto fresh_hash = fresh.pass.uploaded_input_hash();
    ASSERT_TRUE(fresh_hash.has_value());
    EXPECT_EQ(fresh_hash, LegacyUploadedHash(fresh.pass, chunks.size()));
    EXPECT_EQ(restored_hash, fresh_hash);
    ExpectSame(restored, GpuRecords());
}

TEST_F(FoliageRebuildTest, CpuFallbackInstanceBytesDoNotDependOnEvidence) {
    ScopedPass scoped;
    auto& pass = scoped.pass;
    ASSERT_NO_FATAL_FAILURE(Initialize(pass, false));
    const auto chunks = Chunks(2);
    Rebuild(pass, chunks, 1);
    const auto reference = pass.instances();
    ASSERT_FALSE(reference.empty());
    pass.set_evidence_enabled(true);
    pass.set_evidence_frame(2, 2);
    pass.rebuild_instances(
        chunks,
        +[](void*, float, float) { return FoliagePass::SurfaceSample{0, 0, 0, false}; },
        nullptr,
        kCamera);
    ExpectSame(reference, pass.instances());
    EXPECT_FALSE(pass.uploaded_input_hash()); // No GPU surface inputs were uploaded.
}

TEST_F(FoliageRebuildTest, QualificationRequiresCollectedInputsAndKeepsLegacyIdentity) {
    ScopedPass scoped;
    auto& pass = scoped.pass;
    ASSERT_NO_FATAL_FAILURE(Initialize(pass));
    pass.set_readback_enabled(true);
    const auto chunks = Chunks(1);
    for (std::uint64_t frame_id = 1; frame_id <= 4; ++frame_id) {
        Rebuild(pass, chunks, frame_id);
        glFinish(); // Test only: retire the real asynchronous readback.
    }
    ASSERT_FALSE(pass.instances().empty());
    EXPECT_STREQ(pass.qualification_blocker(), "input_evidence_not_collected");
    EXPECT_FALSE(pass.begin_qualification());
    const auto expected_hash = LegacyUploadedHash(pass, chunks.size());
    const std::array<float, 2> wind{0, 0};
    pass.set_evidence_enabled(true);
    Rebuild(pass, chunks, 5);
    ASSERT_TRUE(pass.begin_qualification());
    EXPECT_EQ(pass.qualification_evidence().rebuild.input_hash,
              Fnv(wind.data(), sizeof(wind), expected_hash));
}

TEST_F(FoliageRebuildTest, PaletteReloadUsesOnlyCurrentArchetypeBytes) {
    for (const bool gpu : {false, true}) {
        SCOPED_TRACE(gpu);
        ScopedPass reused;
        ASSERT_NO_FATAL_FAILURE(Initialize(reused.pass, gpu));
        reused.pass.set_evidence_enabled(true);
        const auto root = std::filesystem::path(LUMINUMBRA_SOURCE_ROOT);
        const auto full_palette = root / "data/common/foliage/scatter_set.json";
        const auto small_palette = root / "test/rendering/fixtures/foliage_palette_small.json";
        const auto chunks = Chunks(2);
        std::uint64_t frame_id = 1;
        Rebuild(reused.pass, chunks, frame_id++);
        for (const auto& palette : {small_palette, full_palette}) {
            const auto generation = reused.pass.build_generation();
            ASSERT_TRUE(reused.pass.load_scatter_set(palette));
            Rebuild(reused.pass, chunks, frame_id++);
            EXPECT_EQ(reused.pass.build_generation(), generation + 1);
            const auto records = Records(reused.pass, gpu);
            ASSERT_FALSE(records.empty());
            const auto hash = reused.pass.uploaded_input_hash();
            if (gpu) {
                EXPECT_EQ(hash, LegacyUploadedHash(reused.pass, chunks.size()));
                GLint64 palette_size = 0;
                glGetNamedBufferParameteri64v(Binding(4), GL_BUFFER_SIZE, &palette_size);
                EXPECT_EQ(palette_size,
                          static_cast<GLint64>(reused.pass.archetype_count() * 8 * sizeof(float)));
            }
            ScopedPass fresh;
            ASSERT_NO_FATAL_FAILURE(Initialize(fresh.pass, gpu));
            ASSERT_TRUE(fresh.pass.load_scatter_set(palette));
            fresh.pass.set_evidence_enabled(true);
            Rebuild(fresh.pass, chunks, frame_id);
            EXPECT_EQ(hash, fresh.pass.uploaded_input_hash());
            ExpectSame(records, Records(fresh.pass, gpu));
        }
    }
}

TEST_F(FoliageRebuildTest, DistantAndZeroDensityUploadsDoNotRebuildEitherBackend) {
    for (const bool gpu : {false, true}) {
        SCOPED_TRACE(gpu);
        ScopedPass scoped;
        auto& pass = scoped.pass;
        ASSERT_NO_FATAL_FAILURE(Initialize(pass, gpu));
        pass.use_rendered_ground();
        UploadGround(pass, 1, {0, 0, 0});
        UploadGround(pass, 2, {1, 0, 0});
        auto chunks = SupportedChunks(3);
        chunks.back().density = 0;
        auto distant = SupportedChunks(1).front();
        distant.chunk_xz = {40, 0};
        distant.origin.x = 40 * kColumnExtent;
        chunks.push_back(distant);
        RebuildAttributed(pass, chunks, 1);
        const auto original = Records(pass, gpu);
        ASSERT_FALSE(original.empty());
        const auto generation = pass.build_generation();

        UploadGround(pass, 90, {40, 0, 0});
        UploadGround(pass, 91, {2, 0, 0});
        ExpectReused(RebuildAttributed(pass, chunks, 2), gpu);
        EXPECT_EQ(pass.build_generation(), generation);
        ExpectSame(original, Records(pass, gpu));

        UploadGround(pass, 90, {40, 0, 0}, 6);
        ExpectReused(RebuildAttributed(pass, chunks, 3), gpu);
        pass.remove_ground_mesh(90);
        pass.remove_ground_mesh(91);
        ExpectReused(RebuildAttributed(pass, chunks, 4), gpu);
        // Streaming scatter entries outside admission cannot invalidate either.
        chunks.resize(2);
        ExpectReused(RebuildAttributed(pass, chunks, 5), gpu);
        EXPECT_EQ(pass.build_generation(), generation);
        ExpectSame(original, Records(pass, gpu));
    }
}

TEST_F(FoliageRebuildTest, ContributingUploadAndEditResampleOnlyTheChangedColumn) {
    for (const bool gpu : {false, true}) {
        SCOPED_TRACE(gpu);
        ScopedPass scoped;
        auto& pass = scoped.pass;
        ASSERT_NO_FATAL_FAILURE(Initialize(pass, gpu));
        pass.use_rendered_ground();
        const auto chunks = SupportedChunks(2);
        UploadGround(pass, 1, {0, 0, 0});
        RebuildAttributed(pass, chunks, 1); // Second column has no support yet.
        const auto first_column = Records(pass, gpu);
        ASSERT_FALSE(first_column.empty());

        UploadGround(pass, 2, {1, 0, 0});
        const auto uploaded = RebuildAttributed(pass, chunks, 2);
        EXPECT_EQ(uploaded.reasons, Attribution::GroundRevision);
        EXPECT_EQ(uploaded.surface_misses, 1u);
        EXPECT_EQ(uploaded.admitted_columns, 2u);
        const auto original = Records(pass, gpu);
        ASSERT_GT(original.size(), first_column.size());
        const auto generation = pass.build_generation();

        UploadGround(pass, 2, {1, 0, 0}, 6);
        const auto edited = RebuildAttributed(pass, chunks, 3);
        EXPECT_EQ(edited.reasons, Attribution::GroundRevision);
        EXPECT_EQ(edited.surface_misses, 1u);
        EXPECT_GT(edited.uploaded_bytes, 0u);
        EXPECT_FALSE(edited.backlog);
        EXPECT_EQ(pass.build_generation(), generation + 1);
        const auto changed = Records(pass, gpu);
        ASSERT_EQ(changed.size(), original.size());
        for (std::size_t index = 0; index < changed.size(); ++index) {
            if (original[index].pos[0] < kColumnExtent) {
                EXPECT_EQ(std::memcmp(&original[index], &changed[index], sizeof(changed[index])),
                          0);
            } else {
                EXPECT_NEAR(changed[index].pos[1], original[index].pos[1] - 2, 0.0001f);
            }
        }
        ExpectReused(RebuildAttributed(pass, chunks, 4), gpu);
    }
}

TEST_F(FoliageRebuildTest, LodReplacementInvalidatesContributingSupport) {
    for (const bool gpu : {false, true}) {
        SCOPED_TRACE(gpu);
        ScopedPass scoped;
        auto& pass = scoped.pass;
        ASSERT_NO_FATAL_FAILURE(Initialize(pass, gpu));
        pass.use_rendered_ground();
        const auto chunks = SupportedChunks(1);
        UploadGround(pass, 1, {0, 0, 0});
        RebuildAttributed(pass, chunks, 1);
        const auto original = Records(pass, gpu);
        ASSERT_FALSE(original.empty());

        // LOD adoption replaces the same resident source through update_ground_mesh.
        const auto vertices = GroundVertices(6);
        const std::vector<Luminumbra::u32> coarse_indices{0, 2, 1};
        pass.update_ground_mesh(1, {0, 0, 0}, vertices, coarse_indices);
        const auto replaced = RebuildAttributed(pass, chunks, 2);
        EXPECT_EQ(replaced.reasons, Attribution::GroundRevision);
        EXPECT_EQ(replaced.surface_misses, 1u);
        const auto coarse = Records(pass, gpu);
        ASSERT_FALSE(coarse.empty());
        EXPECT_LT(coarse.size(), original.size());
        const Luminumbra::Rendering::FoliageGroundMesh mesh(glm::vec3(0), vertices, coarse_indices);
        for (const auto& record : coarse) {
            const auto sample = mesh.sample(record.pos[0], record.pos[2]);
            ASSERT_TRUE(sample.valid);
            EXPECT_NEAR(record.pos[1], sample.height, 0.0001f);
        }
        ExpectReused(RebuildAttributed(pass, chunks, 3), gpu);
    }
}

TEST_F(FoliageRebuildTest, SupportLossAndRegainTrackAllSourcesInAColumn) {
    for (const bool gpu : {false, true}) {
        SCOPED_TRACE(gpu);
        ScopedPass scoped;
        auto& pass = scoped.pass;
        ASSERT_NO_FATAL_FAILURE(Initialize(pass, gpu));
        pass.use_rendered_ground();
        const auto chunks = SupportedChunks(1);
        UploadGround(pass, 1, {0, 0, 0});
        UploadGround(pass, 2, {0, 1, 0}, 0);
        RebuildAttributed(pass, chunks, 1);
        const auto upper = Records(pass, gpu);
        ASSERT_FALSE(upper.empty());

        pass.remove_ground_mesh(2);
        const auto lost_upper = RebuildAttributed(pass, chunks, 2);
        EXPECT_EQ(lost_upper.reasons, Attribution::GroundRevision);
        EXPECT_EQ(lost_upper.surface_misses, 1u);
        const auto lower = Records(pass, gpu);
        ASSERT_FALSE(lower.empty());
        for (const auto& record : lower) {
            EXPECT_LT(record.pos[1], 12);
        }
        pass.remove_ground_mesh(1);
        const auto lost_all = RebuildAttributed(pass, chunks, 3);
        EXPECT_EQ(lost_all.reasons, Attribution::GroundRevision);
        EXPECT_EQ(lost_all.surface_misses, 1u);
        EXPECT_TRUE(Records(pass, gpu).empty());
        ExpectReused(RebuildAttributed(pass, chunks, 4), gpu);
        // An absent source may cache empty results, never the former present hits.
        pass.remove_ground_mesh(1);
        ExpectReused(RebuildAttributed(pass, chunks, 5), gpu);

        UploadGround(pass, 2, {0, 1, 0}, 0);
        const auto regained = RebuildAttributed(pass, chunks, 6);
        EXPECT_EQ(regained.reasons, Attribution::GroundRevision);
        EXPECT_EQ(regained.surface_misses, 1u);
        ExpectSame(upper, Records(pass, gpu));
        // Loss and regain between draws must also evict the cached support.
        pass.remove_ground_mesh(2);
        UploadGround(pass, 2, {0, 1, 0}, 0);
        EXPECT_EQ(RebuildAttributed(pass, chunks, 7).reasons, Attribution::GroundRevision);
        ExpectSame(upper, Records(pass, gpu));
    }
}

TEST_F(FoliageRebuildTest, ColumnSignatureIsStableAndIndependentOfScatterOrder) {
    for (const bool gpu : {false, true}) {
        SCOPED_TRACE(gpu);
        ScopedPass scoped;
        auto& pass = scoped.pass;
        ASSERT_NO_FATAL_FAILURE(Initialize(pass, gpu));
        pass.use_rendered_ground();
        auto chunks = SupportedChunks(2);
        UploadGround(pass, 2, {1, 0, 0});
        UploadGround(pass, 1, {0, 0, 0});
        RebuildAttributed(pass, chunks, 1);
        const auto original = Records(pass, gpu);
        ASSERT_FALSE(original.empty());
        const auto generation = pass.build_generation();
        ExpectReused(RebuildAttributed(pass, chunks, 2), gpu);
        std::reverse(chunks.begin(), chunks.end());
        ExpectReused(RebuildAttributed(pass, chunks, 3), gpu);
        EXPECT_EQ(pass.build_generation(), generation);
        ExpectSame(original, Records(pass, gpu));

        UploadGround(pass, 1, {0, 0, 0}, 6);
        EXPECT_EQ(RebuildAttributed(pass, chunks, 4).reasons, Attribution::GroundRevision);
        const auto changed = Records(pass, gpu);
        std::reverse(chunks.begin(), chunks.end());
        ExpectReused(RebuildAttributed(pass, chunks, 5), gpu);
        EXPECT_EQ(pass.build_generation(), generation + 1);
        ExpectSame(changed, Records(pass, gpu));
    }
}

TEST_F(FoliageRebuildTest, MovingASourceInvalidatesBothOldAndNewColumns) {
    for (const bool gpu : {false, true}) {
        SCOPED_TRACE(gpu);
        ScopedPass scoped;
        auto& pass = scoped.pass;
        ASSERT_NO_FATAL_FAILURE(Initialize(pass, gpu));
        pass.use_rendered_ground();
        const auto chunks = SupportedChunks(2);
        UploadGround(pass, 1, {0, 0, 0});
        RebuildAttributed(pass, chunks, 1);
        ASSERT_FALSE(Records(pass, gpu).empty());
        UploadGround(pass, 1, {1, 0, 0});
        const auto moved = RebuildAttributed(pass, chunks, 2);
        EXPECT_EQ(moved.reasons, Attribution::GroundRevision);
        EXPECT_EQ(moved.surface_misses, 2u);
        const auto records = Records(pass, gpu);
        ASSERT_FALSE(records.empty());
        for (const auto& record : records) {
            EXPECT_GE(record.pos[0], kColumnExtent);
        }
        UploadGround(pass, 1, {40, 0, 0});
        EXPECT_EQ(RebuildAttributed(pass, chunks, 3).reasons, Attribution::GroundRevision);
        EXPECT_TRUE(Records(pass, gpu).empty());
    }
}

TEST_F(FoliageRebuildTest, RepeatedScatterColumnsStillInvalidateOnSupportLoss) {
    for (const bool gpu : {false, true}) {
        SCOPED_TRACE(gpu);
        ScopedPass scoped;
        auto& pass = scoped.pass;
        ASSERT_NO_FATAL_FAILURE(Initialize(pass, gpu));
        pass.use_rendered_ground();
        auto chunks = SupportedChunks(1);
        chunks.push_back(chunks.front());
        UploadGround(pass, 1, {0, 0, 0});
        RebuildAttributed(pass, chunks, 1);
        ASSERT_FALSE(Records(pass, gpu).empty());
        pass.remove_ground_mesh(1);
        const auto removed = RebuildAttributed(pass, chunks, 2);
        EXPECT_EQ(removed.reasons, Attribution::GroundRevision);
        EXPECT_EQ(removed.surface_misses, 1u);
        EXPECT_TRUE(Records(pass, gpu).empty());
        ExpectReused(RebuildAttributed(pass, chunks, 3), gpu);
    }
}

TEST_F(FoliageRebuildTest, SelectingRenderedSupportDiscardsProceduralCacheEntries) {
    for (const bool gpu : {false, true}) {
        SCOPED_TRACE(gpu);
        ScopedPass scoped;
        auto& pass = scoped.pass;
        ASSERT_NO_FATAL_FAILURE(Initialize(pass, gpu));
        const auto chunks = SupportedChunks(1);
        RebuildAttributed(pass, chunks, 1);
        ASSERT_FALSE(Records(pass, gpu).empty());
        const auto generation = pass.build_generation();
        UploadGround(pass, 1, {0, 0, 0});
        ExpectReused(RebuildAttributed(pass, chunks, 2), gpu);
        pass.remove_ground_mesh(1);
        ExpectReused(RebuildAttributed(pass, chunks, 3), gpu);
        pass.use_rendered_ground();
        const auto selected = RebuildAttributed(pass, chunks, 4);
        EXPECT_EQ(selected.surface_misses, 1u);
        EXPECT_EQ(pass.build_generation(), generation + 1);
        EXPECT_TRUE(Records(pass, gpu).empty());
        UploadGround(pass, 1, {0, 0, 0});
        EXPECT_EQ(RebuildAttributed(pass, chunks, 5).reasons, Attribution::GroundRevision);
        ASSERT_FALSE(Records(pass, gpu).empty());
        pass.use_rendered_ground();
        ExpectReused(RebuildAttributed(pass, chunks, 6), gpu);
    }
}

TEST_F(FoliageRebuildTest, LocalizedReuseMatchesLegacyForcedRebuildBytesAndHash) {
    for (const bool gpu : {false, true}) {
        SCOPED_TRACE(gpu);
        ScopedPass scoped;
        auto& pass = scoped.pass;
        ASSERT_NO_FATAL_FAILURE(Initialize(pass, gpu));
        pass.use_rendered_ground();
        pass.set_evidence_enabled(true);
        const auto chunks = SupportedChunks(2);
        UploadGround(pass, 1, {0, 0, 0});
        UploadGround(pass, 2, {1, 0, 0});
        Rebuild(pass, chunks, 1);
        const auto original = Records(pass, gpu);
        ASSERT_FALSE(original.empty());
        const auto original_hash = pass.uploaded_input_hash();
        const auto generation = pass.build_generation();
        UploadGround(pass, 90, {40, 0, 0});
        Rebuild(pass, chunks, 2); // Also exercise elision without attribution enabled.
        EXPECT_EQ(pass.build_generation(), generation);
        ExpectSame(original, Records(pass, gpu));

        // The former global revision forced this whole-scatter rebuild after a
        // distant upload. Phase changes request that same rebuild through the
        // public interface, with the original placement and evidence recipes.
        pass.set_evidence_frame(3, 2);
        pass.rebuild_instances(
            chunks,
            +[](void*, float, float) { return FoliagePass::SurfaceSample{100, 0, 1, true}; },
            nullptr,
            kCamera);
        EXPECT_EQ(pass.build_generation(), generation + 1);
        ExpectSame(original, Records(pass, gpu));
        EXPECT_EQ(pass.uploaded_input_hash(), original_hash);
        if (gpu) {
            const auto rebuilt_hash = pass.uploaded_input_hash();
            ASSERT_TRUE(rebuilt_hash.has_value());
            if (rebuilt_hash.has_value()) {
                EXPECT_EQ(*rebuilt_hash, LegacyUploadedHash(pass, chunks.size()));
            }
        } else {
            EXPECT_EQ(pass.instance_hash(),
                      Fnv(original.data(), original.size() * sizeof(original[0])));
        }
        // Resampling identical support must preserve every record byte too.
        UploadGround(pass, 1, {0, 0, 0});
        Rebuild(pass, chunks, 4);
        ExpectSame(original, Records(pass, gpu));
        EXPECT_EQ(pass.uploaded_input_hash(), original_hash);
    }
}

TEST_F(FoliageRebuildTest, StableSlotsUploadOnlyDirtySurfacesAndRetainMetadataAndPalette) {
    ScopedPass scoped;
    auto& pass = scoped.pass;
    ASSERT_NO_FATAL_FAILURE(Initialize(pass));
    pass.use_rendered_ground();
    pass.set_evidence_enabled(true);
    const auto chunks = SupportedChunks(2);
    UploadGround(pass, 1, {0, 0, 0});
    UploadGround(pass, 2, {1, 0, 0});
    const auto initial = RebuildAttributed(pass, chunks, 1);
    EXPECT_EQ(initial.dirty_columns, 2u);
    {
        RebuildGlCalls calls;
        ExpectReused(RebuildAttributed(pass, chunks, 2), true);
        EXPECT_EQ(calls.uploaded_bytes, 0u);
        EXPECT_EQ(calls.scratch_allocations, 0u);
    }
    // Request compute work with identical surfaces through the public phase input.
    {
        RebuildGlCalls calls;
        Attribution::FoliageFrameScope frame(true);
        pass.set_evidence_frame(3, 2);
        pass.rebuild_instances(
            chunks,
            +[](void*, float, float) { return FoliagePass::SurfaceSample{0, 0, 0, false}; },
            nullptr,
            kCamera);
        EXPECT_EQ(frame.snapshot().rebuild.dirty_columns, 0u);
        EXPECT_EQ(frame.snapshot().rebuild.uploaded_bytes, 5u * sizeof(GLuint));
        EXPECT_EQ(calls.uploaded_bytes, 5u * sizeof(GLuint));
        EXPECT_EQ(calls.surface_upload_bytes, 0u);
        EXPECT_EQ(calls.scratch_allocations, 0u);
    }
    UploadGround(pass, 2, {1, 0, 0}, 6);
    {
        RebuildGlCalls calls;
        const auto edited = RebuildAttributed(pass, chunks, 4);
        EXPECT_EQ(edited.dirty_columns, 1u);
        EXPECT_EQ(edited.surface_misses, 1u);
        EXPECT_EQ(calls.surface_uploads, 1u);
        EXPECT_EQ(calls.surface_upload_bytes, 8192u * sizeof(glm::vec4));
        EXPECT_EQ(calls.uploaded_bytes, 8192u * sizeof(glm::vec4) + 5u * sizeof(GLuint));
        EXPECT_EQ(edited.uploaded_bytes, calls.uploaded_bytes);
        EXPECT_EQ(edited.uploads, calls.uploads);
        EXPECT_EQ(calls.scratch_allocations, 0u);
    }
    EXPECT_EQ(pass.uploaded_input_hash(), LegacyUploadedHash(pass, chunks.size()));
}

TEST_F(FoliageRebuildTest, SlotReuseAfterEditsAndHorizonMotionMatchesFreshFullBuild) {
    ScopedPass reused;
    ASSERT_NO_FATAL_FAILURE(Initialize(reused.pass));
    reused.pass.use_rendered_ground();
    reused.pass.set_evidence_enabled(true);
    const auto all = SupportedChunks(3);
    for (int column = 0; column < 3; ++column)
        UploadGround(reused.pass, (static_cast<Luminumbra::ChunkID>(column) + 1), {column, 0, 0});
    std::uint64_t frame = 1;
    constexpr std::array<std::array<std::size_t, 2>, 5> horizons{
        {{0, 1}, {0, 1}, {1, 2}, {2, 0}, {0, 1}}};
    for (const auto indices : horizons) {
        const bool edited = frame >= 2;
        if (frame == 2)
            UploadGround(reused.pass, 2, {1, 0, 0}, 6);
        const std::vector<FoliagePass::ChunkScatter> chunks{all[indices[0]], all[indices[1]]};
        const auto rebuild = RebuildAttributed(reused.pass, chunks, frame);
        EXPECT_EQ(rebuild.dirty_columns, frame == 1 ? 2u : 1u);
        const auto records = GpuRecords();
        ASSERT_FALSE(records.empty());
        const auto hash = reused.pass.uploaded_input_hash();
        EXPECT_EQ(hash, LegacyUploadedHash(reused.pass, chunks.size()));
        ScopedPass fresh;
        ASSERT_NO_FATAL_FAILURE(Initialize(fresh.pass));
        fresh.pass.use_rendered_ground();
        fresh.pass.set_evidence_enabled(true);
        for (int column = 0; column < 3; ++column)
            UploadGround(fresh.pass,
                         (static_cast<Luminumbra::ChunkID>(column) + 1),
                         {column, 0, 0},
                         edited && column == 1 ? 6.0f : 8.0f);
        Rebuild(fresh.pass, chunks, frame++);
        EXPECT_EQ(hash, fresh.pass.uploaded_input_hash());
        ExpectSame(records, GpuRecords());
    }
}

TEST_F(FoliageRebuildTest, ColumnCapFallsBackBeforeSurfaceUploadsAndRecovers) {
    ScopedPass scoped;
    auto& pass = scoped.pass;
    ASSERT_NO_FATAL_FAILURE(Initialize(pass));
    const auto chunks = Chunks(1);
    Rebuild(pass, chunks, 1);
    const auto reference = GpuRecords();
    // Duplicate entries stay inside the distance horizon and exercise the
    // admitted-column cap without a large terrain fixture or sampling workload.
    const std::vector<FoliagePass::ChunkScatter> overflow(
        Luminumbra::Rendering::FoliageSurfaceSlots::kCapacity + 1, chunks.front());
    {
        RebuildGlCalls calls;
        const auto fallback = RebuildAttributed(pass, overflow, 2);
        EXPECT_EQ(fallback.path, Attribution::FoliagePath::Cpu);
        EXPECT_EQ(fallback.fallback, Attribution::FallbackReason::ColumnCapacity);
        EXPECT_EQ(fallback.dirty_columns, 0u);
        EXPECT_EQ(calls.surface_upload_bytes, 0u);
        EXPECT_FALSE(pass.instances().empty());
    }
    const auto recovered = RebuildAttributed(pass, chunks, 3);
    EXPECT_EQ(recovered.path, Attribution::FoliagePath::Gpu);
    ExpectSame(reference, GpuRecords());
}

TEST_F(FoliageRebuildTest, DuplicateColumnsShareUploadsAndDistinctFootprintsKeepExactInputs) {
    ScopedPass scoped;
    auto& pass = scoped.pass;
    ASSERT_NO_FATAL_FAILURE(Initialize(pass));
    pass.use_rendered_ground();
    UploadGround(pass, 1, {0, 0, 0});
    pass.set_evidence_enabled(true);
    auto chunks = Chunks(1);
    chunks.push_back(chunks.front());
    {
        RebuildGlCalls calls;
        const auto rebuilt = RebuildAttributed(pass, chunks, 1);
        EXPECT_EQ(rebuilt.path, Attribution::FoliagePath::Gpu);
        EXPECT_EQ(rebuilt.admitted_columns, 2u);
        EXPECT_EQ(rebuilt.dirty_columns, 1u);
        EXPECT_EQ(calls.surface_uploads, 1u);
    }
    EXPECT_EQ(pass.uploaded_input_hash(), LegacyUploadedHash(pass, chunks.size()));
    chunks.back().origin.x += 1;
    {
        RebuildGlCalls calls;
        const auto rebuilt = RebuildAttributed(pass, chunks, 2);
        EXPECT_EQ(rebuilt.path, Attribution::FoliagePath::Gpu);
        EXPECT_EQ(rebuilt.admitted_columns, 2u);
        EXPECT_EQ(calls.surface_uploads, 2u); // Growth reconstructs the old slot too.
    }
    EXPECT_EQ(pass.uploaded_input_hash(), LegacyUploadedHash(pass, chunks.size()));
    const auto records = GpuRecords();
    const auto hash = pass.uploaded_input_hash();
    ScopedPass fresh;
    ASSERT_NO_FATAL_FAILURE(Initialize(fresh.pass));
    fresh.pass.use_rendered_ground();
    UploadGround(fresh.pass, 1, {0, 0, 0});
    fresh.pass.set_evidence_enabled(true);
    Rebuild(fresh.pass, chunks, 2);
    EXPECT_EQ(hash, fresh.pass.uploaded_input_hash());
    ExpectSame(records, GpuRecords());
    UploadGround(pass, 1, {0, 0, 0}, 6);
    const auto edited = RebuildAttributed(pass, chunks, 3);
    EXPECT_EQ(edited.surface_misses, 2u);
    EXPECT_EQ(edited.dirty_columns, 2u);
    const auto edited_records = GpuRecords();
    const auto edited_hash = pass.uploaded_input_hash();
    EXPECT_EQ(edited_hash, LegacyUploadedHash(pass, chunks.size()));
    UploadGround(fresh.pass, 1, {0, 0, 0}, 6);
    Rebuild(fresh.pass, chunks, 3);
    EXPECT_EQ(edited_hash, fresh.pass.uploaded_input_hash());
    ExpectSame(edited_records, GpuRecords());
}

TEST_F(FoliageRebuildTest, DirtyBatchesStopAtCleanSlotsAndMatchFreshBuild) {
    ScopedPass reused;
    ASSERT_NO_FATAL_FAILURE(Initialize(reused.pass));
    reused.pass.use_rendered_ground();
    auto chunks = SupportedChunks(9);
    for (std::size_t index = 0; index < chunks.size(); ++index) {
        const auto x = static_cast<int>(index % 3);
        const auto z = static_cast<int>(index / 3);
        chunks[index].chunk_xz = {x, z};
        chunks[index].origin = {
            static_cast<float>(x) * kColumnExtent, 0, static_cast<float>(z) * kColumnExtent};
        UploadGround(reused.pass, static_cast<Luminumbra::ChunkID>(index + 1), {x, 0, z});
    }
    for (std::uint64_t frame = 1; frame <= 3; ++frame)
        Rebuild(reused.pass, chunks, frame);
    ASSERT_FALSE(GpuRecords().empty());
    // Read the physical ownership map rather than assuming distance-sort order.
    std::array<std::uint32_t, 9> slots{};
    std::array<float, 9 * 8> params{};
    glGetNamedBufferSubData(
        Binding(7), std::size_t{512} * 8 * sizeof(float), sizeof(slots), slots.data());
    glGetNamedBufferSubData(Binding(0), 0, sizeof(params), params.data());
    std::array<bool, 9> edited{};
    for (std::size_t index = 0; index < slots.size(); ++index) {
        if (slots[index] <= 2 || slots[index] == 4) {
            const auto x = static_cast<int>(params[index * 8 + 4]);
            const auto z = static_cast<int>(params[index * 8 + 5]);
            ASSERT_GE(x, 0);
            ASSERT_LT(x, 3);
            ASSERT_GE(z, 0);
            ASSERT_LT(z, 3);
            const auto source = static_cast<std::size_t>(x) + static_cast<std::size_t>(z) * 3;
            edited[source] = true;
            UploadGround(reused.pass, static_cast<Luminumbra::ChunkID>(source + 1), {x, 0, z}, 6);
        }
    }
    ASSERT_EQ(std::count(edited.begin(), edited.end(), true), 4);
    // A clean gap must neither join the transfer nor be copied into batch scratch.
    {
        RebuildGlCalls calls;
        const auto work = RebuildAttributed(reused.pass, chunks, 4);
        EXPECT_EQ(work.surface_misses, 4u);
        EXPECT_EQ(work.dirty_columns, 4u);
        EXPECT_FALSE(work.backlog);
        EXPECT_EQ(work.uploads, 3u); // Three adjacent slots, one isolated slot, command.
        EXPECT_EQ(work.uploads, calls.uploads);
        EXPECT_EQ(work.uploaded_bytes, sizeof(glm::vec4) * 4u * 8192u + 5u * sizeof(GLuint));
        EXPECT_EQ(work.uploaded_bytes, calls.uploaded_bytes);
        EXPECT_EQ(work.copied_bytes,
                  chunks.size() * (sizeof(FoliagePass::ChunkScatter) + 8u * sizeof(float) +
                                   sizeof(std::uint32_t)) +
                      sizeof(glm::vec4) * 3u * 8192u);
    }
    const auto records = GpuRecords();
    ScopedPass fresh;
    ASSERT_NO_FATAL_FAILURE(Initialize(fresh.pass));
    fresh.pass.use_rendered_ground();
    for (std::size_t index = 0; index < chunks.size(); ++index)
        UploadGround(fresh.pass,
                     static_cast<Luminumbra::ChunkID>(index + 1),
                     {static_cast<int>(index % 3), 0, static_cast<int>(index / 3)},
                     edited[index] ? 6.0f : 8.0f);
    for (std::uint64_t frame = 1; frame <= 3; ++frame)
        Rebuild(fresh.pass, chunks, frame);
    ExpectSame(records, GpuRecords());
}

TEST_F(FoliageRebuildTest, DeterministicWorkBudgetMovementEditDrainAndIdle) {
    using namespace Luminumbra::TestSupport;
    for (const bool gpu : {false, true}) {
        SCOPED_TRACE(gpu);
        ScopedPass scoped;
        auto& pass = scoped.pass;
        ASSERT_NO_FATAL_FAILURE(Initialize(pass, gpu));
        pass.use_rendered_ground();
        // Nine supported columns exceed the production four-column admission
        // bound. Fixture storage is bounded to 9 * 8192 candidate records.
        auto chunks = SupportedChunks(9);
        for (std::size_t index = 0; index < chunks.size(); ++index) {
            const auto x = static_cast<int>(index % 3);
            const auto z = static_cast<int>(index / 3);
            chunks[index].chunk_xz = {x, z};
            chunks[index].origin = {
                static_cast<float>(x) * kColumnExtent, 0, static_cast<float>(z) * kColumnExtent};
            UploadGround(pass, static_cast<Luminumbra::ChunkID>(index + 1), {x, 0, z});
        }
        const auto limits = WorkLimits("foliage");
        WorkCounts peak;
        std::uint64_t frame_id = 0;
        std::uint64_t previous_backlog = 0;
        const auto measure = [&](const glm::vec3& camera, std::uint64_t step) {
            RebuildGlCalls calls;
            const auto work = RebuildAttributed(pass, chunks, ++frame_id, camera);
            if (gpu) {
                EXPECT_EQ(work.uploads, calls.uploads);
                EXPECT_EQ(work.uploaded_bytes, calls.uploaded_bytes);
            }
            const auto growth = work.deferred_columns > previous_backlog
                                    ? work.deferred_columns - previous_backlog
                                    : 0;
            previous_backlog = work.deferred_columns;
            const WorkCounts observed{{"surface_misses", work.surface_misses},
                                      {"surface_samples", work.surface_samples},
                                      {"uploaded_bytes", work.uploaded_bytes},
                                      {"uploads", work.uploads},
                                      {"copied_bytes", work.copied_bytes},
                                      {"hashed_bytes", work.hashed_bytes},
                                      {"deferred_columns", work.deferred_columns},
                                      {"backlog_growth", growth},
                                      {"drain_steps", step}};
            EXPECT_TRUE(CheckWork(observed, "foliage"));
            PeakWork(peak, observed);
            EXPECT_EQ(work.backlog, work.deferred_columns != 0);
            return work;
        };
        Attribution::FoliageRebuild work{};
        for (std::uint64_t step = 1; step <= limits.at("drain_steps"); ++step) {
            work = measure(kCamera, step);
            if (step == 1) {
                EXPECT_EQ(work.surface_misses, 4u);
                EXPECT_GT(work.surface_samples, 0u);
                EXPECT_EQ(work.deferred_columns, 5u);
                EXPECT_EQ(work.uploads, gpu ? 4u : 1u);
            }
            if (!work.backlog)
                break;
        }
        ASSERT_FALSE(work.backlog) << "foliage failed to converge in the pinned step bound";
        ASSERT_EQ(work.deferred_columns, 0u);
        EXPECT_EQ(pass.gpu_scatter_active(), gpu);
        EXPECT_FALSE(Records(pass, gpu).empty());

        // Cross a signature cell while reusing the same column support.
        const glm::vec3 moved_camera{kCamera.x + 16, kCamera.y, kCamera.z};
        const auto moved = measure(moved_camera, 1);
        EXPECT_EQ(moved.surface_misses, 0u);
        EXPECT_GT(moved.uploaded_bytes, 0u);
        EXPECT_EQ(moved.uploads, 1u);
        EXPECT_FALSE(moved.backlog);
        // A real support revision must invalidate exactly one column.
        UploadGround(pass, 1, {0, 0, 0}, 6);
        const auto edited = measure(moved_camera, 1);
        EXPECT_EQ(edited.surface_misses, 1u);
        EXPECT_GT(edited.surface_samples, 0u);
        EXPECT_GT(edited.uploaded_bytes, 0u);
        EXPECT_EQ(edited.uploads, gpu ? 2u : 1u);
        EXPECT_FALSE(edited.backlog);

        // A standing camera after settlement must do precisely zero work for
        // every idle counter, including a single upload/hash byte.
        for (int idle_step = 0; idle_step < 8; ++idle_step) {
            const auto idle = RebuildAttributed(pass, chunks, ++frame_id, moved_camera);
            EXPECT_TRUE(CheckWork({{"uploaded_bytes", idle.uploaded_bytes},
                                   {"hashed_bytes", idle.hashed_bytes},
                                   {"uploads", idle.uploads},
                                   {"surface_misses", idle.surface_misses},
                                   {"surface_samples", idle.surface_samples},
                                   {"deferred_columns", idle.deferred_columns}},
                                  "idle"));
            ExpectReused(idle, gpu);
        }
        EXPECT_TRUE(CheckWork(peak, "foliage"));
    }
}

// Fixed 64x64 color storage for exercising the public draw path.
struct ScopedDrawTarget {
    GLuint framebuffer = 0;
    GLuint color = 0;
    ScopedDrawTarget() {
        glGenRenderbuffers(1, &color);
        glBindRenderbuffer(GL_RENDERBUFFER, color);
        glRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA8, 64, 64);
        glGenFramebuffers(1, &framebuffer);
        glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, color);
    }
    ~ScopedDrawTarget() {
        glDeleteFramebuffers(1, &framebuffer);
        glDeleteRenderbuffers(1, &color);
    }
};

TEST_F(FoliageRebuildTest, GpuDrawsSurviveChurnBeforeFirstSnapshotAndAfterEmptySnapshot) {
    ScopedPass scoped;
    auto& pass = scoped.pass;
    ASSERT_NO_FATAL_FAILURE(Initialize(pass));
    pass.init_shader(LUMINUMBRA_SOURCE_ROOT);
    ASSERT_TRUE(pass.shader()->IsValid());
    pass.set_readback_enabled(true);
    ScopedDrawTarget target;
    ASSERT_EQ(glCheckFramebufferStatus(GL_FRAMEBUFFER),
              static_cast<GLenum>(GL_FRAMEBUFFER_COMPLETE));
    Luminumbra::Rendering::RenderContext context;
    context.screen_width = context.screen_height = 64;
    context.lit_scene = Luminumbra::Rendering::FboHandle{target.framebuffer};
    const Luminumbra::Rendering::Camera camera(kCamera);
    auto chunks = Chunks(1);
    // Retire each result, then invalidate it before adoption. CPU counts remain
    // unknown while the current indirect GPU command must still be submitted.
    for (std::uint64_t frame = 1; frame <= 4; ++frame) {
        pass.set_wind({static_cast<float>(frame), 0});
        Rebuild(pass, chunks, frame);
        ASSERT_FALSE(GpuRecords().empty());
        EXPECT_TRUE(pass.instances().empty());
        EXPECT_FALSE(pass.frame_instance_count_known());
        pass.execute(context, camera);
        EXPECT_EQ(pass.last_draw_calls(), 1u);
        glFinish();
    }
    // A valid zero snapshot must not prevent a later nonempty GPU draw either.
    // A zero scale normalizes to 1.0f. Chunks(1) sets every chunk's density to
    // 0.5f; with kMaxCandidatesPerChunk == 8192 this positive scale gives exactly
    // 0.5 * 8192 / 16384 == 0.25. Both CPU std::lround and GPU round therefore
    // yield zero candidates for every chunk, without a rounding tie.
    pass.set_density_scale(1.0f / 16384.0f);
    Rebuild(pass, chunks, 5);
    glFinish();
    Rebuild(pass, chunks, 6);
    ASSERT_TRUE(pass.frame_instance_count_known());
    EXPECT_EQ(pass.frame_instance_count(), 0u);
    pass.execute(context, camera);
    EXPECT_EQ(pass.last_draw_calls(), 1u); // The indirect command owns the zero count.
    pass.set_density_scale(1.6f);
    Rebuild(pass, chunks, 7);
    ASSERT_FALSE(GpuRecords().empty());
    EXPECT_TRUE(pass.instances().empty());
    pass.execute(context, camera);
    EXPECT_EQ(pass.last_draw_calls(), 1u);
    glFinish();
    Rebuild(pass, chunks, 8);
    EXPECT_FALSE(pass.instances().empty());
    EXPECT_EQ(pass.frame_instance_count_generation(), pass.build_generation());
    // Empty input clears the draw guard even with a previously held snapshot.
    chunks.clear();
    Rebuild(pass, chunks, 9);
    pass.execute(context, camera);
    EXPECT_EQ(pass.last_draw_calls(), 0u);
}

TEST_F(FoliageRebuildTest, OutdatedRevisionReadbackIsDiscardedBeforeAdoption) {
    ScopedPass scoped;
    auto& pass = scoped.pass;
    ASSERT_NO_FATAL_FAILURE(Initialize(pass));
    pass.use_rendered_ground();
    pass.set_readback_enabled(true);
    const auto chunks = SupportedChunks(1);
    UploadGround(pass, 1, {0, 0, 0});
    Rebuild(pass, chunks, 1);
    const auto outdated_generation = pass.build_generation();
    // Finish commands without consuming the public asynchronous result. This
    // makes readiness deterministic; no timing value participates in the test.
    glFinish();
    ASSERT_FALSE(GpuRecords().empty());
    ASSERT_TRUE(pass.instances().empty());
    UploadGround(pass, 1, {0, 0, 0}, 6);
    const auto changed = RebuildAttributed(pass, chunks, 2);
    EXPECT_EQ(changed.stale_results, 1u);
    EXPECT_EQ(pass.build_generation(), outdated_generation + 1);
    EXPECT_TRUE(pass.instances().empty());
    EXPECT_NE(pass.frame_instance_count_generation(), outdated_generation);
    glFinish();
    Rebuild(pass, chunks, 3);
    ASSERT_FALSE(pass.instances().empty());
    EXPECT_EQ(pass.frame_instance_count_generation(), pass.build_generation());
    for (const auto& record : pass.instances())
        EXPECT_LT(record.pos[1], 8.0f);
}
} // namespace
