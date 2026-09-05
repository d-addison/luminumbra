#include "ServerWorldRunner.h"

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

// Folds the chunk-derived top-level hash, the wind sub-hash, and the weather
// sub-hash into the composite world_hash. This is the DELIBERATE bump chain: the
// chunk hash (WorldSaveService::world_hash / ComputeWorldStreamingStateHash) is
// unchanged byte-for-byte (persistence fixtures stay green); the runner-level
// world_hash ALSO commits the wind field ( bump, 2fa007951a21e140 ->
// 0eac465289e7c88b), the weather state ( bump #2, 0eac465289e7c88b ->
// 0857e683b4b8c47e; weather_sub_hash a7d8f3d28401386f), and now the lightning
// STRIKE SCHEDULE folded into the SAME weather sub-hash ( , hash revision,
// 0857e683b4b8c47e -> d950a6afc12a5cdc; weather_sub_hash a7d8f3d28401386f ->
// e3c7e0aa219ebbe5). The strike schedule replaces the reserved single-0 slot
// left in ComputeWeatherSubHash, so the wind term + the byte layout before the
// strike block are unchanged. Order is fixed (chunk, then wind, then weather,
// then aether) so the composite is reproducible.  appends the `aether`
// term (bump #4, d950a6afc12a5cdc -> f17726d44054d133) -- append-only, so the
// bytes before "|aether:" are unchanged (wind/weather sub-hashes are intact).
//  appends the `scents` term after aether, preserving the whole
// pre-ecology byte prefix while making live scent fields authoritative.
// gate-populated-world-replay appends the `ecology` term LAST (canonical bump #6,
// 8a6b7bb6795da912 -> d8f84cf6d7d0b978 empty-roster composite): the id-ordered
// creature-state sub-hash.
// Append-only, so the bytes before "|ecology:" (chunk + wind + weather + aether +
// scents) are byte-identical to the pre-fold composite -- the empty-roster default
// folds an EMPTY ecology value, so the composite differs from pre-fold ONLY by the
// appended "|ecology:" suffix (additivity guard, ).
std::string ComposeWorldHash(const std::string& chunk_hash,
                             const std::string& wind_hash,
                             const std::string& weather_hash,
                             const std::string& aether_hash,
                             const std::string& scent_hash,
                             const std::string& ecology_hash,
                             const std::string& plant_hash) {
    //   (bump #7): fold the plant sub-hash in LAST, append-only. An empty plant
    // roster yields an empty plant_hash (GameSession::ComputePlantSubHash), so the composite
    // differs from the pre-fold value ONLY by the literal "|plants:" suffix (additivity guard) —
    // the bytes before it stay byte-identical. This moved the canonical empty-roster composite
    // once.
    return Persistence::StableChecksum(
        chunk_hash + "|wind:" + wind_hash + "|weather:" + weather_hash + "|aether:" + aether_hash +
        "|scents:" + scent_hash + "|ecology:" + ecology_hash + "|plants:" + plant_hash);
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

ServerWorldRunner::ServerWorldRunner(ServerWorldRunnerConfig config)
    : m_config(std::move(config)) {}

ServerWorldRunner::~ServerWorldRunner() {
    Shutdown();
}

bool ServerWorldRunner::Boot() {
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

    m_session = std::make_unique<world::GameSession>();
    m_session->SetJobSystem(&m_jobSystem);
    m_session->SetRootPath(m_config.root_path);
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
        SpawnPlantRoster(m_session->GetRegistry(), spawn_anchor);
        LUMINUMBRA_CORE_INFO("ServerWorldRunner: spawned deterministic plant roster (6 plants).");
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
        LUMIN_PROFILE_ZONE_N("server_tick"); // no-op unless LUMINUMBRA_ENABLE_TRACY
        // One frame == one fixed tick: feeding the clock exactly fixed_dt
        // keeps the frame/tick mapping 1:1 and removes wall-clock timing from
        // the simulation entirely (determinism discipline).
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
        report.ticks_executed += m_session->TickSimulation(fixed_dt);
        report.frames_executed += 1;

        // Supply the simulation tick to the activation queue so dispatched
        // batches receive deterministic due ticks.
        world_system->begin_tick(static_cast<std::int64_t>(report.ticks_executed));
        // Drive activation-latency telemetry from the same tick base used by
        // the availability digest.
        // Observability only — gated on --avail-trace like the digest itself.
        if (m_config.availability_trace) {
            world_system->begin_tick_shadow(static_cast<std::int64_t>(report.ticks_executed));
        }

        // Spawn-anchor streaming, then quiesce in-flight generation/meshing
        // so every scheduler decision next frame observes the identical
        // settled state in both determinism runs. With avatars, stream
        // around the UNION of avatar positions (multi-anchor); with none, the
        // single spawn anchor via the Vec3 overload (byte-identical to zero-avatar).
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
                // B' harness: walk the streaming anchor +X/+Z each tick so chunks stream IN ahead
                // and OUT behind during the run. ~0.5 m/tick -> ~45 m over 90 ticks (~3 chunks).
                // Pure fn of the tick -> both --smoke runs drift identically; any run!=replay is
                // the moving-case water nondeterminism (boot warm-up only settles the INITIAL
                // residency).
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
            world_system->activate_due(static_cast<std::int64_t>(report.ticks_executed));
        }
        const double _wait_ms =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - _wait_t0)
                .count();
        wait_samples.push_back(_wait_ms);
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

        if (m_config.autosave_interval_ticks > 0 && report.ticks_executed > 0 &&
            (report.ticks_executed % m_config.autosave_interval_ticks) == 0) {
            world::WorldStateSaveReport save_report;
            if (m_session->SaveWorldState(&save_report)) {
                report.autosave_passes += 1;
                if (save_report.saved && save_report.chunks_dirty > 0) {
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
    return ComposeWorldHash(service.world_hash(state),
                            WindSubHash(m_session.get()),
                            WeatherSubHash(m_session.get()),
                            AetherSubHash(m_session.get()),
                            ScentSubHash(m_session.get()),
                            ComputeEcologySubHash(),
                            m_session->ComputePlantSubHash()); //  plant fold (empty-neutral)
}

Persistence::WorldStreamingStateSubHashes ServerWorldRunner::ComputeWorldSubHashes() {
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
    // oracle's authoritative-compare slot when sim.aether_state is ON (Codex
    // finding A). Always empty on the default (OFF) world.
    sub.aether_state = m_session ? m_session->ComputeAetherStateSubHash() : std::string();
    return sub;
}

void ServerWorldRunner::ComputeWorldHashAndSubHashes(
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
    out_world_hash = ComposeWorldHash(service.world_hash(state),
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

std::size_t ServerWorldRunner::SaveFullSnapshot() {
    // write the FULL in-memory streamed-chunk set (not dirty-gated) so
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
    // refresh world_info.json alongside the chunk snapshot so world-level
    // sim state captured there (the water sim-window cursor) reflects THIS save
    // moment, not world creation. Without this a loaded session resimulates from a
    // stale cursor and the water evolution diverges from the original's.
    m_session->SaveWorld();
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

std::string ServerWorldRunner::ComputeEcologySubHash() const {
    // gate-populated-world-replay: id-ordered hash over the live creature roster
    // (empty/neutral when none). Pure read over the session registry; no quiesce
    // needed (creature state is registry-resident, not chunk-derived).
    if (!m_session) {
        return {};
    }
    return luminumbra::ai::ComputeEcologySubHash(m_session->GetRegistry());
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
        // Save on shutdown via WorldSaveService (incremental contract: a
        // never-edited world writes nothing and stays on the fresh path).
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
