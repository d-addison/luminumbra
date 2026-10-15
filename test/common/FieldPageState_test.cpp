#include <gtest/gtest.h>

#include "luminumbra_common/fields/FieldPageState.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {
using namespace Luminumbra::world;

using Cell = std::pair<std::int32_t, std::int32_t>;

constexpr std::uint64_t kSeed = 1337;
constexpr std::int32_t kMin = FieldPageState::kMinCell;
constexpr std::int32_t kMax = FieldPageState::kMaxCell;
// FPG1 v2 ambient section: 12-byte file header + 10-byte typed section header.
constexpr std::size_t kHeaderBytes = 12;
constexpr std::size_t kPageBytes = 2600;
constexpr std::size_t kPage0 = kHeaderBytes + 10;
constexpr std::size_t kMaskOffset = 8;
constexpr std::size_t kChannelCount = static_cast<std::size_t>(FieldPageState::kChannels);

std::uint64_t Fnv(std::string_view bytes) {
    std::uint64_t hash = 14695981039346656037ULL;
    for (const char byte : bytes) {
        hash ^= static_cast<std::uint64_t>(static_cast<unsigned char>(byte));
        hash *= 1099511628211ULL;
    }
    return hash;
}

std::uint64_t ReadU(const std::string& bytes, std::size_t offset, std::size_t count) {
    std::uint64_t value = 0;
    for (std::size_t i = 0; i < count; ++i) {
        value |= static_cast<std::uint64_t>(static_cast<unsigned char>(bytes[offset + i]))
                 << (8 * i);
    }
    return value;
}

void WriteU(std::string& bytes, std::size_t offset, std::uint64_t value, std::size_t count) {
    for (std::size_t i = 0; i < count; ++i) {
        bytes[offset + i] =
            static_cast<char>(static_cast<unsigned char>((value >> (8 * i)) & 0xFFU));
    }
}

// Recomputes the footer so the intended defect is the only one in the file.
void Reseal(std::string& bytes) {
    const std::size_t footer = bytes.size() - 8;
    WriteU(bytes, footer, Fnv(std::string_view(bytes).substr(0, footer)), 8);
}

// Region {0,0} at tick 0: 441 cells on four pages (0,0), (0,1), (1,0), (1,1).
FieldPageState ActivatedStore() {
    FieldPageState store;
    EXPECT_TRUE(store.activate_region(RegionKey{0, 0}, kSeed, 0));
    return store;
}

std::string FourPageFile() {
    return ActivatedStore().serialize();
}

// Channel values of the given cells, cell-major then channel, in the order given.
std::vector<std::uint16_t> Values(const FieldPageState& store, const std::vector<Cell>& cells) {
    std::vector<std::uint16_t> values;
    for (const auto& [cell_x, cell_z] : cells) {
        for (int channel = 0; channel < FieldPageState::kChannels; ++channel) {
            std::uint16_t value = 0;
            EXPECT_TRUE(store.sample(channel, cell_x, cell_z, value));
            values.push_back(value);
        }
    }
    return values;
}

std::int32_t Gap(std::size_t channel, std::uint16_t target, std::uint16_t value) {
    const auto numeric = [channel](std::uint16_t word) {
        return channel < 2 ? static_cast<std::int32_t>(std::bit_cast<std::int16_t>(word))
                           : static_cast<std::int32_t>(word);
    };
    const std::int32_t difference = numeric(target) - numeric(value);
    return difference < 0 ? -difference : difference;
}

// True when parse refuses the bytes, reports an error and leaves the existing pages untouched.
bool RefusedUntouched(const std::string& bytes) {
    FieldPageState out;
    EXPECT_TRUE(out.activate_region(RegionKey{5, 5}, kSeed, 0));
    const auto before = out.pages();
    std::string error;
    const bool accepted = FieldPageState::parse(bytes, out, error);
    return !accepted && !error.empty() && out.pages() == before;
}

} // namespace

TEST(FieldPageStateTest, RegionOfCellFloorsAndSplitsAtRegionEdges) {
    EXPECT_EQ(FieldPageState::region_of_cell(0, 0), (RegionKey{0, 0}));
    EXPECT_EQ(FieldPageState::region_of_cell(20, 20), (RegionKey{0, 0}));
    EXPECT_EQ(FieldPageState::region_of_cell(21, 0), (RegionKey{1, 0}));
    EXPECT_EQ(FieldPageState::region_of_cell(-1, 0), (RegionKey{-1, 0}));
    EXPECT_EQ(FieldPageState::region_of_cell(-21, 0), (RegionKey{-1, 0}));
    EXPECT_EQ(FieldPageState::region_of_cell(-22, 0), (RegionKey{-2, 0}));
    EXPECT_EQ(FieldPageState::region_of_cell(kMax, kMin), (RegionKey{32767, -32768}));
}

TEST(FieldPageStateTest, CellsOfRegionHaveTwentyOneOrTwentyTwoCellsPerSide) {
    const auto origin = FieldPageState::cells_of_region(RegionKey{0, 0});
    ASSERT_EQ(origin.size(), 21u * 21u);
    EXPECT_EQ(origin.front(), (Cell{0, 0}));
    EXPECT_EQ(origin.back(), (Cell{20, 20}));

    const auto east = FieldPageState::cells_of_region(RegionKey{1, 0});
    ASSERT_EQ(east.size(), 22u * 21u);
    EXPECT_EQ(east.front(), (Cell{21, 0}));
    EXPECT_EQ(east.back(), (Cell{42, 20}));
}

TEST(FieldPageStateTest, CellsOfRegionAreAscendingByZThenX) {
    const auto cells = FieldPageState::cells_of_region(RegionKey{1, -1});
    for (std::size_t i = 1; i < cells.size(); ++i) {
        const Cell previous{cells[i - 1].second, cells[i - 1].first};
        const Cell current{cells[i].second, cells[i].first};
        EXPECT_LT(previous, current);
    }
}

TEST(FieldPageStateTest, RegionsAtTheValidEdgeOwnTheRangeEndsAndBeyondOwnNothing) {
    const auto low = FieldPageState::cells_of_region(RegionKey{-32768, 0});
    ASSERT_FALSE(low.empty());
    EXPECT_EQ(low.front(), (Cell{kMin, 0}));

    const auto high = FieldPageState::cells_of_region(RegionKey{32767, 0});
    ASSERT_FALSE(high.empty());
    EXPECT_EQ(high.back(), (Cell{kMax, 20}));

    EXPECT_TRUE(FieldPageState::cells_of_region(RegionKey{32768, 0}).empty());
    EXPECT_TRUE(FieldPageState::cells_of_region(RegionKey{-32769, 0}).empty());
}

