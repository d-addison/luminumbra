#include <gtest/gtest.h>

#include "luminumbra_common/fields/FieldPageState.h"
#include "luminumbra_common/fields/FoliageFieldPages.h"
#include "luminumbra_common/systems/RegionalFoliage.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <iterator>
#include <limits>
#include <map>
#include <random>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using namespace Luminumbra::world;
TEST(FoliageFieldPages, ExactUnitsAndInitializedBaselineSurviveTypedSave) {
    FieldPageState state;
    const ScentCellKey negative{-1, -1}, positive{512, 0};
    ASSERT_TRUE(state.set_soil(negative, 999997));
    ASSERT_TRUE(state.set_soil(positive, FieldPageState::kSoilBaseline));
    ASSERT_TRUE(state.set_irrigation(negative, 3999999));
    const auto bytes = state.serialize();
    FieldPageState restored;
    std::string error;
    ASSERT_TRUE(FieldPageState::parse(bytes, restored, error)) << error;
    EXPECT_EQ(restored.serialize(), bytes);
    EXPECT_EQ(restored.soil().sample(negative, 1000000), 999997);
    EXPECT_EQ(restored.irrigation().sample(negative, 0), 3999999);
    EXPECT_EQ(restored.soil().sample({0, 0}, 1000000), 1000000);
    EXPECT_TRUE(restored.owns_region({-1, -1}));
    EXPECT_TRUE(restored.owns_region({1, 0}));
    EXPECT_FALSE(restored.owns_region({0, 0}));
    EXPECT_NE(restored.region_bytes({1, 0}), "");    // baseline still initialized/owned
    EXPECT_EQ(restored.ambient_bytes(31, true), ""); // ambient slots unchanged by foliage
    EXPECT_EQ(restored.region_bytes({-1, -1}), state.region_bytes({-1, -1}));
}
TEST(FoliageFieldPages, RegionOwnershipFollowsTheFiveHundredTwelveMetreBoundaries) {
    FoliageFieldPages pages;
    // Region 0 spans one-metre cells [0, 511]; region -1 spans [-512, -1].
    ASSERT_TRUE(pages.set({511, 0}, 1, 1000000));
    ASSERT_TRUE(pages.set({-512, -1}, 1, 1000000));
    EXPECT_TRUE(pages.owns_region({0, 0}));
    EXPECT_TRUE(pages.owns_region({-1, -1}));
    EXPECT_FALSE(pages.owns_region({1, 0}));
    EXPECT_FALSE(pages.owns_region({0, -1}));
    EXPECT_FALSE(pages.owns_region({-1, 0}));
    ASSERT_TRUE(pages.set({512, 512}, 1, 1000000));
    ASSERT_TRUE(pages.set({-513, 0}, 1, 1000000));
    EXPECT_TRUE(pages.owns_region({1, 1}));
    EXPECT_TRUE(pages.owns_region({-2, 0}));
    EXPECT_FALSE(pages.owns_region({1, 0}));
    // Regions whose page range would overflow int32 own nothing.
    EXPECT_FALSE(pages.owns_region({std::numeric_limits<std::int32_t>::max(), 0}));
    EXPECT_FALSE(pages.owns_region({0, std::numeric_limits<std::int32_t>::min()}));
}
TEST(FoliageFieldPages, StrictRecordValidationLeavesDestinationUntouched) {
    FoliageFieldPages original;
    ASSERT_TRUE(original.set({0, 0}, 10, 1000000));
    const auto record = original.records();
    FoliageFieldPages restored;
    ASSERT_TRUE(FoliageFieldPages::parse_records(record, 1, 1000000, restored));
    const auto prior = restored.records();
    for (const auto offset : {0u, 8u, 40u, 44u}) {
        auto corrupt = record;
        if (offset == 0)
            corrupt.replace(0, 4, std::string(4, '\x7f')); // invalid page
        else if (offset == 8)
            corrupt.replace(8, 32, std::string(32, '\0')); // empty mask
        else
            corrupt[offset] = '\xff'; // initialized overflow or nonzero uninitialized
        EXPECT_FALSE(FoliageFieldPages::parse_records(corrupt, 1, 100, restored));
        EXPECT_EQ(restored.records(), prior);
    }
    EXPECT_FALSE(FoliageFieldPages::parse_records(record + record, 2, 1000000, restored));
    EXPECT_FALSE(FoliageFieldPages::parse_records(
        record, FoliageFieldPages::kMaxPages + 1, 1000000, restored));
    EXPECT_EQ(restored.records(), prior);
}
TEST(FoliageFieldPages, HalfOpenWorldBoundsAndRefusalAreAtomic) {
    FoliageFieldPages pages;
    ASSERT_TRUE(pages.set({-16777216, -16777216}, 1, 1000000));
    ASSERT_TRUE(pages.set({16777215, 16777215}, 0, 1000000));
    const auto before = pages.records();
    EXPECT_FALSE(pages.set({16777216, 0}, 1, 1000000));
    EXPECT_FALSE(pages.set({-16777217, 0}, 1, 1000000));
    EXPECT_FALSE(pages.set({0, 0}, -1, 1000000));
    EXPECT_FALSE(pages.set({0, 0}, 1000001, 1000000));
    EXPECT_EQ(pages.records(), before);
}

