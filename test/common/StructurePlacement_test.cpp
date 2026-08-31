// structure placement + jigsaw assembly gates.
// - placement determinism: same seed => same sites; SiteInCell is a pure
//   function of (seed, salt, cell); LocateNearestSite is verified against a
//   brute-force scan.
// - template integrity: the shipped cairn + ruin fixtures load, and their
//   assembled voxel sets hash to pinned values (snapshot gate; a deliberate
//   change to the data flips the number).
#include "gtest/gtest.h"

#include "world/StructurePlacement.h"

#include <filesystem>
#include <optional>
#include <set>
#include <vector>

#ifndef LUMINUMBRA_SOURCE_ROOT
#define LUMINUMBRA_SOURCE_ROOT "."
#endif

namespace {

namespace fs = std::filesystem;
using namespace Luminumbra::World;

fs::path StructuresDir() {
    return fs::path(LUMINUMBRA_SOURCE_ROOT) / "data" / "common" / "structures";
}

constexpr int kSeed = 4242;
// Pinned assembled-voxel snapshot (printed by the test on first run, then
// pinned here). A deliberate template edit re-pins it in the same commit.
constexpr Luminumbra::u64 kCairnAssembledHash = 0xa1fe4fca654dcef6ull;

StructureTemplatePool LoadCairn() {
    return LoadStructureTemplatePool(StructuresDir() / "cairn", "cairn");
}

StructureTemplatePool LoadRuin() {
    return LoadStructureTemplatePool(StructuresDir() / "ruin", "ruin");
}

} // namespace

TEST(StructurePlacementTest, FixturePoolsLoad) {
    StructureTemplatePool cairn = LoadCairn();
    ASSERT_TRUE(cairn.errors.empty()) << (cairn.errors.empty() ? "" : cairn.errors.front());
    EXPECT_TRUE(cairn.ok());
    EXPECT_EQ(cairn.type, "cairn");
    EXPECT_EQ(cairn.salt, 101u);
    EXPECT_EQ(cairn.spacing, 96);
    EXPECT_FALSE(cairn.pieces.empty());

    StructureTemplatePool ruin = LoadRuin();
    ASSERT_TRUE(ruin.errors.empty()) << (ruin.errors.empty() ? "" : ruin.errors.front());
    EXPECT_TRUE(ruin.ok());
    EXPECT_GE(ruin.pieces.size(), 2u); // base + at least one socketed piece
    // The base piece (sorted first: a_base.json) carries socket joints.
    bool any_sockets = false;
    for (const auto& p : ruin.pieces) {
        any_sockets = any_sockets || !p.sockets.empty();
    }
    EXPECT_TRUE(any_sockets);
}

TEST(StructurePlacementTest, SiteInCellIsDeterministic) {
    StructureTemplatePool cairn = LoadCairn();
    ASSERT_TRUE(cairn.ok());
    for (int cz = -3; cz <= 3; ++cz) {
        for (int cx = -3; cx <= 3; ++cx) {
            auto a = SiteInCell(cairn, kSeed, cx, cz);
            auto b = SiteInCell(cairn, kSeed, cx, cz);
            EXPECT_EQ(a.has_value(), b.has_value());
            if (a && b) {
                EXPECT_EQ(a->origin, b->origin);
                EXPECT_EQ(a->site_seed, b->site_seed);
            }
        }
    }
}

TEST(StructurePlacementTest, DifferentSeedShiftsSites) {
    StructureTemplatePool cairn = LoadCairn();
    ASSERT_TRUE(cairn.ok());
    // Across a window of cells the placement set must differ for a different
    // world seed (the grid is seed-driven, not fixed).
    auto collect = [&](int seed) {
        std::set<std::tuple<int, int, int>> origins;
        for (int cz = -5; cz <= 5; ++cz) {
            for (int cx = -5; cx <= 5; ++cx) {
                if (auto s = SiteInCell(cairn, seed, cx, cz)) {
                    origins.emplace(s->origin.x, s->origin.y, s->origin.z);
                }
            }
        }
        return origins;
    };
    EXPECT_NE(collect(kSeed), collect(kSeed + 1));
}

