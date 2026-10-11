#include <gtest/gtest.h>

#include "luminumbra_common/fields/FieldPageState.h"

#include <cstddef>
#include <cstdint>
#include <set>
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
// Offsets inside an FPG1 file: 12-byte header, then 2600-byte page records.
constexpr std::size_t kHeaderBytes = 12;
constexpr std::size_t kPageBytes = 2600;
constexpr std::size_t kPage0 = kHeaderBytes;
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

std::int32_t Gap(std::uint16_t target, std::uint16_t value) {
    const std::int32_t difference =
        static_cast<std::int32_t>(target) - static_cast<std::int32_t>(value);
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
                EXPECT_LE(Gap(target, after[i]), Gap(target, before[i]));
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
            EXPECT_LE(Gap(target, v1[i]), Gap(target, v0[i]));
            EXPECT_LE(Gap(target, v2[i]), Gap(target, v1[i]));
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
    EXPECT_EQ(canonical, std::string("field_pages:v1:") + store.serialize());
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
    WriteU(bytes, 4, 2, 2);
    Reseal(bytes);
    EXPECT_TRUE(RefusedUntouched(bytes));

    bytes = FourPageFile();
    WriteU(bytes, 6, 4, 2);
    Reseal(bytes);
    EXPECT_TRUE(RefusedUntouched(bytes));
}

TEST(FieldPageStateTest, ParseRefusesPageCountAboveCap) {
    std::string bytes = FourPageFile();
    WriteU(bytes, 8, FieldPageState::kMaxPages + 1, 4);
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
    const char header[] = {'F', 'P', 'G', '1', 1, 0, 5, 0, 0, 0, 0, 0};
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
