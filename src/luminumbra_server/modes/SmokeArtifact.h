#pragma once

#include "../ServerCliOptions.h"
#include "../ServerWorldRunner.h"
#include "luminumbra_common/simulation/SimBudgetTelemetry.h"
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace Luminumbra::Server {

struct SmokeRunResult {
    bool ok = false;
    luminumbra::simulation::SimBudgetTelemetry sim_budget;
    std::string world_hash;
    // per-system sub-hashes (additive; top-level world_hash unchanged).
    Luminumbra::Persistence::WorldStreamingStateSubHashes sub_hashes;
    std::string scent_hash;
    // gate-populated-world-replay: id-ordered ecology sub-hash (empty when no
    // roster) + creature counts before/after the run (non-vacuity oracle).
    std::string ecology_hash;
    //  id-ordered plant sub-hash (empty when no PlantTag roster). Folded into the
    // composite world_hash (bump #7) and surfaced here so the gate verifies plant run==replay too.
    std::string plant_hash;
    std::size_t creature_count_start = 0;
    std::size_t creature_count_end = 0;
    //  gate: per-tick availability-set trace (empty unless --avail-trace).
    std::vector<std::pair<std::uint64_t, std::string>> avail_trace;
    // per-tick water-state hash trace (empty unless --water-hash-trace).
    std::vector<std::pair<std::uint64_t, std::uint64_t>> water_hash_trace;
    std::string world_id;
    Luminumbra::Server::ServerTickReport ticks;
    std::size_t chunks_streamed = 0;
    Luminumbra::world::WorldStateSaveReport shutdown_save;
};

// Complete production smoke verdict, artifact serialization and file writer.
// Fixed run results let compatibility tests pin all otherwise nondeterministic inputs.
int WriteSmokeArtifact(const ServerCliOptions& options,
                       const SmokeRunResult& first,
                       const SmokeRunResult& replay);

} // namespace Luminumbra::Server
