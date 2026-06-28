#pragma once

// T-I3-13: headless server world runner. Boots an authoritative simulation
// world (no renderer, no UI, no audio, CPU SDF only) from a preset name or an
// existing save directory, streams chunks around a fixed spawn anchor with
// collision ready, and drives the canonical fixed 30 Hz simulation loop
// (SimulationClock + GameSession::TickSimulation).
//
// Determinism contract: RunFixedTicks() advances exactly one fixed tick per
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
    // T-I6 P1 (multiplayer): number of deterministic player avatars to spawn at
    // boot (phyllotaxis ring around spawn; see World::DeterministicAvatarSpawnOffset).
    // Avatar positions feed the multi-anchor streaming vector and fold into the
    // `entities` sub-hash. DEFAULT 0 -> no avatars -> byte-identical to the pre-P1
    // headless lane (single spawn anchor, empty entity snapshot). Network
    // connections drive this list in P3; this config is the test/prep entry point.
    int avatar_count = 0;
    // gate-populated-world-replay (T001): spawn a fixed deterministic KINEMATIC
    // creature roster (the gtest ecology_pipeline_test Populate fixture: 2
    // predators + 6 prey, with genomes/alarm/mortal/decay/migratory/territory)
    // into the SAME registry GameSession::TickSimulation ticks, so the hardened
    // ecology stack (brain -> mate-seek -> steering -> reproduce -> lifespan ->
    // decompose -> pack -> migration -> territory) runs LIVE in the headless
    // binary. v1 is KINEMATIC -- NO CreaturePhysicsComponent (the byte-identical
    // default lane; phase-2 physics-creature roster is tracked separately).
    // Positions are offset from the spawn anchor (a pure fn of seed/preset), so
    // the roster + its ecology sub-hash are a pure function of (seed, preset).
    // DEFAULT false -> empty roster -> the ecology sub-hash is empty/neutral and
    // the composite world_hash differs from pre-fold ONLY by the appended
    // `|ecology:` suffix (additivity guard).
    bool ecology_roster = false;
    // I9-FOLIAGE Phase 3D: opt-in deterministic PLANT roster (6 plants) so the smoke exercises the
    // plant sub-hash + growth + persistence end-to-end. DEFAULT false -> empty -> plant_hash neutral.
    bool planted_roster = false;
    // B' determinism harness: when true, RunFixedTicks DRIFTS the streaming anchor deterministically
    // each tick (chunks stream in/out during the run) to reproduce moving-case water determinism that
    // the boot warm-up (interim C) does not cover. DEFAULT false -> the static (fixed-anchor) lane.
    bool moving_anchor = false;
    // Spec 017-B GATE (Codex audit #4): when true, RunFixedTicks records a per-tick
    // AVAILABILITY-SET digest (the sorted resident-chunk id/state/lod/collision set,
    // captured right after the wait_for_streaming_jobs barrier) into AvailabilityTrace().
    // Observability ONLY — it reads the settled snapshot and changes nothing, so it is
    // hash-neutral and stays OFF in the determinism gate. The trace is the baseline a
    // future activation-queue (017-B) must reproduce per tick when it replaces the barrier.
    // DEFAULT false -> zero cost, zero behaviour change.
    bool availability_trace = false;
};

struct ServerTickReport {
    std::uint64_t ticks_executed = 0;
    std::uint64_t frames_executed = 0;
    std::uint64_t autosave_passes = 0;
    std::uint64_t autosave_writes = 0;
    double simulated_seconds = 0.0;
    double wall_seconds = 0.0;
};

class ServerWorldRunner {
public:
    explicit ServerWorldRunner(ServerWorldRunnerConfig config);
    ~ServerWorldRunner();

    ServerWorldRunner(const ServerWorldRunner&) = delete;
    ServerWorldRunner& operator=(const ServerWorldRunner&) = delete;

    // Boots the world: JobSystem startup, GameSession create/load (headless:
    // no client asset manifest, GPU SDF callback NEVER registered),
    // LoadWorldState BEFORE any chunk generation (existing save chunks are
    // authoritative; generation only fills gaps), then synchronous
    // spawn-anchor streaming with collision ready around the spawn point.
    bool Boot();

    // Runs exactly tick_count fixed 30 Hz simulation ticks (one per frame:
    // physics -> TickSimulation -> spawn-anchor streaming update -> streaming
    // quiesce). Returns the per-run report. Requires Boot() to have succeeded.
    ServerTickReport RunFixedTicks(std::uint64_t tick_count);

    // Deterministic hash over the in-memory streamed-chunk snapshot
    // (WorldSaveService::world_hash; format-independent persistence hash).
    std::string ComputeWorldHash();

    // T-I4-11: per-system sub-hashes over the SAME streamed-chunk snapshot, for
    // desync localization. The top-level ComputeWorldHash() above is unchanged;
    // these are additive. entities is the stable hash of the (currently empty,
    // terrain/water-only headless) ECS snapshot -- present so a future
    // entity-bearing server desync is attributable.
    Persistence::WorldStreamingStateSubHashes ComputeWorldSubHashes();

    // T-I4-12 replay checkpoint capture: computes the top-level world_hash AND
    // the per-system sub-hashes from a SINGLE quiesce + chunk snapshot (instead
    // of two independent snapshots via ComputeWorldHash + ComputeWorldSubHashes).
    // One settled-state read per checkpoint keeps the recorder's mid-run capture
    // minimal. out_world_hash and out_sub are filled together and are exactly the
    // values the separate calls would produce.
    void ComputeWorldHashAndSubHashes(
        std::string& out_world_hash,
        Persistence::WorldStreamingStateSubHashes& out_sub);

    // T-I4-11 heavy oracle support: persists the COMPLETE in-memory streamed-
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

    world::GameSession* Session() { return m_session.get(); }

    // T-I6 P1: the deterministic player avatars spawned at boot (empty when
    // avatar_count == 0). Read-only view for tests/telemetry.
    const std::vector<World::PlayerAvatar>& Avatars() const { return m_avatars; }

    // Spec 017-B gate: the per-tick availability-set trace captured during the last
    // RunFixedTicks when config.availability_trace was set. Each entry is
    // (tick_index, digest) where digest is a deterministic FNV-1a over the sorted
    // resident-chunk (id, state, lod, has_collision) set. Empty unless tracing was on.
    const std::vector<std::pair<std::uint64_t, std::string>>& AvailabilityTrace() const {
        return m_avail_trace;
    }

    // T-I6 P3.1d: apply a player's network movement input (normalized world XZ in
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
    std::vector<World::PlayerAvatar> m_avatars; // T-I6 P1: deterministic player avatars
    // Spec 017-B gate: per-tick (tick_index, availability digest), filled by RunFixedTicks
    // only when m_config.availability_trace is set. See AvailabilityTrace().
    std::vector<std::pair<std::uint64_t, std::string>> m_avail_trace;
    // Deterministic FNV-1a digest of the CURRENT settled resident-chunk availability set
    // (sorted id/state/lod/collision). Called per tick under the trace flag.
    std::string ComputeAvailabilityDigest();
    bool m_booted = false;
    bool m_shutdown = false;
};

} // namespace Luminumbra::Server
