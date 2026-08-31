#pragma once

// headless server world runner. Boots an authoritative simulation
// world (no renderer, no UI, no audio, CPU SDF only) from a preset name or an
// existing save directory, streams chunks around a fixed spawn anchor with
// collision ready, and drives the canonical fixed 30 Hz simulation loop
// (SimulationClock + GameSession::TickSimulation).
//
// Determinism contract: RunFixedTicks advances exactly one fixed tick per
// frame (frame_dt == fixed_dt) and quiesces the streaming jobs after every
// frame, so two boot+tick sequences with the same seed/preset perform the
// identical scheduling sequence and converge to the same world_hash. The
// HeadlessServerTick gate asserts that equality.

#include <cstdint>
#include <memory>
#include <string>

#include <vector>

#include "luminumbra_common/core/JobSystem.h"
#include "luminumbra_common/persistence/WorldPersistenceRoundtrip.h"
#include "luminumbra_common/world/GameSession.h"
#include "luminumbra_common/world/PlayerAvatar.h"

namespace Luminumbra::Server {

struct ServerWorldRunnerConfig {
    // Runtime root with trailing separator (GameSession concatenates paths).
    std::string root_path;
    std::string world_name = "Headless Server World";
    std::string seed = "424242";
    // World preset name (worlds/atlas/presets/<preset>.json).
    std::string preset = "default";
    // Non-empty: boot from this existing save directory id
    // (<root>/worlds/saves/<world_id>) instead of creating a fresh world.
    std::string world_id;
    // Spawn-anchor streaming radii (chunks). Mirrors the client's
    // EnsureSurfaceReadyNear horizon/collision split.
    int surface_radius = 4;
    int collision_radius = 2;
    // Autosave every N simulation ticks through WorldSaveService
    // (GameSession::SaveWorldState incremental contract). 0 disables.
    std::uint64_t autosave_interval_ticks = 0;
    //  number of deterministic player avatars to spawn at
    // boot (phyllotaxis ring around spawn; see World::DeterministicAvatarSpawnOffset).
    // Avatar positions feed the multi-anchor streaming vector and fold into the
    // `entities` sub-hash. DEFAULT 0 -> no avatars -> byte-identical to the zero-avatar
    // headless lane (single spawn anchor, empty entity snapshot). Network
    // connections drive this list in P3; this config is the test/prep entry point.
    int avatar_count = 0;
    // gate-populated-world-replay: spawn a fixed deterministic KINEMATIC
    // creature roster (the gtest ecology_pipeline_test Populate fixture: 2
    // predators + 6 prey, with genomes/alarm/mortal/decay/migratory/territory)
    // into the SAME registry GameSession::TickSimulation ticks, so the hardened
    // ecology stack (brain -> mate-seek -> steering -> reproduce -> lifespan ->
    // decompose -> pack -> migration -> territory) runs LIVE in the headless
    // binary. v1 is KINEMATIC -- NO CreaturePhysicsComponent (the byte-identical
    // default lane; physics-creature physics-creature roster is tracked separately).
    // Positions are offset from the spawn anchor (a pure fn of seed/preset), so
    // the roster + its ecology sub-hash are a pure function of (seed, preset).
    // DEFAULT false -> empty roster -> the ecology sub-hash is empty/neutral and
    // the composite world_hash differs from pre-fold ONLY by the appended
    // `|ecology:` suffix (additivity guard).
    bool ecology_roster = false;
    //  opt-in deterministic PLANT roster (6 plants) so the smoke exercises the
    // plant sub-hash + growth + persistence end-to-end. DEFAULT false -> empty -> plant_hash
    // neutral.
    bool planted_roster = false;
    // B' determinism harness: when true, RunFixedTicks DRIFTS the streaming anchor
    // deterministically each tick (chunks stream in/out during the run) to reproduce moving-case
    // water determinism that the boot warm-up (boot warm-up) does not cover. DEFAULT false -> the
    // static (fixed-anchor) lane.
    bool moving_anchor = false;
    //  GATE (runtime audit): when true, RunFixedTicks records a per-tick
    // AVAILABILITY-SET digest (the sorted Ready-chunk IDS — ids only, deliberately not
    // state/lod/collision micro-timing; see ComputeAvailabilityDigest — captured right
    // after the wait_for_streaming_jobs barrier) into AvailabilityTrace, and drives
    // activation-latency telemetry (begin_tick_shadow).
    // Observability ONLY — it reads the settled snapshot and changes nothing, so it is
    // hash-neutral and stays OFF in the determinism gate. The trace verifies that
    // the activation queue produces the same availability set in replay. DEFAULT
    // false means zero telemetry cost and no behaviour change.
    bool availability_trace = false;
    //  ( water cross-process): record the per-tick water-state hash during
    // RunFixedTicks — the sequence the debug-vs-release WaterCrossBuild gate
    // compares. Observability only; never feeds world_hash.
    bool water_hash_trace = false;
};

struct ServerTickReport {
    std::uint64_t ticks_executed = 0;
    std::uint64_t frames_executed = 0;
    std::uint64_t autosave_passes = 0;
    std::uint64_t autosave_writes = 0;
    double simulated_seconds = 0.0;
    double wall_seconds = 0.0;
    // main-thread blocking-wait instrumentation. Time the main thread spends
    // BLOCKED in the per-tick streaming barrier — since activation queue this is activate_due
    // (ServerWorldRunner.cpp:613), which replaced the old per-tick wait_for_streaming_jobs
    // drain; it is the latency the activation queue targets. Percentiles over the per-tick
    // samples (ms). Observability only; never feeds world_hash (wall-clock, like wall_seconds).
    double main_wait_p50_ms = 0.0;
    double main_wait_p95_ms = 0.0;
    double main_wait_p99_ms = 0.0;
    double main_wait_max_ms = 0.0;
    double main_wait_total_ms = 0.0;
};

class ServerWorldRunner {
public:
    explicit ServerWorldRunner(ServerWorldRunnerConfig config);
    ~ServerWorldRunner();