TEST(FieldPageStateTest, NeighbouringRegionsPartitionTheCellRectangle) {
    std::set<Cell> seen;
    std::size_t total = 0;
    for (std::int32_t z = -2; z <= 2; ++z) {
        for (std::int32_t x = -2; x <= 2; ++x) {
            const auto cells = FieldPageState::cells_of_region(RegionKey{x, z});
            total += cells.size();
            seen.insert(cells.begin(), cells.end());
        }
    }
    // Cells -43..63 on each axis: 107 per side, and no cell is owned twice.
    EXPECT_EQ(total, seen.size());
    EXPECT_EQ(seen.size(), 107u * 107u);
    EXPECT_EQ(seen.count(Cell{-43, -43}), 1u);
    EXPECT_EQ(seen.count(Cell{63, 63}), 1u);
}

TEST(FieldPageStateTest, SeedValueGoldensAreStable) {
    EXPECT_EQ(FieldPageState::seed_value(1337, 0, 0, 0, 0), 7333);
    EXPECT_EQ(FieldPageState::seed_value(1337, 4, -1, 5, 3), 39276);
    EXPECT_EQ(FieldPageState::seed_value(4294967295ULL, 2, 699050, -699051, 123456789ULL), 29741);
}

TEST(FieldPageStateTest, SeedValueIsDeterministicAndSensitiveToEachInput) {
    EXPECT_EQ(FieldPageState::seed_value(kSeed, 1, 3, 4, 5),
              FieldPageState::seed_value(kSeed, 1, 3, 4, 5));

    std::set<std::uint16_t> by_channel;
    std::set<std::uint16_t> by_cell_x;
    std::set<std::uint16_t> by_cell_z;
    std::set<std::uint16_t> by_epoch;
    std::set<std::uint16_t> by_seed;
    for (std::int32_t i = 0; i < 64; ++i) {
        by_channel.insert(FieldPageState::seed_value(kSeed, i, 3, 4, 5));
        by_cell_x.insert(FieldPageState::seed_value(kSeed, 1, i, 4, 5));
        by_cell_z.insert(FieldPageState::seed_value(kSeed, 1, 3, i, 5));
        const auto epoch = static_cast<std::uint64_t>(i);
        by_epoch.insert(FieldPageState::seed_value(kSeed, 1, 3, 4, epoch));
        by_seed.insert(FieldPageState::seed_value(kSeed + epoch, 1, 3, 4, 5));
    }
    EXPECT_GT(by_channel.size(), 1u);
    EXPECT_GT(by_cell_x.size(), 1u);
    EXPECT_GT(by_cell_z.size(), 1u);
    EXPECT_GT(by_epoch.size(), 1u);
    EXPECT_GT(by_seed.size(), 1u);
}

TEST(FieldPageStateTest, ActivateSeedsOwnedCellsOnceAndNeverTouchesThemAgain) {
    FieldPageState store;
    EXPECT_TRUE(store.activate_region(RegionKey{0, 0}, kSeed, 0));
    EXPECT_EQ(store.page_count(), 4u);
    EXPECT_EQ(store.owned_initialized_cells(RegionKey{0, 0}), 441u);
    EXPECT_TRUE(store.owns_region(RegionKey{0, 0}));
    EXPECT_FALSE(store.owns_region(RegionKey{5, 5}));

    std::uint16_t value = 0;
    ASSERT_TRUE(store.sample(2, 7, 9, value));
    EXPECT_EQ(value, FieldPageState::seed_value(kSeed, 2, 7, 9, 0));

    const std::string before = store.region_bytes(RegionKey{0, 0});
    EXPECT_TRUE(store.activate_region(RegionKey{0, 0}, kSeed, 8));
    EXPECT_EQ(store.region_bytes(RegionKey{0, 0}), before);
    EXPECT_EQ(store.page_count(), 4u);
}

TEST(FieldPageStateTest, ActivatingANeighbourAddsOnlyItsMissingPages) {
    FieldPageState store = ActivatedStore();
    EXPECT_TRUE(store.activate_region(RegionKey{1, 0}, kSeed, 0));
    // Region {1,0} spans page columns 1 and 2, rows 0 and 1; columns 1 already exist.
    EXPECT_EQ(store.page_count(), 6u);
    EXPECT_EQ(store.owned_initialized_cells(RegionKey{1, 0}), 22u * 21u);
}

TEST(FieldPageStateTest, RegionsSharingAPhysicalPageOwnDisjointCells) {
    const auto a_cells = FieldPageState::cells_of_region(RegionKey{0, 0});
    const auto b_cells = FieldPageState::cells_of_region(RegionKey{1, 0});
    const std::set<Cell> a_set(a_cells.begin(), a_cells.end());
    for (const Cell& cell : b_cells) {
        EXPECT_EQ(a_set.count(cell), 0u);
    }

    FieldPageState store = ActivatedStore();
    // Region {1,0} has no initialized cells yet, so stepping it does nothing.
    EXPECT_EQ(store.step_region(RegionKey{1, 0}, kSeed, 8), 0u);
    EXPECT_TRUE(store.activate_region(RegionKey{1, 0}, kSeed, 0));

    const std::string b_before = store.region_bytes(RegionKey{1, 0});
    const std::string a_before = store.region_bytes(RegionKey{0, 0});
    ASSERT_EQ(store.step_region(RegionKey{0, 0}, kSeed, 8), 441u);
    EXPECT_EQ(store.region_bytes(RegionKey{1, 0}), b_before);
    EXPECT_NE(store.region_bytes(RegionKey{0, 0}), a_before);
}

TEST(FieldPageStateTest, StepMovesEveryOwnedCellTowardItsEpochTarget) {
    const auto cells = FieldPageState::cells_of_region(RegionKey{0, 0});
    for (const std::uint64_t tick : {0ULL, 7ULL, 8ULL, 9ULL, 16ULL}) {
        FieldPageState store = ActivatedStore();
        const std::vector<std::uint16_t> before = Values(store, cells);
        EXPECT_EQ(store.step_region(RegionKey{0, 0}, kSeed, tick), cells.size());
        const std::vector<std::uint16_t> after = Values(store, cells);
        const std::uint64_t epoch = tick / FieldPageState::kCadenceTicks;

        for (std::size_t c = 0; c < cells.size(); ++c) {
            for (std::size_t channel = 0; channel < kChannelCount; ++channel) {
                const std::size_t i = c * kChannelCount + channel;
                const std::uint16_t target = FieldPageState::seed_value(
                    kSeed, static_cast<int>(channel), cells[c].first, cells[c].second, epoch);
                EXPECT_LE(Gap(channel, target, after[i]), Gap(channel, target, before[i]));
            }
        }
    }
}

TEST(FieldPageStateTest, StepInTheSameEpochAsActivationChangesNothing) {
    FieldPageState store = ActivatedStore();
    const std::string before = store.region_bytes(RegionKey{0, 0});
    // Ticks 0..7 share epoch 0, the epoch the cells were seeded with.
    EXPECT_EQ(store.step_region(RegionKey{0, 0}, kSeed, 7), 441u);
    EXPECT_EQ(store.region_bytes(RegionKey{0, 0}), before);
}

