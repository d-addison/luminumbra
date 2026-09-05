// Adversarial determinism-hardening gates for the E2E streaming path:
// world-streaming-state serialization + sub-hash, structure placement, and the
// far-LOD store. These hunt the bug classes the happy-path siblings
// (WorldStreamingState_test, MultiAnchorStreaming_test, StructurePlacement_test,
// FarLodStore_test) skip:
//   - ORDER INDEPENDENCE: the world hash / per-system sub-hashes must be a pure
//     function of the resident SET, not of chunk insert/iteration order. Build
//     the SAME state two different insertion orders and assert identical bytes.
//   - SERIALIZE/DESERIALIZE SYMMETRY: snapshot(state) -> load -> snapshot must be
//     byte-identical for a rich, adversarial state (negative/boundary coords,
//     zero, empty arrays, every persisted field populated distinctly).
//   - IDEMPOTENCY / DEGENERATE: empty roster -> stable empty hash no-op; re-gen
//     of a structure / far-LOD tile is byte-exact; an anchor / region on a chunk
//     seam stitches consistently.
//   - PURE-FUNCTION PLACEMENT: structure sites are a pure function of
//     (seed, salt, cell); a region's far-LOD tile shares its border samples with
//     the neighbour region; a params-hash mismatch is a clean cache miss.
//
// Determinism contract: every assertion is float/byte EXACT (EXPECT_EQ on the
// canonical strings / hashes; the underlying terrain math is the engine's
// DeterministicMath path). No wall-clock, no std::random.
//
// Namespace discipline (the #1 compile bite): WorldStreamingState/Chunk live in
// CAPITAL-L ::Luminumbra; the persistence helpers in ::Luminumbra::Persistence;
// structures + far-LOD in ::Luminumbra::World; the world system in
// Luminumbra::Systems. All aliased inside this anonymous namespace below.

#include "gtest/gtest.h"

#include "persistence/WorldPersistenceRoundtrip.h"
#include "persistence/WorldSaveService.h"
#include "systems/SHIELD_WorldSystem.h"
#include "world/Chunk.h"
#include "world/FarLodStore.h"
#include "world/StructurePlacement.h"
#include "world/WorldStreamingState.h"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <tuple>
#include <vector>

#ifndef LUMINUMBRA_SOURCE_ROOT
#define LUMINUMBRA_SOURCE_ROOT "."
#endif

namespace {

namespace fs = std::filesystem;

using Luminumbra::Chunk;
using Luminumbra::ChunkID;
using Luminumbra::ChunkSdfProvenance;
using Luminumbra::ChunkState;
using Luminumbra::IVec3;
using Luminumbra::u16;
using Luminumbra::u32;
using Luminumbra::u64;
using Luminumbra::u8;
using Luminumbra::Vec2;
using Luminumbra::Vec3;
using Luminumbra::VoxelVertex;
using Luminumbra::WorldStreamingState;

namespace Persistence = Luminumbra::Persistence;
namespace World = Luminumbra::World;

using Luminumbra::Systems::SHIELD_WorldSystem;
using Luminumbra::Systems::TerrainGenParams;

// ---------------------------------------------------------------------------
// Fixtures / helpers
// ---------------------------------------------------------------------------

// Stamp a chunk with a DISTINCT value in every persisted field so a dropped /
// wrong-width / wrong-sign field in the serializer surfaces as a roundtrip diff
// rather than hiding behind a default. `salt` is woven through every field.
void StampChunk(Chunk& chunk, u32 salt) {
    chunk.set_state(ChunkState::Ready);
    chunk.sdf_data = {-2.0f + static_cast<float>(salt), -0.5f, 0.0f, 0.25f, 1.0f};
    chunk.heightmap_data = {12.0f + static_cast<float>(salt), -13.5f, 0.0f, 14.0f};
    chunk.mesh_vertices = {
        {Vec3(0.0f, 1.0f, 0.0f), Vec3(0.0f, 1.0f, 0.0f), salt + 1u},
        {Vec3(-1.0f, 1.5f, 2.0f), Vec3(0.0f, -1.0f, 0.0f), salt + 2u},
        {Vec3(0.0f, 1.0f, -1.0f), Vec3(1.0f, 0.0f, 0.0f), salt + 3u},
    };
    chunk.mesh_indices = {0u, 1u, 2u};
    chunk.water_mesh_vertices = {
        {Vec3(0.0f, 2.0f, 0.0f), Vec3(0.0f, 1.0f, 0.0f), 5u},
        {Vec3(1.0f, 2.0f, 0.0f), Vec3(0.0f, 1.0f, 0.0f), 6u},
        {Vec3(1.0f, 2.0f, 1.0f), Vec3(0.0f, 1.0f, 0.0f), 7u},
    };
    chunk.water_mesh_indices = {0u, 1u, 2u};
    chunk.pending_mesh_vertices = chunk.mesh_vertices;
    chunk.pending_mesh_indices = chunk.mesh_indices;
    chunk.pending_water_mesh_vertices = chunk.water_mesh_vertices;
    chunk.pending_water_mesh_indices = chunk.water_mesh_indices;
    chunk.has_collision.store(true, std::memory_order_release);
    chunk.current_lod.store(static_cast<int>(salt % 3u), std::memory_order_release);
    chunk.pending_lod.store(static_cast<int>((salt + 1u) % 3u), std::memory_order_release);
    chunk.pending_mesh_ready.store(true, std::memory_order_release);
    chunk.pending_mesh_failed.store(false, std::memory_order_release);
    chunk.mesh_version.store(10u + salt, std::memory_order_release);
    chunk.water_mesh_version.store(20u + salt, std::memory_order_release);
    chunk.water_level_data = {2.25f, 2.5f, 2.75f, 3.0f};
    chunk.water_flow_data = {Vec2(0.1f, -0.2f), Vec2(0.0f, -0.1f)};
    chunk.water_sim_terrain_height = {1.0f, 1.25f, 1.5f, 1.75f};
    chunk.has_water_sim.store(true, std::memory_order_release);
    chunk.water_mesh_generated.store(true, std::memory_order_release);
    chunk.current_water_resolution.store(2, std::memory_order_release);
    chunk.is_water_sleeping.store(false, std::memory_order_release);
    chunk.max_water_delta_last_tick = 0.03125f * static_cast<float>(salt + 1u);
    chunk.ticks_below_threshold = static_cast<int>(salt);
    chunk.water_mesh_dirty_ticks = static_cast<int>(salt + 2u);
}

// A deterministic, adversarial roster: negative coords, zero, mixed signs, a
// near-boundary coordinate. Each (coords, salt) pair is stamped distinctly.
const std::vector<std::pair<IVec3, u32>>& AdversarialRoster() {
    static const std::vector<std::pair<IVec3, u32>> roster = {
        {IVec3(0, 0, 0), 0u},
        {IVec3(1, -1, 2), 1u},
        {IVec3(-2, 0, 1), 2u},
        {IVec3(-5, -7, -9), 3u},
        {IVec3(7, 3, -4), 4u},
        {IVec3(100000, -2000, -100000), 5u}, // far, in documented packing range
    };
    return roster;
}

// Build a fresh state inserting the roster in the given index order.
void BuildState(WorldStreamingState& state, const std::vector<std::size_t>& order) {
    for (std::size_t idx : order) {
        const auto& [coords, salt] = AdversarialRoster()[idx];
        auto chunk = state.get_or_create_chunk(coords);
        StampChunk(*chunk, salt);
    }
}

std::vector<std::size_t> ForwardOrder() {
    std::vector<std::size_t> order(AdversarialRoster().size());
    for (std::size_t i = 0; i < order.size(); ++i) {
        order[i] = i;
    }
    return order;
}

std::vector<std::size_t> ReverseOrder() {
    auto order = ForwardOrder();
    std::reverse(order.begin(), order.end());
    return order;
}

// A scrambled (neither forward nor reverse) insertion order.
std::vector<std::size_t> ScrambledOrder() {
    return {3, 0, 5, 1, 4, 2};
}

TerrainGenParams FixtureParams() {
    TerrainGenParams params;
    params.base_frequency = 0.005f;
    params.base_amplitude = 40.0f;
    params.height_offset = 10.0f;

    return params;
}

fs::path StructuresDir() {
    return fs::path(LUMINUMBRA_SOURCE_ROOT) / "data" / "common" / "structures";
}

World::StructureTemplatePool LoadCairn() {
    return World::LoadStructureTemplatePool(StructuresDir() / "cairn", "cairn");
}

World::StructureTemplatePool LoadRuin() {
    return World::LoadStructureTemplatePool(StructuresDir() / "ruin", "ruin");
}

struct TempSaveDir {
    explicit TempSaveDir(const std::string& tag) {
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        path = fs::temp_directory_path() /
               ("luminumbra_streaming_hardening_" + tag + "_" + std::to_string(stamp));
        fs::create_directories(path);
    }
    ~TempSaveDir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
    fs::path path;
};

constexpr int kSeed = 4242;

} // namespace

