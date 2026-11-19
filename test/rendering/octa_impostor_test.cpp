// Unit tests for the hemi-octahedral impostor mapping (src/.../rendering/OctaImpostor.h).
// GL-free, deterministic — the same discipline as tree_lod_test.cpp. These pin the
// math that both the atlas BAKE (tile -> capture direction) and the runtime impostor
// SHADER (view direction -> tile) rely on, so the two can never drift apart.

#include "luminumbra_client/rendering/ImpostorBake.h"
#include "luminumbra_client/rendering/ImpostorBakePath.h"
#include "luminumbra_client/rendering/OctaImpostor.h"
#include "luminumbra_client/rendering/TreeLod.h"

#include <gtest/gtest.h>

#include <cmath>
#include <limits>
#include <vector>

namespace {

using Luminumbra::Rendering::HemiOctaDecode;
using Luminumbra::Rendering::HemiOctaEncode;
using Luminumbra::Rendering::OctaImpostorGrid;
using Luminumbra::Rendering::OctaNearestTile;
using Luminumbra::Rendering::OctaTileCoord;
using Luminumbra::Rendering::OctaTileDirection;
using Luminumbra::Rendering::Vec2f;
using Luminumbra::Rendering::Vec3f;

Vec3f Normalize(Vec3f v) {
    const float len = std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
    const float inv = (len > 0.0f) ? 1.0f / len : 0.0f;
    return Vec3f{v.x * inv, v.y * inv, v.z * inv};
}

float Dot(const Vec3f& a, const Vec3f& b) {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

// A spread of upper-hemisphere directions (y >= 0) for round-trip coverage.
std::vector<Vec3f> HemisphereSamples() {
    std::vector<Vec3f> out;
    for (int el = 0; el <= 8; ++el) { // elevation 0 (horizon) .. 90 (up)
        const float theta = (static_cast<float>(el) / 8.0f) * (3.14159265f * 0.5f);
        for (int az = 0; az < 16; ++az) { // azimuth around
            const float phi = (static_cast<float>(az) / 16.0f) * (2.0f * 3.14159265f);
            out.push_back(Normalize(Vec3f{std::cos(theta) * std::cos(phi),
                                          std::sin(theta),
                                          std::cos(theta) * std::sin(phi)}));
        }
    }
    return out;
}

TEST(OctaImpostor, EncodeStaysInUnitSquare) {
    for (const Vec3f& dir : HemisphereSamples()) {
        const Vec2f uv = HemiOctaEncode(dir);
        EXPECT_GE(uv.x, -1e-4f);
        EXPECT_LE(uv.x, 1.0f + 1e-4f);
        EXPECT_GE(uv.y, -1e-4f);
        EXPECT_LE(uv.y, 1.0f + 1e-4f);
    }
}

TEST(OctaImpostor, EncodeDecodeRoundTripsOnHemisphere) {
    for (const Vec3f& dir : HemisphereSamples()) {
        const Vec3f back = HemiOctaDecode(HemiOctaEncode(dir));
        // Same unit direction (dot ~ 1). Allow a small epsilon for float math.
        EXPECT_NEAR(Dot(dir, back), 1.0f, 1e-4f)
            << "dir(" << dir.x << "," << dir.y << "," << dir.z << ")";
        EXPECT_GE(back.y, -1e-4f) << "decoded direction left the upper hemisphere";
    }
}

TEST(OctaImpostor, StraightUpMapsToCenter) {
    const Vec2f uv = HemiOctaEncode(Vec3f{0.0f, 1.0f, 0.0f});
    EXPECT_NEAR(uv.x, 0.5f, 1e-5f);
    EXPECT_NEAR(uv.y, 0.5f, 1e-5f);
}

TEST(OctaImpostor, TileDirectionsAreUnitAndCoverBothHemispheres) {
    OctaImpostorGrid grid;
    grid.gridResolution = 8;
    int upper = 0, lower = 0;
    for (int j = 0; j < grid.gridResolution; ++j) {
        for (int i = 0; i < grid.gridResolution; ++i) {
            const Vec3f dir = OctaTileDirection(i, j, grid);
            EXPECT_NEAR(std::sqrt(Dot(dir, dir)), 1.0f, 1e-4f)
                << "tile (" << i << "," << j << ") not unit";
            upper += dir.y > 0.0f ? 1 : 0;
            lower += dir.y < 0.0f ? 1 : 0;
        }
    }
    EXPECT_GT(upper, 0);
    EXPECT_GT(lower, 0);
}

TEST(OctaImpostor, FullSphereRoundTripsIncludingViewsBelowTheCanopy) {
    for (auto direction : HemisphereSamples()) {
        for (const float sign : {-1.0f, 1.0f}) {
            direction.y = std::fabs(direction.y) * sign;
            const auto uv = Luminumbra::Rendering::OctaEncode(direction);
            EXPECT_GE(uv.x, 0.0f);
            EXPECT_LE(uv.x, 1.0f);
            EXPECT_GE(uv.y, 0.0f);
            EXPECT_LE(uv.y, 1.0f);
            EXPECT_NEAR(Dot(direction, Luminumbra::Rendering::OctaDecode(uv)), 1.0f, 1e-4f);
        }
    }
}

TEST(OctaImpostor, NearestTileOfATilesOwnDirectionIsThatTile) {
    OctaImpostorGrid grid;
    grid.gridResolution = 8;
    for (int j = 0; j < grid.gridResolution; ++j) {
        for (int i = 0; i < grid.gridResolution; ++i) {
            const Vec3f dir = OctaTileDirection(i, j, grid);
            int ni = -1, nj = -1;
            OctaNearestTile(dir, grid, ni, nj);
            EXPECT_EQ(ni, i) << "azimuth tile drifted at (" << i << "," << j << ")";
            EXPECT_EQ(nj, j) << "elevation tile drifted at (" << i << "," << j << ")";
        }
    }
}

TEST(OctaImpostor, TileCoordMatchesNearestTileFloor) {
    OctaImpostorGrid grid;
    grid.gridResolution = 8;
    for (const Vec3f& dir : HemisphereSamples()) {
        const Vec2f tc = OctaTileCoord(dir, grid);
        int ni = -1, nj = -1;
        OctaNearestTile(dir, grid, ni, nj);
        EXPECT_EQ(std::min(grid.gridResolution - 1, static_cast<int>(std::floor(tc.x))), ni);
        EXPECT_EQ(std::min(grid.gridResolution - 1, static_cast<int>(std::floor(tc.y))), nj);
    }
}

TEST(OctaImpostor, NormalAtlasPathPreservesDirectoryAndFilenameExtension) {
    using Luminumbra::Rendering::ImpostorNormalAtlasPath;
    EXPECT_EQ(ImpostorNormalAtlasPath("capture.v1/atlas").generic_string(),
              "capture.v1/atlas_normal");
    EXPECT_EQ(ImpostorNormalAtlasPath("/capture.v1/atlas").generic_string(),
              "/capture.v1/atlas_normal");
    EXPECT_EQ(ImpostorNormalAtlasPath("out/atlas.png").generic_string(), "out/atlas_normal.png");
    EXPECT_EQ(ImpostorNormalAtlasPath("out/atlas.v2.png").generic_string(),
              "out/atlas.v2_normal.png");
    EXPECT_EQ(ImpostorNormalAtlasPath("atlas").generic_string(), "atlas_normal");
    EXPECT_EQ(ImpostorNormalAtlasPath("atlas.png").generic_string(), "atlas_normal.png");
    EXPECT_EQ(ImpostorNormalAtlasPath("capture.v1/atlas.png").generic_string(),
              "capture.v1/atlas_normal.png");
    EXPECT_EQ(ImpostorNormalAtlasPath("out/.atlas").generic_string(), "out/.atlas_normal");
    EXPECT_EQ(ImpostorNormalAtlasPath("out/atlas.").generic_string(), "out/atlas_normal.");
}

} // namespace

namespace {
using namespace Luminumbra::Rendering;

std::vector<std::uint8_t> CoveragePixels(std::size_t pixels, std::size_t covered) {
    std::vector<std::uint8_t> rgba(pixels * 4u, 0);
    for (std::size_t pixel = 0; pixel < covered; ++pixel) {
        rgba[pixel * 4u + 3u] = 255;
    }
    return rgba;
}

TEST(ImpostorCoverage, OpaqueAndEmptyAreRefused) {
    const auto opaque = MeasureImpostorCoverage(CoveragePixels(16, 16), 4, 4);
    EXPECT_EQ(opaque.state, ImpostorCoverageState::Opaque);
    EXPECT_DOUBLE_EQ(opaque.coverage, 1.0);
    EXPECT_TRUE(opaque.refused());
    const auto empty = MeasureImpostorCoverage(CoveragePixels(16, 0), 4, 4);
    EXPECT_EQ(empty.state, ImpostorCoverageState::Empty);
    EXPECT_DOUBLE_EQ(empty.coverage, 0.0);
    EXPECT_TRUE(empty.refused());
}

TEST(ImpostorCoverage, SilhouetteMeasuresEveryViewCell) {
    auto rgba = CoveragePixels(64, 0);
    // Each 4x4 view has a 2x3 canopy and a one-texel trunk.
    for (std::size_t y = 0; y < 8; ++y) {
        for (std::size_t x = 0; x < 8; ++x) {
            if ((y % 4u < 3u && (x % 4u == 1u || x % 4u == 2u)) || (y % 4u == 3u && x % 4u == 2u)) {
                rgba[(y * 8u + x) * 4u + 3u] = 128;
            }
        }
    }
    const auto result = MeasureImpostorCoverage(rgba, 8, 8, 2);
    ASSERT_TRUE(result.usable());
    EXPECT_FALSE(result.refused());
    EXPECT_DOUBLE_EQ(result.coverage, 7.0 / 16.0);
    ASSERT_EQ(result.cell_count, 4u);
    for (std::size_t cell = 0; cell < result.cell_count; ++cell) {
        EXPECT_DOUBLE_EQ(result.cells[cell], 7.0 / 16.0);
    }
    EXPECT_DOUBLE_EQ(result.min_cell, 7.0 / 16.0);
    EXPECT_DOUBLE_EQ(result.max_cell, 7.0 / 16.0);
}

TEST(ImpostorCoverage, CellsHaveIndependentCountsAndExtrema) {
    auto rgba = CoveragePixels(16, 0);
    for (std::size_t pixel : {2u, 3u, 8u, 9u, 10u, 11u, 14u, 15u}) {
        rgba[pixel * 4u + 3u] = 255;
    }
    const auto result = MeasureImpostorCoverage(rgba, 4, 4, 2);
    // Whole-atlas coverage is 0.5, but an empty view cell still refuses.
    EXPECT_EQ(result.state, ImpostorCoverageState::Empty);
    EXPECT_TRUE(result.refused());
    EXPECT_DOUBLE_EQ(result.coverage, 0.5);
    EXPECT_DOUBLE_EQ(result.cells[0], 0.0);
    EXPECT_DOUBLE_EQ(result.cells[1], 0.5);
    EXPECT_DOUBLE_EQ(result.cells[2], 0.5);
    EXPECT_DOUBLE_EQ(result.cells[3], 1.0);
    EXPECT_DOUBLE_EQ(result.min_cell, 0.0);
    EXPECT_DOUBLE_EQ(result.max_cell, 1.0);
}

TEST(ImpostorCoverage, OneEmptyOrOpaqueViewCellRefusesAnAcceptableAverage) {
    // 8x8 atlas, 2x2 grid of 4x4 views. Three views hold a 2x3 silhouette;
    // the fourth is either fully empty or fully opaque.
    for (const bool opaque_view : {false, true}) {
        auto rgba = CoveragePixels(64, 0);
        for (std::size_t y = 0; y < 8; ++y) {
            for (std::size_t x = 0; x < 8; ++x) {
                const bool last_view = y >= 4u && x >= 4u;
                const bool silhouette = y % 4u < 3u && (x % 4u == 1u || x % 4u == 2u);
                if (last_view ? opaque_view : silhouette) {
                    rgba[(y * 8u + x) * 4u + 3u] = 255;
                }
            }
        }
        const auto result = MeasureImpostorCoverage(rgba, 8, 8, 2);
        EXPECT_GT(result.coverage, kImpostorEmptyCoverage);
        EXPECT_LT(result.coverage, kImpostorOpaqueCoverage);
        EXPECT_EQ(result.state,
                  opaque_view ? ImpostorCoverageState::Opaque : ImpostorCoverageState::Empty);
        EXPECT_TRUE(result.refused());
        EXPECT_FALSE(result.usable());
        EXPECT_DOUBLE_EQ(result.cells[3], opaque_view ? 1.0 : 0.0);
    }
}

TEST(ImpostorCoverage, InclusiveThresholdBoundariesAndAdjacentTexels) {
    struct Case {
        std::size_t covered;
        ImpostorCoverageState state;
    };
    for (const Case& sample : {Case{1, ImpostorCoverageState::Empty},
                               Case{2, ImpostorCoverageState::Empty},
                               Case{3, ImpostorCoverageState::Accepted},
                               Case{979, ImpostorCoverageState::Accepted},
                               Case{980, ImpostorCoverageState::Opaque},
                               Case{981, ImpostorCoverageState::Opaque}}) {
        const auto result = MeasureImpostorCoverage(CoveragePixels(1000, sample.covered), 1000, 1);
        EXPECT_EQ(result.state, sample.state) << sample.covered;
        EXPECT_DOUBLE_EQ(result.coverage, static_cast<double>(sample.covered) / 1000.0);
    }
    EXPECT_DOUBLE_EQ(kImpostorEmptyCoverage, 0.002);
    EXPECT_DOUBLE_EQ(kImpostorOpaqueCoverage, 0.98);
    const std::array<std::uint8_t, 8> edge{255, 255, 255, 127, 0, 0, 0, 128};
    EXPECT_DOUBLE_EQ(MeasureImpostorCoverage(edge, 2, 1).coverage, 0.5);
}

TEST(ImpostorCoverage, RejectsInvalidDimensionsAndPartialOrExcessData) {
    struct Dimensions {
        int width;
        int height;
        int grid;
    };
    for (const Dimensions& size : {Dimensions{0, 4, 1},
                                   Dimensions{4, 0, 1},
                                   Dimensions{-1, 4, 1},
                                   Dimensions{4, -1, 1},
                                   Dimensions{std::numeric_limits<int>::max(), 4, 1},
                                   Dimensions{4, std::numeric_limits<int>::max(), 1},
                                   Dimensions{kImpostorMaxAtlasSide + 1, 4, 1},
                                   Dimensions{4, kImpostorMaxAtlasSide + 1, 1},
                                   Dimensions{4, 4, 0},
                                   Dimensions{4, 4, -1},
                                   Dimensions{4, 4, kImpostorMaxGrid + 1},
                                   Dimensions{4, 4, 3},
                                   Dimensions{4, 4, 8}}) {
        EXPECT_EQ(ImpostorRgbaBytes(size.width, size.height, size.grid), 0u);
        const auto result = MeasureImpostorCoverage({}, size.width, size.height, size.grid);
        EXPECT_EQ(result.state, ImpostorCoverageState::Invalid);
        EXPECT_TRUE(result.refused());
        EXPECT_EQ(result.cell_count, 0u);
        EXPECT_DOUBLE_EQ(result.coverage, -1.0);
    }
    for (std::size_t bytes : {0u, 63u, 65u}) {
        const auto result = MeasureImpostorCoverage(std::vector<std::uint8_t>(bytes), 4, 4);
        EXPECT_EQ(result.state, ImpostorCoverageState::Invalid);
        EXPECT_TRUE(result.refused());
    }
    EXPECT_EQ(ImpostorRgbaBytes(kImpostorMaxAtlasSide, kImpostorMaxAtlasSide, kImpostorMaxGrid),
              268435456u);
    // 32x32 atlas at the maximal 16x16 grid: every 2x2 view keeps two covered texels.
    auto maximal_rgba = CoveragePixels(1024, 0);
    for (std::size_t pixel = 0; pixel < 1024u; pixel += 2u) {
        maximal_rgba[pixel * 4u + 3u] = 255;
    }
    const auto maximal_grid = MeasureImpostorCoverage(maximal_rgba, 32, 32, 16);
    EXPECT_TRUE(maximal_grid.usable());
    EXPECT_EQ(maximal_grid.cell_count, 256u);
    EXPECT_DOUBLE_EQ(maximal_grid.min_cell, 0.5);
    EXPECT_DOUBLE_EQ(maximal_grid.max_cell, 0.5);
}

TEST(ImpostorCoverage, RefusedAtlasCannotDrawAndRetainsMeshLod) {
    ImpostorAtlasTextures atlas;
    atlas.ok = true;
    atlas.albedoTex = 1;
    atlas.normalTex = 2;
    EXPECT_FALSE(atlas.drawable());
    EXPECT_FALSE(atlas.alpha_coverage.refused());
    for (std::size_t covered : {0u, 2u, 980u, 1000u}) {
        atlas.alpha_coverage = MeasureImpostorCoverage(CoveragePixels(1000, covered), 1000, 1);
        EXPECT_TRUE(atlas.alpha_coverage.refused());
        EXPECT_FALSE(atlas.drawable());
        EXPECT_EQ(SelectTreeLod(10000.0f, AuthoredTreeLodConfig(atlas.drawable())), 2);
    }
    atlas.alpha_coverage = MeasureImpostorCoverage({}, 4, 4);
    EXPECT_FALSE(atlas.drawable());
    atlas.alpha_coverage = MeasureImpostorCoverage(CoveragePixels(16, 8), 4, 4);
    EXPECT_TRUE(atlas.drawable());
    EXPECT_EQ(SelectTreeLod(10000.0f, AuthoredTreeLodConfig(atlas.drawable())), 3);
    atlas.albedoTex = 0;
    EXPECT_FALSE(atlas.drawable());
    atlas.albedoTex = 1;
    atlas.normalTex = 0;
    EXPECT_FALSE(atlas.drawable());
    atlas.normalTex = 2;
    atlas.ok = false;
    EXPECT_FALSE(atlas.drawable());
}
} // namespace