TEST(FieldPageStateTest, TwoStepsInOneEpochConvergeMonotonically) {
    const auto cells = FieldPageState::cells_of_region(RegionKey{0, 0});
    FieldPageState store = ActivatedStore();
    const std::vector<std::uint16_t> v0 = Values(store, cells);
    store.step_region(RegionKey{0, 0}, kSeed, 8);
    const std::vector<std::uint16_t> v1 = Values(store, cells);
    store.step_region(RegionKey{0, 0}, kSeed, 9);
    const std::vector<std::uint16_t> v2 = Values(store, cells);

    for (std::size_t c = 0; c < cells.size(); ++c) {
        for (std::size_t channel = 0; channel < kChannelCount; ++channel) {
            const std::size_t i = c * kChannelCount + channel;
            const std::uint16_t target = FieldPageState::seed_value(
                kSeed, static_cast<int>(channel), cells[c].first, cells[c].second, 1);
            EXPECT_LE(Gap(channel, target, v1[i]), Gap(channel, target, v0[i]));
            EXPECT_LE(Gap(channel, target, v2[i]), Gap(channel, target, v1[i]));
        }
    }
}

TEST(FieldPageStateTest, ActivationIsRefusedAtCapacityAndLeavesStateUntouched) {
    FieldPageState store;
    RegionKey refused{};
    bool was_refused = false;
    for (std::int32_t i = 0; i < 128 && !was_refused; ++i) {
        for (std::int32_t j = 0; j < 128 && !was_refused; ++j) {
            const RegionKey key{2 * i, 2 * j};
            const std::size_t before = store.page_count();
            if (!store.activate_region(key, kSeed, 0)) {
                was_refused = true;
                refused = key;
                EXPECT_EQ(store.page_count(), before);
            }
        }
    }
    ASSERT_TRUE(was_refused);
    EXPECT_LE(store.page_count(), FieldPageState::kMaxPages);

    // The refused region owns nothing and steps nothing; the first region still steps.
    EXPECT_EQ(store.owned_initialized_cells(refused), 0u);
    EXPECT_EQ(store.step_region(refused, kSeed, 8), 0u);
    EXPECT_EQ(store.step_region(RegionKey{0, 0}, kSeed, 8), 441u);
}

TEST(FieldPageStateTest, SampleRefusesBadChannelsAndUninitializedCells) {
    const FieldPageState store = ActivatedStore();
    std::uint16_t value = 1234;
    EXPECT_FALSE(store.sample(-1, 0, 0, value));
    EXPECT_FALSE(store.sample(FieldPageState::kChannels, 0, 0, value));
    EXPECT_FALSE(store.sample(0, 100, 100, value));
    EXPECT_EQ(value, 1234);
    EXPECT_FALSE(store.initialized(100, 100));
    EXPECT_TRUE(store.initialized(0, 0));
}

TEST(FieldPageStateTest, SerializeParseRoundTripIsByteIdentical) {
    FieldPageState store = ActivatedStore();
    EXPECT_TRUE(store.activate_region(RegionKey{1, 0}, kSeed, 0));
    const std::string bytes = store.serialize();

    FieldPageState loaded;
    std::string error;
    ASSERT_TRUE(FieldPageState::parse(bytes, loaded, error)) << error;
    EXPECT_EQ(loaded.pages(), store.pages());
    EXPECT_EQ(loaded.serialize(), bytes);
    EXPECT_EQ(loaded.canonical_bytes(), store.canonical_bytes());
}

TEST(FieldPageStateTest, EmptyStoreWritesNothing) {
    const FieldPageState store{};
    EXPECT_TRUE(store.empty());
    EXPECT_EQ(store.serialize(), "");
    EXPECT_EQ(store.canonical_bytes(), "");
    EXPECT_TRUE(store.region_bytes(RegionKey{0, 0}).empty());
}

TEST(FieldPageStateTest, CanonicalAndRegionBytesAreStableAndFollowCellChanges) {
    FieldPageState store = ActivatedStore();
    const std::string canonical = store.canonical_bytes();
    EXPECT_EQ(canonical, std::string("field_pages:v2:") + store.serialize());
    EXPECT_EQ(store.canonical_bytes(), canonical);

    const std::string region = store.region_bytes(RegionKey{0, 0});
    // 441 cells, each i32 x, i32 z and five u16 channels: 18 bytes per cell.
    EXPECT_EQ(region.size(), 441u * 18u);
    EXPECT_EQ(store.region_bytes(RegionKey{0, 0}), region);
    ASSERT_GT(store.step_region(RegionKey{0, 0}, kSeed, 8), 0u);
    EXPECT_NE(store.region_bytes(RegionKey{0, 0}), region);
}

TEST(FieldPageStateTest, ParseRefusesEmptyTruncatedAndOversizedInput) {
    EXPECT_TRUE(RefusedUntouched(""));
    EXPECT_TRUE(RefusedUntouched("FPG1"));
    EXPECT_TRUE(RefusedUntouched(std::string(FieldPageState::kMaxFileBytes + 1, 'F')));
    EXPECT_TRUE(RefusedUntouched(std::string(100, 'Z')));
}

TEST(FieldPageStateTest, ParseRefusesWrongMagicVersionAndChannelCount) {
    std::string bytes = FourPageFile();
    bytes[0] = 'X';
    Reseal(bytes);
    EXPECT_TRUE(RefusedUntouched(bytes));

    bytes = FourPageFile();
    WriteU(bytes, 4, 1, 2);
    Reseal(bytes);
    EXPECT_TRUE(RefusedUntouched(bytes));

    bytes = FourPageFile();
    WriteU(bytes, 6, 4, 2);
    Reseal(bytes);
    EXPECT_TRUE(RefusedUntouched(bytes));
}

TEST(FieldPageStateTest, ParseRefusesPageCountAboveCap) {
    std::string bytes = FourPageFile();
    WriteU(bytes, kHeaderBytes + 6, FieldPageState::kMaxPages + 1, 4);
    Reseal(bytes);
    EXPECT_TRUE(RefusedUntouched(bytes));
}

TEST(FieldPageStateTest, ParseRefusesLengthMismatchAndFooterMismatch) {
    std::string shorter = FourPageFile();
    shorter.erase(shorter.size() - 9, 1);
    Reseal(shorter);
    EXPECT_TRUE(RefusedUntouched(shorter));

    std::string corrupt = FourPageFile();
    corrupt.back() = static_cast<char>(corrupt.back() ^ 1);
    EXPECT_TRUE(RefusedUntouched(corrupt));
}

TEST(FieldPageStateTest, ParseRefusesKeysNotStrictlyAscending) {
    std::string bytes = FourPageFile();
    for (std::size_t i = 0; i < 8; ++i) {
        std::swap(bytes[kPage0 + i], bytes[kPage0 + kPageBytes + i]);
    }
    Reseal(bytes);
    EXPECT_TRUE(RefusedUntouched(bytes));
}