// ===========================================================================
// 1. ORDER INDEPENDENCE — the resident SET, not the insertion ORDER, decides
//    the world hash and the per-system sub-hashes. (Factorio CRC playbook: a
//    hash that depends on container iteration order is a latent desync.)
// ===========================================================================

TEST(StreamingHardening, WorldHashIsOrderIndependent) {
    WorldStreamingState forward;
    WorldStreamingState reverse;
    BuildState(forward, ForwardOrder());
    BuildState(reverse, ReverseOrder());

    EXPECT_EQ(forward.size(), reverse.size());
    EXPECT_EQ(Persistence::ComputeWorldStreamingStateHash(forward),
              Persistence::ComputeWorldStreamingStateHash(reverse))
        << "world hash changed with chunk insertion order — it must be a pure "
           "function of the resident set";
}

TEST(StreamingHardening, WorldHashIsScrambleIndependent) {
    WorldStreamingState forward;
    WorldStreamingState scrambled;
    BuildState(forward, ForwardOrder());
    BuildState(scrambled, ScrambledOrder());
    EXPECT_EQ(Persistence::ComputeWorldStreamingStateHash(forward),
              Persistence::ComputeWorldStreamingStateHash(scrambled));
}

TEST(StreamingHardening, SnapshotBytesAreOrderIndependent) {
    WorldStreamingState forward;
    WorldStreamingState reverse;
    BuildState(forward, ForwardOrder());
    BuildState(reverse, ReverseOrder());
    // The canonical snapshot string itself (not just its hash) must be identical.
    EXPECT_EQ(Persistence::SerializeWorldStreamingStateSnapshotJson(forward),
              Persistence::SerializeWorldStreamingStateSnapshotJson(reverse));
}

TEST(StreamingHardening, EverySubHashIsOrderIndependent) {
    WorldStreamingState forward;
    WorldStreamingState reverse;
    BuildState(forward, ForwardOrder());
    BuildState(reverse, ReverseOrder());

    const auto a = Persistence::ComputeWorldStreamingStateSubHashes(forward);
    const auto b = Persistence::ComputeWorldStreamingStateSubHashes(reverse);
    EXPECT_EQ(a.terrain, b.terrain) << "terrain sub-hash depends on insertion order";
    EXPECT_EQ(a.mesh, b.mesh) << "mesh sub-hash depends on insertion order";
    EXPECT_EQ(a.water, b.water) << "water sub-hash depends on insertion order";
}

TEST(StreamingHardening, SubHashesAreNonEmptyAndDistinctAcrossSubsystems) {
    WorldStreamingState state;
    BuildState(state, ForwardOrder());
    const auto sub = Persistence::ComputeWorldStreamingStateSubHashes(state);
    EXPECT_FALSE(sub.terrain.empty());
    EXPECT_FALSE(sub.mesh.empty());
    EXPECT_FALSE(sub.water.empty());
    // The three subsystem projections carry different field sets, so for a rich
    // populated state they must not collapse to the same checksum.
    EXPECT_NE(sub.terrain, sub.mesh);
    EXPECT_NE(sub.terrain, sub.water);
    EXPECT_NE(sub.mesh, sub.water);
}

