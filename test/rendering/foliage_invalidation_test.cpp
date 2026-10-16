#include "foliage_rebuild_fixture.h"

#include <cmath>

namespace FoliageTest {

const glm::vec3 kExcludedCamera{17, 12, 8};
const glm::vec3 kEnteredCamera{31, 12, 8};
constexpr float kNarrowRadius = 22;
constexpr float kWideRadius = 38;

class FoliageInvalidationTest : public FoliageRebuildTest {
protected:
    struct Support {
        Luminumbra::ChunkID id;
        glm::ivec3 coords;
        float height;
    };
    enum class SupportEdit {
        Add,
        Remove,
        Modify
    };

    static void UploadSupports(FoliagePass& pass, const std::vector<Support>& supports) {
        for (const auto& support : supports) {
            UploadGround(pass, support.id, support.coords, support.height);
        }
    }
    static void ExpectFreshEquivalent(FoliagePass& incremental,
                                      bool gpu,
                                      const std::vector<FoliagePass::ChunkScatter>& chunks,
                                      const std::vector<Support>& final_supports,
                                      const glm::vec3& camera = kCamera) {
        // Capture before initializing another pass changes the GL SSBO bindings.
        const auto incremental_records = Records(incremental, gpu);
        const auto incremental_hash = incremental.uploaded_input_hash();
        ASSERT_FALSE(incremental_records.empty());

        // This pass has never seen the old support or either column cache. Only
        // the final meshes are uploaded; no evidence-phase rebuild is used.
        ScopedPass fresh;
        ASSERT_NO_FATAL_FAILURE(Initialize(fresh.pass, gpu));
        EXPECT_EQ(fresh.pass.build_generation(), 0u);
        fresh.pass.use_rendered_ground();
        fresh.pass.set_evidence_enabled(true);
        fresh.pass.set_fade_distances(incremental.fade_start_m(), incremental.fade_end_m());
        fresh.pass.set_density_scale(incremental.density_scale());
        fresh.pass.set_wind(incremental.wind());
        UploadSupports(fresh.pass, final_supports);
        const auto rebuilt = RebuildAttributed(fresh.pass, chunks, 1, camera);
        EXPECT_EQ(rebuilt.reasons, Attribution::Initial);
        EXPECT_EQ(rebuilt.path,
                  gpu ? Attribution::FoliagePath::Gpu : Attribution::FoliagePath::Cpu);
        EXPECT_FALSE(rebuilt.backlog);
        ExpectSame(incremental_records, Records(fresh.pass, gpu));
        if (gpu) {
            ASSERT_TRUE(incremental_hash.has_value());
            ASSERT_TRUE(fresh.pass.uploaded_input_hash().has_value());
            EXPECT_EQ(incremental_hash, fresh.pass.uploaded_input_hash());
        } else {
            EXPECT_FALSE(incremental_hash.has_value());
            EXPECT_FALSE(fresh.pass.uploaded_input_hash().has_value());
        }
    }
    static void ExpectEditedSupportMatchesFresh(SupportEdit edit) {
        for (const bool gpu : {false, true}) {
            SCOPED_TRACE(gpu);
            ScopedPass scoped;
            auto& pass = scoped.pass;
            ASSERT_NO_FATAL_FAILURE(Initialize(pass, gpu));
            pass.use_rendered_ground();
            pass.set_evidence_enabled(true);
            const auto chunks = SupportedChunks(2);
            UploadGround(pass, 1, {0, 0, 0});
            if (edit != SupportEdit::Add) {
                UploadGround(pass, 2, {1, 0, 0});
            }
            RebuildAttributed(pass, chunks, 1);
            const auto before = Records(pass, gpu);
            ASSERT_FALSE(before.empty());
            const auto before_hash = pass.uploaded_input_hash();
            const auto generation = pass.build_generation();

            if (edit == SupportEdit::Remove) {
                pass.remove_ground_mesh(2);
            } else {
                UploadGround(pass, 2, {1, 0, 0}, edit == SupportEdit::Modify ? 6.0f : 8.0f);
            }
            const auto edited = RebuildAttributed(pass, chunks, 2);
            EXPECT_EQ(edited.reasons, Attribution::GroundRevision);
            EXPECT_EQ(edited.path,
                      gpu ? Attribution::FoliagePath::Gpu : Attribution::FoliagePath::Cpu);
            EXPECT_EQ(edited.surface_misses, 1u);
            EXPECT_EQ(edited.admitted_columns, 2u);
            EXPECT_FALSE(edited.backlog);
            EXPECT_EQ(pass.build_generation(), generation + 1);
            const auto after = Records(pass, gpu);
            ASSERT_FALSE(after.empty());
            EXPECT_NE(Fnv(before.data(), before.size() * sizeof(before[0])),
                      Fnv(after.data(), after.size() * sizeof(after[0])));
            if (gpu) {
                ASSERT_TRUE(before_hash.has_value());
                ASSERT_TRUE(pass.uploaded_input_hash().has_value());
                EXPECT_NE(before_hash, pass.uploaded_input_hash());
            }
            ExpectReused(RebuildAttributed(pass, chunks, 3), gpu);

            // At most two support meshes, below both backends' build budgets.
            std::vector<Support> final_supports{{1, {0, 0, 0}, 8}};
            if (edit != SupportEdit::Remove) {
                final_supports.push_back({2, {1, 0, 0}, edit == SupportEdit::Modify ? 6.0f : 8.0f});
            }
            ASSERT_NO_FATAL_FAILURE(ExpectFreshEquivalent(pass, gpu, chunks, final_supports));
        }
    }
    static std::vector<FoliagePass::ChunkScatter> HorizonChunks() {
        // Two columns, below the four-column build budget. Column 1 stays live.
        // Column 3's center is x=56: distance minus extent is 23 at x=17,
        // and 9 at x=31. Both camera positions are in the same 16-metre cell.
        auto chunks = SupportedChunks(2);
        chunks[0].chunk_xz = {1, 0};
        chunks[0].origin.x = kColumnExtent;
        chunks[1].chunk_xz = {3, 0};
        chunks[1].origin.x = 3 * kColumnExtent;
        return chunks;
    }
    static std::vector<Support> HorizonSupports(float edge_height = 8) {
        return {{1, {1, 0, 0}, 8}, {2, {3, 0, 0}, edge_height}};
    }
    static std::size_t EdgeRecordCount(const std::vector<FoliagePass::InstanceRecord>& records) {
        return static_cast<std::size_t>(
            std::count_if(records.begin(), records.end(), [](const auto& record) {
                return record.pos[0] >= 3 * kColumnExtent;
            }));
    }
};

TEST_F(FoliageInvalidationTest, AddedSupportMatchesFreshPassBytesAndGpuInputHash) {
    ASSERT_NO_FATAL_FAILURE(ExpectEditedSupportMatchesFresh(SupportEdit::Add));
}

TEST_F(FoliageInvalidationTest, RemovedSupportMatchesFreshPassBytesAndGpuInputHash) {
    ASSERT_NO_FATAL_FAILURE(ExpectEditedSupportMatchesFresh(SupportEdit::Remove));
}

TEST_F(FoliageInvalidationTest, ModifiedSupportMatchesFreshPassBytesAndGpuInputHash) {
    ASSERT_NO_FATAL_FAILURE(ExpectEditedSupportMatchesFresh(SupportEdit::Modify));
}

TEST_F(FoliageInvalidationTest, ColumnsEnterAndLeaveHorizonWithinOneCameraCell) {
    ASSERT_EQ(std::floor(kExcludedCamera.x / 16), std::floor(kEnteredCamera.x / 16));
    ASSERT_EQ(std::floor(kExcludedCamera.z / 16), std::floor(kEnteredCamera.z / 16));
    for (const bool gpu : {false, true}) {
        SCOPED_TRACE(gpu);
        ScopedPass scoped;
        auto& pass = scoped.pass;
        ASSERT_NO_FATAL_FAILURE(Initialize(pass, gpu));
        pass.use_rendered_ground();
        pass.set_evidence_enabled(true);
        pass.set_fade_distances(12, kNarrowRadius);
        const auto chunks = HorizonChunks();
        const auto supports = HorizonSupports();
        UploadSupports(pass, supports);
        const auto initial = RebuildAttributed(pass, chunks, 1, kExcludedCamera);
        EXPECT_EQ(initial.admitted_columns, 1u);
        const auto excluded = Records(pass, gpu);
        ASSERT_FALSE(excluded.empty());
        EXPECT_EQ(EdgeRecordCount(excluded), 0u);
        const auto excluded_hash = pass.uploaded_input_hash();
        const auto generation = pass.build_generation();

        const auto entered = RebuildAttributed(pass, chunks, 2, kEnteredCamera);
        EXPECT_EQ(entered.reasons, Attribution::GroundRevision | Attribution::ScatterChunks);
        EXPECT_EQ(entered.admitted_columns, 2u);
        EXPECT_EQ(entered.surface_misses, 1u);
        EXPECT_FALSE(entered.backlog);
        EXPECT_EQ(pass.build_generation(), generation + 1);
        EXPECT_GT(EdgeRecordCount(Records(pass, gpu)), 0u);
        ExpectReused(RebuildAttributed(pass, chunks, 3, kEnteredCamera), gpu);
        ASSERT_NO_FATAL_FAILURE(ExpectFreshEquivalent(pass, gpu, chunks, supports, kEnteredCamera));

        const auto left = RebuildAttributed(pass, chunks, 4, kExcludedCamera);
        EXPECT_EQ(left.reasons, Attribution::GroundRevision | Attribution::ScatterChunks);
        EXPECT_EQ(left.admitted_columns, 1u);
        EXPECT_EQ(left.surface_misses, 0u);
        EXPECT_FALSE(left.backlog);
        EXPECT_EQ(pass.build_generation(), generation + 2);
        ExpectSame(excluded, Records(pass, gpu));
        EXPECT_EQ(pass.uploaded_input_hash(), excluded_hash);
        ASSERT_NO_FATAL_FAILURE(
            ExpectFreshEquivalent(pass, gpu, chunks, supports, kExcludedCamera));
    }
}

TEST_F(FoliageInvalidationTest, FadeRadiusExpansionAndContractionChangeContributingColumns) {
    for (const bool gpu : {false, true}) {
        SCOPED_TRACE(gpu);
        ScopedPass scoped;
        auto& pass = scoped.pass;
        ASSERT_NO_FATAL_FAILURE(Initialize(pass, gpu));
        pass.use_rendered_ground();
        pass.set_evidence_enabled(true);
        pass.set_fade_distances(12, kNarrowRadius);
        const auto chunks = HorizonChunks();
        const auto supports = HorizonSupports();
        UploadSupports(pass, supports);
        EXPECT_EQ(RebuildAttributed(pass, chunks, 1, kExcludedCamera).admitted_columns, 1u);
        const auto narrow = Records(pass, gpu);
        ASSERT_FALSE(narrow.empty());
        EXPECT_EQ(EdgeRecordCount(narrow), 0u);
        const auto narrow_hash = pass.uploaded_input_hash();
        const auto generation = pass.build_generation();

        pass.set_fade_distances(12, kWideRadius);
        const auto expanded = RebuildAttributed(pass, chunks, 2, kExcludedCamera);
        EXPECT_EQ(expanded.reasons,
                  Attribution::Config | Attribution::GroundRevision | Attribution::ScatterChunks);
        EXPECT_EQ(expanded.admitted_columns, 2u);
        EXPECT_EQ(expanded.surface_misses, 1u);
        EXPECT_FALSE(expanded.backlog);
        EXPECT_EQ(pass.build_generation(), generation + 1);
        EXPECT_GT(EdgeRecordCount(Records(pass, gpu)), 0u);
        ASSERT_NO_FATAL_FAILURE(
            ExpectFreshEquivalent(pass, gpu, chunks, supports, kExcludedCamera));

        pass.set_fade_distances(12, kNarrowRadius);
        const auto contracted = RebuildAttributed(pass, chunks, 3, kExcludedCamera);
        EXPECT_EQ(contracted.reasons,
                  Attribution::Config | Attribution::GroundRevision | Attribution::ScatterChunks);
        EXPECT_EQ(contracted.admitted_columns, 1u);
        EXPECT_EQ(contracted.surface_misses, 0u);
        EXPECT_FALSE(contracted.backlog);
        EXPECT_EQ(pass.build_generation(), generation + 2);
        ExpectSame(narrow, Records(pass, gpu));
        EXPECT_EQ(pass.uploaded_input_hash(), narrow_hash);
        ExpectReused(RebuildAttributed(pass, chunks, 4, kExcludedCamera), gpu);
        ASSERT_NO_FATAL_FAILURE(
            ExpectFreshEquivalent(pass, gpu, chunks, supports, kExcludedCamera));
    }
}

TEST_F(FoliageInvalidationTest, ExcludedSupportEditIsVisibleWhenColumnReenters) {
    for (const bool gpu : {false, true}) {
        SCOPED_TRACE(gpu);
        ScopedPass scoped;
        auto& pass = scoped.pass;
        ASSERT_NO_FATAL_FAILURE(Initialize(pass, gpu));
        pass.use_rendered_ground();
        pass.set_evidence_enabled(true);
        pass.set_fade_distances(12, kNarrowRadius);
        const auto chunks = HorizonChunks();
        UploadSupports(pass, HorizonSupports());
        EXPECT_EQ(RebuildAttributed(pass, chunks, 1, kEnteredCamera).admitted_columns, 2u);
        const auto original = Records(pass, gpu);
        ASSERT_GT(EdgeRecordCount(original), 0u); // Warm both column caches first.
        EXPECT_EQ(RebuildAttributed(pass, chunks, 2, kExcludedCamera).admitted_columns, 1u);
        const auto excluded = Records(pass, gpu);
        ASSERT_FALSE(excluded.empty());
        const auto excluded_hash = pass.uploaded_input_hash();
        const auto generation = pass.build_generation();

        UploadGround(pass, 2, {3, 0, 0}, 6);
        ExpectReused(RebuildAttributed(pass, chunks, 3, kExcludedCamera), gpu);
        EXPECT_EQ(pass.build_generation(), generation);
        ExpectSame(excluded, Records(pass, gpu));
        EXPECT_EQ(pass.uploaded_input_hash(), excluded_hash);

        const auto reentered = RebuildAttributed(pass, chunks, 4, kEnteredCamera);
        EXPECT_EQ(reentered.reasons, Attribution::GroundRevision | Attribution::ScatterChunks);
        EXPECT_EQ(reentered.admitted_columns, 2u);
        EXPECT_EQ(reentered.surface_misses, 1u);
        EXPECT_FALSE(reentered.backlog);
        EXPECT_EQ(pass.build_generation(), generation + 1);
        const auto changed = Records(pass, gpu);
        ASSERT_EQ(changed.size(), original.size());
        ASSERT_GT(EdgeRecordCount(changed), 0u);
        for (std::size_t index = 0; index < changed.size(); ++index) {
            if (original[index].pos[0] >= 3 * kColumnExtent) {
                EXPECT_NEAR(changed[index].pos[1], original[index].pos[1] - 2, 0.0001f);
            } else {
                EXPECT_EQ(std::memcmp(&original[index], &changed[index], sizeof(changed[index])),
                          0);
            }
        }
        ExpectReused(RebuildAttributed(pass, chunks, 5, kEnteredCamera), gpu);
        ASSERT_NO_FATAL_FAILURE(
            ExpectFreshEquivalent(pass, gpu, chunks, HorizonSupports(6), kEnteredCamera));
    }
}

} // namespace FoliageTest
