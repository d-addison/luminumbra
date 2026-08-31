//  implementation note: LIVE-water determinism gate. The existing lockstep/persistence determinism
// gates run hand-built FIXTURE chunk states and NEVER tick a live WaterSystem, so a
// non-deterministic water-sim change (e.g. an async integration whose completion order depends on
// worker timing) would pass them all yet silently desync host==peer. This test ticks a REAL
// streaming world with active rivers, moving the anchor so new water chunks stream + init +
// simulate, and asserts two independent runs (same seed + same anchor path) produce an identical
// per-tick water-state hash sequence — the run==replay / host==peer property for live water. Prereq
// before amortizing the water-sim wait.
#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

#include "luminumbra_common/components/CoreComponents.h" // water-source: TransformComponent
#include "luminumbra_common/core/JobSystem.h"
#include "luminumbra_common/core/WaterComponents.h" // water-source: WaterSourceComponent
#include "luminumbra_common/systems/SHIELD_WorldSystem.h"
#include "luminumbra_common/world/GameSession.h"

namespace fs = std::filesystem;

namespace {

using Luminumbra::JobSystem;
using Luminumbra::Vec3;
using Luminumbra::Systems::SHIELD_WorldSystem;
using Luminumbra::world::GameSession;

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
        // The `default` preset has rivers_enabled + a biome table ("common/biomes.json"); copy it
        // to the locations the loader probes so the rivers/water actually generate.
        std::error_code ec;
        fs::create_directories(root_ / "data" / "common", ec);
        fs::create_directories(root_ / "common", ec);
        fs::copy_file(
            src / "data" / "common" / "biomes.json", root_ / "data" / "common" / "biomes.json", ec);
        fs::copy_file(
            src / "data" / "common" / "biomes.json", root_ / "common" / "biomes.json", ec);
    }
    ~HeadlessRoot() {
        std::error_code ec;
        fs::remove_all(root_, ec);
    }
    [[nodiscard]] std::string root_string() const {
        return root_.string() + static_cast<char>(fs::path::preferred_separator);
    }

private:
    fs::path root_;
};

// Tick a real streaming world for `ticks` ticks, moving the anchor +X each tick so it continuously
// streams NEW terrain (and the rivers/water on it). Returns the per-tick water-state hash sequence;
// `water_chunks_out` is the count of chunks carrying a water sim on the final tick.
std::vector<std::uint64_t> run_water_sequence(const std::string& root,
                                              int ticks,
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
            const float x = spawn.x + static_cast<float>(t) * 8.0f; // ~8 m/tick traverse
            const float z = spawn.z;
            const Vec3 anchor(x, world->GetTerrainHeightAt(x, z) + 2.0f, z);
            world->update(session.GetRegistry(), {anchor}, physics);
            world->wait_for_streaming_jobs();
            // the integer mass invariant (Σdepth change == Σsource − Σsink) must hold
            // every tick — any seam leak or non-deterministic rounding flips it.
            EXPECT_TRUE(world->debug_water_mass_ok())
                << "tick " << t
                << ": fixed-point water mass invariant VIOLATED (leak/rounding bug)";
            const auto wh = world->debug_water_state_hash();
            hashes.push_back(wh.hash);
            water_chunks_out = wh.water_chunks;
            const std::int64_t d = world->debug_max_water_depth_mm();
            if (d > max_depth_out)
                max_depth_out = d;
            //  cross-chunk CONTINUITY — seam cell-pairs wet on both sides of a chunk border.
            const int sw = world->debug_water_seam_wet_pairs();
            if (sw > max_seam_wet_out)
                max_seam_wet_out = sw;
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
    const auto seq_a =
        run_water_sequence(root.root_string(), kTicks, water_chunks_a, max_depth_a, seam_wet_a);
    const auto seq_b =
        run_water_sequence(root.root_string(), kTicks, water_chunks_b, max_depth_b, seam_wet_b);

    ASSERT_GT(water_chunks_a, 0u)
        << "no water chunks were simulated — the test is vacuous (no live water exercised); "
           "pick a wetter preset or a path that crosses rivers";
    ASSERT_EQ(seq_a.size(), seq_b.size());
    EXPECT_EQ(seq_a, seq_b)
        << "live water-sim state diverged between two identical runs — the water sim is "
           "NON-DETERMINISTIC (run!=replay / would desync host==peer)";
    // the river-bearing world must carry FLOWING water — some cell holds depth > 0
    // (carved river channels filled from the deterministic sources; they render dry pre-).
    EXPECT_GT(max_depth_a, 0)
        << "no cell ever held water depth > 0 — rivers never filled (sources/seeding broken)";
    EXPECT_EQ(max_depth_a, max_depth_b)
        << "max water depth differs run-to-run — fixed-point sim is non-deterministic";
    // CROSS-CHUNK CONTINUITY: at some tick a river/lake spans a chunk seam with water
    // on BOTH sides (cross-chunk owner-edge flux working). Internal-edges-only drains at the
    // seam and this stays 0. Deterministic run-to-run.
    EXPECT_GT(seam_wet_a, 0) << "no chunk-seam ever had water on both sides — cross-chunk flux not "
                                "flowing (rivers pool at seams)";
    EXPECT_EQ(seam_wet_a, seam_wet_b)
        << "cross-chunk seam continuity differs run-to-run — non-deterministic";
}