// erase-then-reinsert yields the same resident set => the same hash (the
// container has a different internal layout afterwards).
TEST(StreamingHardening, EraseReinsertConvergesToSameHash) {
    WorldStreamingState state;
    BuildState(state, ForwardOrder());
    const std::string before = Persistence::ComputeWorldStreamingStateHash(state);

    // Evict one chunk and put an identically-stamped one back.
    const auto& [coords, salt] = AdversarialRoster()[2];
    ASSERT_TRUE(state.contains_chunk(coords));
    ASSERT_TRUE(state.erase_chunk(coords));
    EXPECT_NE(Persistence::ComputeWorldStreamingStateHash(state), before)
        << "removing a chunk must change the world hash";
    auto chunk = state.get_or_create_chunk(coords);
    StampChunk(*chunk, salt);

    EXPECT_EQ(Persistence::ComputeWorldStreamingStateHash(state), before)
        << "restoring the identical resident set must restore the identical hash";
}

// ===========================================================================
// 2. SERIALIZE / DESERIALIZE SYMMETRY — encode(x) -> decode -> encode is byte
//    exact for an adversarial, fully-populated state. A field written but not
//    read (or read at the wrong width/sign) shows up as a snapshot diff here.
// ===========================================================================

TEST(StreamingHardening, SnapshotRoundTripIsByteExact) {
    WorldStreamingState before;
    BuildState(before, ScrambledOrder());
    const std::string before_json = Persistence::SerializeWorldStreamingStateSnapshotJson(before);

    WorldStreamingState after;
    std::vector<std::string> errors;
    ASSERT_TRUE(Persistence::LoadWorldStreamingStateSnapshotJson(before_json, after, errors))
        << (errors.empty() ? "" : errors.front());
    EXPECT_TRUE(errors.empty());
    EXPECT_EQ(before.size(), after.size());

    const std::string after_json = Persistence::SerializeWorldStreamingStateSnapshotJson(after);
    EXPECT_EQ(before_json, after_json)
        << "snapshot -> load -> snapshot diverged: a persisted field is dropped "
           "or read with the wrong width/sign";
}

TEST(StreamingHardening, RoundTripPreservesWorldHashAndAllSubHashes) {
    WorldStreamingState before;
    BuildState(before, ForwardOrder());
    const std::string before_json = Persistence::SerializeWorldStreamingStateSnapshotJson(before);

    WorldStreamingState after;
    std::vector<std::string> errors;
    ASSERT_TRUE(Persistence::LoadWorldStreamingStateSnapshotJson(before_json, after, errors));

    EXPECT_EQ(Persistence::ComputeWorldStreamingStateHash(before),
              Persistence::ComputeWorldStreamingStateHash(after));
    const auto sa = Persistence::ComputeWorldStreamingStateSubHashes(before);
    const auto sb = Persistence::ComputeWorldStreamingStateSubHashes(after);
    EXPECT_EQ(sa.terrain, sb.terrain);
    EXPECT_EQ(sa.mesh, sb.mesh);
    EXPECT_EQ(sa.water, sb.water);
}

TEST(StreamingHardening, RoundTripPreservesEveryChunkIdentity) {
    WorldStreamingState before;
    BuildState(before, ForwardOrder());
    const std::string json = Persistence::SerializeWorldStreamingStateSnapshotJson(before);

    WorldStreamingState after;
    std::vector<std::string> errors;
    ASSERT_TRUE(Persistence::LoadWorldStreamingStateSnapshotJson(json, after, errors));

    for (const auto& [coords, salt] : AdversarialRoster()) {
        (void)salt;
        const ChunkID id = Chunk::calculate_id(coords);
        EXPECT_TRUE(after.contains_chunk(id))
            << "chunk id " << id << " from coords (" << coords.x << "," << coords.y << ","
            << coords.z << ") lost across roundtrip";
    }
}

// Re-serializing the SAME state twice (no mutation) is byte-identical: the
// serializer has no hidden run-to-run state.
TEST(StreamingHardening, RepeatedSerializeIsStable) {
    WorldStreamingState state;
    BuildState(state, ScrambledOrder());
    const std::string first = Persistence::SerializeWorldStreamingStateSnapshotJson(state);
    const std::string second = Persistence::SerializeWorldStreamingStateSnapshotJson(state);
    EXPECT_EQ(first, second);
}

// A chunk holding empty vectors / negative coords / zeroed scalars must round
// trip — empty arrays are a classic "read count != written count" trap.
TEST(StreamingHardening, EmptyArrayChunkRoundTripsExactly) {
    WorldStreamingState before;
    auto chunk = before.get_or_create_chunk(IVec3(-13, 0, 41));
    chunk->set_state(ChunkState::Idle);
    // Leave every vector empty and every scalar at its default; only flip a few
    // boundary values.
    chunk->current_lod.store(-1, std::memory_order_release);
    chunk->pending_lod.store(-1, std::memory_order_release);
    chunk->mesh_version.store(0u, std::memory_order_release);
    chunk->max_water_delta_last_tick = 0.0f;

    const std::string before_json = Persistence::SerializeWorldStreamingStateSnapshotJson(before);
    WorldStreamingState after;
    std::vector<std::string> errors;
    ASSERT_TRUE(Persistence::LoadWorldStreamingStateSnapshotJson(before_json, after, errors));
    EXPECT_EQ(before_json, Persistence::SerializeWorldStreamingStateSnapshotJson(after));
}

// ===========================================================================
// 3. DEGENERATE / EMPTY — empty roster is a stable no-op; single entity works.
// ===========================================================================