    ServerWorldRunner(const ServerWorldRunner&) = delete;
    ServerWorldRunner& operator=(const ServerWorldRunner&) = delete;

    // Boots the world: JobSystem startup, GameSession create/load (headless,
    // with no client asset manifest),
    // LoadWorldState BEFORE any chunk generation (existing save chunks are
    // authoritative; generation only fills gaps), then synchronous
    // spawn-anchor streaming with collision ready around the spawn point.
    bool Boot();

    // boot water-settle exit stats + the settle CONTRACT. A global all-asleep
    // fixed point does not exist for this solver (wet/dry boundary cells limit-cycle and
    // wake propagation re-wakes neighbours), so the contract that makes save/load water
    // round-trip is: FRESH boots leave zero uninitialized chunks (contract_ok) and run a
    // fixed deterministic transient budget; LOADED boots skip the water settle entirely
    // (water paused through Boot — water_settle_skipped — the restored mid-flow state is
    // authoritative) and resume from the persisted sim-window cursor. awake > 0 at exit
    // is expected and fine. Populated by Boot; asserted by the heavy oracle.
    struct BootSettleStats {
        std::size_t water_chunks = 0;      // chunks with has_water_sim at exit
        std::size_t awake = 0;             // water chunks not asleep at exit (informational)
        std::size_t uninited = 0;          // streamed chunks never water-initialized
        int iterations = 0;                // physics-creature settle iterations consumed
        bool water_settle_skipped = false; // true = loaded boot (water paused, no settle)
        bool contract_ok() const {
            return uninited == 0;
        }
    };
    const BootSettleStats& GetBootSettleStats() const {
        return m_boot_settle;
    }

    // Runs exactly tick_count fixed 30 Hz simulation ticks (one per frame:
    // physics -> TickSimulation -> spawn-anchor streaming update -> streaming
    // quiesce). Returns the per-run report. Requires Boot to have succeeded.
    ServerTickReport RunFixedTicks(std::uint64_t tick_count);

    // Deterministic hash over the in-memory streamed-chunk snapshot
    // (WorldSaveService::world_hash; format-independent persistence hash).
    std::string ComputeWorldHash();

    // per-system sub-hashes over the SAME streamed-chunk snapshot, for
    // desync localization. The top-level ComputeWorldHash above is unchanged;
    // these are additive. entities is the stable hash of the (currently empty,
    // terrain/water-only headless) ECS snapshot -- present so a future
    // entity-bearing server desync is attributable.
    Persistence::WorldStreamingStateSubHashes ComputeWorldSubHashes();