// TERRAFORM coupling. Tick a world to a water steady-state, then DIG (lower the
// integer water bed by 4 m over a 24 m region) mid-sim and tick again. Returns the pre/post
// water-state hash, whether the mass invariant held throughout, and the pre/post max depth.
struct DigResult {
    std::uint64_t pre_hash = 0, post_hash = 0;
    bool mass_ok = true;
    std::int64_t pre_max = 0, post_max = 0;
};

DigResult run_dig_drain(const std::string& root) {
    JobSystem jobs;
    jobs.startup();
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
        auto tick = [&] {
            world->update(session.GetRegistry(), {anchor}, physics);
            world->wait_for_streaming_jobs();
            if (!world->debug_water_mass_ok())
                r.mass_ok = false;
        };
        for (int t = 0; t < 80; ++t)
            tick(); // settle a water body
        r.pre_hash = world->debug_water_state_hash().hash;
        r.pre_max = world->debug_max_water_depth_mm();
        world->EditTerrainBed(anchor, -4000, 24.0f); // DIG: carve the bed down 4 m
        for (int t = 0; t < 80; ++t)
            tick(); // solver drains/pools into the new bed
        r.post_hash = world->debug_water_state_hash().hash;
        r.post_max = world->debug_max_water_depth_mm();
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
    EXPECT_TRUE(a.mass_ok)
        << "the integer mass invariant was VIOLATED during/after the terraform edit";
    EXPECT_EQ(a.pre_hash, b.pre_hash) << "pre-edit water state is non-deterministic";
    EXPECT_EQ(a.post_hash, b.post_hash)
        << "the terraform edit is NON-DETERMINISTIC (post-edit water diverged "
           "run-to-run — would desync host==peer)";
    EXPECT_NE(a.post_hash, a.pre_hash) << "the bed edit had no effect on the water state";
    // The water RESPONDED: digging a deep pit reshapes the water field (drain/pool), so the deepest
    // cell changes. (Run-to-run identical, asserted above.)
    EXPECT_NE(a.post_max, a.pre_max) << "max water depth unchanged after digging a 4 m pit — the "
                                        "solver did not respond to the bed edit "
                                        "(pre="
                                     << a.pre_max << "mm post=" << a.post_max << "mm)";
}

