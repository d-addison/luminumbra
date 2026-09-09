#pragma once

#include "luminumbra_server/ServerCliOptions.h"
#include <string>

// Shared by the production-writer regression and the pinned devel baseline generator.
// Templates keep these legacy inputs independent of the telemetry-only result member.
template<typename Run>
void FixedSmokeInputs(int scenario, ServerCliOptions& options, Run& first, Run& replay) {
    options.preset = "fixture-\"preset";
    options.seed = "1337";
    options.ticks = 2;
    options.surface_radius = 3;
    options.collision_radius = 1;
    options.water_smoke = scenario != 0;
    options.availability_trace = scenario != 0;
    options.water_hash_trace = scenario != 0;
    first.ok = true;
    first.world_hash = "0123456789abcdef";
    first.sub_hashes.terrain = "1000000000000001";
    first.sub_hashes.mesh = "2000000000000002";
    first.sub_hashes.water = "3000000000000003";
    first.sub_hashes.entities = "4000000000000004";
    first.sub_hashes.wind = "5000000000000005";
    first.sub_hashes.weather = "6000000000000006";
    first.sub_hashes.aether = "7000000000000007";
    first.sub_hashes.aether_state = "8000000000000008";
    first.scent_hash = "9000000000000009";
    first.ecology_hash = "a00000000000000a";
    first.plant_hash = "b00000000000000b";
    first.creature_count_start = 8;
    first.creature_count_end = 9;
    first.avail_trace = {{1, "c00000000000000c"}, {2, "d00000000000000d"}};
    first.water_hash_trace = {{1, 0x0123456789abcdefULL}, {2, 0xfedcba9876543210ULL}};
    first.world_id = "fixed world\nrun-1";
    first.ticks.ticks_executed = 2;
    first.ticks.frames_executed = 3;
    first.ticks.autosave_passes = 4;
    first.ticks.autosave_writes = 1;
    first.ticks.simulated_seconds = 0.0625;
    first.ticks.wall_seconds = 1.25;
    first.ticks.main_wait_p50_ms = 0.125;
    first.ticks.main_wait_p95_ms = 0.25;
    first.ticks.main_wait_p99_ms = 0.5;
    first.ticks.main_wait_max_ms = 1.0;
    first.ticks.main_wait_total_ms = 1.125;
    auto& water = first.ticks.water;
    water.enabled = scenario != 0;
    water.init = {0.125, 0.25, 0.5};
    water.sim = {1.125, 1.25, 1.5};
    water.seam = {2.125, 2.25, 2.5};
    water.bookkeeping = {3.125, 3.25, 3.5};
    water.total = {6.5, 7.0, 8.0};
    water.cells_simmed_per_tick = 96.0;
    water.cells_simmed_total = 192;
    water.awake_chunks_max = 3;
    water.mass_ok = true;
    water.seam_wet_pairs_max = 8;
    first.chunks_streamed = 5;
    first.shutdown_save.chunks_total = 5;
    first.shutdown_save.chunks_dirty = 3;
    first.shutdown_save.chunks_saved = 2;
    first.shutdown_save.saved = true;
    replay = first;
    replay.world_id = "fixed world\\run-2";
    replay.ticks.wall_seconds = 2.5;
    replay.sub_hashes.mesh = "eeeeeeeeeeeeeeee";
    replay.shutdown_save.saved = false;
    if (scenario == 2) {
        replay.world_hash = "ffffffffffffffff";
        replay.plant_hash = "ffffffffffffffff";
        replay.avail_trace[1].second = "ffffffffffffffff";
        replay.water_hash_trace[1].second = 0;
        replay.ticks.water.mass_ok = false;
    }
}