TEST(FieldPageStateTest, ParseRefusesAnEmptyMask) {
    std::string bytes = FourPageFile();
    for (std::size_t i = kPage0 + kMaskOffset; i < kPage0 + kPageBytes; ++i) {
        bytes[i] = '\0';
    }
    Reseal(bytes);
    EXPECT_TRUE(RefusedUntouched(bytes));
}

TEST(FieldPageStateTest, ParseRefusesAValueOnAnUninitializedCell) {
    std::string bytes = FourPageFile();
    // Clear the mask bit of cell 0 on page 0; its channel 0 value (7333) stays behind.
    WriteU(bytes, kPage0 + kMaskOffset, ReadU(bytes, kPage0 + kMaskOffset, 8) & ~1ULL, 8);
    Reseal(bytes);
    EXPECT_TRUE(RefusedUntouched(bytes));
}

TEST(FieldPageStateTest, ParseRefusesAnInitializedCellOutsideTheValidRange) {
    std::string bytes = FourPageFile();
    // Page x 43700 covers cells from 699200, beyond kMaxCell. Keys stay ascending.
    WriteU(bytes, kPage0 + 3 * kPageBytes, 43700, 4);
    Reseal(bytes);
    EXPECT_TRUE(RefusedUntouched(bytes));
}

TEST(FieldPageStateTest, ParseRefusesAZeroPageFile) {
    const char header[] = {'F', 'P', 'G', '1', 2, 0, 31, 0, 0, 0, 0, 0};
    std::string bytes(header, sizeof(header));
    bytes.append(8, '\0');
    Reseal(bytes);
    EXPECT_TRUE(RefusedUntouched(bytes));
}

TEST(FieldPageStateTest, ParseAcceptsTheUntouchedFourPageFile) {
    const std::string bytes = FourPageFile();
    FieldPageState loaded;
    std::string error;
    EXPECT_TRUE(FieldPageState::parse(bytes, loaded, error)) << error;
    EXPECT_EQ(loaded.page_count(), 4u);
}

TEST(FieldPageStateTest, TypedEvolutionSectionsRoundTripExactBitsWithoutAmbientPages) {
    FieldPageState store;
    RegionalStorm storm;
    storm.owner = {3, -4};
    storm.center_x = std::bit_cast<std::uint32_t>(1600.25f);
    storm.center_z = std::bit_cast<std::uint32_t>(-1800.5f);
    storm.velocity_x = std::bit_cast<std::uint32_t>(-3.125f);
    storm.velocity_z = std::bit_cast<std::uint32_t>(2.5f);
    storm.intensity = std::bit_cast<std::uint32_t>(0.75f);
    storm.spawn_tick = 20;
    storm.lifetime_ticks = 100;
    storm.active_age = 9;
    storm.seed_salt = 31;
    RegionalStrike strike;
    strike.owner = storm.owner;
    strike.strike_tick = 29;
    strike.world_x = storm.center_x;
    strike.world_z = storm.center_z;
    strike.magnitude = std::bit_cast<std::uint32_t>(0.5f);
    strike.storm_salt = 31;
    strike.active_age = 1;
    ASSERT_TRUE(store.replace_weather({storm}, {strike}));
    const ScentCell values{
        std::bit_cast<std::uint64_t>(0.125), std::bit_cast<std::uint64_t>(100.5), 0, 0};
    ASSERT_TRUE(store.set_scent({-1, 512}, values));
    EXPECT_TRUE(store.owns_region(storm.owner));
    EXPECT_TRUE(store.owns_region({-1, 1}));
    const auto bytes = store.serialize();
    EXPECT_EQ(ReadU(bytes, 4, 2), 2u);
    EXPECT_EQ(ReadU(bytes, 8, 4), 3u);
    FieldPageState restored;
    std::string error;
    ASSERT_TRUE(FieldPageState::parse(bytes, restored, error)) << error;
    EXPECT_EQ(restored.storms(), store.storms());
    EXPECT_EQ(restored.strikes(), store.strikes());
    EXPECT_EQ(restored.scent_cells(), store.scent_cells());
    EXPECT_EQ(restored.serialize(), bytes);
    EXPECT_EQ(restored.region_bytes(storm.owner), store.region_bytes(storm.owner));
    EXPECT_FALSE(restored.region_bytes({-1, 1}).empty());
}

TEST(FieldPageStateTest, TypedSectionsRefuseUnsupportedTypesAndInvalidEvolutionAtomically) {
    FieldPageState store;
    ScentCell valid{std::bit_cast<std::uint64_t>(1.0), 0, 0, 0};
    ASSERT_TRUE(store.set_scent({0, 0}, valid));
    const auto bytes = store.serialize();
    for (const auto type : {0u, 5u, 6u}) {
        auto corrupt = bytes;
        WriteU(corrupt, 12, type, 2);
        Reseal(corrupt);
        EXPECT_TRUE(RefusedUntouched(corrupt));
    }
    auto nonfinite = bytes;
    WriteU(nonfinite, 30, std::bit_cast<std::uint64_t>(std::numeric_limits<double>::infinity()), 8);
    Reseal(nonfinite);
    EXPECT_TRUE(RefusedUntouched(nonfinite));
    EXPECT_FALSE(store.set_scent({0, 0}, {std::bit_cast<std::uint64_t>(-0.0), 0, 0, 0}));
    EXPECT_EQ(store.serialize(), bytes);
    EXPECT_TRUE(store.set_scent({0, 0}, {}));
    EXPECT_TRUE(store.empty());
}

TEST(FieldPageStateTest, BorderExchangeUsesGlobalCommonPhaseAndChargesBothOwners) {
    FieldPageState store;
    const RegionKey left{0, 0}, right{1, 0};
    ASSERT_TRUE(store.activate_region(left, kSeed, 0));
    ASSERT_TRUE(store.activate_region(right, kSeed, 0));
    std::map<RegionKey, ActiveRegionRecord> records;
    records[left].state = records[right].state = RegionState::Reduced;
    records[left].cadence_shift = 1;
    records[left].cadence_phase = 1;
    records[right].cadence_shift = 2;
    records[right].cadence_phase = 3;
    const auto before = store.serialize();
    // Neither interior is due, yet the edge runs at t mod 4 == 0.
    EXPECT_TRUE(store.step_scheduled(records, {}, kSeed, 2).empty());
    EXPECT_EQ(store.serialize(), before);
    std::array<std::int64_t, FieldPageState::kChannels> sum_before{};
    for (const auto& [key, page] : store.pages()) {
        (void)key;
        for (int c = 0; c < FieldPageState::kChannels; ++c)
            for (const auto value : page.cells[c])
                sum_before[c] +=
                    c < 2 ? static_cast<std::int64_t>(std::bit_cast<std::int16_t>(value)) : value;
    }
    const auto work = store.step_scheduled(records, {}, kSeed, 4);
    ASSERT_EQ(work.size(), 2u);
    EXPECT_EQ(work.at(left), 21u);
    EXPECT_EQ(work.at(right), 21u);
    EXPECT_NE(store.serialize(), before);
    std::array<std::int64_t, FieldPageState::kChannels> sum_after{};
    for (const auto& [key, page] : store.pages()) {
        (void)key;
        for (int c = 0; c < FieldPageState::kChannels; ++c)
            for (const auto value : page.cells[c])
                sum_after[c] +=
                    c < 2 ? static_cast<std::int64_t>(std::bit_cast<std::int16_t>(value)) : value;
    }
    EXPECT_EQ(sum_after, sum_before);
    records[right].state = RegionState::Frozen;
    const auto sealed = store.serialize();
    EXPECT_TRUE(store.step_scheduled(records, {}, kSeed, 8).empty());
    EXPECT_EQ(store.serialize(), sealed);
}

