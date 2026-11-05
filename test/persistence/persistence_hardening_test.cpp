// Adversarial determinism-hardening tests for the world save/load roundtrip and
// the LMR1 region container. These hunt the bugs the happy-path persistence
// tests miss on the E2E determinism path: serialize/deserialize SYMMETRY
// (every field, byte/bit-exact), ORDER-INDEPENDENCE of the world hash,
// IDEMPOTENCY (save->load->save byte-stable), region-format header/version/
// truncation validation, and post-load hash == pre-save hash for adversarial,
// boundary, and degenerate worlds.
//
// This file registers into frontier_gates_test and matches the include style of
// test/persistence/world_save_service_test.cpp (already building in that target).
//
// Each test asserts the documented persistence behavior and reports any
// divergence through the normal test failure path.
#include "gtest/gtest.h"

#include "../support/UniqueTempPath.h"

#include "core/ResidencyContract.h"
#include "luminumbra/core/Types.h"
#include "nlohmann/json.hpp"
#include "persistence/WorldPersistenceRoundtrip.h"
#include "persistence/WorldSaveService.h"
#include "world/WorldStreamingState.h"

#include <algorithm>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

using Luminumbra::Chunk;
using Luminumbra::ChunkID;
using Luminumbra::ChunkSdfProvenance;
using Luminumbra::ChunkState;
using Luminumbra::IVec3;
using Luminumbra::Vec2;
using Luminumbra::Vec3;
using Luminumbra::VoxelVertex;
using Luminumbra::WorldStreamingState;
namespace P = Luminumbra::Persistence;
using Luminumbra::Persistence::ComputeWorldStreamingStateHash;
using Luminumbra::Persistence::ComputeWorldStreamingStateSubHashes;
using Luminumbra::Persistence::LoadWorldStreamingStateSnapshotJson;
using Luminumbra::Persistence::SerializeWorldStreamingStateSnapshotJson;
using Luminumbra::Persistence::WorldSaveService;
using Luminumbra::Persistence::WorldStreamingStateSubHashes;

std::filesystem::path MakeTempSaveDir(const std::string& tag) {
    const std::filesystem::path dir = Luminumbra::TestSupport::CreateUniqueTempDirectory(
        "luminumbra_persistence_hardening_" + tag + "_");

    return dir;
}

struct TempSaveDir {
    explicit TempSaveDir(const std::string& tag)
        : path(MakeTempSaveDir(tag)) {}
    ~TempSaveDir() {
        std::error_code remove_error;
        std::filesystem::remove_all(path, remove_error);
    }
    std::filesystem::path path;
};

std::string ReadFileBytes(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    std::ostringstream buffer;
    buffer << input.rdbuf();
    return buffer.str();
}

void WriteFileBytes(const std::filesystem::path& path, const std::string& bytes) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

// A chunk carrying an adversarial-but-finite payload keyed by salt. Avoids
// NaN/Inf (outside the determinism contract and unrepresentable in JSON).
std::shared_ptr<Chunk> AddRichChunk(WorldStreamingState& state,
                                    const IVec3& coords,
                                    ChunkState chunk_state,
                                    Luminumbra::u32 salt) {
    auto chunk = state.get_or_create_chunk(coords);
    chunk->set_state(chunk_state);
    chunk->sdf_data = {-1.5f + static_cast<float>(salt), -0.25f, 0.0f, 0.5f, 1.25f};
    chunk->heightmap_data = {7.0f + static_cast<float>(salt), 8.5f, -3.5f};
    chunk->mesh_vertices = {{Vec3(0.0f, 1.0f, 0.0f), Vec3(0.0f, 1.0f, 0.0f), salt + 1u},
                            {Vec3(1.0f, 1.0f, 0.0f), Vec3(0.0f, 1.0f, 0.0f), salt + 2u},
                            {Vec3(0.0f, 1.0f, 1.0f), Vec3(0.0f, 1.0f, 0.0f), salt + 3u}};
    chunk->mesh_indices = {0u, 1u, 2u};
    chunk->water_mesh_vertices = {{Vec3(0.0f, 2.0f, 0.0f), Vec3(0.0f, 1.0f, 0.0f), 5u},
                                  {Vec3(1.0f, 2.0f, 0.0f), Vec3(0.0f, 1.0f, 0.0f), 6u},
                                  {Vec3(1.0f, 2.0f, 1.0f), Vec3(0.0f, 1.0f, 0.0f), 7u}};
    chunk->water_mesh_indices = {0u, 1u, 2u};
    chunk->pending_mesh_vertices = chunk->mesh_vertices;
    chunk->pending_mesh_indices = chunk->mesh_indices;
    chunk->pending_water_mesh_vertices = chunk->water_mesh_vertices;
    chunk->pending_water_mesh_indices = chunk->water_mesh_indices;
    chunk->has_collision.store(true, std::memory_order_release);
    chunk->current_lod.store(static_cast<int>(salt % 3u), std::memory_order_release);
    chunk->pending_lod.store(static_cast<int>((salt + 1u) % 3u), std::memory_order_release);
    chunk->pending_mesh_ready.store(true, std::memory_order_release);
    chunk->pending_mesh_failed.store(false, std::memory_order_release);
    chunk->mesh_version.store(5u + salt, std::memory_order_release);
    chunk->water_mesh_version.store(11u + salt, std::memory_order_release);
    chunk->water_level_data = {2.0f, 2.25f};
    chunk->water_flow_data = {Vec2(0.25f, -0.125f)};
    chunk->water_sim_terrain_height = {1.0f, 1.5f};
    // / (authoritative-state change): the authoritative fixed-point state (hashed).
    chunk->water_depth_mm = {static_cast<std::int32_t>(150 + salt), 0};
    chunk->water_bed_mm = {1000, static_cast<std::int32_t>(1500 + salt)};
    chunk->has_water_sim.store(true, std::memory_order_release);
    chunk->water_mesh_generated.store(true, std::memory_order_release);
    chunk->current_water_resolution.store(16, std::memory_order_release);
    chunk->is_water_sleeping.store(static_cast<bool>(salt & 1u), std::memory_order_release);
    chunk->max_water_delta_last_tick = 0.0625f * static_cast<float>(salt + 1u);
    chunk->ticks_below_threshold = static_cast<int>(salt);
    chunk->water_mesh_dirty_ticks = static_cast<int>(salt + 2u);
    // / (authoritative-state change): flow momentum is persisted + hashed sim truth.
    chunk->water_edge_flux = {static_cast<std::int32_t>(3 + salt), -7, 0, 12};
    return chunk;
}

void PopulateRichWorld(WorldStreamingState& state) {
    AddRichChunk(state, IVec3(0, 0, 0), ChunkState::Ready, 1u);
    AddRichChunk(state, IVec3(2, -1, 3), ChunkState::Idle, 2u);
    AddRichChunk(state, IVec3(-1, 1, -2), ChunkState::Meshing, 3u);
}

// Flattens every persisted scalar of a chunk into a single vector so two chunks
// can be compared field-by-field with EXPECT_EQ / EXPECT_FLOAT_EQ.
void FlattenChunk(const Chunk& chunk, std::vector<float>& floats, std::vector<std::int64_t>& ints) {
    floats.clear();
    ints.clear();
    const auto push_verts = [&](const std::vector<VoxelVertex>& verts) {
        ints.push_back(static_cast<std::int64_t>(verts.size()));
        for (const VoxelVertex& v : verts) {
            floats.insert(
                floats.end(),
                {v.position.x, v.position.y, v.position.z, v.normal.x, v.normal.y, v.normal.z});
            ints.push_back(static_cast<std::int64_t>(v.material_id));
        }
    };
    const auto push_floats = [&](const std::vector<float>& src) {
        ints.push_back(static_cast<std::int64_t>(src.size()));
        floats.insert(floats.end(), src.begin(), src.end());
    };
    const auto push_indices = [&](const std::vector<Luminumbra::u32>& src) {
        ints.push_back(static_cast<std::int64_t>(src.size()));
        for (Luminumbra::u32 i : src)
            ints.push_back(static_cast<std::int64_t>(i));
    };

    ints.push_back(static_cast<std::int64_t>(chunk.get_id()));
    ints.push_back(chunk.get_coords().x);
    ints.push_back(chunk.get_coords().y);
    ints.push_back(chunk.get_coords().z);
    ints.push_back(static_cast<std::int64_t>(chunk.get_state()));
    push_floats(chunk.sdf_data);
    push_floats(chunk.heightmap_data);
    push_verts(chunk.mesh_vertices);
    push_indices(chunk.mesh_indices);
    push_verts(chunk.water_mesh_vertices);
    push_indices(chunk.water_mesh_indices);
    push_verts(chunk.pending_mesh_vertices);
    push_indices(chunk.pending_mesh_indices);
    push_verts(chunk.pending_water_mesh_vertices);
    push_indices(chunk.pending_water_mesh_indices);
    ints.push_back(chunk.has_collision.load(std::memory_order_acquire));
    ints.push_back(chunk.current_lod.load(std::memory_order_acquire));
    ints.push_back(chunk.pending_lod.load(std::memory_order_acquire));
    ints.push_back(chunk.pending_mesh_ready.load(std::memory_order_acquire));
    ints.push_back(chunk.pending_mesh_failed.load(std::memory_order_acquire));
    ints.push_back(chunk.mesh_version.load(std::memory_order_acquire));
    ints.push_back(chunk.water_mesh_version.load(std::memory_order_acquire));
    push_floats(chunk.water_level_data);
    ints.push_back(static_cast<std::int64_t>(chunk.water_flow_data.size()));
    for (const Vec2& f : chunk.water_flow_data)
        floats.insert(floats.end(), {f.x, f.y});
    push_floats(chunk.water_sim_terrain_height);
    ints.push_back(chunk.has_water_sim.load(std::memory_order_acquire));
    ints.push_back(chunk.water_mesh_generated.load(std::memory_order_acquire));
    ints.push_back(chunk.current_water_resolution.load(std::memory_order_acquire));
    ints.push_back(chunk.is_water_sleeping.load(std::memory_order_acquire));
    floats.push_back(chunk.max_water_delta_last_tick);
    ints.push_back(chunk.ticks_below_threshold);
    ints.push_back(chunk.water_mesh_dirty_ticks);
    ints.push_back(static_cast<std::int64_t>(chunk.water_depth_mm.size()));
    for (std::int32_t d : chunk.water_depth_mm)
        ints.push_back(d);
    ints.push_back(static_cast<std::int64_t>(chunk.water_bed_mm.size()));
    for (std::int32_t b : chunk.water_bed_mm)
        ints.push_back(b);
    ints.push_back(static_cast<std::int64_t>(chunk.water_edge_flux.size()));
    for (std::int32_t q : chunk.water_edge_flux)
        ints.push_back(q);
}

void ExpectChunkFieldExact(const Chunk& expected, const Chunk& actual) {
    std::vector<float> ef, af;
    std::vector<std::int64_t> ei, ai;
    FlattenChunk(expected, ef, ei);
    FlattenChunk(actual, af, ai);
    ASSERT_EQ(ei.size(), ai.size());
    ASSERT_EQ(ef.size(), af.size());
    EXPECT_EQ(ei, ai) << "integer/id/state/index/size fields diverged across roundtrip";
    for (std::size_t i = 0; i < ef.size(); ++i) {
        EXPECT_FLOAT_EQ(ef[i], af[i]) << "float field index " << i << " diverged across roundtrip";
    }
}

