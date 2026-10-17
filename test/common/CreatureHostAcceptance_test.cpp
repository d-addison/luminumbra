#include "../support/SimulationContentFixture.h"
#include "../support/UniqueTempPath.h"
#include "luminumbra_common/ai/CreaturePersistence.h"
#include "luminumbra_common/ai/EcologyHash.h"
#include "luminumbra_common/components/AlarmComponents.h"
#include "luminumbra_common/components/CircadianComponents.h"
#include "luminumbra_common/components/CombustionComponents.h"
#include "luminumbra_common/components/CoreComponents.h"
#include "luminumbra_common/components/CreatureComponents.h"
#include "luminumbra_common/components/DecayComponents.h"
#include "luminumbra_common/components/FieldEmitterComponents.h"
#include "luminumbra_common/components/ForagingComponents.h"
#include "luminumbra_common/components/GrazeableComponent.h"
#include "luminumbra_common/components/InstinctComponents.h"
#include "luminumbra_common/components/IrrigationComponents.h"
#include "luminumbra_common/components/MigratoryComponents.h"
#include "luminumbra_common/components/MortalComponents.h"
#include "luminumbra_common/components/PackHunterComponents.h"
#include "luminumbra_common/components/PlantComponents.h"
#include "luminumbra_common/components/ScavengerComponent.h"
#include "luminumbra_common/components/TerritoryComponents.h"
#include "luminumbra_common/components/ThirstComponents.h"
#include "luminumbra_common/core/Environment.h"
#include "luminumbra_common/core/SystemConfig.h"
#include "luminumbra_common/core/WaterComponents.h"
#include "luminumbra_common/persistence/WorldSaveService.h"
#include "luminumbra_common/replay/RegionScheduleTrace.h"
#include "luminumbra_common/systems/PhysicsSystem.h"
#include "luminumbra_server/ServerWorldRunner.h"
#include "luminumbra_server/modes/ModeHelpers.h"
#include "luminumbra_server/modes/Modes.h"
#include <algorithm>
#include <bit>
#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include <map>
#include <sstream>
#include <type_traits>

// BEGIN frozen HEAD runner oracle. The class/include renames isolate the old
// host behavior; the implementation below does not call the changed runner.
namespace Luminumbra::Server {
class HeadServerWorldRunner {
public:
    explicit HeadServerWorldRunner(ServerWorldRunnerConfig config);
    ~HeadServerWorldRunner();

    HeadServerWorldRunner(const HeadServerWorldRunner&) = delete;
    HeadServerWorldRunner& operator=(const HeadServerWorldRunner&) = delete;