TEST(FoliageFieldPages, BaselineHistoryAndClearPreserveInitializedMasks) {
    FieldPageState state;
    const ScentCellKey cell{-17, 16};
    EXPECT_FALSE(state.soil().initialized(cell));
    EXPECT_FALSE(state.irrigation().initialized(cell));
    ASSERT_TRUE(state.set_soil(cell, 1));
    ASSERT_TRUE(state.set_irrigation(cell, FieldPageState::kIrrigationLimit));
    ASSERT_TRUE(state.set_soil(cell, FieldPageState::kSoilBaseline));
    ASSERT_TRUE(state.set_irrigation(cell, 0));
    FieldPageState restored;
    std::string error;
    ASSERT_TRUE(FieldPageState::parse(state.serialize(), restored, error)) << error;
    EXPECT_TRUE(restored.soil().initialized(cell));
    EXPECT_TRUE(restored.irrigation().initialized(cell));
    EXPECT_EQ(restored.soil().sample(cell, -1), FieldPageState::kSoilBaseline);
    EXPECT_EQ(restored.irrigation().sample(cell, -1), 0);
    EXPECT_EQ(restored.soil().pages(), state.soil().pages());
    EXPECT_EQ(restored.irrigation().pages(), state.irrigation().pages());
    EXPECT_TRUE(restored.owns_region({-1, 0}));
    EXPECT_FALSE(restored.empty());
    restored.clear();
    EXPECT_TRUE(restored.empty());
    EXPECT_TRUE(restored.soil().pages().empty());
    EXPECT_TRUE(restored.irrigation().pages().empty());
    EXPECT_FALSE(restored.owns_region({-1, 0}));
    EXPECT_TRUE(restored.serialize().empty());
}

TEST(FoliageFieldPages, PageCapacityRefusesNewPagesButAllowsExistingPageEdits) {
    FoliageFieldPages pages;
    for (std::size_t index = 0; index < FoliageFieldPages::kMaxPages; ++index) {
        ASSERT_TRUE(pages.set({static_cast<std::int32_t>(index) * 16, 0}, 7, 1000000));
    }
    ASSERT_EQ(pages.pages().size(), FoliageFieldPages::kMaxPages);
    const auto before = pages.records();
    EXPECT_FALSE(pages.set({-1, 0}, 3, 1000000));
    EXPECT_EQ(pages.records(), before);
    ASSERT_TRUE(pages.set({0, 0}, 9, 1000000));
    ASSERT_TRUE(pages.set({15, 15}, 0, 1000000));
    EXPECT_EQ(pages.sample({0, 0}, -1), 9);
    EXPECT_TRUE(pages.initialized({15, 15}));
    EXPECT_EQ(pages.pages().size(), FoliageFieldPages::kMaxPages);
    FoliageFieldPages restored;
    ASSERT_TRUE(FoliageFieldPages::parse_records(
        pages.records(), FoliageFieldPages::kMaxPages, 1000000, restored));
    EXPECT_EQ(restored.records(), pages.records());
}