std::shared_ptr<Chunk> CloneChunk(const Chunk& src) {
    auto copy = std::make_shared<Chunk>(src.get_coords());
    std::vector<float> f;
    std::vector<std::int64_t> i;
    (void)f;
    (void)i;
    copy->set_state(src.get_state());
    copy->sdf_data = src.sdf_data;
    copy->heightmap_data = src.heightmap_data;
    copy->mesh_vertices = src.mesh_vertices;
    copy->mesh_indices = src.mesh_indices;
    copy->water_mesh_vertices = src.water_mesh_vertices;
    copy->water_mesh_indices = src.water_mesh_indices;
    copy->pending_mesh_vertices = src.pending_mesh_vertices;
    copy->pending_mesh_indices = src.pending_mesh_indices;
    copy->pending_water_mesh_vertices = src.pending_water_mesh_vertices;
    copy->pending_water_mesh_indices = src.pending_water_mesh_indices;
    copy->has_collision.store(src.has_collision.load(), std::memory_order_release);
    copy->current_lod.store(src.current_lod.load(), std::memory_order_release);
    copy->pending_lod.store(src.pending_lod.load(), std::memory_order_release);
    copy->pending_mesh_ready.store(src.pending_mesh_ready.load(), std::memory_order_release);
    copy->pending_mesh_failed.store(src.pending_mesh_failed.load(), std::memory_order_release);
    copy->mesh_version.store(src.mesh_version.load(), std::memory_order_release);
    copy->water_mesh_version.store(src.water_mesh_version.load(), std::memory_order_release);
    copy->water_level_data = src.water_level_data;
    copy->water_flow_data = src.water_flow_data;
    copy->water_sim_terrain_height = src.water_sim_terrain_height;
    copy->water_depth_mm = src.water_depth_mm;
    copy->water_bed_mm = src.water_bed_mm;
    copy->water_edge_flux = src.water_edge_flux;
    copy->has_water_sim.store(src.has_water_sim.load(), std::memory_order_release);
    copy->water_mesh_generated.store(src.water_mesh_generated.load(), std::memory_order_release);
    copy->current_water_resolution.store(src.current_water_resolution.load(),
                                         std::memory_order_release);
    copy->is_water_sleeping.store(src.is_water_sleeping.load(), std::memory_order_release);
    copy->max_water_delta_last_tick = src.max_water_delta_last_tick;
    copy->ticks_below_threshold = src.ticks_below_threshold;
    copy->water_mesh_dirty_ticks = src.water_mesh_dirty_ticks;
    return copy;
}

// Small, complete water grid; unlike the legacy serialization fixture, this
// also satisfies the admission contract for an evolving column.
std::shared_ptr<Chunk> AddCompleteWaterChunk(WorldStreamingState& world, const IVec3& coords) {
    auto chunk = AddRichChunk(world, coords, ChunkState::Ready, 1u);
    chunk->current_water_resolution.store(4);
    chunk->water_depth_mm.assign(16, 250);
    chunk->water_bed_mm.assign(16, -1250);
    chunk->water_edge_flux.assign(32, -7);
    chunk->water_level_data.assign(16, -1.0f);
    chunk->water_flow_data.assign(16, Vec2(0.25f, -0.125f));
    chunk->water_sim_terrain_height.assign(16, -1.25f);
    return chunk;
}

} // namespace

// ---------------------------------------------------------------------------
// SERIALIZE/DESERIALIZE SYMMETRY (in-memory JSON snapshot path)
// ---------------------------------------------------------------------------

TEST(PersistenceHardening, SnapshotRoundTripIsByteStable) {
    WorldStreamingState original;
    PopulateRichWorld(original);
    const std::string json = SerializeWorldStreamingStateSnapshotJson(original);

    WorldStreamingState restored;
    std::vector<std::string> errors;
    ASSERT_TRUE(LoadWorldStreamingStateSnapshotJson(json, restored, errors)) << errors.empty();
    EXPECT_TRUE(errors.empty());

    // encode(decode(encode(x))) == encode(x): the canonical bytes are stable.
    const std::string reserialized = SerializeWorldStreamingStateSnapshotJson(restored);
    EXPECT_EQ(reserialized, json) << "snapshot is not byte-stable across a roundtrip";
}

TEST(PersistenceHardening, SnapshotRoundTripPreservesEveryFieldExactly) {
    WorldStreamingState original;
    auto src = AddRichChunk(original, IVec3(4, -2, 6), ChunkState::Ready, 9u);
    const std::shared_ptr<Chunk> expected = CloneChunk(*src);

    const std::string json = SerializeWorldStreamingStateSnapshotJson(original);
    WorldStreamingState restored;
    std::vector<std::string> errors;
    ASSERT_TRUE(LoadWorldStreamingStateSnapshotJson(json, restored, errors));

    auto loaded = restored.find_chunk(IVec3(4, -2, 6));
    ASSERT_NE(loaded, nullptr);
    ExpectChunkFieldExact(*expected, *loaded);
}

TEST(PersistenceHardening, WaterDirtinessKeepsVoxelGeometryAndOwnershipIndependent) {
    Chunk chunk({0, 0, 0});
    const auto seed_generation = chunk.water_seed_edit_generation();
    EXPECT_EQ(chunk.authority_hash_revision(), 0u);
    EXPECT_FALSE(chunk.has_simulation_authority());
    chunk.mark_water_data_dirty();
    EXPECT_TRUE(chunk.is_voxel_data_dirty());
    EXPECT_TRUE(chunk.has_water_authority());
    EXPECT_TRUE(chunk.has_simulation_authority());
    EXPECT_EQ(chunk.sdf_provenance(), ChunkSdfProvenance::GeneratedCurrentParams);
    EXPECT_EQ(chunk.voxel_revision(), 0u);
    EXPECT_EQ(chunk.water_seed_edit_generation(), seed_generation);
    EXPECT_EQ(chunk.authority_hash_revision(), 1u);
    const auto saved_stamp = chunk.authority_stamp();
    chunk.clear_voxel_data_dirty();
    EXPECT_FALSE(chunk.is_voxel_data_dirty());
    EXPECT_TRUE(chunk.has_water_authority());
    EXPECT_EQ(chunk.authority_stamp(), saved_stamp);
    chunk.mark_voxel_data_dirty();
    EXPECT_EQ(chunk.voxel_revision(), 1u);
    EXPECT_EQ(chunk.authority_hash_revision(), 2u);
    EXPECT_EQ(chunk.water_seed_edit_generation(), seed_generation + 1u);
    EXPECT_EQ(chunk.sdf_provenance(), ChunkSdfProvenance::LoadedOrEdited);
    EXPECT_TRUE(chunk.has_water_authority());
}

TEST(PersistenceHardening, RegeneratingTerrainPreservesInitializedWaterAuthority) {
    for (const bool terrain_edited : {false, true}) {
        for (const bool saved : {false, true}) {
            SCOPED_TRACE(terrain_edited);
            SCOPED_TRACE(saved);
            WorldStreamingState original;
            const auto chunk = AddCompleteWaterChunk(original, {0, 0, 0});
            if (terrain_edited) {
                chunk->mark_voxel_data_dirty();
            }
            chunk->mark_water_state_dirty();
            chunk->set_water_authority();
            if (saved) {
                chunk->clear_voxel_data_dirty();
            }
            const auto depths = chunk->water_depth_mm;
            const auto flux = chunk->water_edge_flux;
            P::RegionContentHasher before;
            before.append(*chunk);
            chunk->mark_sdf_generated_current_params();
            chunk->clear_voxel_data_dirty_after_generation();
            EXPECT_TRUE(chunk->has_water_sim.load());
            EXPECT_TRUE(chunk->has_water_authority());
            EXPECT_TRUE(chunk->has_simulation_authority());
            EXPECT_TRUE(chunk->needs_authority());
            EXPECT_FALSE(chunk->has_terrain_authority());
            EXPECT_TRUE(chunk->is_voxel_data_dirty());
            EXPECT_EQ(chunk->water_depth_mm, depths);
            EXPECT_EQ(chunk->water_edge_flux, flux);
            P::RegionContentHasher after;
            after.append(*chunk);
            EXPECT_FALSE(after.empty());
            EXPECT_EQ(after.water_value(), before.water_value());
            const auto snapshot = P::SerializeChunkAuthoritySnapshotJson(*chunk);
            const auto record = nlohmann::json::parse(snapshot)["chunks"][0];
            EXPECT_EQ(record["water_authority"], "generated");
            EXPECT_EQ(record["authority"]["terrain_edited"], false);
            EXPECT_EQ(record["authority"]["water_owned"], true);
            WorldStreamingState restored;
            std::vector<std::string> errors;
            ASSERT_TRUE(LoadWorldStreamingStateSnapshotJson(snapshot, restored, errors));
            ASSERT_TRUE(errors.empty());
            const auto loaded = restored.find_chunk(chunk->get_id());
            ASSERT_NE(loaded, nullptr);
            EXPECT_TRUE(loaded->has_water_authority());
            EXPECT_FALSE(loaded->has_terrain_authority());
            EXPECT_TRUE(loaded->same_water_state(*chunk));
            EXPECT_EQ(P::ComputeRegionContentHash(restored), P::ComputeRegionContentHash(original));
            chunk->clear_water_data();
            EXPECT_FALSE(chunk->has_water_authority());
            EXPECT_FALSE(chunk->has_simulation_authority());
            EXPECT_FALSE(chunk->needs_authority());
        }
    }
}

TEST(PersistenceHardening, AuthorityStampObservesSolverBookkeepingWithoutArrayRevision) {
    Chunk chunk({0, 0, 0});
    chunk.mark_water_data_dirty();
    const auto revision = chunk.authority_hash_revision();
    auto previous = chunk.authority_stamp();
    const auto verify_change = [&] {
        const auto current = chunk.authority_stamp();
        EXPECT_NE(current, previous);
        EXPECT_EQ(current.revision, revision);
        previous = current;
    };
    chunk.has_water_sim.store(true);
    verify_change();
    chunk.is_water_sleeping.store(true);
    verify_change();
    chunk.current_water_resolution.store(4);
    verify_change();
    chunk.ticks_below_threshold = 3;
    verify_change();
    chunk.max_water_delta_last_tick = -0.0f;
    verify_change();
    EXPECT_EQ(previous.max_delta_bits, std::bit_cast<Luminumbra::u32>(-0.0f));
    chunk.water_mesh_dirty_ticks = 7;
    chunk.water_mesh_generated.store(true);
    chunk.mesh_version.store(9u);
    chunk.pending_lod.store(2);
    EXPECT_EQ(chunk.authority_stamp(), previous);
}

TEST(PersistenceHardening, CompleteWaterStateBoundsResolutionArraysAndFiniteMirrors) {
    WorldStreamingState world;
    const auto chunk = AddCompleteWaterChunk(world, {0, 0, 0});
    ASSERT_TRUE(chunk->has_complete_water_state());
    for (const int resolution : {4, 8, 16, 32}) {
        const auto cells =
            static_cast<std::size_t>(resolution) * static_cast<std::size_t>(resolution);
        chunk->current_water_resolution.store(resolution);
        chunk->water_depth_mm.assign(cells, std::numeric_limits<std::int32_t>::max());
        chunk->water_bed_mm.assign(cells, std::numeric_limits<std::int32_t>::min());
        chunk->water_edge_flux.assign(2u * cells, std::numeric_limits<std::int32_t>::min());
        chunk->water_level_data.assign(cells, 1.0f);
        chunk->water_flow_data.assign(cells, Vec2(0.0f));
        chunk->water_sim_terrain_height.assign(cells, -1.0f);
        EXPECT_TRUE(chunk->has_complete_water_state());
    }
    const auto reject_change = [&](const auto& change) {
        const auto changed = CloneChunk(*chunk);
        ASSERT_TRUE(changed->has_complete_water_state());
        change(*changed);
        EXPECT_FALSE(changed->has_complete_water_state());
    };
    for (const int invalid :
         {std::numeric_limits<int>::min(), -1, 0, 3, 64, std::numeric_limits<int>::max()}) {
        reject_change(
            [invalid](Chunk& changed) { changed.current_water_resolution.store(invalid); });
    }
    reject_change([](Chunk& changed) { changed.has_water_sim.store(false); });
    reject_change([](Chunk& changed) { changed.water_depth_mm.pop_back(); });
    reject_change([](Chunk& changed) { changed.water_depth_mm[0] = -1; });
    reject_change([](Chunk& changed) { changed.water_bed_mm.pop_back(); });
    reject_change([](Chunk& changed) { changed.water_edge_flux.pop_back(); });
    reject_change([](Chunk& changed) { changed.water_level_data.clear(); });
    reject_change([](Chunk& changed) { changed.water_flow_data.clear(); });
    reject_change([](Chunk& changed) { changed.water_sim_terrain_height.clear(); });
    reject_change([](Chunk& changed) { changed.water_rest_level = {1.0f}; });
    reject_change([](Chunk& changed) { changed.ticks_below_threshold = -1; });
    for (const float invalid :
         {std::numeric_limits<float>::infinity(), std::numeric_limits<float>::quiet_NaN()}) {
        reject_change([invalid](Chunk& changed) { changed.max_water_delta_last_tick = invalid; });
        reject_change([invalid](Chunk& changed) { changed.water_level_data[0] = invalid; });
        reject_change([invalid](Chunk& changed) { changed.water_sim_terrain_height[0] = invalid; });
        reject_change([invalid](Chunk& changed) { changed.water_flow_data[0].x = invalid; });
        reject_change([invalid](Chunk& changed) { changed.water_flow_data[0].y = invalid; });
        reject_change(
            [invalid](Chunk& changed) { changed.water_rest_level.assign(1024, invalid); });
    }
}

