#include "../fixtures/TerrainFixtureParams.h"
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
#include "systems/WaterSystem.h"
#include "world/Chunk.h"
#include "world/TerrainCollision.h"

#include <glm/glm.hpp>

#include <array>
#include <cmath>
#include <limits>
#include <memory>
#include <stdexcept>
#include <utility>
#include <vector>

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
            chunk->heightmap_data[static_cast<std::size_t>(z) * static_cast<std::size_t>(side) +
                                  static_cast<std::size_t>(x)] =
                4.0f + 0.5f * static_cast<float>(x);
        }
    }
    return chunk;
}

// Default-params world system (biomes/rivers off): the KnobLayer_test idiom --
// no job system, no water system, pure analytic worldgen queries only.
std::unique_ptr<SHIELD_WorldSystem> MakeWorldSystem() {
    TerrainGenParams params = Luminumbra::Test::SyntheticTerrainParams();
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

TEST(SurfaceQuery, AvatarFeetSettleAboveNewlyAdmittedFloor) {
    PhysicsSystem physics;
    physics.startup();
    ASSERT_TRUE(physics.is_started());
    constexpr float kFloorHeight = 256.0f;
    for (const int column : {-5121, 0, 5120}) {
        const IVec3 coords(column, 16, column);
        const float centre = static_cast<float>(column * Luminumbra::CHUNK_SIZE_X) + 8.0f;
        const glm::vec3 feet(centre, kFloorHeight + 0.25f, centre);
        const auto avatar = physics.create_avatar_character(feet);
        EXPECT_EQ(physics.get_avatar_position(avatar), feet);
        auto floor = MakeFlatFloorChunk(coords, kFloorHeight);
        physics.add_chunk_collision(*floor);
    }
    for (int tick = 0; tick < 30; ++tick) {
        physics.update_avatars(1.0f / 30.0f);
        for (std::size_t avatar = 0; avatar < 3; ++avatar) {
            EXPECT_GT(physics.get_avatar_position(avatar).y, kFloorHeight - 0.25f);
        }
    }
    for (std::size_t avatar = 0; avatar < 3; ++avatar) {
        EXPECT_NEAR(physics.get_avatar_position(avatar).y, kFloorHeight, 0.1f);
        EXPECT_TRUE(physics.is_avatar_grounded(avatar));
    }
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

TEST(SurfaceQuery, IncompleteHeightmapsNeverCreateCollision) {
    PhysicsSystem physics;
    physics.startup();
    ASSERT_TRUE(physics.is_started());
    constexpr std::size_t side = Luminumbra::CHUNK_SIZE_X + 1;
    for (const auto count : {std::size_t{0}, std::size_t{1}, side * side - 1, side * side + 1}) {
        auto chunk = MakeFlatFloorChunk(IVec3(0, 0, 0), 8.0f);
        chunk->heightmap_data.resize(count, 8.0f);
        physics.add_chunk_collision(*chunk);
        EXPECT_FALSE(physics.audio_raycast({8, 12, 8}, {8, 4, 8}).hit);
    }
    auto complete = MakeFlatFloorChunk(IVec3(0, 0, 0), 8.0f);
    physics.add_chunk_collision(*complete);
    EXPECT_TRUE(physics.audio_raycast({8, 12, 8}, {8, 4, 8}).hit);
    physics.shutdown();
}

// A continuation restores a terrain collider under its saved BodyID. Removing and
// re-adding it must then allocate the generation an uninterrupted run allocates.
TEST(SurfaceQuery, RestoredTerrainColliderReusesTheUninterruptedGeneration) {
    using Owners = std::vector<std::pair<Luminumbra::ChunkID, JPH::BodyID>>;
    auto floor = MakeFlatFloorChunk(IVec3(0, 0, 0), 8.0f);
    const auto owner = [](const PhysicsSystem& physics) {
        Owners owners;
        EXPECT_TRUE(physics.capture_chunk_collision_owners(owners));
        return owners.size() == 1 ? owners.front().second : JPH::BodyID();
    };

    PhysicsSystem uninterrupted;
    uninterrupted.startup();
    ASSERT_TRUE(uninterrupted.add_chunk_collision(*floor));
    uninterrupted.remove_chunk_collision(floor->get_id());
    ASSERT_TRUE(uninterrupted.add_chunk_collision(*floor));
    const JPH::BodyID saved = owner(uninterrupted);
    ASSERT_FALSE(saved.IsInvalid());
    uninterrupted.remove_chunk_collision(floor->get_id());
    ASSERT_TRUE(uninterrupted.add_chunk_collision(*floor));
    const JPH::BodyID expected = owner(uninterrupted);
    ASSERT_EQ(expected.GetIndex(), saved.GetIndex());
    ASSERT_NE(expected, saved);

    PhysicsSystem restored;
    restored.startup();
    ASSERT_TRUE(restored.add_chunk_collision(*floor, saved));
    EXPECT_EQ(owner(restored), saved);
    restored.remove_chunk_collision(floor->get_id());
    ASSERT_TRUE(restored.add_chunk_collision(*floor));
    EXPECT_EQ(owner(restored), expected);

    restored.shutdown();
    uninterrupted.shutdown();
}

namespace {
class TerrainEditCollision : public ::testing::TestWithParam<bool> {
protected:
    void SetUp() override {
        auto params = Luminumbra::Test::SyntheticTerrainParams();
        params.base_amplitude = 0;
        params.peaks_amplitude = 0;
        params.height_offset = 8;
        params.continental_spline = {{{-1, 0}}, {{1, 0}}};
        params.caves_enabled = false;
        world = std::make_unique<SHIELD_WorldSystem>(nullptr, nullptr, params, 4242);
        world->set_durable_residency(GetParam());
        physics.startup();
        physics.set_world_system(world.get());
        floor = MakeFlatFloorChunk({0, 0, 0}, 8);
        world->GenerateChunkData(*floor);
        ASSERT_TRUE(world->adopt_streamed_chunk(floor));
        ASSERT_TRUE(physics.add_chunk_collision(*floor));
        const std::array<Luminumbra::Vec3, 1> anchors{{{8, 16, 8}}};
        ASSERT_TRUE(physics.prepare_simulation_collisions(anchors, 7));
    }

    void DropAvatar(float expected, float tolerance) {
        const auto avatar = physics.create_avatar_character({8.25f, 16, 8.25f});
        ASSERT_NE(avatar, PhysicsSystem::kInvalidAvatar);
        for (int tick = 0; tick < 120; ++tick) {
            ASSERT_TRUE(physics.prepare_simulation_collisions({}));
            physics.update(1.0f / 60);
            physics.update_avatars(1.0f / 60);
        }
        EXPECT_NEAR(physics.get_avatar_position(avatar).y, expected, tolerance);
        EXPECT_TRUE(physics.is_avatar_grounded(avatar));
    }

    std::unique_ptr<SHIELD_WorldSystem> world;
    PhysicsSystem physics;
    std::shared_ptr<Chunk> floor;
};
} // namespace

TEST_P(TerrainEditCollision, DigRefreshesRayAvatarAndBody) {
    const auto saved_heightmap = floor->heightmap_data;
    const auto initial = physics.audio_raycast({8.25f, 20, 8.25f}, {8.25f, -4, 8.25f});
    ASSERT_TRUE(initial.hit);
    EXPECT_NEAR(initial.hit_point.y, 8, 0.01f);
    ASSERT_GT(world->EditTerrainVoxel({8, 8, 8}, 4, false, &physics), 0);
    const std::array<Luminumbra::Vec3, 1> anchors{{{8, 16, 8}}};
    ASSERT_TRUE(physics.prepare_simulation_collisions(anchors, 7)); // Same tick as before edit.
    const auto dug = physics.audio_raycast({8.25f, 20, 8.25f}, {8.25f, -4, 8.25f});
    ASSERT_TRUE(dug.hit);
    EXPECT_LT(dug.hit_point.y, 5);
    EXPECT_EQ(floor->heightmap_data, saved_heightmap);
    const auto body = physics.create_dynamic_sphere({6.75f, 18, 8.25f}, {0, 0, 0}, 0.2f);
    ASSERT_FALSE(body.IsInvalid());
    DropAvatar(4.2f, 0.8f);
    EXPECT_NEAR(physics.get_body_position(body).y, 4.4f, 0.8f);
    const auto work = world->simulation_collision_sdf_samples();
    const auto hash = physics.simulation_collision_hash();
    for (int attempt = 0; attempt < 8; ++attempt)
        ASSERT_TRUE(physics.prepare_simulation_collisions({}));
    EXPECT_EQ(world->simulation_collision_sdf_samples(), work);
    EXPECT_EQ(physics.simulation_collision_hash(), hash);
}

TEST_P(TerrainEditCollision, FillRefreshesRayAndAvatar) {
    ASSERT_GT(world->EditTerrainVoxel({8, 8, 8}, 4, true, &physics), 0);
    const auto filled = physics.audio_raycast({8.25f, 20, 8.25f}, {8.25f, 0, 8.25f});
    ASSERT_TRUE(filled.hit);
    EXPECT_GT(filled.hit_point.y, 11);
    DropAvatar(11.8f, 0.8f);
}

TEST_P(TerrainEditCollision, EditRetainsDistantAnchorOnlyCollision) {
    auto distant = MakeFlatFloorChunk({10, 0, 0}, 8);
    world->GenerateChunkData(*distant);
    ASSERT_TRUE(world->adopt_streamed_chunk(distant));
    ASSERT_TRUE(physics.add_chunk_collision(*distant));
    const std::array<Luminumbra::Vec3, 2> anchors{{{8, 16, 8}, {168, 16, 8}}};
    ASSERT_TRUE(physics.prepare_simulation_collisions(anchors, 8));
    ASSERT_TRUE(physics.audio_raycast({168, 20, 8}, {168, 0, 8}).hit);
    ASSERT_GT(world->EditTerrainVoxel({8, 8, 8}, 4, false, &physics), 0);
    EXPECT_TRUE(physics.simulation_collisions_ready());
    const auto result = physics.audio_raycast({168, 20, 8}, {168, 0, 8});
    ASSERT_TRUE(result.hit);
    EXPECT_NEAR(result.hit_point.y, 8, 0.01f);
    // Repeated edits must preserve the same anchor-only working set.
    ASSERT_GT(world->EditTerrainVoxel({8, 8, 8}, 4, true, &physics), 0);
    EXPECT_TRUE(physics.audio_raycast({168, 20, 8}, {168, 0, 8}).hit);
}

TEST_P(TerrainEditCollision, RetainedHorizonRefusalPreservesPublishedGeometry) {
    if (!GetParam()) {
        EXPECT_TRUE(physics.refresh_simulation_collisions({}));
        return;
    }
    // Fourteen disjoint rings occupy 126 of the fixed 128 column slots.
    std::array<Luminumbra::Vec3, 14> anchors{};
    for (std::size_t index = 0; index < anchors.size(); ++index) {
        anchors[index] = {8 + static_cast<float>(index) * 64, 16, 8};
    }
    ASSERT_TRUE(physics.prepare_simulation_collisions(anchors));
    const auto published = physics.simulation_collision_hash();
    EXPECT_FALSE(physics.refresh_simulation_collisions({1000, 16, 8}));
    EXPECT_EQ(physics.simulation_collision_hash(), published);
    EXPECT_FALSE(physics.simulation_collisions_ready());
    EXPECT_FALSE(
        physics.refresh_simulation_collisions({std::numeric_limits<float>::quiet_NaN(), 16, 8}));
    EXPECT_EQ(physics.simulation_collision_hash(), published);
    ASSERT_TRUE(physics.refresh_simulation_collisions(anchors.front()));
    EXPECT_TRUE(physics.simulation_collisions_ready());
    EXPECT_EQ(physics.simulation_collision_sample_count(), 126u);

    auto malformed = MakeFlatFloorChunk({4, 0, 0}, 8);
    malformed->mark_sdf_loaded_or_edited();
    malformed->heightmap_data.resize(1);
    ASSERT_TRUE(world->adopt_streamed_chunk(malformed));
    EXPECT_FALSE(physics.refresh_simulation_collisions(anchors.front()));
    EXPECT_EQ(physics.simulation_collision_hash(), published);
    malformed->heightmap_data.assign(17 * 17, 8);
    ASSERT_TRUE(physics.refresh_simulation_collisions(anchors.front()));
    EXPECT_TRUE(physics.audio_raycast({72, 20, 8}, {72, 0, 8}).hit);
}

TEST_P(TerrainEditCollision, RefreshAdmitsBrushAfterClearedOrEmptyHorizon) {
    physics.clear_simulation_collisions();
    ASSERT_TRUE(physics.refresh_simulation_collisions({8, 16, 8}));
    EXPECT_TRUE(physics.audio_raycast({8, 20, 8}, {8, 0, 8}).hit);
    ASSERT_TRUE(physics.prepare_simulation_collisions({}));
    ASSERT_TRUE(physics.refresh_simulation_collisions({8, 16, 8}));
    EXPECT_TRUE(physics.audio_raycast({8, 20, 8}, {8, 0, 8}).hit);
}

TEST_P(TerrainEditCollision, ColdInventoryAllocatesProjectionSamplesOnlyWhenQueried) {
    // Use a fresh world so resident fixture data cannot hide cold allocations.
    auto cold = std::make_unique<SHIELD_WorldSystem>(nullptr, nullptr, world->get_params(), 4242);
    cold->set_durable_residency(true);
    std::vector<IVec3> inventory;
    for (int column = 0; column < 512; ++column) {
        inventory.emplace_back(column, 1, 0);
    }
    std::size_t reads = 0;
    cold->set_durable_chunk_loader(
        [&](const IVec3& coords) {
            ++reads;
            auto slab = MakeFlatFloorChunk(coords, 8);
            slab->sdf_data.assign(17 * 17 * 17, 1);
            slab->mark_sdf_loaded_or_edited();
            return slab;
        },
        {});
    cold->remember_durable_chunks(inventory);
    EXPECT_EQ(cold->simulation_collision_projection_bytes(), 0u);
    EXPECT_EQ(reads, 0u);
    std::array<float, 17 * 17> heights{};
    ASSERT_TRUE(cold->sample_simulation_collision(0, 0, heights));
    const auto bytes = cold->simulation_collision_projection_bytes();
    EXPECT_EQ(bytes, heights.size() * (2 * sizeof(float) + sizeof(Luminumbra::i32)));
    EXPECT_EQ(reads, 1u);
    cold->set_durable_chunk_loader([](const IVec3&) -> std::shared_ptr<Chunk> { return {}; }, {});
    EXPECT_FALSE(cold->sample_simulation_collision(1, 0, heights));
    EXPECT_EQ(cold->simulation_collision_projection_bytes(), bytes);
    cold->replace_durable_chunks({});
    EXPECT_EQ(cold->simulation_collision_projection_bytes(), 0u);
    EXPECT_TRUE(cold->sample_simulation_collision(0, 0, heights));
}

TEST_P(TerrainEditCollision, SavedBaselineAndUpperSlabsAreEachReadOnce) {
    // The baseline and pending-slab projection share one committed read, even
    // when the record has only water authority or an empty legacy SDF.
    for (const int authority_kind : {0, 1, 2}) {
        SCOPED_TRACE(authority_kind);
        auto cold =
            std::make_unique<SHIELD_WorldSystem>(nullptr, nullptr, world->get_params(), 4242);
        cold->set_durable_residency(true);
        std::array<std::shared_ptr<Chunk>, 2> slabs{MakeFlatFloorChunk({0, 0, 0}, 8),
                                                    MakeFlatFloorChunk({0, 1, 0}, 8)};
        slabs.back()->mark_sdf_loaded_or_edited();
        if (authority_kind == 0) {
            cold->GenerateChunkData(*slabs.front());
            slabs.front()->mark_sdf_loaded_or_edited();
        } else if (authority_kind == 2) {
            slabs.front()->set_water_authority();
        } else {
            slabs.front()->mark_sdf_loaded_or_edited();
        }
        std::array<std::size_t, 2> reads{};
        cold->set_durable_chunk_loader(
            [&](const IVec3& coords) {
                const auto index = static_cast<std::size_t>(coords.y);
                ++reads.at(index);
                return slabs.at(index);
            },
            {});
        cold->remember_durable_chunks({{0, 0, 0}, {0, 1, 0}});
        std::array<float, 17 * 17> heights{};
        for (int preparation = 0; preparation < 3; ++preparation) {
            ASSERT_TRUE(cold->sample_simulation_collision(0, 0, heights));
            EXPECT_EQ(reads, (std::array<std::size_t, 2>{1, 1}));
        }
        cold->remember_durable_chunks({{0, 0, 0}, {0, 1, 0}});
        ASSERT_TRUE(cold->sample_simulation_collision(0, 0, heights));
        EXPECT_EQ(reads, (std::array<std::size_t, 2>{2, 2}));
    }
}

TEST_P(TerrainEditCollision, SavedSlabProjectionRetriesMalformedSdfWithoutInventoryChanges) {
    for (const int slab_y : {0, 1}) {
        for (const int invalid : {0, 1, 2}) {
            SCOPED_TRACE(slab_y);
            SCOPED_TRACE(invalid);
            auto cold =
                std::make_unique<SHIELD_WorldSystem>(nullptr, nullptr, world->get_params(), 4242);
            cold->set_durable_residency(true);
            auto slab = MakeFlatFloorChunk({0, slab_y, 0}, 8);
            slab->sdf_data.assign(17 * 17 * 17, 1);
            slab->mark_sdf_loaded_or_edited();
            if (invalid == 0) {
                slab->sdf_data.resize(1);
            } else {
                slab->sdf_data.front() = invalid == 1 ? std::numeric_limits<float>::quiet_NaN()
                                                      : std::numeric_limits<float>::infinity();
            }
            std::size_t reads = 0;
            cold->set_durable_chunk_loader(
                [&](const IVec3&) {
                    ++reads;
                    return slab;
                },
                {});
            cold->remember_durable_chunks({slab->get_coords()});
            std::array<float, 17 * 17> heights{};
            EXPECT_FALSE(cold->sample_simulation_collision(0, 0, heights));
            EXPECT_EQ(reads, 1u);
            slab->sdf_data.assign(17 * 17 * 17, 1);
            ASSERT_TRUE(cold->sample_simulation_collision(0, 0, heights));
            EXPECT_EQ(reads, 2u);
            ASSERT_TRUE(cold->sample_simulation_collision(0, 0, heights));
            EXPECT_EQ(reads, 2u);
        }
    }
}

TEST_P(TerrainEditCollision, SharedBorderAndVerticalSlabsRefreshTogether) {
    auto neighbour = MakeFlatFloorChunk({1, 0, 0}, 8);
    world->GenerateChunkData(*neighbour);
    ASSERT_TRUE(world->adopt_streamed_chunk(neighbour));
    ASSERT_TRUE(physics.add_chunk_collision(*neighbour));
    ASSERT_GT(world->EditTerrainVoxel({16, 8, 8}, 3, false, &physics), 0);
    for (const float x : {15.75f, 16.25f}) {
        const auto dug = physics.audio_raycast({x, 20, 8.25f}, {x, -4, 8.25f});
        ASSERT_TRUE(dug.hit);
        EXPECT_LT(dug.hit_point.y, 6);
    }
    // A separate fill above the original surface must affect the column even
    // though the original surface slab is outside the brush's vertical AABB.
    // The dig's surface rebuild already admitted the upper margin slab.
    const auto upper = world->find_streamed_chunk({0, 1, 0});
    ASSERT_NE(upper, nullptr);
    ASSERT_EQ(upper->sdf_data.size(), 17u * 17u * 17u);
    ASSERT_GT(world->EditTerrainVoxel({8, 19, 8}, 2, true, &physics), 0);
    const auto filled = physics.audio_raycast({8.25f, 24, 8.25f}, {8.25f, 0, 8.25f});
    ASSERT_TRUE(filled.hit);
    EXPECT_GT(filled.hit_point.y, 20);
}

TEST(SurfaceQuery, AuthoritativeCollisionProjectionRefusesPartialAndNonfiniteSdf) {
    auto floor = MakeFlatFloorChunk({0, 0, 0}, 8);
    floor->mark_sdf_loaded_or_edited();
    std::array<float, 17 * 17> heights{};
    heights.fill(8);
    const auto original = heights;
    EXPECT_TRUE(Luminumbra::World::OverlayTerrainCollision(*floor, heights));
    floor->sdf_data.assign(1, -1);
    EXPECT_FALSE(Luminumbra::World::OverlayTerrainCollision(*floor, heights));
    EXPECT_EQ(heights, original);
    floor->sdf_data.assign(17 * 17 * 17, -1);
    floor->sdf_data.back() = std::numeric_limits<float>::quiet_NaN();
    EXPECT_FALSE(Luminumbra::World::OverlayTerrainCollision(*floor, heights));
    EXPECT_EQ(heights, original);
    floor->sdf_data.back() = std::numeric_limits<float>::infinity();
    EXPECT_FALSE(Luminumbra::World::OverlayTerrainCollision(*floor, heights));
    EXPECT_EQ(heights, original);
    EXPECT_FALSE(Luminumbra::World::OverlayTerrainCollision(*floor, std::span<float>{}));
}

TEST(SurfaceQuery, GeneratedCollisionRetainsOriginalHeightBitsAndExtremeSdfIsFinite) {
    auto floor = MakeFlatFloorChunk({0, 0, 0}, 8);
    floor->sdf_data.assign(17 * 17 * 17, -1);
    std::array<float, 17 * 17> heights{};
    for (std::size_t index = 0; index < heights.size(); ++index)
        heights[index] = 8 + static_cast<float>(index) / 1024;
    const auto original = heights;
    ASSERT_TRUE(Luminumbra::World::OverlayTerrainCollision(*floor, heights));
    EXPECT_EQ(heights, original); // Non-authoritative SDF cannot change old collision bytes.
    for (std::size_t z = 0; z < 17; ++z)
        for (std::size_t y = 0; y < 17; ++y)
            for (std::size_t x = 0; x < 17; ++x)
                floor->sdf_data[x + y * 17 + z * 17 * 17] =
                    y < 8 ? -std::numeric_limits<float>::max() : std::numeric_limits<float>::max();
    floor->mark_sdf_loaded_or_edited();
    ASSERT_TRUE(Luminumbra::World::OverlayTerrainCollision(*floor, heights));
    for (const float height : heights)
        EXPECT_EQ(height, 7.5f);
}

TEST(SurfaceQuery, CollisionProjectionKeepsRoofUntilTopSolidSurfaceIsRemoved) {
    auto floor = MakeFlatFloorChunk({0, 0, 0}, 12.5f);
    floor->sdf_data.assign(17 * 17 * 17, 1);
    for (std::size_t z = 0; z < 17; ++z)
        for (std::size_t y = 0; y < 17; ++y)
            for (std::size_t x = 0; x < 17; ++x)
                if (y < 4 || (y >= 10 && y <= 12))
                    floor->sdf_data[x + y * 17 + z * 17 * 17] = -1;
    floor->mark_sdf_loaded_or_edited();
    std::array<float, 17 * 17> heights{};
    heights.fill(12.5f);
    ASSERT_TRUE(Luminumbra::World::OverlayTerrainCollision(*floor, heights));
    for (const float height : heights)
        EXPECT_EQ(height, 12.5f);
    for (std::size_t z = 0; z < 17; ++z)
        for (std::size_t y = 10; y <= 12; ++y)
            for (std::size_t x = 0; x < 17; ++x)
                floor->sdf_data[x + y * 17 + z * 17 * 17] = 1;
    floor->mark_voxel_data_dirty();
    ASSERT_TRUE(Luminumbra::World::OverlayTerrainCollision(*floor, heights));
    for (const float height : heights)
        EXPECT_EQ(height, 3.5f);
}

TEST_P(TerrainEditCollision, MalformedUpperAuthorityRetainsPublishedRegionGeometry) {
    if (!GetParam()) {
        // Standalone legacy refusal is exercised by the projection regression;
        // here no existing collider may be replaced after shape preparation fails.
        auto partial = MakeFlatFloorChunk({4, 0, 0}, 8);
        partial->sdf_data.assign(1, -1);
        partial->mark_sdf_loaded_or_edited();
        PhysicsSystem standalone;
        standalone.startup();
        EXPECT_FALSE(standalone.add_chunk_collision(*partial));
        EXPECT_FALSE(standalone.chunk_collision_body(partial->get_id()).has_value());
        return;
    }
    const auto published = physics.simulation_collision_hash();
    auto partial = MakeFlatFloorChunk({0, 1, 0}, 8);
    partial->sdf_data.assign(1, -1);
    partial->mark_sdf_loaded_or_edited();
    ASSERT_TRUE(world->adopt_streamed_chunk(partial));
    const std::array<Luminumbra::Vec3, 1> anchors{{{8, 16, 8}}};
    EXPECT_FALSE(physics.prepare_simulation_collisions(anchors, 8));
    EXPECT_EQ(physics.simulation_collision_hash(), published);
    EXPECT_FALSE(physics.simulation_collisions_ready());
    const auto hit = physics.audio_raycast({8.25f, 20, 8.25f}, {8.25f, 0, 8.25f});
    ASSERT_TRUE(hit.hit);
    EXPECT_NEAR(hit.hit_point.y, 8, 0.01f);
}

TEST(SurfaceQuery, RestoredLegacyColliderUsesProjectedSavedSlabsAndRejectsInvalidSamples) {
    auto floor = MakeFlatFloorChunk({0, 0, 0}, 8);
    auto upper = MakeFlatFloorChunk({0, 1, 0}, 8);
    upper->sdf_data.assign(17 * 17 * 17, 1);
    for (std::size_t z = 0; z < 17; ++z)
        for (std::size_t y = 3; y <= 5; ++y)
            for (std::size_t x = 0; x < 17; ++x)
                upper->sdf_data[x + y * 17 + z * 17 * 17] = -1;
    upper->mark_sdf_loaded_or_edited();
    std::array<float, 17 * 17> heights{};
    heights.fill(8);
    ASSERT_TRUE(Luminumbra::World::OverlayTerrainCollision(*upper, heights));
    PhysicsSystem original;
    original.startup();
    ASSERT_TRUE(original.add_chunk_collision(*floor, JPH::BodyID(), heights));
    const auto saved = original.chunk_collision_body(floor->get_id());
    ASSERT_TRUE(saved.has_value());
    PhysicsSystem restored;
    restored.startup();
    EXPECT_FALSE(restored.add_chunk_collision(*floor, JPH::BodyID(), std::span(heights).first(1)));
    EXPECT_FALSE(restored.chunk_collision_body(floor->get_id()).has_value());
    const auto projected = heights;
    for (const float invalid : {std::numeric_limits<float>::infinity(),
                                std::numeric_limits<float>::quiet_NaN(),
                                std::numeric_limits<float>::max(),
                                std::numeric_limits<float>::lowest()}) {
        heights.back() = invalid;
        EXPECT_FALSE(restored.add_chunk_collision(*floor, JPH::BodyID(), heights));
        EXPECT_FALSE(restored.chunk_collision_body(floor->get_id()).has_value());
    }
    ASSERT_TRUE(restored.add_chunk_collision(*floor, saved.value_or(JPH::BodyID()), projected));
    EXPECT_EQ(restored.chunk_collision_body(floor->get_id()), saved);
    for (auto* current : {&original, &restored}) {
        const auto hit = current->audio_raycast({8.25f, 24, 8.25f}, {8.25f, 0, 8.25f});
        ASSERT_TRUE(hit.hit);
        EXPECT_NEAR(hit.hit_point.y, 21.5f, 0.01f);
    }
}

TEST_P(TerrainEditCollision, DigWakesSleepingSupportedBody) {
    const auto body = physics.create_dynamic_sphere({8.25f, 9, 8.25f}, {0, 0, 0}, 0.2f);
    ASSERT_FALSE(body.IsInvalid());
    for (int tick = 0; tick < 180; ++tick) {
        ASSERT_TRUE(physics.prepare_simulation_collisions({}));
        physics.update(1.0f / 60);
    }
    ASSERT_FALSE(physics.body_is_active(body));
    ASSERT_NEAR(physics.get_body_position(body).y, 8.2f, 0.1f);
    ASSERT_GT(world->EditTerrainVoxel({8, 8, 8}, 4, false, &physics), 0);
    EXPECT_TRUE(physics.body_is_active(body));
    for (int tick = 0; tick < 120; ++tick) {
        ASSERT_TRUE(physics.prepare_simulation_collisions({}));
        physics.update(1.0f / 60);
    }
    EXPECT_LT(physics.get_body_position(body).y, 5);
}

TEST_P(TerrainEditCollision, ExactBrushBoundaryUpdatesPrecedingSharedLattice) {
    auto neighbour = MakeFlatFloorChunk({1, 0, 0}, 8);
    world->GenerateChunkData(*neighbour);
    ASSERT_TRUE(world->adopt_streamed_chunk(neighbour));
    ASSERT_TRUE(physics.add_chunk_collision(*neighbour));
    ASSERT_EQ(world->EditTerrainVoxel({19, 7, 8}, 3, false, &physics), 2);
    EXPECT_EQ(floor->sdf_data[16 + 7 * 17 + 8 * 17 * 17], 0);
    EXPECT_GT(floor->voxel_revision(), 0u);
    const auto hit = physics.audio_raycast({15.75f, 20, 8}, {15.75f, 0, 8});
    ASSERT_TRUE(hit.hit);
    EXPECT_LT(hit.hit_point.y, 7.8f);
}

TEST_P(TerrainEditCollision, PreparationIncludesMissingPrecedingSharedLattice) {
    ASSERT_EQ(world->find_streamed_chunk({2, 0, 0}), nullptr);
    ASSERT_TRUE(world->PrepareTerrainVoxelEdit({51, 7, 8}, 3));
    const auto preceding = world->find_streamed_chunk({2, 0, 0});
    ASSERT_NE(preceding, nullptr);
    ASSERT_EQ(preceding->sdf_data.size(), 17u * 17u * 17u);
    ASSERT_GT(world->EditTerrainVoxel({51, 7, 8}, 3, false, &physics), 0);
    EXPECT_EQ(preceding->sdf_data[16 + 7 * 17 + 8 * 17 * 17], 0);
}

TEST_P(TerrainEditCollision, DigLowersColumnAcrossVerticalChunkBoundary) {
    auto lower = MakeFlatFloorChunk({0, -1, 0}, 8);
    world->GenerateChunkData(*lower);
    ASSERT_TRUE(world->adopt_streamed_chunk(lower));
    ASSERT_GT(world->EditTerrainVoxel({8, 8, 8}, 12, false, &physics), 0);
    const auto hit = physics.audio_raycast({8.25f, 20, 8.25f}, {8.25f, -8, 8.25f});
    ASSERT_TRUE(hit.hit);
    EXPECT_LT(hit.hit_point.y, -3);
}

TEST_P(TerrainEditCollision, ColdProjectionAfterEditIsBoundedByBrushFootprint) {
    // Pre-admit the remesh band so the diagnostic isolates editing, rather than
    // first-time procedural generation. The upper slabs are outside that band.
    ASSERT_TRUE(world->EnsureSurfaceReadyNear({8, 8, 8}, &physics, 2, 2));
    for (int slab_y = 2; slab_y <= 21; ++slab_y) {
        auto slab = MakeFlatFloorChunk({0, slab_y, 0}, 8);
        slab->sdf_data.assign(17 * 17 * 17, 1);
        for (std::size_t z = 0; z < 17; ++z)
            for (std::size_t y = 3; y <= 5; ++y)
                for (std::size_t x = 0; x < 17; ++x)
                    slab->sdf_data[x + y * 17 + z * 17 * 17] = -1;
        slab->mark_sdf_loaded_or_edited();
        ASSERT_TRUE(world->adopt_streamed_chunk(slab));
        if (GetParam()) {
            world->remember_durable_chunks({slab->get_coords()});
        }
    }
    if (GetParam()) {
        // Loader replacement retains resident authority and completes its
        // admission before the next edit, just like inventory publication.
        world->set_durable_chunk_loader({}, {});
    }
    std::array<float, 17 * 17> heights{};
    const auto make_cold = [&] {
        // Evict the column from the 128-entry sampling LRU, retaining its
        // revisioned aggregate. No slab or terrain input is changed here.
        for (int column_x = 100; column_x < 229; ++column_x)
            EXPECT_TRUE(world->sample_simulation_collision(column_x, 0, heights));
    };
    make_cold();
    auto work = world->simulation_collision_sdf_samples();
    ASSERT_GT(world->EditTerrainVoxel({8, 8, 8}, 2, false, nullptr), 0);
    ASSERT_TRUE(world->sample_simulation_collision(0, 0, heights));
    EXPECT_EQ(heights[8 * 17 + 8], 341.5f);
    // A radius-2 brush touches at most 5x5 horizontal samples. Each validates
    // 17 inputs and projects at most 32 density endpoints, independent of height.
    EXPECT_LE(world->simulation_collision_sdf_samples() - work, 5u * 5u * 49u);

    make_cold();
    work = world->simulation_collision_sdf_samples();
    ASSERT_GT(world->EditTerrainVoxel({8, 340, 8}, 3, false, nullptr), 0);
    ASSERT_TRUE(world->sample_simulation_collision(0, 0, heights));
    EXPECT_EQ(heights[8 * 17 + 8], 325.5f); // Removed top sample reveals slab 20.
    EXPECT_EQ(heights[0], 341.5f);          // Outside the footprint keeps its roof.
    EXPECT_LE(world->simulation_collision_sdf_samples() - work, 7u * 7u * 49u);
}

TEST(SurfaceQuery, WaterAdmissionStagesProjectionBeforeFirstEdit) {
    auto params = Luminumbra::Test::SyntheticTerrainParams();
    params.base_amplitude = 0;
    params.peaks_amplitude = 0;
    params.height_offset = 8;
    params.continental_spline = {{{-1, 0}}, {{1, 0}}};
    params.caves_enabled = false;
    auto world = std::make_unique<SHIELD_WorldSystem>(nullptr, nullptr, params, 4242);
    PhysicsSystem physics;
    physics.startup();
    physics.set_world_system(world.get());
    Luminumbra::Systems::WaterSystem water(nullptr, world.get());
    world->set_durable_residency(true);
    world->SetWaterSystem(&water);
    const IVec3 coords(4, 0, 0);
    const auto admitted = world->resolve_water_column(coords);
    ASSERT_NE(admitted, nullptr);
    ASSERT_TRUE(world->unpark_chunk(admitted->get_id()));
    // Meshing an already populated water admission must reuse its staged SDF
    // projection, just like streaming generation and direct adoption.
    ASSERT_TRUE(world->EnsureSurfaceReadyNear({72, 8, 8}, &physics, 2, 2));
    const auto work = world->simulation_collision_sdf_samples();
    ASSERT_GT(world->EditTerrainVoxel({72, 8, 8}, 2, false, nullptr), 0);
    std::array<float, 17 * 17> heights{};
    ASSERT_TRUE(world->sample_simulation_collision(coords.x, coords.z, heights));
    EXPECT_LT(heights[8 * 17 + 8], 8);
    EXPECT_EQ(heights[0], 8);
    EXPECT_LE(world->simulation_collision_sdf_samples() - work, 5u * 5u * 49u);
    world->SetWaterSystem(nullptr);
}

TEST(SurfaceQuery, ColumnAggregateMatchesDescendingProjectionAcrossSlabRuns) {
    auto world = MakeWorldSystem();
    world->set_durable_residency(true);
    std::array<std::shared_ptr<Chunk>, 4> slabs{};
    for (std::size_t index = 0; index < slabs.size(); ++index) {
        auto& slab = slabs[index];
        slab = MakeFlatFloorChunk({0, static_cast<int>(index), 0}, 8);
        slab->sdf_data.assign(17 * 17 * 17, 1);
        slab->mark_sdf_loaded_or_edited();
    }
    // A low roof and two empty slabs test baseline clearing through contiguous
    // runs, exact shared endpoints, gaps, and retirement of the highest surface.
    slabs[0]->sdf_data[8 + 5 * 17 + 8 * 17 * 17] = -1;
    slabs[3]->sdf_data[8 + 3 * 17 + 8 * 17 * 17] = -1;
    world->set_durable_chunk_loader(
        [&slabs](const IVec3& coords) { return slabs[static_cast<std::size_t>(coords.y)]; }, {});
    for (const auto& inventory :
         std::vector<std::vector<IVec3>>{{{0, 0, 0}, {0, 2, 0}, {0, 1, 0}, {0, 3, 0}},
                                         {{0, 0, 0}, {0, 1, 0}, {0, 3, 0}},
                                         {{0, 0, 0}, {0, 1, 0}},
                                         {}}) {
        world->replace_durable_chunks(inventory);
        for (const float baseline_height : {-8.0f, 0.0f, 8.0f, 16.0f, 32.0f, 48.0f, 64.0f, 80.0f}) {
            SCOPED_TRACE(baseline_height);
            world->remember_durable_chunks(inventory);
            std::array<float, 17 * 17> baseline{};
            baseline.fill(baseline_height);
            auto expected = baseline;
            for (auto slab = slabs.rbegin(); slab != slabs.rend(); ++slab) {
                if (std::find(inventory.begin(), inventory.end(), (*slab)->get_coords()) !=
                    inventory.end()) {
                    ASSERT_TRUE(Luminumbra::World::OverlayTerrainCollision(**slab, expected));
                }
            }
            auto actual = baseline;
            ASSERT_TRUE(world->sample_simulation_collision(0, 0, actual, baseline));
            EXPECT_EQ(actual, expected);
        }
    }
}

TEST(SurfaceQuery, ColumnAggregateRefusesMalformedReplacementAndRecovers) {
    auto world = MakeWorldSystem();
    auto upper = MakeFlatFloorChunk({0, 1, 0}, 8);
    upper->sdf_data.assign(17 * 17 * 17, 1);
    upper->sdf_data[8 + 5 * 17 + 8 * 17 * 17] = -1;
    upper->mark_sdf_loaded_or_edited();
    ASSERT_TRUE(world->adopt_streamed_chunk(upper));
    const auto valid_sdf = upper->sdf_data;
    std::array<float, 17 * 17> baseline{};
    baseline.fill(8);
    auto heights = baseline;
    ASSERT_TRUE(world->sample_simulation_collision(0, 0, heights, baseline));
    EXPECT_EQ(heights[8 * 17 + 8], 21.5f);
    for (const int input : {0, 1, 2}) {
        SCOPED_TRACE(input);
        if (input == 0)
            upper->sdf_data.resize(1);
        else
            upper->sdf_data[0] = input == 1 ? std::numeric_limits<float>::quiet_NaN()
                                            : std::numeric_limits<float>::infinity();
        world->notify_far_lod_authority_changed(upper->get_coords());
        EXPECT_FALSE(world->sample_simulation_collision(0, 0, heights, baseline));
        upper->sdf_data = valid_sdf;
        world->notify_far_lod_authority_changed(upper->get_coords());
        ASSERT_TRUE(world->sample_simulation_collision(0, 0, heights, baseline));
        EXPECT_EQ(heights[8 * 17 + 8], 21.5f);
    }
    // Empty legacy SDF retires the projection and its baseline-clearing run.
    upper->sdf_data.clear();
    world->notify_far_lod_authority_changed(upper->get_coords());
    ASSERT_TRUE(world->sample_simulation_collision(0, 0, heights, baseline));
    EXPECT_EQ(heights, baseline);
}

TEST(SurfaceQuery, DurableSlabCacheInvalidatesWhenInventoryIsRetiredOrReplaced) {
    auto params = Luminumbra::Test::SyntheticTerrainParams();
    params.base_amplitude = 0;
    params.peaks_amplitude = 0;
    params.height_offset = 8;
    params.continental_spline = {{{-1, 0}}, {{1, 0}}};
    params.caves_enabled = false;
    auto world = std::make_unique<SHIELD_WorldSystem>(nullptr, nullptr, params, 4242);
    world->set_durable_residency(true);
    auto upper = MakeFlatFloorChunk({0, 1, 0}, 8);
    upper->sdf_data.assign(17 * 17 * 17, 1);
    for (std::size_t z = 0; z < 17; ++z)
        for (std::size_t y = 3; y <= 5; ++y)
            for (std::size_t x = 0; x < 17; ++x)
                upper->sdf_data[x + y * 17 + z * 17 * 17] = -1;
    upper->mark_sdf_loaded_or_edited();
    world->set_durable_chunk_loader([upper](const IVec3&) { return upper; }, {});
    std::array<float, 17 * 17> heights{};
    EXPECT_FALSE(world->sample_simulation_collision(0, 0, heights, std::span(heights).first(1)));
    for (const bool replace_inventory : {false, true}) {
        world->remember_durable_chunks({upper->get_coords()});
        ASSERT_TRUE(world->sample_simulation_collision(0, 0, heights));
        EXPECT_EQ(heights[8 * 17 + 8], 21.5f);
        const auto work = world->simulation_collision_sdf_samples();
        ASSERT_TRUE(world->sample_simulation_collision(0, 0, heights));
        EXPECT_EQ(world->simulation_collision_sdf_samples(), work);
        if (replace_inventory)
            world->replace_durable_chunks({});
        else
            world->forget_durable_chunks({upper->get_coords()});
        ASSERT_TRUE(world->sample_simulation_collision(0, 0, heights));
        EXPECT_EQ(heights[8 * 17 + 8], 8);
    }
}

TEST(SurfaceQuery, LegacyAttachedWorldProjectsAnUnadoptedAuthoritativeChunk) {
    auto world = MakeWorldSystem();
    auto floor = MakeFlatFloorChunk({0, 0, 0}, 8);
    floor->sdf_data.assign(17 * 17 * 17, 1);
    for (std::size_t z = 0; z < 17; ++z)
        for (std::size_t x = 0; x < 17; ++x)
            floor->sdf_data[x + 3 * 17 + z * 17 * 17] = -1;
    floor->mark_sdf_loaded_or_edited();
    PhysicsSystem physics;
    physics.startup();
    physics.set_world_system(world.get());
    ASSERT_TRUE(physics.add_chunk_collision(*floor));
    const auto hit = physics.audio_raycast({8, 12, 8}, {8, 0, 8});
    ASSERT_TRUE(hit.hit);
    EXPECT_NEAR(hit.hit_point.y, 3.5f, 0.01f);
    const auto saved = physics.chunk_collision_body(floor->get_id());
    floor->sdf_data.resize(1);
    floor->mark_voxel_data_dirty();
    EXPECT_FALSE(physics.refresh_chunk_collision(*floor));
    EXPECT_EQ(physics.chunk_collision_body(floor->get_id()), saved);
    const auto retained = physics.audio_raycast({8, 12, 8}, {8, 0, 8});
    ASSERT_TRUE(retained.hit);
    EXPECT_NEAR(retained.hit_point.y, 3.5f, 0.01f);
    floor->sdf_data.assign(17 * 17 * 17, -1);
    floor->mark_voxel_data_dirty();
    ASSERT_TRUE(physics.refresh_chunk_collision(*floor));
    const auto replaced = physics.audio_raycast({8, 20, 8}, {8, 0, 8});
    ASSERT_TRUE(replaced.hit);
    EXPECT_NEAR(replaced.hit_point.y, 16, 0.01f);
    const auto replacement_id = physics.chunk_collision_body(floor->get_id());
    floor->heightmap_data.resize(1);
    EXPECT_FALSE(physics.refresh_chunk_collision(*floor));
    EXPECT_EQ(physics.chunk_collision_body(floor->get_id()), replacement_id);
    // A valid worldgen surface moving to another slab retires the old owner.
    floor->heightmap_data.assign(17 * 17, 24);
    EXPECT_TRUE(physics.refresh_chunk_collision(*floor));
    EXPECT_FALSE(physics.chunk_collision_body(floor->get_id()).has_value());
}

TEST(SurfaceQuery, DurableUpperSlabRefusesMissingUnknownAndUnreadableAuthority) {
    for (int input = 0; input < 5; ++input) {
        SCOPED_TRACE(input);
        auto world = MakeWorldSystem();
        world->set_durable_residency(true);
        auto upper = MakeFlatFloorChunk({0, 1, 0}, 8);
        upper->sdf_data.assign(17 * 17 * 17, -1);
        if (input == 3)
            upper->mark_sdf_loaded_or_edited();
        else if (input == 4)
            upper->set_water_authority();
        auto placeholder = MakeFlatFloorChunk({0, 1, 0}, 8);
        placeholder->set_water_authority();
        ASSERT_TRUE(world->adopt_streamed_chunk(placeholder));
        world->set_durable_chunk_loader(
            [input, upper](const IVec3&) -> std::shared_ptr<Chunk> {
                if (input == 0)
                    return nullptr;
                if (input == 2)
                    throw std::runtime_error("unreadable terrain slab");
                return upper;
            },
            {});
        world->remember_durable_chunks({upper->get_coords()});
        std::array<float, 17 * 17> baseline{};
        baseline.fill(8);
        auto heights = baseline;
        EXPECT_EQ(world->sample_simulation_collision(0, 0, heights, baseline), input >= 3);
        if (input >= 3) {
            EXPECT_EQ(heights[8 * 17 + 8], input == 3 ? 32.0f : 8.0f);
        }
    }
}

INSTANTIATE_TEST_SUITE_P(LegacyAndRegion, TerrainEditCollision, ::testing::Bool());
