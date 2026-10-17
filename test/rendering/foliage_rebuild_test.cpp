#include "foliage_rebuild_fixture.h"

#include "../support/DeterministicWorkBudget.h"
#include "luminumbra/core/Types.h"
#include "luminumbra_client/app/FrameAttribution.h"
#include "luminumbra_client/rendering/Camera.h"
#include "luminumbra_client/rendering/FoliageGroundMesh.h"
#include "luminumbra_client/rendering/RenderContext.h"
#include "luminumbra_client/rendering/RenderResourceHandles.h"
#include "luminumbra_client/rendering/Shader.h"

#include <GLFW/glfw3.h>
#include <glad/glad.h>
#include <glm/ext/vector_float3.hpp>
#include <glm/ext/vector_float4.hpp>
#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <span>
#include <vector>

namespace FoliageTest {

// Count the public GL entry points, forwarding every call to the real context.
class RebuildGlCalls {
public:
    RebuildGlCalls() {
        current = this;
        glad_glBufferData = BufferData;
        glad_glGetUniformLocation = UniformLocation;
        glad_glFenceSync = FenceSync;
    }
    ~RebuildGlCalls() {
        glad_glBufferData = original_buffer_data;
        glad_glGetUniformLocation = original_uniform_location;
        glad_glFenceSync = original_fence_sync;
        current = nullptr;
    }
    RebuildGlCalls(const RebuildGlCalls&) = delete;
    RebuildGlCalls& operator=(const RebuildGlCalls&) = delete;
    void reset_scratch_allocations() {
        m_scratch_allocations = 0;
    }
    std::size_t scratch_allocations() const {
        return m_scratch_allocations;
    }
    std::size_t uniform_queries() const {
        return m_uniform_queries;
    }
    std::size_t fences() const {
        return m_fences;
    }

private:
    static void GLAPIENTRY BufferData(GLenum target,
                                      GLsizeiptr size,
                                      const void* data,
                                      GLenum usage) {
        if (target == GL_SHADER_STORAGE_BUFFER && data == nullptr) {
            ++current->m_scratch_allocations;
        }
        current->original_buffer_data(target, size, data, usage);
    }
    static GLint GLAPIENTRY UniformLocation(GLuint program, const GLchar* name) {
        ++current->m_uniform_queries;
        return current->original_uniform_location(program, name);
    }
    static GLsync GLAPIENTRY FenceSync(GLenum condition, GLbitfield flags) {
        ++current->m_fences;
        return current->original_fence_sync(condition, flags);
    }
    inline static RebuildGlCalls* current = nullptr;
    mutable std::size_t m_scratch_allocations = 0;
    mutable std::size_t m_uniform_queries = 0;
    mutable std::size_t m_fences = 0;
    PFNGLBUFFERDATAPROC original_buffer_data = glad_glBufferData;
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
    std::size_t polls() const {
        return m_polls;
    }

private:
    static GLenum GLAPIENTRY Wait(GLsync, GLbitfield, GLuint64 timeout) {
        EXPECT_EQ(timeout, 0u);
        ++current->m_polls;
        return current->m_result;
    }
    inline static ScopedReadbackNotReady* current = nullptr;
    mutable std::size_t m_polls = 0;
    PFNGLCLIENTWAITSYNCPROC m_original_wait = glad_glClientWaitSync;
    GLenum m_result;
};

TEST_F(FoliageRebuildTest, CompletedCountIsUnknownAcrossOrdinaryAndFrozenGenerationRollover) {
    for (const bool frozen : {false, true}) {
        SCOPED_TRACE(frozen);
        ScopedPass scoped;
        auto& pass = scoped.pass();
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
            const Attribution::FoliageFrameScope frame(true);
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
            const ScopedReadbackNotReady unavailable(readiness);
            const Attribution::FoliageFrameScope frame(true);
            Rebuild(pass, chunks, frame_id++);
            EXPECT_GT(unavailable.polls(), 0u);
            EXPECT_EQ(pass.build_generation(), first_generation + 1);
            EXPECT_FALSE(pass.frame_instance_count_known());
            EXPECT_EQ(pass.frame_instance_count(), 0u);
            EXPECT_FALSE(frame.snapshot().count_known);
            EXPECT_EQ(frame.snapshot().rebuild.copied_bytes, 0u);
        }
        {
            const Attribution::FoliageFrameScope frame(true);
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
            const ScopedReadbackNotReady unavailable(GL_TIMEOUT_EXPIRED);
            const Attribution::FoliageFrameScope frame(true);
            Rebuild(pass, chunks, frame_id);
            EXPECT_GT(unavailable.polls(), 0u);
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
        auto& pass = scoped.pass();
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
            const Attribution::FoliageFrameScope frame(true);
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
        auto& pass = scoped.pass();
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
        auto& pass = scoped.pass();
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
            const RebuildGlCalls calls;
            pass.set_wind({1, 0});
            Rebuild(pass, chunks, 4);
            Rebuild(pass, chunks, 5);
            EXPECT_EQ(calls.fences(), 0u);
            EXPECT_EQ(pass.build_generation(), generation + 1);
            ++generation;
        }
        pass.set_readback_enabled(true);
        {
            const ScopedReadbackNotReady unavailable(GL_TIMEOUT_EXPIRED);
            const RebuildGlCalls calls;
            Rebuild(pass, chunks, 6);
            EXPECT_EQ(calls.fences(), 1u);
            EXPECT_GT(unavailable.polls(), 0u);
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
    auto& pass = scoped.pass();
    ASSERT_NO_FATAL_FAILURE(Initialize(pass));
    const auto chunks = Chunks(2);
    // Default evidence is off, even while attribution is explicitly active.
    {
        const Attribution::FoliageFrameScope frame(true);
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
        const Attribution::FoliageFrameScope frame(true);
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
        const Attribution::FoliageFrameScope frame(true);
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
    ASSERT_NO_FATAL_FAILURE(Initialize(reused.pass()));
    reused.pass().set_evidence_enabled(true);
    std::size_t high_water_columns = 0;
    GLuint reused_count_buffer = 0;
    std::uint64_t frame_id = 1;
    for (const std::size_t columns : {1u, 3u, 2u, 0u, 1u, 3u}) {
        SCOPED_TRACE(columns);
        const auto chunks = Chunks(columns);
        {
            const RebuildGlCalls calls;
            const Attribution::FoliageFrameScope frame(true);
            Rebuild(reused.pass(), chunks, frame_id++);
            const bool grows = columns > high_water_columns;
            EXPECT_EQ(calls.scratch_allocations(), grows ? 2u : 0u);
            EXPECT_EQ(calls.uniform_queries(), 0u);
            EXPECT_EQ(frame.snapshot().rebuild.scratch_bytes,
                      grows ? 2u * columns * 128u * sizeof(GLuint) : 0u);
        }
        high_water_columns = std::max(high_water_columns, columns);
        if (columns == 0) {
            EXPECT_EQ(reused.pass().frame_instance_count(), 0u);
            EXPECT_FALSE(reused.pass().uploaded_input_hash());
            std::array<GLuint, 5> command{};
            glGetNamedBufferSubData(reused_count_buffer, 0, sizeof(command), command.data());
            EXPECT_EQ(command, (std::array<GLuint, 5>{0, 12, 0, 0, 0}));
            continue;
        }
        reused_count_buffer = Binding(3);
        const auto reused_records = GpuRecords();
        const auto reused_hash = reused.pass().uploaded_input_hash();
        EXPECT_EQ(reused_hash, LegacyUploadedHash(reused.pass()));
        GLint64 storage_size = 0;
        glGetNamedBufferParameteri64v(Binding(5), GL_BUFFER_SIZE, &storage_size);
        EXPECT_EQ(storage_size, static_cast<GLint64>(high_water_columns * 128u * sizeof(GLuint)));
        ScopedPass fresh;
        ASSERT_NO_FATAL_FAILURE(Initialize(fresh.pass()));
        fresh.pass().set_evidence_enabled(true);
        Rebuild(fresh.pass(), chunks, frame_id);
        EXPECT_EQ(reused_hash, fresh.pass().uploaded_input_hash());
        ExpectSame(reused_records, GpuRecords());
    }
}

TEST_F(FoliageRebuildTest, ComputeRecreationRefreshesUniformsAndScratchStorage) {
    ScopedPass scoped;
    auto& pass = scoped.pass();
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
        EXPECT_EQ(calls.uniform_queries(), 12u);
        calls.reset_scratch_allocations();
        pass.set_evidence_frame(2, 2);
        pass.rebuild_instances(
            chunks,
            +[](void*, float, float) { return FoliagePass::SurfaceSample{0, 0, 0, false}; },
            nullptr,
            kCamera); // Cached exact surface inputs must be reused.
        EXPECT_EQ(calls.scratch_allocations(), 2u);
        EXPECT_EQ(calls.uniform_queries(), 12u);
    }
    EXPECT_EQ(pass.uploaded_input_hash(), hash);
    ExpectSame(reference, GpuRecords());
}

TEST_F(FoliageRebuildTest, CpuFallbackInstanceBytesDoNotDependOnEvidence) {
    ScopedPass scoped;
    auto& pass = scoped.pass();
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
    auto& pass = scoped.pass();
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
    // Exercise palette-only reloads as well as simultaneous column growth/shrink.
    // Two backends, two input sequences and three reloads bound this fixture.
    for (const bool change_columns : {false, true}) {
        SCOPED_TRACE(change_columns);
        for (const bool gpu : {false, true}) {
            SCOPED_TRACE(gpu);
            ScopedPass reused;
            ASSERT_NO_FATAL_FAILURE(Initialize(reused.pass(), gpu));
            reused.pass().set_evidence_enabled(true);
            const auto root = std::filesystem::path(LUMINUMBRA_SOURCE_ROOT);
            const auto full_palette = root / "data/common/foliage/scatter_set.json";
            const auto small_palette = root / "test/rendering/fixtures/foliage_palette_small.json";
            auto chunks = Chunks(2);
            std::uint64_t frame_id = 1;
            Rebuild(reused.pass(), chunks, frame_id++);
            for (const std::size_t columns : {2u, 1u, 2u}) {
                const auto& palette = columns == 1 ? small_palette : full_palette;
                chunks = Chunks(change_columns ? columns : 2u);
                const auto generation = reused.pass().build_generation();
                ASSERT_TRUE(reused.pass().load_scatter_set(palette));
                Rebuild(reused.pass(), chunks, frame_id++);
                EXPECT_EQ(reused.pass().build_generation(), generation + 1);
                const auto records = Records(reused.pass(), gpu);
                ASSERT_FALSE(records.empty());
                const auto hash = reused.pass().uploaded_input_hash();
                if (gpu) {
                    EXPECT_EQ(hash, LegacyUploadedHash(reused.pass()));
                    GLint64 palette_size = 0;
                    glGetNamedBufferParameteri64v(Binding(4), GL_BUFFER_SIZE, &palette_size);
                    EXPECT_EQ(
                        palette_size,
                        static_cast<GLint64>(reused.pass().archetype_count() * 8 * sizeof(float)));
                }
                ScopedPass fresh;
                ASSERT_NO_FATAL_FAILURE(Initialize(fresh.pass(), gpu));
                ASSERT_TRUE(fresh.pass().load_scatter_set(palette));
                fresh.pass().set_evidence_enabled(true);
                Rebuild(fresh.pass(), chunks, frame_id);
                EXPECT_EQ(hash, fresh.pass().uploaded_input_hash());
                ExpectSame(records, Records(fresh.pass(), gpu));
            }
        }
    }
}

TEST_F(FoliageRebuildTest, DistantAndZeroDensityUploadsDoNotRebuildEitherBackend) {
    for (const bool gpu : {false, true}) {
        SCOPED_TRACE(gpu);
        ScopedPass scoped;
        auto& pass = scoped.pass();
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
        auto& pass = scoped.pass();
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
                EXPECT_EQ(std::memcmp(std::as_bytes(std::span(&original[index], 1)).data(),
                                      std::as_bytes(std::span(&changed[index], 1)).data(),
                                      sizeof(changed[index])),
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
        auto& pass = scoped.pass();
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
        auto& pass = scoped.pass();
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
        auto& pass = scoped.pass();
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
        auto& pass = scoped.pass();
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
        auto& pass = scoped.pass();
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
        auto& pass = scoped.pass();
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
        auto& pass = scoped.pass();
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
                EXPECT_EQ(*rebuilt_hash, LegacyUploadedHash(pass));
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

TEST_F(FoliageRebuildTest, DeterministicWorkBudgetMovementEditDrainAndIdle) {
    using namespace Luminumbra::TestSupport;
    for (const bool gpu : {false, true}) {
        SCOPED_TRACE(gpu);
        ScopedPass scoped;
        auto& pass = scoped.pass();
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
            const auto work = RebuildAttributed(pass, chunks, ++frame_id, camera);
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
        EXPECT_EQ(moved.uploads, gpu ? 4u : 1u);
        EXPECT_FALSE(moved.backlog);
        // A real support revision must invalidate exactly one column.
        UploadGround(pass, 1, {0, 0, 0}, 6);
        const auto edited = measure(moved_camera, 1);
        EXPECT_EQ(edited.surface_misses, 1u);
        EXPECT_GT(edited.surface_samples, 0u);
        EXPECT_GT(edited.uploaded_bytes, 0u);
        EXPECT_EQ(edited.uploads, gpu ? 4u : 1u);
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
class ScopedDrawTarget {
public:
    GLuint framebuffer() const {
        return m_framebuffer;
    }
    ScopedDrawTarget() {
        glGenRenderbuffers(1, &m_color);
        glBindRenderbuffer(GL_RENDERBUFFER, m_color);
        glRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA8, 64, 64);
        glGenFramebuffers(1, &m_framebuffer);
        glBindFramebuffer(GL_FRAMEBUFFER, m_framebuffer);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, m_color);
    }
    ~ScopedDrawTarget() {
        glDeleteFramebuffers(1, &m_framebuffer);
        glDeleteRenderbuffers(1, &m_color);
    }

private:
    GLuint m_framebuffer = 0;
    GLuint m_color = 0;
};

TEST_F(FoliageRebuildTest, GpuDrawsSurviveChurnBeforeFirstSnapshotAndAfterEmptySnapshot) {
    ScopedPass scoped;
    auto& pass = scoped.pass();
    ASSERT_NO_FATAL_FAILURE(Initialize(pass));
    pass.init_shader(LUMINUMBRA_SOURCE_ROOT);
    ASSERT_TRUE(pass.shader()->IsValid());
    pass.set_readback_enabled(true);
    const ScopedDrawTarget target;
    ASSERT_EQ(glCheckFramebufferStatus(GL_FRAMEBUFFER),
              static_cast<GLenum>(GL_FRAMEBUFFER_COMPLETE));
    Luminumbra::Rendering::RenderContext context;
    context.screen_width = context.screen_height = 64;
    context.lit_scene = Luminumbra::Rendering::FboHandle{target.framebuffer()};
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
    auto& pass = scoped.pass();
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
} // namespace FoliageTest