TEST(PersistenceHardening, WaterStateComparisonIncludesPersistedMirrorsAndSolverBookkeeping) {
    WorldStreamingState world;
    const auto chunk = AddCompleteWaterChunk(world, {0, 0, 0});
    const auto reject_change = [&](const auto& change) {
        const auto changed = CloneChunk(*chunk);
        ASSERT_TRUE(chunk->same_water_state(*changed));
        change(*changed);
        EXPECT_FALSE(chunk->same_water_state(*changed));
        EXPECT_FALSE(changed->same_water_state(*chunk));
    };
    reject_change([](Chunk& changed) { ++changed.water_depth_mm[0]; });
    reject_change([](Chunk& changed) { ++changed.water_bed_mm[0]; });
    reject_change([](Chunk& changed) { ++changed.water_edge_flux[0]; });
    reject_change([](Chunk& changed) { changed.water_level_data[0] += 0.25f; });
    reject_change([](Chunk& changed) { changed.water_flow_data[0].x += 0.25f; });
    reject_change([](Chunk& changed) { changed.water_sim_terrain_height[0] += 0.25f; });
    reject_change([](Chunk& changed) { changed.current_water_resolution.store(8); });
    reject_change([](Chunk& changed) { changed.has_water_sim.store(false); });
    reject_change([](Chunk& changed) { changed.is_water_sleeping.store(false); });
    reject_change([](Chunk& changed) { ++changed.ticks_below_threshold; });
    reject_change([](Chunk& changed) { changed.max_water_delta_last_tick += 0.25f; });
    chunk->max_water_delta_last_tick = 0.0f;
    reject_change([](Chunk& changed) { changed.max_water_delta_last_tick = -0.0f; });
    const auto derived = CloneChunk(*chunk);
    derived->water_src_mm.assign(16, 1);
    derived->water_rest_level.assign(16, 5.0f);
    derived->water_mesh_dirty_ticks = 99;
    derived->water_mesh_vertices.clear();
    derived->water_mesh_generated.store(false);
    EXPECT_TRUE(chunk->same_water_state(*derived));
}

TEST(PersistenceHardening, AuthoritySnapshotsKeepTerrainWaterAndRegenerableOwnership) {
    for (const bool terrain_edited : {false, true}) {
        for (const bool water_owned : {false, true}) {
            SCOPED_TRACE(terrain_edited);
            SCOPED_TRACE(water_owned);
            WorldStreamingState original;
            const auto chunk = AddCompleteWaterChunk(original, {0, 0, 0});
            if (terrain_edited) {
                chunk->mark_voxel_data_dirty();
            }
            if (water_owned) {
                chunk->mark_water_data_dirty();
            }
            for (const auto& snapshot :
                 {P::SerializeChunkAuthoritySnapshotJson(*chunk),
                  SerializeWorldStreamingStateSnapshotJson(original, true)}) {
                WorldStreamingState loaded;
                std::vector<std::string> errors;
                // Explicit metadata overrides both legacy migration defaults.
                ASSERT_TRUE(LoadWorldStreamingStateSnapshotJson(
                    snapshot,
                    loaded,
                    errors,
                    {.terrain_edited = true, .migrate_legacy_water = true}));
                EXPECT_TRUE(errors.empty());
                const auto restored = loaded.find_chunk(chunk->get_id());
                ASSERT_NE(restored, nullptr);
                EXPECT_EQ(restored->has_terrain_authority(), terrain_edited);
                EXPECT_EQ(restored->sdf_provenance() == ChunkSdfProvenance::LoadedOrEdited,
                          terrain_edited || water_owned);
                EXPECT_EQ(restored->has_water_authority(), water_owned);
                EXPECT_EQ(restored->has_simulation_authority(), terrain_edited || water_owned);
                EXPECT_FALSE(restored->is_voxel_data_dirty());
                EXPECT_TRUE(restored->has_complete_water_state());
                EXPECT_TRUE(restored->same_water_state(*chunk));
                EXPECT_EQ(restored->sdf_data, chunk->sdf_data);
                EXPECT_EQ(P::ComputeRegionContentHash(loaded),
                          P::ComputeRegionContentHash(original));
                EXPECT_EQ(SerializeWorldStreamingStateSnapshotJson(loaded, true), snapshot);
            }
        }
    }
}

TEST(PersistenceHardening, AuthorityMetadataIsOptInAndKeepsLegacyBytesAndCursorHashes) {
    WorldStreamingState original;
    const auto chunk = AddCompleteWaterChunk(original, {0, 0, 0});
    chunk->mark_voxel_data_dirty();
    chunk->mark_water_data_dirty();
    const auto legacy = SerializeWorldStreamingStateSnapshotJson(original);
    const auto legacy_world = ComputeWorldStreamingStateHash(original);
    const auto legacy_sub = ComputeWorldStreamingStateSubHashes(original);
    constexpr std::uint64_t cursor = (std::uint64_t{1} << 63) + 7u;
    const auto scheduled_world = ComputeWorldStreamingStateHash(original, cursor);
    const auto scheduled_sub = ComputeWorldStreamingStateSubHashes(original, cursor);
    EXPECT_EQ(SerializeWorldStreamingStateSnapshotJson(original), legacy);
    EXPECT_EQ(SerializeWorldStreamingStateSnapshotJson(original, false), legacy);
    EXPECT_EQ(ComputeWorldStreamingStateHash(original), legacy_world);
    EXPECT_EQ(ComputeWorldStreamingStateHash(original, 0u), legacy_world);
    EXPECT_EQ(ComputeWorldStreamingStateSubHashes(original).water, legacy_sub.water);
    EXPECT_EQ(ComputeWorldStreamingStateHash(original, cursor), scheduled_world);
    EXPECT_EQ(ComputeWorldStreamingStateSubHashes(original, cursor).water, scheduled_sub.water);
    EXPECT_NE(scheduled_world, legacy_world);
    EXPECT_NE(scheduled_sub.water, legacy_sub.water);
    const auto explicit_json = SerializeWorldStreamingStateSnapshotJson(original, true);
    auto metadata = nlohmann::json::parse(explicit_json);
    EXPECT_EQ(metadata["chunks"][0]["water_authority"], "edited");
    metadata["chunks"][0].erase("water_authority");
    metadata["chunks"][0].erase("authority");
    EXPECT_EQ(metadata.dump(4) + '\n', legacy);
    WorldStreamingState loaded;
    std::vector<std::string> errors;
    ASSERT_TRUE(LoadWorldStreamingStateSnapshotJson(legacy, loaded, errors));
    const auto restored = loaded.find_chunk(chunk->get_id());
    ASSERT_NE(restored, nullptr);
    EXPECT_TRUE(restored->has_simulation_authority());
    EXPECT_EQ(SerializeWorldStreamingStateSnapshotJson(loaded), legacy);
}

TEST(PersistenceHardening, LegacyAuthorityPolicyMigratesWaterOnlyWhenRequested) {
    WorldStreamingState original;
    const auto chunk = AddCompleteWaterChunk(original, {0, 0, 0});
    const auto legacy = SerializeWorldStreamingStateSnapshotJson(original);
    for (const bool terrain_edited : {false, true}) {
        for (const bool migrate_water : {false, true}) {
            WorldStreamingState loaded;
            std::vector<std::string> errors;
            ASSERT_TRUE(LoadWorldStreamingStateSnapshotJson(
                legacy,
                loaded,
                errors,
                {.terrain_edited = terrain_edited, .migrate_legacy_water = migrate_water}));
            const auto restored = loaded.find_chunk(chunk->get_id());
            ASSERT_NE(restored, nullptr);
            EXPECT_EQ(restored->has_terrain_authority(), terrain_edited);
            EXPECT_EQ(restored->has_water_authority(), migrate_water);
            EXPECT_FALSE(restored->is_voxel_data_dirty());
            EXPECT_TRUE(restored->same_water_state(*chunk));
            auto expected = nlohmann::json::parse(legacy);
            if (migrate_water) {
                expected["chunks"][0]["water_state"]["terrain_authoritative"] = terrain_edited;
            }
            EXPECT_EQ(SerializeWorldStreamingStateSnapshotJson(loaded), expected.dump(4) + '\n');
        }
    }
    chunk->has_water_sim.store(false);
    WorldStreamingState uninitialized;
    std::vector<std::string> errors;
    ASSERT_TRUE(
        LoadWorldStreamingStateSnapshotJson(SerializeWorldStreamingStateSnapshotJson(original),
                                            uninitialized,
                                            errors,
                                            {.migrate_legacy_water = true}));
    const auto restored = uninitialized.find_chunk(chunk->get_id());
    ASSERT_NE(restored, nullptr);
    EXPECT_FALSE(restored->has_water_authority());
}

TEST(PersistenceHardening, AuthorityLoadPolicyValidatesContainerOwnershipBeforePublishing) {
    for (const bool terrain_edited : {false, true}) {
        for (const bool water_owned : {false, true}) {
            WorldStreamingState original;
            const auto chunk = AddCompleteWaterChunk(original, {0, 0, 0});
            if (terrain_edited) {
                chunk->mark_voxel_data_dirty();
            }
            if (water_owned) {
                chunk->mark_water_data_dirty();
            }
            const auto snapshot = P::SerializeChunkAuthoritySnapshotJson(*chunk);
            for (const bool header_edited : {false, true}) {
                WorldStreamingState loaded;
                loaded.get_or_create_chunk({1, 0, 0});
                std::vector<std::string> errors;
                const bool expected = header_edited == (terrain_edited || water_owned);
                EXPECT_EQ(LoadWorldStreamingStateSnapshotJson(
                              snapshot,
                              loaded,
                              errors,
                              {.terrain_edited = header_edited, .validate_record_authority = true}),
                          expected);
                EXPECT_EQ(loaded.size(), expected ? 1u : 0u);
                EXPECT_EQ(errors.empty(), expected);
            }
        }
    }
}

