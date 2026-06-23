// spec 008 follow-up: LIVE-water determinism gate. The existing lockstep/persistence determinism
// gates run hand-built FIXTURE chunk states and NEVER tick a live WaterSystem, so a non-deterministic
// water-sim change (e.g. an async integration whose completion order depends on worker timing) would
// pass them all yet silently desync host==peer. This test ticks a REAL streaming world with active
// rivers, moving the anchor so new water chunks stream + init + simulate, and asserts two independent
// runs (same seed + same anchor path) produce an identical per-tick water-state hash sequence — the
// run==replay / host==peer property for live water. Prereq before amortizing the water-sim wait().
#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "luminumbra_common/core/JobSystem.h"
#include "luminumbra_common/world/GameSession.h"
#include "luminumbra_common/systems/SHIELD_WorldSystem.h"

namespace fs = std::filesystem;

namespace {

using Luminumbra::JobSystem;
using Luminumbra::Vec3;
using Luminumbra::world::GameSession;
using Luminumbra::Systems::SHIELD_WorldSystem;

#ifndef LUMINUMBRA_SOURCE_ROOT
#define LUMINUMBRA_SOURCE_ROOT "."
#endif

// Temp root containing the river-bearing `default` preset + the biome table the rivers need.
class HeadlessRoot {
public:
    HeadlessRoot() {
        root_ = fs::temp_directory_path() / "luminumbra_water_determinism_test";
        fs::remove_all(root_);
        const fs::path src(LUMINUMBRA_SOURCE_ROOT);
        fs::create_directories(root_ / "worlds" / "atlas" / "presets");
        fs::copy_file(src / "worlds" / "atlas" / "presets" / "default.json",
                      root_ / "worlds" / "atlas" / "presets" / "default.json");
        // The `default` preset has rivers_enabled + a biome table ("common/biomes.json"); copy it to
        // the locations the loader probes so the rivers/water actually generate.
        std::error_code ec;
        fs::create_directories(root_ / "data" / "common", ec);
        fs::create_directories(root_ / "common", ec);
        fs::copy_file(src / "data" / "common" / "biomes.json",
                      root_ / "data" / "common" / "biomes.json", ec);
        fs::copy_file(src / "data" / "common" / "biomes.json",
                      root_ / "common" / "biomes.json", ec);
    }
    ~HeadlessRoot() { std::error_code ec; fs::remove_all(root_, ec); }
    [[nodiscard]] std::string root_string() const {
        return root_.string() + static_cast<char>(fs::path::preferred_separator);
    }
private:
    fs::path root_;
};

// Tick a real streaming world for `ticks` ticks, moving the anchor +X each tick so it continuously
// streams NEW terrain (and the rivers/water on it). Returns the per-tick water-state hash sequence;
// `water_chunks_out` is the count of chunks carrying a water sim on the final tick.
std::vector<std::uint64_t> run_water_sequence(const std::string& root, int ticks,
                                              std::size_t& water_chunks_out,
                                              std::int64_t& max_depth_out,
                                              int& max_seam_wet_out) {
    JobSystem jobs;
    jobs.startup();
    std::vector<std::uint64_t> hashes;
    hashes.reserve(static_cast<std::size_t>(ticks));
    {
        GameSession session;
        session.SetJobSystem(&jobs);
        session.SetRootPath(root);
        EXPECT_TRUE(session.CreateWorld("WaterDet", "777", "default"));
        SHIELD_WorldSystem* world = session.GetWorldSystem();
        EXPECT_NE(world, nullptr);
        auto* physics = session.GetPhysicsSystem();
        const Vec3 spawn = session.GetMetadata().spawnPoint;
        for (int t = 0; t < ticks; ++t) {
            const float x = spawn.x + static_cast<float>(t) * 8.0f;  // ~8 m/tick traverse
            const float z = spawn.z;
            const Vec3 anchor(x, world->GetTerrainHeightAt(x, z) + 2.0f, z);
            world->update(session.GetRegistry(), {anchor}, physics);
            world->wait_for_streaming_jobs();
            // Spec 009 AC-2: the integer mass invariant (Σdepth change == Σsource − Σsink) must hold
            // every tick — any seam leak or non-deterministic rounding flips it.
            EXPECT_TRUE(world->debug_water_mass_ok())
                << "tick " << t << ": fixed-point water mass invariant VIOLATED (leak/rounding bug)";
            const auto wh = world->debug_water_state_hash();
            hashes.push_back(wh.hash);
            water_chunks_out = wh.water_chunks;
            const std::int64_t d = world->debug_max_water_depth_mm();
            if (d > max_depth_out) max_depth_out = d;
            // Spec 009 Phase 3: cross-chunk CONTINUITY — seam cell-pairs wet on both sides of a chunk border.
            const int sw = world->debug_water_seam_wet_pairs();
            if (sw > max_seam_wet_out) max_seam_wet_out = sw;
        }
    }
    jobs.shutdown();
    return hashes;
}

constexpr int kTicks = 64;

// Two independent runs with the same seed + same anchor path must produce the identical per-tick
// water-state hash sequence. (Synchronous water sim today → passes. An async integration whose
// timing varies run-to-run would diverge here.)
TEST(WaterDeterminism, LiveWaterSimIsRunReplayDeterministic) {
    const HeadlessRoot root;
    std::size_t water_chunks_a = 0, water_chunks_b = 0;
    std::int64_t max_depth_a = 0, max_depth_b = 0;
    int seam_wet_a = 0, seam_wet_b = 0;
    const auto seq_a = run_water_sequence(root.root_string(), kTicks, water_chunks_a, max_depth_a, seam_wet_a);
    const auto seq_b = run_water_sequence(root.root_string(), kTicks, water_chunks_b, max_depth_b, seam_wet_b);

    ASSERT_GT(water_chunks_a, 0u)
        << "no water chunks were simulated — the test is vacuous (no live water exercised); "
           "pick a wetter preset or a path that crosses rivers";
    ASSERT_EQ(seq_a.size(), seq_b.size());
    EXPECT_EQ(seq_a, seq_b)
        << "live water-sim state diverged between two identical runs — the water sim is "
           "NON-DETERMINISTIC (run!=replay / would desync host==peer)";
    // Spec 009 AC-1: the river-bearing world must carry FLOWING water — some cell holds depth > 0
    // (carved river channels filled from the deterministic sources; they render dry pre-Spec-009).
    EXPECT_GT(max_depth_a, 0)
        << "no cell ever held water depth > 0 — rivers never filled (sources/seeding broken)";
    EXPECT_EQ(max_depth_a, max_depth_b)
        << "max water depth differs run-to-run — fixed-point sim is non-deterministic";
    // Spec 009 Phase 3 — CROSS-CHUNK CONTINUITY: at some tick a river/lake spans a chunk seam with water
    // on BOTH sides (cross-chunk owner-edge flux working). Internal-edges-only (Slice-1) drains at the
    // seam and this stays 0. Deterministic run-to-run.
    EXPECT_GT(seam_wet_a, 0)
        << "no chunk-seam ever had water on both sides — cross-chunk flux not flowing (rivers pool at seams)";
    EXPECT_EQ(seam_wet_a, seam_wet_b)
        << "cross-chunk seam continuity differs run-to-run — non-deterministic";
}

// Spec 009 Phase 2 — TERRAFORM coupling. Tick a world to a water steady-state, then DIG (lower the
// integer water bed by 4 m over a 24 m region) mid-sim and tick again. Returns the pre/post water-state
// hash, whether the mass invariant held throughout, and the pre/post max depth.
struct DigResult {
    std::uint64_t pre_hash = 0, post_hash = 0;
    bool mass_ok = true;
    std::int64_t pre_max = 0, post_max = 0;
};

DigResult run_dig_drain(const std::string& root) {
    JobSystem jobs; jobs.startup();
    DigResult r;
    {
        GameSession session;
        session.SetJobSystem(&jobs);
        session.SetRootPath(root);
        EXPECT_TRUE(session.CreateWorld("WaterDig", "777", "default"));
        SHIELD_WorldSystem* world = session.GetWorldSystem();
        auto* physics = session.GetPhysicsSystem();
        const Vec3 spawn = session.GetMetadata().spawnPoint;
        const Vec3 anchor(spawn.x, world->GetTerrainHeightAt(spawn.x, spawn.z) + 2.0f, spawn.z);
        auto tick = [&]{ world->update(session.GetRegistry(), {anchor}, physics);
                         world->wait_for_streaming_jobs();
                         if (!world->debug_water_mass_ok()) r.mass_ok = false; };
        for (int t = 0; t < 80; ++t) tick();             // settle a water body
        r.pre_hash = world->debug_water_state_hash().hash;
        r.pre_max  = world->debug_max_water_depth_mm();
        world->EditTerrainBed(anchor, -4000, 24.0f);     // DIG: carve the bed down 4 m
        for (int t = 0; t < 80; ++t) tick();             // solver drains/pools into the new bed
        r.post_hash = world->debug_water_state_hash().hash;
        r.post_max  = world->debug_max_water_depth_mm();
    }
    jobs.shutdown();
    return r;
}

// The terraform bed edit must be DETERMINISTIC (lockstep-safe), MASS-CONSERVING, and the water must
// RESPOND to it — the core of "terraform land to drain/dam a river".
TEST(WaterDeterminism, TerraformBedEditIsDeterministicAndConserving) {
    const HeadlessRoot root;
    const DigResult a = run_dig_drain(root.root_string());
    const DigResult b = run_dig_drain(root.root_string());
    EXPECT_TRUE(a.mass_ok) << "the integer mass invariant was VIOLATED during/after the terraform edit";
    EXPECT_EQ(a.pre_hash, b.pre_hash)   << "pre-edit water state is non-deterministic";
    EXPECT_EQ(a.post_hash, b.post_hash) << "the terraform edit is NON-DETERMINISTIC (post-edit water diverged "
                                           "run-to-run — would desync host==peer)";
    EXPECT_NE(a.post_hash, a.pre_hash)  << "the bed edit had no effect on the water state";
    // The water RESPONDED: digging a deep pit reshapes the water field (drain/pool), so the deepest cell
    // changes. (Run-to-run identical, asserted above.)
    EXPECT_NE(a.post_max, a.pre_max)
        << "max water depth unchanged after digging a 4 m pit — the solver did not respond to the bed edit "
           "(pre=" << a.pre_max << "mm post=" << a.post_max << "mm)";
}

// Spec 009 Phase 2 — PLAYER VOXEL DIG. The headline player action: carve a sphere out of the
// actual VOXEL terrain (not just the water bed) mid-sim, which remeshes the terrain AND drains
// the water into the new pit. EditTerrainVoxel must edit >0 chunks, be run==replay deterministic
// (sdf_data is hashed+persisted; the water couple feeds the water-state hash), conserve mass, and
// the water must respond.
struct VoxelDigResult {
    int chunks_edited = 0;
    std::uint64_t pre_hash = 0, post_hash = 0;
    bool mass_ok = true;
    std::int64_t pre_max = 0, post_max = 0;
};

VoxelDigResult run_voxel_dig(const std::string& root) {
    JobSystem jobs; jobs.startup();
    VoxelDigResult r;
    {
        GameSession session;
        session.SetJobSystem(&jobs);
        session.SetRootPath(root);
        EXPECT_TRUE(session.CreateWorld("VoxelDig", "777", "default"));
        SHIELD_WorldSystem* world = session.GetWorldSystem();
        auto* physics = session.GetPhysicsSystem();
        const Vec3 spawn = session.GetMetadata().spawnPoint;
        const Vec3 anchor(spawn.x, world->GetTerrainHeightAt(spawn.x, spawn.z) + 2.0f, spawn.z);
        auto tick = [&]{ world->update(session.GetRegistry(), {anchor}, physics);
                         world->wait_for_streaming_jobs();
                         if (!world->debug_water_mass_ok()) r.mass_ok = false; };
        for (int t = 0; t < 80; ++t) tick();             // settle a water body
        r.pre_hash = world->debug_water_state_hash().hash;
        r.pre_max  = world->debug_max_water_depth_mm();
        // DIG a 4 m sphere centred just below the surface at spawn — carves voxel terrain + drains.
        const Vec3 dig_center(spawn.x, world->GetTerrainHeightAt(spawn.x, spawn.z) - 2.0f, spawn.z);
        r.chunks_edited = world->EditTerrainVoxel(dig_center, 4.0f, /*fill=*/false, physics);
        for (int t = 0; t < 80; ++t) tick();             // solver drains/pools into the carved pit
        r.post_hash = world->debug_water_state_hash().hash;
        r.post_max  = world->debug_max_water_depth_mm();
    }
    jobs.shutdown();
    return r;
}

TEST(WaterDeterminism, PlayerVoxelDigIsDeterministicCarvesTerrainAndDrains) {
    const HeadlessRoot root;
    const VoxelDigResult a = run_voxel_dig(root.root_string());
    const VoxelDigResult b = run_voxel_dig(root.root_string());
    EXPECT_GT(a.chunks_edited, 0)
        << "EditTerrainVoxel carved no chunks — the dig sphere never overlapped streamed voxel terrain";
    EXPECT_EQ(a.chunks_edited, b.chunks_edited) << "voxel-dig chunk count is non-deterministic";
    EXPECT_TRUE(a.mass_ok) << "the integer mass invariant was VIOLATED during/after the voxel dig";
    EXPECT_EQ(a.pre_hash, b.pre_hash)   << "pre-dig water state is non-deterministic";
    EXPECT_EQ(a.post_hash, b.post_hash) << "the voxel dig is NON-DETERMINISTIC (post-edit water diverged "
                                           "run-to-run — would desync host==peer)";
    EXPECT_NE(a.post_hash, a.pre_hash)  << "the voxel dig had no effect on the water state";
}

} // namespace