    // Boots the world: JobSystem startup, GameSession create/load (headless,
    // with no client asset manifest),
    // LoadWorldState BEFORE any chunk generation (existing save chunks are
    // authoritative; generation only fills gaps), then synchronous
    // spawn-anchor streaming with collision ready around the spawn point.
    bool Boot();
    const std::string& GetBootError() const {
        return m_bootError;
    }

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
    // these are additive. entities hashes the deterministic avatar snapshot
    // (empty only when no avatars are configured). Creature ecology and plants
    // have their own sub-hashes in the composite world hash.
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
    std::string m_bootError;
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
// Frozen HEAD runner semantics; only class/include names differ.

#include "luminumbra_server/ServerStreamingBudget.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <utility>
#include <vector>

#include "luminumbra_common/ai/EcologyHash.h"
#include "luminumbra_common/components/AlarmComponents.h"
#include "luminumbra_common/components/CoreComponents.h"
#include "luminumbra_common/components/CreatureComponents.h"
#include "luminumbra_common/components/DecayComponents.h"
#include "luminumbra_common/components/InstinctComponents.h" // SensableComponent (prey scent)
#include "luminumbra_common/components/MigratoryComponents.h"
#include "luminumbra_common/components/MortalComponents.h"
#include "luminumbra_common/components/PackHunterComponents.h"
#include "luminumbra_common/components/TerritoryComponents.h"
#include "luminumbra_common/core/Environment.h"
#include "luminumbra_common/core/Log.h"
#include "luminumbra_common/core/Profiler.h"
#include "luminumbra_common/core/SystemConfig.h" // sim.water_high_res (host==peer)
#include "luminumbra_common/ecs/EntitySnapshot.h"
#include "luminumbra_common/persistence/WorldPersistenceRoundtrip.h"
#include "luminumbra_common/persistence/WorldSaveService.h"
#include "luminumbra_common/systems/AetherFieldSystem.h"
#include "luminumbra_common/systems/FarmingSystem.h"
#include "luminumbra_common/systems/PhysicsSystem.h"
#include "luminumbra_common/systems/SHIELD_WorldSystem.h"
#include "luminumbra_common/systems/WaterSystem.h" // water-smoke telemetry reads
#include "luminumbra_common/systems/WeatherSystem.h"
#include "luminumbra_common/systems/WindFieldSystem.h"
#include "luminumbra_common/world/Chunk.h"
#include "luminumbra_common/world/WorldStreamingState.h"

namespace Luminumbra::Server {

namespace {

// water-smoke: deterministic wet anchor — the nearest point to spawn where the PURE
// worldgen samplers say standing water exists (WaterLevelAt > terrain): sea or a lake
// basin. Pure functions of seed/preset, independent of chunk residency, so it is
// identical in every run and callable before any chunk streams. Square-ring scan
// outward in 8 m steps to +-192 m, first hit wins; falls back to spawn if everything
// in range is dry (the artifact's non-vacuity fields are the tripwire). Mirrors the
// WaterDeterminism gate's find_wet_anchor so the workload provably starts wet.
Vec3 FindWetAnchor(const Systems::SHIELD_WorldSystem& world, const Vec3& spawn) {
    constexpr float kStep = 8.0f;
    constexpr int kMaxRing = 24; // 24 * 8 m = 192 m
    for (int ring = 0; ring <= kMaxRing; ++ring) {
        for (int dz = -ring; dz <= ring; ++dz) {
            for (int dx = -ring; dx <= ring; ++dx) {
                if (std::max(std::abs(dx), std::abs(dz)) != ring) {
                    continue; // perimeter only — nearest ring first
                }
                const float x = spawn.x + static_cast<float>(dx) * kStep;
                const float z = spawn.z + static_cast<float>(dz) * kStep;
                const float terrain = world.GetTerrainHeightAt(x, z);
                if (world.WaterLevelAt(x, z) > terrain + 0.25f) {
                    return Vec3(x, terrain + 2.0f, z);
                }
            }
        }
    }
    return Vec3(spawn.x, world.GetTerrainHeightAt(spawn.x, spawn.z) + 2.0f, spawn.z);
}

//   world_hash hash revision: the wind sub-hash from the session's wind
// field, or empty when no wind field exists (defensive; the headless runner
// always constructs one on world create/load).
std::string WindSubHash(world::GameSession* session) {
    if (!session) {
        return {};
    }
    const Systems::WindFieldSystem* wind = session->GetWindFieldSystem();
    return wind ? wind->ComputeWindSubHash() : std::string();
}

//   world_hash hash revision: the weather sub-hash from the session's
// weather core, or empty when none exists (defensive; the headless runner always
// constructs one on world create/load).
std::string WeatherSubHash(world::GameSession* session) {
    if (!session) {
        return {};
    }
    const Systems::WeatherSystem* weather = session->GetWeatherSystem();
    return weather ? weather->ComputeWeatherSubHash() : std::string();
}

//  world_hash bump #4: the aether sub-hash from the session's Aether
// scalar field, or empty when none exists (defensive; the headless runner always
// constructs one on world create/load).
//
//  (, -3): when the STATEFUL energy layer is active and
// nonzero, its aether_state:v1: sub-hash folds into THIS EXISTING slot (the
// composite becomes StableChecksum(rederivable + "|state:" + state)) rather
// than adding an 8th ComposeWorldHash term — a new term literal would move the
// canonical composite even while empty (the |plants: precedent), violating the
// zero-re-pin contract. Default OFF -> the state hash is empty -> this function
// returns the re-derivable value byte-identically. Activation (the owner-menu
// bump) moves this slot with NO further code change.
std::string AetherSubHash(world::GameSession* session) {
    if (!session) {
        return {};
    }
    const Systems::AetherFieldSystem* aether = session->GetAetherFieldSystem();
    std::string rederivable = aether ? aether->ComputeAetherSubHash() : std::string();
    const std::string state = session->ComputeAetherStateSubHash();
    if (state.empty()) {
        return rederivable;
    }
    return Persistence::StableChecksum(rederivable + "|state:" + state);
}

// scent/stigmergy sub-hash from the session-owned ecology
// field. Empty when no entity has opted into scent emission/sensing.
std::string ScentSubHash(world::GameSession* session) {
    return session ? session->ComputeScentSubHash() : std::string();
}

// gate-populated-world-replay: spawn the deterministic KINEMATIC creature
// roster into `registry`. This mirrors the gtest ecology_pipeline_test Populate
// fixture (2 predators + 6 prey, with genomes/alarm/mortal/decay/migratory/
// territory) component-for-component and value-for-value, with the ONLY
// difference being positions offset from the spawn anchor's XZ (so they sit in
// the world the headless runner streams). KINEMATIC -- NO CreaturePhysicsComponent
// -- so the brain integrates X/Z directly (terrain-independent) and the roster is
// a pure function of (seed, preset) via the anchor. Y is set to the anchor's Y but
// is sim-irrelevant on the kinematic lane.
void SpawnEcologyRoster(entt::registry& r, const Vec3& anchor) {
    namespace Comp = ::Luminumbra::Components;
    const float ox = anchor.x;
    const float oz = anchor.z;
    const float oy = anchor.y;

    auto pred = [&](float x, float z) {
        auto e = r.create();
        auto& tf = r.emplace<Comp::TransformComponent>(e);
        tf.position = Vec3(ox + x, oy, oz + z);
        auto& cr = r.emplace<Comp::CreatureComponent>(e);
        cr.is_predator = true;
        cr.hunger = 0.9f;
        cr.move_speed = 4.2f;
        cr.species_id = Comp::CreatureSpeciesId16("ridgeback_stalker");
        r.emplace<Comp::PackHunterComponent>(e);
        r.emplace<Comp::MortalComponent>(e).lifespan_ticks = 5000u;
    };
    int idx = 0;
    auto prey = [&](float x, float z) {
        auto e = r.create();
        auto& tf = r.emplace<Comp::TransformComponent>(e);
        tf.position = Vec3(ox + x, oy, oz + z);
        auto& cr = r.emplace<Comp::CreatureComponent>(e);
        cr.is_predator = false;
        cr.hunger = 0.05f;
        cr.stamina = 1.0f;
        cr.move_speed = 3.0f;
        cr.species_id = Comp::CreatureSpeciesId16("grovestrider");
        auto& gn = r.emplace<Comp::CreatureGenomeComponent>(e);
        gn.female = (idx++ % 2 == 0);
        gn.age_ticks = 100u;
        r.emplace<Comp::AlarmComponent>(e);
        r.emplace<Comp::MortalComponent>(e).lifespan_ticks = 600u;
        r.emplace<Comp::DecayComponent>(e).decay_duration = 90u;
        r.emplace<Comp::MigratoryComponent>(e);
        r.emplace<Comp::TerritoryComponent>(e);
        r.emplace<Comp::TerritoryBiasComponent>(e);
        //  ( scent tracking): prey DEPOSIT scent (channel 0) so predators
        // can track them by smell when out of direct perception. Adding a scent
        // participant activates the deposit tick -> the populated golden re-pins
        // (the defined  refresh); the canonical smoke has no roster and is
        // untouched.
        auto& sn = r.emplace<Comp::SensableComponent>(e);
        sn.scent_channel = 0;
        sn.scent_deposit = 0.5f;
    };
    pred(-6.0f, 9.0f);
    pred(6.0f, 9.0f);
    for (int i = 0; i < 6; ++i)
        prey(-7.0f + i * 2.4f, -2.0f);
}

//  spawn a deterministic PLANT roster so the headless smoke exercises the plant
// path end-to-end (ComputePlantSubHash non-empty, PlantGrowthSystem ticking, persistence roundtrip)
// rather than only the empty-neutral path. Uses the  MakePlantFromSpecies (sample genome ->
// PlantSeed -> stamp CropLifecycle) from an INLINE species template (no file I/O), seeded from
// ints, so the roster is a pure function of (seed, preset) via the anchor -> run==replay.
void SpawnPlantRoster(entt::registry& r, const Vec3& anchor) {
    namespace fol = luminumbra::foliage;
    fol::SpeciesTemplate tmpl;
    tmpl.id = "smoke_crop";
    tmpl.perennial = false;
    tmpl.lifespan_ticks = 900u;
    tmpl.gene_lo.fill(0.30f);
    tmpl.gene_hi.fill(0.70f);
    auto rng = luminumbra::core::DeterministicRng::seeded(fol::kPlantSeedOffset, 9001u, 7u);
    for (int i = 0; i < 6; ++i) {
        const Vec3 pos(anchor.x - 6.0f + static_cast<float>(i) * 2.0f, anchor.y, anchor.z + 4.0f);
        fol::MakePlantFromSpecies(r, pos, tmpl, rng, 0u);
    }
}

std::pair<int, int> HorizontalChunkCoords(const Vec3& position) {
    return {static_cast<int>(std::floor(position.x / CHUNK_SIZE_X)),
            static_cast<int>(std::floor(position.z / CHUNK_SIZE_Z))};
}

bool HorizontalChunkHorizonCovers(const Vec3& center, const Vec3& candidate, int radius) {
    const auto [cx, cz] = HorizontalChunkCoords(center);
    const auto [tx, tz] = HorizontalChunkCoords(candidate);
    return std::max(std::abs(tx - cx), std::abs(tz - cz)) <= std::max(0, radius);
}

} // namespace

HeadServerWorldRunner::HeadServerWorldRunner(ServerWorldRunnerConfig config)
    : m_config(std::move(config)) {}

HeadServerWorldRunner::~HeadServerWorldRunner() {
    Shutdown();
}

bool HeadServerWorldRunner::Boot() {
    if (m_booted) {
        return true;
    }

    // optional worker-count override (LUMINUMBRA_JOB_WORKERS). The
    // streamed-chunk world_hash is INVARIANT to worker count (hashes are computed
    // after a full streaming quiesce), so this is a pure scheduling knob: the
    // replay gates can set it to 1 to bound worker usage without touching the
    // hash. Unset => one worker per hardware thread (production).
    if (const auto workers = Core::ReadEnvironment("LUMINUMBRA_JOB_WORKERS")) {
        m_jobSystem.startup(static_cast<std::size_t>(std::strtoul(workers->c_str(), nullptr, 10)));
    } else {
        m_jobSystem.startup();
    }

    m_session = std::make_unique<world::GameSession>();
    m_session->SetSimBudgetTelemetryEnabled(m_config.sim_budget);
    m_session->SetJobSystem(&m_jobSystem);
    m_session->SetRootPath(m_config.root_path);
    if (m_config.region_scheduler)
        m_session->SetRegionSchedulerConfig(*m_config.region_scheduler);
    // host==peer: the headless host must run the SAME water-sim resolution the
    // client derives from data/common/systems.json — sim.water_high_res is baked into
    // the hashed mm grids, so a mismatched host could never agree with its peers.
    // Resolved against the runner root with a CWD-relative fallback (the client's
    // load path); a missing file is all-defaults = Medium = byte-identical.
    {
        namespace fsys = std::filesystem;
        const fsys::path root =
            m_config.root_path.empty() ? fsys::path(".") : fsys::path(m_config.root_path);
        fsys::path systems_json = root / "data" / "common" / "systems.json";
        std::error_code systems_ec;
        if (!fsys::exists(systems_json, systems_ec)) {
            systems_json = fsys::path("data") / "common" / "systems.json";
        }
        const auto sys_cfg = luminumbra::core::SystemConfig::LoadFromFile(systems_json.string());
        m_session->SetActiveRegionsEnabled(
            sys_cfg.enabled(luminumbra::core::SysKey::SimActiveRegions));
        m_session->SetParkedBytesLimit(static_cast<std::size_t>(m_config.parked_bytes_limit));
        m_session->SetWaterHighResEnabled(
            sys_cfg.enabled(luminumbra::core::SysKey::SimWaterHighRes));
    }
    // A headless host has no client asset manifest; simulation assets alone are
    // validated and every voxel field uses the authoritative CPU generator.

    bool world_ready = false;
    if (!m_config.world_id.empty()) {
        world_ready = m_session->LoadWorld(m_config.world_id);
    } else {
        world_ready = m_session->CreateWorld(m_config.world_name, m_config.seed, m_config.preset);
    }
    if (!world_ready) {
        m_bootError = m_session->GetWorldOpenError();
        LUMINUMBRA_CORE_ERROR(
            "HeadServerWorldRunner: world boot failed (preset='{}', world_id='{}')",
            m_config.preset,
            m_config.world_id);
        m_session.reset();
        m_jobSystem.shutdown();
        return false;
    }

    // Contract: LoadWorldState BEFORE generation. Saved chunks are adopted
    // into the streaming map first, so the spawn-anchor generation below only
    // fills gaps and can never clobber authoritative saved voxel data. A
    // fresh world is a clean miss here.
    if (m_config.world_id.empty() && !m_session->LoadWorldState()) {
        m_bootError = m_session->GetWorldOpenError();
        LUMINUMBRA_CORE_ERROR("HeadServerWorldRunner: world open refused: {}", m_bootError);
        m_session.reset();
        m_jobSystem.shutdown();
        return false;
    }

    auto* world_system = m_session->GetWorldSystem();
    auto* physics_system = m_session->GetPhysicsSystem();
    if (!world_system || !physics_system) {
        LUMINUMBRA_CORE_ERROR("HeadServerWorldRunner: world systems missing after boot");
        m_session.reset();
        m_jobSystem.shutdown();
        return false;
    }

    world_system->debug_set_streaming_radius_cap(m_config.test_streaming_radius_cap);

    // a session booted FROM A SAVE must not advance water anywhere in Boot —
    // the restored mid-flow state (depths, sleep flags, counters, persisted sim-window
    // cursor) is authoritative, and the water network flows perpetually, so any boot
    // stepping advances the loaded session past the original's saved state and the
    // water sub-hash can never round-trip. Pause BEFORE the first streaming call
    // (EnsureSurfaceReadyNear runs the water update too); unpause at the end of Boot.
    const bool water_loaded_from_save = m_session->GetLastLoadedChunkCount() > 0;
    if (water_loaded_from_save) {
        world_system->SetWaterBootPaused(true);
    }

    // Spawn-anchor streaming: synchronous surface horizon with collision
    // ready so the physics system can query terrain from tick 1. Meshing
    // stays on because the supported server profile keeps meshes resident.
    const Vec3 spawn_anchor = m_session->GetMetadata().spawnPoint;
    const bool horizon_ready = world_system->EnsureSurfaceReadyNear(
        spawn_anchor, physics_system, m_config.surface_radius, m_config.collision_radius);
    if (!horizon_ready) {
        LUMINUMBRA_CORE_ERROR(
            "HeadServerWorldRunner: spawn-anchor surface horizon failed to become ready");
        m_session.reset();
        m_jobSystem.shutdown();
        return false;
    }

    LUMINUMBRA_CORE_INFO(
        "HeadServerWorldRunner: booted world '{}' (id {}, preset {}, seed {}) - spawn "
        "anchor ({}, {}, {}), {} chunks loaded from save",
        m_session->GetMetadata().name,
        m_session->GetMetadata().worldId,
        m_config.preset,
        m_session->GetMetadata().seed,
        spawn_anchor.x,
        spawn_anchor.y,
        spawn_anchor.z,
        m_session->GetLastLoadedChunkCount());

    //  spawn the deterministic player avatars. Each avatar's
    // XZ is the world spawn plus a pure phyllotaxis offset (player_id-indexed);
    // Y is terrain-clamped. Avatar positions become the multi-anchor streaming
    // vector in RunFixedTicks and fold into the `entities` sub-hash. avatar_count
    // == 0 leaves m_avatars empty -> the zero-avatar single-anchor / empty-entity lane.
    m_avatars.clear();
    physics_system->clear_avatar_characters();
    if (m_config.avatar_count > 0) {
        m_avatars.reserve(static_cast<std::size_t>(m_config.avatar_count));
        for (int i = 0; i < m_config.avatar_count; ++i) {
            const Vec3 offset =
                World::DeterministicAvatarSpawnOffset(static_cast<std::uint32_t>(i));
            const float ax = spawn_anchor.x + offset.x;
            const float az = spawn_anchor.z + offset.z;
            // Spawn a touch above the terrain so the physics character
            // settles down onto the ground deterministically on the first ticks.
            const float ay = world_system->GetTerrainHeightAt(ax, az) + 1.5f;
            World::PlayerAvatar avatar;
            avatar.player_id = static_cast<std::uint32_t>(i);
            avatar.position = Vec3(ax, ay, az);
            // Deterministic initial facing fanned around the circle (pure id fn).
            avatar.facing = static_cast<float>(i) * 2.39996323f;
            m_avatars.push_back(avatar);
            // Create one server-authoritative physics capsule per avatar.
            physics_system->create_avatar_character(avatar.position);
        }
        LUMINUMBRA_CORE_INFO("HeadServerWorldRunner: spawned {} deterministic player avatar(s) "
                             "(+physics characters).",
                             m_avatars.size());

        // The spawn horizon is already fully ready. For avatars outside that
        // collision neighbourhood, synchronously warm only a compact local
        // collision horizon; the regular multi-anchor streamer expands the
        // visual AOI over subsequent fixed ticks.
        const int avatar_collision_radius = std::max(0, m_config.collision_radius);
        const int avatar_surface_radius =
            std::max(avatar_collision_radius,
                     std::min(m_config.surface_radius, std::max(avatar_collision_radius, 1)));
        std::vector<Vec3> warmed_collision_anchors;
        warmed_collision_anchors.reserve(m_avatars.size() + 1u);
        warmed_collision_anchors.push_back(spawn_anchor);
        std::size_t warmed_avatar_horizons = 0;
        for (const World::PlayerAvatar& avatar : m_avatars) {
            bool covered = false;
            for (const Vec3& warmed_anchor : warmed_collision_anchors) {
                if (HorizontalChunkHorizonCovers(
                        warmed_anchor, avatar.position, avatar_collision_radius)) {
                    covered = true;
                    break;
                }
            }
            if (covered) {
                continue;
            }

            const bool avatar_ready = world_system->EnsureSurfaceReadyNear(
                avatar.position, physics_system, avatar_surface_radius, avatar_collision_radius);
            if (!avatar_ready) {
                LUMINUMBRA_CORE_ERROR("HeadServerWorldRunner: avatar collision horizon failed to "
                                      "become ready for player {}",
                                      avatar.player_id);
                m_session.reset();
                m_jobSystem.shutdown();
                return false;
            }
            warmed_collision_anchors.push_back(avatar.position);
            ++warmed_avatar_horizons;
        }
        if (warmed_avatar_horizons > 0u) {
            LUMINUMBRA_CORE_INFO("HeadServerWorldRunner: warmed {} additional avatar collision "
                                 "horizon(s) (surface_radius={}, collision_radius={}).",
                                 warmed_avatar_horizons,
                                 avatar_surface_radius,
                                 avatar_collision_radius);
        }
    }

    // gate-populated-world-replay: spawn the deterministic KINEMATIC
    // creature roster into the SAME registry GameSession::TickSimulation ticks, so
    // the hardened ecology stack runs LIVE in the headless binary. Opt-in
    // (ecology_roster); default false leaves the roster empty -> the ecology
    // sub-hash is empty/neutral and the composite world_hash differs from pre-fold
    // only by the appended `|ecology:` suffix (additivity guard). The roster is
    // spawned AFTER the surface horizon is ready (anchor Y resolved), though the
    // kinematic lane never queries terrain.
    if (m_config.ecology_roster) {
        SpawnEcologyRoster(m_session->GetRegistry(), spawn_anchor);
        LUMINUMBRA_CORE_INFO("HeadServerWorldRunner: spawned deterministic ecology roster ({} "
                             "creatures, kinematic).",
                             CreatureCount());
    }

    //  opt-in plant roster so the smoke exercises the plant sub-hash + growth +
    // persistence end-to-end (empty otherwise -> plant_hash neutral). Pure fn of (seed, preset).
    if (m_config.planted_roster) {
        SpawnPlantRoster(m_session->GetRegistry(), spawn_anchor);
        LUMINUMBRA_CORE_INFO(
            "HeadServerWorldRunner: spawned deterministic plant roster (6 plants).");
    }

    // --- Water determinism warm-up: settle water to a steady state BEFORE the counted sim ---
    // The water sim only sleeps after 120 consecutive calm ticks, but a short run (e.g. the
    // 90-tick --smoke) measures water MID-settle, so each chunk's depth depends on WHICH tick
    // it streamed in (async meshing arrival). That made the WATER sub-hash flake cold-vs-warm
    // and would desync two lockstep peers with different timing (water is a halting peer hash
    // oracle). Here we drive streaming + water to a settled steady state on the SHARED boot
    // path both peers run, so the subsequent sim starts from the (trajectory-independent)
    // equilibrium. Bounded: settle residency (stable chunk count), then run water until every
    // chunk is calm (asleep) or a hard cap. If water does NOT reach a static fixed point this
    // will hit the cap and the smoke will still flake — that result decides option C vs B'
    // Server/lockstep boot only; interactive streaming uses the normal residency path.
    {
        auto* ws = m_session->GetWorldSystem();
        auto* phys = m_session->GetPhysicsSystem();
        auto stream_once = [&]() {
            if (m_avatars.empty()) {
                ws->update(m_session->GetRegistry(), spawn_anchor, phys);
            } else {
                std::vector<Vec3> anchors;
                anchors.reserve(m_avatars.size());
                for (const World::PlayerAvatar& a : m_avatars)
                    anchors.push_back(a.position);
                ws->update(m_session->GetRegistry(), anchors, phys);
            }
            ws->wait_for_streaming_jobs();
        };
        //  settle chunk residency (count stable for several consecutive iters).
        std::size_t last_count = static_cast<std::size_t>(-1);
        int stable = 0;
        for (int i = 0; i < 400 && stable < 8; ++i) {
            stream_once();
            const std::size_t count = ws->snapshot_streamed_chunks().size();
            stable = (count == last_count) ? stable + 1 : 0;
            last_count = count;
        }
        // FRESH worlds only — complete water INIT and drain the
        // initial flood transient. Boot-settle mode lifts the live-play init/sim caps
        // (see WaterSystem::SetBootSettleMode): under them, init drains at 6/tick while
        // thousands of chunks wait, and the 64-chunk rotating sim window makes the
        // 120-calm-tick sleep threshold unreachable for a large awake set. A GLOBAL
        // all-asleep fixed point does NOT exist for this solver (wet/dry boundary cells
        // limit-cycle and wake propagation re-wakes neighbours — measured: awake GROWS
        // past 2500/5433 even after 3000 full-set iterations), so the settle contract
        // is: (a) every streamed chunk water-initialized (uninited == 0) and (b) a
        // FIXED, deterministic transient budget of full-set sim ticks — NOT "wait for
        // calm", which never terminates. Reproducibility across save/load comes from
        // the loaded-boot water pause + the persisted sim-window cursor, not from
        // reaching a (nonexistent) static equilibrium.
        //
        // LOADED worlds skip this phase entirely: their water is paused for the whole
        // Boot (see above) — the restored mid-flow state must not be advanced.
        int settle_iters = 0;
        if (!water_loaded_from_save) {
            ws->SetWaterBootSettleMode(true);
            // 2a: init the full backlog (boot mode seeds ALL pending per update; the
            // loop tolerates late streamers). Bounded.
            constexpr int kInitSettleCap = 50;
            std::size_t uninited_now = 0;
            for (int i = 0; i < kInitSettleCap; ++i) {
                ++settle_iters;
                stream_once();
                uninited_now = 0;
                for (const auto& c : ws->snapshot_streamed_chunks()) {
                    if (c && !c->has_water_sim.load(std::memory_order_acquire))
                        ++uninited_now;
                }
                if (uninited_now == 0)
                    break;
            }
            // 2b: exactly kPostInitSettleTicks full-set sim iterations — the flood
            // transient (lakes/rivers redistributing from their seeded rest levels)
            // drains and the calm-able majority of chunks reaches sleep. A fixed count
            // is deterministic (run==replay, host==peer) and bounded by construction.
            constexpr int kPostInitSettleTicks = 240;
            for (int i = 0; i < kPostInitSettleTicks; ++i) {
                ++settle_iters;
                stream_once();
            }
            ws->SetWaterBootSettleMode(false);
        }
        // Settle-exit summary: the heavy oracle asserts the settle CONTRACT —
        // fresh boots leave zero uninitialized chunks; loaded boots skip the settle
        // (water paused, state preserved). awake > 0 is EXPECTED (the network flows
        // perpetually); reproducibility comes from the pause + persisted cursor.
        {
            std::size_t water_chunks = 0, awake = 0, uninited = 0;
            for (const auto& c : ws->snapshot_streamed_chunks()) {
                if (!c)
                    continue;
                if (c->has_water_sim.load(std::memory_order_acquire)) {
                    ++water_chunks;
                    if (!c->is_water_sleeping.load(std::memory_order_relaxed))
                        ++awake;
                } else {
                    ++uninited;
                }
            }
            LUMINUMBRA_CORE_INFO(
                "Boot water settle exit: {} water-inited ({} awake), {} NOT water-inited, {} "
                "chunks total{}",
                water_chunks,
                awake,
                uninited,
                ws->snapshot_streamed_chunks().size(),
                water_loaded_from_save ? " [loaded: water settle skipped, state preserved]" : "");
            m_boot_settle.water_chunks = water_chunks;
            m_boot_settle.awake = awake;
            m_boot_settle.uninited = uninited;
            m_boot_settle.iterations = settle_iters;
            m_boot_settle.water_settle_skipped = water_loaded_from_save;
        }
    }

    // live ticks resume the loaded water state exactly where the save left it.
    if (water_loaded_from_save) {
        world_system->SetWaterBootPaused(false);
    }

    m_booted = true;
    return true;
}

void HeadServerWorldRunner::SetAvatarMove(std::uint32_t player_id, float move_x, float move_z) {
    if (!m_session)
        return;
    auto* physics = m_session->GetPhysicsSystem();
    if (!physics)
        return;
    // Normalized input -> wish velocity. A gentle walk speed (zen game, not a sprinter).
    constexpr float kWalkSpeedMs = 4.0f;
    physics->set_avatar_wish_velocity(static_cast<std::size_t>(player_id),
                                      glm::vec2(move_x, move_z) * kWalkSpeedMs);
}

ServerTickReport HeadServerWorldRunner::RunFixedTicks(std::uint64_t tick_count) {
    ServerTickReport report;
    if (!m_booted || !m_session) {
        return report;
    }

    auto* world_system = m_session->GetWorldSystem();
    auto* physics_system = m_session->GetPhysicsSystem();
    const Vec3 spawn_anchor = m_session->GetMetadata().spawnPoint;
    const double fixed_dt = m_session->GetSimulationClock().fixed_dt();

    // per-tick main-thread blocking-wait samples (ms) at the streaming barrier.
    std::vector<double> wait_samples;
    wait_samples.reserve(static_cast<std::size_t>(tick_count));

    // water-smoke: wet walk start (pure worldgen samplers; see FindWetAnchor) and
    // per-tick water sub-phase samples (ms). Empty vectors when the lane is off.
    Vec3 water_walk_start = spawn_anchor;
    std::vector<double> water_init_samples, water_sim_samples, water_seam_samples,
        water_book_samples, water_total_samples;
    if (m_config.water_smoke) {
        report.water.enabled = true;
        water_walk_start = FindWetAnchor(*world_system, spawn_anchor);
        const auto n = static_cast<std::size_t>(tick_count);
        water_init_samples.reserve(n);
        water_sim_samples.reserve(n);
        water_seam_samples.reserve(n);
        water_book_samples.reserve(n);
        water_total_samples.reserve(n);
    }

    const auto wall_start = std::chrono::steady_clock::now();
    while (report.ticks_executed < tick_count) {
        if (const auto& region_fault = m_session->GetRegionSchedulerFault();
            region_fault.has_value()) {
            report.region_fault = true;
            report.region_fault_message = region_fault.value().message;
            break;
        }
        LUMIN_PROFILE_ZONE_N("server_tick"); // no-op unless LUMINUMBRA_ENABLE_TRACY
        // One frame == one fixed tick: feeding the clock exactly fixed_dt
        // keeps the frame/tick mapping 1:1 and removes wall-clock timing from
        // the simulation entirely (determinism discipline).
        using BudgetStage = luminumbra::simulation::SimBudgetStage;
        auto& budget = m_session->GetSimBudgetTelemetry();
        const std::uint64_t budget_tick = m_session->GetSimulationTickCount() + 1;
        luminumbra::simulation::SimBudgetTelemetry::TickScope physics_budget(budget, budget_tick);
        physics_budget.Next(BudgetStage::ServerPhysics, [] { return 1u; });
        physics_system->update(static_cast<float>(fixed_dt));
        //  step the server-authoritative avatar characters (gravity +
        // world collision; deterministic index order) and read their settled
        // transforms back into m_avatars, so the streaming anchors + the
        // `entities` sub-hash reflect the physics-authoritative positions.
        if (!m_avatars.empty()) {
            physics_system->update_avatars(static_cast<float>(fixed_dt));
            for (std::size_t i = 0; i < m_avatars.size(); ++i) {
                m_avatars[i].position = physics_system->get_avatar_position(i);
                m_avatars[i].velocity = physics_system->get_avatar_velocity(i);
            }
        }
        physics_budget.Finish();
        if (m_session->ActiveRegionsEnabled()) {
            std::vector<Vec3> anchors;
            for (const auto& avatar : m_avatars)
                anchors.push_back(avatar.position);
            m_session->SetReplicatedSimulationAnchors(std::move(anchors));
        }
        report.ticks_executed += m_session->TickSimulation(fixed_dt);
        report.frames_executed += 1;
        if (const auto& fault = m_session->GetRegionSchedulerFault()) {
            report.region_fault = true;
            report.region_fault_message = fault->message;
            break;
        }
        const std::uint64_t simulation_tick = m_session->ActiveRegionsEnabled()
                                                  ? m_session->GetSimulationTickCount()
                                                  : report.ticks_executed;

        // Supply the simulation tick to the activation queue so dispatched
        // batches receive deterministic due ticks.
        world_system->begin_tick(static_cast<std::int64_t>(simulation_tick));
        // Drive activation-latency telemetry from the same tick base used by
        // the availability digest.
        // Observability only — gated on --avail-trace like the digest itself.
        if (m_config.availability_trace) {
            world_system->begin_tick_shadow(static_cast<std::int64_t>(simulation_tick));
        }

        // Spawn-anchor streaming, then quiesce in-flight generation/meshing
        // so every scheduler decision next frame observes the identical
        // settled state in both determinism runs. With avatars, stream
        // around the UNION of avatar positions (multi-anchor); with none, the
        // single spawn anchor via the Vec3 overload (byte-identical to zero-avatar).
        ServerStreamingBudget streaming_budget(budget);
        streaming_budget.MeasureUpdate([&] {
            if (m_avatars.empty()) {
                Vec3 anchor = spawn_anchor;
                if (m_config.water_smoke) {
                    // water-smoke walk: +X 2 m/tick from the wet anchor, so the run keeps
                    // streaming fresh columns THROUGH/along the wet body — first-time grid
                    // inits, the rotating sim window, and cross-chunk seam flux all engage
                    // every activation. Pure fn of the tick -> run==replay holds.
                    const float d = 2.0f * static_cast<float>(report.ticks_executed);
                    anchor = Vec3(water_walk_start.x + d, water_walk_start.y, water_walk_start.z);
                } else if (m_config.moving_anchor) {
                    // B' harness: walk the streaming anchor +X/+Z each tick so chunks stream IN
                    // ahead and OUT behind during the run. ~0.5 m/tick -> ~45 m over 90 ticks (~3
                    // chunks). Pure fn of the tick -> both --smoke runs drift identically; any
                    // run!=replay is the moving-case water nondeterminism (boot warm-up only
                    // settles the INITIAL residency).
                    const float d = 0.5f * static_cast<float>(report.ticks_executed);
                    anchor = Vec3(spawn_anchor.x + d, spawn_anchor.y, spawn_anchor.z + d);
                }
                world_system->update(m_session->GetRegistry(), anchor, physics_system);
            } else {
                std::vector<Vec3> anchors;
                anchors.reserve(m_avatars.size());
                for (const World::PlayerAvatar& a : m_avatars) {
                    anchors.push_back(a.position);
                }
                world_system->update(m_session->GetRegistry(), anchors, physics_system);
            }
        });
        // water-smoke: sample the water sub-phase timings + sim-load counters the
        // update above just produced. Const reads of never-hashed telemetry (the same
        // read class as the availability digest) — hash-neutral, off by default.
        if (m_config.water_smoke) {
            if (const Systems::WaterSystem* water = m_session->GetWaterSystem()) {
                const auto& wt = water->dbg_water_timings();
                water_init_samples.push_back(wt.init);
                water_sim_samples.push_back(wt.sim);
                water_seam_samples.push_back(wt.seam);
                water_book_samples.push_back(wt.bookkeeping);
                water_total_samples.push_back(wt.init + wt.sim + wt.seam + wt.bookkeeping);
                report.water.cells_simmed_total += water->dbg_cells_simmed();
                report.water.awake_chunks_max =
                    std::max(report.water.awake_chunks_max, water->dbg_awake_water_chunks());
                report.water.mass_ok = report.water.mass_ok && water->dbg_mass_ok();
                report.water.seam_wet_pairs_max =
                    std::max(report.water.seam_wet_pairs_max, water->dbg_seam_wet_pairs());
            }
        }
        //  5b (activation queue ): THE SWAP. The per-tick full barrier
        // is replaced by tick-keyed activation — publish exactly the batches
        // due this tick (dispatch + K), blocking only on a due-but-unfinished
        // batch. The activation-wait timing now measures activate_due's block time (the
        // number the queue exists to shrink). Wall-clock, never feeds
        // world_hash. Explicit full drains remain at boot / hash / mutate /
        // teardown sites via wait_for_streaming_jobs.
        const auto _wait_t0 = std::chrono::steady_clock::now();
        {
            LUMIN_PROFILE_ZONE_N(
                "streaming_activate_due"); // the p99 latency the queue exists to shrink
            world_system->activate_due(static_cast<std::int64_t>(simulation_tick));
        }
        const double _wait_ms =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - _wait_t0)
                .count();
        wait_samples.push_back(_wait_ms);
        if (budget.Enabled()) {
            // Include the complete update, including its final resident-chunk scan and
            // queue bookkeeping. Water has its own disjoint bracket; activation is separate.
            const auto& timing = world_system->dbg_stream_timings();
            const auto scheduled =
                world_system->get_last_streaming_budget_stats().scheduled_generation;
            const auto* water = m_session->GetWaterSystem();
            streaming_budget.Record(budget_tick,
                                    timing,
                                    scheduled,
                                    water ? water->cells_stepped_last_update() : 0u,
                                    _wait_ms);
        }
        LUMIN_PROFILE_PLOT("streaming_wait_ms", _wait_ms); // no-op unless LUMINUMBRA_ENABLE_TRACY

        //  gate (runtime audit): record the per-tick availability set right
        // after the barrier settles it. Observability only (off by default); the digest
        // reads the settled snapshot and mutates nothing, so the world_hash is unchanged.
        if (m_config.availability_trace) {
            m_avail_trace.emplace_back(report.ticks_executed, ComputeAvailabilityDigest());
        }

        //  ( water cross-process): per-tick water-state hash for the debug-vs-release
        // WaterCrossBuild gate. Reads the settled water grids only (the same read
        // class as the availability digest); mutates nothing, never feeds world_hash.
        if (m_config.water_hash_trace) {
            m_water_hash_trace.emplace_back(report.ticks_executed,
                                            world_system->debug_water_state_hash().hash);
        }

        std::uint64_t autosave_interval = m_config.autosave_interval_ticks;
        if (autosave_interval == 0 && m_config.autosave_default_when_unset &&
            m_session->ActiveRegionsEnabled()) {
            autosave_interval = kDefaultActiveRegionsAutosaveTicks;
        }
        if (autosave_interval > 0 && simulation_tick > 0 &&
            (simulation_tick % autosave_interval) == 0) {
            // Synchronous host-thread save. Snapshot/shutdown run between RunFixedTicks calls.
            world::WorldStateSaveReport save_report;
            if (m_session->SaveWorldState(&save_report)) {
                report.autosave_passes += 1;
                if (save_report.saved && save_report.chunks_dirty > 0) {
                    report.autosave_writes += 1;
                }
            }
        }

        // Byte-limit pressure: a dirty authoritative chunk could not be parked, so persist now.
        if (m_session->PersistenceSaveRequested()) {
            world::WorldStateSaveReport pressure_report;
            if (m_session->SaveWorldState(&pressure_report)) {
                report.autosave_passes += 1;
                if (pressure_report.saved && pressure_report.chunks_dirty > 0) {
                    report.autosave_writes += 1;
                }
            }
        }

        // One server tick == one frame: delimit it for the profiler timeline.
        LUMIN_PROFILE_FRAME(); // no-op unless LUMINUMBRA_ENABLE_TRACY
    }
    const auto wall_end = std::chrono::steady_clock::now();