TEST(PersistenceHardening, MalformedAuthorityMetadataRefusesTheWholeSnapshot) {
    WorldStreamingState original;
    const auto chunk = AddCompleteWaterChunk(original, {0, 0, 0});
    chunk->mark_voxel_data_dirty();
    chunk->mark_water_data_dirty();
    const auto later = AddCompleteWaterChunk(original, {1, 0, 0});
    later->mark_voxel_data_dirty();
    later->mark_water_data_dirty();
    const auto valid =
        nlohmann::json::parse(SerializeWorldStreamingStateSnapshotJson(original, true));
    const auto reject_record = [&](const nlohmann::json& record) {
        auto document = valid;
        document["chunks"][1] = record;
        WorldStreamingState loaded;
        loaded.get_or_create_chunk({2, 0, 0});
        std::vector<std::string> errors;
        EXPECT_FALSE(LoadWorldStreamingStateSnapshotJson(document.dump(), loaded, errors));
        EXPECT_TRUE(loaded.empty());
        EXPECT_FALSE(errors.empty());
        errors.clear();
        EXPECT_FALSE(P::ValidateWorldStreamingChunkFormatJson(record.dump(), errors));
        EXPECT_FALSE(errors.empty());
    };
    const auto owned = valid["chunks"][1];
    for (const auto& invalid :
         {nlohmann::json(nullptr),
          nlohmann::json("edited"),
          nlohmann::json{{"terrain_edited", true}},
          nlohmann::json{{"terrain_edited", 1}, {"water_owned", true}},
          nlohmann::json{{"terrain_edited", true}, {"water_owned", "false"}},
          nlohmann::json{{"terrain_edited", true}, {"water_owned", true}, {"extra", 0}}}) {
        auto changed = owned;
        changed["authority"] = invalid;
        reject_record(changed);
    }
    for (const auto& invalid : {nlohmann::json("unknown"),
                                nlohmann::json(nullptr),
                                nlohmann::json(1),
                                nlohmann::json::object()}) {
        auto changed = owned;
        changed["water_authority"] = invalid;
        reject_record(changed);
    }
    auto inconsistent = owned;
    inconsistent["water_authority"] = "generated";
    reject_record(inconsistent);
    inconsistent = owned;
    inconsistent["authority"]["water_owned"] = false;
    reject_record(inconsistent);
    inconsistent = owned;
    inconsistent["water_state"]["terrain_authoritative"] = "true";
    reject_record(inconsistent);
    inconsistent["water_state"]["terrain_authoritative"] = false;
    reject_record(inconsistent);
}

TEST(PersistenceHardening, WaterAuthorityMetadataRejectsMalformedValuesForEitherTerrainState) {
    for (const bool terrain_edited : {false, true}) {
        SCOPED_TRACE(terrain_edited);
        WorldStreamingState original;
        const auto chunk = AddCompleteWaterChunk(original, {0, 0, 0});
        if (terrain_edited) {
            chunk->mark_voxel_data_dirty();
        }
        chunk->mark_water_data_dirty();
        const auto plain = nlohmann::json::parse(P::SerializeChunkAuthoritySnapshotJson(*chunk));
        auto compact = nlohmann::json::parse(P::SerializeChunkRecordPayloadJson(*chunk));
        compact["chunks"][0]["authority"] = plain["chunks"][0]["authority"];
        compact["chunks"][0]["water_authority"] = plain["chunks"][0]["water_authority"];
        for (const auto& invalid : {nlohmann::json(nullptr),
                                    nlohmann::json(false),
                                    nlohmann::json(true),
                                    nlohmann::json(1),
                                    nlohmann::json(1.5),
                                    nlohmann::json::object(),
                                    nlohmann::json::array(),
                                    nlohmann::json(""),
                                    nlohmann::json("unknown")}) {
            SCOPED_TRACE(invalid.dump());
            for (auto document : {plain, compact}) {
                document["chunks"][0]["water_authority"] = invalid;
                WorldStreamingState restored;
                restored.get_or_create_chunk({1, 0, 0});
                std::vector<std::string> errors;
                EXPECT_FALSE(
                    LoadWorldStreamingStateSnapshotJson(document.dump(), restored, errors));
                EXPECT_TRUE(restored.empty());
                EXPECT_FALSE(errors.empty());
                if (!document.contains("record_format")) {
                    errors.clear();
                    EXPECT_FALSE(P::ValidateWorldStreamingChunkFormatJson(
                        document["chunks"][0].dump(), errors));
                    EXPECT_FALSE(errors.empty());
                }
            }
        }
    }
}

TEST(PersistenceHardening, WaterRetirementAndOmittedSnapshotPreserveTerrainAuthority) {
    for (const bool terrain_edited : {false, true}) {
        WorldStreamingState original;
        const auto chunk = AddCompleteWaterChunk(original, {0, 1, 0});
        if (terrain_edited) {
            chunk->mark_voxel_data_dirty();
        }
        chunk->mark_water_data_dirty();
        chunk->water_src_mm.assign(16, 1);
        chunk->water_rest_level.assign(16, 2.0f);
        const auto before = P::SerializeChunkAuthoritySnapshotJson(*chunk);
        const auto stamp = chunk->authority_stamp();
        const auto voxel_version = chunk->voxel_revision();
        const auto seed_generation = chunk->water_seed_edit_generation();
        const auto omitted = P::SerializeChunkAuthoritySnapshotJson(*chunk, true);
        EXPECT_EQ(P::SerializeChunkAuthoritySnapshotJson(*chunk), before);
        EXPECT_EQ(chunk->authority_stamp(), stamp);
        chunk->clear_voxel_data_dirty();
        chunk->clear_water_data();
        EXPECT_EQ(P::SerializeChunkAuthoritySnapshotJson(*chunk), omitted);
        EXPECT_TRUE(chunk->is_voxel_data_dirty());
        EXPECT_FALSE(chunk->has_water_authority());
        EXPECT_EQ(chunk->has_simulation_authority(), terrain_edited);
        EXPECT_EQ(chunk->sdf_provenance() == ChunkSdfProvenance::LoadedOrEdited, terrain_edited);
        EXPECT_EQ(chunk->voxel_revision(), voxel_version);
        EXPECT_EQ(chunk->water_seed_edit_generation(), seed_generation);
        EXPECT_EQ(chunk->authority_hash_revision(), stamp.revision + 1u);
        EXPECT_TRUE(chunk->water_src_mm.empty());
        EXPECT_TRUE(chunk->water_rest_level.empty());
        EXPECT_FALSE(chunk->has_complete_water_state());
        WorldStreamingState loaded;
        std::vector<std::string> errors;
        ASSERT_TRUE(LoadWorldStreamingStateSnapshotJson(omitted, loaded, errors));
        const auto restored = loaded.find_chunk(chunk->get_id());
        ASSERT_NE(restored, nullptr);
        EXPECT_TRUE(restored->same_water_state(*chunk));
        EXPECT_EQ(restored->sdf_data, chunk->sdf_data);
        EXPECT_EQ(restored->heightmap_data, chunk->heightmap_data);
        EXPECT_EQ(restored->has_simulation_authority(), terrain_edited);
    }
}

TEST(PersistenceHardening, AuthoritySnapshotOmitsWorkerArraysAndPendingResetKeepsLiveState) {
    WorldStreamingState original;
    const auto chunk = AddCompleteWaterChunk(original, {0, 0, 0});
    chunk->mark_water_data_dirty();
    chunk->pending_mesh_failed.store(true);
    chunk->pending_promotion_ready.store(true);
    chunk->pending_promotion_failed.store(true);
    chunk->pending_generation_ready.store(true);
    chunk->pending_sdf_data = {1.0f};
    chunk->pending_heightmap_data = {2.0f};
    chunk->pending_material_data = {3};
    const auto stamp = chunk->authority_stamp();
    auto full = nlohmann::json::parse(SerializeWorldStreamingStateSnapshotJson(original, true));
    const auto eviction = nlohmann::json::parse(P::SerializeChunkAuthoritySnapshotJson(*chunk));
    for (const char* field : {"pending_mesh_vertices",
                              "pending_mesh_indices",
                              "pending_water_mesh_vertices",
                              "pending_water_mesh_indices"}) {
        EXPECT_FALSE(full["chunks"][0][field].empty());
        EXPECT_TRUE(eviction["chunks"][0][field].empty());
        full["chunks"][0][field] = nlohmann::json::array();
    }
    EXPECT_EQ(eviction, full);
    WorldStreamingState restored;
    std::vector<std::string> errors;
    ASSERT_TRUE(LoadWorldStreamingStateSnapshotJson(eviction.dump(), restored, errors));
    const auto loaded = restored.find_chunk(chunk->get_id());
    ASSERT_NE(loaded, nullptr);
    EXPECT_TRUE(loaded->has_water_authority());
    EXPECT_TRUE(loaded->same_water_state(*chunk));
    // Decoding itself keeps the legacy staging flags. Reset is an explicit
    // adoption operation after the caller has drained outstanding workers.
    EXPECT_EQ(loaded->pending_lod.load(), chunk->pending_lod.load());
    EXPECT_TRUE(loaded->pending_mesh_ready.load());
    EXPECT_TRUE(loaded->pending_mesh_failed.load());
    chunk->reset_pending_jobs();
    EXPECT_EQ(chunk->pending_lod.load(), -1);
    EXPECT_FALSE(chunk->pending_mesh_ready.load());
    EXPECT_FALSE(chunk->pending_mesh_failed.load());
    EXPECT_FALSE(chunk->pending_promotion_ready.load());
    EXPECT_FALSE(chunk->pending_promotion_failed.load());
    EXPECT_FALSE(chunk->pending_generation_ready.load());
    EXPECT_TRUE(chunk->pending_sdf_data.empty());
    EXPECT_TRUE(chunk->pending_heightmap_data.empty());
    EXPECT_TRUE(chunk->pending_material_data.empty());
    EXPECT_EQ(chunk->authority_stamp(), stamp);
    full["chunks"][0]["pending_lod"] = -1;
    full["chunks"][0]["pending_mesh_ready"] = false;
    full["chunks"][0]["pending_mesh_failed"] = false;
    EXPECT_EQ(nlohmann::json::parse(SerializeWorldStreamingStateSnapshotJson(original, true)),
              full);
}

TEST(PersistenceHardening, AuthoritySnapshotKeepsWideWaterValuesAndLegacyFluxDefaults) {
    WorldStreamingState original;
    const auto chunk = AddCompleteWaterChunk(original, {0, 0, 0});
    chunk->mark_water_data_dirty();
    constexpr auto maximum = std::numeric_limits<std::int32_t>::max();
    constexpr auto minimum = std::numeric_limits<std::int32_t>::min();
    chunk->water_depth_mm[0] = maximum;
    chunk->water_bed_mm[0] = maximum;
    chunk->water_depth_mm[1] = 0;
    chunk->water_bed_mm[1] = minimum;
    chunk->water_edge_flux[0] = minimum;
    chunk->water_edge_flux[1] = maximum;
    const auto snapshot = P::SerializeChunkAuthoritySnapshotJson(*chunk);
    WorldStreamingState restored;
    std::vector<std::string> errors;
    ASSERT_TRUE(LoadWorldStreamingStateSnapshotJson(snapshot, restored, errors));
    const auto loaded = restored.find_chunk(chunk->get_id());
    ASSERT_NE(loaded, nullptr);
    EXPECT_TRUE(loaded->has_complete_water_state());
    EXPECT_TRUE(loaded->same_water_state(*chunk));
    EXPECT_EQ(static_cast<std::int64_t>(loaded->water_depth_mm[0]) + loaded->water_bed_mm[0],
              4294967294LL);
    EXPECT_EQ(static_cast<std::int64_t>(loaded->water_depth_mm[1]) + loaded->water_bed_mm[1],
              -2147483648LL);
    EXPECT_EQ(P::SerializeChunkAuthoritySnapshotJson(*loaded), snapshot);
    auto legacy = nlohmann::json::parse(snapshot);
    legacy["chunks"][0].erase("water_edge_flux");
    WorldStreamingState without_flux;
    ASSERT_TRUE(LoadWorldStreamingStateSnapshotJson(legacy.dump(), without_flux, errors));
    const auto migrated = without_flux.find_chunk(chunk->get_id());
    ASSERT_NE(migrated, nullptr);
    EXPECT_EQ(migrated->water_depth_mm, chunk->water_depth_mm);
    EXPECT_EQ(migrated->water_bed_mm, chunk->water_bed_mm);
    EXPECT_EQ(migrated->water_edge_flux, std::vector<std::int32_t>(32, 0));
    EXPECT_TRUE(migrated->has_water_authority());
    EXPECT_TRUE(migrated->has_complete_water_state());
}

