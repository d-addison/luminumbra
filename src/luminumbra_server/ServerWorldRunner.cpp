#include "ServerWorldRunner.h"

#include "ServerStreamingBudget.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <stdexcept>
#include <thread>
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
#include "luminumbra_common/replay/ReplayStream.h"
#include "luminumbra_common/systems/FarmingSystem.h"
#include "luminumbra_common/systems/PhysicsSystem.h"
#include "luminumbra_common/systems/SHIELD_WorldSystem.h"
#include "luminumbra_common/systems/WaterSystem.h" // water-smoke telemetry reads
#include "luminumbra_common/systems/WeatherSystem.h"
#include "luminumbra_common/world/Chunk.h"
#include "luminumbra_common/world/SurfaceWorkBudget.h"

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
        prey(-7.0f + static_cast<float>(i) * 2.4f, -2.0f);
}

//  spawn a deterministic PLANT roster so the headless smoke exercises the plant
// path end-to-end (ComputePlantSubHash non-empty, PlantGrowthSystem ticking, persistence roundtrip)
// rather than only the empty-neutral path. Uses the  MakePlantFromSpecies (sample genome ->
// PlantSeed -> stamp CropLifecycle) from an INLINE species template (no file I/O), seeded from
// ints, so the roster is a pure function of (seed, preset) via the anchor -> run==replay.
void SpawnPlantRoster(world::GameSession& session, const Vec3& anchor) {
    namespace fol = luminumbra::foliage;
    auto& r = session.GetRegistry();
    fol::SpeciesTemplate tmpl;
    tmpl.id = "smoke_crop";
    tmpl.perennial = false;
    tmpl.lifespan_ticks = 900u;
    tmpl.gene_lo.fill(0.30f);
    tmpl.gene_hi.fill(0.70f);
    auto rng = luminumbra::core::DeterministicRng::seeded(fol::kPlantSeedOffset, 9001u, 7u);
    for (int i = 0; i < 6; ++i) {
        const Vec3 pos(anchor.x - 6.0f + static_cast<float>(i) * 2.0f, anchor.y, anchor.z + 4.0f);
        // Admit only the six roster positions' regions, even with no avatar anchors.
        // The consumer gate resolves these legacy plants' owners from their transforms.
        session.PinActiveRegion(world::ActiveRegionLedger::region_at(pos), true);
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

world::AuthorityBootDescriptor DescribeWorkload(const ServerWorldRunnerConfig& config) {
    if (config.avatar_count < 0 ||
        config.avatar_count > static_cast<int>(world::AuthorityBootDescriptor::kMaxAvatars)) {
        throw std::invalid_argument("Replay avatar roster exceeds descriptor bound");
    }
    world::AuthorityBootDescriptor boot;
    boot.parked_bytes_limit = config.parked_bytes_limit;
    boot.avatar_count = static_cast<std::uint32_t>(config.avatar_count);
    boot.ecology_roster = config.ecology_roster;
    boot.planted_roster = config.planted_roster;
    boot.moving_anchor = config.moving_anchor;
    boot.water_smoke = config.water_smoke;
    return boot;
}

} // namespace

world::AuthorityBootDescriptor
ServerWorldRunner::DescribeReplayBoot(const ServerWorldRunnerConfig& config) {
    if (config.seed.empty()) {
        throw std::invalid_argument("Replay boot description requires a nonempty seed");
    }
    world::GameSession session;
    session.ApplySystemConfig(
        config.systems_config_override
            ? *config.systems_config_override
            : luminumbra::core::SystemConfig::LoadFromRuntimeRoot(config.root_path));
    session.SetFiniteHydrologyEnabled(config.finite_hydrology);
    if (config.region_scheduler) {
        session.SetRegionSchedulerConfig(*config.region_scheduler);
    }
    return Replay::CaptureReplayBoot(session,
                                     world::ResolveWorldSeed(config.seed, 1),
                                     world::WorldCalendar{},
                                     DescribeWorkload(config));
}

Replay::ReplayHeader ServerWorldRunner::CaptureReplayHeader() const {
    if (!m_booted || !m_session || !m_config.world_id.empty() || TickCount() != 0 ||
        (!m_session->GetWeatherSchedule() && !m_continuationOptions.owner_continuation)) {
        throw std::invalid_argument("Replay requires a fresh boot at tick zero");
    }
    return Replay::CaptureReplayHeader(*m_session,
                                       static_cast<std::uint32_t>(m_config.surface_radius),
                                       static_cast<std::uint32_t>(m_config.collision_radius),
                                       DescribeWorkload(m_config));
}

ServerWorldRunner::ServerWorldRunner(ServerWorldRunnerConfig config)
    : ServerWorldRunner(std::move(config), world::OwnerContinuationOptions{}) {}

ServerWorldRunner::ServerWorldRunner(ServerWorldRunnerConfig config,
                                     const world::OwnerContinuationOptions& options)
    : m_config(std::move(config))
    , m_continuationOptions(options) {}

ServerWorldRunner::~ServerWorldRunner() {
    Shutdown();
}

bool ServerWorldRunner::Boot() {
    if (!World::SurfaceRadiiSupported(m_config.surface_radius, m_config.collision_radius)) {
        m_bootError = "Surface and collision radii must be between 0 and 12 chunks";
        return false;
    }
    if (m_booted) {
        return true;
    }

    // optional worker-count override (LUMINUMBRA_JOB_WORKERS). The
    // streamed-chunk world_hash is INVARIANT to worker count (hashes are computed
    // after a full streaming quiesce), so this is a pure scheduling knob: the
    // replay gates set it to 1 to minimize the headless server's known
    // intermittent shutdown/streaming race (documented in ) without
    // touching the hash. Unset => one worker per hardware thread (production).
    if (const auto workers = Core::ReadEnvironment("LUMINUMBRA_JOB_WORKERS")) {
        m_jobSystem.startup(static_cast<std::size_t>(std::strtoul(workers->c_str(), nullptr, 10)));
    } else {
        m_jobSystem.startup();
    }

    if (m_continuationOptions.owner_continuation &&
        (m_config.avatar_count < 0 || static_cast<std::size_t>(m_config.avatar_count) >
                                          Systems::PhysicsSystem::kMaxRestoredAvatars)) {
        m_bootError = "Owner continuation avatar roster exceeds its bound.";
        m_jobSystem.shutdown();
        return false;
    }
    m_session = std::make_unique<world::GameSession>(m_continuationOptions);
    if (m_continuationOptions.owner_continuation) {
        m_session->ConfigureOwnerContinuation(m_avatars,
                                              static_cast<std::uint32_t>(m_config.avatar_count),
                                              m_config.moving_anchor,
                                              m_config.water_smoke);
    }
    m_session->SetSimBudgetTelemetryEnabled(m_config.sim_budget);
    m_session->SetJobSystem(&m_jobSystem);
    m_session->SetRootPath(m_config.root_path);
    if (m_config.region_scheduler)
        m_session->SetRegionSchedulerConfig(*m_config.region_scheduler);
    // host==peer: the headless host must run the SAME water-sim resolution the
    // client derives from data/common/systems.json — sim.water_high_res is baked into
    // the hashed mm grids, so a mismatched host could never agree with its peers.
    // Missing runtime defaults use compiled defaults, independently of the launch directory.
    {
        const auto sys_cfg =
            m_config.systems_config_override
                ? *m_config.systems_config_override
                : luminumbra::core::SystemConfig::LoadFromRuntimeRoot(m_config.root_path);
        m_session->ApplySystemConfig(sys_cfg);
        m_session->SetParkedBytesLimit(static_cast<std::size_t>(m_config.parked_bytes_limit));
    }
    m_session->SetFiniteHydrologyEnabled(m_config.finite_hydrology);
    if (m_session->ActiveRegionsEnabled() &&
        m_config.avatar_count > static_cast<int>(Systems::PhysicsSystem::kSimulationOwnerBudget)) {
        m_bootError = "Avatar collider preparation budget exceeded";
        m_session.reset();
        m_jobSystem.shutdown();
        return false;
    }
    if (m_config.weather_schedule && !m_session->SetWeatherSchedule(*m_config.weather_schedule)) {
        m_bootError = "Invalid weather schedule";
        return false;
    }
    // A headless host has no client asset manifest; simulation assets alone are
    // validated and every voxel field uses the authoritative CPU generator.

    bool world_ready = false;
    if (!m_config.world_id.empty()) {
        world_ready = m_session->LoadWorld(m_config.world_id);
    } else {
        world_ready = m_session->CreateWorld(
            m_config.world_name,
            m_config.seed,
            m_config.preset,
            m_config.preset_json_override ? &*m_config.preset_json_override : nullptr,
            m_config.preset_use,
            m_config.required_preset_content_hash);
    }
    if (!world_ready) {
        m_bootError = m_session->GetWorldOpenError();
        LUMINUMBRA_CORE_ERROR("ServerWorldRunner: world boot failed (preset='{}', world_id='{}')",
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
        LUMINUMBRA_CORE_ERROR("ServerWorldRunner: world open refused: {}", m_bootError);
        m_session.reset();
        m_jobSystem.shutdown();
        return false;
    }

    auto* world_system = m_session->GetWorldSystem();
    auto* physics_system = m_session->GetPhysicsSystem();
    if (!world_system || !physics_system) {
        LUMINUMBRA_CORE_ERROR("ServerWorldRunner: world systems missing after boot");
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
    const auto* continuation = m_session->RestoredOwnerContinuation();
    const bool water_loaded_from_save = continuation || m_session->GetLastLoadedChunkCount() > 0;
    if (water_loaded_from_save) {
        world_system->SetWaterBootPaused(true);
    }

    // Spawn-anchor streaming: synchronous surface horizon with collision
    // ready so the physics system can query terrain from tick 1. Meshing
    // stays on because the supported server profile keeps meshes resident.
    const Vec3 spawn_anchor = m_session->GetMetadata().spawnPoint;
    m_ownerOrigin = continuation ? continuation->origin : spawn_anchor;
    m_ownerAnchor = continuation ? continuation->anchor : spawn_anchor;
    m_ownerStreamTick = continuation ? continuation->stream_tick : 0;
    // A continuation already restored its terrain and owners at a counted
    // boundary. Boot-time streaming would allocate colliders ahead of the next
    // tick and change both authority flags and the subsequent BodyID sequence.
    const bool horizon_ready =
        continuation ||
        world_system->EnsureSurfaceReadyNear(
            spawn_anchor, physics_system, m_config.surface_radius, m_config.collision_radius);
    if (!horizon_ready) {
        LUMINUMBRA_CORE_ERROR(
            "ServerWorldRunner: spawn-anchor surface horizon failed to become ready");
        m_session.reset();
        m_jobSystem.shutdown();
        return false;
    }

    LUMINUMBRA_CORE_INFO("ServerWorldRunner: booted world '{}' (id {}, preset {}, seed {}) - spawn "
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
    m_spawnedAvatarCount = 0;
    if (continuation) {
        m_avatars.reserve(continuation->avatars.size());
        for (const auto& owner : continuation->avatars) {
            m_avatars.push_back(owner.avatar);
        }
    } else {
        physics_system->clear_avatar_characters();
    }
    if (!continuation && m_config.avatar_count > 0) {
        m_avatars.reserve(std::min(static_cast<std::size_t>(m_config.avatar_count),
                                   Systems::PhysicsSystem::kSimulationOwnerBudget));
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
            // Publish the roster only after its corresponding controller exists.
            const auto character = physics_system->create_avatar_character(avatar.position);
            if (character == Systems::PhysicsSystem::kInvalidAvatar) {
                m_bootError = "Avatar collider preparation budget exceeded";
                m_avatars.clear();
                m_session.reset();
                m_jobSystem.shutdown();
                return false;
            }
            m_avatars.push_back(avatar);
            ++m_spawnedAvatarCount;
        }
        LUMINUMBRA_CORE_INFO(
            "ServerWorldRunner: spawned {} deterministic player avatar(s) (+physics characters).",
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
                LUMINUMBRA_CORE_ERROR("ServerWorldRunner: avatar collision horizon failed to "
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
            LUMINUMBRA_CORE_INFO("ServerWorldRunner: warmed {} additional avatar collision "
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
        LUMINUMBRA_CORE_INFO(
            "ServerWorldRunner: spawned deterministic ecology roster ({} creatures, kinematic).",
            CreatureCount());
    }

    //  opt-in plant roster so the smoke exercises the plant sub-hash + growth +
    // persistence end-to-end (empty otherwise -> plant_hash neutral). Pure fn of (seed, preset).
    if (m_config.planted_roster) {
        SpawnPlantRoster(*m_session, spawn_anchor);
        LUMINUMBRA_CORE_INFO("ServerWorldRunner: spawned deterministic plant roster (6 plants).");
    }

    // One shared bounded residency/water boot for runner and ordinary clients.
    // Bounded by the already admitted avatar roster, reused across all boot passes.
    std::vector<Vec3> boot_anchors;
    boot_anchors.reserve(m_avatars.size());
    for (const World::PlayerAvatar& avatar : m_avatars) {
        boot_anchors.push_back(avatar.position);
    }
    if (!m_session->SettleInitialSimulation(boot_anchors, m_boot_settle)) {
        m_bootError = "Initial simulation preparation failed";
        m_session.reset();
        m_jobSystem.shutdown();
        return false;
    }

    // live ticks resume the loaded water state exactly where the save left it.
    if (water_loaded_from_save) {
        world_system->SetWaterBootPaused(false);
    }

    // Boot settles water outside the counted ticks. Publish its ownership before any initial
    // checkpoint or save; hash reads themselves only observe this completed boundary.
    world_system->sync_water_authority();
    if (m_continuationOptions.owner_continuation) {
        m_ownerStreamingAnchors.reserve(Systems::PhysicsSystem::kMaxRestoredAvatars);
        std::vector<Vec3> simulation_anchors;
        simulation_anchors.reserve(Systems::PhysicsSystem::kMaxRestoredAvatars);
        m_session->SetReplicatedSimulationAnchors(std::move(simulation_anchors));
        m_session->ConfigureOwnerContinuation(m_avatars,
                                              static_cast<std::uint32_t>(m_config.avatar_count),
                                              m_config.moving_anchor,
                                              m_config.water_smoke);
        if (!m_avatars.empty()) {
            m_ownerAnchor = m_avatars.front().position;
        }
        if (!continuation && m_config.water_smoke) {
            m_ownerOrigin = FindWetAnchor(*world_system, spawn_anchor);
        }
        m_session->SetOwnerContinuationAnchor(m_ownerOrigin, m_ownerAnchor, m_ownerStreamTick);
    }
    m_booted = true;
    return true;
}

void ServerWorldRunner::SetAvatarMove(std::uint32_t player_id, float move_x, float move_z) {
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

ServerTickReport ServerWorldRunner::RunFixedTicks(std::uint64_t tick_count) {
    ServerTickReport report;
    if (!m_booted || !m_session) {
        report.region_fault = true;
        report.region_fault_message = "Server world is not booted";
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
    // Keep batch tick indices and telemetry in this invocation. Retries cannot
    // exceed the requested tick budget, even if preparation refuses permanently.
    const std::uint64_t max_preparation_retries = tick_count;
    std::uint64_t preparation_retries = 0;
    while (report.ticks_executed < tick_count) {
        if (m_continuationOptions.owner_continuation &&
            m_ownerStreamTick == world::WorldClock::kTickLimit) {
            report.region_fault = true;
            report.region_fault_message = "Owner continuation stream clock exhausted.";
            break;
        }

        if (m_continuationOptions.owner_continuation && m_avatars.empty()) {
            Vec3 next = m_ownerOrigin;
            const auto next_tick = m_ownerStreamTick + 1;
            if (m_config.water_smoke) {
                next.x += 2.0f * static_cast<float>(next_tick);
            } else if (m_config.moving_anchor) {
                const float distance = 0.5f * static_cast<float>(next_tick);
                next.x += distance;
                next.z += distance;
            }
            if (!world::ActiveRegionLedger::valid_anchor(next)) {
                report.region_fault = true;
                report.region_fault_message = "Owner continuation stream anchor exhausted.";
                break;
            }
        }

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
        const auto snapshots_before = budget.Enabled() ? world_system->snapshot_copies() : 0;
        const std::uint64_t budget_tick = m_session->GetSimulationTickCount() + 1;
        // A refused preparation consumes no tick, so it records no ServerPhysics
        // sample: completed-tick work traces stay identical across retries.
        const auto physics_start = std::chrono::steady_clock::now();
        // Every host gathers the same registered controllers and projectiles.
        // No host-specific spawn/camera anchor can change the horizon or its budget.
        if (!physics_system->prepare_simulation_collisions({}, budget_tick)) {
            // Zero progress without a region fault is the retry outcome. A durable
            // read failure can latch a terminal fault during preparation itself.
            if (const auto& fault = m_session->GetRegionSchedulerFault()) {
                report.region_fault = true;
                report.region_fault_message = fault->message;
                break;
            }
            if (preparation_retries < max_preparation_retries) {
                ++preparation_retries;
                std::this_thread::sleep_for(std::chrono::milliseconds(33));
                continue; // No physics, input or tick state was consumed.
            }
            break; // Return retryable partial progress when the retry budget is spent.
        }
        physics_system->update(static_cast<float>(fixed_dt));
        //  step the server-authoritative avatar characters (gravity +
        // world collision; deterministic creation order) and read their settled
        // transforms back into m_avatars, so the streaming anchors + the
        // `entities` sub-hash reflect the physics-authoritative positions.
        if (!m_avatars.empty()) {
            physics_system->update_avatars(static_cast<float>(fixed_dt));
            for (std::size_t i = 0; i < m_avatars.size(); ++i) {
                m_avatars[i].position = physics_system->get_avatar_position(i);
                m_avatars[i].velocity = physics_system->get_avatar_velocity(i);
            }
        }
        budget.Record(BudgetStage::ServerPhysics,
                      budget_tick,
                      1u,
                      std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() -
                                                                physics_start)
                          .count());
        if (m_continuationOptions.owner_continuation) {
            m_ownerAnchorVisits = 0;
            m_ownerStreamingAnchors.clear();
            for (const auto& avatar : m_avatars) {
                m_ownerStreamingAnchors.push_back(avatar.position);
                ++m_ownerAnchorVisits;
            }
        }
        if (m_session->ActiveRegionsEnabled() && m_continuationOptions.owner_continuation) {
            m_session->SetOwnerContinuationSimulationAnchors(m_ownerStreamingAnchors);
        } else if (m_session->ActiveRegionsEnabled()) {
            std::vector<Vec3> anchors;
            for (const auto& avatar : m_avatars)
                anchors.push_back(avatar.position);
            m_session->SetReplicatedSimulationAnchors(std::move(anchors));
        }
        report.ticks_executed +=
            m_session->TickSimulation(fixed_dt, /*defer_water_authority=*/true);
        report.frames_executed += 1;
        if (m_continuationOptions.owner_continuation) {
            ++m_ownerStreamTick;
        }
        if (const auto& fault = m_session->GetRegionSchedulerFault()) {
            report.region_fault = true;
            report.region_fault_message = fault->message;
            break;
        }
        const std::uint64_t simulation_tick =
            m_continuationOptions.owner_continuation ? m_ownerStreamTick
            : m_session->ActiveRegionsEnabled()      ? m_session->GetSimulationTickCount()
                                                     : report.ticks_executed;

        // Supply the simulation tick to the activation queue so dispatched
        // batches receive deterministic due ticks.
        world_system->begin_tick(static_cast<std::int64_t>(simulation_tick),
                                 m_continuationOptions.owner_continuation);
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
                    const auto tick = m_continuationOptions.owner_continuation
                                          ? m_ownerStreamTick
                                          : report.ticks_executed;
                    const float d = 2.0f * static_cast<float>(tick);
                    const Vec3 origin =
                        m_continuationOptions.owner_continuation ? m_ownerOrigin : water_walk_start;
                    anchor = Vec3(origin.x + d, origin.y, origin.z);
                } else if (m_config.moving_anchor) {
                    // B' harness: walk the streaming anchor +X/+Z each tick so chunks stream IN
                    // ahead and OUT behind during the run. ~0.5 m/tick -> ~45 m over 90 ticks (~3
                    // chunks). Pure fn of the tick -> both --smoke runs drift identically; any
                    // run!=replay is the moving-case water nondeterminism (boot warm-up only
                    // settles the INITIAL residency).
                    const auto tick = m_continuationOptions.owner_continuation
                                          ? m_ownerStreamTick
                                          : report.ticks_executed;
                    const float d = 0.5f * static_cast<float>(tick);
                    const Vec3 origin =
                        m_continuationOptions.owner_continuation ? m_ownerOrigin : spawn_anchor;
                    anchor = Vec3(origin.x + d, origin.y, origin.z + d);
                }
                if (m_continuationOptions.owner_continuation) {
                    m_ownerAnchor = anchor;
                }
                world_system->update(m_session->GetRegistry(), anchor, physics_system);
            } else if (m_continuationOptions.owner_continuation) {
                m_ownerAnchor = m_avatars.front().position;
                world_system->update(
                    m_session->GetRegistry(), m_ownerStreamingAnchors, physics_system);
            } else {
                std::vector<Vec3> anchors;
                anchors.reserve(m_avatars.size());
                for (const World::PlayerAvatar& a : m_avatars) {
                    anchors.push_back(a.position);
                }
                world_system->update(m_session->GetRegistry(), anchors, physics_system);
            }
        });
        if (m_continuationOptions.owner_continuation) {
            m_session->SetOwnerContinuationAnchor(m_ownerOrigin, m_ownerAnchor, m_ownerStreamTick);
        }
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
        // Ordinary runs publish only batches due this tick (dispatch + K).
        // Continuations finish the current admitted work at the counted boundary,
        // since their snapshots cannot retain process-local publication queues.
        // This wall-clock timing never feeds world_hash.
        const auto _wait_t0 = std::chrono::steady_clock::now();
        {
            LUMIN_PROFILE_ZONE_N(
                "streaming_activate_due"); // the p99 latency the queue exists to shrink
            if (m_continuationOptions.owner_continuation) {
                // A resumable tick cannot retain unpublished generation/promotion
                // or mesh work: the save format carries chunks, not job queues.
                // Settle only the admitted/deferred work, without another update,
                // water step or collision pass. Saves remain non-publishing.
                world_system->wait_for_streaming_jobs();
            } else {
                world_system->activate_due(static_cast<std::int64_t>(simulation_tick));
            }
        }
        const double _wait_ms =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - _wait_t0)
                .count();
        wait_samples.push_back(_wait_ms);
        // Water runs in update(), after TickSimulation. Promote once, after activation has
        // settled its writers and before autosave or any checkpoint can observe this tick.
        // sync_water_authority visits only queued changes, never the resident chunk set.
        world_system->sync_water_authority();
        if (budget.Enabled()) {
            // Include the complete update, including its final resident-chunk scan and
            // queue bookkeeping. Water has its own disjoint bracket; activation is separate.
            const auto& timing = world_system->dbg_stream_timings();
            const auto scheduled =
                world_system->get_last_streaming_budget_stats().scheduled_generation;
            const auto* water = m_session->GetWaterSystem();
            streaming_budget.RecordWorkItems(budget_tick,
                                             world_system->get_last_streaming_budget_stats(),
                                             physics_system->work_items(),
                                             water ? water->cells_stepped_last_update() : 0u,
                                             world_system->snapshot_copies() - snapshots_before);
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
        bool autosave_due = autosave_interval > 0 && simulation_tick > 0 &&
                            (simulation_tick % autosave_interval) == 0;
        if (m_autosave_retry_countdown > 0) {
            m_autosave_retry_countdown -= 1;
            if (m_autosave_retry_countdown == 0)
                autosave_due = autosave_interval > 0;
        }
        if (autosave_due) {
            // Synchronous host-thread save. Snapshot/shutdown run between RunFixedTicks calls.
            world::WorldStateSaveReport save_report;
            if (m_session->SaveWorldState(&save_report)) {
                m_autosave_retry_countdown = 0;
                report.autosave_passes += 1;
                if (save_report.saved && save_report.chunks_dirty > 0) {
                    report.autosave_writes += 1;
                }
            } else {
                m_autosave_retry_countdown = kAutosaveRetryTicks;
            }
        }

        // Byte-limit pressure: a dirty authoritative chunk could not be parked, so persist now.
        if (m_session->PersistenceSaveRequested()) {
            world::WorldStateSaveReport pressure_report;
            if (m_session->SaveWorldState(&pressure_report)) {
                m_autosave_retry_countdown = 0;
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

std::string ServerWorldRunner::ComputeAvailabilityDigest() {
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

std::string ServerWorldRunner::ComputeWorldHash() {
    if (!m_booted || !m_session) {
        throw Persistence::AuthorityCaptureRefused(
            Persistence::AuthorityCaptureStatus::MissingCursor);
    }
    const auto result =
        m_session->CaptureAuthorityHashes({}, Persistence::AuthorityCaptureParts::Composite);
    if (!result.capture) {
        throw Persistence::AuthorityCaptureRefused(result.status);
    }
    return result.capture->composite;
}

Persistence::WorldStreamingStateSubHashes ServerWorldRunner::ComputeWorldSubHashes() {
    if (!m_booted || !m_session) {
        throw Persistence::AuthorityCaptureRefused(
            Persistence::AuthorityCaptureStatus::MissingCursor);
    }
    const std::string entities_snapshot =
        Ecs::SerializeEntityRegistrySnapshotJson(World::BuildAvatarEntitySnapshot(m_avatars));
    const auto result = m_session->CaptureAuthorityHashes(
        entities_snapshot, Persistence::AuthorityCaptureParts::SubHashes);
    if (!result.capture) {
        throw Persistence::AuthorityCaptureRefused(result.status);
    }
    return result.capture->sub;
}

void ServerWorldRunner::ComputeWorldHashAndSubHashes(
    std::string& out_world_hash, Persistence::WorldStreamingStateSubHashes& out_sub) {
    if (!m_booted || !m_session) {
        throw Persistence::AuthorityCaptureRefused(
            Persistence::AuthorityCaptureStatus::MissingCursor);
    }
    const std::string entities_snapshot =
        Ecs::SerializeEntityRegistrySnapshotJson(World::BuildAvatarEntitySnapshot(m_avatars));
    const auto result = m_session->CaptureAuthorityHashes(entities_snapshot);
    if (!result.capture) {
        throw Persistence::AuthorityCaptureRefused(result.status);
    }
    out_world_hash = result.capture->composite;
    out_sub = result.capture->sub;
}

std::size_t ServerWorldRunner::SaveFullSnapshot() {
    // Host-thread-only, after RunFixedTicks returns; never dispatch a save to workers.
    // write the FULL in-memory streamed-chunk set (not dirty-gated) so
    // a loaded session can adopt exactly this set. Reuses WorldSaveService.
    if (!m_booted || !m_session || !m_session->GetWorldSystem() ||
        !m_session->IsSimulationTickBoundary()) {
        return 0;
    }
    const std::filesystem::path save_dir = m_session->GetWorldSaveDir();
    if (save_dir.empty()) {
        return 0;
    }
    if (!m_session->SavedPresetCompatible(save_dir)) {
        LUMINUMBRA_CORE_ERROR(
            "SaveFullSnapshot failed: destination preset or identity is incompatible");
        return 0;
    }
    world::WorldStateSaveReport report;
    if (!m_session->SaveWorldStateTo(save_dir, &report, /*full_snapshot=*/true))
        return 0;
    return report.chunks_total;
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

std::string ServerWorldRunner::ComputeEcologySubHash() const {
    // gate-populated-world-replay: id-ordered hash over the live creature roster
    // (empty/neutral when none). Pure read over the session registry; no quiesce
    // needed (creature state is registry-resident, not chunk-derived).
    if (!m_session) {
        return {};
    }
    return m_session->FoldClockIntoEcologyHash(
        luminumbra::ai::ComputeEcologySubHash(m_session->GetRegistry()));
}

std::size_t ServerWorldRunner::CreatureCount() const {
    if (!m_session) {
        return 0;
    }
    return m_session->GetRegistry()
        .view<const ::Luminumbra::Components::CreatureComponent>()
        .size();
}

void ServerWorldRunner::Shutdown(world::WorldStateSaveReport* shutdown_save_report) {
    if (m_shutdown) {
        return;
    }
    m_shutdown = true;

    if (m_booted && m_session) {
        // The host tick loop has returned, so this cannot overlap autosave/snapshot.
        // Save on shutdown via WorldSaveService (incremental contract: a
        // never-edited world writes no chunk snapshot).
        world::WorldStateSaveReport save_report;
        m_session->SaveWorldState(&save_report);
        if (shutdown_save_report) {
            *shutdown_save_report = save_report;
        }
        LUMINUMBRA_CORE_INFO(
            "ServerWorldRunner: shutdown save - {} chunks total, {} dirty, saved={}",
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
