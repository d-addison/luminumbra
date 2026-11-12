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
#include <optional>
#include <random>
#include <set>
#include <span>
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
// Test-only drain of the public bounded cursor; session callers advance once
// per update and retain the cursor until remaining_steps() reaches zero.
std::map<RegionKey, std::uint64_t>
ReplayAllFoliageInteriors(entt::registry& registry,
                          FieldPageState& pages,
                          const std::map<RegionKey, std::uint32_t>& windows,
                          std::optional<std::uint64_t> tick = {},
                          FoliageInteriorReplay::Work* diagnostic = nullptr) {
    FoliageWindowReplay replay(pages, windows, tick);
    std::map<RegionKey, std::uint64_t> work;
    while (replay.remaining_steps() != 0) {
        for (const auto& [key, count] : replay.Advance(registry, pages)) {
            work[key] += count;
        }
    }
    if (diagnostic) {
        *diagnostic = replay.work();
    }
    return work;
}
FoliageEdges ScheduledFoliageBorderEdges(const FieldPageState& pages,
                                         const std::map<RegionKey, ActiveRegionRecord>& records,
                                         std::span<const RegionKey> owners,
                                         std::uint64_t tick) {
    return SelectFoliageBorderEdges(FoliageBorderEdges(pages, records, owners), records, tick);
}
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
    ReplayAllFoliageInteriors(registry, pages, {{{0, 0}, 17}, {{1, 0}, 0}, {{2, 0}, 65536}});
    FieldPageState expected;
    CommitReference(soil, moisture, expected);
    EXPECT_EQ(pages.serialize(), expected.serialize());
    EXPECT_EQ(registry.get<C::SoilFeederComponent>(plant).absorbed, absorbed);
    const auto before = pages.serialize();
    EXPECT_THROW(ReplayAllFoliageInteriors(registry, pages, {{{0, 0}, 65537}}),
                 std::invalid_argument);
    EXPECT_EQ(pages.serialize(), before);
    EXPECT_TRUE(ReplayAllFoliageInteriors(registry, pages, {}).empty());
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
    const auto edges = ScheduledFoliageBorderEdges(prior, ledger.records(), owners, 32);
    ASSERT_EQ(edges.size(), 1u);
    auto next = prior;
    ASSERT_TRUE(next.set_irrigation({511, 0}, 794));
    const auto fluxes = FoliageBorderFluxes(prior.irrigation(), edges);
    ASSERT_EQ(fluxes.size(), edges.size());
    EXPECT_EQ(fluxes.front(), 100);
    const auto work = ExchangeFoliageBorders(next, fluxes, edges);
    EXPECT_EQ(next.irrigation().sample({511, 0}, -1), 694);
    EXPECT_EQ(next.irrigation().sample({512, 0}, -1), 100);
    EXPECT_EQ(work.at({0, 0}), 1u);
    EXPECT_EQ(work.at({1, 0}), 1u);
    EXPECT_TRUE(ScheduledFoliageBorderEdges(prior, {}, owners, 32).empty());
    auto records = ledger.records();
    records.at({1, 0}).cadence_shift = 2;
    EXPECT_TRUE(ScheduledFoliageBorderEdges(prior, records, owners, 33).empty());
    records.at({1, 0}).state = RegionState::Frozen;
    EXPECT_TRUE(ScheduledFoliageBorderEdges(prior, records, owners, 32).empty());
}
TEST(FoliageFieldPages, BorderSnapshotContainsOnlyEdgesAndRefusesIncompleteInput) {
    FieldPageState pages;
    for (int index = 0; index < 128; ++index) {
        ASSERT_TRUE(ActivateFoliagePage(pages, {index * 16, 32}, true));
        ASSERT_TRUE(ActivateFoliagePage(pages, {index * 16, 32}, false));
    }
    ASSERT_TRUE(pages.set_irrigation({511, 0}, 800));
    ASSERT_TRUE(pages.set_irrigation({512, 0}, 0));
    const FoliageEdges edges{{{511, 0}, {512, 0}}};
    const auto fluxes = FoliageBorderFluxes(pages.irrigation(), edges);
    ASSERT_EQ(fluxes.size(), 1u);
    EXPECT_EQ(fluxes.front(), 100);
    EXPECT_TRUE(FoliageBorderFluxes(pages.irrigation(), {}).empty());
    const auto before = pages.serialize();
    EXPECT_THROW(ExchangeFoliageBorders(pages, {}, edges), std::invalid_argument);
    EXPECT_THROW(ExchangeFoliageBorders(pages, fluxes, {}), std::invalid_argument);
    EXPECT_EQ(pages.serialize(), before);
    EXPECT_TRUE(ExchangeFoliageBorders(pages, {}, {}).empty());
    EXPECT_EQ(ExchangeFoliageBorders(pages, fluxes, edges).size(), 2u);
    EXPECT_EQ(pages.irrigation().sample({511, 0}, -1), 700);
    EXPECT_EQ(pages.irrigation().sample({512, 0}, -1), 100);
}
TEST(FoliageFieldPages, BorderDiscoveryRequiresBothEndpointsAndRejectsUnavailableNeighbors) {
    const std::array<ScentCellKey, 4> cells{{{511, 8}, {0, 8}, {8, 511}, {8, 0}}};
    const std::array<ScentCellKey, 4> neighbors{{{512, 8}, {-1, 8}, {8, 512}, {8, -1}}};
    for (std::size_t direction = 0; direction < cells.size(); ++direction) {
        for (const bool initialize_source : {false, true}) {
            SCOPED_TRACE(direction);
            SCOPED_TRACE(initialize_source);
            const auto cell = cells[direction], next = neighbors[direction];
            const auto owner = FieldPageState::region_of_scent_cell(cell);
            const auto other = FieldPageState::region_of_scent_cell(next);
            ActiveRegionLedger ledger;
            ledger.pin(owner, true, WorldClock(32));
            ledger.pin(other, true, WorldClock(32));
            const std::array<RegionKey, 1> owners{{owner}};
            FieldPageState pages;
            EXPECT_TRUE(ScheduledFoliageBorderEdges(pages, ledger.records(), owners, 32).empty());
            ASSERT_TRUE(pages.set_irrigation(initialize_source ? cell : next, 800));
            const auto sparse = ScheduledFoliageBorderEdges(pages, ledger.records(), owners, 32);
            EXPECT_TRUE(sparse.empty());
            const auto sparse_fluxes = FoliageBorderFluxes(pages.irrigation(), sparse);
            auto continuous = FoliageCells(pages.irrigation());
            EvolveFoliageIrrigation(continuous);
            auto stepped = pages;
            FoliageInteriorReplay replay;
            replay.Step(stepped, {{owner, 1}, {other, 1}});
            EXPECT_NO_THROW(ExchangeFoliageBorders(stepped, sparse_fluxes, sparse));
            FieldPageState expected;
            CommitReference({}, continuous, expected);
            EXPECT_EQ(stepped.serialize(), expected.serialize());
            EXPECT_FALSE(stepped.irrigation().initialized(initialize_source ? next : cell));
            // Explicit admission enables the same edge in either direction.
            ASSERT_TRUE(pages.set_irrigation(initialize_source ? next : cell, 0));
            const auto edges = ScheduledFoliageBorderEdges(pages, ledger.records(), owners, 32);
            ASSERT_EQ(edges.size(), 1u);
            EXPECT_EQ(edges.front().first, std::min(cell, next));
            EXPECT_EQ(edges.front().second, std::max(cell, next));
            const auto fluxes = FoliageBorderFluxes(pages.irrigation(), edges);
            ASSERT_EQ(fluxes.size(), 1u);
            EXPECT_EQ(fluxes.front(), (initialize_source == (direction % 2 == 0)) ? 100 : -100);
            auto records = ledger.records();
            records.at(other).cadence_shift = 1;
            EXPECT_TRUE(ScheduledFoliageBorderEdges(pages, records, owners, 33).empty());
            records.at(other).cadence_shift = 17;
            EXPECT_TRUE(ScheduledFoliageBorderEdges(pages, records, owners, 32).empty());
            records.at(other).state = RegionState::Frozen;
            EXPECT_TRUE(ScheduledFoliageBorderEdges(pages, records, owners, 32).empty());
            records.erase(other);
            EXPECT_TRUE(ScheduledFoliageBorderEdges(pages, records, owners, 32).empty());
        }
    }
    for (const int coordinate : {-16777216, 16777215}) {
        FieldPageState pages;
        ASSERT_TRUE(pages.set_irrigation({coordinate, 8}, 800));
        const auto owner = FieldPageState::region_of_scent_cell({coordinate, 8});
        const RegionKey other{owner.x + (coordinate < 0 ? -1 : 1), owner.z};
        ActiveRegionLedger ledger;
        ledger.pin(owner, true, WorldClock(32));
        // The neighbour lies past the ledger's key range, so it can never be active.
        EXPECT_FALSE(ledger.can_activate(other));
        const std::array<RegionKey, 2> owners{{owner, other}};
        EXPECT_TRUE(ScheduledFoliageBorderEdges(pages, ledger.records(), owners, 32).empty());
    }
}
TEST(FoliageFieldPages, FirstDueWindowsDistinguishNewOwnersFromConsumedTicks) {
    ActiveRegionLedger ledger;
    ledger.pin({0, 0}, true, WorldClock(1));
    const WorldClock clock(32);
    const auto schedule = ledger.schedule(clock, std::span<const Luminumbra::Vec3>{}, {}, {});
    ASSERT_EQ(schedule.due.size(), 1u);
    EXPECT_EQ(BuildRegionalFoliageWindows(ledger, schedule.due, {}, {}, clock).elapsed.at({0, 0}),
              1u);
    for (const std::uint64_t previous : {20u, 31u, 32u}) {
        const std::map<RegionKey, std::uint64_t> history{{{0, 0}, previous}};
        EXPECT_EQ(BuildRegionalFoliageWindows(ledger, schedule.due, history, {}, clock)
                      .elapsed.at({0, 0}),
                  32u - previous);
    }
    const std::map<RegionKey, std::uint64_t> future{{{0, 0}, 33}};
    EXPECT_THROW(BuildRegionalFoliageWindows(ledger, schedule.due, future, {}, clock),
                 std::invalid_argument);
    EXPECT_TRUE(BuildRegionalFoliageWindows(ledger, {}, {}, {}, clock).elapsed.empty());
    const std::array<RegionKey, 1> missing{{{1, 0}}};
    EXPECT_TRUE(BuildRegionalFoliageWindows(ledger, missing, {}, {}, clock).elapsed.empty());
}
TEST(FoliageFieldPages, CurrentTickMaterializationKeepsFieldsAndSourcesUnchangedAcrossReload) {
    namespace C = Luminumbra::Components;
    const RegionKey owner{0, 0};
    const std::array<RegionKey, 1> due{{owner}};
    for (const std::uint64_t tick :
         {std::uint64_t{1}, std::uint64_t{48}, WorldClock::kTickLimit - 1}) {
        for (const bool materialized : {false, true}) {
            SCOPED_TRACE(tick);
            SCOPED_TRACE(materialized);
            ActiveRegionLedger ledger;
            const WorldClock clock(tick);
            if (materialized) {
                ledger.mark_materialized_edit(owner, clock);
                ledger.mark_materialized_edit(owner, clock);
            } else {
                // Scheduling needs a tick newer than the activation stamp.
                ledger.pin(owner, true, WorldClock(tick - 1));
                ledger.schedule(clock, std::span<const Luminumbra::Vec3>{});
                ledger.mark_edited(owner, clock);
            }
            ActiveRegionLedger reopened;
            std::string error;
            ASSERT_TRUE(ActiveRegionLedger::decode(ledger.encode(), reopened, error)) << error;
            EXPECT_EQ(reopened.encode(), ledger.encode());
            for (const auto* saved : {&ledger, &reopened}) {
                entt::registry registry;
                const auto plant = registry.create();
                registry.emplace<C::PlantTag>(plant);
                registry.emplace<C::TransformComponent>(plant);
                registry.emplace<C::PlantGrowthComponent>(plant).stage =
                    static_cast<std::uint8_t>(C::PlantStage::Mature);
                registry.emplace<C::SoilFeederComponent>(plant);
                registry.emplace<C::WaterSourceComponent>(plant);
                FieldPageState pages;
                ASSERT_TRUE(pages.set_soil({0, 0}, 1200));
                ASSERT_TRUE(pages.set_irrigation({0, 0}, 1000));
                const auto before = pages.serialize();
                auto expected = pages;
                // An unconsumed first tick still consumes, deposits and evolves once.
                if (!materialized) {
                    ASSERT_TRUE(expected.set_soil({0, 0}, 1200 - 500 + kSoilRegenPerTick));
                    ASSERT_TRUE(expected.set_irrigation({0, 0}, 2000 - kMoistureDrainPerTick));
                }
                const auto windows = BuildRegionalFoliageWindows(*saved, due, {}, {}, clock);
                EXPECT_EQ(windows.elapsed.at(owner), materialized ? 0u : 1u);
                EXPECT_TRUE(windows.resumes.empty());
                FoliageWindowReplay replay(pages, windows.elapsed, tick);
                EXPECT_EQ(replay.remaining_steps(), materialized ? 0u : 1u);
                const auto completed = replay.Advance(registry, pages);
                EXPECT_EQ(completed.empty(), materialized);
                EXPECT_EQ(replay.remaining_steps(), 0u);
                EXPECT_EQ(pages.serialize(), expected.serialize());
                EXPECT_EQ(pages.serialize() == before, materialized);
                EXPECT_EQ(registry.get<C::SoilFeederComponent>(plant).absorbed,
                          materialized ? 0u : 500u);
                EXPECT_EQ(replay.work().evolved_cells, materialized ? 0u : 2u);
                EXPECT_EQ(replay.work().source_visits, materialized ? 0u : 2u);
                EXPECT_TRUE(replay.Advance(registry, pages).empty());
                EXPECT_EQ(pages.serialize(), expected.serialize());
            }
        }
    }
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

namespace {
TEST(FoliageFieldPages, ReplayCollectsSourcesOnceAndVisitsOnlyDueOwnersAndBirths) {
    namespace C = Luminumbra::Components;
    entt::registry registry;
    FieldPageState pages;
    ASSERT_TRUE(pages.set_soil({0, 0}, kSoilBaseline));
    ASSERT_TRUE(pages.set_irrigation({512, 0}, 0));
    for (int index = 0; index < 64; ++index) {
        const auto plant = registry.create();
        registry.emplace<C::PlantTag>(plant);
        registry.emplace<C::TransformComponent>(plant);
        registry.emplace<C::PlantGrowthComponent>(plant);
        registry.emplace<C::SoilFeederComponent>(plant);
        registry.emplace<C::PlantRegionComponent>(
            plant, static_cast<std::uint64_t>(index + 1), 0, 0, std::uint64_t{999});
        const auto source = registry.create();
        registry.emplace<C::PlantTag>(source);
        auto& source_growth = registry.emplace<C::PlantGrowthComponent>(source);
        source_growth.planted_tick = source_growth.last_tick = 1000;
        registry.emplace<C::TransformComponent>(source).position = {512, 0, 0};
        registry.emplace<C::WaterSourceComponent>(source);
    }
    FoliageInteriorReplay::Work work;
    ReplayAllFoliageInteriors(registry, pages, {{{0, 0}, 1000}, {{1, 0}, 1}}, 1000, &work);
    EXPECT_EQ(work.source_preparations, 128u);
    EXPECT_EQ(work.source_visits, 128u);
    EXPECT_EQ(work.evolved_cells, 1001u);
    EXPECT_TRUE(ReplayAllFoliageInteriors(registry, pages, {}, 1000, &work).empty());
    EXPECT_EQ(work.source_preparations, 0u);
    const auto before = pages.serialize();
    EXPECT_THROW(ReplayAllFoliageInteriors(registry, pages, {{{0, 0}, 1001}}, 1000),
                 std::invalid_argument);
    EXPECT_EQ(pages.serialize(), before);
}

TEST(FoliageFieldPages, SparsePublicationPreservesUntouchedCellsAndPreflightsBothChannels) {
    FieldPageState live, candidate;
    ASSERT_TRUE(live.set_soil({0, 0}, 7));
    ASSERT_TRUE(candidate.set_soil({1, 0}, 9));
    ASSERT_TRUE(live.can_adopt_foliage(candidate));
    live.adopt_foliage(std::move(candidate));
    EXPECT_EQ(live.soil().sample({0, 0}, -1), 7);
    EXPECT_EQ(live.soil().sample({1, 0}, -1), 9);
    for (std::size_t index = 0; index < FoliageFieldPages::kMaxPages; ++index) {
        ASSERT_TRUE(live.set_irrigation({static_cast<std::int32_t>(index * 16), 0}, 0));
    }
    const auto before = live.serialize();
    FieldPageState refused;
    ASSERT_TRUE(refused.set_soil({16, 0}, 11));
    ASSERT_TRUE(refused.set_irrigation({-16, 0}, 13));
    EXPECT_FALSE(live.can_adopt_foliage(refused));
    EXPECT_EQ(live.serialize(), before);
    FieldPageState replacement;
    ASSERT_TRUE(replacement.set_irrigation({0, 0}, 13));
    ASSERT_TRUE(live.can_adopt_foliage(replacement));
    live.adopt_foliage(std::move(replacement));
    EXPECT_EQ(live.irrigation().sample({0, 0}, -1), 13);
}
} // namespace

namespace {
TEST(FoliageFieldPages, BoundedReplayMatchesUninterruptedWindowAndLimitsEveryCall) {
    namespace C = Luminumbra::Components;
    const auto populate = [](entt::registry& registry) {
        for (const std::uint64_t birth : {0u, 32u, 33u, 35999u, 36001u}) {
            const auto entity = registry.create();
            registry.emplace<C::PlantTag>(entity);
            registry.emplace<C::TransformComponent>(entity);
            auto& growth = registry.emplace<C::PlantGrowthComponent>(entity);
            growth.stage = static_cast<std::uint8_t>(C::PlantStage::Mature);
            growth.planted_tick = birth;
            registry.emplace<C::SoilFeederComponent>(entity);
            registry.emplace<C::WaterSourceComponent>(entity);
            registry.emplace<C::PlantRegionComponent>(entity, 36002 - birth, 0, 0, birth);
        }
    };
    entt::registry bounded_registry, reference_registry;
    populate(bounded_registry);
    populate(reference_registry);
    FieldPageState pages;
    ASSERT_TRUE(pages.set_soil({0, 0}, 1200));
    ASSERT_TRUE(pages.set_irrigation({0, 0}, 3999999));
    ASSERT_TRUE(pages.set_irrigation({1, 0}, 0));
    ASSERT_TRUE(pages.set_soil({512, 0}, 37));
    ASSERT_TRUE(pages.set_soil({1024, 0}, 53));
    auto expected = pages;
    // Mixed lengths activate the short window near the end of the shared
    // timeline. Zero windows and pageless owners must never consume steps.
    const std::map<RegionKey, std::uint32_t> windows{
        {{0, 0}, 36000}, {{1, 0}, 47}, {{2, 0}, 0}, {{3, 0}, 36000}};
    FoliageWindowReplay replay(pages, windows, 36000);
    EXPECT_EQ(replay.remaining_steps(), 36000u);
    std::uint32_t calls = 0;
    while (replay.remaining_steps() != 0) {
        const auto remaining = replay.remaining_steps();
        const auto evolved = replay.work().evolved_cells;
        const auto visits = replay.work().source_visits;
        replay.Advance(bounded_registry, pages);
        const auto advanced = remaining - replay.remaining_steps();
        EXPECT_LE(advanced, 32u);
        EXPECT_EQ(advanced, std::min(remaining, FoliageWindowReplay::kStepBudget));
        EXPECT_LE(replay.work().evolved_cells - evolved, 4u * advanced);
        EXPECT_LE(replay.work().source_visits - visits, 10u * advanced);
        ++calls;
    }
    EXPECT_EQ(calls,
              (36000u + FoliageWindowReplay::kStepBudget - 1) / FoliageWindowReplay::kStepBudget);
    EXPECT_EQ(replay.work().source_preparations, 10u);
    EXPECT_EQ(replay.work().preparation_cells, 4u);
    EXPECT_EQ(replay.work().evolved_cells, 36000u * 3 + 47u);
    // The unbounded oracle uses one uninterrupted replay with the original
    // absolute subticks and independently selects the owners on each step.
    FoliageInteriorReplay reference;
    for (std::uint32_t step = 1; step <= 36000; ++step) {
        std::map<RegionKey, std::uint32_t> due{{{0, 0}, 1}};
        if (step > 36000 - 47) {
            due.emplace(RegionKey{1, 0}, 1);
        }
        reference.Step(reference_registry, expected, due, step);
    }
    EXPECT_EQ(pages.serialize(), expected.serialize());
    for (const auto entity : bounded_registry.view<C::SoilFeederComponent>()) {
        EXPECT_EQ(bounded_registry.get<C::SoilFeederComponent>(entity).absorbed,
                  reference_registry.get<C::SoilFeederComponent>(entity).absorbed);
    }
    const auto bytes = pages.serialize();
    EXPECT_TRUE(replay.Advance(bounded_registry, pages).empty());
    EXPECT_EQ(pages.serialize(), bytes);
    EXPECT_EQ(replay.work().evolved_cells, reference.work().evolved_cells);
    EXPECT_EQ(replay.work().source_visits, reference.work().source_visits);
}

TEST(FoliageFieldPages, BoundedReplayEmptyZeroAndInvalidWindowsDoNotMutateState) {
    entt::registry registry;
    FieldPageState pages;
    ASSERT_TRUE(pages.set_soil({0, 0}, 37));
    const auto bytes = pages.serialize();
    for (const auto& windows : {std::map<RegionKey, std::uint32_t>{},
                                std::map<RegionKey, std::uint32_t>{{{0, 0}, 0}},
                                std::map<RegionKey, std::uint32_t>{{{1, 0}, 65536}}}) {
        FoliageWindowReplay replay(pages, windows);
        EXPECT_EQ(replay.remaining_steps(), 0u);
        EXPECT_TRUE(replay.Advance(registry, pages).empty());
        EXPECT_EQ(replay.work().preparation_cells, 0u);
        EXPECT_EQ(replay.work().source_preparations, 0u);
    }
    const std::map<RegionKey, std::uint32_t> too_long{{{0, 0}, 65537}};
    EXPECT_THROW((FoliageWindowReplay(pages, too_long)), std::invalid_argument);
    const std::map<RegionKey, std::uint32_t> underflow{{{0, 0}, 33}};
    EXPECT_THROW((FoliageWindowReplay(pages, underflow, 32)), std::invalid_argument);
    EXPECT_EQ(pages.serialize(), bytes);
}

TEST(FoliageFieldPages, BorderDiscoveryRetainsNonzeroPhasesAndSelectionRequiresBothOwners) {
    FieldPageState pages;
    ASSERT_TRUE(pages.set_irrigation({511, 8}, 800));
    ASSERT_TRUE(pages.set_irrigation({512, 8}, 0));
    std::map<RegionKey, ActiveRegionRecord> records{{{0, 0}, ActiveRegionRecord{}},
                                                    {{1, 0}, ActiveRegionRecord{}}};
    const std::array<RegionKey, 2> owners{{{0, 0}, {1, 0}}};
    auto& left = records.at({0, 0});
    auto& right = records.at({1, 0});
    left.state = right.state = RegionState::Reduced;
    left.cadence_shift = 1;
    left.cadence_phase = 1;
    right.cadence_shift = 2;
    for (const std::uint32_t phase : {1u, 2u, 3u}) {
        right.cadence_phase = phase;
        const auto edges = FoliageBorderEdges(pages, records, owners);
        ASSERT_EQ(edges.size(), 1u);
        for (std::uint64_t tick = 0; tick < 16; ++tick) {
            SCOPED_TRACE(phase);
            SCOPED_TRACE(tick);
            const auto selected = SelectFoliageBorderEdges(edges, records, tick);
            EXPECT_EQ(selected.size(), tick % 2 == 1 && tick % 4 == phase ? 1u : 0u);
        }
        right.cadence_shift = 17;
        EXPECT_TRUE(SelectFoliageBorderEdges(edges, records, phase).empty());
        right.cadence_shift = 2;
        right.cadence_phase = 4;
        EXPECT_TRUE(SelectFoliageBorderEdges(edges, records, phase).empty());
        right.cadence_phase = phase;
        right.state = RegionState::Frozen;
        EXPECT_TRUE(SelectFoliageBorderEdges(edges, records, phase).empty());
        right.state = RegionState::Reduced;
        auto missing = records;
        missing.erase({1, 0});
        EXPECT_TRUE(SelectFoliageBorderEdges(edges, missing, phase).empty());
    }
    EXPECT_TRUE(SelectFoliageBorderEdges({}, records, 3).empty());
}

TEST(FoliageFieldPages, MaterializedEditsPreserveResumeTokensAndRoundTripWithoutReplayingEdits) {
    const RegionKey owner{0, 0};
    for (const auto state : {RegionState::Active, RegionState::Reduced, RegionState::Frozen}) {
        ActiveRegionLedger ledger;
        ledger.pin(owner, true, WorldClock(32));
        auto& record =
            const_cast<std::map<RegionKey, ActiveRegionRecord>&>(ledger.records()).at(owner);
        record.state = state;
        record.cadence_shift = state == RegionState::Active ? 0 : 1;
        record.cadence_phase = ActiveRegionLedger::phase(owner, record.cadence_shift);
        record.last_ticked = 31;
        record.frozen_at = 32;
        EXPECT_EQ(FoliagePriorTick(record, ledger.foliage_completion(owner)), 32u);
        ledger.mark_edited(owner, WorldClock(40));
        EXPECT_EQ(FoliagePriorTick(record, ledger.foliage_completion(owner)), 32u);
        ledger.mark_materialized_edit(owner, WorldClock(48));
        EXPECT_EQ(record.last_edit, 48u);
        EXPECT_EQ(record.last_ticked, 48u);
        EXPECT_EQ(record.frozen_at, 32u);
        EXPECT_TRUE(record.wake_pending);
        EXPECT_NE(record.state, RegionState::Frozen);
        EXPECT_EQ(FoliagePriorTick(record, ledger.foliage_completion(owner)), 48u);
        ActiveRegionLedger reopened;
        std::string error;
        ASSERT_TRUE(ActiveRegionLedger::decode(ledger.encode(), reopened, error)) << error;
        EXPECT_EQ(reopened.encode(), ledger.encode());
        const std::set<RegionKey> pending{owner};
        const auto window = BuildRegionalFoliageWindows(reopened, {}, {}, pending, WorldClock(52));
        EXPECT_EQ(window.elapsed.at(owner), 4u);
        EXPECT_EQ(window.resumes.at(owner), 32u);
    }
    ActiveRegionLedger ledger;
    ledger.mark_materialized_edit(owner, WorldClock(8));
    EXPECT_EQ(ledger.records().at(owner).last_ticked, 8u);
    const auto bytes = ledger.encode();
    EXPECT_THROW(ledger.mark_materialized_edit({32768, 0}, WorldClock(16)), std::invalid_argument);
    EXPECT_EQ(ledger.encode(), bytes);
}

TEST(FoliageFieldPages, OrdinaryEditsRetainMaterializedCompletionAcrossReloads) {
    const RegionKey owner{0, 0};
    ActiveRegionLedger ledger({.work_limit = 0, .hold_ticks = 1, .reduced_shift = 1});
    ledger.mark_edited(owner, WorldClock(29));
    const std::array<RegionWork, 1> work{{{owner, 1, 0, 0, 0}}};
    for (std::uint64_t tick = 30; tick <= 32; ++tick) {
        ledger.schedule(WorldClock(tick), std::span<const Luminumbra::Vec3>{}, work, [](RegionKey) {
            return std::uint64_t{1};
        });
    }
    ASSERT_EQ(ledger.records().at(owner).state, RegionState::Frozen);
    ASSERT_EQ(ledger.records().at(owner).frozen_at, 32u);
    ledger.mark_edited(owner, WorldClock(40));
    EXPECT_EQ(FoliagePriorTick(ledger.records().at(owner), ledger.foliage_completion(owner)), 32u);
    ledger.mark_materialized_edit(owner, WorldClock(48));
    ledger.mark_edited(owner, WorldClock(50));
    ledger.mark_edited(owner, WorldClock(50));
    ledger.mark_edited(owner, WorldClock(51));
    EXPECT_EQ(FoliagePriorTick(ledger.records().at(owner), ledger.foliage_completion(owner)), 48u);

    ActiveRegionLedger reopened;
    std::string error;
    ASSERT_TRUE(ActiveRegionLedger::decode(ledger.encode(), reopened, error)) << error;
    EXPECT_EQ(reopened.encode(), ledger.encode());
    FieldPageState materialized;
    ASSERT_TRUE(materialized.set_soil({0, 0}, 100));
    ASSERT_TRUE(materialized.set_irrigation({0, 0}, 1000));
    entt::registry registry;
    ReplayAllFoliageInteriors(registry, materialized, {{owner, 16}}, 48);
    auto continuous = materialized;
    ReplayAllFoliageInteriors(registry, continuous, {{owner, 4}}, 52);
    for (const auto* saved : {&ledger, &reopened}) {
        const auto window = BuildRegionalFoliageWindows(*saved, {}, {}, {owner}, WorldClock(52));
        EXPECT_EQ(window.elapsed.at(owner), 4u);
        EXPECT_EQ(window.resumes.at(owner), 32u);
        auto recovered = materialized;
        FoliageInteriorReplay::Work replay_work;
        ReplayAllFoliageInteriors(registry, recovered, window.elapsed, 52, &replay_work);
        EXPECT_EQ(replay_work.evolved_cells, 8u);
        EXPECT_EQ(recovered.serialize(), continuous.serialize());
    }
    // A later materialization replaces the exclusive completion register.
    // Ordinary edits preserve it; invalid/backward calls cannot change it.
    reopened.mark_materialized_edit(owner, WorldClock(56));
    reopened.mark_materialized_edit(owner, WorldClock(56));
    reopened.mark_edited(owner, WorldClock(58));
    EXPECT_EQ(FoliagePriorTick(reopened.records().at(owner), reopened.foliage_completion(owner)),
              56u);
    const auto bytes = reopened.encode();
    EXPECT_THROW(reopened.mark_materialized_edit(owner, WorldClock(57)), std::invalid_argument);
    EXPECT_THROW(reopened.mark_edited({32768, 0}, WorldClock(60)), std::invalid_argument);
    EXPECT_EQ(reopened.encode(), bytes);
}

TEST(FoliageFieldPages, ExclusiveCompletionSurvivesSchedulingAndFrozenWakeReload) {
    const RegionKey owner{0, 0};
    for (const bool materialized : {false, true}) {
        ActiveRegionLedger ledger({.work_limit = 0, .hold_ticks = 1, .reduced_shift = 1});
        ledger.mark_edited(owner, WorldClock(29));
        const std::array work{RegionWork{owner, 1, 0, 0, 0}};
        for (std::uint64_t tick = 30; tick <= 32; ++tick)
            ledger.schedule(WorldClock(tick),
                            std::span<const Luminumbra::Vec3>{},
                            work,
                            [](RegionKey) { return std::uint64_t{1}; });
        ASSERT_EQ(ledger.records().at(owner).state, RegionState::Frozen);
        ledger.wake_frozen(owner);
        ledger.schedule(WorldClock(48), std::span<const Luminumbra::Vec3>{});
        ASSERT_EQ(ledger.records().at(owner).last_ticked, 48u);
        if (materialized)
            ledger.mark_materialized_edit(owner, WorldClock(48));
        else
            ledger.mark_edited(owner, WorldClock(48));
        // Both histories still have matching ordinary record stamps.
        EXPECT_EQ(ledger.records().at(owner).last_edit, 48u);
        EXPECT_EQ(ledger.records().at(owner).last_ticked, 48u);
        ActiveRegionLedger edited_reload;
        std::string error;
        ASSERT_TRUE(ActiveRegionLedger::decode(ledger.encode(), edited_reload, error)) << error;
        for (const auto* saved : {&ledger, &edited_reload}) {
            const auto at_edit =
                BuildRegionalFoliageWindows(*saved, {}, {}, {owner}, WorldClock(48));
            EXPECT_EQ(at_edit.elapsed.at(owner), materialized ? 0u : 16u);
            EXPECT_EQ(at_edit.resumes.at(owner), 32u);
        }
        ledger.schedule(WorldClock(52), std::span<const Luminumbra::Vec3>{});
        EXPECT_EQ(ledger.records().at(owner).last_ticked, 52u);
        ActiveRegionLedger reopened;
        ASSERT_TRUE(ActiveRegionLedger::decode(ledger.encode(), reopened, error)) << error;
        for (const auto* saved : {&ledger, &reopened}) {
            const auto windows =
                BuildRegionalFoliageWindows(*saved, {}, {}, {owner}, WorldClock(52));
            EXPECT_EQ(windows.elapsed.at(owner), materialized ? 4u : 20u);
            EXPECT_EQ(windows.resumes.at(owner), 32u);
            if (materialized) {
                // A stale live history cannot replay a completed materialization.
                const auto stale =
                    BuildRegionalFoliageWindows(*saved, {}, {{owner, 40}}, {owner}, WorldClock(52));
                EXPECT_EQ(stale.elapsed.at(owner), 4u);
                EXPECT_THROW(BuildRegionalFoliageWindows(*saved, {}, {}, {owner}, WorldClock(47)),
                             std::invalid_argument);
            }
        }
        if (materialized) {
            ASSERT_TRUE(reopened.acknowledge_resume(owner, 32));
            EXPECT_EQ(reopened.foliage_completion(owner), 48u);
        }
    }
}

TEST(FoliageFieldPages, AlreadyScheduledZeroWindowUsesExclusiveCompletionIncludingTickZero) {
    const RegionKey owner{0, 0};
    for (const std::uint64_t tick :
         {std::uint64_t{0}, std::uint64_t{48}, WorldClock::kTickLimit - 1}) {
        for (const bool materialized : {false, true}) {
            ActiveRegionLedger ledger;
            ledger.activate_owned(owner, WorldClock{});
            if (tick != 0)
                ledger.schedule(WorldClock(tick), std::span<const Luminumbra::Vec3>{});
            if (materialized)
                ledger.mark_materialized_edit(owner, WorldClock(tick));
            else
                ledger.mark_edited(owner, WorldClock(tick));
            EXPECT_EQ(ledger.tick(), tick);
            ActiveRegionLedger reopened;
            std::string error;
            ASSERT_TRUE(ActiveRegionLedger::decode(ledger.encode(), reopened, error)) << error;
            const std::array due{owner};
            for (const auto* saved : {&ledger, &reopened}) {
                const auto windows =
                    BuildRegionalFoliageWindows(*saved, due, {}, {}, WorldClock(tick));
                EXPECT_EQ(windows.elapsed.at(owner), materialized ? 0u : 1u);
                EXPECT_TRUE(windows.resumes.empty());
            }
            if (tick < WorldClock::kTickLimit - 1) {
                reopened.schedule(WorldClock(tick + 1), std::span<const Luminumbra::Vec3>{});
                const auto next =
                    BuildRegionalFoliageWindows(reopened, due, {}, {}, WorldClock(tick + 1));
                EXPECT_EQ(next.elapsed.at(owner), 1u);
            }
        }
    }
}

TEST(FoliageFieldPages, StandaloneWaterSourcesAreLiveTickOnlyAcrossReplayWindows) {
    namespace C = Luminumbra::Components;
    const std::map<RegionKey, std::uint32_t> due{{{0, 0}, 1}};
    for (const std::uint64_t birth : {33u, 48u, 64u}) {
        // Missing growth, tag, or both cannot establish a persisted plant birth.
        for (unsigned components = 0; components < 3; ++components) {
            SCOPED_TRACE(birth);
            SCOPED_TRACE(components);
            entt::registry registry;
            const auto source = registry.create();
            registry.emplace<C::TransformComponent>(source);
            registry.emplace<C::WaterSourceComponent>(source);
            if ((components & 1u) != 0) {
                registry.emplace<C::PlantTag>(source);
            }
            if ((components & 2u) != 0) {
                auto& growth = registry.emplace<C::PlantGrowthComponent>(source);
                growth.planted_tick = growth.last_tick = birth;
            }
            FieldPageState initial;
            ASSERT_TRUE(initial.set_irrigation({0, 0}, 0));
            // Historical windows exclude this emitter, including ticks after
            // its creation. With no birth metadata it is never replayable.
            for (const auto end :
                 {std::optional<std::uint64_t>{64}, std::optional<std::uint64_t>{}}) {
                auto replayed = initial;
                FoliageInteriorReplay::Work replay_work;
                ReplayAllFoliageInteriors(registry, replayed, {{{0, 0}, 32}}, end, &replay_work);
                EXPECT_EQ(replayed.serialize(), initial.serialize());
                EXPECT_EQ(replay_work.source_visits, 0u);
                EXPECT_EQ(replay_work.source_preparations, 1u);
            }
            auto live = initial;
            FoliageInteriorReplay continuous;
            // The source is created at birth in live execution, so the caller
            // starts supplying it then. Compare with independent scalar deposits.
            auto expected = FoliageCells(initial.irrigation());
            for (std::uint64_t tick = 33; tick <= 64; ++tick) {
                if (tick >= birth) {
                    continuous.Step(registry, live, due);
                    expected.at({0, 0}) += 1000;
                } else {
                    continuous.Step(live, due);
                }
                EvolveFoliageIrrigation(expected);
                EXPECT_EQ(live.irrigation().sample({0, 0}, -1), expected.at({0, 0}));
            }
            EXPECT_EQ(continuous.work().source_visits, 65u - birth);
            // Reusing a prepared live cursor for historical work cannot admit
            // an emitter that was already activated by the live tick.
            const auto visits = continuous.work().source_visits;
            continuous.Step(registry, live, due, 65);
            EvolveFoliageIrrigation(expected);
            EXPECT_EQ(live.irrigation().sample({0, 0}, -1), expected.at({0, 0}));
            EXPECT_EQ(continuous.work().source_visits, visits);
        }
    }
}

TEST(FoliageFieldPages, PlantWaterBirthsUsePersistedTicksAtEveryWindowBoundary) {
    namespace C = Luminumbra::Components;
    for (const std::uint64_t birth : {33u, 48u, 64u, 65u}) {
        // Absent ownership uses planted_tick. Matching stamps activate at that
        // stamp; a completed growth stamp activates on its following tick.
        for (unsigned ownership = 0; ownership < 3; ++ownership) {
            SCOPED_TRACE(birth);
            SCOPED_TRACE(ownership);
            entt::registry registry;
            const auto plant = registry.create();
            registry.emplace<C::PlantTag>(plant);
            registry.emplace<C::TransformComponent>(plant);
            auto& growth = registry.emplace<C::PlantGrowthComponent>(plant);
            growth.planted_tick = ownership == 2 ? birth - 2 : birth;
            growth.last_tick = ownership == 2 ? birth - 1 : birth;
            registry.emplace<C::WaterSourceComponent>(plant);
            if (ownership != 0) {
                registry.emplace<C::PlantRegionComponent>(
                    plant, std::uint64_t{1}, 0, 0, ownership == 1 ? birth : birth - 1);
            }
            FieldPageState pages;
            ASSERT_TRUE(pages.set_irrigation({0, 0}, 0));
            auto before_birth = pages;
            ReplayAllFoliageInteriors(registry,
                                      before_birth,
                                      {{{0, 0}, static_cast<std::uint32_t>(birth - 33)}},
                                      birth - 1);
            EXPECT_EQ(before_birth.serialize(), pages.serialize());
            auto continuous = FoliageCells(pages.irrigation());
            for (std::uint64_t tick = 33; tick <= 64; ++tick) {
                if (tick >= birth) {
                    continuous.at({0, 0}) += 1000;
                }
                EvolveFoliageIrrigation(continuous);
            }
            FoliageInteriorReplay::Work replay_work;
            ReplayAllFoliageInteriors(registry, pages, {{{0, 0}, 32}}, 64, &replay_work);
            FieldPageState expected;
            ASSERT_TRUE(expected.set_irrigation({0, 0}, continuous.at({0, 0})));
            EXPECT_EQ(pages.serialize(), expected.serialize());
            EXPECT_EQ(replay_work.source_visits, birth <= 64 ? 65u - birth : 0u);
        }
    }
}

TEST(FoliageFieldPages, BorderExchangeRejectsPartialEndpointsAndInvalidFluxBeforeAnyWrites) {
    FieldPageState pages;
    ASSERT_TRUE(pages.set_irrigation({511, 8}, 800));
    ASSERT_TRUE(pages.set_irrigation({512, 8}, 0));
    const auto bytes = pages.serialize();
    const FoliageEdges partial{{{511, 8}, {512, 8}}, {{511, 9}, {512, 9}}};
    const std::array<std::int32_t, 2> fluxes{{100, 100}};
    EXPECT_THROW(ExchangeFoliageBorders(pages, fluxes, partial), std::invalid_argument);
    const FoliageEdges edge{{{511, 8}, {512, 8}}};
    for (const auto flux :
         {std::numeric_limits<std::int32_t>::min(), std::numeric_limits<std::int32_t>::max()}) {
        const std::array<std::int32_t, 1> invalid{{flux}};
        EXPECT_THROW(ExchangeFoliageBorders(pages, invalid, edge), std::invalid_argument);
    }
    EXPECT_EQ(pages.serialize(), bytes);
}
} // namespace

namespace {
TEST(FoliageFieldPages, WindowPreparationAndEveryCallExcludeDormantPages) {
    FieldPageState pages;
    for (std::size_t index = 0; index < FoliageFieldPages::kMaxPages; ++index) {
        const ScentCellKey cell{0, static_cast<std::int32_t>(index) * 512};
        ASSERT_TRUE(pages.set_soil(cell, 0));
        ASSERT_TRUE(pages.set_irrigation(cell, 1000));
    }
    entt::registry registry;
    const auto dormant = pages.region_bytes({0, 1});
    const std::map<RegionKey, std::uint32_t> windows{{{0, 0}, 65}, {{0, 1}, 0}};
    FoliageWindowReplay replay(pages, windows, 65);
    EXPECT_EQ(replay.work().preparation_pages, 2u);
    EXPECT_EQ(replay.work().preparation_cells, 2u);
    EXPECT_EQ(replay.work().page_probes, 64u);
    for (const auto expected : {33u, 1u, 0u}) {
        const auto evolved = replay.work().evolved_cells;
        const auto work = replay.Advance(registry, pages);
        EXPECT_EQ(replay.remaining_steps(), expected);
        EXPECT_EQ(work.size(), 1u);
        EXPECT_LE(replay.work().evolved_cells - evolved, 64u);
        EXPECT_EQ(replay.work().preparation_pages, 2u);
        EXPECT_EQ(replay.work().page_probes, 64u);
    }
    EXPECT_EQ(replay.work().evolved_cells, 130u);
    EXPECT_EQ(pages.region_bytes({0, 1}), dormant);
}

TEST(FoliageFieldPages, SparsePublicationPreflightsSoilCapacityAndEmptyCandidates) {
    FieldPageState live;
    for (std::size_t index = 0; index < FoliageFieldPages::kMaxPages; ++index) {
        ASSERT_TRUE(live.set_soil({static_cast<std::int32_t>(index * 16), 0}, 7));
    }
    FieldPageState refused;
    ASSERT_TRUE(refused.set_soil({-16, 0}, 11));
    ASSERT_TRUE(refused.set_irrigation({0, 0}, 13));
    const auto bytes = live.serialize();
    EXPECT_FALSE(live.can_adopt_foliage(refused));
    EXPECT_EQ(live.serialize(), bytes);
    FieldPageState empty;
    EXPECT_TRUE(live.can_adopt_foliage(empty));
    live.adopt_foliage(std::move(empty));
    EXPECT_EQ(live.serialize(), bytes);
}
} // namespace