TEST(FoliageFieldPages, RecordLengthsLimitsAndOrderingRefuseAtomically) {
    FoliageFieldPages pages;
    ASSERT_TRUE(pages.set({-1, -1}, 1, 1000000));
    ASSERT_TRUE(pages.set({16, 16}, 1000000, 1000000));
    const auto bytes = pages.records();
    FoliageFieldPages restored;
    ASSERT_TRUE(restored.set({512, 512}, 9, 1000000));
    const auto before = restored.records();
    const auto refuses = [&](const std::string& candidate, std::size_t count, std::int32_t limit) {
        EXPECT_FALSE(FoliageFieldPages::parse_records(candidate, count, limit, restored));
        EXPECT_EQ(restored.records(), before);
    };
    refuses({}, 0, 1000000);
    refuses(bytes, 0, 1000000);
    refuses(bytes.substr(0, bytes.size() - 1), 2, 1000000);
    refuses(bytes + '\0', 2, 1000000);
    refuses(bytes, 1, 1000000);
    refuses(bytes, 2, -1);
    refuses(bytes, 2, 999999);
    refuses(bytes, (std::numeric_limits<std::size_t>::max)(), 1000000);
    refuses(bytes.substr(FoliageFieldPages::kRecordBytes) +
                bytes.substr(0, FoliageFieldPages::kRecordBytes),
            2,
            1000000);
    auto negative = bytes;
    negative.replace(FoliageFieldPages::kRecordBytes - 4, 4, std::string(4, '\xff'));
    refuses(negative, 2, 1000000);
    ASSERT_TRUE(FoliageFieldPages::parse_records(bytes, 2, 1000000, restored));
    EXPECT_EQ(restored.pages(), pages.pages());
}
} // namespace

