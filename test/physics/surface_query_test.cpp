// Physics surface queries for audio raycasts.
//
// PhysicsSystem::audio_raycast used to hardcode an upward surface normal and a
// Y-band material classifier. It now queries the hit Jolt body for the REAL
// surface normal (Body::GetWorldSpaceSurfaceNormal, flipped to oppose the ray)
// and, when the worldgen seam is attached (set_world_system), classifies the
// surface material through SHIELD_WorldSystem::SurfaceVertexMaterial -- the
// same biome-aware function the mesher uses. These tests pin:
//   1. flat floor -> normal (0,1,0);
//   2. a ramp -> the analytic slope normal, and a from-below cast still gets a
//      normal OPPOSING the ray (the opposition-flip invariant);
//   3. a dynamic sphere -> radial normal + the non-terrain Stone fallback;
//   4. the null-world legacy Y-band path stays byte-identical (absorption pin);
//   5. terrain material matches SurfaceVertexMaterial at the hit point.
//
// Terrain colliders are built through the real add_chunk_collision heightfield
// path (the collider the live world uses), same fixture idiom as
// jolt_tunneling_test. Registered as its own gtest exe (surface_query_test);
// links luminumbra_common, which carries Jolt.
#include "gtest/gtest.h"

#include "luminumbra/core/Types.h"
#include "systems/PhysicsSystem.h"
#include "systems/SHIELD_WorldSystem.h"
#include "world/Chunk.h"

#include <glm/glm.hpp>

#include <cmath>
#include <memory>

namespace {

using Luminumbra::Chunk;
using Luminumbra::IVec3;
using Luminumbra::MaterialType;
using Luminumbra::Systems::PhysicsSystem;
using Luminumbra::Systems::SHIELD_WorldSystem;
using Luminumbra::Systems::TerrainGenParams;

// Builds a chunk with a FLAT heightmap floor at world height `floor_y` (same
// fixture as jolt_tunneling_test): the HeightFieldShape resolution is
// CHUNK_SIZE_X+1 per side, and add_chunk_collision gates on the chunk-center
// sample lying inside [cc.y*CHUNK_SIZE_Y, (cc.y+1)*CHUNK_SIZE_Y), so floor_y
// must sit in [0,16) for a chunk at cc.y=0.
std::shared_ptr<Chunk> MakeFlatFloorChunk(const IVec3& coords, float floor_y) {
    auto chunk = std::make_shared<Chunk>(coords);
    const int side = Luminumbra::CHUNK_SIZE_X + 1;
    chunk->heightmap_data.assign(static_cast<std::size_t>(side) * static_cast<std::size_t>(side),
                                 floor_y);
    return chunk;
}

// Builds a chunk whose heightmap is the plane h = 4 + 0.5*x (constant in z), a
// ramp whose analytic surface normal is normalize(-0.5, 1, 0). Jolt heightfield
// samples are laid out row-major [z*side + x] with the sample at local (x, z).
std::shared_ptr<Chunk> MakeRampChunk(const IVec3& coords) {
    auto chunk = std::make_shared<Chunk>(coords);
    const int side = Luminumbra::CHUNK_SIZE_X + 1;
    chunk->heightmap_data.resize(static_cast<std::size_t>(side) * static_cast<std::size_t>(side));
    for (int z = 0; z < side; ++z) {
        for (int x = 0; x < side; ++x) {
            chunk->heightmap_data[static_cast<std::size_t>(z) * side + x] =
                4.0f + 0.5f * static_cast<float>(x);
        }
    }
    return chunk;
}

// Default-params world system (biomes/rivers off): the KnobLayer_test idiom --
// no job system, no water system, pure analytic worldgen queries only.
std::unique_ptr<SHIELD_WorldSystem> MakeWorldSystem() {
    TerrainGenParams params;
    return std::make_unique<SHIELD_WorldSystem>(nullptr, nullptr, params, 4242);
}

} // namespace

TEST(SurfaceQuery, FlatFloorReportsUpwardNormal) {
    PhysicsSystem physics;
    physics.startup();
    ASSERT_TRUE(physics.is_started());

    auto floor = MakeFlatFloorChunk(IVec3(0, 0, 0), 8.0f);
    physics.add_chunk_collision(*floor);

    const auto result =
        physics.audio_raycast(glm::vec3(8.0f, 20.0f, 8.0f), glm::vec3(8.0f, 0.0f, 8.0f));
    ASSERT_TRUE(result.hit);
    EXPECT_NEAR(result.hit_point.y, 8.0f, 1e-3f);
    EXPECT_NEAR(result.surface_normal.x, 0.0f, 1e-3f);
    EXPECT_NEAR(result.surface_normal.y, 1.0f, 1e-3f);
    EXPECT_NEAR(result.surface_normal.z, 0.0f, 1e-3f);

    physics.shutdown();
}

TEST(SurfaceQuery, RampReportsSlopeNormal) {
    PhysicsSystem physics;
    physics.startup();
    ASSERT_TRUE(physics.is_started());

    auto ramp = MakeRampChunk(IVec3(0, 0, 0));
    physics.add_chunk_collision(*ramp);

    // Downward cast onto the ramp (off the lattice lines so no vertex/edge is
    // hit exactly). The plane h = 4 + 0.5*x has normal normalize(-0.5, 1, 0).
    const auto result =
        physics.audio_raycast(glm::vec3(8.3f, 20.0f, 8.3f), glm::vec3(8.3f, 0.0f, 8.3f));
    ASSERT_TRUE(result.hit);
    const glm::vec3 expected = glm::normalize(glm::vec3(-0.5f, 1.0f, 0.0f));
    EXPECT_NEAR(result.surface_normal.x, expected.x, 0.05f);
    EXPECT_NEAR(result.surface_normal.y, expected.y, 0.05f);
    EXPECT_NEAR(result.surface_normal.z, expected.z, 0.05f);

    physics.shutdown();
}

