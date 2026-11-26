#include "../../tools/active_regions_bench.h"
#include "../support/GameSessionTestWorld.h"

#include "luminumbra_common/core/JobSystem.h"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <string>

namespace {
namespace Measure = Luminumbra::RegionMeasurement;
using Luminumbra::world::ActiveRegionLedger;

TEST(ActiveRegionScale, SmallLedgerPreservesScheduleAndEncodedBytes) {
    for (const auto count : {8u, 32u}) {
        auto measured = Measure::MakeLedger(count);
        auto control = measured;
        std::uint64_t evictions = 0, transitions = 0;
        bool frozen = false;
        for (std::size_t index = 0; index < Measure::kSchedulerPhases.size(); ++index) {
            SCOPED_TRACE(Measure::kSchedulerPhases[index]);
            const auto sample = Measure::MeasurePhase(measured, index);
            const auto expected = Measure::SchedulePhase(control, index);
            ASSERT_NE(measured.last_schedule(), nullptr);
            EXPECT_EQ(*measured.last_schedule(), expected);
            EXPECT_EQ(measured.canonical_bytes(), control.canonical_bytes());
            EXPECT_EQ(measured.encode(), control.encode());
            EXPECT_EQ(sample.digest, expected.digest);
            EXPECT_EQ(sample.records, sample.populated_records + sample.empty_unowned_records);
            EXPECT_EQ(sample.populated_records, count / 2);
            EXPECT_GT(sample.encoded_bytes, 0u);
            transitions += sample.transitions;
            evictions += sample.evicted;
            for (const auto& [key, record] : measured.records()) {
                (void)key;
                frozen = frozen || record.state == Luminumbra::world::RegionState::Frozen;
            }
            ActiveRegionLedger restored;
            std::string error;
            ASSERT_TRUE(ActiveRegionLedger::decode(measured.encode(), restored, error)) << error;
            EXPECT_EQ(restored.encode(), measured.encode());
        }
        EXPECT_TRUE(frozen);
        EXPECT_GE(transitions, 2u);
        EXPECT_GT(evictions, 0u);
        EXPECT_EQ(measured.records().size(), count / 2);
    }
}

TEST(ActiveRegionScale, UnchangedPrioritiesAndSparseSaveHaveBoundedWork) {
    auto ledger = Measure::MakeLedger(32);
    EXPECT_GT(Measure::MeasurePhase(ledger, 0).priority_comparisons, 0u);
    EXPECT_EQ(Measure::MeasurePhase(ledger, 1).priority_comparisons, 0u);
    const Luminumbra::TestSupport::HeadlessRoot root;
    Luminumbra::JobSystem jobs;
    jobs.startup(1);
    Luminumbra::world::GameSession session;
    session.SetRootPath(root.root_string());
    session.SetJobSystem(&jobs);
    session.SetActiveRegionsEnabled(true);
    session.SetWaterHighResEnabled(false);
    session.SetWeatherRainEnabled(false);
    ASSERT_TRUE(session.CreateWorld("Scale regression", "1337", "default"));
    const auto chunks = Measure::PopulateSave(session, 4, 32);
    const auto first = Measure::MeasureSave(session, 4);
    EXPECT_EQ(first.work.owners_serialized, 4u);
    EXPECT_EQ(first.work.owners_rewritten, 4u);
    EXPECT_GT(first.work.bytes_compressed, 0u);
    const auto unchanged = Measure::MeasureSave(session, 4);
    EXPECT_EQ(unchanged.work.chunk_decodes, 0u);
    EXPECT_EQ(unchanged.work.owners_serialized, 0u);
    EXPECT_EQ(unchanged.work.owners_rewritten, 0u);
    EXPECT_EQ(unchanged.work.bytes_compressed, 0u);
    chunks.front()->sdf_data.front() -= 0.25f;
    chunks.front()->mark_voxel_data_dirty();
    const auto edited = Measure::MeasureSave(session, 4);
    EXPECT_LE(edited.work.world_decodes, 1u);
    EXPECT_EQ(edited.work.chunk_decodes, 0u);
    EXPECT_EQ(edited.work.regions_rewritten, 1u);
    EXPECT_EQ(edited.work.owners_rewritten, 2u);
    EXPECT_EQ(edited.work.owners_serialized, 1u + edited.work.clean_siblings_serialized);
    EXPECT_LE(edited.work.clean_siblings_serialized, 1u);
    EXPECT_GT(edited.work.bytes_compressed, 0u);
    // Snapshots may visit residents, but may not grow with saved history or repeat unboundedly.
    for (const auto& sample : {first, unchanged, edited}) {
        EXPECT_EQ(sample.chunks, 4u);
        EXPECT_GE(sample.ledger_records, 32u);
        EXPECT_GT(sample.ledger_encoded_bytes, 0u);
        EXPECT_LE(sample.snapshot_copies, 8u * sample.chunks);
        EXPECT_EQ(sample.payload_value_bytes, 4u * (Measure::kSdfValues + 3) * sizeof(float));
    }
    const auto restored = Luminumbra::Persistence::WorldSaveService::read_chunk(
        session.GetWorldSaveDir(), chunks.front()->get_coords(), true);
    ASSERT_EQ(restored.status, Luminumbra::Persistence::WorldSaveService::ChunkReadStatus::Found);
    ASSERT_NE(restored.chunk, nullptr);
    EXPECT_EQ(restored.chunk->sdf_data, chunks.front()->sdf_data);
}

TEST(ActiveRegionScale, InvalidFixtureSizesRefuseBeforeConstruction) {
    EXPECT_EQ(Measure::ParseSize("0"), 0u);
    EXPECT_EQ(Measure::ParseSize("524288"), Measure::kMaxLedgerRecords);
    EXPECT_THROW(Measure::ParseSize(std::string_view{}), std::invalid_argument);
    for (const auto* text : {"", "-1", "+1", "1e38", "4294967296", "1x", " 1"}) {
        EXPECT_THROW(Measure::ParseSize(text), std::invalid_argument);
    }
    for (const auto count : {0u, Measure::kMaxLedgerRecords + 1}) {
        EXPECT_THROW(Measure::MakeLedger(count), std::invalid_argument);
    }
    Luminumbra::world::GameSession session;
    for (const auto count : {0u, Measure::kMaxPayloadChunks + 1}) {
        EXPECT_THROW(Measure::PopulateSave(session, count), std::invalid_argument);
    }
    EXPECT_THROW(Measure::PopulateSave(session, 1), std::invalid_argument);
    EXPECT_THROW(Measure::MeasureSave(session, 1), std::invalid_argument);
    auto ledger = Measure::MakeLedger(1);
    EXPECT_THROW(Measure::MeasurePhase(ledger, 6), std::invalid_argument);
    ActiveRegionLedger empty;
    EXPECT_THROW(Measure::MeasurePhase(empty, 0), std::invalid_argument);
}
} // namespace
