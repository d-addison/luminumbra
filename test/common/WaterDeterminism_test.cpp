// Live-water determinism coverage. The lockstep and persistence tests use hand-built chunk states
// and do not tick a live WaterSystem, so a
// non-deterministic water-sim change (e.g. an async integration whose completion order depends on
// worker timing) would pass them all yet silently desync host==peer. This test ticks a REAL
// streaming world with active rivers, moving the anchor so new water chunks stream + init +
// simulate, and asserts two independent runs (same seed + same anchor path) produce an identical
// per-tick water-state hash sequence — the run==replay / host==peer property for live water.
//
// Streaming is CAPPED (debug_set_streaming_radius_cap) to a small disc around a
// deterministically chosen wet anchor. Uncapped, every tick pays the full
// RENDER_DISTANCE fill (a ~1,800-column full-detail wanted set that never
// finishes inside the test window) behind wait_for_streaming_jobs — ~7 s/tick on
// slow hosted runners, which is what kept timing these gates out on Windows CI.
// The cap does not weaken any gate: every comparison here is A/B (or A/control)
// within a run over a deterministic bounded region — there are no golden hashes —
// and the non-vacuity guards (water_chunks / max_depth / seam_wet / found_water /
// land_water) still prove real water was exercised.
#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

#include "luminumbra_common/components/CoreComponents.h" // water-source: TransformComponent
#include "luminumbra_common/core/JobSystem.h"
#include "luminumbra_common/core/SystemConfig.h"    // sim.water_high_res gate
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
// The root is unique per fixture instance so concurrent common_tests processes never share
// (or clobber) a directory; each TEST builds ONE instance and reuses it for its A/B runs, so
// within-run path stability still holds. The destructor removes the tree.
class HeadlessRoot {
public:
    HeadlessRoot() {
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        root_ = fs::temp_directory_path() /
                ("luminumbra_water_determinism_test_" + std::to_string(stamp));
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

// Streaming cap for these gates: big enough to hold a wet body + its seams + the
// surrounding land inside the resident disc, small enough that the disc
// (~13x13 columns, all full-detail) generates in a couple of activation passes
// instead of the ~1,800-column RENDER_DISTANCE fill that dominated every tick.
constexpr int kStreamRadiusCap = 6; // chunks (96 m)

// Deterministic wet anchor: the nearest point to spawn where the PURE worldgen
// samplers say standing water exists at init (WaterLevelAt > terrain) — sea or a
// lake basin; river channels start dry and fill from sources. Pure functions of
// seed/preset: no streaming, no residency, identical in every run. Square-ring
// scan outward in 8 m steps to +-192 m, first hit wins; falls back to spawn if
// everything in range is dry (the non-vacuity asserts are the tripwire).
Vec3 find_wet_anchor(const SHIELD_WorldSystem* world, const Vec3& spawn) {
    constexpr float kStep = 8.0f;
    constexpr int kMaxRing = 24; // 24 * 8 m = 192 m
    for (int ring = 0; ring <= kMaxRing; ++ring) {
        for (int dz = -ring; dz <= ring; ++dz) {
            for (int dx = -ring; dx <= ring; ++dx) {
                if (std::max(std::abs(dx), std::abs(dz)) != ring)
                    continue; // perimeter only — nearest ring first
                const float x = spawn.x + static_cast<float>(dx) * kStep;
                const float z = spawn.z + static_cast<float>(dz) * kStep;
                const float terrain = world->GetTerrainHeightAt(x, z);
                if (world->WaterLevelAt(x, z) > terrain + 0.25f) {
                    return Vec3(x, terrain + 2.0f, z);
                }
            }
        }
    }
    return Vec3(spawn.x, world->GetTerrainHeightAt(spawn.x, spawn.z) + 2.0f, spawn.z);
}

// Tick a real streaming world for `ticks` ticks, moving the anchor +X each tick so it continuously
// streams NEW terrain (and the rivers/water on it). Returns the per-tick water-state hash sequence;
// `water_chunks_out` is the count of chunks carrying a water sim on the final tick.
std::vector<std::uint64_t> run_water_sequence(const std::string& root,
                                              int ticks,
                                              std::size_t& water_chunks_out,
                                              std::int64_t& max_depth_out,
                                              int& max_seam_wet_out,
                                              bool high_res = false) {
    JobSystem jobs;
    jobs.startup();
    std::vector<std::uint64_t> hashes;
    hashes.reserve(static_cast<std::size_t>(ticks));
    {
        GameSession session;
        session.SetJobSystem(&jobs);
        session.SetRootPath(root);
        // High-resolution water must be REQUESTED pre-world (the session wires it at
        // create, mirroring the sim.water_high_res production path). Default false is
        // a stored no-op — the historical scenarios are untouched.
        session.SetWaterHighResEnabled(high_res);
        EXPECT_TRUE(session.CreateWorld("WaterDet", "777", "default"));
        SHIELD_WorldSystem* world = session.GetWorldSystem();
        EXPECT_NE(world, nullptr);
        world->debug_set_streaming_radius_cap(kStreamRadiusCap);
        auto* physics = session.GetPhysicsSystem();
        const Vec3 spawn = session.GetMetadata().spawnPoint;
        // Start the traverse AT the wet body so the capped disc carries water (and
        // its seams) from the first ticks; the +8 m/tick move still streams, inits
        // and simulates fresh water chunks every activation.
        const Vec3 start = find_wet_anchor(world, spawn);
        for (int t = 0; t < ticks; ++t) {
            const float x = start.x + static_cast<float>(t) * 8.0f; // ~8 m/tick traverse
            const float z = start.z;
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
constexpr int kWeatherRainTicks = 32;
constexpr int kSourceWarmupTicks = 24;
constexpr int kSourceObservationTicks = 8;
constexpr int kFiniteHydrologyTicks = 32;

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
    // (carved river channels filled from deterministic sources).
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
        world->debug_set_streaming_radius_cap(kStreamRadiusCap);
        auto* physics = session.GetPhysicsSystem();
        const Vec3 spawn = session.GetMetadata().spawnPoint;
        const Vec3 anchor = find_wet_anchor(world, spawn); // dig at the wet body's edge
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
    // The full state hash proves the field responded. A global maximum can
    // legitimately remain unchanged when an unrelated cell is already the
    // deepest point, so it is diagnostic rather than a correctness oracle.
    EXPECT_EQ(a.post_max, b.post_max) << "post-dig max depth differs run-to-run";
}

// Dam complement of the dig test. Raise the integer water bed
// (+4 m over a 24 m region) under a settled water body: the displaced water must go SOMEWHERE
// (mass invariant), the field must respond (pool against the new dam / spread), and the whole
// interaction must be run==replay deterministic — "terraform land to dam a river", the second
// half of the water-interaction contract.
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
        world->debug_set_streaming_radius_cap(kStreamRadiusCap);
        auto* physics = session.GetPhysicsSystem();
        const Vec3 spawn = session.GetMetadata().spawnPoint;
        const Vec3 anchor = find_wet_anchor(world, spawn); // dam at the wet body's edge
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
    // The changed full-state hash above is the non-vacuous response proof; the
    // maximum alone may be owned by an unaffected cell elsewhere.
}

// Weather-driven rain determinism. With finite hydrology and
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
        world->debug_set_streaming_radius_cap(kStreamRadiusCap);
        auto* physics = session.GetPhysicsSystem();
        // Finite hydrology: rain is the water source ( semantics).
        world->SetWaterHydrology(/*finite=*/true, /*rain=*/0, /*evap=*/1);
        const Vec3 spawn = session.GetMetadata().spawnPoint;
        const Vec3 anchor = find_wet_anchor(world, spawn);
        for (int t = 0; t < kWeatherRainTicks; ++t) {
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
    ASSERT_EQ(a.hashes.size(), static_cast<std::size_t>(kWeatherRainTicks));
    ASSERT_EQ(a.hashes.size(), b.hashes.size());
    ASSERT_EQ(a.hashes.size(), control.hashes.size());
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
        world->debug_set_streaming_radius_cap(kStreamRadiusCap);
        auto* physics = session.GetPhysicsSystem();
        const Vec3 spawn = session.GetMetadata().spawnPoint;
        const Vec3 anchor = find_wet_anchor(world, spawn);
        auto tick = [&] {
            world->update(session.GetRegistry(), {anchor}, physics);
            world->wait_for_streaming_jobs();
            if (!world->debug_water_mass_ok())
                r.mass_ok = false;
        };
        for (int t = 0; t < kSourceWarmupTicks; ++t)
            tick(); // let the spawn area's water grids init
        // Sources inject only into gridded chunks, and a spring dropped in the
        // river's SINK band is clamped back to equilibrium within the same tick
        // (neutral-state boundary conditions — zero post-tick trace, hash-invisible).
        // So: find a GRIDDED position (any wet cell qualifies), DIG A PIT a few
        // metres off the wet cell in BOTH runs (dug pits verifiably hold water —
        // the dig gate), and spring INTO the pit only in the sourced run. The
        // pit is not a sink; the spring's water accumulates there.
        // The anchor sits at the wet body, so scan chunk-granular around IT out
        // to the streaming cap — everything further is not resident.
        float wet_x = anchor.x, wet_z = anchor.z;
        for (int gz = -kStreamRadiusCap; gz <= kStreamRadiusCap && !r.found_water; ++gz) {
            for (int gx = -kStreamRadiusCap; gx <= kStreamRadiusCap && !r.found_water; ++gx) {
                const float px = anchor.x + static_cast<float>(gx) * 16.0f;
                const float pz = anchor.z + static_cast<float>(gz) * 16.0f;
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
        for (int t = 0; t < kSourceObservationTicks; ++t) {
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
    ASSERT_EQ(a.hashes.size(), static_cast<std::size_t>(kSourceObservationTicks));
    ASSERT_EQ(a.hashes.size(), b.hashes.size());
    ASSERT_EQ(a.hashes.size(), control.hashes.size());
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
        world->debug_set_streaming_radius_cap(kStreamRadiusCap);
        auto* physics = session.GetPhysicsSystem();
        const Vec3 spawn = session.GetMetadata().spawnPoint;
        const Vec3 anchor = find_wet_anchor(world, spawn);
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
        // DIG a 4 m sphere centred just below the surface beside the wet body —
        // carves voxel terrain + verifiably drains real adjacent water.
        const Vec3 dig_center(
            anchor.x, world->GetTerrainHeightAt(anchor.x, anchor.z) - 2.0f, anchor.z);
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

// Finite hydrology — rain-fed, drainable, conserved water. Tick a world with the perpetual
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
        world->debug_set_streaming_radius_cap(kStreamRadiusCap);
        auto* physics = session.GetPhysicsSystem();
        world->SetWaterHydrology(
            /*finite=*/true, rain_mm, evap_mm); // no source, rain on, optional evap
        const Vec3 spawn = session.GetMetadata().spawnPoint;
        // Wet anchor: the nearest shoreline keeps both water AND land in the
        // capped disc (the land-water probe needs land cells to rain on).
        const Vec3 anchor = find_wet_anchor(world, spawn);
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
    const HydroResult rain_a =
        run_rain_hydrology(root.root_string(), kFiniteHydrologyTicks, /*rain=*/30, /*evap=*/0);
    const HydroResult rain_b =
        run_rain_hydrology(root.root_string(), kFiniteHydrologyTicks, /*rain=*/30, /*evap=*/0);
    const HydroResult dry =
        run_rain_hydrology(root.root_string(), kFiniteHydrologyTicks, /*rain=*/0, /*evap=*/0);
    EXPECT_TRUE(rain_a.mass_ok)
        << "finite-hydrology mass invariant VIOLATED (rain/evap accounting bug)";
    ASSERT_EQ(rain_a.hashes.size(), static_cast<std::size_t>(kFiniteHydrologyTicks));
    ASSERT_EQ(rain_a.hashes.size(), rain_b.hashes.size());
    ASSERT_EQ(rain_a.hashes.size(), dry.hashes.size());
    EXPECT_EQ(rain_a.hashes, rain_b.hashes)
        << "finite-hydrology water state diverged run-to-run — rain path is NON-DETERMINISTIC";
    EXPECT_EQ(rain_a.land_water, rain_b.land_water)
        << "land water differs run-to-run — non-deterministic";
    EXPECT_GT(rain_a.land_water, dry.land_water)
        << "rain did NOT add water on land (rain=" << rain_a.land_water
        << " vs dry=" << dry.land_water
        << ") — the rainfall input is not accumulating in the terrain";
}

// gated High-resolution water (sim.water_high_res). With the flag ON the session
// runs the 16x16 (1 m cell) grid: A/B determinism, the mass invariant and the
// non-vacuity guards must all hold exactly as at Medium, the hashed state must
// actually differ from a Medium run (the flag is not a no-op), and the config
// sub-hash must move off the all-defaults baseline — the same contract every
// hashed sim flag carries (see sim.hydrology_weather's wiring comment).
constexpr int kHighResTicks = 32;

TEST(WaterDeterminism, HighResWaterFlagIsDeterministicAndMovesConfigSubHash) {
    // Config plumbing: all-defaults hashes to the empty baseline; enabling
    // sim.water_high_res must move the sub-hash (a deliberate hash bump).
    const auto defaults = luminumbra::core::SystemConfig::Defaults();
    EXPECT_TRUE(defaults.ComputeConfigSubHash().empty());
    const auto cfg = luminumbra::core::SystemConfig::FromJsonString(
        R"({ "sim": { "water_high_res": { "enabled": true } } })");
    ASSERT_TRUE(cfg.enabled(luminumbra::core::SysKey::SimWaterHighRes));
    EXPECT_FALSE(cfg.ComputeConfigSubHash().empty())
        << "sim.water_high_res is enabled but contributes no config sub-hash bytes";
    EXPECT_NE(cfg.ComputeConfigSubHash(), defaults.ComputeConfigSubHash());

    const HeadlessRoot root;
    std::size_t water_chunks_a = 0, water_chunks_b = 0, water_chunks_med = 0;
    std::int64_t max_depth_a = 0, max_depth_b = 0, max_depth_med = 0;
    int seam_wet_a = 0, seam_wet_b = 0, seam_wet_med = 0;
    const auto seq_a = run_water_sequence(root.root_string(),
                                          kHighResTicks,
                                          water_chunks_a,
                                          max_depth_a,
                                          seam_wet_a,
                                          /*high_res=*/true);
    const auto seq_b = run_water_sequence(root.root_string(),
                                          kHighResTicks,
                                          water_chunks_b,
                                          max_depth_b,
                                          seam_wet_b,
                                          /*high_res=*/true);
    const auto seq_med = run_water_sequence(
        root.root_string(), kHighResTicks, water_chunks_med, max_depth_med, seam_wet_med);

    ASSERT_GT(water_chunks_a, 0u) << "no water chunks were simulated at High — vacuous";
    ASSERT_EQ(seq_a.size(), seq_b.size());
    EXPECT_EQ(seq_a, seq_b) << "High-resolution water diverged between two identical runs — "
                               "NON-DETERMINISTIC (would desync host==peer)";
    EXPECT_GT(max_depth_a, 0) << "no cell ever held depth > 0 at High — water never filled";
    EXPECT_EQ(max_depth_a, max_depth_b);
    EXPECT_GT(seam_wet_a, 0) << "no chunk-seam ever had water on both sides at High — "
                                "cross-chunk flux not flowing on the 16x16 grid";
    EXPECT_EQ(seam_wet_a, seam_wet_b);
    // The flag must CHANGE the hashed water state (the 16x16 mm arrays cannot hash
    // like the 8x8 ones once any water chunk exists).
    ASSERT_EQ(seq_a.size(), seq_med.size());
    EXPECT_NE(seq_a.back(), seq_med.back())
        << "High-resolution water hashed identically to Medium — the flag did nothing";
}

// SAVE MIGRATION. current_water_resolution is persisted per chunk; a session whose
// sim.water_high_res flag mismatches the loaded chunks must converge in ONE boot-time
// pass (LoadWorldStateFrom -> MigrateWaterSimResolution) — left to the live path it
// would resize at 1 chunk/tick while the seam pass (which hard-gates on equal
// resolution) walls off water at every mixed seam. Proven both directions:
// Medium save -> High session and High save -> Medium session.
struct MigrationLoadResult {
    std::size_t adopted = 0;
    std::size_t off_res_after_load = 0; // must be 0 BEFORE the first tick
    int session_res = 0;
    std::size_t water_chunks = 0;
    std::int64_t max_depth = 0;
    int seam_wet_max = 0;
    bool mass_ok = true;
};

// Create a world, settle water at the wet anchor, make one voxel edit (arming the
// full-snapshot save path), and persist the streamed state into save_dir.
void save_world_with_water(const std::string& root, const fs::path& save_dir, bool high_res) {
    JobSystem jobs;
    jobs.startup();
    {
        GameSession session;
        session.SetJobSystem(&jobs);
        session.SetRootPath(root);
        session.SetWaterHighResEnabled(high_res);
        ASSERT_TRUE(session.CreateWorld("MigrationSave", "777", "default"));
        SHIELD_WorldSystem* world = session.GetWorldSystem();
        world->debug_set_streaming_radius_cap(kStreamRadiusCap);
        auto* physics = session.GetPhysicsSystem();
        const Vec3 anchor = find_wet_anchor(world, session.GetMetadata().spawnPoint);
        for (int t = 0; t < 48; ++t) {
            world->update(session.GetRegistry(), {anchor}, physics);
            world->wait_for_streaming_jobs();
        }
        // The incremental save contract writes nothing for a never-edited world; one
        // real voxel edit arms the full snapshot (the PlayerVoxelDig pattern).
        const Vec3 dig_center(
            anchor.x, world->GetTerrainHeightAt(anchor.x, anchor.z) - 2.0f, anchor.z);
        ASSERT_GT(world->EditTerrainVoxel(dig_center, 4.0f, /*fill=*/false, physics), 0);
        for (int t = 0; t < 8; ++t) {
            world->update(session.GetRegistry(), {anchor}, physics);
            world->wait_for_streaming_jobs();
        }
        ASSERT_TRUE(session.SaveWorldStateTo(save_dir));
    }
    jobs.shutdown();
}

// Load save_dir into a fresh session with the given flag, capture the migration
// convergence BEFORE the first tick, then tick and collect the water guards.
MigrationLoadResult
load_world_and_probe(const std::string& root, const fs::path& save_dir, bool high_res) {
    JobSystem jobs;
    jobs.startup();
    MigrationLoadResult r;
    {
        GameSession session;
        session.SetJobSystem(&jobs);
        session.SetRootPath(root);
        session.SetWaterHighResEnabled(high_res);
        EXPECT_TRUE(session.CreateWorld("MigrationLoad", "777", "default"));
        SHIELD_WorldSystem* world = session.GetWorldSystem();
        world->debug_set_streaming_radius_cap(kStreamRadiusCap);
        EXPECT_TRUE(session.LoadWorldStateFrom(save_dir));
        r.adopted = session.GetLastLoadedChunkCount();
        // The boot migration must have converged BEFORE the first live tick: no
        // loaded water chunk may still carry a mismatched resolution (a mismatch
        // would silence the seam pass — a water wall).
        r.off_res_after_load = world->debug_water_chunks_off_resolution();
        r.session_res = world->debug_water_sim_resolution();
        auto* physics = session.GetPhysicsSystem();
        const Vec3 anchor = find_wet_anchor(world, session.GetMetadata().spawnPoint);
        for (int t = 0; t < 16; ++t) {
            world->update(session.GetRegistry(), {anchor}, physics);
            world->wait_for_streaming_jobs();
            if (!world->debug_water_mass_ok())
                r.mass_ok = false;
            r.seam_wet_max = std::max(r.seam_wet_max, world->debug_water_seam_wet_pairs());
        }
        r.water_chunks = world->debug_water_state_hash().water_chunks;
        r.max_depth = world->debug_max_water_depth_mm();
    }
    jobs.shutdown();
    return r;
}

TEST(WaterDeterminism, WaterResolutionMigrationConvergesAtLoadBothWays) {
    const HeadlessRoot root;
    const fs::path save_med = fs::path(root.root_string()) / "save_medium";
    const fs::path save_high = fs::path(root.root_string()) / "save_high";
    save_world_with_water(root.root_string(), save_med, /*high_res=*/false);
    save_world_with_water(root.root_string(), save_high, /*high_res=*/true);

    const MigrationLoadResult up =
        load_world_and_probe(root.root_string(), save_med, /*high_res=*/true);
    const MigrationLoadResult down =
        load_world_and_probe(root.root_string(), save_high, /*high_res=*/false);

    for (const auto* r : {&up, &down}) {
        const bool is_up = (r == &up);
        SCOPED_TRACE(is_up ? "Medium save -> High session" : "High save -> Medium session");
        ASSERT_GT(r->adopted, 0u) << "no chunks were adopted from the save — vacuous";
        EXPECT_EQ(r->session_res, is_up ? 16 : 8);
        EXPECT_EQ(r->off_res_after_load, 0u)
            << "loaded water chunks still carry the saved resolution after LoadWorldStateFrom "
               "— the boot migration did not converge before the first tick (water walls)";
        EXPECT_TRUE(r->mass_ok) << "mass invariant violated while ticking the migrated world";
        EXPECT_GT(r->water_chunks, 0u);
        EXPECT_GT(r->max_depth, 0) << "the migrated world lost its water";
        EXPECT_GT(r->seam_wet_max, 0)
            << "no wet chunk-seam after migration — cross-chunk flux is walled off "
               "(resolution mismatch would look exactly like this)";
    }
}

} // namespace