TEST(SurfaceQuery, FromBelowCastNormalOpposesRay) {
    PhysicsSystem physics;
    physics.startup();
    ASSERT_TRUE(physics.is_started());

    auto ramp = MakeRampChunk(IVec3(0, 0, 0));
    physics.add_chunk_collision(*ramp);

    // Cast UP through the ramp from underneath. The heightfield triangles face
    // up, so the raw surface normal points ALONG the ray -- the opposition flip
    // must hand back a normal facing AGAINST it (a reflection off a downward-
    // facing normal is what a cave/under-terrain audio bounce needs).
    const glm::vec3 from(8.3f, 0.5f, 8.3f);
    const glm::vec3 to(8.3f, 20.0f, 8.3f);
    const auto result = physics.audio_raycast(from, to);
    ASSERT_TRUE(result.hit);
    const glm::vec3 ray_dir = glm::normalize(to - from);
    EXPECT_LT(glm::dot(result.surface_normal, ray_dir), 0.0f)
        << "surface normal does not oppose the from-below ray (normal y=" << result.surface_normal.y
        << ")";

    physics.shutdown();
}

TEST(SurfaceQuery, DynamicSphereReportsRadialNormalAndStoneFallback) {
    PhysicsSystem physics;
    physics.startup();
    ASSERT_TRUE(physics.is_started());

    auto world = MakeWorldSystem();
    physics.set_world_system(world.get());

    auto floor = MakeFlatFloorChunk(IVec3(0, 0, 0), 8.0f);
    physics.add_chunk_collision(*floor);

    // A dynamic sphere hanging above the floor; cast a horizontal ray at its
    // center height so the first hit is the sphere, not the terrain.
    constexpr float kRadius = 0.5f;
    const glm::vec3 center(8.0f, 12.0f, 8.0f);
    const JPH::BodyID ball = physics.create_dynamic_sphere(center, glm::vec3(0.0f), kRadius);
    ASSERT_FALSE(ball.IsInvalid());

    const auto result =
        physics.audio_raycast(glm::vec3(2.0f, 12.0f, 8.0f), glm::vec3(14.0f, 12.0f, 8.0f));
    ASSERT_TRUE(result.hit);
    EXPECT_NEAR(result.hit_point.x, center.x - kRadius, 1e-2f);

    // Radial normal: at the hit point the sphere's outward normal is the unit
    // vector from the center to the hit point (here ~(-1, 0, 0)).
    const glm::vec3 radial = glm::normalize(result.hit_point - center);
    EXPECT_NEAR(result.surface_normal.x, radial.x, 1e-2f);
    EXPECT_NEAR(result.surface_normal.y, radial.y, 1e-2f);
    EXPECT_NEAR(result.surface_normal.z, radial.z, 1e-2f);

    // Non-terrain body with a world system attached -> the Stone fallback.
    EXPECT_EQ(result.material_type, static_cast<int>(MaterialType::Stone));
    EXPECT_FLOAT_EQ(result.material_absorption,
                    physics.get_material_audio_absorption(result.material_type));

    physics.destroy_body(ball);
    physics.shutdown();
}

TEST(SurfaceQuery, NullWorldKeepsLegacyYBandClassification) {
    PhysicsSystem physics;
    physics.startup();
    ASSERT_TRUE(physics.is_started());

    // NO set_world_system: the legacy Y-band path must stay byte-identical.
    auto floor = MakeFlatFloorChunk(IVec3(0, 0, 0), 8.0f);
    physics.add_chunk_collision(*floor);

    const auto result =
        physics.audio_raycast(glm::vec3(8.0f, 20.0f, 8.0f), glm::vec3(8.0f, 0.0f, 8.0f));
    ASSERT_TRUE(result.hit);
    // Hit at y ~= 8 < 10 -> legacy Stone band: code 0, absorption exactly 0.15.
    EXPECT_EQ(result.material_type, 0);
    EXPECT_FLOAT_EQ(result.material_absorption, 0.15f);

    physics.shutdown();
}

TEST(SurfaceQuery, TerrainMaterialMatchesSurfaceVertexMaterial) {
    PhysicsSystem physics;
    physics.startup();
    ASSERT_TRUE(physics.is_started());

    auto world = MakeWorldSystem();
    physics.set_world_system(world.get());

    auto floor = MakeFlatFloorChunk(IVec3(0, 0, 0), 8.0f);
    physics.add_chunk_collision(*floor);

    const auto result =
        physics.audio_raycast(glm::vec3(8.0f, 20.0f, 8.0f), glm::vec3(8.0f, 0.0f, 8.0f));
    ASSERT_TRUE(result.hit);

    // The terrain hit must classify through the SAME biome-aware function the
    // mesher uses, fed the hit point (whose y IS the cached terrain height).
    const MaterialType expected =
        world->SurfaceVertexMaterial(result.hit_point.x, result.hit_point.z, result.hit_point.y);
    EXPECT_EQ(result.material_type, static_cast<int>(expected));
    EXPECT_FLOAT_EQ(result.material_absorption,
                    physics.get_material_audio_absorption(result.material_type));

    physics.shutdown();
}
