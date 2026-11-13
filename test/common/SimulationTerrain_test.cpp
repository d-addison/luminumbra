#include "world/SimulationTerrain.h"

#include "gtest/gtest.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <span>

namespace {
using Luminumbra::world::SimulationTerrain;
using Key = SimulationTerrain::Key;
using Admission = SimulationTerrain::Admission;
using Status = SimulationTerrain::Status;
using Samples = std::array<float, SimulationTerrain::kLatticeSamples>;

template<typename Density>
Samples Lattice(Key key, const Density& density) {
    Samples samples{};
    for (std::size_t z = 0; z < 17; ++z) {
        for (std::size_t y = 0; y < 17; ++y) {
            for (std::size_t x = 0; x < 17; ++x) {
                samples[x + 17 * y + 17 * 17 * z] = static_cast<float>(
                    density(static_cast<double>(key.x) * 16.0 + static_cast<double>(x),
                            static_cast<double>(key.y) * 16.0 + static_cast<double>(y),
                            static_cast<double>(key.z) * 16.0 + static_cast<double>(z)));
            }
        }
    }
    return samples;
}

Samples Plane(Key key, double height = 4.0) {
    return Lattice(key, [height](double, double y, double) { return y - height; });
}

void ExpectSurface(const SimulationTerrain::Result& result, std::int64_t y_mm) {
    ASSERT_EQ(result.status, Status::Surface);
    EXPECT_EQ(result.y_mm, y_mm);
}
} // namespace

TEST(SimulationTerrainTest, OwnsFullLatticeAndReplacesWithoutBorrowingInput) {
    SimulationTerrain terrain;
    auto samples = Plane({});
    ASSERT_EQ(terrain.admit({}, samples), Admission::Accepted);
    samples.fill(1.0f);
    ExpectSurface(terrain.first_surface(500, 500, 8000, 0), 4000);
    ASSERT_EQ(terrain.admit({}, samples), Admission::Accepted);
    EXPECT_EQ(terrain.lattice_count(), 1u);
    EXPECT_EQ(terrain.first_surface(500, 500, 8000, 0).status, Status::NoSurface);
}

TEST(SimulationTerrainTest, FractionalNegativeCoordinatesUseFloorAndXZInterpolation) {
    SimulationTerrain terrain;
    // Cross term distinguishes bilinear interpolation from a nearest-node or diagonal sample.
    const Key key{-1, 0, -1};
    const auto samples = Lattice(key, [](double x, double y, double z) {
        return y - (5.0 + x * 0.5 + z * 0.25 + x * z * 0.125);
    });
    ASSERT_EQ(terrain.admit(key, samples), Admission::Accepted);
    ExpectSurface(terrain.first_surface(-250, -500, 8000, 0), 4766);
    EXPECT_EQ(terrain.first_surface(-16'000, -16'000, 8000, 0).status, Status::NoSurface);
}