namespace {
using namespace luminumbra::foliage;

TEST(FoliageFieldPages, AnchorsFloorNegativeCoordinatesAndRejectInvalidInputs) {
    EXPECT_EQ(FoliageCell({-0.25f, 2.0f, -16.25f}), (ScentCellKey{-1, -17}));
    EXPECT_EQ(FoliageCell({15.75f, -2.0f, 16.0f}), (ScentCellKey{15, 16}));
    EXPECT_EQ(FoliageCell({-16777216.0f, 0.0f, 16777215.0f}), (ScentCellKey{-16777216, 16777215}));
    const float infinity = std::numeric_limits<float>::infinity();
    const float nan = std::numeric_limits<float>::quiet_NaN();
    for (const auto anchor : {Luminumbra::Vec3{nan, 0.0f, 0.0f},
                              Luminumbra::Vec3{0.0f, nan, 0.0f},
                              Luminumbra::Vec3{0.0f, 0.0f, nan},
                              Luminumbra::Vec3{infinity, 0.0f, 0.0f},
                              Luminumbra::Vec3{0.0f, -infinity, 0.0f},
                              Luminumbra::Vec3{0.0f, 0.0f, infinity},
                              Luminumbra::Vec3{16777216.0f, 0.0f, 0.0f},
                              Luminumbra::Vec3{0.0f, 0.0f, -16777218.0f}}) {
        EXPECT_THROW(FoliageCell(anchor), std::invalid_argument);
    }
}

void ExpectSnapshot(const FlatFoliageCells& flat, const FoliageCellMap& reference) {
    const std::vector<FlatFoliageCells::Cell> expected(reference.begin(), reference.end());
    ASSERT_EQ(flat.cells(), expected);
    const auto neighbors = flat.interior_neighbors();
    ASSERT_EQ(neighbors.size(), expected.size());
    for (std::size_t index = 0; index < expected.size(); ++index) {
        const auto cell = expected[index].first;
        EXPECT_TRUE(flat.contains(cell));
        EXPECT_EQ(flat.at(cell), reference.at(cell));
        const std::array<ScentCellKey, 4> adjacent{{{cell.x - 1, cell.z},
                                                    {cell.x + 1, cell.z},
                                                    {cell.x, cell.z - 1},
                                                    {cell.x, cell.z + 1}}};
        for (std::size_t direction = 0; direction < adjacent.size(); ++direction) {
            const auto found = reference.find(adjacent[direction]);
            const auto expected_index =
                found != reference.end() &&
                        FieldPageState::region_of_scent_cell(adjacent[direction]) ==
                            FieldPageState::region_of_scent_cell(cell)
                    ? static_cast<std::size_t>(std::distance(reference.begin(), found))
                    : FlatFoliageCells::absent;
            EXPECT_EQ(neighbors[index][direction], expected_index);
        }
    }
}

TEST(FoliageFieldPages, FlatKeyOrderAndNeighborsExactlyMatchSparseMapAtWorldAndOwnerEdges) {
    FoliageFieldPages pages;
    for (const int x : {-16777216, -513, -512, -17, -16, -1, 0, 15, 16, 511, 512, 16777215}) {
        for (const int z : {-16777216, -513, -512, -17, -16, -1, 0, 15, 16, 511, 512, 16777215}) {
            if ((x % 3) != (z % 3)) {
                ASSERT_TRUE(pages.set({x, z}, 123, kSoilBaseline));
            }
        }
    }
    const FlatFoliageCells flat(pages);
    ExpectSnapshot(flat, FoliageCells(pages));
    EXPECT_EQ(flat.prepared_pages(), static_cast<std::uint64_t>(pages.pages().size()));
    EXPECT_FALSE(flat.contains({7, 7}));
    EXPECT_EQ(flat.find({7, 7}), flat.end());
    EXPECT_THROW(flat.at({7, 7}), std::out_of_range);
    FlatFoliageCells editable(pages);
    EXPECT_THROW(editable.at({7, 7}), std::out_of_range);
    const FlatFoliageCells empty;
    EXPECT_TRUE(empty.cells().empty());
    EXPECT_TRUE(empty.interior_neighbors().empty());
    EXPECT_EQ(empty.begin(), empty.end());
    EXPECT_THROW(empty.at({0, 0}), std::out_of_range);
}

void PopulateSparseFields(FieldPageState& pages, std::mt19937& random) {
    // Page coordinates straddle zero and owner edges; every page has a sparse mask.
    for (const int px : {-33, -32, -2, -1, 0, 1, 31, 32}) {
        for (const int pz : {-33, -1, 0, 1, 32}) {
            for (unsigned sample = 0; sample < 24; ++sample) {
                const auto slot = static_cast<unsigned>(random() % 256u);
                const ScentCellKey cell{px * 16 + static_cast<int>(slot % 16),
                                        pz * 16 + static_cast<int>(slot / 16)};
                ASSERT_TRUE(pages.set_soil(cell, static_cast<std::int32_t>(random() % 1000001u)));
                ASSERT_TRUE(
                    pages.set_irrigation(cell, static_cast<std::int32_t>(random() % 4000001u)));
            }
        }
    }
}

void CommitReference(const FoliageCellMap& soil,
                     const FoliageCellMap& irrigation,
                     FieldPageState& pages) {
    for (const auto& [cell, value] : soil) {
        ASSERT_TRUE(pages.set_soil(cell, value));
    }
    for (const auto& [cell, value] : irrigation) {
        ASSERT_TRUE(pages.set_irrigation(cell, value));
    }
}

TEST(FoliageFieldPages, RandomizedSparseScalarAndFlatEvolutionRemainByteIdentical) {
    std::mt19937 random(static_cast<std::mt19937::result_type>(0x61b203u));
    for (unsigned trial = 0; trial < 6; ++trial) {
        SCOPED_TRACE(trial);
        FieldPageState initial;
        ASSERT_NO_FATAL_FAILURE(PopulateSparseFields(initial, random));
        auto expected = initial;
        auto actual = initial;
        auto soil = FoliageCells(initial.soil());
        auto moisture = FoliageCells(initial.irrigation());
        FlatFoliageCells flat_soil(initial.soil());
        FlatFoliageCells flat_moisture(initial.irrigation());
        ExpectSnapshot(flat_soil, soil);
        ExpectSnapshot(flat_moisture, moisture);
        for (unsigned step = 0; step < 12; ++step) {
            SCOPED_TRACE(step);
            // Vary inputs between steps without changing the sealed topology.
            const auto cell = soil.begin()->first;
            const auto draw = static_cast<std::int32_t>(random() % 1000001u);
            soil.at(cell) = std::max(0, soil.at(cell) - draw);
            flat_soil.at(cell) = std::max(0, flat_soil.at(cell) - draw);
            moisture.at(cell) = kMoistureMax;
            flat_moisture.at(cell) = kMoistureMax;
            EvolveFoliageSoil(soil);
            EvolveFoliageSoil(flat_soil);
            EvolveFoliageIrrigation(moisture);
            EvolveFoliageIrrigation(flat_moisture);
            CommitReference(soil, moisture, expected);
            for (const auto& [key, value] : flat_soil) {
                ASSERT_TRUE(actual.set_soil(key, value));
            }
            for (const auto& [key, value] : flat_moisture) {
                ASSERT_TRUE(actual.set_irrigation(key, value));
            }
            EXPECT_EQ(actual.soil().records(), expected.soil().records());
            EXPECT_EQ(actual.irrigation().records(), expected.irrigation().records());
            EXPECT_EQ(actual.serialize(), expected.serialize());
            EXPECT_EQ(actual.canonical_bytes(), expected.canonical_bytes());
        }
    }
}

TEST(FoliageFieldPages, EvolutionUsesSealedPriorDrainClampingAndOwnerIsolation) {
    FieldPageState pages;
    for (const auto cell : {ScentCellKey{-1, 0},
                            ScentCellKey{0, 0},
                            ScentCellKey{1, 0},
                            ScentCellKey{511, 0},
                            ScentCellKey{512, 0}}) {
        ASSERT_TRUE(pages.set_soil(cell, kSoilBaseline - 1));
        ASSERT_TRUE(pages.set_irrigation(cell, 0));
    }
    ASSERT_TRUE(pages.set_soil({0, 0}, 0));
    ASSERT_TRUE(pages.set_irrigation({0, 0}, 80));
    ASSERT_TRUE(pages.set_irrigation({511, 0}, kMoistureMax));
    auto soil = FoliageCells(pages.soil());
    auto moisture = FoliageCells(pages.irrigation());
    FlatFoliageCells flat_soil(pages.soil()), flat_moisture(pages.irrigation());
    EvolveFoliageSoil(soil);
    EvolveFoliageSoil(flat_soil);
    EvolveFoliageIrrigation(moisture);
    EvolveFoliageIrrigation(flat_moisture);
    EXPECT_EQ(soil.at({0, 0}), kSoilRegenPerTick);
    EXPECT_EQ(soil.at({1, 0}), kSoilBaseline);
    EXPECT_EQ(moisture.at({0, 0}), 64);
    EXPECT_EQ(moisture.at({1, 0}), 4);
    EXPECT_EQ(moisture.at({-1, 0}), 0);
    EXPECT_EQ(moisture.at({511, 0}), kMoistureMax - kMoistureDrainPerTick);
    EXPECT_EQ(moisture.at({512, 0}), 0);
    ExpectSnapshot(flat_soil, soil);
    ExpectSnapshot(flat_moisture, moisture);
    FoliageCellMap empty;
    FlatFoliageCells flat_empty;
    EvolveFoliageSoil(empty);
    EvolveFoliageIrrigation(empty);
    EvolveFoliageSoil(flat_empty);
    EvolveFoliageIrrigation(flat_empty);
    EXPECT_TRUE(empty.empty());
    EXPECT_TRUE(flat_empty.cells().empty());
}

TEST(FoliageFieldPages, OwnerSnapshotsSelectOnlyTheirPagesIncludingExtremeAndAbsentOwners) {
    FoliageFieldPages pages;
    for (const auto cell : {ScentCellKey{-16777216, -16777216},
                            ScentCellKey{-513, -1},
                            ScentCellKey{-512, -1},
                            ScentCellKey{-1, -1},
                            ScentCellKey{0, 0},
                            ScentCellKey{511, 511},
                            ScentCellKey{512, 0},
                            ScentCellKey{16777215, 16777215}}) {
        ASSERT_TRUE(pages.set(cell, 42, kSoilBaseline));
    }
    const auto all = FoliageCells(pages);
    for (const auto owner : {RegionKey{-32768, -32768},
                             RegionKey{-2, -1},
                             RegionKey{-1, -1},
                             RegionKey{0, 0},
                             RegionKey{1, 0},
                             RegionKey{32767, 32767},
                             RegionKey{4, 4},
                             RegionKey{std::numeric_limits<std::int32_t>::max(), 0},
                             RegionKey{0, std::numeric_limits<std::int32_t>::min()}}) {
        FoliageCellMap expected;
        for (const auto& [cell, value] : all) {
            if (FieldPageState::region_of_scent_cell(cell) == owner) {
                expected.emplace(cell, value);
            }
        }
        const FlatFoliageCells flat(pages, owner);
        ExpectSnapshot(flat, expected);
        EXPECT_LE(flat.prepared_pages(), static_cast<std::uint64_t>(expected.size()));
        EXPECT_LE(flat.page_probes(), std::uint64_t{32});
    }
}

TEST(FoliageFieldPages, PreparationAndStepsVisitOnlyDueOwnersDespiteDormantPageCapacity) {
    FieldPageState pages;
    // All dormant pages share the due owner's X column: a column scan is insufficient.
    for (std::size_t index = 0; index < FoliageFieldPages::kMaxPages; ++index) {
        const ScentCellKey cell{0, static_cast<std::int32_t>(index) * 512};
        ASSERT_TRUE(pages.set_soil(cell, 0));
        ASSERT_TRUE(pages.set_irrigation(cell, 1000));
    }
    const auto dormant = pages.region_bytes({0, 1});
    FoliageInteriorReplay replay;
    const std::map<RegionKey, std::uint32_t> off_due{{{0, 0}, 0}};
    EXPECT_TRUE(replay.Step(pages, off_due).empty());
    EXPECT_EQ(replay.work().preparation_pages, std::uint64_t{0});
    EXPECT_EQ(replay.work().page_probes, std::uint64_t{0});
    const std::map<RegionKey, std::uint32_t> due{{{0, 0}, 1}, {{0, 1}, 0}};
    const std::map<RegionKey, std::uint64_t> expected_work{{{0, 0}, std::uint64_t{2}}};
    for (unsigned step = 0; step < 24; ++step) {
        EXPECT_EQ(replay.Step(pages, due), expected_work);
    }
    EXPECT_EQ(replay.work().preparation_pages, std::uint64_t{2});
    EXPECT_EQ(replay.work().preparation_cells, std::uint64_t{2});
    EXPECT_EQ(replay.work().neighbor_cells, std::uint64_t{1});
    EXPECT_EQ(replay.work().page_probes, std::uint64_t{64});
    EXPECT_EQ(replay.work().evolved_cells, std::uint64_t{48});
    EXPECT_EQ(pages.region_bytes({0, 1}), dormant);
    EXPECT_EQ(pages.soil().sample({0, 0}, -1), 24 * kSoilRegenPerTick);
    const std::map<RegionKey, std::uint32_t> newly_due{{{0, 1}, 1}, {{1, 1}, 1}};
    EXPECT_EQ(replay.Step(pages, newly_due).at({0, 1}), std::uint64_t{2});
    EXPECT_EQ(replay.work().preparation_pages, std::uint64_t{4});
    EXPECT_EQ(replay.work().preparation_cells, std::uint64_t{4});
    EXPECT_EQ(replay.work().page_probes, std::uint64_t{192});
    EXPECT_TRUE(replay.Step(pages, {}).empty());
    EXPECT_EQ(replay.work().evolved_cells, std::uint64_t{50});
}

FoliageCellMap OwnerCells(const FoliageFieldPages& pages, RegionKey owner) {
    FoliageCellMap cells;
    for (const auto& [cell, value] : FoliageCells(pages)) {
        if (FieldPageState::region_of_scent_cell(cell) == owner) {
            cells.emplace(cell, value);
        }
    }
    return cells;
}

TEST(FoliageFieldPages, PreparedOwnersMatchScalarStepsAcrossCadenceChangesAndLoad) {
    std::mt19937 random(static_cast<std::mt19937::result_type>(0x379a1u));
    FieldPageState actual;
    ASSERT_NO_FATAL_FAILURE(PopulateSparseFields(actual, random));
    auto expected = actual;
    FoliageInteriorReplay replay;
    for (unsigned step = 0; step < 12; ++step) {
        SCOPED_TRACE(step);
        const std::map<RegionKey, std::uint32_t> due{
            {{-1, -1}, 1}, {{0, 0}, step % 2}, {{1, 1}, step < 4 ? 0u : 1u}};
        std::map<RegionKey, std::uint64_t> expected_work;
        for (const auto& [owner, ticks] : due) {
            if (ticks == 0) {
                continue;
            }
            auto soil = OwnerCells(expected.soil(), owner);
            auto moisture = OwnerCells(expected.irrigation(), owner);
            EvolveFoliageSoil(soil);
            EvolveFoliageIrrigation(moisture);
            CommitReference(soil, moisture, expected);
            expected_work.emplace(owner,
                                  static_cast<std::uint64_t>(soil.size()) +
                                      static_cast<std::uint64_t>(moisture.size()));
        }
        EXPECT_EQ(replay.Step(actual, due), expected_work);
        EXPECT_EQ(actual.serialize(), expected.serialize());
        EXPECT_EQ(actual.canonical_bytes(), expected.canonical_bytes());
        if (step == 5) {
            FieldPageState loaded;
            std::string error;
            ASSERT_TRUE(FieldPageState::parse(actual.serialize(), loaded, error)) << error;
            actual = std::move(loaded);
            replay = FoliageInteriorReplay{};
        }
    }
    // Preparation exposes mutable values for inputs before pure evolution.
    auto& owner = replay.Prepare(actual, {0, 0});
    ASSERT_FALSE(owner.soil.cells().empty());
    const auto cell = owner.soil.begin()->first;
    owner.soil.at(cell) = 0;
    owner.moisture.at(cell) = kMoistureMax;
    ASSERT_TRUE(expected.set_soil(cell, 0));
    ASSERT_TRUE(expected.set_irrigation(cell, kMoistureMax));
    auto soil = OwnerCells(expected.soil(), {0, 0});
    auto moisture = OwnerCells(expected.irrigation(), {0, 0});
    EvolveFoliageSoil(soil);
    EvolveFoliageIrrigation(moisture);
    CommitReference(soil, moisture, expected);
    replay.Step(actual, {{{0, 0}, 1}});
    EXPECT_EQ(actual.serialize(), expected.serialize());
}

TEST(FoliageFieldPages, PageActivationPreservesSparseValuesAndRefusesInvalidOrFullAdmissions) {
    FieldPageState pages;
    ASSERT_TRUE(pages.set_soil({-1, -1}, 17));
    ASSERT_TRUE(pages.set_irrigation({-1, -1}, 23));
    ASSERT_TRUE(ActivateFoliagePage(pages, {-1, -1}, true));
    ASSERT_TRUE(ActivateFoliagePage(pages, {-1, -1}, false));
    EXPECT_EQ(FoliageCells(pages.soil()).size(), std::size_t{256});
    EXPECT_EQ(FoliageCells(pages.irrigation()).size(), std::size_t{256});
    EXPECT_EQ(pages.soil().sample({-1, -1}, -1), 17);
    EXPECT_EQ(pages.irrigation().sample({-1, -1}, -1), 23);
    EXPECT_EQ(pages.soil().sample({-16, -16}, -1), kSoilBaseline);
    EXPECT_EQ(pages.irrigation().sample({-16, -16}, -1), 0);
    const auto before = pages.serialize();
    EXPECT_FALSE(ActivateFoliagePage(pages, {std::numeric_limits<std::int32_t>::min(), 0}, true));
    EXPECT_FALSE(ActivateFoliagePage(pages, {0, std::numeric_limits<std::int32_t>::max()}, false));
    EXPECT_EQ(pages.serialize(), before);
    for (std::size_t index = 1; index < FoliageFieldPages::kMaxPages; ++index) {
        ASSERT_TRUE(pages.set_soil({static_cast<std::int32_t>(index) * 16, 0}, 0));
        ASSERT_TRUE(pages.set_irrigation({static_cast<std::int32_t>(index) * 16, 0}, 17));
    }
    const auto full_soil = pages.soil().records();
    const auto full_irrigation = pages.irrigation().records();
    EXPECT_FALSE(ActivateFoliagePage(pages, {0, 0}, true));
    EXPECT_FALSE(ActivateFoliagePage(pages, {0, 0}, false));
    EXPECT_EQ(pages.soil().records(), full_soil);
    EXPECT_EQ(pages.irrigation().records(), full_irrigation);
    EXPECT_TRUE(ActivateFoliagePage(pages, {16, 0}, true));
    EXPECT_TRUE(ActivateFoliagePage(pages, {16, 0}, false));
    EXPECT_EQ(pages.soil().sample({16, 0}, -1), 0);
    EXPECT_EQ(pages.soil().sample({17, 0}, -1), kSoilBaseline);
    EXPECT_EQ(pages.irrigation().sample({16, 0}, -1), 17);
    EXPECT_EQ(pages.irrigation().sample({17, 0}, -1), 0);
}
} // namespace