TEST(FieldPageStateTest, WeatherClockPersistsWithoutStormsAndRefusesInvalidAges) {
    FieldPageState store;
    ASSERT_TRUE(store.set_weather_age({0, 0}, 44));
    EXPECT_FALSE(store.empty());
    EXPECT_TRUE(store.owns_region({0, 0}));
    EXPECT_TRUE(store.storms().empty());
    const auto bytes = store.serialize();
    EXPECT_EQ(ReadU(bytes, 12, 2), 7u);
    FieldPageState restored;
    std::string error;
    ASSERT_TRUE(FieldPageState::parse(bytes, restored, error)) << error;
    EXPECT_EQ(restored.weather_clocks(), store.weather_clocks());
    EXPECT_EQ(restored.region_bytes({0, 0}), store.region_bytes({0, 0}));
    auto corrupt = bytes;
    WriteU(corrupt, 30, WorldClock::kTickLimit, 8);
    Reseal(corrupt);
    EXPECT_TRUE(RefusedUntouched(corrupt));
    EXPECT_FALSE(store.set_weather_age({0, 0}, WorldClock::kTickLimit));
    EXPECT_EQ(store.serialize(), bytes);
}

TEST(FieldPageStateTest, NewActivationCannotSupplyPreviousTickBorderExchange) {
    FieldPageState store;
    const RegionKey left{0, 0}, right{1, 0};
    ASSERT_TRUE(store.activate_region(left, kSeed, 0));
    const auto previous = store;
    ASSERT_TRUE(store.activate_region(right, kSeed, 4));
    std::map<RegionKey, ActiveRegionRecord> records{{left, {}}, {right, {}}};
    const auto before = store.serialize();
    EXPECT_TRUE(store.step_scheduled(records, {}, kSeed, 4, &previous).empty());
    EXPECT_EQ(store.serialize(), before);
}

TEST(FieldPageStateTest, ScheduledSnapshotMatchesFullSealedInputAndWork) {
    FieldPageState initial;
    std::map<RegionKey, ActiveRegionRecord> records;
    for (int z = -1; z <= 1; ++z)
        for (int x = -1; x <= 1; ++x) {
            const RegionKey key{x, z};
            ASSERT_TRUE(initial.activate_region(key, kSeed, 0));
            auto& record = records[key];
            record.state = x == -1 ? RegionState::Frozen : RegionState::Reduced;
            record.cadence_shift = static_cast<std::uint8_t>(z == 0 ? 1 : 2);
            record.cadence_phase = ActiveRegionLedger::phase(key, record.cadence_shift);
        }
    records[{0, 0}].state = RegionState::Active;
    records[{0, 0}].cadence_shift = std::uint8_t{0};
    records[{0, 0}].cadence_phase = std::uint32_t{0};
    // Absent authority and distant frozen pages must not join the sealed working set.
    ASSERT_TRUE(initial.activate_region({20, 20}, kSeed, 0));
    ASSERT_TRUE(initial.activate_region({100, 100}, kSeed, 0));
    records[{100, 100}].state = RegionState::Frozen;
    // This owner is admitted only AFTER the snapshot, including a shared page boundary.
    records[{2, 0}] = {};
    ASSERT_TRUE(initial.set_scent({0, 0}, {std::bit_cast<std::uint64_t>(1.0), 0, 0, 0}));
    ASSERT_TRUE(initial.set_weather_age({0, 0}, 19));
    for (std::uint64_t tick = 1; tick <= 8; ++tick) {
        SCOPED_TRACE(tick);
        std::vector<RegionKey> due;
        for (const auto& [key, record] : records)
            if (record.state != RegionState::Frozen &&
                (tick & ((std::uint64_t{1} << record.cadence_shift) - 1)) == record.cadence_phase)
                due.push_back(key);
        const auto full_previous = initial;
        const auto sparse_previous = initial.scheduled_snapshot(records, due, tick);
        EXPECT_LT(sparse_previous.page_count(), full_previous.page_count());
        EXPECT_TRUE(sparse_previous.scent_cells().empty());
        EXPECT_TRUE(sparse_previous.weather_clocks().empty());
        auto full = initial, sparse = initial;
        for (const auto key : due) {
            ASSERT_TRUE(full.activate_region(key, kSeed, tick));
            ASSERT_TRUE(sparse.activate_region(key, kSeed, tick));
            EXPECT_EQ(
                full.owned_initialized_cells(key) - full_previous.owned_initialized_cells(key),
                sparse.owned_initialized_cells(key) - sparse_previous.owned_initialized_cells(key));
        }
        EXPECT_EQ(full.step_scheduled(records, due, kSeed, tick, &full_previous),
                  sparse.step_scheduled(records, due, kSeed, tick, &sparse_previous));
        EXPECT_EQ(full.serialize(), sparse.serialize());
        // Also exercise standalone and aliased calls, which must seal their own input.
        auto standalone = initial, aliased = initial, reference = initial;
        const auto work = reference.step_scheduled(records, due, kSeed, tick, &full_previous);
        EXPECT_EQ(standalone.step_scheduled(records, due, kSeed, tick), work);
        EXPECT_EQ(aliased.step_scheduled(records, due, kSeed, tick, &aliased), work);
        EXPECT_EQ(standalone.serialize(), reference.serialize());
        EXPECT_EQ(aliased.serialize(), reference.serialize());
    }
}

TEST(FieldPageStateTest, ScentBatchMatchesOrderedAdmissionsAndRejectsAtomically) {
    auto store = ActivatedStore();
    const ScentCell one{std::bit_cast<std::uint64_t>(1.0), 0, 0, 0};
    const ScentCell two{0, std::bit_cast<std::uint64_t>(2.0), 0, 0};
    ASSERT_TRUE(store.set_scent({-1, -1}, one));
    ASSERT_TRUE(store.set_scent({0, 0}, one));
    auto expected = store;
    const std::vector<std::pair<ScentCellKey, ScentCell>> updates{
        {{-1, -1}, {}}, {{0, 0}, two}, {{-2, 1}, one}, {{1, 1}, two}};
    for (const auto& [key, values] : updates)
        ASSERT_TRUE(expected.set_scent(key, values));
    ASSERT_TRUE(store.update_scent(updates));
    EXPECT_EQ(store.serialize(), expected.serialize());
    const auto before = store.serialize();
    auto invalid = updates;
    invalid[1].second = one;
    invalid.back().second[0] = std::bit_cast<std::uint64_t>(-0.0);
    EXPECT_FALSE(store.update_scent(invalid));
    EXPECT_EQ(store.serialize(), before);
    invalid = updates;
    invalid[1].second = one;
    invalid.push_back(updates.back()); // duplicate admission
    EXPECT_FALSE(store.update_scent(invalid));
    EXPECT_EQ(store.serialize(), before);
}