//  ( dam regression) — the DAM complement of the dig test. RAISE the integer water bed
// (+4 m over a 24 m region) under a settled water body: the displaced water must go SOMEWHERE
// (mass invariant), the field must respond (pool against the new dam / spread), and the whole
// interaction must be run==replay deterministic — "terraform land to dam a river", the second
// half of the  water-interaction contract that only had its dig half gated.
DigResult run_dam_pool(const std::string& root) {
    JobSystem jobs;
    jobs.startup();
    DigResult r;
    {
        GameSession session;
        session.SetJobSystem(&jobs);
        session.SetRootPath(root);
        EXPECT_TRUE(session.CreateWorld("WaterDam", "777", "default"));
        SHIELD_WorldSystem* world = session.GetWorldSystem();
        auto* physics = session.GetPhysicsSystem();
        const Vec3 spawn = session.GetMetadata().spawnPoint;
        const Vec3 anchor(spawn.x, world->GetTerrainHeightAt(spawn.x, spawn.z) + 2.0f, spawn.z);
        auto tick = [&] {
            world->update(session.GetRegistry(), {anchor}, physics);
            world->wait_for_streaming_jobs();
            if (!world->debug_water_mass_ok())
                r.mass_ok = false;
        };
        for (int t = 0; t < 80; ++t)
            tick(); // settle a water body
        r.pre_hash = world->debug_water_state_hash().hash;
        r.pre_max = world->debug_max_water_depth_mm();
        world->EditTerrainBed(anchor, +4000, 24.0f); // DAM: raise the bed 4 m
        for (int t = 0; t < 80; ++t)
            tick(); // displaced water pools/spreads
        r.post_hash = world->debug_water_state_hash().hash;
        r.post_max = world->debug_max_water_depth_mm();
    }
    jobs.shutdown();
    return r;
}

TEST(WaterDeterminism, DamBedEditPoolsWaterDeterministically) {
    const HeadlessRoot root;
    const DigResult a = run_dam_pool(root.root_string());
    const DigResult b = run_dam_pool(root.root_string());
    EXPECT_TRUE(a.mass_ok) << "the integer mass invariant was VIOLATED during/after the dam edit";
    EXPECT_EQ(a.pre_hash, b.pre_hash) << "pre-edit water state is non-deterministic";
    EXPECT_EQ(a.post_hash, b.post_hash)
        << "the dam edit is NON-DETERMINISTIC (post-edit water diverged "
           "run-to-run — would desync host==peer)";
    EXPECT_EQ(a.post_max, b.post_max) << "post-dam max depth differs run-to-run";
    EXPECT_NE(a.post_hash, a.pre_hash) << "the dam edit had no effect on the water state";
    // The water RESPONDED to the dam: raising a 4 m bed under a wet region displaces its water
    // (mass held above), reshaping the depth field — the deepest cell changes.
    EXPECT_NE(a.post_max, a.pre_max)
        << "max water depth unchanged after raising a 4 m dam — the solver did not respond "
           "(pre="
        << a.pre_max << "mm post=" << a.post_max << "mm)";
}

//  ( == ): WEATHER-DRIVEN rain determinism. With finite hydrology +
// the weather system wired as the rain source (per-cell PrecipitationAt, integer-
// quantized AT THE BOUNDARY), two identical runs must produce identical per-tick
// water-state hash sequences, and the rain must actually LAND (the rained world's
// final state differs from an unrained control). The weather state here is the boot
// state (this harness ticks the world system directly, not TickSimulation) — constant
// but non-trivial precipitation, which is exactly what the quantization + coupling
// determinism claim needs.
struct RainResult {
    std::vector<std::uint64_t> hashes;
    bool mass_ok = true;
    std::int64_t final_max = 0;
};

RainResult run_weather_rain(const std::string& root, bool wire_weather) {
    JobSystem jobs;
    jobs.startup();
    RainResult r;
    {
        GameSession session;
        session.SetJobSystem(&jobs);
        session.SetRootPath(root);
        // The wiring must be REQUESTED pre-world (the session wires it at create,
        // mirroring the sim.hydrology_weather production path).
        session.SetWeatherRainEnabled(wire_weather, /*scale_mm=*/40);
        EXPECT_TRUE(session.CreateWorld("WeatherRain", "777", "default"));
        SHIELD_WorldSystem* world = session.GetWorldSystem();
        auto* physics = session.GetPhysicsSystem();
        // Finite hydrology: rain is the water source ( semantics).
        world->SetWaterHydrology(/*finite=*/true, /*rain=*/0, /*evap=*/1);
        const Vec3 spawn = session.GetMetadata().spawnPoint;
        const Vec3 anchor(spawn.x, world->GetTerrainHeightAt(spawn.x, spawn.z) + 2.0f, spawn.z);
        for (int t = 0; t < 48; ++t) {
            // Advance the weather deterministically (a pure function of seed + tick +
            // anchor) so storm cells/precip exist — this harness bypasses
            // TickSimulation, which normally does this before water each tick.
            if (auto* weather = session.GetWeatherSystem()) {
                weather->Update(
                    static_cast<std::uint64_t>(t), anchor, session.GetWindFieldSystem());
            }
            world->update(session.GetRegistry(), {anchor}, physics);
            world->wait_for_streaming_jobs();
            if (!world->debug_water_mass_ok())
                r.mass_ok = false;
            r.hashes.push_back(world->debug_water_state_hash().hash);
        }
        r.final_max = world->debug_max_water_depth_mm();
    }
    jobs.shutdown();
    return r;
}

