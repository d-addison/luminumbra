// Unit tests for the hemi-octahedral impostor mapping (src/.../rendering/OctaImpostor.h).
// GL-free, deterministic — the same discipline as tree_lod_test.cpp. These pin the
// math that both the atlas BAKE (tile -> capture direction) and the runtime impostor
// SHADER (view direction -> tile) rely on, so the two can never drift apart.

#include "luminumbra_client/rendering/OctaImpostor.h"

#include <gtest/gtest.h>

#include <cmath>
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

TEST(OctaImpostor, TileDirectionsAreUnitAndUpperHemisphere) {
    OctaImpostorGrid grid;
    grid.gridResolution = 8;
    for (int j = 0; j < grid.gridResolution; ++j) {
        for (int i = 0; i < grid.gridResolution; ++i) {
            const Vec3f dir = OctaTileDirection(i, j, grid);
            EXPECT_NEAR(std::sqrt(Dot(dir, dir)), 1.0f, 1e-4f)
                << "tile (" << i << "," << j << ") not unit";
            EXPECT_GE(dir.y, -1e-4f) << "tile (" << i << "," << j << ") points below horizon";
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

} // namespace