TEST(FieldPageStateTest, DenseScentBatchPreservesBytesAcrossEraseInsertAndMovingDomains) {
    FieldPageState store, reference;
    for (int tick = 0; tick < 6; ++tick) {
        std::vector<std::pair<ScentCellKey, ScentCell>> updates;
        for (int z = -8 + tick; z < 8 + tick; ++z)
            for (int x = -8 - tick; x < 8 - tick; ++x) {
                ScentCell value{};
                if ((x + z + tick) % 3 != 0)
                    value[tick % 4] = std::bit_cast<std::uint64_t>(1.0 + tick);
                updates.emplace_back(ScentCellKey{x, z}, value);
                ASSERT_TRUE(reference.set_scent({x, z}, value));
            }
        ASSERT_TRUE(store.update_scent(updates));
        EXPECT_EQ(store.serialize(), reference.serialize());
        EXPECT_EQ(store.canonical_bytes(), reference.canonical_bytes());
    }
}

TEST(FieldPageStateTest, FoliageSnapshotAndRollbackPreserveAllBytesWithoutCopyingAmbientState) {
    auto store = ActivatedStore();
    ASSERT_TRUE(store.set_scent({-1, -1}, {std::bit_cast<std::uint64_t>(1.25), 0, 0, 0}));
    ASSERT_TRUE(store.set_weather_age({0, 0}, 13));
    ASSERT_TRUE(store.set_soil({-1, -1}, 999997));
    ASSERT_TRUE(store.set_irrigation({0, 0}, 12345));
    const auto before = store.serialize();
    auto saved = store.foliage_snapshot();
    EXPECT_TRUE(saved.pages().empty());
    EXPECT_TRUE(saved.scent_cells().empty());
    EXPECT_TRUE(saved.storms().empty());
    EXPECT_TRUE(saved.strikes().empty());
    EXPECT_TRUE(saved.weather_clocks().empty());
    EXPECT_EQ(saved.soil().records(), store.soil().records());
    EXPECT_EQ(saved.irrigation().records(), store.irrigation().records());
    // Cover both existing-cell evolution and admission of new pages during a birth.
    ASSERT_TRUE(store.set_soil({-1, -1}, 888888));
    ASSERT_TRUE(store.set_irrigation({512, 512}, 100));
    EXPECT_EQ(saved.soil().sample({-1, -1}, 0), 999997);
    EXPECT_FALSE(saved.irrigation().initialized({512, 512}));
    store.restore_foliage(std::move(saved));
    EXPECT_EQ(store.serialize(), before);
}

TEST(FieldPageStateTest, WeatherClockBatchRefusesBeforeChangingAnyAuthority) {
    auto store = ActivatedStore();
    ASSERT_TRUE(store.set_weather_age({0, 0}, 19));
    const auto before = store.serialize();
    EXPECT_FALSE(store.replace_weather({}, {}, {{{0, 0}, WorldClock::kTickLimit}}));
    EXPECT_EQ(store.serialize(), before);
    EXPECT_FALSE(store.replace_weather({}, {}, {{{32768, 0}, 1}}));
    EXPECT_EQ(store.serialize(), before);
    auto expected = store;
    ASSERT_TRUE(expected.set_weather_age({0, 0}, 20));
    ASSERT_TRUE(store.replace_weather({}, {}, {{{0, 0}, 20}}));
    EXPECT_EQ(store.serialize(), expected.serialize());
}

TEST(FieldPageStateTest, WeatherCapacityIsOwnerAwareAndRefusesAtomically) {
    FieldPageState store;
    std::vector<RegionalStorm> storms;
    std::vector<RegionalStrike> strikes;
    for (int owner = 0; owner < 2; ++owner) {
        for (std::uint32_t salt = 0; salt < FieldPageState::kMaxOwnerStorms; ++salt)
            storms.push_back(
                {{owner, 0},
                 std::bit_cast<std::uint32_t>(static_cast<float>(owner) * 512.0f + 256.0f),
                 std::bit_cast<std::uint32_t>(256.0f),
                 0,
                 0,
                 0,
                 0,
                 240,
                 static_cast<std::uint32_t>(owner * 100) + salt,
                 0});
        for (std::uint32_t salt = 0; salt < FieldPageState::kMaxOwnerStrikes; ++salt)
            strikes.push_back(
                {{owner, 0},
                 0,
                 std::bit_cast<std::uint32_t>(static_cast<float>(owner) * 512.0f + 256.0f),
                 std::bit_cast<std::uint32_t>(256.0f),
                 0,
                 static_cast<std::uint32_t>(owner * 100) + salt,
                 0});
    }
    // Both owners can retain their full allowance; global legacy 16/32 caps would fail this.
    ASSERT_TRUE(store.replace_weather(storms, strikes));
    const auto before = store.serialize();
    FieldPageState restored;
    std::string error;
    ASSERT_TRUE(FieldPageState::parse(before, restored, error)) << error;
    EXPECT_EQ(restored.serialize(), before);
    auto excess_storms = storms;
    excess_storms.push_back({{0, 0},
                             std::bit_cast<std::uint32_t>(256.0f),
                             std::bit_cast<std::uint32_t>(256.0f),
                             0,
                             0,
                             0,
                             0,
                             240,
                             99,
                             0});
    EXPECT_FALSE(store.replace_weather(excess_storms, strikes));
    EXPECT_EQ(store.serialize(), before);
    auto excess_strikes = strikes;
    excess_strikes.push_back({{0, 0},
                              0,
                              std::bit_cast<std::uint32_t>(256.0f),
                              std::bit_cast<std::uint32_t>(256.0f),
                              0,
                              99,
                              0});
    EXPECT_FALSE(store.replace_weather(storms, excess_strikes));
    EXPECT_EQ(store.serialize(), before);
    EXPECT_FALSE(
        store.replace_weather(std::vector<RegionalStorm>(FieldPageState::kMaxStorms + 1), {}));
    EXPECT_FALSE(
        store.replace_weather({}, std::vector<RegionalStrike>(FieldPageState::kMaxStrikes + 1)));
    EXPECT_EQ(store.serialize(), before);
}