    report.simulated_seconds = static_cast<double>(report.ticks_executed) * fixed_dt;
    report.wall_seconds = std::chrono::duration<double>(wall_end - wall_start).count();

    // summarize the per-tick main-thread streaming-wait latency (ms).
    if (!wait_samples.empty()) {
        for (double w : wait_samples)
            report.main_wait_total_ms += w;
        std::sort(wait_samples.begin(), wait_samples.end());
        const auto pct = [&wait_samples](double p) {
            // nearest-rank percentile over the sorted samples
            const std::size_t n = wait_samples.size();
            std::size_t idx = static_cast<std::size_t>(std::ceil(p * static_cast<double>(n)));
            if (idx > 0)
                --idx;
            if (idx >= n)
                idx = n - 1;
            return wait_samples[idx];
        };
        report.main_wait_p50_ms = pct(0.50);
        report.main_wait_p95_ms = pct(0.95);
        report.main_wait_p99_ms = pct(0.99);
        report.main_wait_max_ms = wait_samples.back();
    }

    // water-smoke: summarize the per-tick sub-phase samples (same nearest-rank
    // percentile convention as main_wait above).
    if (m_config.water_smoke) {
        const auto summarize_phase = [](std::vector<double>& samples,
                                        ServerTickReport::WaterPhaseStats& out) {
            if (samples.empty()) {
                return;
            }
            std::sort(samples.begin(), samples.end());
            const auto pct = [&samples](double p) {
                const std::size_t n = samples.size();
                std::size_t idx = static_cast<std::size_t>(std::ceil(p * static_cast<double>(n)));
                if (idx > 0)
                    --idx;
                if (idx >= n)
                    idx = n - 1;
                return samples[idx];
            };
            out.p50_ms = pct(0.50);
            out.p95_ms = pct(0.95);
            out.max_ms = samples.back();
        };
        summarize_phase(water_init_samples, report.water.init);
        summarize_phase(water_sim_samples, report.water.sim);
        summarize_phase(water_seam_samples, report.water.seam);
        summarize_phase(water_book_samples, report.water.bookkeeping);
        summarize_phase(water_total_samples, report.water.total);
        if (report.ticks_executed > 0) {
            report.water.cells_simmed_per_tick =
                static_cast<double>(report.water.cells_simmed_total) /
                static_cast<double>(report.ticks_executed);
        }
    }