TEST(WaterDeterminism, WeatherDrivenRainIsDeterministic) {
    const HeadlessRoot root;
    const RainResult a = run_weather_rain(root.root_string(), true);
    const RainResult b = run_weather_rain(root.root_string(), true);
    const RainResult control = run_weather_rain(root.root_string(), false);
    EXPECT_TRUE(a.mass_ok) << "mass invariant VIOLATED under weather-driven rain";
    ASSERT_EQ(a.hashes.size(), b.hashes.size());
    EXPECT_EQ(a.hashes, b.hashes)
        << "weather-driven rain diverged between two identical runs — the coupling is "
           "NON-DETERMINISTIC (float leak past the quantization boundary?)";
    // The rain LANDED: the rained world's final water state differs from the
    // unrained finite-hydrology control (which only evaporates).
    ASSERT_FALSE(a.hashes.empty());
    EXPECT_NE(a.hashes.back(), control.hashes.back())
        << "wiring the weather rain changed NOTHING — the coupling is not landing water";
}

// water-source: SOURCE INJECTION routes through the fixed-point mm domain.
// The proof leans on derived-state reclassification: the water hash covers ONLY the mm truth (the
// float mirrors are Render-classified) — so if the WaterSourceComponent injection still wrote only
// the float mirror (the pre-water-source bug), the sourced world's hash would EQUAL the control's
// and this test would fail. Deterministic: two sourced runs match.
struct SourceResult {
    std::vector<std::uint64_t> hashes;
    bool mass_ok = true;
    bool found_water = false;
    std::int64_t sources_seen = 0;
    std::int64_t injected_mm = 0;
};

SourceResult run_with_source(const std::string& root, bool add_source) {
    JobSystem jobs;
    jobs.startup();
    SourceResult r;
    {
        GameSession session;
        session.SetJobSystem(&jobs);
        session.SetRootPath(root);
        EXPECT_TRUE(session.CreateWorld("WaterSrc", "777", "default"));
        SHIELD_WorldSystem* world = session.GetWorldSystem();
        auto* physics = session.GetPhysicsSystem();
        const Vec3 spawn = session.GetMetadata().spawnPoint;
        const Vec3 anchor(spawn.x, world->GetTerrainHeightAt(spawn.x, spawn.z) + 2.0f, spawn.z);
        auto tick = [&] {
            world->update(session.GetRegistry(), {anchor}, physics);
            world->wait_for_streaming_jobs();
            if (!world->debug_water_mass_ok())
                r.mass_ok = false;
        };
        for (int t = 0; t < 24; ++t)
            tick(); // let the spawn area's water grids init
        // STAGING (empirically derived — three failed attempts taught this):
        // sources inject ONLY into gridded chunks, and a spring dropped in the
        // river's SINK band is clamped back to equilibrium within the same tick
        // (neutral-state boundary conditions — zero post-tick trace, hash-invisible).
        // So: find a GRIDDED position (any wet cell qualifies), DIG A PIT a few
        // metres off the wet cell in BOTH runs (dug pits verifiably hold water —
        // the dig gate), and spring INTO the pit only in the sourced run. The
        // pit is not a sink; the spring's water accumulates there.
        // The gridded (river) chunks sit ~128-176 m from spawn in this preset
        // (empirical: chunk x = +-8..11) — scan chunk-granular out to +-12 chunks.
        float wet_x = spawn.x, wet_z = spawn.z;
        for (int gz = -12; gz <= 12 && !r.found_water; ++gz) {
            for (int gx = -12; gx <= 12 && !r.found_water; ++gx) {
                const float px = spawn.x + static_cast<float>(gx) * 16.0f;
                const float pz = spawn.z + static_cast<float>(gz) * 16.0f;
                if (world->debug_water_grid_at(px, pz)) {
                    wet_x = px;
                    wet_z = pz;
                    r.found_water = true;
                }
            }
        }
        const Vec3 pit(wet_x + 6.0f, 0.0f, wet_z + 6.0f);
        world->EditTerrainBed(pit, -4000, 5.0f); // both runs dig the same pit
        if (add_source) {
            auto& reg = session.GetRegistry();
            const auto e = reg.create();
            auto& tf = reg.emplace<Luminumbra::Components::TransformComponent>(e);
            tf.position = Vec3(pit.x, world->GetTerrainHeightAt(pit.x, pit.z), pit.z);
            auto& src = reg.emplace<Luminumbra::Components::WaterSourceComponent>(e);
            src.flow_rate = 40.0f; // strong spring: >= 1 mm/cell/tick after quantization
        }
        for (int t = 0; t < 24; ++t) {
            tick();
            r.hashes.push_back(world->debug_water_state_hash().hash);
        }
        r.sources_seen = world->debug_water_sources_seen();
        r.injected_mm = world->debug_water_source_injected_mm();
    }
    jobs.shutdown();
    return r;
}