TEST(StreamingHardening, EmptyStateHashIsStableNoOp) {
    WorldStreamingState a;
    WorldStreamingState b;
    EXPECT_TRUE(a.empty());
    // Two independently-constructed empty states hash identically and stably.
    EXPECT_EQ(Persistence::ComputeWorldStreamingStateHash(a),
              Persistence::ComputeWorldStreamingStateHash(b));
    EXPECT_EQ(Persistence::ComputeWorldStreamingStateHash(a),
              Persistence::ComputeWorldStreamingStateHash(a));

    const auto sub = Persistence::ComputeWorldStreamingStateSubHashes(a);
    EXPECT_FALSE(sub.terrain.empty()); // empty SECTION still hashes (chunk_count 0)
    EXPECT_FALSE(sub.mesh.empty());
    EXPECT_FALSE(sub.water.empty());
}

TEST(StreamingHardening, EmptyStateRoundTripsToEmpty) {
    WorldStreamingState before;
    const std::string json = Persistence::SerializeWorldStreamingStateSnapshotJson(before);
    WorldStreamingState after;
    std::vector<std::string> errors;
    ASSERT_TRUE(Persistence::LoadWorldStreamingStateSnapshotJson(json, after, errors));
    EXPECT_TRUE(after.empty());
    EXPECT_EQ(json, Persistence::SerializeWorldStreamingStateSnapshotJson(after));
}

TEST(StreamingHardening, SingleChunkStateRoundTrips) {
    WorldStreamingState before;
    auto chunk = before.get_or_create_chunk(IVec3(0, 0, 0));
    StampChunk(*chunk, 9u);
    const std::string json = Persistence::SerializeWorldStreamingStateSnapshotJson(before);
    WorldStreamingState after;
    std::vector<std::string> errors;
    ASSERT_TRUE(Persistence::LoadWorldStreamingStateSnapshotJson(json, after, errors));
    EXPECT_EQ(after.size(), 1u);
    EXPECT_EQ(json, Persistence::SerializeWorldStreamingStateSnapshotJson(after));
}

// ===========================================================================
// 4. STRUCTURE PLACEMENT — sites are a PURE FUNCTION of (seed, salt, cell);
//    enumeration is deterministic; assembly re-gen is byte-exact; a structure's
//    voxel set is independent of WHEN it is assembled.
// ===========================================================================

TEST(StreamingHardening, SiteInCellIsPureFunctionOfSeedAndCell) {
    const auto cairn = LoadCairn();
    ASSERT_TRUE(cairn.ok());
    for (int cz = -4; cz <= 4; ++cz) {
        for (int cx = -4; cx <= 4; ++cx) {
            const auto a = World::SiteInCell(cairn, kSeed, cx, cz);
            const auto b = World::SiteInCell(cairn, kSeed, cx, cz);
            ASSERT_EQ(a.has_value(), b.has_value()) << "cell " << cx << "," << cz;
            if (a && b) {
                EXPECT_EQ(a->origin, b->origin);
                EXPECT_EQ(a->site_seed, b->site_seed);
                EXPECT_EQ(a->type, b->type);
            }
        }
    }
}

TEST(StreamingHardening, SitesInAreaIsDeterministicAndOrderStable) {
    const auto cairn = LoadCairn();
    ASSERT_TRUE(cairn.ok());
    const auto first = World::SitesInArea(cairn, kSeed, -300, -300, 700, 700);
    const auto second = World::SitesInArea(cairn, kSeed, -300, -300, 700, 700);
    ASSERT_EQ(first.size(), second.size());
    for (std::size_t i = 0; i < first.size(); ++i) {
        EXPECT_EQ(first[i].origin, second[i].origin) << "site " << i;
        EXPECT_EQ(first[i].site_seed, second[i].site_seed);
    }
}

// The per-cell query and the area enumeration must AGREE: every site that
// SiteInCell reports for a cell inside the window must appear in SitesInArea
// (no double-count, no drop). This is the placement analogue of the
// multi-anchor "union/dedup must not double-count or drop" hazard.
TEST(StreamingHardening, SitesInAreaMatchesPerCellEnumeration) {
    const auto cairn = LoadCairn();
    ASSERT_TRUE(cairn.ok());
    const int min_x = -200, min_z = -200, max_x = 600, max_z = 600;
    const auto area = World::SitesInArea(cairn, kSeed, min_x, min_z, max_x, max_z);

    // Reference: brute-force every cell whose origin could land in the window.
    std::set<std::tuple<int, int, int>> reference;
    const int span = cairn.spacing;
    const int cmin_x = (min_x - span) / span - 1;
    const int cmax_x = (max_x + span) / span + 1;
    const int cmin_z = (min_z - span) / span - 1;
    const int cmax_z = (max_z + span) / span + 1;
    for (int cz = cmin_z; cz <= cmax_z; ++cz) {
        for (int cx = cmin_x; cx <= cmax_x; ++cx) {
            if (auto s = World::SiteInCell(cairn, kSeed, cx, cz)) {
                if (s->origin.x >= min_x && s->origin.x < max_x && s->origin.z >= min_z &&
                    s->origin.z < max_z) {
                    reference.emplace(s->origin.x, s->origin.y, s->origin.z);
                }
            }
        }
    }
    std::set<std::tuple<int, int, int>> seen;
    for (const auto& s : area) {
        // No duplicate sites in the enumeration.
        const auto key = std::make_tuple(s.origin.x, s.origin.y, s.origin.z);
        EXPECT_TRUE(seen.insert(key).second) << "SitesInArea returned a duplicate site";
    }
    EXPECT_EQ(seen, reference) << "SitesInArea disagrees with the per-cell SiteInCell enumeration "
                                  "(a shared/edge cell was double-counted or dropped)";
}

