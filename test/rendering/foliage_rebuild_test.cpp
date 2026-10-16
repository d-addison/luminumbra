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

namespace {
using Luminumbra::Rendering::FoliagePass;
namespace Attribution = Luminumbra::Client::Attribution;

const glm::vec3 kCamera{16, 12, 16};

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
        if (initialized)
            glfwTerminate();
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
                        std::uint64_t frame) {
        pass.set_evidence_frame(frame, 1);
        pass.rebuild_instances(
            chunks,
            +[](void*, float x, float z) {
                return FoliagePass::SurfaceSample{10.0f + x * 0.01f + z * 0.02f, 0.1f, 0.7f, true};
            },
            nullptr,
            kCamera);
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
        if (!records.empty())
            glGetNamedBufferSubData(Binding(2),
                                    0,
                                    static_cast<GLsizeiptr>(records.size() * sizeof(records[0])),
                                    records.data());
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
        for (std::size_t offset = 0; offset < size; ++offset)
            hash = (hash ^ bytes[offset]) * 1099511628211ull;
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
            if (size <= 0 || size > 3 * 8192 * 16)
                return 0;
            std::vector<unsigned char> bytes(static_cast<std::size_t>(size));
            glGetNamedBufferSubData(buffer, 0, static_cast<GLsizeiptr>(size), bytes.data());
            hash = Fnv(bytes.data(), bytes.size(), hash);
        }
        float total_weight = 0;
        for (std::size_t index = 0; index < pass.archetype_count(); ++index)
            total_weight += pass.archetype(index).density_weight;
        if (total_weight <= 0)
            total_weight = 1;
        for (const float value :
             {kCamera.x, kCamera.z, pass.density_scale(), pass.fade_end_m(), total_weight})
            hash = Fnv(&value, sizeof(value), hash);
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
        glad_glGetUniformLocation = UniformLocation;
    }
    ~RebuildGlCalls() {
        glad_glBufferData = original_buffer_data;
        glad_glGetUniformLocation = original_uniform_location;
        current = nullptr;
    }
    RebuildGlCalls(const RebuildGlCalls&) = delete;
    RebuildGlCalls& operator=(const RebuildGlCalls&) = delete;
    std::size_t scratch_allocations = 0;
    std::size_t uniform_queries = 0;

private:
    static void GLAPIENTRY BufferData(GLenum target,
                                      GLsizeiptr size,
                                      const void* data,
                                      GLenum usage) {
        if (target == GL_SHADER_STORAGE_BUFFER && data == nullptr)
            ++current->scratch_allocations;
        current->original_buffer_data(target, size, data, usage);
    }
    static GLint GLAPIENTRY UniformLocation(GLuint program, const GLchar* name) {
        ++current->uniform_queries;
        return current->original_uniform_location(program, name);
    }
    inline static RebuildGlCalls* current = nullptr;
    PFNGLBUFFERDATAPROC original_buffer_data = glad_glBufferData;
    PFNGLGETUNIFORMLOCATIONPROC original_uniform_location = glad_glGetUniformLocation;
};

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
    const auto expected_hash = LegacyUploadedHash(pass);
    const auto generation = pass.build_generation();

    // Enabling evidence on cached inputs hashes once without a new build/upload.
    pass.set_evidence_enabled(true);
    {
        Attribution::FoliageFrameScope frame(true);
        Rebuild(pass, chunks, 2);
        const auto cached_hash = pass.uploaded_input_hash();
        ASSERT_TRUE(cached_hash);
        if (!cached_hash)
            return;
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
    if (!rebuilt_hash)
        return;
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
            EXPECT_EQ(calls.scratch_allocations, grows ? 2u : 0u);
            EXPECT_EQ(calls.uniform_queries, 0u);
            EXPECT_EQ(frame.snapshot().rebuild.scratch_bytes,
                      grows ? 2u * columns * 128u * sizeof(GLuint) : 0u);
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
        EXPECT_EQ(reused_hash, LegacyUploadedHash(reused.pass));
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
    const auto hash = pass.uploaded_input_hash();
    pass.destroy_compute();
    EXPECT_FALSE(pass.uploaded_input_hash());
    {
        RebuildGlCalls calls;
        pass.init_compute(LUMINUMBRA_SOURCE_ROOT);
        ASSERT_TRUE(pass.gpu_scatter_active());
        EXPECT_EQ(calls.uniform_queries, 12u);
        calls.scratch_allocations = 0;
        pass.set_evidence_frame(2, 2);
        pass.rebuild_instances(
            chunks,
            +[](void*, float, float) { return FoliagePass::SurfaceSample{0, 0, 0, false}; },
            nullptr,
            kCamera); // Cached exact surface inputs must be reused.
        EXPECT_EQ(calls.scratch_allocations, 2u);
        EXPECT_EQ(calls.uniform_queries, 12u);
    }
    EXPECT_EQ(pass.uploaded_input_hash(), hash);
    ExpectSame(reference, GpuRecords());
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
    const auto expected_hash = LegacyUploadedHash(pass);
    const std::array<float, 2> wind{0, 0};
    pass.set_evidence_enabled(true);
    Rebuild(pass, chunks, 5);
    ASSERT_TRUE(pass.begin_qualification());
    EXPECT_EQ(pass.qualification_evidence().rebuild.input_hash,
              Fnv(wind.data(), sizeof(wind), expected_hash));
}

TEST_F(FoliageRebuildTest, PaletteReloadUsesOnlyCurrentArchetypeBytes) {
    ScopedPass reused;
    ASSERT_NO_FATAL_FAILURE(Initialize(reused.pass));
    reused.pass.set_evidence_enabled(true);
    const auto root = std::filesystem::path(LUMINUMBRA_SOURCE_ROOT);
    const auto full_palette = root / "data/common/foliage/scatter_set.json";
    const auto small_palette = root / "test/rendering/fixtures/foliage_palette_small.json";
    std::uint64_t frame_id = 1;
    for (const std::size_t columns : {2u, 1u, 2u}) {
        const auto& palette = columns == 1 ? small_palette : full_palette;
        ASSERT_TRUE(reused.pass.load_scatter_set(palette));
        const auto chunks = Chunks(columns); // Changed input forces the existing rebuild policy.
        Rebuild(reused.pass, chunks, frame_id++);
        const auto records = GpuRecords();
        ASSERT_FALSE(records.empty());
        const auto hash = reused.pass.uploaded_input_hash();
        EXPECT_EQ(hash, LegacyUploadedHash(reused.pass));
        GLint64 palette_size = 0;
        glGetNamedBufferParameteri64v(Binding(4), GL_BUFFER_SIZE, &palette_size);
        EXPECT_EQ(palette_size,
                  static_cast<GLint64>(reused.pass.archetype_count() * 8 * sizeof(float)));
        ScopedPass fresh;
        ASSERT_NO_FATAL_FAILURE(Initialize(fresh.pass));
        ASSERT_TRUE(fresh.pass.load_scatter_set(palette));
        fresh.pass.set_evidence_enabled(true);
        Rebuild(fresh.pass, chunks, frame_id);
        EXPECT_EQ(hash, fresh.pass.uploaded_input_hash());
        ExpectSame(records, GpuRecords());
    }
}
} // namespace