TEST(WaterDeterminism, SourceInjectionRoutesThroughFixedPoint) {
    const HeadlessRoot root;
    const SourceResult a = run_with_source(root.root_string(), true);
    const SourceResult b = run_with_source(root.root_string(), true);
    const SourceResult control = run_with_source(root.root_string(), false);
    ASSERT_TRUE(a.found_water)
        << "no GRIDDED chunk within 80 m of spawn (vacuous test; widen the scan)";
    EXPECT_TRUE(a.mass_ok);
    ASSERT_EQ(a.hashes.size(), b.hashes.size());
    EXPECT_EQ(a.hashes, b.hashes) << "source injection is NON-DETERMINISTIC across identical runs";
    ASSERT_FALSE(a.hashes.empty());
    // Bisect diagnostics: the loop must SEE the springs, and mm must LAND.
    EXPECT_GT(a.sources_seen, 0)
        << "the injection loop never saw the source entities (view/registry wiring)";
    EXPECT_GT(a.injected_mm, 0) << "sources seen (" << a.sources_seen
                                << ") but ZERO mm injected — the chunk "
                                   "lookup / grid guard / quantization is eating the spring";
    // Compare the FULL sequences: even if a sink eventually re-equilibrates the
    // body, SOME post-tick state along the way must differ once mm water landed.
    EXPECT_NE(a.hashes, control.hashes)
        << "the WaterSourceComponent injection did not reach the HASHED mm truth at "
           "ANY tick — it is writing the float mirror only (the pre-water-source bug)";
}

// PLAYER VOXEL DIG. The headline player action: carve a sphere out of the
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
    JobSystem jobs;
    jobs.startup();
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
        auto tick = [&] {
            world->update(session.GetRegistry(), {anchor}, physics);
            world->wait_for_streaming_jobs();
            if (!world->debug_water_mass_ok())
                r.mass_ok = false;
        };
        for (int t = 0; t < 80; ++t)
            tick(); // settle a water body
        r.pre_hash = world->debug_water_state_hash().hash;
        r.pre_max = world->debug_max_water_depth_mm();
        // DIG a 4 m sphere centred just below the surface at spawn — carves voxel terrain + drains.
        const Vec3 dig_center(spawn.x, world->GetTerrainHeightAt(spawn.x, spawn.z) - 2.0f, spawn.z);
        r.chunks_edited = world->EditTerrainVoxel(dig_center, 4.0f, /*fill=*/false, physics);
        for (int t = 0; t < 80; ++t)
            tick(); // solver drains/pools into the carved pit
        r.post_hash = world->debug_water_state_hash().hash;
        r.post_max = world->debug_max_water_depth_mm();
    }
    jobs.shutdown();
    return r;
}