    //  shadow (activation queue step 2, ): the activation-latency
    // distribution in SIM TICKS — the empirical basis for the activation
    // queue's fixed pipeline-latency K. Log-only, never hashed.
    if (m_config.availability_trace) {
        if (auto* shadow_world = m_session ? m_session->GetWorldSystem() : nullptr) {
            auto shadow = shadow_world->activation_shadow_report();
            const auto pct_ticks = [](std::vector<std::int64_t>& v, double p) -> std::int64_t {
                if (v.empty()) {
                    return 0;
                }
                std::sort(v.begin(), v.end());
                std::size_t idx =
                    static_cast<std::size_t>(std::ceil(p * static_cast<double>(v.size())));
                if (idx > 0)
                    --idx;
                if (idx >= v.size())
                    idx = v.size() - 1;
                return v[idx];
            };
            auto& gen = shadow.generation_to_ready_ticks;
            auto& promo = shadow.promotion_to_publish_ticks;
            LUMINUMBRA_CORE_INFO("Activation shadow (activation queue K design): gen->Ready n={} "
                                 "p50={} p95={} p99={} max={} ticks; "
                                 "promotion->publish n={} p50={} p99={} max={} ticks; dispatched "
                                 "gen={} promo={} still_pending={}",
                                 gen.size(),
                                 pct_ticks(gen, 0.50),
                                 pct_ticks(gen, 0.95),
                                 pct_ticks(gen, 0.99),
                                 gen.empty() ? 0 : gen.back(),
                                 promo.size(),
                                 pct_ticks(promo, 0.50),
                                 pct_ticks(promo, 0.99),
                                 promo.empty() ? 0 : promo.back(),
                                 shadow.generation_dispatches,
                                 shadow.promotion_dispatches,
                                 shadow.still_pending);
        }
    }
    return report;
}

std::string HeadServerWorldRunner::ComputeAvailabilityDigest() {
    // The AVAILABILITY SET at this tick = which chunks are settled to Ready right after the
    // wait_for_streaming_jobs barrier. We digest the sorted Ready-chunk IDS with FNV-1a —
    // deliberately NOT the chunk CONTENT (sdf/heightmap/mesh): content is a pure function of
    // coords, so a stable set of resident coords implies stable content. This is the cheap,
    // per-tick-affordable proxy the  activation queue must reproduce when it
    // replaces the barrier. Sorting by id makes the digest independent of snapshot/container
    // order. (An earlier draft digested (id, state, lod, collision) tuples; the code below
    // is ids-only by design — see the in-body comment.)
    auto* world_system = m_session ? m_session->GetWorldSystem() : nullptr;
    if (!world_system) {
        return {};
    }
    // The availability set = the COORDS of chunks the sim can actually read this tick, i.e.
    // those settled to ChunkState::Ready after the barrier. We digest sorted coords ONLY —
    // deliberately NOT the transient state/lod/has_collision timing, which can legitimately
    // jitter run-to-run during streaming while converging to the same final world_hash. The
    // activation-queue contract is about WHICH chunks are available per tick, not
    // the micro-timing of their LOD/collision bring-up.
    std::vector<std::int64_t> ready_ids;
    for (const auto& chunk : world_system->snapshot_streamed_chunks()) {
        if (!chunk)
            continue;
        if (chunk->get_state() != ChunkState::Ready)
            continue;
        ready_ids.push_back(
            static_cast<std::int64_t>(::Luminumbra::Chunk::calculate_id(chunk->get_coords())));
    }
    std::sort(ready_ids.begin(), ready_ids.end());

    std::uint64_t h = 1469598103934665603ull; // FNV-1a 64-bit offset basis
    const auto mix = [&h](std::int64_t v) {
        const auto u = static_cast<std::uint64_t>(v);
        for (int b = 0; b < 8; ++b) {
            h ^= (u >> (b * 8)) & 0xffull;
            h *= 1099511628211ull; // FNV-1a 64-bit prime
        }
    };
    mix(static_cast<std::int64_t>(ready_ids.size())); // count first so an empty set is distinct
    for (std::int64_t id : ready_ids)
        mix(id);
    char buf[17];
    std::snprintf(buf, sizeof(buf), "%016llx", static_cast<unsigned long long>(h));
    return std::string(buf);
}

std::string HeadServerWorldRunner::ComputeWorldHash() {
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
    //   +   hash revisionS: fold the wind AND weather sub-hashes
    // into the top-level hash. The chunk hash itself is unchanged; the composite
    // deliberately is not.
    return Persistence::ComposeWorldHash(
        m_session->ActiveRegionsEnabled() ? m_session->ComputeChunkStateHash()
                                          : service.world_hash(state),
        WindSubHash(m_session.get()),
        WeatherSubHash(m_session.get()),
        AetherSubHash(m_session.get()),
        ScentSubHash(m_session.get()),
        ComputeEcologySubHash(),
        m_session->ComputePlantSubHash()); //  plant fold (empty-neutral)
}

Persistence::WorldStreamingStateSubHashes HeadServerWorldRunner::ComputeWorldSubHashes() {
    // per-system sub-hashes over the SAME streamed-chunk snapshot the
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

    // The entities sub-hash is the stable checksum of the canonical ECS snapshot.
    //  that snapshot now carries the deterministic player avatars (empty
    // when avatar_count == 0 -> byte-identical to the zero-avatar terrain/water-only
    // lane, so the default world_hash/entities sub-hash is unchanged).
    const std::string entities_snapshot =
        Ecs::SerializeEntityRegistrySnapshotJson(World::BuildAvatarEntitySnapshot(m_avatars));
    Persistence::WorldStreamingStateSubHashes sub =
        Persistence::ComputeWorldStreamingStateSubHashes(state, entities_snapshot);
    // the wind sub-hash slot, supplied from the session's wind
    // field (not chunk-derived). Present + stable for the WindFieldDeterminism
    // gate and the desync-localization oracle.
    sub.wind = WindSubHash(m_session.get());
    // the weather sub-hash slot, supplied from the session's weather
    // core (not chunk-derived). Present + stable for the WeatherVisual state-hash
    // assertion and the desync-localization oracle.
    sub.weather = WeatherSubHash(m_session.get());
    // the aether sub-hash slot, supplied from the session's Aether
    // scalar field (not chunk-derived). Present + stable for the
    // AetherFieldDeterminism gate and the desync-localization oracle.
    sub.aether = AetherSubHash(m_session.get());
    // the STATE-ONLY stateful-layer sub-hash — the heavy
    // oracle's authoritative-compare slot when sim.aether_state is ON.
    // Always empty on the default (OFF) world.
    sub.aether_state = m_session ? m_session->ComputeAetherStateSubHash() : std::string();
    return sub;
}

void HeadServerWorldRunner::ComputeWorldHashAndSubHashes(
    std::string& out_world_hash, Persistence::WorldStreamingStateSubHashes& out_sub) {
    // single quiesce + single chunk snapshot feeding BOTH hashes. This
    // is the recorder/replayer's mid-run checkpoint capture; minimizing the
    // settled-state reads keeps the capture window tight. The produced values are
    // byte-identical to ComputeWorldHash and ComputeWorldSubHashes called
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
    const std::string aether_hash = AetherSubHash(m_session.get());
    const std::string scent_hash = ScentSubHash(m_session.get());
    const std::string ecology_hash = ComputeEcologySubHash();
    const std::string plant_hash = m_session->ComputePlantSubHash(); //  empty-neutral

    Persistence::WorldSaveService service;
    //   +   +  + gate-populated-world-replay +
    // hash revisionS: composite world_hash (chunk + wind + weather + aether + scents + ecology +
    // plants).
    out_world_hash = Persistence::ComposeWorldHash(m_session->ActiveRegionsEnabled()
                                                       ? m_session->ComputeChunkStateHash()
                                                       : service.world_hash(state),
                                                   wind_hash,
                                                   weather_hash,
                                                   aether_hash,
                                                   scent_hash,
                                                   ecology_hash,
                                                   plant_hash);

    const std::string entities_snapshot =
        Ecs::SerializeEntityRegistrySnapshotJson(World::BuildAvatarEntitySnapshot(m_avatars));
    out_sub = Persistence::ComputeWorldStreamingStateSubHashes(state, entities_snapshot);
    out_sub.wind = wind_hash;
    out_sub.weather = weather_hash;
    out_sub.aether = aether_hash;
    // state-only slot for LREC1 checkpoint localization
    // (always empty on the default OFF world).
    out_sub.aether_state = m_session->ComputeAetherStateSubHash();
}

std::size_t HeadServerWorldRunner::SaveFullSnapshot() {
    // Host-thread-only, after RunFixedTicks returns; never dispatch a save to workers.
    // write the FULL in-memory streamed-chunk set (not dirty-gated) so
    // a loaded session can adopt exactly this set. Reuses WorldSaveService.
    if (!m_booted || !m_session || !m_session->GetWorldSystem() ||
        !m_session->IsSimulationTickBoundary()) {
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
    // Flag on: the chunk snapshot, the session state and the metadata are ONE snapshot
    // transaction (a single index publication); flag off just runs the body.
    const bool saved = m_session->RunInSnapshotTransaction([&]() {
        std::vector<std::string> errors;
        if (!service.save_world(state, save_dir, &errors)) {
            for (const std::string& error : errors) {
                LUMINUMBRA_CORE_ERROR("SaveFullSnapshot failed: {}", error);
            }
            return false;
        }
        // refresh world_info.json alongside the chunk snapshot so world-level
        // sim state captured there (the water sim-window cursor) reflects THIS save
        // moment, not world creation. Without this a loaded session resimulates from a
        // stale cursor and the water evolution diverges from the original's.
        if (m_session->ActiveRegionsEnabled() && !m_session->SaveWorldState())
            return false;
        const bool metadata_saved = m_session->SaveWorld();
        return !m_session->ActiveRegionsEnabled() || metadata_saved;
    });
    if (!saved)
        return 0;
    return state.size();
}

std::size_t HeadServerWorldRunner::StreamedChunkCount() {
    if (!m_session || !m_session->GetWorldSystem()) {
        return 0;
    }
    return m_session->GetWorldSystem()->snapshot_streamed_chunks().size();
}

std::size_t HeadServerWorldRunner::LoadedChunkCount() const {
    return m_session ? m_session->GetLastLoadedChunkCount() : 0;
}

std::uint64_t HeadServerWorldRunner::TickCount() const {
    return m_session ? m_session->GetSimulationTickCount() : 0;
}

std::string HeadServerWorldRunner::ComputeEcologySubHash() const {
    // gate-populated-world-replay: id-ordered hash over the live creature roster
    // (empty/neutral when none). Pure read over the session registry; no quiesce
    // needed (creature state is registry-resident, not chunk-derived).
    if (!m_session) {
        return {};
    }
    return m_session->FoldClockIntoEcologyHash(
        luminumbra::ai::ComputeEcologySubHash(m_session->GetRegistry()));
}

std::size_t HeadServerWorldRunner::CreatureCount() const {
    if (!m_session) {
        return 0;
    }
    return m_session->GetRegistry()
        .view<const ::Luminumbra::Components::CreatureComponent>()
        .size();
}

void HeadServerWorldRunner::Shutdown(world::WorldStateSaveReport* shutdown_save_report) {
    if (m_shutdown) {
        return;
    }
    m_shutdown = true;

    if (m_booted && m_session) {
        // The host tick loop has returned, so this cannot overlap autosave/snapshot.
        // Save on shutdown via WorldSaveService (incremental contract: a
        // never-edited world writes nothing and stays on the fresh path).
        world::WorldStateSaveReport save_report;
        m_session->SaveWorldState(&save_report);
        if (shutdown_save_report) {
            *shutdown_save_report = save_report;
        }
        LUMINUMBRA_CORE_INFO(
            "HeadServerWorldRunner: shutdown save - {} chunks total, {} dirty, saved={}",
            save_report.chunks_total,
            save_report.chunks_dirty,
            save_report.saved);
    }

    m_session.reset();
    if (m_booted) {
        m_jobSystem.shutdown();
    }
    m_booted = false;
}

} // namespace Luminumbra::Server
// END frozen HEAD runner oracle.

// Hoist Replay.cpp's headers above so all of its definitions are isolated here.
namespace CreatureHostAcceptanceReplay {
#include "../../src/luminumbra_server/modes/Replay.cpp"
} // namespace CreatureHostAcceptanceReplay

namespace {
namespace fs = std::filesystem;
namespace C = Luminumbra::Components;
namespace A = luminumbra::ai;
namespace W = Luminumbra::world;
namespace S = Luminumbra::Server;

class CreatureHostAcceptance : public testing::Test {
protected:
    fs::path root;
    void SetUp() override {
        root = Luminumbra::TestSupport::CreateUniqueTempDirectory("creature_host_");
        Luminumbra::TestSupport::WriteSimulationContentFixture(root);
        fs::create_directories(root / "worlds/atlas/presets");
        fs::copy_file(fs::path(LUMINUMBRA_SOURCE_ROOT) / "worlds/atlas/presets/default.json",
                      root / "worlds/atlas/presets/default.json");
        for (const auto* name : {"grovestrider", "ridgeback_stalker"})
            std::ofstream(root / "data/common/creatures/species" / (std::string(name) + ".json"))
                << "{\"id\":\"" << name << "\"}";
        std::ofstream(root / "data/common/systems.json")
            << R"({"sim":{"active_regions":{"enabled":true}}})";
    }
    void TearDown() override {
        std::error_code error;
        fs::remove_all(root, error);
    }
    S::ServerWorldRunnerConfig Config() const {
        S::ServerWorldRunnerConfig config;
        config.root_path = root.generic_string() + "/";
        config.seed = "1337";
        config.surface_radius = 1;
        config.collision_radius = 1;
        config.test_streaming_radius_cap = 1;
        config.ecology_roster = true;
        return config;
    }
};

TEST_F(CreatureHostAcceptance, FreshEnabledServerUsesRegisteredPopulationContract) {
    S::ServerWorldRunner runner(Config());
    ASSERT_TRUE(runner.Boot()) << runner.GetBootError();
    ASSERT_NE(runner.Session()->GetSimulationContent(), nullptr);
    EXPECT_EQ(runner.TickCount(), 0u);
    EXPECT_EQ(runner.CreatureCount(), 0u);
    EXPECT_EQ(runner.Session()->GetPopulationActivity().successes, 0u);
    S::ServerWorldRunner peer(Config());
    ASSERT_TRUE(peer.Boot()) << peer.GetBootError();
    EXPECT_EQ(peer.CreatureProjection(), runner.CreatureProjection());
    EXPECT_EQ(peer.ComputeWorldHash(), runner.ComputeWorldHash());
    EXPECT_EQ(peer.Session()->GetSimulationContent()->identity,
              runner.Session()->GetSimulationContent()->identity);
    EXPECT_NE(W::BuildSimulationContentIdentity(*runner.Session()->GetSimulationContent())
                  .population_digest,
              runner.Session()->GetSimulationContent()->identity.population_digest);
    const auto before = runner.CreatureProjection();
    EXPECT_EQ(runner.ComputeEcologySubHash(),
              runner.Session()->FoldClockIntoEcologyHash(
                  A::ComputeEcologySubHash(runner.Session()->GetRegistry(), true)));
    EXPECT_EQ(runner.CreatureProjection(), before);
    EXPECT_EQ(runner.RunFixedTicks(1).ticks_executed, 1u);
    EXPECT_EQ(runner.CreatureCount(), 8u);
    EXPECT_GT(runner.Session()->GetPopulationActivity().successes, 0u);
    EXPECT_TRUE(runner.Session()->GetActiveRegionLedger().records().at({0, 0}).populated);
    ASSERT_EQ(peer.RunFixedTicks(1).ticks_executed, 1u);
    EXPECT_EQ(peer.CreatureProjection(), runner.CreatureProjection());
    EXPECT_EQ(peer.ComputeWorldHash(), runner.ComputeWorldHash());
}

TEST_F(CreatureHostAcceptance, RestoredEnabledServerDoesNotRespawnRoster) {
    auto config = Config();
    std::string expected, world_hash, ecology;
    std::uint64_t next_id = 0;
    {
        S::ServerWorldRunner original(config);
        ASSERT_TRUE(original.Boot());
        ASSERT_EQ(original.RunFixedTicks(2).ticks_executed, 2u);
        config.world_id = original.Session()->GetMetadata().worldId;
        ASSERT_GT(original.SaveFullSnapshot(), 0u);
        expected = original.CreatureProjection();
        ecology = original.ComputeEcologySubHash();
        world_hash = original.ComputeWorldHash();
        next_id = original.Session()->GetActiveRegionLedger().next_persistent_id();
    }
    S::ServerWorldRunner restored(config);
    ASSERT_TRUE(restored.Boot()) << restored.GetBootError();
    EXPECT_EQ(restored.CreatureCount(), 8u);
    EXPECT_EQ(restored.CreatureProjection(), expected);
    EXPECT_EQ(restored.ComputeEcologySubHash(), ecology);
    EXPECT_EQ(restored.ComputeWorldHash(), world_hash);
    EXPECT_EQ(restored.Session()->GetActiveRegionLedger().next_persistent_id(), next_id);
    EXPECT_EQ(restored.Session()->GetPopulationActivity().successes, 0u);
    ASSERT_EQ(restored.RunFixedTicks(1).ticks_executed, 1u);
    EXPECT_EQ(restored.CreatureCount(), 8u);
    EXPECT_EQ(restored.Session()->GetActiveRegionLedger().next_persistent_id(), next_id);
}

std::string ReadAcceptanceFile(const fs::path& path) {
    std::ifstream input(path, std::ios::binary);
    EXPECT_TRUE(input.is_open()) << path;
    std::ostringstream bytes;
    bytes << input.rdbuf();
    return bytes.str();
}
std::map<std::string, std::string> AcceptanceSaveFiles(const fs::path& directory) {
    std::map<std::string, std::string> files;
    for (const auto& entry : fs::recursive_directory_iterator(directory))
        if (entry.is_regular_file())
            files.emplace(entry.path().lexically_relative(directory).generic_string(),
                          ReadAcceptanceFile(entry.path()));
    return files;
}

TEST_F(CreatureHostAcceptance, FlagOffHostSaveFileCountHashAndArtifactBytesMatchGolden) {
    std::ofstream(root / "data/common/systems.json")
        << R"({"sim":{"active_regions":{"enabled":false}}})";
    auto config = Config();
    config.avatar_count = 2;
    S::ServerWorldRunner actual(config);
    S::HeadServerWorldRunner golden(config);
    ASSERT_TRUE(actual.Boot()) << actual.GetBootError();
    ASSERT_TRUE(golden.Boot()) << golden.GetBootError();
    ASSERT_FALSE(actual.Session()->ActiveRegionsEnabled());
    ASSERT_FALSE(golden.Session()->ActiveRegionsEnabled());
    ASSERT_EQ(actual.CreatureCount(), 8u);
    ASSERT_EQ(golden.CreatureCount(), 8u);
    for (auto* session : {actual.Session(), golden.Session()})
        const_cast<Luminumbra::world::WorldMetadata&>(session->GetMetadata()).creationTime =
            123456789;
    EXPECT_EQ(actual.ComputeWorldHash(), golden.ComputeWorldHash());
    for (unsigned tick = 0; tick < 3; ++tick) {
        actual.SetAvatarMove(1, 1.0f, 0.0f);
        golden.SetAvatarMove(1, 1.0f, 0.0f);
        ASSERT_EQ(actual.RunFixedTicks(1).ticks_executed, 1u);
        ASSERT_EQ(golden.RunFixedTicks(1).ticks_executed, 1u);
        EXPECT_EQ(actual.ComputeWorldHash(), golden.ComputeWorldHash());
        EXPECT_EQ(actual.ComputeEcologySubHash(), golden.ComputeEcologySubHash());
        EXPECT_EQ(actual.ComputeWorldSubHashes().entities, golden.ComputeWorldSubHashes().entities);
        EXPECT_EQ(actual.Avatars()[1].position, golden.Avatars()[1].position);
        EXPECT_EQ(actual.Avatars()[1].velocity, golden.Avatars()[1].velocity);
    }
    const auto saved = actual.SaveFullSnapshot();
    ASSERT_GT(saved, 0u);
    EXPECT_EQ(saved, golden.SaveFullSnapshot());
    const auto actual_files = AcceptanceSaveFiles(actual.Session()->GetWorldSaveDir());
    const auto golden_files = AcceptanceSaveFiles(golden.Session()->GetWorldSaveDir());
    ASSERT_FALSE(golden_files.empty());
    EXPECT_EQ(actual_files.size(), golden_files.size());
    EXPECT_EQ(actual_files, golden_files);
    actual.Shutdown();
    golden.Shutdown();

    // The LREC1 header has no ambient/player roster settings. Generate its
    // start/checkpoint hashes using the frozen HEAD runner's default roster.
    config.avatar_count = 0;
    config.ecology_roster = false;
    S::HeadServerWorldRunner recording(config);
    ASSERT_TRUE(recording.Boot()) << recording.GetBootError();
    Luminumbra::Replay::ReplayHeader header;
    header.seed_string = config.seed;
    header.seed = 1337;
    header.preset = config.preset;
    header.surface_radius = 1;
    header.collision_radius = 1;
    header.engine_version = std::string(luminumbra::core::GetEngineVersionString());
    header.system_config_identity = recording.Session()->SystemConfigIdentity();
    header.preset_content_hash = recording.Session()->PresetContentHash();
    header.start_world_hash = recording.ComputeWorldHash();
    ServerCliOptions options;
    options.root = config.root_path;
    options.test_streaming_radius_cap = 1;
    options.replay_path = (root / "head-golden.lrec1").string();
    options.artifact_path = (root / "actual-replay.json").string();
    Luminumbra::Replay::ReplayWriter writer;
    ASSERT_TRUE(writer.Open(options.replay_path, header));
    for (std::uint64_t tick = 1; tick <= 30; ++tick) {
        writer.RecordInput(tick, {});
        ASSERT_EQ(recording.RunFixedTicks(1).ticks_executed, 1u);
    }
    Luminumbra::Replay::CheckpointRecord checkpoint;
    checkpoint.tick = 30;
    Luminumbra::Persistence::WorldStreamingStateSubHashes sub;
    recording.ComputeWorldHashAndSubHashes(checkpoint.world_hash, sub);
    checkpoint.terrain = sub.terrain;
    checkpoint.water = sub.water;
    checkpoint.entities = sub.entities;
    writer.RecordCheckpoint(checkpoint);
    ASSERT_TRUE(writer.Finalize(30));
    recording.Shutdown();
    // Compare the complete real production artifact, without stripping fields
    // or substituting hashes captured from the changed runner.
    ASSERT_EQ(CreatureHostAcceptanceReplay::RunReplay(options), 0);
    const nlohmann::json expected{{"schema", "luminumbra.replay_roundtrip.v1"},
                                  {"generated_by", "luminumbra_server_app --replay ()"},
                                  {"replay_path", options.replay_path},
                                  {"diverged", false},
                                  {"engine_version", header.engine_version},
                                  {"replayed_engine_version", header.engine_version},
                                  {"preset", header.preset},
                                  {"seed", header.seed_string},
                                  {"tick_rate_hz", header.tick_rate_hz},
                                  {"start_world_hash", header.start_world_hash},
                                  {"start_world_hash_match", true},
                                  {"ticks_replayed", 30},
                                  {"tick_count", 30},
                                  {"checkpoints_verified", 1},
                                  {"input_records", 30},
                                  {"end_world_hash", checkpoint.world_hash},
                                  {"end_hash_match", true},
                                  {"passed", true}};
    EXPECT_EQ(ReadAcceptanceFile(options.artifact_path), expected.dump(2) + "\n");
}

TEST_F(CreatureHostAcceptance, MixedPlayersAndCreaturesStepExactlyOnce) {
    auto config = Config();
    config.ecology_roster = false;
    config.avatar_count = 2;
    S::ServerWorldRunner runner(config);
    ASSERT_TRUE(runner.Boot());
    auto& session = *runner.Session();
    auto* physics = session.GetPhysicsSystem();
    ASSERT_NE(physics, nullptr);
    const auto position = runner.Avatars()[0].position + Luminumbra::Vec3(0.0f, 100.0f, 0.0f);
    session.NotifyGroundObjectEdit(position);
    const auto creature = session.GetRegistry().create();
    session.GetRegistry().emplace<C::TransformComponent>(creature).position = position;
    session.GetRegistry().emplace<C::CreatureComponent>(creature).species_id =
        C::CreatureSpeciesId16("fixture_creature");
    session.GetRegistry().get<C::CreatureComponent>(creature).move_speed = 0.0f;
    ASSERT_TRUE(session.AdmitCreature(creature, true));
    // An unrelated controller must remain untouched by both selected lanes.
    const auto sentinel = physics->create_avatar_handle(position, {0.0f, 2.0f, 0.0f}, false);
    const auto sentinel_before = physics->get_avatar_position(sentinel);
    runner.SetAvatarMove(1, 1.0f, 0.0f);
    runner.SetAvatarMove(900, 1.0f, 1.0f);
    const auto start = runner.Avatars();
    ASSERT_EQ(runner.RunFixedTicks(1).ticks_executed, 1u);
    ASSERT_FALSE(session.GetRegionSchedulerFault());
    EXPECT_EQ(physics->get_avatar_position(sentinel), sentinel_before);
    EXPECT_GT(runner.Avatars()[1].position.x, start[1].position.x);
    const auto* runtime = session.GetRegistry().try_get<C::CreaturePhysicsComponent>(creature);
    ASSERT_NE(runtime, nullptr);
    const auto& continuation =
        session.GetRegistry().get<C::CreatureLocomotionStateComponent>(creature);
    // With the same initial Y and zero wish, gravity is one fixed step for both lanes.
    EXPECT_FLOAT_EQ(continuation.velocity_y, runner.Avatars()[0].velocity.y);
    EXPECT_FLOAT_EQ(continuation.velocity_y, physics->get_avatar_velocity(runtime->avatar_index).y);
    EXPECT_EQ(session.GetCreatureActivity().full_updates, 1u);
}

TEST_F(CreatureHostAcceptance, RestoredControlledRosterContinuesBeforeFirstMovementStep) {
    auto config = Config();
    config.ecology_roster = false;
    config.avatar_count = 1;
    W::RegionPopulator population;
    population.contract = "controlled-founder:v1";
    population.canonical_configuration =
        R"({"owner":[0,0],"position_m":[8,200,8],"species":"fixture_creature","physics_controlled":true})";
    population.populate = [](const W::RegionPopulateRequest& request) {
        if (request.key == W::RegionKey{0, 0})
            request.sink.spawn(W::CreatureSpawnCommand{C::CreatureSpeciesId16("fixture_creature"),
                                                       Luminumbra::Vec3(8.0f, 200.0f, 8.0f),
                                                       true});
    };
    config.region_populator = std::move(population);
    S::ServerWorldRunner uninterrupted(config);
    ASSERT_TRUE(uninterrupted.Boot()) << uninterrupted.GetBootError();
    ASSERT_EQ(uninterrupted.RunFixedTicks(1).ticks_executed, 1u);
    // Population occurs on tick one. Newborns enter the controller/movement
    // lanes on the next real tick, without a tick-zero population mode.
    ASSERT_EQ(uninterrupted.CreatureCount(), 1u);
    ASSERT_EQ(uninterrupted.RunFixedTicks(1).ticks_executed, 1u);
    const auto& registry = uninterrupted.Session()->GetRegistry();
    const auto controlled =
        registry
            .view<const C::CreaturePhysicsComponent, const C::CreatureLocomotionStateComponent>();
    ASSERT_EQ(controlled.size_hint(), 1u);
    for (const auto entity : controlled)
        EXPECT_LT(controlled.get<const C::CreatureLocomotionStateComponent>(entity).velocity_y,
                  0.0f);
    config.world_id = uninterrupted.Session()->GetMetadata().worldId;
    ASSERT_GT(uninterrupted.SaveFullSnapshot(), 0u);
    S::ServerWorldRunner restored(config);
    ASSERT_TRUE(restored.Boot()) << restored.GetBootError();
    ASSERT_EQ(restored.CreatureProjection(), uninterrupted.CreatureProjection());
    ASSERT_EQ(restored.ComputeEcologySubHash(), uninterrupted.ComputeEcologySubHash());
    ASSERT_EQ(uninterrupted.RunFixedTicks(1).ticks_executed, 1u);
    ASSERT_EQ(restored.RunFixedTicks(1).ticks_executed, 1u);
    ASSERT_FALSE(uninterrupted.Session()->GetRegionSchedulerFault());
    ASSERT_FALSE(restored.Session()->GetRegionSchedulerFault());
    EXPECT_EQ(restored.CreatureProjection(), uninterrupted.CreatureProjection());
    EXPECT_EQ(restored.ComputeEcologySubHash(), uninterrupted.ComputeEcologySubHash());
}
} // namespace

namespace {
template<class T>
void Nondefault(T& v) {
    if constexpr (std::is_floating_point_v<T>)
        v = static_cast<T>(0.375);
    else if constexpr (std::is_same_v<T, bool>)
        v = true;
    else if constexpr (std::is_integral_v<T>)
        v = 1;
    else if constexpr (std::is_enum_v<T>)
        v = static_cast<T>(1);
    else if constexpr (std::is_same_v<T, std::string>)
        v = "sample";
}
void EveryComponent(entt::registry& r, entt::entity e, entt::entity target) {
    {
        auto& c = r.get_or_emplace<C::TransformComponent>(e);
        Nondefault(c.position.x);
        Nondefault(c.position.y);
        Nondefault(c.position.z);
        Nondefault(c.rotation.w);
        Nondefault(c.rotation.x);
        Nondefault(c.rotation.y);
        Nondefault(c.rotation.z);
        Nondefault(c.scale.x);
        Nondefault(c.scale.y);
        Nondefault(c.scale.z);
    }
    {
        auto& c = r.get_or_emplace<C::CreatureRegionComponent>(e);
        Nondefault(c.persistent_id);
        Nondefault(c.owner_x);
        Nondefault(c.owner_z);
        Nondefault(c.region_stamp);
    }
    {
        auto& c = r.get_or_emplace<C::CreatureCoarseComponent>(e);
        Nondefault(c.tier);
        Nondefault(c.transition);
        Nondefault(c.x_mm);
        Nondefault(c.y_mm);
        Nondefault(c.z_mm);
        Nondefault(c.hunger_ppm);
        Nondefault(c.thirst_ppm);
        Nondefault(c.needs);
        Nondefault(c.age_cadences);
        Nondefault(c.reproduce_cadences);
        Nondefault(c.reproduce_cooldown_cadences);
        Nondefault(c.courting_cadences);
        Nondefault(c.travel_pending);
        Nondefault(c.destination_x);
        Nondefault(c.destination_z);
    }
    {
        auto& c = r.get_or_emplace<C::CreatureLocomotionStateComponent>(e);
        Nondefault(c.velocity_x);
        Nondefault(c.velocity_y);
        Nondefault(c.velocity_z);
        Nondefault(c.grounded);
    }
    {
        auto& c = r.get_or_emplace<C::CreatureComponent>(e);
        Nondefault(c.is_predator);
        Nondefault(c.species_id);
        Nondefault(c.hunger);
        Nondefault(c.stamina);
        Nondefault(c.energy);
        Nondefault(c.move_speed);
        Nondefault(c.last_action);
        Nondefault(c.wish_x);
        Nondefault(c.wish_z);
        Nondefault(c.eaten);
    }
    {
        auto& c = r.get_or_emplace<C::CreatureGenomeComponent>(e);
        Nondefault(c.move_speed);
        Nondefault(c.vigilance);
        Nondefault(c.hunger_threshold);
        Nondefault(c.size_scale);
        Nondefault(c.vision_cos_half_fov);
        Nondefault(c.vision_range);
        Nondefault(c.hearing_range);
        Nondefault(c.age_ticks);
        Nondefault(c.reproduce_cooldown);
        Nondefault(c.generation);
        Nondefault(c.female);
        Nondefault(c.courting_ticks);
    }
    {
        auto& c = r.get_or_emplace<C::MortalComponent>(e);
        Nondefault(c.age_ticks);
        Nondefault(c.lifespan_ticks);
        Nondefault(c.dead);
    }
    {
        auto& c = r.get_or_emplace<C::DecayComponent>(e);
        Nondefault(c.decay_ticks);
        Nondefault(c.decay_duration);
        Nondefault(c.nutrient_released_milli);
        Nondefault(c.fully_decomposed);
    }
    {
        auto& c = r.get_or_emplace<C::ThirstComponent>(e);
        Nondefault(c.thirst);
        Nondefault(c.wish_x);
        Nondefault(c.wish_z);
        Nondefault(c.drinking);
        Nondefault(c.water_proximity);
    }
    {
        auto& c = r.get_or_emplace<C::CircadianComponent>(e);
        Nondefault(c.nocturnal);
        Nondefault(c.activity);
    }
    {
        auto& c = r.get_or_emplace<C::AlarmComponent>(e);
        Nondefault(c.level);
        Nondefault(c.alarmed);
    }
    {
        auto& c = r.get_or_emplace<C::PackHunterComponent>(e);
        Nondefault(c.coord_x);
        Nondefault(c.coord_z);
        Nondefault(c.in_pack);
    }
    {
        auto& c = r.get_or_emplace<C::TerritoryComponent>(e);
        Nondefault(c.home_x);
        Nondefault(c.home_z);
        Nondefault(c.radius);
        Nondefault(c.established);
    }
    {
        auto& c = r.get_or_emplace<C::TerritoryBiasComponent>(e);
        Nondefault(c.wish_x);
        Nondefault(c.wish_z);
    }
    {
        auto& c = r.get_or_emplace<C::MigratoryComponent>(e);
        Nondefault(c.drive);
        Nondefault(c.wish_x);
        Nondefault(c.wish_z);
    }
    {
        auto& c = r.get_or_emplace<C::ScavengerComponent>(e);
        Nondefault(c.wish_x);
        Nondefault(c.wish_z);
        Nondefault(c.feeding);
    }
    {
        auto& c = r.get_or_emplace<C::NeedsComponent>(e);
        Nondefault(c.needs);
    }
    {
        auto& c = r.get_or_emplace<C::SensesComponent>(e);
        c.nearest_food_source = target;
        c.nearest_water_source = target;
        c.nearest_threat = target;
        Nondefault(c.threat_distance);
    }
    {
        auto& c = r.get_or_emplace<C::OpportunityComponent>(e);
        Nondefault(c.id);
        Nondefault(c.action);
        Nondefault(c.target);
        Nondefault(c.need);
        Nondefault(c.satisfaction);
        Nondefault(c.urgency);
        Nondefault(c.risk);
        Nondefault(c.stamina_cost);
        Nondefault(c.radius);
    }
    {
        auto& c = r.get_or_emplace<C::ActionPlanComponent>(e);
        Nondefault(c.plan);
        Nondefault(c.current_action_index);
        Nondefault(c.time_in_current_action);
    }
    { r.get_or_emplace<C::InstinctAgent>(e); }
    {
        auto& c = r.get_or_emplace<C::LocomotionProfile>(e);
        Nondefault(c.move_speed);
        Nondefault(c.arrival_radius);
        Nondefault(c.slow_radius);
        Nondefault(c.separation_radius);
        Nondefault(c.separation_strength);
        Nondefault(c.flock_radius);
        Nondefault(c.cohesion_strength);
        Nondefault(c.alignment_strength);
    }
    {
        auto& c = r.get_or_emplace<C::SensableComponent>(e);
        Nondefault(c.scent_channel);
        Nondefault(c.scent_deposit);
        Nondefault(c.noise_loudness);
        Nondefault(c.noise_pitch);
        Nondefault(c.faction);
    }
    {
        auto& c = r.get_or_emplace<C::PerceptionComponent>(e);
        Nondefault(c.vision_cos_half_fov);
        Nondefault(c.vision_range);
        Nondefault(c.facing_x);
        Nondefault(c.facing_z);
        Nondefault(c.ear.range);
        Nondefault(c.ear.peak_pitch);
        Nondefault(c.ear.bandwidth);
        Nondefault(c.ear.gain);
        Nondefault(c.ear.threshold);
        Nondefault(c.faction);
    }
    {
        auto& c = r.get_or_emplace<C::AwarenessComponent>(e);
        Nondefault(c.awareness.meter);
        Nondefault(c.awareness.state);
        Nondefault(c.awareness.has_last_known);
        Nondefault(c.awareness.last_known_x);
        Nondefault(c.awareness.last_known_z);
        Nondefault(c.awareness.time_since_signal);
        Nondefault(c.params.gain);
        Nondefault(c.params.decay);
        Nondefault(c.params.suspicious_at);
        Nondefault(c.params.alert_at);
        Nondefault(c.params.engaged_at);
    }
    {
        auto& c = r.get_or_emplace<C::LocomotionIntentComponent>(e);
        Nondefault(c.wish_xz.x);
        Nondefault(c.wish_xz.y);
        Nondefault(c.arrived);
    }
    {
        auto& c = r.get_or_emplace<C::ScentSenseComponent>(e);
        Nondefault(c.channel);
        Nondefault(c.sign);
        Nondefault(c.strength);
        Nondefault(c.floor);
        Nondefault(c.weber_k);
    }
    {
        auto& c = r.get_or_emplace<C::LocomotionPathComponent>(e);
        Nondefault(c.waypoints);
        Nondefault(c.index);
    }
    {
        auto& c = r.get_or_emplace<C::StimulusSubscriptionComponent>(e);
        Nondefault(c.subscriptions);
    }
    {
        auto& c = r.get_or_emplace<C::InstinctAgentComponent>(e);
        Nondefault(c.actor_id);
        Nondefault(c.archetype);
        Nondefault(c.replan_interval_ticks);
        Nondefault(c.last_planned_tick);
        Nondefault(c.plans_executed);
        Nondefault(c.current_plan);
    }
    {
        auto& c = r.get_or_emplace<C::ForagerComponent>(e);
        Nondefault(c.cell_x);
        Nondefault(c.cell_z);
        Nondefault(c.home_x);
        Nondefault(c.home_z);
        Nondefault(c.carrying_food);
        Nondefault(c.deliveries);
    }
    {
        auto& c = r.get_or_emplace<C::FoodSourceComponent>(e);
        Nondefault(c.cell_x);
        Nondefault(c.cell_z);
        Nondefault(c.amount);
    }
    {
        auto& c = r.get_or_emplace<C::FieldEmitterComponent>(e);
        Nondefault(c.channel);
        Nondefault(c.rate_raw_per_tick);
        Nondefault(c.radius_cells);
    }
    { r.get_or_emplace<C::PendingDeletion>(e); }
    {
        auto& c = r.get_or_emplace<C::GrazeableComponent>(e);
        Nondefault(c.biomass);
        Nondefault(c.last_grazed_tick);
    }
    {
        auto& c = r.get_or_emplace<C::WaterHoleComponent>(e);
        Nondefault(c.radius);
    }
    {
        auto& c = r.get_or_emplace<C::CombustibleComponent>(e);
        Nondefault(c.fuel_milli);
        Nondefault(c.moisture_milli);
        Nondefault(c.burn_state);
        Nondefault(c.burn_ticks_remaining);
        Nondefault(c.ignition_radius);
    }
    {
        auto& c = r.get_or_emplace<C::IgnitionSourceComponent>(e);
        Nondefault(c.radius);
        Nondefault(c.intensity);
    }
    {
        auto& c = r.get_or_emplace<C::WaterSourceComponent>(e);
        Nondefault(c.output);
    }
    {
        auto& c = r.get_or_emplace<C::WaterFlowSourceComponent>(e);
        Nondefault(c.flow_rate);
    }
    auto& owner = r.get<C::CreatureRegionComponent>(e);
    owner.persistent_id = 1;
    owner.owner_x = owner.owner_z = 0;
    auto& coarse = r.get<C::CreatureCoarseComponent>(e);
    coarse.needs = {{"hunger", 123456}, {"curiosity", 234567}};
    r.get<C::ScentSenseComponent>(e).sign = -1.0f;
    r.get<C::TransformComponent>(e).position.z = std::bit_cast<float>(0x80000000u);
    r.get<C::CreatureComponent>(e).wish_z = std::bit_cast<float>(0x80000000u);
    r.get<C::NeedsComponent>(e).needs = {{"hunger", 0.3125f, 0.0009765625f}};
    auto& plan = r.get<C::ActionPlanComponent>(e);
    plan.plan = {{"move", target, 0.5f, true}, {"eat", entt::null, 0.25f, false}};
    r.get<C::LocomotionPathComponent>(e).waypoints = {{1.25f, -2.5f}, {3.75f, -4.5f}};
    r.get<C::StimulusSubscriptionComponent>(e).subscriptions = {
        {A::StimulusChannel::Weather, "safety", 0.125f}};
    auto& instinct = r.get<C::InstinctAgentComponent>(e);
    instinct.current_plan = {"actor", "archetype", "contract", {}, 0, "checksum", true};
    instinct.current_plan.candidates = {
        {1, "food", "eat", "plant", "hunger", 0.12500000000000003, 0.37500000000000006, "ranked"}};
    r.get<C::DecayComponent>(e).decay_duration = 100;
    r.get<C::DecayComponent>(e).fully_decomposed = 0;
    r.get<C::MortalComponent>(e).lifespan_ticks = 30;
    // Runtime handles must not leak into the record.
    r.emplace<C::CreaturePhysicsComponent>(e).avatar_index = 9876;
}

void CollectScalarPaths(const nlohmann::json& data,
                        const std::string& prefix,
                        std::vector<std::string>& paths) {
    if (data.is_object()) {
        for (auto it = data.begin(); it != data.end(); ++it)
            CollectScalarPaths(it.value(), prefix + "/" + it.key(), paths);
    } else if (data.is_array()) {
        for (std::size_t index = 0; index < data.size(); ++index)
            CollectScalarPaths(data[index], prefix + "/" + std::to_string(index), paths);
    } else if (!data.is_null()) {
        paths.push_back(prefix);
    }
}
void AlterScalar(nlohmann::json& value) {
    if (value.is_boolean())
        value = !value.get<bool>();
    else if (value.is_number_unsigned())
        value = value.get<std::uint64_t>() ^ std::uint64_t{1};
    else if (value.is_number_integer())
        value = value.get<std::int64_t>() + std::int64_t{1};
    else if (value.is_number_float())
        value = value.get<double>() + 0.125;
    else if (value.is_string())
        value = value.get<std::string>() + "_changed";
}
entt::entity ProjectionCreature(entt::registry& registry, std::uint64_t id) {
    const auto entity = registry.create();
    registry.emplace<C::TransformComponent>(entity);
    registry.emplace<C::CreatureComponent>(entity).species_id = std::uint16_t{1};
    registry.emplace<C::CreatureRegionComponent>(entity).persistent_id = id;
    registry.emplace<C::CreatureCoarseComponent>(entity);
    return entity;
}
TEST_F(CreatureHostAcceptance, ContinuationNegativeControlsCoverEveryPersistedComponent) {
    entt::registry registry;
    const auto entity = ProjectionCreature(registry, 1);
    const auto target = ProjectionCreature(registry, 2);
    EveryComponent(registry, entity, target);
    registry.emplace<C::CreatureControllerComponent>(entity);
    const auto original = A::BuildCreatureSnapshot(registry);
    const auto original_bytes = A::SerializeCreatureSnapshot(original);
    const auto original_hash = A::ComputeEcologySubHash(registry, true);
    const auto partial_hash = A::ComputeEcologySubHash(registry, false);
    std::size_t invisible_to_partial = 0, accepted = 0, refused = 0;
    for (auto component = original.entities[0].components.begin();
         component != original.entities[0].components.end();
         ++component) {
        SCOPED_TRACE(component.key());
        const auto detected_before = accepted + refused;
        std::size_t attempted = 0;
        std::vector<std::string> paths;
        CollectScalarPaths(component.value().at("data"), "", paths);
        if (paths.empty()) {
            ++attempted;
            auto control = original;
            control.entities[0].components.erase(component.key());
            entt::registry restored;
            A::ApplyCreatureSnapshot(restored, control);
            EXPECT_NE(A::SerializeCreatureSnapshot(A::BuildCreatureSnapshot(restored)),
                      original_bytes);
            EXPECT_NE(A::ComputeEcologySubHash(restored, true), original_hash);
            ++accepted;
        }
        for (const auto& path : paths) {
            SCOPED_TRACE(path);
            ++attempted;
            auto control = original;
            auto& data = control.entities[0].components[component.key()]["data"];
            AlterScalar(data[nlohmann::json::json_pointer(path)]);
            std::string bytes;
            try {
                bytes = A::SerializeCreatureSnapshot(control);
            } catch (const std::exception&) {
                ++refused; // Invalid identity/reference detected by the writer.
                continue;
            }
            ASSERT_NE(bytes, original_bytes);
            A::CreatureSnapshot decoded;
            std::string error;
            if (!A::LoadCreatureSnapshot(bytes, decoded, error)) {
                EXPECT_FALSE(error.empty());
                ++refused;
                continue;
            }
            entt::registry restored;
            try {
                A::ApplyCreatureSnapshot(restored, decoded);
            } catch (const std::exception&) {
                ++refused; // A changed reference/identity must refuse transactionally.
                EXPECT_TRUE(restored.view<C::CreatureComponent>().empty());
                continue;
            }
            EXPECT_EQ(A::SerializeCreatureSnapshot(A::BuildCreatureSnapshot(restored)), bytes);
            EXPECT_NE(A::ComputeEcologySubHash(restored, true), original_hash);
            if (A::ComputeEcologySubHash(restored, false) == partial_hash)
                ++invisible_to_partial;
            ++accepted;
        }
        EXPECT_GT(attempted, 0u);
        EXPECT_EQ(accepted + refused - detected_before, attempted);
    }
    EXPECT_GT(original.entities[0].components.size(), 35u);
    EXPECT_GT(accepted, 100u);
    EXPECT_GT(refused, 0u);
    EXPECT_GT(invisible_to_partial, 30u);
}
} // namespace

namespace {
TEST_F(CreatureHostAcceptance, PopulatedHeadlessQualification) {
    const auto qualification =
        Luminumbra::Core::ReadEnvironment("LUMINUMBRA_CREATURE_QUALIFICATION");
    const std::uint64_t total = qualification && *qualification == "1" ? 54000u : 90u;
    auto config = Config();
    config.avatar_count = 1;
    S::ServerWorldRunner uninterrupted(config);
    ASSERT_TRUE(uninterrupted.Boot()) << uninterrupted.GetBootError();
    ASSERT_EQ(uninterrupted.CreatureCount(), 0u);
    const auto checkpoint = total / 3u;
    ASSERT_EQ(uninterrupted.RunFixedTicks(checkpoint).ticks_executed, checkpoint);
    ASSERT_FALSE(uninterrupted.Session()->GetRegionSchedulerFault());
    ASSERT_EQ(uninterrupted.CreatureCount(), 8u);
    config.world_id = uninterrupted.Session()->GetMetadata().worldId;
    ASSERT_GT(uninterrupted.SaveFullSnapshot(), 0u);
    S::ServerWorldRunner restored(config);
    ASSERT_TRUE(restored.Boot()) << restored.GetBootError();
    ASSERT_EQ(restored.CreatureProjection(), uninterrupted.CreatureProjection());
    ASSERT_EQ(restored.ComputeEcologySubHash(), uninterrupted.ComputeEcologySubHash());
    for (std::uint64_t tick = checkpoint; tick < total; tick += 30u) {
        const auto count = std::min(std::uint64_t{30}, total - tick);
        ASSERT_EQ(uninterrupted.RunFixedTicks(count).ticks_executed, count);
        ASSERT_EQ(restored.RunFixedTicks(count).ticks_executed, count);
        ASSERT_FALSE(uninterrupted.Session()->GetRegionSchedulerFault());
        ASSERT_FALSE(restored.Session()->GetRegionSchedulerFault());
        EXPECT_EQ(restored.CreatureProjection(), uninterrupted.CreatureProjection());
        EXPECT_EQ(restored.ComputeEcologySubHash(), uninterrupted.ComputeEcologySubHash());
        EXPECT_EQ(restored.ComputeWorldHash(), uninterrupted.ComputeWorldHash());
        EXPECT_EQ(restored.Session()->GetActiveRegionLedger().completed_work(),
                  uninterrupted.Session()->GetActiveRegionLedger().completed_work());
    }
    EXPECT_EQ(uninterrupted.TickCount(), total);
    EXPECT_GT(uninterrupted.CreatureCount(), 0u);
}
} // namespace