TEST(PersistenceHardening, RegionContentHasherMatchesCanonicalProjectionAndSeparatesWater) {
    WorldStreamingState original;
    const auto first = AddCompleteWaterChunk(original, {0, 0, 0});
    const auto second = AddCompleteWaterChunk(original, {1, 0, 0});
    first->mark_voxel_data_dirty();
    second->mark_water_data_dirty();
    P::RegionContentHasher hasher;
    EXPECT_TRUE(hasher.empty());
    auto contents = nlohmann::json::array();
    const auto verify = [&] {
        const auto canonical =
            nlohmann::json{{"hash_scope", "region_content_v2"}, {"chunks", contents}};
        std::ostringstream digest;
        digest << std::hex << std::setw(16) << std::setfill('0') << hasher.value();
        EXPECT_EQ(digest.str(), P::StableChecksum(canonical.dump(4) + '\n'));
        EXPECT_EQ(hasher.value(), hasher.value());
    };
    verify();
    for (const auto& chunk : {first, second}) {
        const auto snapshot = nlohmann::json::parse(P::SerializeChunkAuthoritySnapshotJson(*chunk));
        nlohmann::json projected;
        for (const char* field : {"chunk_id",
                                  "coords",
                                  "sdf_data",
                                  "heightmap_data",
                                  "material_data",
                                  "water_depth_mm",
                                  "water_bed_mm",
                                  "water_edge_flux",
                                  "has_water_sim",
                                  "current_water_resolution",
                                  "is_water_sleeping",
                                  "max_water_delta_last_tick",
                                  "ticks_below_threshold"}) {
            projected[field] = snapshot["chunks"][0].at(field);
        }
        if (!chunk->has_terrain_authority()) {
            for (const char* field : {"sdf_data", "heightmap_data", "material_data"}) {
                projected[field] = nlohmann::json::array();
            }
        }
        contents.push_back(std::move(projected));
        hasher.append(*chunk);
        verify();
    }
    EXPECT_FALSE(hasher.empty());
    std::ostringstream digest;
    digest << std::hex << std::setw(16) << std::setfill('0') << hasher.value();
    EXPECT_EQ(P::ComputeRegionContentHash(original), digest.str());
    const auto base = hasher.value();
    Chunk regenerable({2, 0, 0});
    hasher.append(regenerable);
    EXPECT_EQ(hasher.value(), base);
    EXPECT_THROW(hasher.append(*second), std::invalid_argument);
    EXPECT_THROW(hasher.append(*first), std::invalid_argument);
    EXPECT_EQ(hasher.value(), base);
    ++second->water_depth_mm[0];
    P::RegionContentHasher changed_water;
    changed_water.append(*first);
    changed_water.append(*second);
    EXPECT_EQ(changed_water.terrain_value(), hasher.terrain_value());
    EXPECT_NE(changed_water.water_value(), hasher.water_value());
    EXPECT_NE(changed_water.value(), base);
    first->sdf_data[0] += 0.5f;
    P::RegionContentHasher changed_terrain;
    changed_terrain.append(*first);
    changed_terrain.append(*second);
    EXPECT_NE(changed_terrain.terrain_value(), changed_water.terrain_value());
    EXPECT_EQ(changed_terrain.water_value(), changed_water.water_value());
}

TEST(PersistenceHardening, AdversarialFloatPayloadSurvivesRoundTripBitExact) {
    // Boundary / extreme finite floats: denormal, smallest/largest normal,
    // negative zero, fractional values that have no exact decimal form.
    WorldStreamingState original;
    auto chunk = original.get_or_create_chunk(IVec3(0, 0, 0));
    chunk->set_state(ChunkState::Ready);
    chunk->sdf_data = {0.0f,
                       -0.0f,
                       std::numeric_limits<float>::min(),        // smallest positive normal
                       std::numeric_limits<float>::denorm_min(), // smallest subnormal
                       std::numeric_limits<float>::max(),        // largest finite
                       -std::numeric_limits<float>::max(),
                       0.1f,
                       0.2f,
                       0.3f,
                       1.0f / 3.0f,
                       2.0f / 3.0f,
                       123456.789f,
                       -987654.321f};
    chunk->heightmap_data = {std::numeric_limits<float>::lowest(),
                             std::numeric_limits<float>::epsilon()};

    const std::string json = SerializeWorldStreamingStateSnapshotJson(original);
    WorldStreamingState restored;
    std::vector<std::string> errors;
    ASSERT_TRUE(LoadWorldStreamingStateSnapshotJson(json, restored, errors)) << errors.empty();

    auto loaded = restored.find_chunk(IVec3(0, 0, 0));
    ASSERT_NE(loaded, nullptr);
    ASSERT_EQ(loaded->sdf_data.size(), chunk->sdf_data.size());
    // Bit-exact, not just FLOAT_EQ: a float written then read must be identical.
    for (std::size_t i = 0; i < chunk->sdf_data.size(); ++i) {
        EXPECT_EQ(loaded->sdf_data[i], chunk->sdf_data[i])
            << "sdf float index " << i << " lost precision in the snapshot text";
    }
    ASSERT_EQ(loaded->heightmap_data.size(), chunk->heightmap_data.size());
    for (std::size_t i = 0; i < chunk->heightmap_data.size(); ++i) {
        EXPECT_EQ(loaded->heightmap_data[i], chunk->heightmap_data[i]);
    }
}

TEST(PersistenceHardening, NegativeZeroSdfPreservedAcrossRoundTrip) {
    // -0.0f and +0.0f are distinct bit patterns; a save that collapses them
    // would still EXPECT_EQ as numbers but break a CRC-equal sub-hash gate.
    WorldStreamingState original;
    auto chunk = original.get_or_create_chunk(IVec3(0, 0, 0));
    chunk->set_state(ChunkState::Ready);
    chunk->sdf_data = {-0.0f};

    const std::string json = SerializeWorldStreamingStateSnapshotJson(original);
    WorldStreamingState restored;
    std::vector<std::string> errors;
    ASSERT_TRUE(LoadWorldStreamingStateSnapshotJson(json, restored, errors));
    auto loaded = restored.find_chunk(IVec3(0, 0, 0));
    ASSERT_NE(loaded, nullptr);
    ASSERT_EQ(loaded->sdf_data.size(), 1u);
    // signbit must survive so the sub-hash is stable.
    EXPECT_EQ(std::signbit(loaded->sdf_data[0]), std::signbit(-0.0f))
        << "sign of zero dropped across roundtrip";
}

TEST(PersistenceHardening, MaxU32IndicesAndMaterialIdsRoundTrip) {
    WorldStreamingState original;
    auto chunk = original.get_or_create_chunk(IVec3(0, 0, 0));
    chunk->set_state(ChunkState::Ready);
    const Luminumbra::u32 umax = std::numeric_limits<Luminumbra::u32>::max();
    chunk->mesh_indices = {0u, 1u, umax, umax - 1u};
    chunk->mesh_vertices = {{Vec3(0, 0, 0), Vec3(0, 1, 0), umax}};

    const std::string json = SerializeWorldStreamingStateSnapshotJson(original);
    WorldStreamingState restored;
    std::vector<std::string> errors;
    ASSERT_TRUE(LoadWorldStreamingStateSnapshotJson(json, restored, errors));
    auto loaded = restored.find_chunk(IVec3(0, 0, 0));
    ASSERT_NE(loaded, nullptr);
    EXPECT_EQ(loaded->mesh_indices, chunk->mesh_indices);
    ASSERT_EQ(loaded->mesh_vertices.size(), 1u);
    EXPECT_EQ(loaded->mesh_vertices[0].material_id, umax) << "u32 material_id width truncated";
}

TEST(PersistenceHardening, NegativeAndExtremeCoordsRoundTripWithMatchingId) {
    WorldStreamingState original;
    const std::vector<IVec3> coords = {IVec3(-1, -1, -1),
                                       IVec3(-100000, 50000, -75000),
                                       IVec3(0, 0, 0),
                                       IVec3(2147483647, -2147483647, 1073741823)};
    Luminumbra::u32 salt = 0u;
    for (const IVec3& c : coords)
        AddRichChunk(original, c, ChunkState::Ready, salt++);

    const std::string json = SerializeWorldStreamingStateSnapshotJson(original);
    WorldStreamingState restored;
    std::vector<std::string> errors;
    ASSERT_TRUE(LoadWorldStreamingStateSnapshotJson(json, restored, errors)) << errors.empty();
    EXPECT_TRUE(errors.empty());
    EXPECT_EQ(restored.size(), coords.size());
    for (const IVec3& c : coords) {
        auto loaded = restored.find_chunk(c);
        ASSERT_NE(loaded, nullptr) << "coord " << c.x << "," << c.y << "," << c.z << " lost";
        EXPECT_EQ(loaded->get_id(), Chunk::calculate_id(c)) << "chunk id mismatch after roundtrip";
    }
}

// ---------------------------------------------------------------------------
// ORDER & ITERATION: hash must be id-ordered, not insertion-ordered
// ---------------------------------------------------------------------------

TEST(PersistenceHardening, HashIndependentOfInsertionOrder) {
    const std::vector<IVec3> coords = {
        IVec3(5, 0, 5), IVec3(-3, 2, 1), IVec3(0, 0, 0), IVec3(9, -4, 9), IVec3(1, 1, 1)};

    WorldStreamingState forward;
    for (std::size_t i = 0; i < coords.size(); ++i)
        AddRichChunk(forward, coords[i], ChunkState::Ready, static_cast<Luminumbra::u32>(i + 1));

    WorldStreamingState reverse;
    for (std::size_t i = coords.size(); i-- > 0;)
        AddRichChunk(reverse, coords[i], ChunkState::Ready, static_cast<Luminumbra::u32>(i + 1));

    EXPECT_EQ(SerializeWorldStreamingStateSnapshotJson(forward),
              SerializeWorldStreamingStateSnapshotJson(reverse))
        << "snapshot bytes depend on chunk insertion order";
    EXPECT_EQ(ComputeWorldStreamingStateHash(forward), ComputeWorldStreamingStateHash(reverse))
        << "world hash depends on chunk insertion order";
}

TEST(PersistenceHardening, SubHashesIndependentOfInsertionOrder) {
    const std::vector<IVec3> coords = {IVec3(7, 1, 2), IVec3(-2, 0, -8), IVec3(3, 3, 3)};

    WorldStreamingState a;
    for (std::size_t i = 0; i < coords.size(); ++i)
        AddRichChunk(a, coords[i], ChunkState::Ready, static_cast<Luminumbra::u32>(i + 1));
    WorldStreamingState b;
    for (std::size_t i = coords.size(); i-- > 0;)
        AddRichChunk(b, coords[i], ChunkState::Ready, static_cast<Luminumbra::u32>(i + 1));

    const WorldStreamingStateSubHashes sa = ComputeWorldStreamingStateSubHashes(a);
    const WorldStreamingStateSubHashes sb = ComputeWorldStreamingStateSubHashes(b);
    EXPECT_EQ(sa.terrain, sb.terrain);
    EXPECT_EQ(sa.mesh, sb.mesh);
    EXPECT_EQ(sa.water, sb.water);
}