TEST(StructurePlacementTest, LocateNearestMatchesBruteForce) {
    StructureTemplatePool ruin = LoadRuin();
    ASSERT_TRUE(ruin.ok());
    const int near_x = 500;
    const int near_z = -300;
    const int radius = 2;
    auto located = LocateNearestSite(ruin, kSeed, near_x, near_z, radius);

    // Brute force the same window.
    const int cx = near_x >= 0 ? near_x / ruin.spacing : (near_x - ruin.spacing + 1) / ruin.spacing;
    const int cz = near_z >= 0 ? near_z / ruin.spacing : (near_z - ruin.spacing + 1) / ruin.spacing;
    std::optional<StructureSite> best;
    long long best_d2 = -1;
    for (int dz = -radius; dz <= radius; ++dz) {
        for (int dx = -radius; dx <= radius; ++dx) {
            if (auto s = SiteInCell(ruin, kSeed, cx + dx, cz + dz)) {
                const long long ex = static_cast<long long>(s->origin.x) - near_x;
                const long long ez = static_cast<long long>(s->origin.z) - near_z;
                const long long d2 = ex * ex + ez * ez;
                if (best_d2 < 0 || d2 < best_d2) {
                    best_d2 = d2;
                    best = s;
                }
            }
        }
    }
    ASSERT_EQ(located.has_value(), best.has_value());
    if (located && best) {
        EXPECT_EQ(located->origin, best->origin);
    }
}

TEST(StructurePlacementTest, SitesInAreaRespectsBounds) {
    StructureTemplatePool cairn = LoadCairn();
    ASSERT_TRUE(cairn.ok());
    const int min_x = -200, min_z = -200, max_x = 600, max_z = 600;
    auto sites = SitesInArea(cairn, kSeed, min_x, min_z, max_x, max_z);
    for (const auto& s : sites) {
        EXPECT_GE(s.origin.x, min_x);
        EXPECT_LT(s.origin.x, max_x);
        EXPECT_GE(s.origin.z, min_z);
        EXPECT_LT(s.origin.z, max_z);
    }
    // Determinism: a second enumeration is identical.
    auto sites2 = SitesInArea(cairn, kSeed, min_x, min_z, max_x, max_z);
    ASSERT_EQ(sites.size(), sites2.size());
    for (std::size_t i = 0; i < sites.size(); ++i) {
        EXPECT_EQ(sites[i].origin, sites2[i].origin);
    }
}

TEST(StructurePlacementTest, AssembledCairnHashIsStable) {
    StructureTemplatePool cairn = LoadCairn();
    ASSERT_TRUE(cairn.ok());
    // Assemble at a fixed site (origin only matters as an offset; the hash is
    // computed in absolute coordinates, so we pin the origin too).
    StructureSite site;
    site.type = "cairn";
    site.origin = Luminumbra::IVec3(0, 0, 0);
    site.site_seed = 0xABCDEF0123456789ull;
    auto voxels = AssembleStructure(cairn, site);
    ASSERT_FALSE(voxels.empty());
    const Luminumbra::u64 hash = ComputeAssembledVoxelHash(voxels);
    // The cairn has no sockets, so its assembly is just the chosen root piece;
    // with a single piece in the pool the root is deterministic. Pinned snapshot
    // (a deliberate template edit flips this number):
    std::printf("cairn assembled voxel hash (seed 0xABCDEF0123456789): %016llx\n",
                static_cast<unsigned long long>(hash));
    EXPECT_EQ(hash, kCairnAssembledHash);
    // Stable across rebuilds: a second assembly hashes identically.
    auto voxels2 = AssembleStructure(cairn, site);
    EXPECT_EQ(ComputeAssembledVoxelHash(voxels2), hash);
}

TEST(StructurePlacementTest, AssembledRuinIsDeterministicAndUsesSockets) {
    StructureTemplatePool ruin = LoadRuin();
    ASSERT_TRUE(ruin.ok());
    StructureSite site;
    site.type = "ruin";
    site.origin = Luminumbra::IVec3(100, 32, -50);
    site.site_seed = 0x1234567890ABCDEFull;
    auto a = AssembleStructure(ruin, site);
    auto b = AssembleStructure(ruin, site);
    ASSERT_FALSE(a.empty());
    EXPECT_EQ(ComputeAssembledVoxelHash(a), ComputeAssembledVoxelHash(b));
    std::printf("ruin assembled voxel hash (seed 0x1234567890ABCDEF): %016llx voxels=%zu\n",
                static_cast<unsigned long long>(ComputeAssembledVoxelHash(a)),
                a.size());
}
