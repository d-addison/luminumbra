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

#include "luminumbra_common/core/JobSystem.h"
#include "luminumbra_common/world/GameSession.h"

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

    std::size_t StreamedChunkCount();
    std::size_t LoadedChunkCount() const;
    [[nodiscard]] std::uint64_t TickCount() const;

    world::GameSession* Session() { return m_session.get(); }

    // Saves world state through WorldSaveService (incremental contract: a
    // never-edited world writes nothing) and tears the session down.
    // Called by the destructor when not invoked explicitly.
    void Shutdown(world::WorldStateSaveReport* shutdown_save_report = nullptr);

private:
    ServerWorldRunnerConfig m_config;
    Luminumbra::JobSystem m_jobSystem;
    std::unique_ptr<world::GameSession> m_session;
    bool m_booted = false;
    bool m_shutdown = false;
};

} // namespace Luminumbra::Server