//  (folded into the  authoritative-state change): the water sub-hash covers the
// AUTHORITATIVE fixed-point state — flipping one water_depth_mm bit (or one flux
// value) must move the `water` section and ONLY the `water` section. Before this,
// the group hashed only the float mirrors, so a fixed-point desync was invisible
// to sub-hash localization.
TEST(WaterSubHash, CoversFixedPointState) {
    WorldStreamingState base;
    AddRichChunk(base, IVec3(0, 0, 0), ChunkState::Ready, 1u);
    const WorldStreamingStateSubHashes s0 = ComputeWorldStreamingStateSubHashes(base);

    {
        WorldStreamingState w;
        auto c = AddRichChunk(w, IVec3(0, 0, 0), ChunkState::Ready, 1u);
        c->water_depth_mm[0] += 1;
        const WorldStreamingStateSubHashes s = ComputeWorldStreamingStateSubHashes(w);
        EXPECT_NE(s.water, s0.water) << "water_depth_mm must move the water sub-hash";
        EXPECT_EQ(s.terrain, s0.terrain);
        EXPECT_EQ(s.mesh, s0.mesh);
    }
    {
        WorldStreamingState w;
        auto c = AddRichChunk(w, IVec3(0, 0, 0), ChunkState::Ready, 1u);
        c->water_bed_mm[0] += 1;
        const WorldStreamingStateSubHashes s = ComputeWorldStreamingStateSubHashes(w);
        EXPECT_NE(s.water, s0.water) << "water_bed_mm must move the water sub-hash";
        EXPECT_EQ(s.terrain, s0.terrain);
    }
    {
        WorldStreamingState w;
        auto c = AddRichChunk(w, IVec3(0, 0, 0), ChunkState::Ready, 1u);
        c->water_edge_flux[0] += 3;
        const WorldStreamingStateSubHashes s = ComputeWorldStreamingStateSubHashes(w);
        EXPECT_NE(s.water, s0.water) << "water_edge_flux must move the water sub-hash";
        EXPECT_EQ(s.terrain, s0.terrain);
    }
    // meshing bookkeeping must NOT move the water section (it lives in
    // the mesh section now — the loaded-boot remesh flips it while water is paused).
    {
        WorldStreamingState w;
        auto c = AddRichChunk(w, IVec3(0, 0, 0), ChunkState::Ready, 1u);
        c->water_mesh_generated.store(!c->water_mesh_generated.load(), std::memory_order_release);
        c->water_mesh_dirty_ticks += 9;
        const WorldStreamingStateSubHashes s = ComputeWorldStreamingStateSubHashes(w);
        EXPECT_EQ(s.water, s0.water)
            << "water-mesh bookkeeping must NOT move the water sub-hash ()";
        EXPECT_NE(s.mesh, s0.mesh) << "it localizes under mesh instead";
    }
    // water residency (, derived-state reclassification): the float mirrors are one-way DERIVED
    // from mm — mutating them must NOT move the water section (or any section).
    {
        WorldStreamingState w;
        auto c = AddRichChunk(w, IVec3(0, 0, 0), ChunkState::Ready, 1u);
        c->water_level_data[0] += 0.5f;
        c->water_flow_data[0].y -= 0.25f;
        c->water_sim_terrain_height[0] += 0.125f;
        const WorldStreamingStateSubHashes s = ComputeWorldStreamingStateSubHashes(w);
        EXPECT_EQ(s.water, s0.water) << "the float mirrors must NOT move the water sub-hash "
                                        "(derived-state reclassification)";
        EXPECT_EQ(s.mesh, s0.mesh);
        EXPECT_EQ(s.terrain, s0.terrain);
    }
}

//  (folded into the  authoritative-state change): the save/load flow-momentum
// contract — water_edge_flux round-trips EXACTLY (it was transient/cleared-on-load
// before, which made the heavy oracle's resim leg diverge: the loaded session
// restarted from zero momentum while the original carried its flux).
TEST(WaterPersistenceSettleParity, EdgeFluxRoundTripsExactly) {
    TempSaveDir save_dir("flux_roundtrip");
    WorldSaveService service;

    WorldStreamingState original;
    auto src = AddRichChunk(original, IVec3(2, 0, -3), ChunkState::Ready, 5u);
    src->water_edge_flux = {101, -202, 303, -404};
    const std::vector<std::int32_t> expected_flux = src->water_edge_flux;

    std::vector<std::string> errors;
    ASSERT_TRUE(service.save_world(original, save_dir.path, &errors));
    WorldStreamingState restored;
    std::vector<std::string> load_errors;
    ASSERT_TRUE(service.load_world(restored, save_dir.path, load_errors));
    auto loaded = restored.find_chunk(IVec3(2, 0, -3));
    ASSERT_NE(loaded, nullptr);
    EXPECT_EQ(loaded->water_edge_flux, expected_flux)
        << "flow momentum must survive save/load bit-exactly ()";
}

// ---------------------------------------------------------------------------
// SAVE -> LOAD roundtrip over the LMR1 region container (the E2E path)
// ---------------------------------------------------------------------------

TEST(PersistenceHardening, RegionRoundTripPreservesEveryFieldExactly) {
    TempSaveDir save_dir("region_fields");
    WorldSaveService service;

    WorldStreamingState original;
    auto src = AddRichChunk(original, IVec3(1, 2, 3), ChunkState::Ready, 4u);
    const std::shared_ptr<Chunk> expected = CloneChunk(*src);

    std::vector<std::string> save_errors;
    ASSERT_TRUE(service.save_world(original, save_dir.path, &save_errors));
    EXPECT_TRUE(save_errors.empty());

    WorldStreamingState restored;
    std::vector<std::string> load_errors;
    ASSERT_TRUE(service.load_world(restored, save_dir.path, load_errors));
    EXPECT_TRUE(load_errors.empty());

    auto loaded = restored.find_chunk(IVec3(1, 2, 3));
    ASSERT_NE(loaded, nullptr);
    ExpectChunkFieldExact(*expected, *loaded);
}

TEST(PersistenceHardening, PostLoadHashEqualsPreSaveHash) {
    TempSaveDir save_dir("hash_eq");
    WorldSaveService service;

    WorldStreamingState original;
    PopulateRichWorld(original);
    const std::string pre = service.world_hash(original);
    ASSERT_FALSE(pre.empty());

    std::vector<std::string> errors;
    ASSERT_TRUE(service.save_world(original, save_dir.path, &errors));
    WorldStreamingState restored;
    std::vector<std::string> load_errors;
    ASSERT_TRUE(service.load_world(restored, save_dir.path, load_errors));

    EXPECT_EQ(service.world_hash(restored), pre) << "world hash not preserved across save/load";
    // The sub-hashes (desync localization) must also survive a roundtrip.
    const WorldStreamingStateSubHashes before = ComputeWorldStreamingStateSubHashes(original);
    const WorldStreamingStateSubHashes after = ComputeWorldStreamingStateSubHashes(restored);
    EXPECT_EQ(after.terrain, before.terrain);
    EXPECT_EQ(after.mesh, before.mesh);
    EXPECT_EQ(after.water, before.water);
}

TEST(PersistenceHardening, SaveLoadSaveIsByteStable) {
    TempSaveDir dir1("stable_a");
    TempSaveDir dir2("stable_b");
    WorldSaveService service;

    WorldStreamingState original;
    PopulateRichWorld(original);

    std::vector<std::string> errors;
    ASSERT_TRUE(service.save_world(original, dir1.path, &errors));
    const std::string region_first =
        ReadFileBytes(WorldSaveService::region_file_path(dir1.path, 0, 0));
    ASSERT_FALSE(region_first.empty());

    WorldStreamingState restored;
    std::vector<std::string> load_errors;
    ASSERT_TRUE(service.load_world(restored, dir1.path, load_errors));

    // Re-saving the loaded world into a fresh dir must reproduce identical bytes.
    ASSERT_TRUE(service.save_world(restored, dir2.path, &errors));
    const std::string region_second =
        ReadFileBytes(WorldSaveService::region_file_path(dir2.path, 0, 0));
    EXPECT_EQ(region_second, region_first) << "save->load->save is not byte-stable";
}

TEST(PersistenceHardening, ResaveOfUnchangedWorldIsByteIdentical) {
    TempSaveDir dir1("idem_a");
    TempSaveDir dir2("idem_b");
    WorldSaveService service;

    WorldStreamingState world;
    PopulateRichWorld(world);

    std::vector<std::string> errors;
    ASSERT_TRUE(service.save_world(world, dir1.path, &errors));
    ASSERT_TRUE(service.save_world(world, dir2.path, &errors));

    for (int rx = -1; rx <= 0; ++rx) {
        const auto p1 = WorldSaveService::region_file_path(dir1.path, rx, 0);
        const auto p2 = WorldSaveService::region_file_path(dir2.path, rx, 0);
        if (std::filesystem::exists(p1) || std::filesystem::exists(p2)) {
            EXPECT_EQ(ReadFileBytes(p1), ReadFileBytes(p2))
                << "two full saves of the same world differ (region " << rx << ",0)";
        }
    }
}

// ---------------------------------------------------------------------------
// BOUNDARY / DEGENERATE worlds
// ---------------------------------------------------------------------------

TEST(PersistenceHardening, EmptyWorldRoundTripHashStable) {
    TempSaveDir save_dir("empty");
    WorldSaveService service;

    WorldStreamingState empty;
    const std::string empty_hash = service.world_hash(empty);

    // An empty world is a clean miss on save_world's dirty machinery, but the
    // in-memory hash must be stable and the load of nothing must be a clean
    // miss (no spurious error).
    WorldStreamingState reloaded;
    std::vector<std::string> errors;
    EXPECT_FALSE(service.load_world(reloaded, save_dir.path, errors));
    EXPECT_TRUE(errors.empty());
    EXPECT_TRUE(reloaded.empty());
    EXPECT_EQ(service.world_hash(reloaded), empty_hash);
}

TEST(PersistenceHardening, SingleChunkRoundTrip) {
    TempSaveDir save_dir("single");
    WorldSaveService service;

    WorldStreamingState world;
    AddRichChunk(world, IVec3(0, 0, 0), ChunkState::Ready, 1u);
    const std::string pre = service.world_hash(world);

    std::vector<std::string> errors;
    ASSERT_TRUE(service.save_world(world, save_dir.path, &errors));
    WorldStreamingState restored;
    std::vector<std::string> load_errors;
    ASSERT_TRUE(service.load_world(restored, save_dir.path, load_errors));
    EXPECT_EQ(restored.size(), 1u);
    EXPECT_EQ(service.world_hash(restored), pre);
}

TEST(PersistenceHardening, ChunkWithAllEmptyArraysRoundTrips) {
    TempSaveDir save_dir("empty_arrays");
    WorldSaveService service;

    WorldStreamingState world;
    auto chunk = world.get_or_create_chunk(IVec3(3, 0, 3));
    chunk->set_state(ChunkState::Idle);
    // Every vector field left empty: a degenerate but legal chunk.
    chunk->mark_voxel_data_dirty();
    const std::string pre = service.world_hash(world);

    std::vector<std::string> errors;
    ASSERT_TRUE(service.save_world(world, save_dir.path, &errors));
    EXPECT_TRUE(errors.empty());
    WorldStreamingState restored;
    std::vector<std::string> load_errors;
    ASSERT_TRUE(service.load_world(restored, save_dir.path, load_errors));
    auto loaded = restored.find_chunk(IVec3(3, 0, 3));
    ASSERT_NE(loaded, nullptr);
    EXPECT_TRUE(loaded->sdf_data.empty());
    EXPECT_TRUE(loaded->mesh_vertices.empty());
    EXPECT_TRUE(loaded->water_flow_data.empty());
    EXPECT_EQ(service.world_hash(restored), pre);
}