TEST(StreamingHardening, SitesInAreaRespectsHalfOpenBounds) {
    const auto cairn = LoadCairn();
    ASSERT_TRUE(cairn.ok());
    const int min_x = -200, min_z = -200, max_x = 600, max_z = 600;
    const auto sites = World::SitesInArea(cairn, kSeed, min_x, min_z, max_x, max_z);
    for (const auto& s : sites) {
        EXPECT_GE(s.origin.x, min_x);
        EXPECT_LT(s.origin.x, max_x);
        EXPECT_GE(s.origin.z, min_z);
        EXPECT_LT(s.origin.z, max_z);
    }
}

TEST(StreamingHardening, AssembledVoxelsAreByteExactOnRegen) {
    const auto cairn = LoadCairn();
    ASSERT_TRUE(cairn.ok());
    World::StructureSite site;
    site.type = "cairn";
    site.origin = IVec3(0, 0, 0);
    site.site_seed = 0xABCDEF0123456789ull;

    const auto a = World::AssembleStructure(cairn, site);
    const auto b = World::AssembleStructure(cairn, site);
    ASSERT_FALSE(a.empty());
    ASSERT_EQ(a.size(), b.size());
    for (std::size_t i = 0; i < a.size(); ++i) {
        EXPECT_EQ(a[i].position, b[i].position) << "voxel " << i;
        EXPECT_EQ(a[i].material, b[i].material) << "voxel " << i;
    }
    EXPECT_EQ(World::ComputeAssembledVoxelHash(a), World::ComputeAssembledVoxelHash(b));
}

// The assembled-voxel hash must be order-independent: it canonicalizes
// (sorts by position then material) before hashing, so a permuted input set
// hashes identically. Guards against an order-dependent assembly digest.
TEST(StreamingHardening, AssembledVoxelHashIsCanonicalOrderIndependent) {
    const auto ruin = LoadRuin();
    ASSERT_TRUE(ruin.ok());
    World::StructureSite site;
    site.type = "ruin";
    site.origin = IVec3(100, 32, -50);
    site.site_seed = 0x1234567890ABCDEFull;

    auto voxels = World::AssembleStructure(ruin, site);
    ASSERT_GT(voxels.size(), 1u);
    const u64 ordered = World::ComputeAssembledVoxelHash(voxels);

    std::reverse(voxels.begin(), voxels.end());
    EXPECT_EQ(World::ComputeAssembledVoxelHash(voxels), ordered)
        << "assembled-voxel hash depends on voxel order — it must canonicalize first";
}

// Translation invariance of the SHAPE: assembling the same structure at a
// different origin shifts every voxel by exactly that origin delta (a structure
// straddling a chunk seam must not deform with position).
TEST(StreamingHardening, AssemblyIsPureTranslationOfShape) {
    const auto cairn = LoadCairn();
    ASSERT_TRUE(cairn.ok());
    World::StructureSite at_origin;
    at_origin.type = "cairn";
    at_origin.origin = IVec3(0, 0, 0);
    at_origin.site_seed = 0x55AA55AA55AA55AAull;

    World::StructureSite shifted = at_origin;
    const IVec3 delta(48, 7, -129); // off-grid + crosses a 16 m chunk seam
    shifted.origin = delta;

    const auto base = World::AssembleStructure(cairn, at_origin);
    const auto moved = World::AssembleStructure(cairn, shifted);
    ASSERT_FALSE(base.empty());
    ASSERT_EQ(base.size(), moved.size());
    for (std::size_t i = 0; i < base.size(); ++i) {
        EXPECT_EQ(moved[i].position, base[i].position + delta) << "voxel " << i;
        EXPECT_EQ(moved[i].material, base[i].material) << "voxel " << i;
    }
}

