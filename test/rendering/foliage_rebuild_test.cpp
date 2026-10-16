#include "foliage_rebuild_fixture.h"

namespace FoliageTest {

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
        if (target == GL_SHADER_STORAGE_BUFFER && data == nullptr) {
            ++current->scratch_allocations;
        }
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
} // namespace FoliageTest