TEST(PersistenceHardening, ChunkAtRegionSeamRoundTrips) {
    // Chunks straddling the region floor-division seam (x=31 -> region 0,
    // x=32 -> region 1, x=-1 -> region -1) must all survive.
    TempSaveDir save_dir("seam");
    WorldSaveService service;

    WorldStreamingState world;
    AddRichChunk(world, IVec3(31, 0, 31), ChunkState::Ready, 1u);
    AddRichChunk(world, IVec3(32, 0, 32), ChunkState::Ready, 2u);
    AddRichChunk(world, IVec3(-1, 0, -1), ChunkState::Ready, 3u);
    AddRichChunk(world, IVec3(0, 0, 0), ChunkState::Ready, 4u);
    const std::string pre = service.world_hash(world);

    std::vector<std::string> errors;
    ASSERT_TRUE(service.save_world(world, save_dir.path, &errors));
    WorldStreamingState restored;
    std::vector<std::string> load_errors;
    ASSERT_TRUE(service.load_world(restored, save_dir.path, load_errors));
    EXPECT_EQ(restored.size(), 4u);
    EXPECT_EQ(service.world_hash(restored), pre);
}

// ---------------------------------------------------------------------------
// IDEMPOTENCY / CONVERGENCE on the incremental path
// ---------------------------------------------------------------------------

TEST(PersistenceHardening, EmptyWorldSaveDirtyIsNoOp) {
    TempSaveDir save_dir("noop");
    WorldSaveService service;

    WorldStreamingState empty;
    std::vector<std::string> errors;
    const auto report = service.save_dirty_chunks(empty, save_dir.path, &errors);
    EXPECT_TRUE(errors.empty());
    EXPECT_EQ(report.chunks_total, 0u);
    EXPECT_EQ(report.chunks_dirty, 0u);
    EXPECT_FALSE(report.saved);
    EXPECT_FALSE(std::filesystem::exists(WorldSaveService::region_directory(save_dir.path)));
}

TEST(PersistenceHardening, RepeatedDirtySaveOfSameEditConverges) {
    TempSaveDir save_dir("converge");
    WorldSaveService service;

    WorldStreamingState world;
    PopulateRichWorld(world);
    std::vector<std::string> errors;
    ASSERT_TRUE(service.save_world(world, save_dir.path, &errors));

    // Apply an edit and save it twice; the second pass (no dirty chunks) must
    // be a no-op and the on-disk region byte-identical to the first.
    auto edited = world.find_chunk(IVec3(0, 0, 0));
    ASSERT_NE(edited, nullptr);
    edited->sdf_data[0] = -42.5f;
    edited->mark_voxel_data_dirty();

    const auto first = service.save_dirty_chunks(world, save_dir.path, &errors);
    EXPECT_TRUE(first.saved);
    const std::string region_after_first =
        ReadFileBytes(WorldSaveService::region_file_path(save_dir.path, 0, 0));

    const auto second = service.save_dirty_chunks(world, save_dir.path, &errors);
    EXPECT_FALSE(second.saved) << "re-saving with nothing dirty must be a no-op";
    EXPECT_EQ(ReadFileBytes(WorldSaveService::region_file_path(save_dir.path, 0, 0)),
              region_after_first)
        << "no-op dirty save mutated the region file";

    // Re-marking and re-saving the SAME data must reproduce identical bytes.
    edited->mark_voxel_data_dirty();
    const auto third = service.save_dirty_chunks(world, save_dir.path, &errors);
    EXPECT_TRUE(third.saved);
    EXPECT_EQ(ReadFileBytes(WorldSaveService::region_file_path(save_dir.path, 0, 0)),
              region_after_first)
        << "re-saving identical chunk data produced different bytes";
}

TEST(PersistenceHardening, FullSaveThenDirtySaveOfSameWorldAgree) {
    TempSaveDir dir_full("agree_full");
    TempSaveDir dir_inc("agree_inc");
    WorldSaveService service;

    WorldStreamingState world_a;
    PopulateRichWorld(world_a);
    std::vector<std::string> errors;
    ASSERT_TRUE(service.save_world(world_a, dir_full.path, &errors));

    // A world built identically, marked fully dirty, then incremental-saved
    // from scratch.
    WorldStreamingState world_b;
    PopulateRichWorld(world_b);
    for (auto& chunk : world_b.snapshot_chunks())
        chunk->mark_voxel_data_dirty();
    const auto report = service.save_dirty_chunks(world_b, dir_inc.path, &errors);
    EXPECT_TRUE(report.saved);

    // The two region files are NOT byte-identical, and that is CORRECT: the LMR1
    // per-record `flags` byte carries the sticky kRecordFlagEdited bit (0x01). The
    // full save of a freshly-generated world records NO edits (flags = water bit
    // only), whereas the incremental path was explicitly told these chunks were
    // edited (mark_voxel_data_dirty), so it records kRecordFlagEdited. That is
    // edit-PROVENANCE metadata, not world STATE -- the chunk payloads are identical.
    // The determinism contract is therefore on the reloaded WORLD STATE, not on the
    // raw header bytes: both saves must reload to a hash-identical world.
    WorldStreamingState loaded_full;
    WorldStreamingState loaded_inc;
    std::vector<std::string> load_errors;
    ASSERT_TRUE(service.load_world(loaded_full, dir_full.path, load_errors)) << "full load failed";
    ASSERT_TRUE(service.load_world(loaded_inc, dir_inc.path, load_errors))
        << "incremental load failed";
    EXPECT_TRUE(load_errors.empty());
    EXPECT_EQ(ComputeWorldStreamingStateHash(loaded_full),
              ComputeWorldStreamingStateHash(loaded_inc))
        << "incremental and full save of the same world reload to different state";
}

// ---------------------------------------------------------------------------
// REGION FORMAT validation: header / version / truncation must be rejected
// deterministically on load (never a silent partial world).
// ---------------------------------------------------------------------------

TEST(PersistenceHardening, BadMagicRejectedWithError) {
    TempSaveDir save_dir("bad_magic");
    WorldSaveService service;
    std::string bytes = "XXXX"; // wrong magic
    bytes.append(8, '\0');
    WriteFileBytes(WorldSaveService::region_file_path(save_dir.path, 0, 0), bytes);

    WorldStreamingState state;
    std::vector<std::string> errors;
    EXPECT_FALSE(service.load_world(state, save_dir.path, errors));
    EXPECT_FALSE(errors.empty()) << "wrong magic must surface a load error";
}

TEST(PersistenceHardening, UnsupportedVersionRejectedWithError) {
    TempSaveDir save_dir("bad_version");
    WorldSaveService service;
    // Build a syntactically plausible header with magic LMR1 but version 0xFFFF
    // and zero records.
    std::string bytes = "LMR1";
    bytes.push_back('\xFF');
    bytes.push_back('\xFF'); // version u16 LE = 65535
    bytes.push_back('\0');
    bytes.push_back('\0'); // record_count = 0
    WriteFileBytes(WorldSaveService::region_file_path(save_dir.path, 0, 0), bytes);

    WorldStreamingState state;
    std::vector<std::string> errors;
    EXPECT_FALSE(service.load_world(state, save_dir.path, errors));
    EXPECT_FALSE(errors.empty()) << "unknown container version must be rejected";
}

TEST(PersistenceHardening, TruncatedRecordManifestRejected) {
    TempSaveDir save_dir("trunc_manifest");
    WorldSaveService service;
    // Claims 5 records but provides no record manifest bytes.
    std::string bytes = "LMR1";
    bytes.push_back('\x02');
    bytes.push_back('\0'); // version 2
    bytes.push_back('\x05');
    bytes.push_back('\0'); // record_count = 5, but no manifest follows
    WriteFileBytes(WorldSaveService::region_file_path(save_dir.path, 0, 0), bytes);

    WorldStreamingState state;
    std::vector<std::string> errors;
    EXPECT_FALSE(service.load_world(state, save_dir.path, errors));
    EXPECT_FALSE(errors.empty()) << "truncated record manifest must be rejected";
}

TEST(PersistenceHardening, TruncatedPayloadRejected) {
    // Save a real region, then chop the trailing payload bytes off and confirm
    // the loader rejects the partial file rather than loading a partial world.
    TempSaveDir save_dir("trunc_payload");
    WorldSaveService service;
    WorldStreamingState world;
    AddRichChunk(world, IVec3(0, 0, 0), ChunkState::Ready, 1u);
    AddRichChunk(world, IVec3(1, 0, 1), ChunkState::Ready, 2u);
    std::vector<std::string> errors;
    ASSERT_TRUE(service.save_world(world, save_dir.path, &errors));

    const auto region = WorldSaveService::region_file_path(save_dir.path, 0, 0);
    std::string bytes = ReadFileBytes(region);
    ASSERT_GT(bytes.size(), 16u);
    bytes.resize(bytes.size() - 8); // drop the tail of the last compressed payload
    WriteFileBytes(region, bytes);

    WorldStreamingState restored;
    std::vector<std::string> load_errors;
    EXPECT_FALSE(service.load_world(restored, save_dir.path, load_errors))
        << "a truncated region payload must not load as a partial world";
    EXPECT_FALSE(load_errors.empty());
}

TEST(PersistenceHardening, EmptyRegionFileRejected) {
    TempSaveDir save_dir("empty_region");
    WorldSaveService service;
    WriteFileBytes(WorldSaveService::region_file_path(save_dir.path, 0, 0), std::string());

    WorldStreamingState state;
    std::vector<std::string> errors;
    EXPECT_FALSE(service.load_world(state, save_dir.path, errors));
    EXPECT_FALSE(errors.empty()) << "a zero-byte region file must be rejected";
}

TEST(PersistenceHardening, HeaderShorterThanEightBytesRejected) {
    TempSaveDir save_dir("short_header");
    WorldSaveService service;
    WriteFileBytes(WorldSaveService::region_file_path(save_dir.path, 0, 0), std::string("LMR"));

    WorldStreamingState state;
    std::vector<std::string> errors;
    EXPECT_FALSE(service.load_world(state, save_dir.path, errors));
    EXPECT_FALSE(errors.empty()) << "a sub-header region file must be rejected, not crash";
}

// ---------------------------------------------------------------------------
// MUTATION DETECTION: any persisted field change must move the world hash
// (proves nothing important is silently dropped from the canonical bytes).
// ---------------------------------------------------------------------------

TEST(PersistenceHardening, SimHashMatchesCanonicalSnapshotProjection) {
    WorldStreamingState world;
    PopulateRichWorld(world);
    const std::string snapshot = SerializeWorldStreamingStateSnapshotJson(world);
    auto projected = nlohmann::json::parse(snapshot);
    for (auto& chunk : projected.at("chunks")) {
        ASSERT_FALSE(chunk.at("mesh_vertices").empty());
        ASSERT_FALSE(chunk.at("water_flow_data").empty());
        for (const auto& entry : luminumbra::core::kChunkFieldResidency) {
            if (!luminumbra::core::MayFeedWorldHash(entry.residency)) {
                chunk.erase(entry.field);
                chunk.at("water_state").erase(entry.field);
            }
        }
    }
    projected["hash_scope"] = "sim_truth_no_render_mesh_v1";
    // Independent oracle: serialize the complete persisted representation first,
    // then apply the declared projection and checksum its canonical bytes.
    const std::string bytes = projected.dump(4) + "\n";
    std::uint64_t hash = 14695981039346656037ull;
    for (const char ch : bytes) {
        const auto byte = static_cast<unsigned char>(ch);
        hash ^= byte;
        hash *= 1099511628211ull;
    }
    std::ostringstream expected;
    expected << std::hex << std::setw(16) << std::setfill('0') << hash;
    EXPECT_EQ(ComputeWorldStreamingStateHash(world), expected.str());
    EXPECT_EQ(SerializeWorldStreamingStateSnapshotJson(world), snapshot);
}

