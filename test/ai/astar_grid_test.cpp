// deterministic grid A* pathfinding coverage. Pure (no ECS/world);
// validates shortest-path optimality, obstacle avoidance, corner-cut prevention,
// the no-path case, and bit-stable determinism (the sim contract).

#include "gtest/gtest.h"

#include "luminumbra_common/ai/AStarGrid.h"

#include <vector>

namespace {
using luminumbra::ai::FindPath;
using luminumbra::ai::GridCoord;
using luminumbra::ai::NavGrid;

// Build a NavGrid from a row-major ASCII map ('.' walkable, '#' blocked).
// rows[0] is z=0. All rows must share a width.
NavGrid FromAscii(const std::vector<std::string>& rows) {
    const int h = static_cast<int>(rows.size());
    const int w = h > 0 ? static_cast<int>(rows[0].size()) : 0;
    std::vector<std::uint8_t> cells(static_cast<std::size_t>(w) * static_cast<std::size_t>(h), 0u);
    for (int z = 0; z < h; ++z)
        for (int x = 0; x < w; ++x)
            cells[static_cast<std::size_t>(z) * w + x] = (rows[z][x] == '#') ? 0u : 1u;
    return NavGrid(w, h, std::move(cells));
}
} // namespace

TEST(AStarGrid, StraightLineOnOpenGrid) {
    auto grid = FromAscii({"....", "....", "...."});
    auto path = FindPath(grid, {0, 0}, {3, 0});
    ASSERT_FALSE(path.empty());
    EXPECT_TRUE((path.front() == GridCoord{0, 0}));
    EXPECT_TRUE((path.back() == GridCoord{3, 0}));
    EXPECT_EQ(path.size(), 4u); // 0,1,2,3 — no detour on open ground
}

TEST(AStarGrid, DiagonalIsShorterThanManhattan) {
    auto grid = FromAscii({"....", "....", "....", "...."});
    auto path = FindPath(grid, {0, 0}, {3, 3});
    ASSERT_FALSE(path.empty());
    EXPECT_TRUE((path.back() == GridCoord{3, 3}));
    EXPECT_EQ(path.size(), 4u); // pure diagonal: (0,0)(1,1)(2,2)(3,3)
}

TEST(AStarGrid, RoutesAroundAWall) {
    // A vertical wall with a gap at the bottom row forces a detour.
    auto grid = FromAscii({
        "..#..",
        "..#..",
        "..#..",
        ".....",
    });
    auto path = FindPath(grid, {0, 0}, {4, 0});
    ASSERT_FALSE(path.empty());
    EXPECT_TRUE((path.front() == GridCoord{0, 0}));
    EXPECT_TRUE((path.back() == GridCoord{4, 0}));
    // Never steps on a blocked cell (the x==2 column for z in 0..2).
    for (const auto& c : path) {
        EXPECT_FALSE(c.x == 2 && c.z <= 2)
            << "path entered the wall at (" << c.x << "," << c.z << ")";
    }
}

TEST(AStarGrid, NoPathWhenWalledOff) {
    // Goal fully enclosed by '#'.
    auto grid = FromAscii({
        ".....",
        ".###.",
        ".#G#.",
        ".###.",
        ".....",
    });
    auto path = FindPath(grid, {0, 0}, {2, 2});
    EXPECT_TRUE(path.empty());
}

TEST(AStarGrid, DoesNotCutBlockedCorners) {
    // Diagonal from (0,0) to (1,1) must NOT clip between the two blockers at
    // (1,0) and (0,1); with both orthogonals blocked there is no diagonal move,
    // and (1,1) is otherwise unreachable -> empty path.
    auto grid = FromAscii({
        ".#",
        "#.",
    });
    auto path = FindPath(grid, {0, 0}, {1, 1});
    EXPECT_TRUE(path.empty());
}

TEST(AStarGrid, StartEqualsGoalIsSingletonAndBlockedEndpointsFail) {
    auto grid = FromAscii({"...", "...", "..."});
    auto same = FindPath(grid, {1, 1}, {1, 1});
    ASSERT_EQ(same.size(), 1u);
    EXPECT_TRUE((same.front() == GridCoord{1, 1}));

    auto blocked = FromAscii({"...", ".#.", "..."});
    EXPECT_TRUE(FindPath(blocked, {0, 0}, {1, 1}).empty()); // goal blocked
    EXPECT_TRUE(FindPath(blocked, {1, 1}, {2, 2}).empty()); // start blocked
}

TEST(AStarGrid, IsDeterministicAcrossRuns) {
    auto grid = FromAscii({
        "..........",
        ".##.###.#.",
        "....#...#.",
        ".#.##.##..",
        "..#....#..",
        ".##.##.#..",
        "..........",
    });
    auto a = FindPath(grid, {0, 0}, {9, 6});
    auto b = FindPath(grid, {0, 0}, {9, 6});
    ASSERT_FALSE(a.empty());
    ASSERT_EQ(a.size(), b.size());
    for (std::size_t i = 0; i < a.size(); ++i) {
        EXPECT_TRUE(a[i] == b[i]) << "divergence at step " << i;
    }
    // Path is contiguous (each step is an 8-neighbour move) and stays walkable.
    for (std::size_t i = 1; i < a.size(); ++i) {
        const int dx = a[i].x - a[i - 1].x, dz = a[i].z - a[i - 1].z;
        EXPECT_LE(dx * dx, 1);
        EXPECT_LE(dz * dz, 1);
        EXPECT_TRUE(grid.walkable(a[i].x, a[i].z));
    }
}