TEST(StreamingHardening, LocateNearestMatchesBruteForce) {
    const auto ruin = LoadRuin();
    ASSERT_TRUE(ruin.ok());
    const int near_x = 500, near_z = -300, radius = 2;
    const auto located = World::LocateNearestSite(ruin, kSeed, near_x, near_z, radius);

    const int cx = near_x >= 0 ? near_x / ruin.spacing : (near_x - ruin.spacing + 1) / ruin.spacing;
    const int cz = near_z >= 0 ? near_z / ruin.spacing : (near_z - ruin.spacing + 1) / ruin.spacing;
    std::optional<World::StructureSite> best;
    long long best_d2 = -1;
    for (int dz = -radius; dz <= radius; ++dz) {
        for (int dx = -radius; dx <= radius; ++dx) {
            if (auto s = World::SiteInCell(ruin, kSeed, cx + dx, cz + dz)) {
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

// ===========================================================================
// 5. FAR-LOD STORE — quantization roundtrip, tile-hash determinism, the
//    shared-border seam contract, persistence roundtrip, and the
//    params-hash-mismatch cache-miss contract.
// ===========================================================================

TEST(StreamingHardening, HeightQuantizationRoundTripsAtBoundaries) {
    using namespace World;
    // Exact representable extremes: min and max-1-step round trip to themselves.
    EXPECT_EQ(QuantizeFarLodHeight(kFarLodHeightQuantMin), 0u);
    EXPECT_FLOAT_EQ(DequantizeFarLodHeight(0u), kFarLodHeightQuantMin);
    // The max representable height (header: +2047.9375 m) maps to 65535.
    const float max_height = kFarLodHeightQuantMin + 65535.0f / kFarLodHeightQuantScale;
    EXPECT_EQ(QuantizeFarLodHeight(max_height), 65535u);
    EXPECT_FLOAT_EQ(DequantizeFarLodHeight(65535u), max_height);
    // Out-of-domain heights CLAMP (do not wrap).
    EXPECT_EQ(QuantizeFarLodHeight(-1.0e6f), 0u);
    EXPECT_EQ(QuantizeFarLodHeight(1.0e6f), 65535u);
    // Every quantized step round-trips within half a step.
    for (float h : {-300.0f, -1.25f, 0.0f, 0.0625f, 17.5f, 950.0f}) {
        EXPECT_NEAR(
            DequantizeFarLodHeight(QuantizeFarLodHeight(h)), h, 0.5f / kFarLodHeightQuantScale);
    }
}

TEST(StreamingHardening, FarLodSnapshotOwnsBytesAndRejectsStaleLiveData) {
    const TerrainGenParams params = FixtureParams();
    SHIELD_WorldSystem world(nullptr, nullptr, params, kSeed);
    auto chunk = std::make_shared<Chunk>(IVec3(0, 0, 0));
    const std::size_t full_lattice = static_cast<std::size_t>(Luminumbra::CHUNK_SIZE_X + 1) *
                                     static_cast<std::size_t>(Luminumbra::CHUNK_SIZE_Y + 1) *
                                     static_cast<std::size_t>(Luminumbra::CHUNK_SIZE_Z + 1);
    chunk->sdf_data.assign(full_lattice, -1.0f);
    chunk->material_data.assign(full_lattice, 7u);
    ASSERT_TRUE(world.adopt_streamed_chunk(chunk));

    const auto captured = world.capture_far_lod_sdf_snapshot(0, 0);
    ASSERT_TRUE(captured);
    ASSERT_EQ(captured->entries.size(), 1u);
    const auto& entry = captured->entries.front();
    EXPECT_EQ(entry.sdf_data.front(), -1.0f);
    EXPECT_EQ(entry.material_data.front(), 7u);
    EXPECT_TRUE(world.is_far_lod_sdf_snapshot_current(*captured));

    // The worker's copy is independent from the mutable live lattice. The
    // revision/provenance check rejects a subsequently changed live chunk.
    chunk->sdf_data.front() = 3.0f;
    chunk->material_data.front() = 2u;
    chunk->mark_voxel_data_dirty();
    EXPECT_EQ(entry.sdf_data.front(), -1.0f);
    EXPECT_EQ(entry.material_data.front(), 7u);
    EXPECT_FALSE(world.is_far_lod_sdf_snapshot_current(*captured));
}

TEST(StreamingHardening, ChunkSdfProvenanceRevisionLifecycleIsSticky) {
    Chunk chunk(IVec3(0, 0, 0));
    EXPECT_EQ(chunk.sdf_provenance(), ChunkSdfProvenance::GeneratedCurrentParams);
    EXPECT_EQ(chunk.voxel_revision(), 0u);
    EXPECT_FALSE(chunk.is_voxel_data_dirty());

    chunk.mark_voxel_data_dirty();
    EXPECT_EQ(chunk.sdf_provenance(), ChunkSdfProvenance::LoadedOrEdited);
    EXPECT_EQ(chunk.voxel_revision(), 1u);
    EXPECT_TRUE(chunk.is_voxel_data_dirty());
    chunk.clear_voxel_data_dirty();
    EXPECT_EQ(chunk.sdf_provenance(), ChunkSdfProvenance::LoadedOrEdited);
    EXPECT_EQ(chunk.voxel_revision(), 1u);
    EXPECT_FALSE(chunk.is_voxel_data_dirty());
    chunk.mark_voxel_data_dirty();
    EXPECT_EQ(chunk.voxel_revision(), 2u);

    chunk.mark_sdf_generated_current_params();
    EXPECT_EQ(chunk.sdf_provenance(), ChunkSdfProvenance::GeneratedCurrentParams);
    EXPECT_EQ(chunk.voxel_revision(), 0u);
    EXPECT_FALSE(chunk.is_voxel_data_dirty());
    chunk.mark_sdf_loaded_or_edited();
    EXPECT_EQ(chunk.sdf_provenance(), ChunkSdfProvenance::LoadedOrEdited);
    EXPECT_EQ(chunk.voxel_revision(), 1u);
    EXPECT_FALSE(chunk.is_voxel_data_dirty());
}

TEST(StreamingHardening, ChunkRecordEditedFlagRestoresOnlyDurableAuthority) {
    Persistence::WorldSaveService service;
    std::vector<std::string> errors;

    TempSaveDir pristine_dir("pristine_provenance");
    WorldStreamingState pristine;
    auto pristine_chunk = pristine.get_or_create_chunk(IVec3(0, 0, 0));
    pristine_chunk->sdf_data = {-1.0f, 1.0f};
    pristine_chunk->mark_sdf_generated_current_params();
    ASSERT_TRUE(service.save_world(pristine, pristine_dir.path, &errors));
    WorldStreamingState pristine_loaded;
    ASSERT_TRUE(service.load_world(pristine_loaded, pristine_dir.path, errors));
    ASSERT_EQ(pristine_loaded.size(), 1u);
    EXPECT_EQ(pristine_loaded.snapshot_chunks().front()->sdf_provenance(),
              ChunkSdfProvenance::GeneratedCurrentParams);
    EXPECT_EQ(pristine_loaded.snapshot_chunks().front()->voxel_revision(), 0u);

    TempSaveDir edited_dir("edited_provenance");
    WorldStreamingState edited;
    auto edited_chunk = edited.get_or_create_chunk(IVec3(0, 0, 0));
    edited_chunk->sdf_data = {-2.0f, 2.0f};
    edited_chunk->mark_voxel_data_dirty();
    errors.clear();
    ASSERT_TRUE(service.save_world(edited, edited_dir.path, &errors));
    edited_chunk->clear_voxel_data_dirty();
    EXPECT_EQ(edited_chunk->sdf_provenance(), ChunkSdfProvenance::LoadedOrEdited);
    WorldStreamingState edited_loaded;
    ASSERT_TRUE(service.load_world(edited_loaded, edited_dir.path, errors));
    ASSERT_EQ(edited_loaded.size(), 1u);
    EXPECT_EQ(edited_loaded.snapshot_chunks().front()->sdf_provenance(),
              ChunkSdfProvenance::LoadedOrEdited);
    EXPECT_EQ(edited_loaded.snapshot_chunks().front()->voxel_revision(), 1u);
}

TEST(StreamingHardening, FarLodSnapshotFiltersAndOrdersNegativeRegionWithHalo) {
    const TerrainGenParams params = FixtureParams();
    SHIELD_WorldSystem world(nullptr, nullptr, params, kSeed);
    const std::size_t full_lattice = static_cast<std::size_t>(Luminumbra::CHUNK_SIZE_X + 1) *
                                     static_cast<std::size_t>(Luminumbra::CHUNK_SIZE_Y + 1) *
                                     static_cast<std::size_t>(Luminumbra::CHUNK_SIZE_Z + 1);
    const auto adopt =
        [&](const IVec3& coords, bool material, bool malformed_sdf, bool malformed_material) {
            auto chunk = std::make_shared<Chunk>(coords);
            chunk->sdf_data.assign(malformed_sdf ? 3u : full_lattice, static_cast<float>(coords.y));
            if (material) {
                chunk->material_data.assign(malformed_material ? 3u : full_lattice,
                                            static_cast<u8>(coords.y + 8));
            }
            EXPECT_TRUE(world.adopt_streamed_chunk(chunk));
        };
    adopt(IVec3(-33, 2, -33), true, false, false); // negative halo
    adopt(IVec3(-32, 1, -32), false, false, false);
    adopt(IVec3(-32, -1, -32), true, false, false);
    adopt(IVec3(0, 0, 0), true, false, false);    // positive halo
    adopt(IVec3(1, 0, 0), true, false, false);    // outside halo
    adopt(IVec3(-31, 0, -31), true, true, false); // malformed SDF
    adopt(IVec3(-30, 0, -30), true, false, true); // malformed material

    const auto snapshot = world.capture_far_lod_sdf_snapshot(-1, -1);
    ASSERT_TRUE(snapshot);
    ASSERT_EQ(snapshot->entries.size(), 4u);
    EXPECT_EQ(snapshot->entries[0].coords, IVec3(-33, 2, -33));
    EXPECT_EQ(snapshot->entries[1].coords, IVec3(-32, -1, -32));
    EXPECT_EQ(snapshot->entries[2].coords, IVec3(-32, 1, -32));
    EXPECT_EQ(snapshot->entries[3].coords, IVec3(0, 0, 0));
    EXPECT_TRUE(snapshot->entries[2].material_data.empty());
    EXPECT_EQ(snapshot->entries[1].material_data.size(), full_lattice);
}

TEST(StreamingHardening, PristineTileBuildIsDeterministic) {
    const TerrainGenParams params = FixtureParams();
    const SHIELD_WorldSystem world(nullptr, nullptr, params, kSeed);
    const u64 ph = World::ComputeTerrainParamsHash(params, kSeed);

    const World::FarLodTile a =
        World::BuildPristineFarLodTile(world, World::FarLodTier::F1, 0, 0, ph);
    const World::FarLodTile b =
        World::BuildPristineFarLodTile(world, World::FarLodTier::F1, 0, 0, ph);
    EXPECT_FALSE(a.edited);
    EXPECT_EQ(World::ComputeFarLodTileHash(a), World::ComputeFarLodTileHash(b));
    // Different region / tier must hash differently.
    const World::FarLodTile other =
        World::BuildPristineFarLodTile(world, World::FarLodTier::F1, 1, 0, ph);
    EXPECT_NE(World::ComputeFarLodTileHash(a), World::ComputeFarLodTileHash(other));
    const World::FarLodTile f2 =
        World::BuildPristineFarLodTile(world, World::FarLodTier::F2, 0, 0, ph);
    EXPECT_NE(World::ComputeFarLodTileHash(a), World::ComputeFarLodTileHash(f2));
}

// The store record id distinguishes adjacent regions and the two tiers but is a
// pure function of (tier, rx, rz) — re-deriving it is byte-exact. Within the
// documented packing range it never collides for distinct in-range keys.
TEST(StreamingHardening, TileRecordIdIsDeterministicAndDistinct) {
    using World::FarLodStore;
    using World::FarLodTier;
    EXPECT_EQ(FarLodStore::tile_record_id(FarLodTier::F1, 3, -5),
              FarLodStore::tile_record_id(FarLodTier::F1, 3, -5));
    EXPECT_NE(FarLodStore::tile_record_id(FarLodTier::F1, 3, -5),
              FarLodStore::tile_record_id(FarLodTier::F1, 4, -5));
    EXPECT_NE(FarLodStore::tile_record_id(FarLodTier::F1, 3, -5),
              FarLodStore::tile_record_id(FarLodTier::F1, 3, -4));
    EXPECT_NE(FarLodStore::tile_record_id(FarLodTier::F1, 3, -5),
              FarLodStore::tile_record_id(FarLodTier::F2, 3, -5));
}

// SEAM CONTRACT: the max-X column of region (rx,rz) is byte-equal to the min-X
// column of region (rx+1,rz). A region exactly on the seam must stitch.
TEST(StreamingHardening, AdjacentRegionsShareBorderColumnExactly) {
    const TerrainGenParams params = FixtureParams();
    const SHIELD_WorldSystem world(nullptr, nullptr, params, kSeed);
    const u64 ph = World::ComputeTerrainParamsHash(params, kSeed);

    const World::FarLodTile left =
        World::BuildPristineFarLodTile(world, World::FarLodTier::F1, 0, 0, ph);
    const World::FarLodTile right =
        World::BuildPristineFarLodTile(world, World::FarLodTier::F1, 1, 0, ph);
    const u32 n = left.samples_per_side;
    ASSERT_EQ(right.samples_per_side, n);
    for (u32 z = 0; z < n; ++z) {
        const std::size_t li = static_cast<std::size_t>(z) * n + (n - 1u);
        const std::size_t ri = static_cast<std::size_t>(z) * n;
        EXPECT_EQ(left.height_q[li], right.height_q[ri]) << "row " << z << " height";
        EXPECT_EQ(left.material[li], right.material[ri]) << "row " << z << " material";
        EXPECT_EQ(left.flags[li], right.flags[ri]) << "row " << z << " flags";
    }
    // ... and across the Z seam too.
    const World::FarLodTile up =
        World::BuildPristineFarLodTile(world, World::FarLodTier::F1, 0, 1, ph);
    for (u32 x = 0; x < n; ++x) {
        const std::size_t bi = static_cast<std::size_t>(n - 1u) * n + x; // max-Z row of (0,0)
        const std::size_t ti = static_cast<std::size_t>(0) * n + x;      // min-Z row of (0,1)
        EXPECT_EQ(left.height_q[bi], up.height_q[ti]) << "col " << x << " z-seam height";
    }
}

TEST(StreamingHardening, TilePersistenceRoundTripsExactly) {
    const TerrainGenParams params = FixtureParams();
    const SHIELD_WorldSystem world(nullptr, nullptr, params, kSeed);
    const u64 ph = World::ComputeTerrainParamsHash(params, kSeed);

    TempSaveDir dir("tile");
    const World::FarLodStore store(dir.path);

    const World::FarLodTile tile =
        World::BuildPristineFarLodTile(world, World::FarLodTier::F1, -1, 2, ph);
    std::vector<std::string> errors;
    ASSERT_TRUE(store.save_tile(tile, &errors)) << (errors.empty() ? "" : errors.front());

    World::FarLodTile loaded;
    ASSERT_TRUE(store.load_tile(World::FarLodTier::F1, -1, 2, ph, loaded, &errors));
    EXPECT_EQ(loaded.samples_per_side, tile.samples_per_side);
    EXPECT_EQ(loaded.params_hash, tile.params_hash);
    EXPECT_EQ(loaded.edited, tile.edited);
    // Every sample array byte-identical.
    ASSERT_EQ(loaded.height_q.size(), tile.height_q.size());
    EXPECT_EQ(loaded.height_q, tile.height_q);
    EXPECT_EQ(loaded.material, tile.material);
    EXPECT_EQ(loaded.flags, tile.flags);
    EXPECT_EQ(World::ComputeFarLodTileHash(loaded), World::ComputeFarLodTileHash(tile));
}

TEST(StreamingHardening, PristineCacheMissesOnParamsMismatch) {
    const TerrainGenParams params = FixtureParams();
    const SHIELD_WorldSystem world(nullptr, nullptr, params, kSeed);
    const u64 ph = World::ComputeTerrainParamsHash(params, kSeed);
    const u64 other = World::ComputeTerrainParamsHash(params, kSeed + 1);
    ASSERT_NE(ph, other);

    TempSaveDir dir("cache");
    const World::FarLodStore store(dir.path);
    std::vector<std::string> errors;

    const World::FarLodTile pristine =
        World::BuildPristineFarLodTile(world, World::FarLodTier::F2, 0, 0, ph);
    ASSERT_TRUE(store.save_tile(pristine, &errors));
    World::FarLodTile loaded;
    EXPECT_FALSE(store.load_tile(World::FarLodTier::F2, 0, 0, other, loaded, &errors))
        << "a params-hash mismatch on a PRISTINE (regenerable) tile must be a clean miss";
    EXPECT_TRUE(errors.empty());
}

// Saving the same tile twice (overwrite) and reloading must yield the identical
// tile — the store is idempotent for a repeated write.
TEST(StreamingHardening, TileSaveIsIdempotent) {
    const TerrainGenParams params = FixtureParams();
    const SHIELD_WorldSystem world(nullptr, nullptr, params, kSeed);
    const u64 ph = World::ComputeTerrainParamsHash(params, kSeed);

    TempSaveDir dir("idem");
    const World::FarLodStore store(dir.path);
    const World::FarLodTile tile =
        World::BuildPristineFarLodTile(world, World::FarLodTier::F1, 2, -3, ph);

    std::vector<std::string> errors;
    ASSERT_TRUE(store.save_tile(tile, &errors));
    ASSERT_TRUE(store.save_tile(tile, &errors)); // overwrite with identical content

    World::FarLodTile loaded;
    ASSERT_TRUE(store.load_tile(World::FarLodTier::F1, 2, -3, ph, loaded, &errors));
    EXPECT_EQ(World::ComputeFarLodTileHash(loaded), World::ComputeFarLodTileHash(tile));
    EXPECT_EQ(loaded.height_q, tile.height_q);
}

// A missing (tier,region) record is a clean miss, not an error — the loader must
// distinguish "not present" from "corrupt".
TEST(StreamingHardening, MissingTileIsCleanMiss) {
    TempSaveDir dir("miss");
    const World::FarLodStore store(dir.path);
    World::FarLodTile loaded;
    std::vector<std::string> errors;
    EXPECT_FALSE(store.load_tile(World::FarLodTier::F1, 99, 99, 0u, loaded, &errors));
    EXPECT_TRUE(errors.empty());
}

// ===========================================================================
// 6. RUN == REPLAY (roundtrip-exact end-to-end): a state built, serialized,
//    loaded, and re-serialized reproduces the SAME canonical bytes regardless
//    of the insertion order used on the rebuild path — the streaming-state
//    determinism contract in one assertion.
// ===========================================================================

TEST(StreamingHardening, RunEqualsReplayThroughSerializationAndReorder) {
    // "Run": build forward, serialize.
    WorldStreamingState run;
    BuildState(run, ForwardOrder());
    const std::string run_json = Persistence::SerializeWorldStreamingStateSnapshotJson(run);

    // "Replay": rebuild the SAME resident set in scrambled order, serialize.
    WorldStreamingState replay;
    BuildState(replay, ScrambledOrder());
    const std::string replay_json = Persistence::SerializeWorldStreamingStateSnapshotJson(replay);

    // Run == replay: byte-exact despite the different build order.
    EXPECT_EQ(run_json, replay_json);

    // And a load of the run bytes re-serializes to the same canonical string.
    WorldStreamingState loaded;
    std::vector<std::string> errors;
    ASSERT_TRUE(Persistence::LoadWorldStreamingStateSnapshotJson(run_json, loaded, errors));
    EXPECT_EQ(run_json, Persistence::SerializeWorldStreamingStateSnapshotJson(loaded));
}