namespace {
using PageMap = std::map<FieldPageKey, FieldPage>;
struct ReferenceSlot {
    FieldPageKey key;
    std::size_t index;
};
ReferenceSlot CellSlotOf(int x, int z) {
    const auto floor16 = [](int v) {
        return v >= 0 ? v / 16 : (v - 15) / 16;
    };
    const FieldPageKey key{floor16(x), floor16(z)};
    return {key, static_cast<std::size_t>((z - key.z * 16) * 16 + x - key.x * 16)};
}
bool CellBit(const FieldPage& page, std::size_t i) {
    return (page.mask[i / 64] >> (i % 64)) & 1;
}
const FieldPage* InitializedPage(const PageMap& pages, ReferenceSlot slot) {
    const auto it = pages.find(slot.key);
    return it != pages.end() && CellBit(it->second, slot.index) ? &it->second : nullptr;
}
std::int32_t ChannelValue(int channel, std::uint16_t word) {
    return channel < 2 ? static_cast<std::int32_t>(std::bit_cast<std::int16_t>(word)) : word;
}
// Scalar oracle retains per-cell tree lookups and the public full seed chain.
std::map<RegionKey, std::uint64_t>
ReferenceScheduledStep(PageMap& live,
                       const PageMap& previous,
                       const std::map<RegionKey, ActiveRegionRecord>& records,
                       std::span<const RegionKey> due,
                       std::uint64_t world_seed,
                       std::uint64_t tick) {
    const std::set<RegionKey> due_keys(due.begin(), due.end());
    std::map<RegionKey, std::uint64_t> work;
    const auto eligible = [&records](RegionKey owner) {
        const auto found = records.find(owner);
        return found != records.end() && found->second.state != RegionState::Frozen;
    };
    // Seed relaxation uses only the sealed old values, even when an adjacent region's
    // interior phase differs. No consumer observes a partially updated neighbour.
    for (const auto& [page_key, old_page] : previous) {
        for (std::size_t i = 0; i < FieldPage::kCells; ++i) {
            if (!CellBit(old_page, i))
                continue;
            const auto x = page_key.x * FieldPageState::kPageCells +
                           static_cast<std::int32_t>(i % FieldPageState::kPageCells);
            const auto z = page_key.z * FieldPageState::kPageCells +
                           static_cast<std::int32_t>(i / FieldPageState::kPageCells);
            const RegionKey owner = FieldPageState::region_of_cell(x, z);
            if (!eligible(owner) || !due_keys.contains(owner))
                continue;
            FieldPage& page = live.at(page_key);
            for (int channel = 0; channel < FieldPageState::kChannels; ++channel)
                page.cells[channel][i] = static_cast<std::uint16_t>(
                    ChannelValue(channel, old_page.cells[channel][i]) +
                    ((ChannelValue(
                          channel,
                          FieldPageState::seed_value(
                              world_seed, channel, x, z, tick / FieldPageState::kCadenceTicks)) -
                      ChannelValue(channel, old_page.cells[channel][i])) >>
                     FieldPageState::kRelaxShift));
            ++work[owner];
        }
    }
    // Visit every undirected edge once, in page/cell order, and transfer equal/opposite
    // integer fluxes. The 1/32 edge coefficient plus 1/16 relaxation is a bounded convex
    // update, so all intermediate and final concentrations remain representable as u16.
    for (const auto& [page_key, old_page] : previous) {
        for (std::size_t i = 0; i < FieldPage::kCells; ++i) {
            if (!CellBit(old_page, i))
                continue;
            const auto x = page_key.x * FieldPageState::kPageCells +
                           static_cast<std::int32_t>(i % FieldPageState::kPageCells);
            const auto z = page_key.z * FieldPageState::kPageCells +
                           static_cast<std::int32_t>(i / FieldPageState::kPageCells);
            const RegionKey owner = FieldPageState::region_of_cell(x, z);
            if (!eligible(owner))
                continue;
            for (const auto& [dx, dz] : {std::pair{1, 0}, std::pair{0, 1}}) {
                const ReferenceSlot neighbour = CellSlotOf(x + dx, z + dz);
                const auto* old_neighbour = InitializedPage(previous, neighbour);
                const RegionKey other = FieldPageState::region_of_cell(x + dx, z + dz);
                if (!old_neighbour || !eligible(other))
                    continue;
                if (owner == other) {
                    if (!due_keys.contains(owner))
                        continue;
                } else {
                    const auto shift =
                        std::max(records.at(owner).cadence_shift, records.at(other).cadence_shift);
                    if (shift >= 63 || (tick & ((std::uint64_t{1} << shift) - 1)) != 0)
                        continue;
                }
                auto& here = live.at(page_key);
                auto& there = live.at(neighbour.key);
                for (int channel = 0; channel < FieldPageState::kChannels; ++channel) {
                    const auto difference =
                        ChannelValue(channel, old_neighbour->cells[channel][neighbour.index]) -
                        ChannelValue(channel, old_page.cells[channel][i]);
                    const auto flux = difference / 32;
                    here.cells[channel][i] =
                        static_cast<std::uint16_t>(here.cells[channel][i] + flux);
                    there.cells[channel][neighbour.index] =
                        static_cast<std::uint16_t>(there.cells[channel][neighbour.index] - flux);
                }
                ++work[owner];
                ++work[other];
            }
        }
    }
    return work;
}
} // namespace

TEST(FieldPageStateTest, TiledSteppingMatchesScalarOracleAcrossAdmissionLoadAndTransitions) {
    FieldPageState store;
    std::map<RegionKey, ActiveRegionRecord> records;
    for (int x = -2; x <= 2; ++x)
        for (int z = -1; z <= 1; ++z) {
            const RegionKey key{x, z};
            records[key] = {};
            if (x != 2)
                ASSERT_TRUE(store.activate_region(key, kSeed, 0));
        }
    // Distant, absent owners must remain sealed even in a full snapshot.
    ASSERT_TRUE(store.activate_region({100, -100}, kSeed, 0));
    for (std::uint64_t tick = 1; tick <= 24; ++tick) {
        SCOPED_TRACE(tick);
        for (auto& [key, record] : records) {
            record.state = static_cast<RegionState>((tick / 4 + key.x + 3) % 3);
            record.cadence_shift = record.state == RegionState::Active ? 0 : 2;
            record.cadence_phase = ActiveRegionLedger::phase(key, record.cadence_shift);
        }
        std::vector<RegionKey> due;
        for (const auto& [key, record] : records)
            if (record.state != RegionState::Frozen &&
                (tick & ((1ull << record.cadence_shift) - 1)) == record.cadence_phase)
                due.push_back(key);
        const auto previous = store;
        for (const auto key : due)
            ASSERT_TRUE(store.activate_region(key, kSeed, tick));
        auto expected_pages = store.pages();
        const auto expected_work =
            ReferenceScheduledStep(expected_pages, previous.pages(), records, due, kSeed, tick);
        EXPECT_EQ(store.step_scheduled(records, due, kSeed, tick, &previous), expected_work);
        EXPECT_EQ(store.pages(), expected_pages);
        if (tick % 5 == 0) {
            const auto bytes = store.serialize();
            FieldPageState restored;
            std::string error;
            ASSERT_TRUE(FieldPageState::parse(bytes, restored, error)) << error;
            EXPECT_EQ(restored.serialize(), bytes);
            store = std::move(restored);
        }
    }
}