TEST(WaterDeterminism, PlayerVoxelDigIsDeterministicCarvesTerrainAndDrains) {
    const HeadlessRoot root;
    const VoxelDigResult a = run_voxel_dig(root.root_string());
    const VoxelDigResult b = run_voxel_dig(root.root_string());
    EXPECT_GT(a.chunks_edited, 0) << "EditTerrainVoxel carved no chunks — the dig sphere never "
                                     "overlapped streamed voxel terrain";
    EXPECT_EQ(a.chunks_edited, b.chunks_edited) << "voxel-dig chunk count is non-deterministic";
    EXPECT_TRUE(a.mass_ok) << "the integer mass invariant was VIOLATED during/after the voxel dig";
    EXPECT_EQ(a.pre_hash, b.pre_hash) << "pre-dig water state is non-deterministic";
    EXPECT_EQ(a.post_hash, b.post_hash)
        << "the voxel dig is NON-DETERMINISTIC (post-edit water diverged "
           "run-to-run — would desync host==peer)";
    EXPECT_NE(a.post_hash, a.pre_hash) << "the voxel dig had no effect on the water state";
}

//  FINITE HYDROLOGY — rain-fed, drainable, conserved water. Tick a world with the perpetual
// river source REMOVED and a deterministic rain input, and confirm the water state is run==replay
// identical, mass-conserving, and that rain actually accumulates (max depth grows).
struct HydroResult {
    std::vector<std::uint64_t> hashes;
    std::int64_t land_water =
        0; // standing water ON LAND (excludes the sea) — the rain ground-truth
    bool mass_ok = true;
};

HydroResult
run_rain_hydrology(const std::string& root, int ticks, std::int32_t rain_mm, std::int32_t evap_mm) {
    JobSystem jobs;
    jobs.startup();
    HydroResult r;
    {
        GameSession session;
        session.SetJobSystem(&jobs);
        session.SetRootPath(root);
        EXPECT_TRUE(session.CreateWorld("Hydro", "777", "default"));
        SHIELD_WorldSystem* world = session.GetWorldSystem();
        auto* physics = session.GetPhysicsSystem();
        world->SetWaterHydrology(
            /*finite=*/true, rain_mm, evap_mm); // no source, rain on, optional evap
        const Vec3 spawn = session.GetMetadata().spawnPoint;
        const Vec3 anchor(spawn.x, world->GetTerrainHeightAt(spawn.x, spawn.z) + 2.0f, spawn.z);
        for (int t = 0; t < ticks; ++t) {
            world->update(session.GetRegistry(), {anchor}, physics);
            world->wait_for_streaming_jobs();
            if (!world->debug_water_mass_ok())
                r.mass_ok = false;
            r.hashes.push_back(world->debug_water_state_hash().hash);
        }
        r.land_water = world->debug_land_water_volume_mm();
    }
    jobs.shutdown();
    return r;
}

// Finite hydrology must be deterministic AND rain must genuinely accumulate ON LAND (not just leave
// the pre-existing sea, which a max-depth probe can't distinguish). Compare a rain run to a no-rain
// run on the identical world: the rain run must hold strictly more land water, and be run==replay
// identical.
TEST(WaterDeterminism, FiniteHydrologyRainFillsLandDeterministically) {
    const HeadlessRoot root;
    const HydroResult rain_a = run_rain_hydrology(root.root_string(), 64, /*rain=*/30, /*evap=*/0);
    const HydroResult rain_b = run_rain_hydrology(root.root_string(), 64, /*rain=*/30, /*evap=*/0);
    const HydroResult dry = run_rain_hydrology(root.root_string(), 64, /*rain=*/0, /*evap=*/0);
    EXPECT_TRUE(rain_a.mass_ok)
        << "finite-hydrology mass invariant VIOLATED (rain/evap accounting bug)";
    EXPECT_EQ(rain_a.hashes, rain_b.hashes)
        << "finite-hydrology water state diverged run-to-run — rain path is NON-DETERMINISTIC";
    EXPECT_EQ(rain_a.land_water, rain_b.land_water)
        << "land water differs run-to-run — non-deterministic";
    EXPECT_GT(rain_a.land_water, dry.land_water)
        << "rain did NOT add water on land (rain=" << rain_a.land_water
        << " vs dry=" << dry.land_water
        << ") — the rainfall input is not accumulating in the terrain";
}

} // namespace