TEST(PersistenceHardening, EachPersistedFieldMutationMovesHash) {
    WorldSaveService service;
    const auto base_hash = [&]() {
        WorldStreamingState w;
        AddRichChunk(w, IVec3(0, 0, 0), ChunkState::Ready, 1u);
        return service.world_hash(w);
    }();

    // sdf
    {
        WorldStreamingState w;
        auto c = AddRichChunk(w, IVec3(0, 0, 0), ChunkState::Ready, 1u);
        c->sdf_data[0] += 1.0f;
        EXPECT_NE(service.world_hash(w), base_hash) << "sdf change not reflected in hash";
    }
    // state
    {
        WorldStreamingState w;
        auto c = AddRichChunk(w, IVec3(0, 0, 0), ChunkState::Ready, 1u);
        c->set_state(ChunkState::Idle);
        EXPECT_NE(service.world_hash(w), base_hash) << "state change not reflected in hash";
    }
    // RENDER MESH IS EXCLUDED FROM THE DETERMINISM HASH (intentional): the marching-cubes
    // geometry is a non-deterministic visual derivative of the deterministic SDF, and
    // collision is built from the heightmap, not this mesh (WorldPersistenceRoundtrip
    // kRenderMeshHashExcludedFields). So mutating a render-mesh field must NOT move the
    // world_hash — that is what keeps the run==replay oracle from flapping on render order.
    {
        WorldStreamingState w;
        auto c = AddRichChunk(w, IVec3(0, 0, 0), ChunkState::Ready, 1u);
        c->mesh_indices[0] = 99u;
        EXPECT_EQ(service.world_hash(w), base_hash)
            << "render mesh_indices must NOT be in the determinism hash";
    }
    {
        WorldStreamingState w;
        auto c = AddRichChunk(w, IVec3(0, 0, 0), ChunkState::Ready, 1u);
        c->mesh_version.store(c->mesh_version.load() + 1u, std::memory_order_release);
        EXPECT_EQ(service.world_hash(w), base_hash)
            << "render mesh_version must NOT be in the determinism hash";
    }
    // water residency (, derived-state reclassification): the float mirrors are one-way DERIVED
    // render state (every writer regenerates them FROM the mm truth) — mutating them must NOT move
    // the determinism hash. The mm arrays below are the hashed sim truth.
    {
        WorldStreamingState w;
        auto c = AddRichChunk(w, IVec3(0, 0, 0), ChunkState::Ready, 1u);
        c->water_flow_data[0].x += 0.5f;
        EXPECT_EQ(service.world_hash(w), base_hash)
            << "water_flow_data (float mirror) must NOT be in the determinism hash (derived-state "
               "reclassification)";
    }
    {
        WorldStreamingState w;
        auto c = AddRichChunk(w, IVec3(0, 0, 0), ChunkState::Ready, 1u);
        c->water_level_data[0] += 0.25f;
        EXPECT_EQ(service.world_hash(w), base_hash)
            << "water_level_data (float mirror) must NOT be in the determinism hash (derived-state "
               "reclassification)";
    }
    // water residency (derived-state reclassification): the mm DEPTH is the sim truth — it must
    // move the hash.
    {
        WorldStreamingState w;
        auto c = AddRichChunk(w, IVec3(0, 0, 0), ChunkState::Ready, 1u);
        c->water_depth_mm[0] += 25;
        EXPECT_NE(service.world_hash(w), base_hash)
            << "water_depth_mm change not reflected in hash (the fixed-point truth)";
    }
    // / (authoritative-state change): flow momentum is SIM TRUTH — must move the hash.
    {
        WorldStreamingState w;
        auto c = AddRichChunk(w, IVec3(0, 0, 0), ChunkState::Ready, 1u);
        c->water_edge_flux[0] += 5;
        EXPECT_NE(service.world_hash(w), base_hash)
            << "water_edge_flux change not reflected in hash ( momentum)";
    }
    //  (authoritative-state change): water-mesh bookkeeping is MESHING state, not sim truth — the
    // render-side mesh pipeline flips it (e.g. the loaded-boot remesh, while the water
    // sim is paused), so hashing it made the save/load water round-trip impossible.
    // Mutating it must NOT move the determinism hash.
    {
        WorldStreamingState w;
        auto c = AddRichChunk(w, IVec3(0, 0, 0), ChunkState::Ready, 1u);
        c->water_mesh_generated.store(!c->water_mesh_generated.load(), std::memory_order_release);
        EXPECT_EQ(service.world_hash(w), base_hash)
            << "water_mesh_generated must NOT be in the determinism hash ()";
    }
    {
        WorldStreamingState w;
        auto c = AddRichChunk(w, IVec3(0, 0, 0), ChunkState::Ready, 1u);
        c->water_mesh_dirty_ticks += 7;
        EXPECT_EQ(service.world_hash(w), base_hash)
            << "water_mesh_dirty_ticks must NOT be in the determinism hash ()";
    }
}

TEST(PersistenceHardening, MeshVersionAndLodSurviveRegionRoundTrip) {
    // mesh_version / current_lod / pending_lod are persisted as integers; an
    // off-by-width read would corrupt the post-load hash silently.
    TempSaveDir save_dir("intfields");
    WorldSaveService service;

    WorldStreamingState world;
    auto c = AddRichChunk(world, IVec3(0, 0, 0), ChunkState::Ready, 1u);
    c->mesh_version.store(4000000000u, std::memory_order_release); // > INT32_MAX, valid u32
    c->water_mesh_version.store(4111222333u, std::memory_order_release);
    c->current_lod.store(-1, std::memory_order_release); // sentinel
    c->pending_lod.store(2, std::memory_order_release);
    const Luminumbra::u32 expect_mesh = c->mesh_version.load();
    const Luminumbra::u32 expect_water = c->water_mesh_version.load();

    std::vector<std::string> errors;
    ASSERT_TRUE(service.save_world(world, save_dir.path, &errors));
    WorldStreamingState restored;
    std::vector<std::string> load_errors;
    ASSERT_TRUE(service.load_world(restored, save_dir.path, load_errors));
    auto loaded = restored.find_chunk(IVec3(0, 0, 0));
    ASSERT_NE(loaded, nullptr);
    EXPECT_EQ(loaded->mesh_version.load(), expect_mesh) << "high u32 mesh_version truncated";
    EXPECT_EQ(loaded->water_mesh_version.load(), expect_water);
    EXPECT_EQ(loaded->current_lod.load(), -1) << "negative LOD sentinel not preserved";
    EXPECT_EQ(loaded->pending_lod.load(), 2);
}

TEST(PersistenceHardening, AllChunkStatesRoundTripExactly) {
    TempSaveDir save_dir("states");
    WorldSaveService service;

    WorldStreamingState world;
    const std::vector<ChunkState> states = {ChunkState::Unloaded,
                                            ChunkState::Loading,
                                            ChunkState::Idle,
                                            ChunkState::Meshing,
                                            ChunkState::Ready,
                                            ChunkState::Unloading};
    for (std::size_t i = 0; i < states.size(); ++i) {
        auto c = AddRichChunk(world,
                              IVec3(static_cast<int>(i), 0, 0),
                              states[i],
                              static_cast<Luminumbra::u32>(i + 1));
        c->mark_voxel_data_dirty();
    }
    std::vector<std::string> errors;
    ASSERT_TRUE(service.save_world(world, save_dir.path, &errors));
    WorldStreamingState restored;
    std::vector<std::string> load_errors;
    ASSERT_TRUE(service.load_world(restored, save_dir.path, load_errors));
    for (std::size_t i = 0; i < states.size(); ++i) {
        auto loaded = restored.find_chunk(IVec3(static_cast<int>(i), 0, 0));
        ASSERT_NE(loaded, nullptr);
        EXPECT_EQ(loaded->get_state(), states[i])
            << "ChunkState " << static_cast<int>(states[i]) << " not preserved across roundtrip";
    }
}

// ---------------------------------------------------------------------------
// DESPAWN / INCREMENTAL CONSISTENCY mid-stream
// ---------------------------------------------------------------------------

TEST(PersistenceHardening, DirtySaveAfterChunkRemovalConverges) {
    // A chunk despawning from memory between full save and incremental save:
    // its on-disk record must survive (the merge keys on id) and the reload
    // must contain BOTH the surviving and the edited chunk, hash-consistent.
    TempSaveDir save_dir("despawn");
    WorldSaveService service;

    WorldStreamingState world;
    AddRichChunk(world, IVec3(0, 0, 0), ChunkState::Ready, 1u);
    AddRichChunk(world, IVec3(5, 1, 7), ChunkState::Ready, 2u);
    std::vector<std::string> errors;
    ASSERT_TRUE(service.save_world(world, save_dir.path, &errors));

    // Remove (5,1,7) from memory, edit (0,0,0), incremental-save.
    ASSERT_TRUE(world.erase_chunk(IVec3(5, 1, 7)));
    auto edited = world.find_chunk(IVec3(0, 0, 0));
    ASSERT_NE(edited, nullptr);
    edited->sdf_data[1] = 13.5f;
    edited->mark_voxel_data_dirty();
    const auto report = service.save_dirty_chunks(world, save_dir.path, &errors);
    EXPECT_TRUE(report.saved);

    WorldStreamingState restored;
    std::vector<std::string> load_errors;
    ASSERT_TRUE(service.load_world(restored, save_dir.path, load_errors));
    EXPECT_EQ(restored.size(), 2u) << "despawned-from-memory chunk dropped from disk";
    auto survivor = restored.find_chunk(IVec3(5, 1, 7));
    ASSERT_NE(survivor, nullptr);
    auto reloaded_edit = restored.find_chunk(IVec3(0, 0, 0));
    ASSERT_NE(reloaded_edit, nullptr);
    ASSERT_GT(reloaded_edit->sdf_data.size(), 1u);
    EXPECT_FLOAT_EQ(reloaded_edit->sdf_data[1], 13.5f);
}

// ---------------------------------------------------------------------------
// RUN == REPLAY style determinism over the serializer itself
// ---------------------------------------------------------------------------

TEST(PersistenceHardening, RepeatedSerializationIsIdentical) {
    WorldStreamingState world;
    PopulateRichWorld(world);
    const std::string a = SerializeWorldStreamingStateSnapshotJson(world);
    const std::string b = SerializeWorldStreamingStateSnapshotJson(world);
    EXPECT_EQ(a, b) << "serializing the same state twice differs (non-determinism)";
    EXPECT_EQ(ComputeWorldStreamingStateHash(world), ComputeWorldStreamingStateHash(world));
}

TEST(PersistenceHardening, DuplicateCoordsInLoadRejected) {
    // A hand-built snapshot text with two chunks sharing the same coords/id must
    // be rejected deterministically rather than silently collapsing the world.
    WorldStreamingState world;
    AddRichChunk(world, IVec3(0, 0, 0), ChunkState::Ready, 1u);
    std::string json = SerializeWorldStreamingStateSnapshotJson(world);

    // Duplicate the single chunk object within the chunks array and bump
    // chunk_count to keep the count gate happy, exercising the duplicate guard.
    const std::string chunk_open = "\"chunks\": [";
    const auto chunks_pos = json.find(chunk_open);
    ASSERT_NE(chunks_pos, std::string::npos);
    const auto first_obj_start = json.find('{', chunks_pos);
    ASSERT_NE(first_obj_start, std::string::npos);
    // Find the matching close brace of the first chunk object.
    int depth = 0;
    std::size_t first_obj_end = std::string::npos;
    for (std::size_t i = first_obj_start; i < json.size(); ++i) {
        if (json[i] == '{')
            ++depth;
        else if (json[i] == '}') {
            if (--depth == 0) {
                first_obj_end = i;
                break;
            }
        }
    }
    ASSERT_NE(first_obj_end, std::string::npos);
    const std::string first_obj = json.substr(first_obj_start, first_obj_end - first_obj_start + 1);
    json.insert(first_obj_end + 1, "," + first_obj);
    // Bump chunk_count from 1 to 2.
    const auto cc = json.find("\"chunk_count\": 1");
    if (cc != std::string::npos)
        json.replace(cc, std::string("\"chunk_count\": 1").size(), "\"chunk_count\": 2");

    WorldStreamingState restored;
    std::vector<std::string> errors;
    EXPECT_FALSE(LoadWorldStreamingStateSnapshotJson(json, restored, errors))
        << "duplicate chunk ids must be rejected on load";
    EXPECT_FALSE(errors.empty());
}