namespace {
TEST(FoliageFieldPages, ElapsedWindowsMatchIndependentSoilAndIrrigationSteps) {
    namespace C = Luminumbra::Components;
    entt::registry registry;
    const auto plant = registry.create();
    registry.emplace<C::PlantTag>(plant);
    registry.emplace<C::TransformComponent>(plant).position = {0.0f, 0.0f, 0.0f};
    registry.emplace<C::PlantGrowthComponent>(plant).stage =
        static_cast<std::uint8_t>(C::PlantStage::Mature);
    registry.emplace<C::SoilFeederComponent>(plant);
    registry.emplace<C::WaterSourceComponent>(plant);
    FieldPageState pages;
    ASSERT_TRUE(pages.set_soil({0, 0}, 1200));
    ASSERT_TRUE(pages.set_irrigation({0, 0}, 3999999));
    ASSERT_TRUE(pages.set_irrigation({1, 0}, 0));
    ASSERT_TRUE(pages.set_soil({512, 0}, 37));
    auto soil = FoliageCells(pages.soil());
    auto moisture = FoliageCells(pages.irrigation());
    std::uint32_t absorbed = 0;
    for (int step = 0; step < 17; ++step) {
        const auto draw = std::min(soil.at({0, 0}), 500);
        soil.at({0, 0}) -= draw;
        absorbed += static_cast<std::uint32_t>(draw);
        soil.at({0, 0}) = std::min(kSoilBaseline, soil.at({0, 0}) + kSoilRegenPerTick);
        moisture.at({0, 0}) = std::min(kMoistureMax, moisture.at({0, 0}) + 1000);
        EvolveFoliageIrrigation(moisture);
    }
    AdvanceFoliageInteriors(registry, pages, {{{0, 0}, 17}, {{1, 0}, 0}, {{2, 0}, 65536}});
    FieldPageState expected;
    CommitReference(soil, moisture, expected);
    EXPECT_EQ(pages.serialize(), expected.serialize());
    EXPECT_EQ(registry.get<C::SoilFeederComponent>(plant).absorbed, absorbed);
    const auto before = pages.serialize();
    EXPECT_THROW(AdvanceFoliageInteriors(registry, pages, {{{0, 0}, 65537}}),
                 std::invalid_argument);
    EXPECT_EQ(pages.serialize(), before);
    EXPECT_TRUE(AdvanceFoliageInteriors(registry, pages, {}).empty());
}
TEST(FoliageFieldPages, BordersUsePriorHostValuesCommonPhaseAndBothOwners) {
    ActiveRegionLedger ledger;
    const WorldClock clock(32);
    ledger.pin({0, 0}, true, clock);
    ledger.pin({1, 0}, true, clock);
    FieldPageState prior;
    ASSERT_TRUE(prior.set_irrigation({511, 0}, 800));
    ASSERT_TRUE(prior.set_irrigation({512, 0}, 0));
    const std::vector<RegionKey> owners{{0, 0}, {1, 0}};
    const auto edges = FoliageBorderEdges(prior, ledger.records(), owners, 32);
    ASSERT_EQ(edges.size(), 1u);
    auto next = prior;
    ASSERT_TRUE(next.set_irrigation({511, 0}, 794));
    const auto work = ExchangeFoliageBorders(next, prior, edges);
    EXPECT_EQ(next.irrigation().sample({511, 0}, -1), 694);
    EXPECT_EQ(next.irrigation().sample({512, 0}, -1), 100);
    EXPECT_EQ(work.at({0, 0}), 1u);
    EXPECT_EQ(work.at({1, 0}), 1u);
    EXPECT_TRUE(FoliageBorderEdges(prior, {}, owners, 32).empty());
    auto& records = const_cast<std::map<RegionKey, ActiveRegionRecord>&>(ledger.records());
    records.at({1, 0}).cadence_shift = 2;
    EXPECT_TRUE(FoliageBorderEdges(prior, records, owners, 33).empty());
    records.at({1, 0}).state = RegionState::Frozen;
    EXPECT_TRUE(FoliageBorderEdges(prior, records, owners, 32).empty());
}
TEST(FoliageFieldPages, ZeroResumeAndStaleOrRepeatedTokensCannotReplayAWindow) {
    ActiveRegionLedger ledger;
    WorldClock clock(32);
    ledger.pin({0, 0}, true, clock);
    auto& record =
        const_cast<std::map<RegionKey, ActiveRegionRecord>&>(ledger.records()).at({0, 0});
    record.frozen_at = 32;
    const std::set<RegionKey> pending{{0, 0}};
    const std::map<RegionKey, std::uint64_t> previous{{{0, 0}, 32}};
    const auto windows = BuildRegionalFoliageWindows(ledger, {}, previous, pending, clock);
    EXPECT_EQ(windows.elapsed.at({0, 0}), 0u);
    EXPECT_EQ(windows.resumes.at({0, 0}), 32u);
    EXPECT_FALSE(ledger.acknowledge_resume({0, 0}, 31));
    EXPECT_TRUE(ledger.acknowledge_resume({0, 0}, 32));
    const auto bytes = ledger.encode();
    EXPECT_FALSE(ledger.acknowledge_resume({0, 0}, 32));
    EXPECT_EQ(ledger.encode(), bytes);
    EXPECT_TRUE(BuildRegionalFoliageWindows(ledger, {}, previous, pending, clock).elapsed.empty());
    record.state = RegionState::Frozen;
    record.frozen_at = 32;
    EXPECT_TRUE(BuildRegionalFoliageWindows(ledger, {}, previous, pending, clock).elapsed.empty());
}
} // namespace
