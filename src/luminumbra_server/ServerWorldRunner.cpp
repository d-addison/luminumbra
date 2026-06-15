#include "ServerWorldRunner.h"

#include <chrono>
#include <cstdlib>
#include <utility>

#include "luminumbra_common/core/Log.h"
#include "luminumbra_common/ecs/EntitySnapshot.h"
#include "luminumbra_common/persistence/WorldPersistenceRoundtrip.h"
#include "luminumbra_common/persistence/WorldSaveService.h"
#include "luminumbra_common/systems/PhysicsSystem.h"
#include "luminumbra_common/systems/SHIELD_WorldSystem.h"
#include "luminumbra_common/systems/WindFieldSystem.h"
#include "luminumbra_common/systems/WeatherSystem.h"
#include "luminumbra_common/world/WorldStreamingState.h"

namespace Luminumbra::Server {

namespace {

// T-I5a-2 (A2) world_hash MEGA-BUMP: the wind sub-hash from the session's wind
// field, or empty when no wind field exists (defensive; the headless runner
// always constructs one on world create/load).
std::string WindSubHash(world::GameSession* session) {
    if (!session) {
        return {};
    }
    const Systems::WindFieldSystem* wind = session->GetWindFieldSystem();
    return wind ? wind->ComputeWindSubHash() : std::string();
}

// T-I5a-3 (B1) world_hash MEGA-BUMP #2: the weather sub-hash from the session's
// weather core, or empty when none exists (defensive; the headless runner always
// constructs one on world create/load).
std::string WeatherSubHash(world::GameSession* session) {
    if (!session) {
        return {};
    }
    const Systems::WeatherSystem* weather = session->GetWeatherSystem();
    return weather ? weather->ComputeWeatherSubHash() : std::string();
}

// Folds the chunk-derived top-level hash, the wind sub-hash, and the weather
// sub-hash into the composite world_hash. This is the DELIBERATE bump chain: the
// chunk hash (WorldSaveService::world_hash / ComputeWorldStreamingStateHash) is
// unchanged byte-for-byte (persistence fixtures stay green); the runner-level
// world_hash ALSO commits the wind field (A2 bump, 2fa007951a21e140 ->
// 0eac465289e7c88b) and now the weather state (B1 bump #2, 0eac465289e7c88b ->
// 0857e683b4b8c47e; weather_sub_hash a7d8f3d28401386f). Order is fixed (chunk,
// then wind, then weather) so the composite is
// reproducible; appending weather is append-only so the wind term is unchanged.
std::string ComposeWorldHash(const std::string& chunk_hash,
                             const std::string& wind_hash,
                             const std::string& weather_hash) {
    return Persistence::StableChecksum(
        chunk_hash + "|wind:" + wind_hash + "|weather:" + weather_hash);
}

} // namespace

ServerWorldRunner::ServerWorldRunner(ServerWorldRunnerConfig config)
    : m_config(std::move(config)) {}

ServerWorldRunner::~ServerWorldRunner() {
    Shutdown();
}

bool ServerWorldRunner::Boot() {
    if (m_booted) {
        return true;
    }

    // T-I4-12: optional worker-count override (LUMINUMBRA_JOB_WORKERS). The
    // streamed-chunk world_hash is INVARIANT to worker count (hashes are computed
    // after a full streaming quiesce), so this is a pure scheduling knob: the
    // replay gates set it to 1 to minimize the headless server's known
    // intermittent shutdown/streaming race (documented in T-I4-11) without
    // touching the hash. Unset => one worker per hardware thread (production).
    if (const char* wc = std::getenv("LUMINUMBRA_JOB_WORKERS")) {
        m_jobSystem.startup(static_cast<std::size_t>(std::strtoul(wc, nullptr, 10)));
    } else {
        m_jobSystem.startup();
    }

    m_session = std::make_unique<world::GameSession>();
    m_session->SetJobSystem(&m_jobSystem);
    m_session->SetRootPath(m_config.root_path);
    // Headless host: NO SetRequiredClientAssets call (T-I3-6 asset-manifest
    // split - simulation-only validation) and the GPU SDF callback is never
    // registered (SetGPUSDFCallback is client/RenderPipeline territory), so
    // every voxel field is generated on the CPU path.

    bool world_ready = false;
    if (!m_config.world_id.empty()) {
        world_ready = m_session->LoadWorld(m_config.world_id);
    } else {
        world_ready = m_session->CreateWorld(m_config.world_name, m_config.seed, m_config.preset);
    }
    if (!world_ready) {
        LUMINUMBRA_CORE_ERROR("ServerWorldRunner: world boot failed (preset='{}', world_id='{}')",
            m_config.preset, m_config.world_id);
        m_session.reset();
        m_jobSystem.shutdown();
        return false;
    }

    // Contract: LoadWorldState BEFORE generation. Saved chunks are adopted
    // into the streaming map first, so the spawn-anchor generation below only
    // fills gaps and can never clobber authoritative saved voxel data. A
    // fresh world is a clean miss here.
    m_session->LoadWorldState();

    auto* world_system = m_session->GetWorldSystem();
    auto* physics_system = m_session->GetPhysicsSystem();
    if (!world_system || !physics_system) {
        LUMINUMBRA_CORE_ERROR("ServerWorldRunner: world systems missing after boot");
        m_session.reset();
        m_jobSystem.shutdown();
        return false;
    }

    // Spawn-anchor streaming: synchronous surface horizon with collision
    // ready so the physics system can query terrain from tick 1. Meshing
    // stays ON (StreamingProfile meshing-skip is deferred to iteration 4).
    const Vec3 spawn_anchor = m_session->GetMetadata().spawnPoint;
    const bool horizon_ready = world_system->EnsureSurfaceReadyNear(
        spawn_anchor, physics_system, m_config.surface_radius, m_config.collision_radius);
    if (!horizon_ready) {
        LUMINUMBRA_CORE_ERROR("ServerWorldRunner: spawn-anchor surface horizon failed to become ready");
        m_session.reset();
        m_jobSystem.shutdown();
        return false;
    }

    LUMINUMBRA_CORE_INFO(
        "ServerWorldRunner: booted world '{}' (id {}, preset {}, seed {}) - spawn anchor ({}, {}, {}), {} chunks loaded from save",
        m_session->GetMetadata().name, m_session->GetMetadata().worldId,
        m_config.preset, m_session->GetMetadata().seed,
        spawn_anchor.x, spawn_anchor.y, spawn_anchor.z,
        m_session->GetLastLoadedChunkCount());

    m_booted = true;
    return true;
}

ServerTickReport ServerWorldRunner::RunFixedTicks(std::uint64_t tick_count) {
    ServerTickReport report;
    if (!m_booted || !m_session) {
        return report;
    }

    auto* world_system = m_session->GetWorldSystem();
    auto* physics_system = m_session->GetPhysicsSystem();
    const Vec3 spawn_anchor = m_session->GetMetadata().spawnPoint;
    const double fixed_dt = m_session->GetSimulationClock().fixed_dt();

    const auto wall_start = std::chrono::steady_clock::now();
    while (report.ticks_executed < tick_count) {
        // One frame == one fixed tick: feeding the clock exactly fixed_dt
        // keeps the frame/tick mapping 1:1 and removes wall-clock timing from
        // the simulation entirely (determinism discipline).
        physics_system->update(static_cast<float>(fixed_dt));
        report.ticks_executed += m_session->TickSimulation(fixed_dt);
        report.frames_executed += 1;

        // Spawn-anchor streaming, then quiesce in-flight generation/meshing
        // so every scheduler decision next frame observes the identical
        // settled state in both determinism runs.
        world_system->update(m_session->GetRegistry(), spawn_anchor, physics_system);
        world_system->wait_for_streaming_jobs();

        if (m_config.autosave_interval_ticks > 0 &&
            report.ticks_executed > 0 &&
            (report.ticks_executed % m_config.autosave_interval_ticks) == 0) {
            world::WorldStateSaveReport save_report;
            if (m_session->SaveWorldState(&save_report)) {
                report.autosave_passes += 1;
                if (save_report.saved && save_report.chunks_dirty > 0) {
                    report.autosave_writes += 1;
                }
            }
        }
    }
    const auto wall_end = std::chrono::steady_clock::now();

    report.simulated_seconds = static_cast<double>(report.ticks_executed) * fixed_dt;
    report.wall_seconds = std::chrono::duration<double>(wall_end - wall_start).count();
    return report;
}

std::string ServerWorldRunner::ComputeWorldHash() {
    if (!m_booted || !m_session || !m_session->GetWorldSystem()) {
        return {};
    }

    auto* world_system = m_session->GetWorldSystem();
    world_system->wait_for_streaming_jobs();

    WorldStreamingState state;
    for (const auto& chunk : world_system->snapshot_streamed_chunks()) {
        state.insert_chunk(chunk);
    }

    Persistence::WorldSaveService service;
    // T-I5a-2 (A2) + T-I5a-3 (B1) MEGA-BUMPS: fold the wind AND weather sub-hashes
    // into the top-level hash. The chunk hash itself is unchanged; the composite
    // deliberately is not.
    return ComposeWorldHash(service.world_hash(state),
                            WindSubHash(m_session.get()),
                            WeatherSubHash(m_session.get()));
}

Persistence::WorldStreamingStateSubHashes ServerWorldRunner::ComputeWorldSubHashes() {
    // T-I4-11: per-system sub-hashes over the SAME streamed-chunk snapshot the
    // top-level world_hash is built from. Additive desync localization; the
    // top-level hash (ComputeWorldHash) is unchanged.
    Persistence::WorldStreamingStateSubHashes empty;
    if (!m_booted || !m_session || !m_session->GetWorldSystem()) {
        return empty;
    }

    auto* world_system = m_session->GetWorldSystem();
    world_system->wait_for_streaming_jobs();

    WorldStreamingState state;
    for (const auto& chunk : world_system->snapshot_streamed_chunks()) {
        state.insert_chunk(chunk);
    }

    // The headless server is terrain/water authority only: no game entities are
    // streamed, so the entities sub-hash is the stable checksum of the EMPTY
    // canonical ECS snapshot. Present (not blank) so a future entity-bearing
    // server reports an entity-section divergence rather than a silent gap.
    const std::string empty_entities =
        Ecs::SerializeEntityRegistrySnapshotJson(Ecs::EntityRegistrySnapshot{});
    Persistence::WorldStreamingStateSubHashes sub =
        Persistence::ComputeWorldStreamingStateSubHashes(state, empty_entities);
    // T-I5a-2 (A2): the wind sub-hash slot, supplied from the session's wind
    // field (not chunk-derived). Present + stable for the WindFieldDeterminism
    // gate and the desync-localization oracle.
    sub.wind = WindSubHash(m_session.get());
    // T-I5a-3 (B1): the weather sub-hash slot, supplied from the session's weather
    // core (not chunk-derived). Present + stable for the WeatherVisual state-hash
    // assertion and the desync-localization oracle.
    sub.weather = WeatherSubHash(m_session.get());
    return sub;
}

void ServerWorldRunner::ComputeWorldHashAndSubHashes(
    std::string& out_world_hash,
    Persistence::WorldStreamingStateSubHashes& out_sub) {
    // T-I4-12: single quiesce + single chunk snapshot feeding BOTH hashes. This
    // is the recorder/replayer's mid-run checkpoint capture; minimizing the
    // settled-state reads keeps the capture window tight. The produced values are
    // byte-identical to ComputeWorldHash() and ComputeWorldSubHashes() called
    // separately (same WorldSaveService::world_hash + same projection).
    out_world_hash.clear();
    out_sub = Persistence::WorldStreamingStateSubHashes{};
    if (!m_booted || !m_session || !m_session->GetWorldSystem()) {
        return;
    }

    auto* world_system = m_session->GetWorldSystem();
    world_system->wait_for_streaming_jobs();

    WorldStreamingState state;
    for (const auto& chunk : world_system->snapshot_streamed_chunks()) {
        state.insert_chunk(chunk);
    }

    const std::string wind_hash = WindSubHash(m_session.get());
    const std::string weather_hash = WeatherSubHash(m_session.get());

    Persistence::WorldSaveService service;
    // T-I5a-2 (A2) + T-I5a-3 (B1) MEGA-BUMPS: composite world_hash (chunk + wind +
    // weather).
    out_world_hash = ComposeWorldHash(service.world_hash(state), wind_hash, weather_hash);

    const std::string empty_entities =
        Ecs::SerializeEntityRegistrySnapshotJson(Ecs::EntityRegistrySnapshot{});
    out_sub = Persistence::ComputeWorldStreamingStateSubHashes(state, empty_entities);
    out_sub.wind = wind_hash;
    out_sub.weather = weather_hash;
}

std::size_t ServerWorldRunner::SaveFullSnapshot() {
    // T-I4-11: write the FULL in-memory streamed-chunk set (not dirty-gated) so
    // a loaded session can adopt exactly this set. Reuses WorldSaveService.
    if (!m_booted || !m_session || !m_session->GetWorldSystem()) {
        return 0;
    }
    auto* world_system = m_session->GetWorldSystem();
    world_system->wait_for_streaming_jobs();

    WorldStreamingState state;
    for (const auto& chunk : world_system->snapshot_streamed_chunks()) {
        state.insert_chunk(chunk);
    }

    const std::filesystem::path save_dir = m_session->GetWorldSaveDir();
    if (save_dir.empty()) {
        return 0;
    }
    Persistence::WorldSaveService service;
    std::vector<std::string> errors;
    if (!service.save_world(state, save_dir, &errors)) {
        for (const std::string& error : errors) {
            LUMINUMBRA_CORE_ERROR("SaveFullSnapshot failed: {}", error);
        }
        return 0;
    }
    return state.size();
}

std::size_t ServerWorldRunner::StreamedChunkCount() {
    if (!m_session || !m_session->GetWorldSystem()) {
        return 0;
    }
    return m_session->GetWorldSystem()->snapshot_streamed_chunks().size();
}

std::size_t ServerWorldRunner::LoadedChunkCount() const {
    return m_session ? m_session->GetLastLoadedChunkCount() : 0;
}

std::uint64_t ServerWorldRunner::TickCount() const {
    return m_session ? m_session->GetSimulationTickCount() : 0;
}

void ServerWorldRunner::Shutdown(world::WorldStateSaveReport* shutdown_save_report) {
    if (m_shutdown) {
        return;
    }
    m_shutdown = true;

    if (m_booted && m_session) {
        // Save on shutdown via WorldSaveService (incremental contract: a
        // never-edited world writes nothing and stays on the fresh path).
        world::WorldStateSaveReport save_report;
        m_session->SaveWorldState(&save_report);
        if (shutdown_save_report) {
            *shutdown_save_report = save_report;
        }
        LUMINUMBRA_CORE_INFO(
            "ServerWorldRunner: shutdown save - {} chunks total, {} dirty, saved={}",
            save_report.chunks_total, save_report.chunks_dirty, save_report.saved);
    }

    m_session.reset();
    if (m_booted) {
        m_jobSystem.shutdown();
    }
    m_booted = false;
}

} // namespace Luminumbra::Server