    //  replay checkpoint capture: computes the top-level world_hash AND
    // the per-system sub-hashes from a SINGLE quiesce + chunk snapshot (instead
    // of two independent snapshots via ComputeWorldHash + ComputeWorldSubHashes).
    // One settled-state read per checkpoint keeps the recorder's mid-run capture
    // minimal. out_world_hash and out_sub are filled together and are exactly the
    // values the separate calls would produce.
    void ComputeWorldHashAndSubHashes(std::string& out_world_hash,
                                      Persistence::WorldStreamingStateSubHashes& out_sub);

    //  heavy oracle support: persists the COMPLETE in-memory streamed-
    // chunk snapshot via WorldSaveService::save_world (NOT the dirty-gated
    // GameSession::SaveWorldState, which writes nothing for a never-edited
    // world). Used so a freshly loaded session adopts exactly this chunk set and
    // the save/load round-trip is comparable. Returns the chunk count written.
    std::size_t SaveFullSnapshot();

    std::size_t StreamedChunkCount();
    std::size_t LoadedChunkCount() const;
    [[nodiscard]] std::uint64_t TickCount() const;

    // gate-populated-world-replay: the id-ordered ecology sub-hash over the live
    // creature roster (empty/neutral string when no creature is spawned). Folded
    // into ComposeWorldHash as the 6th canonical term and surfaced to the gate
    // for the run==replay assertion.
    std::string ComputeEcologySubHash() const;
    // gate-populated-world-replay: live count of CreatureComponent-bearing
    // entities (for the gate's non-vacuity check: start != end => births/culls).
    [[nodiscard]] std::size_t CreatureCount() const;

    world::GameSession* Session() {
        return m_session.get();
    }

    //  the deterministic player avatars spawned at boot (empty when
    // avatar_count == 0). Read-only view for tests/telemetry.
    const std::vector<World::PlayerAvatar>& Avatars() const {
        return m_avatars;
    }

    //  gate: the per-tick availability-set trace captured during the last
    // RunFixedTicks when config.availability_trace was set. Each entry is
    // (tick_index, digest) where digest is a deterministic FNV-1a over the sorted
    // Ready-chunk IDS (ids only — see ComputeAvailabilityDigest). Empty unless tracing was on.
    const std::vector<std::pair<std::uint64_t, std::string>>& AvailabilityTrace() const {
        return m_avail_trace;
    }

    // the per-tick (tick_index, water-state hash) trace captured during the
    // last RunFixedTicks when config.water_hash_trace was set. Empty unless tracing.
    const std::vector<std::pair<std::uint64_t, std::uint64_t>>& WaterHashTrace() const {
        return m_water_hash_trace;
    }

    //  apply a player's network movement input (normalized world XZ in
    // [-1,1]) to its avatar's physics for the next tick. player_id == avatar index.
    // The caller decodes this from the replicated usercmd; persists until changed.
    void SetAvatarMove(std::uint32_t player_id, float move_x, float move_z);

    // Saves world state through WorldSaveService (incremental contract: a
    // never-edited world writes nothing) and tears the session down.
    // Called by the destructor when not invoked explicitly.
    void Shutdown(world::WorldStateSaveReport* shutdown_save_report = nullptr);

private:
    ServerWorldRunnerConfig m_config;
    Luminumbra::JobSystem m_jobSystem;
    std::unique_ptr<world::GameSession> m_session;
    std::vector<World::PlayerAvatar> m_avatars; // Deterministic player avatars.
    //  gate: per-tick (tick_index, availability digest), filled by RunFixedTicks
    // only when m_config.availability_trace is set. See AvailabilityTrace.
    std::vector<std::pair<std::uint64_t, std::string>> m_avail_trace;
    // per-tick (tick_index, water-state hash), filled by RunFixedTicks
    // only when m_config.water_hash_trace is set. See WaterHashTrace.
    std::vector<std::pair<std::uint64_t, std::uint64_t>> m_water_hash_trace;
    // Deterministic FNV-1a digest of the CURRENT settled resident-chunk availability set
    // (sorted id/state/lod/collision). Called per tick under the trace flag.
    std::string ComputeAvailabilityDigest();
    bool m_booted = false;
    bool m_shutdown = false;
    BootSettleStats m_boot_settle; //  settle-exit stats (see getter)
};

} // namespace Luminumbra::Server