TEST(FieldPageStateTest, InitializationCountsFollowMasksAcrossPartialSnapshotsLoadAndClear) {
    FieldPageState store;
    const std::vector<RegionKey> keys{{-32768, 0}, {-2, -1}, {-1, 0}, {0, 0}, {1, 0}, {32767, 0}};
    const auto check = [&keys](const FieldPageState& state) {
        for (const auto key : keys) {
            std::size_t count = 0;
            for (const auto& [x, z] : FieldPageState::cells_of_region(key))
                count += state.initialized(x, z);
            EXPECT_EQ(state.owned_initialized_cells(key), count);
            EXPECT_EQ(state.owns_region(key), count != 0);
        }
    };
    for (const auto key : keys) {
        ASSERT_TRUE(store.activate_region(key, kSeed, 0));
        check(store);
        const auto before = store.serialize();
        ASSERT_TRUE(store.activate_region(key, 9999, 800));
        EXPECT_EQ(store.serialize(), before); // Fully initialized admission must not reseed.
    }
    std::map<RegionKey, ActiveRegionRecord> records;
    records[{0, 0}] = {};
    const std::vector<RegionKey> due{{0, 0}};
    auto subset = store.scheduled_snapshot(records, due, 1);
    check(subset); // Shared pages can contain only part of another owner's cells.
    const auto partial = subset.owned_initialized_cells({1, 0});
    EXPECT_GT(partial, 0u);
    EXPECT_LT(partial, FieldPageState::cells_of_region({1, 0}).size());
    ASSERT_TRUE(subset.activate_region({1, 0}, kSeed, 2));
    check(subset);
    EXPECT_EQ(subset.owned_initialized_cells({1, 0}),
              FieldPageState::cells_of_region({1, 0}).size());
    const auto bytes = store.serialize();
    std::string error;
    FieldPageState restored;
    ASSERT_TRUE(FieldPageState::parse(bytes, restored, error)) << error;
    check(restored);
    FieldPageState assigned;
    ASSERT_TRUE(assigned.activate_region({7, 7}, kSeed, 0));
    assigned = restored;
    check(assigned);
    EXPECT_EQ(assigned.owned_initialized_cells({7, 7}), 0u);
    EXPECT_EQ(assigned.serialize(), bytes);
    const auto& self = assigned;
    assigned = self;
    check(assigned);
    auto copied = assigned;
    check(copied);
    assigned = std::move(copied);
    check(assigned);
    auto corrupt = bytes;
    corrupt.back() ^= 1;
    EXPECT_FALSE(FieldPageState::parse(corrupt, restored, error));
    check(restored);
    EXPECT_EQ(restored.serialize(), bytes);
    restored.clear();
    check(restored);
    ASSERT_TRUE(restored.activate_region({0, 0}, kSeed, 9));
    check(restored);
}

TEST(FieldPageStateTest, ScentColumnWalkHandlesEmptyColumnsErasureEditsLoadAndWideFallback) {
    FieldPageState store, reference;
    const ScentCell one{std::bit_cast<std::uint64_t>(1.0), 0, 0, 0};
    // Empty columns' lower_bound would alias the node erased in the adjacent column.
    ASSERT_TRUE(store.set_scent({1, 0}, one));
    ASSERT_TRUE(reference.set_scent({1, 0}, one));
    const auto apply = [&store, &reference](const auto& updates) {
        for (const auto& [key, value] : updates)
            ASSERT_TRUE(reference.set_scent(key, value));
        ASSERT_TRUE(store.update_scent(updates));
        EXPECT_EQ(store.serialize(), reference.serialize());
    };
    apply(std::vector<std::pair<ScentCellKey, ScentCell>>{
        {{0, 0}, {}}, {{1, 0}, {}}, {{0, 1}, one}, {{1, 1}, one}});
    ASSERT_TRUE(store.set_scent({0, 1}, {}));
    ASSERT_TRUE(reference.set_scent({0, 1}, {}));
    FieldPageState restored;
    std::string error;
    ASSERT_TRUE(FieldPageState::parse(store.serialize(), restored, error)) << error;
    store = std::move(restored);
    apply(std::vector<std::pair<ScentCellKey, ScentCell>>{
        {{-2, -1}, one}, {{1, 1}, {}}, {{-2, 4}, one}, {{1, 4}, {}}});
    apply(std::vector<std::pair<ScentCellKey, ScentCell>>{
        {{-200, -2}, one}, {{200, -2}, one}, {{-200, 9}, {}}, {{200, 9}, one}});
}

TEST(FieldPageStateTest, ReusedScheduledStorageMatchesFreshAcrossAdmissionAndPhaseChanges) {
    FieldPageState reused, fresh, previous;
    FieldPageState::ScheduledScratch scratch;
    std::map<RegionKey, ActiveRegionRecord> records;
    records[{0, 0}] = {};
    records[{1, 0}] = {};
    ASSERT_TRUE(reused.activate_region({0, 0}, kSeed, 0));
    ASSERT_TRUE(fresh.activate_region({0, 0}, kSeed, 0));
    for (std::uint64_t tick = 1; tick <= 8; ++tick) {
        SCOPED_TRACE(tick);
        records.at({1, 0}).state = tick == 3 ? RegionState::Frozen : RegionState::Reduced;
        records.at({1, 0}).cadence_shift = 1;
        std::vector<RegionKey> due{{1, 0}, {0, 0}};
        if (tick % 2)
            due.erase(due.begin());
        reused.scheduled_snapshot_into(records, due, tick, previous, scratch);
        const auto expected_previous = fresh.scheduled_snapshot(records, due, tick);
        EXPECT_EQ(previous.serialize(), expected_previous.serialize());
        if (tick == 2) {
            ASSERT_TRUE(reused.activate_region({1, 0}, kSeed, tick));
            ASSERT_TRUE(fresh.activate_region({1, 0}, kSeed, tick));
        }
        EXPECT_EQ(reused.step_scheduled(records, due, kSeed, tick, &previous, &scratch),
                  fresh.step_scheduled(records, due, kSeed, tick, &expected_previous));
        EXPECT_EQ(reused.serialize(), fresh.serialize());
        for (const auto& [key, record] : records) {
            (void)record;
            EXPECT_EQ(reused.owned_initialized_cells(key), fresh.owned_initialized_cells(key));
        }
    }
    const auto before = reused.serialize();
    EXPECT_THROW(reused.scheduled_snapshot_into(records, {}, 9, reused, scratch),
                 std::invalid_argument);
    EXPECT_EQ(reused.serialize(), before);
}