TEST(SimulationTerrainTest, FacesEdgesAndCornersUseConsistentSharedNodes) {
    SimulationTerrain terrain;
    const auto density = [](double x, double y, double z) {
        return y - (4.0 + x / 8.0 + z / 8.0);
    };
    for (std::int32_t x = -1; x <= 0; ++x) {
        for (std::int32_t z = -1; z <= 0; ++z) {
            const Key key{x, 0, z};
            ASSERT_EQ(terrain.admit(key, Lattice(key, density)), Admission::Accepted);
        }
    }
    for (const auto x : {-16'000, -1, 0, 1, 15'999}) {
        for (const auto z : {-16'000, -1, 0, 1, 15'999}) {
            const auto expected =
                static_cast<std::int64_t>(std::round(4000.0 + static_cast<double>(x + z) / 8.0));
            ExpectSurface(terrain.first_surface(x, z, 10'000, 0), expected);
        }
    }
}

TEST(SimulationTerrainTest, ExactXZBoundaryRequiresPositiveSideAuthority) {
    SimulationTerrain terrain;
    ASSERT_EQ(terrain.admit({-1, 0, -1}, Plane({-1, 0, -1})), Admission::Accepted);
    ExpectSurface(terrain.first_surface(-1, -1, 8000, 0), 4000);
    EXPECT_EQ(terrain.first_surface(0, -1, 8000, 0).status, Status::Unavailable);
    EXPECT_EQ(terrain.first_surface(-1, 0, 8000, 0).status, Status::Unavailable);
    EXPECT_EQ(terrain.first_surface(0, 0, 8000, 0).status, Status::Unavailable);
}

TEST(SimulationTerrainTest, ExactContactAndInclusiveLowerEndpoint) {
    SimulationTerrain terrain;
    ASSERT_EQ(terrain.admit({}, Plane({}, 4.5)), Admission::Accepted);
    ExpectSurface(terrain.first_surface(0, 0, 4500, 0), 4500);
    ExpectSurface(terrain.first_surface(0, 0, 4500, 4500), 4500);
    ExpectSurface(terrain.first_surface(0, 0, 8000, 4500), 4500);
    EXPECT_EQ(terrain.first_surface(0, 0, 4499, 0).status, Status::NoSurface);
    EXPECT_EQ(terrain.first_surface(0, 0, 8000, 4501).status, Status::NoSurface);
    ASSERT_EQ(terrain.admit({}, Plane({})), Admission::Accepted);
    ExpectSurface(terrain.first_surface(0, 0, 4000, 4000), 4000);
    EXPECT_EQ(terrain.first_surface(0, 0, 4500, 4500).status, Status::NoSurface);
}

TEST(SimulationTerrainTest, ExactVerticalChunkContactUsesSupportingLattice) {
    SimulationTerrain terrain;
    ASSERT_EQ(terrain.admit({}, Plane({}, 16.0)), Admission::Accepted);
    ExpectSurface(terrain.first_surface(0, 0, 16'000, 0), 16'000);
    EXPECT_EQ(terrain.first_surface(0, 0, 16'001, 0).status, Status::Unavailable);
    const Key below{0, -1, 0};
    ASSERT_EQ(terrain.admit(below, Plane(below, 16.0)), Admission::Accepted);
    EXPECT_EQ(terrain.first_surface(0, 0, 0, -16'000).status, Status::NoSurface);
}

TEST(SimulationTerrainTest, CaveRoofInsideSolidAndUndersideNeverSnapUpward) {
    SimulationTerrain terrain;
    const auto samples = Lattice({}, [](double, double y, double) {
        // Ground at 3m, a hollow (3,8), roof [8,12], air above 12.
        return std::min(y - 3.0, std::max(8.0 - y, y - 12.0));
    });
    ASSERT_EQ(terrain.admit({}, samples), Admission::Accepted);
    ExpectSurface(terrain.first_surface(500, 500, 15'000, 0), 12'000);
    ExpectSurface(terrain.first_surface(500, 500, 12'000, 0), 12'000);
    for (const auto start : {11'500, 10'000, 8000, 7999, 5000, 3000}) {
        ExpectSurface(terrain.first_surface(500, 500, start, 0), 3000);
    }
    EXPECT_EQ(terrain.first_surface(500, 500, 2999, 0).status, Status::NoSurface);
    EXPECT_EQ(terrain.first_surface(500, 500, 11'000, 4000).status, Status::NoSurface);
}

TEST(SimulationTerrainTest, ConstantZeroAndSolidHaveNoInventedSurface) {
    for (const float density : {0.0f, -0.0f, -1.0f, 1.0f}) {
        SimulationTerrain terrain;
        Samples samples{};
        samples.fill(density);
        ASSERT_EQ(terrain.admit({}, samples), Admission::Accepted);
        EXPECT_EQ(terrain.first_surface(0, 0, 8000, 0).status, Status::NoSurface);
        EXPECT_EQ(terrain.first_surface(0, 0, 4000, 4000).status, Status::NoSurface);
    }
}

TEST(SimulationTerrainTest, ZeroNodeAndZeroPlateauAreResolvedInDownwardOrder) {
    SimulationTerrain terrain;
    auto samples = Lattice({}, [](double, double y, double) { return std::max(0.0, y - 6.0); });
    ASSERT_EQ(terrain.admit({}, samples), Admission::Accepted);
    ExpectSurface(terrain.first_surface(0, 0, 8000, 0), 6000);
    samples = Lattice({}, [](double, double y, double) { return y - 6.0; });
    ASSERT_EQ(terrain.admit({}, samples), Admission::Accepted);
    ExpectSurface(terrain.first_surface(0, 0, 6000, 0), 6000);
}

TEST(SimulationTerrainTest, SignedZeroSharedNodesAgree) {
    SimulationTerrain terrain;
    Samples samples{};
    samples.fill(-0.0f);
    ASSERT_EQ(terrain.admit({}, samples), Admission::Accepted);
    samples.fill(0.0f);
    ASSERT_EQ(terrain.admit({1, 0, 0}, samples), Admission::Accepted);
    EXPECT_EQ(terrain.first_surface(16'000, 0, 8000, 0).status, Status::NoSurface);
}

TEST(SimulationTerrainTest, RoundsOnlyFinalHeightWithSignedHalfTiesAwayFromZero) {
    for (const double height : {4.0625, 4.062, 4.063, -4.0625, -4.062, -4.063}) {
        SimulationTerrain terrain;
        const Key key{0, height < 0 ? -1 : 0, 0};
        // 1/16m gives exact float samples and an exact 62.5mm crossing.
        ASSERT_EQ(terrain.admit(key, Plane(key, height)), Admission::Accepted);
        const auto result =
            terrain.first_surface(0, 0, height < 0 ? -1000 : 8000, height < 0 ? -8000 : 0);
        ExpectSurface(result, static_cast<std::int64_t>(std::round(height * 1000.0)));
    }
}

TEST(SimulationTerrainTest, FractionalCrossingIsSolvedRatherThanSteppedToNode) {
    SimulationTerrain terrain;
    const auto samples = Lattice({}, [](double, double y, double) { return 3.0 * y - 13.0; });
    ASSERT_EQ(terrain.admit({}, samples), Admission::Accepted);
    ExpectSurface(terrain.first_surface(123, 456, 8000, 0), 4333);
    EXPECT_EQ(terrain.first_surface(123, 456, 4333, 0).status, Status::NoSurface);
    ExpectSurface(terrain.first_surface(123, 456, 4334, 0), 4333);
}

TEST(SimulationTerrainTest, EmptyMissingAndPartialColumnsRefuseRatherThanFallback) {
    SimulationTerrain terrain;
    EXPECT_EQ(terrain.first_surface(0, 0, 8000, 0).status, Status::Unavailable);
    auto air = Plane({}, -100.0);
    ASSERT_EQ(terrain.admit({}, air), Admission::Accepted);
    const auto missing = terrain.first_surface(0, 0, 8000, -1000);
    EXPECT_EQ(missing.status, Status::Unavailable);
    EXPECT_EQ(missing.visits.cells, 8u);
    EXPECT_EQ(missing.visits.lattice_lookups, 2u);
    EXPECT_EQ(terrain.first_surface(0, 0, 8000, 0).status, Status::NoSurface);
    ASSERT_EQ(terrain.admit({}, Plane({})), Admission::Accepted);
    ExpectSurface(terrain.first_surface(0, 0, 8000, -1000), 4000);
}

TEST(SimulationTerrainTest, CrossesVerticalChunksInIndexOrderIndependentOfAdmissionOrder) {
    SimulationTerrain terrain;
    for (const std::int32_t y : {-2, 1, -1, 0}) {
        const Key key{0, y, 0};
        ASSERT_EQ(terrain.admit(key, Plane(key, -20.5)), Admission::Accepted);
    }
    const auto result = terrain.first_surface(500, 500, 20'500, -30'000);
    ExpectSurface(result, -20'500);
    EXPECT_EQ(result.visits.cells, 42u);
    EXPECT_EQ(result.visits.lattice_lookups, 4u);
    EXPECT_EQ(result.visits.samples, 336u);
}

TEST(SimulationTerrainTest, MalformedAndNonfiniteLatticesLeaveOwnedAuthorityUnchanged) {
    SimulationTerrain terrain;
    const auto good = Plane({});
    ASSERT_EQ(terrain.admit({}, good), Admission::Accepted);
    const auto bytes = terrain.owned_bytes();
    for (const std::size_t count : {0u, 1u, 729u, 4912u}) {
        EXPECT_EQ(terrain.admit({}, std::span(good).first(count)), Admission::Unavailable);
        EXPECT_EQ(terrain.admit({2, 0, 0}, std::span(good).first(count)), Admission::Unavailable);
    }
    std::array<float, SimulationTerrain::kLatticeSamples + 1> oversized{};
    EXPECT_EQ(terrain.admit({}, oversized), Admission::Unavailable);
    for (const float invalid : {std::numeric_limits<float>::quiet_NaN(),
                                std::numeric_limits<float>::infinity(),
                                -std::numeric_limits<float>::infinity()}) {
        auto samples = good;
        samples.back() = invalid;
        EXPECT_EQ(terrain.admit({}, samples), Admission::Unavailable);
        EXPECT_EQ(terrain.admit({2, 0, 0}, samples), Admission::Unavailable);
    }
    EXPECT_EQ(terrain.lattice_count(), 1u);
    EXPECT_EQ(terrain.owned_bytes(), bytes);
    ExpectSurface(terrain.first_surface(0, 0, 8000, 0), 4000);
}

TEST(SimulationTerrainTest, VerticalChunkKeyExtremesPreserveWorldMillimetreRounding) {
    SimulationTerrain terrain;
    for (const auto edge :
         {std::numeric_limits<std::int32_t>::min(), std::numeric_limits<std::int32_t>::max()}) {
        const Key key{0, edge, 0};
        const auto base_mm = static_cast<std::int64_t>(edge) * 16'000;
        ASSERT_EQ(terrain.admit(key, Plane(key, static_cast<double>(edge) * 16.0 + 4.0625)),
                  Admission::Accepted);
        ExpectSurface(terrain.first_surface(0, 0, base_mm + 8000, base_mm),
                      base_mm + (edge < 0 ? 4062 : 4063));
    }
}

TEST(SimulationTerrainTest, EverySharedFaceEdgeAndCornerDisagreementIsRefusedAtomically) {
    for (std::int32_t z = -1; z <= 1; ++z) {
        for (std::int32_t y = -1; y <= 1; ++y) {
            for (std::int32_t x = -1; x <= 1; ++x) {
                if (x == 0 && y == 0 && z == 0) {
                    continue;
                }
                SimulationTerrain terrain;
                ASSERT_EQ(terrain.admit({}, Plane({})), Admission::Accepted);
                const Key neighbour{x, y, z};
                auto samples = Plane(neighbour);
                const std::size_t sx = x > 0 ? 0 : 16;
                const std::size_t sy = y > 0 ? 0 : 16;
                const std::size_t sz = z > 0 ? 0 : 16;
                samples[sx + 17 * sy + 17 * 17 * sz] += 1.0f;
                EXPECT_EQ(terrain.admit(neighbour, samples), Admission::Unavailable);
                EXPECT_EQ(terrain.lattice_count(), 1u);
                ASSERT_EQ(terrain.admit(neighbour, Plane(neighbour)), Admission::Accepted);
                EXPECT_EQ(terrain.admit(neighbour, samples), Admission::Unavailable);
                EXPECT_EQ(terrain.lattice_count(), 2u);
                ExpectSurface(terrain.first_surface(0, 0, 8000, 0), 4000);
            }
        }
    }
}

TEST(SimulationTerrainTest, StorageAndCountRefusalsPrecedeAllocationAndPreserveData) {
    SimulationTerrain terrain({.lattices = 1, .owned_bytes = SimulationTerrain::kMaxOwnedBytes});
    const auto samples = Plane({});
    ASSERT_EQ(terrain.admit({}, samples), Admission::Accepted);
    const auto bytes = terrain.owned_bytes();
    EXPECT_EQ(terrain.admit({2, 0, 0}, samples), Admission::WorkLimit);
    EXPECT_EQ(terrain.lattice_count(), 1u);
    EXPECT_EQ(terrain.owned_bytes(), bytes);
    ASSERT_EQ(terrain.admit({}, Plane({}, 5.0)), Admission::Accepted);
    ExpectSurface(terrain.first_surface(0, 0, 8000, 0), 5000);
    for (const auto limits :
         {SimulationTerrain::Limits{.lattices = 0, .owned_bytes = 0},
          SimulationTerrain::Limits{.lattices = 1, .owned_bytes = 1},
          SimulationTerrain::Limits{.lattices = SimulationTerrain::kMaxLattices + 1,
                                    .owned_bytes = SimulationTerrain::kMaxOwnedBytes},
          SimulationTerrain::Limits{.lattices = std::numeric_limits<std::size_t>::max(),
                                    .owned_bytes = std::numeric_limits<std::size_t>::max()},
          SimulationTerrain::Limits{.lattices = 1,
                                    .owned_bytes = SimulationTerrain::kMaxOwnedBytes + 1}}) {
        SimulationTerrain refused(limits);
        EXPECT_EQ(refused.admit({}, samples), Admission::WorkLimit);
        EXPECT_EQ(refused.owned_bytes(), 0u);
        EXPECT_EQ(refused.lattice_count(), 0u);
    }
    // Fit the published record but not a temporary replacement: refusal retains the original.
    SimulationTerrain tight({.lattices = 1, .owned_bytes = bytes});
    ASSERT_EQ(tight.admit({}, samples), Admission::Accepted);
    EXPECT_EQ(tight.owned_bytes(), bytes);
    EXPECT_EQ(tight.admit({}, Plane({}, 5.0)), Admission::WorkLimit);
    ExpectSurface(tight.first_surface(0, 0, 8000, 0), 4000);
}

TEST(SimulationTerrainTest, TraversalBudgetCountsCellsAndRefusesBeforeAnyVisit) {
    SimulationTerrain terrain;
    ASSERT_EQ(terrain.admit({}, Plane({})), Admission::Accepted);
    for (const std::size_t limit : {0u, 1u, 7u}) {
        const auto result = terrain.first_surface(0, 0, 8000, 0, limit);
        EXPECT_EQ(result.status, Status::WorkLimit);
        EXPECT_EQ(result.visits, SimulationTerrain::Visits{});
    }
    const auto accepted = terrain.first_surface(0, 0, 8000, 0, 8);
    ExpectSurface(accepted, 4000);
    EXPECT_EQ(accepted.visits.cells, 4u);
    EXPECT_EQ(accepted.visits.samples, 32u);
    EXPECT_EQ(accepted.visits.lattice_lookups, 1u);
    const auto hard_limit =
        terrain.first_surface(0, 0, 65'537'000, 0, std::numeric_limits<std::size_t>::max());
    EXPECT_EQ(hard_limit.status, Status::WorkLimit);
    EXPECT_EQ(hard_limit.visits, SimulationTerrain::Visits{});
    EXPECT_EQ(terrain.first_surface(0, 0, 65'536'000, 0).status, Status::Unavailable);
}

TEST(SimulationTerrainTest, UnrelatedWorldGrowthDoesNotAddQueryVisits) {
    SimulationTerrain terrain;
    ASSERT_EQ(terrain.admit({}, Plane({})), Admission::Accepted);
    const auto before = terrain.first_surface(123, 456, 8000, 0);
    for (std::int32_t x = 2; x < 102; ++x) {
        const Key key{x, 0, 0};
        ASSERT_EQ(terrain.admit(key, Plane(key)), Admission::Accepted);
    }
    const SimulationTerrain& view = terrain;
    const auto after = view.first_surface(123, 456, 8000, 0);
    EXPECT_EQ(after.status, before.status);
    EXPECT_EQ(after.y_mm, before.y_mm);
    EXPECT_EQ(after.visits, before.visits);
}

TEST(SimulationTerrainTest, InvalidRangesAndExtremeCoordinatesRefuseBeforeConversion) {
    SimulationTerrain terrain;
    ASSERT_EQ(terrain.admit({}, Plane({})), Admission::Accepted);
    EXPECT_EQ(terrain.first_surface(0, 0, 1000, 1001).status, Status::Unavailable);
    constexpr auto low = std::numeric_limits<std::int64_t>::min();
    constexpr auto high = std::numeric_limits<std::int64_t>::max();
    for (const auto coordinate : {low, high}) {
        EXPECT_EQ(terrain.first_surface(coordinate, 0, 8000, 0).status, Status::Unavailable);
        EXPECT_EQ(terrain.first_surface(0, coordinate, 8000, 0).status, Status::Unavailable);
        EXPECT_EQ(terrain.first_surface(0, 0, coordinate, coordinate).status, Status::Unavailable);
        EXPECT_EQ(terrain.first_surface(0, 0, high, 0).visits, SimulationTerrain::Visits{});
    }
}

TEST(SimulationTerrainTest, Int32ChunkExtremesAndNegativeVerticalFlooringAreSafe) {
    SimulationTerrain terrain;
    for (const auto edge :
         {std::numeric_limits<std::int32_t>::min(), std::numeric_limits<std::int32_t>::max()}) {
        const Key key{edge, -1, edge};
        ASSERT_EQ(terrain.admit(key, Plane(key, -0.25)), Admission::Accepted);
        const auto coordinate = static_cast<std::int64_t>(edge) * 16'000;
        ExpectSurface(terrain.first_surface(coordinate, coordinate, -1, -1000), -250);
        ExpectSurface(terrain.first_surface(coordinate + 15'999, coordinate + 15'999, -1, -1000),
                      -250);
    }
    constexpr auto lowest_mm =
        static_cast<std::int64_t>(std::numeric_limits<std::int32_t>::min()) * 16'000;
    constexpr auto highest_mm =
        (static_cast<std::int64_t>(std::numeric_limits<std::int32_t>::max()) + 1) * 16'000;
    EXPECT_EQ(terrain.first_surface(lowest_mm - 1, 0, 8000, 0).status, Status::Unavailable);
    EXPECT_EQ(terrain.first_surface(highest_mm, 0, 8000, 0).status, Status::Unavailable);
    EXPECT_EQ(terrain.first_surface(0, 0, lowest_mm, lowest_mm).status, Status::Unavailable);
}

TEST(SimulationTerrainTest, LargestFiniteDensitiesRemainFiniteDuringInterpolation) {
    SimulationTerrain terrain;
    const auto samples = Lattice({}, [](double x, double y, double z) {
        const double magnitude = std::numeric_limits<float>::max();
        return (y - 4.5) * magnitude / 16.0 + (x + z) * magnitude / 128.0;
    });
    ASSERT_EQ(terrain.admit({}, samples), Admission::Accepted);
    ExpectSurface(terrain.first_surface(500, 500, 8000, 0), 4375);
}
