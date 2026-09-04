#if defined(_WIN32) && !defined(NOMINMAX)
#define NOMINMAX
#endif

#include <glad/glad.h>

#include "core/Log.h"
#include "core/RuntimeScenarioHarness.h"
#include "core/scenarios/ScenarioCommon.h"
#include "luminumbra_common/ai/CreatureSpeciesRegistry.h" //  species base_color -> creature tint
#include "luminumbra_common/animation/AnimationRuntime.h"
#include "luminumbra_common/components/CoreComponents.h"
#include "luminumbra_common/components/InstinctComponents.h"
#include "luminumbra_common/core/Environment.h"
#include "luminumbra_common/core/JobSystem.h"
#include "luminumbra_common/ecs/EntitySnapshot.h"
#include "luminumbra_common/persistence/WorldPersistenceRoundtrip.h"
#include "luminumbra_common/persistence/WorldSaveService.h"
#include "luminumbra_common/systems/CreatureProcgen.h" //  genome -> body-proportion build
#include "luminumbra_common/systems/PhysicsSystem.h"
#include "luminumbra_common/systems/SHIELD_WorldSystem.h"
#include "luminumbra_common/systems/WeatherSystem.h"
#include "luminumbra_common/systems/WindFieldSystem.h"
#include "luminumbra_common/world/GameSession.h"
#include "luminumbra_common/world/WorldStreamingState.h"
#include "rendering/Camera.h"
#include "rendering/LightningBolt.h"
#include "rendering/passes/FoliagePass.h"
#include "rendering/passes/ParticlePass.h"
// lockstep transport seam (engine-generic; ILockstepTransport +
// LoopbackTransport + LockstepSession). Named SendFrame/TryReceiveFrame to dodge
// the <windows.h> SendMessage macro (see LockstepSession.h note).
#include "luminumbra_common/net/LockstepSession.h"
//  (AU1): atmosphere audio telemetry. The harness sweeps the replicated
// weather/wind state through the REAL EnvironmentalAudioSystem atmosphere model +
// the AudioPropagationSystem ambience bed and emits the AtmosphereAudio artifact.
// Client-side dressing only -- no world_hash, no visual-gate dependency.
#include "audio/AudioPropagationSystem.h"
#include "audio/EnvironmentalAudioSystem.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <limits>
#include <sstream>
#include <system_error>

namespace Luminumbra::Client::ScenarioHarness {

namespace anim = luminumbra::animation;

// --- creature_slice_smoke ---

namespace {

// Stable storage for the creature's runtime skeleton + clips (the scenario
// spawns one creature once); keyed by clip name from the archetype data.
// AnimationPlayerComponent holds raw pointers into these, so they must outlive
// the spawned creature and must not move. FindCreatureClip also hands out
// pointers into the map's nodes. Static storage is the requirement, not a
// convenience.
// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
anim::Skeleton g_creature_skeleton;
// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
std::map<std::string, anim::AnimationClip> g_creature_clips;

const anim::AnimationClip* FindCreatureClip(const std::string& name) {
    const auto found = g_creature_clips.find(name);
    return found == g_creature_clips.end() ? nullptr : &found->second;
}

Luminumbra::Components::OpportunityComponent OpportunityFromJson(const nlohmann::json& data) {
    Luminumbra::Components::OpportunityComponent opportunity;
    opportunity.id = data.at("id").get<std::string>();
    opportunity.action = data.at("action").get<std::string>();
    opportunity.target = data.at("target").get<std::string>();
    opportunity.need = data.at("need").get<std::string>();
    opportunity.satisfaction = data.at("satisfaction").get<float>();
    opportunity.urgency = data.at("urgency").get<float>();
    opportunity.risk = data.at("risk").get<float>();
    opportunity.stamina_cost = data.at("stamina_cost").get<float>();
    return opportunity;
}

void ApplyLocomotionFromJson(entt::registry& registry,
                             entt::entity entity,
                             const nlohmann::json& data,
                             CreatureSliceScene& scene) {
    auto& profile = registry.get_or_emplace<Luminumbra::Components::LocomotionProfile>(entity);
    profile.move_speed = data.value("move_speed", profile.move_speed);
    profile.arrival_radius = data.value("arrival_radius", profile.arrival_radius);
    profile.slow_radius = data.value("slow_radius", profile.slow_radius);
    profile.separation_radius = data.value("separation_radius", profile.separation_radius);
    profile.separation_strength = data.value("separation_strength", profile.separation_strength);
    profile.flock_radius = data.value("flock_radius", profile.flock_radius);
    profile.cohesion_strength = data.value("cohesion_strength", profile.cohesion_strength);
    profile.alignment_strength = data.value("alignment_strength", profile.alignment_strength);
    (void)registry.get_or_emplace<Luminumbra::Components::LocomotionIntentComponent>(entity);
    scene.ecology_locomotion = true;
}

void ApplySensableFromJson(entt::registry& registry,
                           entt::entity entity,
                           const nlohmann::json& data,
                           CreatureSliceScene& scene) {
    auto& sensable = registry.get_or_emplace<Luminumbra::Components::SensableComponent>(entity);
    sensable.scent_channel =
        data.value("scent_channel", data.value("channel", sensable.scent_channel));
    sensable.scent_deposit =
        data.value("scent_deposit", data.value("deposit", sensable.scent_deposit));
    sensable.noise_loudness = data.value("noise_loudness", sensable.noise_loudness);
    sensable.noise_pitch = data.value("noise_pitch", sensable.noise_pitch);
    sensable.faction =
        static_cast<std::uint32_t>(data.value("faction", static_cast<int>(sensable.faction)));
    if (sensable.scent_channel >= 0 && sensable.scent_deposit > 0.0f) {
        scene.ecology_scent_emitter = true;
    }
}

void ApplyScentSenseFromJson(entt::registry& registry,
                             entt::entity entity,
                             const nlohmann::json& data,
                             CreatureSliceScene& scene) {
    auto& sense = registry.get_or_emplace<Luminumbra::Components::ScentSenseComponent>(entity);
    sense.channel = data.value("channel", sense.channel);
    sense.sign = data.value("sign", sense.sign);
    sense.strength = data.value("strength", sense.strength);
    sense.floor = data.value("floor", sense.floor);
    sense.weber_k = data.value("weber_k", sense.weber_k);
    (void)registry.get_or_emplace<Luminumbra::Components::LocomotionProfile>(entity);
    (void)registry.get_or_emplace<Luminumbra::Components::LocomotionIntentComponent>(entity);
    scene.ecology_scent_sense = sense.channel >= 0;
    scene.ecology_locomotion = true;
}

void ApplyPerceptionFromJson(entt::registry& registry,
                             entt::entity entity,
                             const nlohmann::json& data,
                             CreatureSliceScene& scene) {
    auto& perception = registry.get_or_emplace<Luminumbra::Components::PerceptionComponent>(entity);
    perception.vision_cos_half_fov =
        data.value("vision_cos_half_fov", perception.vision_cos_half_fov);
    perception.vision_range = data.value("vision_range", perception.vision_range);
    perception.facing_x = data.value("facing_x", perception.facing_x);
    perception.facing_z = data.value("facing_z", perception.facing_z);
    perception.faction =
        static_cast<std::uint32_t>(data.value("faction", static_cast<int>(perception.faction)));
    (void)registry.get_or_emplace<Luminumbra::Components::AwarenessComponent>(entity);
    scene.ecology_perception = true;
}

void ApplyOptionalEcologyBlocks(entt::registry& registry,
                                entt::entity entity,
                                const nlohmann::json& data,
                                CreatureSliceScene& scene) {
    if (data.contains("locomotion")) {
        ApplyLocomotionFromJson(registry, entity, data.at("locomotion"), scene);
    }
    if (data.contains("sensable")) {
        ApplySensableFromJson(registry, entity, data.at("sensable"), scene);
    }
    if (data.contains("scent_emitter")) {
        ApplySensableFromJson(registry, entity, data.at("scent_emitter"), scene);
    }
    if (data.contains("scent_sense")) {
        ApplyScentSenseFromJson(registry, entity, data.at("scent_sense"), scene);
    }
    if (data.contains("perception")) {
        ApplyPerceptionFromJson(registry, entity, data.at("perception"), scene);
    }
}

} // namespace

CreatureSliceScene SpawnCreatureSliceScene(Luminumbra::world::GameSession* game_session,
                                           const std::filesystem::path& root_dir,
                                           const std::string& archetype_relative_path) {
    CreatureSliceScene scene;
    if (!game_session || !game_session->GetWorldSystem()) {
        scene.failure_reason = "no_world_system";
        return scene;
    }
    if (archetype_relative_path.empty()) {
        scene.failure_reason = "no_creature_archetype_argument";
        return scene;
    }
    auto* world_system = game_session->GetWorldSystem();

    // Game data: the supplied archetype drives everything below (the engine
    // harness names no game content; --creature-archetype does).
    const std::filesystem::path archetype_path = root_dir / archetype_relative_path;
    {
        std::ifstream input(archetype_path);
        if (!input.is_open()) {
            scene.failure_reason = "archetype_data_missing";
            return scene;
        }
        try {
            input >> scene.archetype;
        } catch (...) {
            scene.failure_reason = "archetype_data_invalid";
            return scene;
        }
    }
    if (!scene.archetype.contains("creature") || !scene.archetype.contains("slice")) {
        scene.failure_reason = "archetype_missing_creature_slice_blocks";
        return scene;
    }
    scene.archetype["__source_path"] = archetype_relative_path;
    const nlohmann::json& creature_data = scene.archetype.at("creature");
    const nlohmann::json& slice = scene.archetype.at("slice");
    scene.archetype_name = scene.archetype.at("archetype").get<std::string>();
    scene.expected_before_action = slice.at("expected_before_action").get<std::string>();
    scene.expected_after_action = slice.at("expected_after_action").get<std::string>();

    // Rigged mesh + clips (committed game assets under data/models/).
    const std::string mesh_relative = creature_data.at("mesh").get<std::string>();
    anim::SkinnedMeshAsset mesh_asset;
    if (!anim::LoadSkinnedMeshAsset((root_dir / mesh_relative).string(), mesh_asset)) {
        scene.failure_reason = "creature_mesh_load_failed";
        return scene;
    }
    g_creature_skeleton = anim::BuildSkeleton(mesh_asset);
    g_creature_clips.clear();
    for (const auto& [clip_name, clip_path] : creature_data.at("clips").items()) {
        anim::AnimClipAsset clip_asset;
        if (!anim::LoadAnimClipAsset((root_dir / clip_path.get<std::string>()).string(),
                                     clip_asset)) {
            scene.failure_reason = "creature_clip_load_failed:" + clip_name;
            return scene;
        }
        g_creature_clips.emplace(clip_name, anim::BuildClip(clip_asset));
    }

    // Stage selection: the archipelago spawn neighborhood is spiky; spiral
    // outward for a locally flat, dry pocket so the creature, the graze
    // spot, the stimulus, and the camera all share usable ground.
    const Luminumbra::Vec3 spawn = game_session->GetMetadata().spawnPoint;
    const auto local_relief = [world_system](float x, float z) {
        const float h = world_system->GetTerrainHeightAt(x, z);
        float worst = 0.0f;
        const std::array<std::pair<float, float>, 8> probes{{{2.5f, 0.0f},
                                                             {-2.5f, 0.0f},
                                                             {0.0f, 2.5f},
                                                             {0.0f, -2.5f},
                                                             {2.0f, 2.0f},
                                                             {-2.0f, 2.0f},
                                                             {2.0f, -2.0f},
                                                             {-2.0f, -2.0f}}};
        for (const auto& [dx, dz] : probes) {
            worst =
                std::max(worst, std::fabs(world_system->GetTerrainHeightAt(x + dx, z + dz) - h));
        }
        return worst;
    };
    float creature_x = spawn.x + 4.0f;
    float creature_z = spawn.z + 2.0f;
    {
        float best_relief = std::numeric_limits<float>::max();
        for (float radius = 4.0f; radius <= 28.0f; radius += 4.0f) {
            for (int step = 0; step < 12; ++step) {
                const float angle = glm::radians(30.0f * static_cast<float>(step));
                const float x = spawn.x + std::sin(angle) * radius;
                const float z = spawn.z + std::cos(angle) * radius;
                const float h = world_system->GetTerrainHeightAt(x, z);
                if (h < Luminumbra::SEA_LEVEL + 1.5f) {
                    continue; // keep the stage dry
                }
                const float relief = local_relief(x, z);
                if (relief < best_relief) {
                    best_relief = relief;
                    creature_x = x;
                    creature_z = z;
                }
            }
            if (best_relief <= 1.2f) {
                break; // flat enough; prefer the nearest qualifying pocket
            }
        }
    }
    const float creature_y = world_system->GetTerrainHeightAt(creature_x, creature_z);
    scene.creature_position = {creature_x, creature_y, creature_z};

    const nlohmann::json& graze_data = slice.at("graze_opportunity");
    scene.graze_position =
        scene.creature_position + Luminumbra::Vec3(graze_data.at("offset").at("x").get<float>(),
                                                   0.0f,
                                                   graze_data.at("offset").at("z").get<float>());
    scene.graze_position.y =
        world_system->GetTerrainHeightAt(scene.graze_position.x, scene.graze_position.z);

    // Stimulus placement: prefer the archetype's offset, but the planner
    // scores 3D distance — on sloped terrain a spot down a cliff face is
    // legitimately not worth approaching. Scan a ring of candidates and take
    // the first within 1.5 m of the creature's elevation.
    const nlohmann::json& stimulus_data = slice.at("light_stimulus");
    {
        const float preferred_dx = stimulus_data.at("offset").at("x").get<float>();
        const float preferred_dz = stimulus_data.at("offset").at("z").get<float>();
        const float preferred_radius =
            std::sqrt(preferred_dx * preferred_dx + preferred_dz * preferred_dz);
        const float preferred_angle = std::atan2(preferred_dx, preferred_dz);
        bool placed = false;
        for (const float ring_radius : {preferred_radius, 3.2f, 2.6f}) {
            for (int step = 0; step < 12 && !placed; ++step) {
                // 0, +30, -30, +60, -60... degrees around the preferred azimuth.
                const float delta = glm::radians(30.0f) * static_cast<float>((step + 1) / 2) *
                                    ((step % 2) == 0 ? 1.0f : -1.0f);
                const float angle = preferred_angle + (step == 0 ? 0.0f : delta);
                const float x = scene.creature_position.x + std::sin(angle) * ring_radius;
                const float z = scene.creature_position.z + std::cos(angle) * ring_radius;
                const float y = world_system->GetTerrainHeightAt(x, z);
                // The spot AND the walking path to it must stay near the
                // creature's elevation (no spike tops across gullies).
                const float mid_y = world_system->GetTerrainHeightAt(
                    (x + scene.creature_position.x) * 0.5f, (z + scene.creature_position.z) * 0.5f);
                if (std::fabs(y - scene.creature_position.y) <= 1.5f &&
                    std::fabs(mid_y - scene.creature_position.y) <= 2.0f) {
                    scene.stimulus_position = {x, y, z};
                    placed = true;
                }
            }
            if (placed) {
                break;
            }
        }
        if (!placed) {
            scene.stimulus_position =
                scene.creature_position + Luminumbra::Vec3(preferred_dx, 0.0f, preferred_dz);
            scene.stimulus_position.y = world_system->GetTerrainHeightAt(scene.stimulus_position.x,
                                                                         scene.stimulus_position.z);
        }
    }

    entt::registry& registry = game_session->GetRegistry();

    // The creature: rigged mesh + animation player + planner agent, all from
    // archetype data.
    const auto creature = registry.create();
    {
        auto& transform = registry.emplace<Luminumbra::Components::TransformComponent>(creature);
        transform.position = scene.creature_position;
        auto& mesh_component =
            registry.emplace<Luminumbra::Components::SkinnedMeshComponent>(creature);
        mesh_component.meshPath = mesh_relative;
        mesh_component.materialId = creature_data.value("material_id", 2u);
        //  tint the creature from its species base_color (by archetype name), so
        // the rendered creature matches its codex identity. White (no-op) if unregistered.
        {
            luminumbra::ai::CreatureSpeciesRegistry _sp_reg;
            std::vector<std::string> _sp_errs;
            _sp_reg.LoadFromDirectory(root_dir / "data" / "common" / "creatures" / "species",
                                      _sp_errs);
            if (const auto* sp = _sp_reg.FindByName(scene.archetype_name)) {
                mesh_component.tintR = sp->base_color[0];
                mesh_component.tintG = sp->base_color[1];
                mesh_component.tintB = sp->base_color[2];
            }
        }
        auto& player = registry.emplace<anim::AnimationPlayerComponent>(creature);
        player.skeleton = &g_creature_skeleton;
        scene.active_clip = "idle";
        player.clip = FindCreatureClip(scene.active_clip);
        player.looping = true;
        auto& agent = registry.emplace<Luminumbra::Components::InstinctAgentComponent>(creature);
        agent.actor_id = scene.archetype.at("actor_id").get<std::string>();
        agent.archetype = scene.archetype_name;
        agent.replan_interval_ticks = creature_data.value("replan_interval_ticks", 10u);
        registry.emplace<Luminumbra::Components::InstinctAgent>(creature);
        auto& needs = registry.emplace<Luminumbra::Components::NeedsComponent>(creature);
        for (const nlohmann::json& need : slice.at("needs")) {
            needs.needs.push_back({need.at("name").get<std::string>(),
                                   need.at("pressure").get<float>(),
                                   need.at("growth_per_tick").get<float>()});
        }
        // OPTIONAL ecology stimulus subscriptions (game-data opt-in).
        // Only archetypes whose slice declares a `stimulus_subscriptions` block
        // react to the environment; the default archetype carries none, so its
        // planner path (and the CreatureSlice gate) is unchanged. Each entry maps a
        // named channel onto a need with a gain; the engine names no channel-to-need
        // semantics -- the archetype does. Unknown channel names are skipped.
        if (slice.contains("stimulus_subscriptions")) {
            auto& subscription =
                registry.emplace<Luminumbra::Components::StimulusSubscriptionComponent>(creature);
            for (const nlohmann::json& entry : slice.at("stimulus_subscriptions")) {
                const std::string channel_name = entry.at("channel").get<std::string>();
                luminumbra::ai::StimulusChannel channel{};
                bool known = true;
                if (channel_name == "weather") {
                    channel = luminumbra::ai::StimulusChannel::Weather;
                } else if (channel_name == "temperature") {
                    channel = luminumbra::ai::StimulusChannel::Temperature;
                } else if (channel_name == "time_of_day") {
                    channel = luminumbra::ai::StimulusChannel::TimeOfDay;
                } else if (channel_name == "season") {
                    channel = luminumbra::ai::StimulusChannel::Season;
                } else if (channel_name == "light_level") {
                    channel = luminumbra::ai::StimulusChannel::LightLevel;
                } else {
                    known = false;
                }
                if (known) {
                    subscription.subscriptions.push_back(
                        {channel, entry.at("need").get<std::string>(), entry.value("gain", 1.0f)});
                }
            }
            if (subscription.subscriptions.empty()) {
                registry.remove<Luminumbra::Components::StimulusSubscriptionComponent>(creature);
            }
        }
        ApplyOptionalEcologyBlocks(registry, creature, creature_data, scene);
    }
    scene.creature = creature;

    // The ambient graze opportunity (pre-stimulus behavior).
    const auto graze = registry.create();
    {
        auto& transform = registry.emplace<Luminumbra::Components::TransformComponent>(graze);
        transform.position = scene.graze_position;
        registry.emplace<Luminumbra::Components::OpportunityComponent>(graze) =
            OpportunityFromJson(graze_data);
    }
    scene.graze_opportunity = graze;

    // Fixed photographic framing covering the creature and the (future)
    // stimulus spot.
    scene.camera_focus = (scene.creature_position + scene.stimulus_position) * 0.5f +
                         Luminumbra::Vec3(0.0f, 1.0f, 0.0f);
    const float cam_x = scene.camera_focus.x + 9.0f;
    const float cam_z = scene.camera_focus.z - 3.0f;
    const float cam_terrain = world_system->GetTerrainHeightAt(cam_x, cam_z);
    const float cam_y = std::max(scene.camera_focus.y + 2.6f, cam_terrain + 1.7f);
    scene.camera_position = {cam_x, cam_y, cam_z};

    scene.spawned = true;
    LUMINUMBRA_CORE_INFO("creature_slice_smoke: spawned {} at ({:.1f}, {:.1f}, {:.1f}); stimulus "
                         "spot ({:.1f}, {:.1f}, {:.1f})",
                         scene.archetype_name,
                         scene.creature_position.x,
                         scene.creature_position.y,
                         scene.creature_position.z,
                         scene.stimulus_position.x,
                         scene.stimulus_position.y,
                         scene.stimulus_position.z);
    return scene;
}

bool SpawnCreatureSliceStimulus(Luminumbra::world::GameSession* game_session,
                                CreatureSliceScene& scene) {
    if (!game_session || !scene.spawned || scene.stimulus_spawned) {
        return scene.stimulus_spawned;
    }
    const nlohmann::json& stimulus_data = scene.archetype.at("slice").at("light_stimulus");

    entt::registry& registry = game_session->GetRegistry();
    const auto stimulus = registry.create();
    auto& transform = registry.emplace<Luminumbra::Components::TransformComponent>(stimulus);
    transform.position = scene.stimulus_position;
    // The visible light source: a prop drawn through the instanced static
    // mesh path with the emissive LUT material, both named by the archetype
    // data (data/common/materials.json gives the material non-zero emission;
    // the lighting pass renders the glow).
    if (stimulus_data.contains("prop_mesh")) {
        auto& mesh = registry.emplace<Luminumbra::Components::StaticMeshComponent>(stimulus);
        mesh.meshPath = stimulus_data.at("prop_mesh").get<std::string>();
        mesh.materialId = stimulus_data.value("emissive_material_id", 6u);
        LUMINUMBRA_CORE_INFO("creature_slice_smoke: stimulus prop '{}' (material {})",
                             mesh.meshPath,
                             mesh.materialId);
    }
    registry.emplace<Luminumbra::Components::OpportunityComponent>(stimulus) =
        OpportunityFromJson(stimulus_data);
    ApplyOptionalEcologyBlocks(registry, stimulus, stimulus_data, scene);

    scene.stimulus = stimulus;
    scene.stimulus_spawned = true;
    return true;
}

void UpdateCreatureSliceScene(Luminumbra::world::GameSession* game_session,
                              CreatureSliceScene& scene,
                              double dt) {
    if (!game_session || !scene.spawned) {
        return;
    }
    entt::registry& registry = game_session->GetRegistry();
    if (!registry.valid(scene.creature)) {
        return;
    }
    auto* agent = registry.try_get<Luminumbra::Components::InstinctAgentComponent>(scene.creature);
    auto* player = registry.try_get<anim::AnimationPlayerComponent>(scene.creature);
    auto* transform = registry.try_get<Luminumbra::Components::TransformComponent>(scene.creature);
    if (agent == nullptr || player == nullptr || transform == nullptr) {
        return;
    }

    std::string current_action;
    if (agent->current_plan.selected_index >= 0 &&
        static_cast<std::size_t>(agent->current_plan.selected_index) <
            agent->current_plan.candidates.size()) {
        current_action =
            agent->current_plan
                .candidates[static_cast<std::size_t>(agent->current_plan.selected_index)]
                .action;
    }

    // Planner-driven clip choice from the archetype's clip_by_action map.
    const nlohmann::json& clip_by_action = scene.archetype.at("creature").at("clip_by_action");
    if (!current_action.empty() && clip_by_action.contains(current_action)) {
        const std::string clip_name = clip_by_action.at(current_action).get<std::string>();
        if (clip_name != scene.active_clip) {
            if (const anim::AnimationClip* clip = FindCreatureClip(clip_name)) {
                player->clip = clip;
                player->time = 0.0;
                scene.active_clip = clip_name;
                LUMINUMBRA_CORE_INFO(
                    "creature_slice_smoke: action '{}' -> clip '{}'", current_action, clip_name);
            }
        }
    }

    // Approach locomotion: walk toward the stimulus, terrain-following,
    // facing the movement direction; stop short of the glow.
    if (current_action == scene.expected_after_action && scene.stimulus_spawned) {
        const Luminumbra::Vec3 to_target = scene.stimulus_position - transform->position;
        const glm::vec2 flat(to_target.x, to_target.z);
        const float distance = glm::length(flat);
        if (distance > 1.8f) {
            const glm::vec2 direction = flat / distance;
            const float step = static_cast<float>(dt) * 1.2f;
            transform->position.x += direction.x * step;
            transform->position.z += direction.y * step;
            if (auto* world_system = game_session->GetWorldSystem()) {
                transform->position.y =
                    world_system->GetTerrainHeightAt(transform->position.x, transform->position.z);
            }
            // The quadruped mesh faces +Z; yaw toward the movement direction.
            const float yaw = std::atan2(direction.x, direction.y);
            transform->rotation = glm::angleAxis(yaw, glm::vec3(0.0f, 1.0f, 0.0f));
        }
    }
    scene.ecology_scent_hash = game_session->ComputeScentSubHash();
}

void ApplyCreatureSliceCamera(Luminumbra::world::GameSession* game_session,
                              Luminumbra::Rendering::Camera* camera,
                              CreatureSliceScene& scene) {
    if (!game_session || !camera || !scene.spawned) {
        return;
    }
    auto* world_system = game_session->GetWorldSystem();
    entt::registry& registry = game_session->GetRegistry();

    Luminumbra::Vec3 creature_pos = scene.creature_position;
    if (registry.valid(scene.creature)) {
        if (const auto* transform =
                registry.try_get<const Luminumbra::Components::TransformComponent>(
                    scene.creature)) {
            creature_pos = transform->position;
        }
    }
    const Luminumbra::Vec3 target =
        scene.stimulus_spawned ? scene.stimulus_position : scene.graze_position;

    glm::vec2 flat_dir(target.x - creature_pos.x, target.z - creature_pos.z);
    if (glm::dot(flat_dir, flat_dir) < 1.0e-4f) {
        flat_dir = glm::vec2(0.0f, 1.0f);
    } else {
        flat_dir = glm::normalize(flat_dir);
    }
    //  re-frame: over-the-shoulder at CREATURE EYE LEVEL with the
    // horizon visible (sky in the upper third), not a high downward shot that
    // stares at the ground. The camera sits just behind and a touch above the
    // creature on the creature->target axis (subject between lens and point of
    // interest), and aims at a focus raised to roughly camera height so the
    // gaze is near-level: a small downward pitch keeps the creature a third up
    // from the bottom while open sky fills the top of the frame.
    const float kEyeLift = 1.6f;      // camera a touch above the creature's body center
    const float kBackDistance = 5.0f; // over-the-shoulder distance
    const glm::vec2 cam_xz = glm::vec2(creature_pos.x, creature_pos.z) - flat_dir * kBackDistance;
    float cam_y = creature_pos.y + kEyeLift;

    // Clear only the terrain directly between the camera and the subject so
    // the lens does not start inside a dune; do NOT lift to clear all the way
    // to the distant target (that is what pitched the old shot into the dirt).
    if (world_system != nullptr) {
        constexpr float kClearance = 0.6f;
        for (float t : {0.0f, 0.25f, 0.5f}) {
            const glm::vec2 sample = glm::mix(cam_xz, glm::vec2(creature_pos.x, creature_pos.z), t);
            const float h = world_system->GetTerrainHeightAt(sample.x, sample.y);
            cam_y = std::max(cam_y, h + kClearance);
        }
    }
    // Cap the lift so the gaze stays near-level (sky stays in frame).
    cam_y = std::min(cam_y, creature_pos.y + 2.8f);

    // Focus: the creature's body center, nudged toward the target, raised to
    // just below camera height. A near-level aim (camera only slightly above
    // focus) seats the horizon around a third down from the top.
    Luminumbra::Vec3 focus = creature_pos * 0.78f + target * 0.22f;
    focus.y = cam_y - 0.6f;

    scene.camera_position = {cam_xz.x, cam_y, cam_xz.y};
    scene.camera_focus = focus;
    camera->Position = scene.camera_position;
    camera->Zoom = 45.0f;
    AimCameraAt(camera, scene.camera_focus);
}

CreatureSlicePlanProbe ProbeCreatureSlicePlan(Luminumbra::world::GameSession* game_session,
                                              const CreatureSliceScene& scene) {
    CreatureSlicePlanProbe probe;
    if (!game_session || !scene.spawned) {
        return probe;
    }
    entt::registry& registry = game_session->GetRegistry();
    if (!registry.valid(scene.creature) ||
        !registry.all_of<Luminumbra::Components::InstinctAgentComponent>(scene.creature)) {
        return probe;
    }
    const auto& agent =
        registry.get<Luminumbra::Components::InstinctAgentComponent>(scene.creature);
    probe.plans_executed = agent.plans_executed;
    probe.checksum = agent.current_plan.checksum;
    probe.active_clip = scene.active_clip;
    probe.camera_position = scene.camera_position;
    if (const auto* transform =
            registry.try_get<const Luminumbra::Components::TransformComponent>(scene.creature)) {
        probe.creature_position = transform->position;
    }
    if (agent.current_plan.selected_index >= 0 &&
        static_cast<std::size_t>(agent.current_plan.selected_index) <
            agent.current_plan.candidates.size()) {
        const auto& winner =
            agent.current_plan
                .candidates[static_cast<std::size_t>(agent.current_plan.selected_index)];
        probe.valid = true;
        probe.action = winner.action;
        probe.target = winner.target;
        probe.need = winner.need;
        probe.score = winner.score;
    }
    return probe;
}

CreatureSliceComposition AnalyzeCreatureSliceComposition(const std::vector<unsigned char>& pixels,
                                                         int width,
                                                         int height,
                                                         int creature_screen_x_from_left,
                                                         int creature_screen_y_from_top,
                                                         int stimulus_screen_x_from_left,
                                                         int stimulus_screen_y_from_top) {
    CreatureSliceComposition comp;
    comp.creature_screen_x = creature_screen_x_from_left;
    comp.creature_screen_y = creature_screen_y_from_top;
    comp.stimulus_screen_x = stimulus_screen_x_from_left;
    comp.stimulus_screen_y = stimulus_screen_y_from_top;
    if (width <= 0 || height <= 0 ||
        pixels.size() < static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 3u) {
        return comp;
    }
    const std::size_t row_stride = static_cast<std::size_t>(width) * 3u;
    const auto px = [&](int x, int y_from_top, int channel) -> unsigned char {
        const int y = height - 1 - y_from_top; // glReadPixels rows are bottom-up
        return pixels[static_cast<std::size_t>(y) * row_stride + static_cast<std::size_t>(x) * 3u +
                      static_cast<std::size_t>(channel)];
    };

    // Whole-frame sky ratio (horizon-in-frame proof). Uses the same sky
    // classifier as the PlayerView gate.
    std::uint64_t sky_pixels = 0;
    const std::uint64_t total_pixels =
        static_cast<std::uint64_t>(width) * static_cast<std::uint64_t>(height);
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            if (IsBelowHorizonSkyPixel(px(x, y, 0), px(x, y, 1), px(x, y, 2))) {
                ++sky_pixels;
            }
        }
    }
    comp.sky_ratio = total_pixels > 0
                         ? static_cast<double>(sky_pixels) / static_cast<double>(total_pixels)
                         : 0.0;

    // Creature ROI: a box around the projected creature position. The
    // half-extent scales with the frame so it covers the subject without
    // swallowing the whole shot.
    const bool in_frame = creature_screen_x_from_left >= 0 && creature_screen_x_from_left < width &&
                          creature_screen_y_from_top >= 0 && creature_screen_y_from_top < height;
    if (!in_frame) {
        return comp; // valid stays false; ROI metrics zero
    }
    const int roi_half = std::max(6, std::min(width, height) / 16);
    const int rx0 = std::max(0, creature_screen_x_from_left - roi_half);
    const int rx1 = std::min(width - 1, creature_screen_x_from_left + roi_half);
    const int ry0 = std::max(0, creature_screen_y_from_top - roi_half);
    const int ry1 = std::min(height - 1, creature_screen_y_from_top + roi_half);

    double roi_sum[3] = {0.0, 0.0, 0.0};
    std::size_t roi_count = 0;
    for (int y = ry0; y <= ry1; ++y) {
        for (int x = rx0; x <= rx1; ++x) {
            // Skip sky pixels inside the ROI so the creature mean reflects the
            // subject, not the sky behind it.
            if (IsBelowHorizonSkyPixel(px(x, y, 0), px(x, y, 1), px(x, y, 2))) {
                continue;
            }
            roi_sum[0] += px(x, y, 0);
            roi_sum[1] += px(x, y, 1);
            roi_sum[2] += px(x, y, 2);
            ++roi_count;
        }
    }

    // Terrain reference: a ring around (but outside) the creature ROI, sky
    // excluded — the ground the creature stands on. Sampled within 3x the ROI
    // half-extent so it stays local to the subject.
    const int ref_half = roi_half * 3;
    const int fx0 = std::max(0, creature_screen_x_from_left - ref_half);
    const int fx1 = std::min(width - 1, creature_screen_x_from_left + ref_half);
    const int fy0 = std::max(0, creature_screen_y_from_top - ref_half);
    const int fy1 = std::min(height - 1, creature_screen_y_from_top + ref_half);
    double ref_sum[3] = {0.0, 0.0, 0.0};
    std::size_t ref_count = 0;
    for (int y = fy0; y <= fy1; ++y) {
        for (int x = fx0; x <= fx1; ++x) {
            if (x >= rx0 && x <= rx1 && y >= ry0 && y <= ry1) {
                continue; // inside the creature ROI
            }
            if (IsBelowHorizonSkyPixel(px(x, y, 0), px(x, y, 1), px(x, y, 2))) {
                continue; // sky is not terrain
            }
            ref_sum[0] += px(x, y, 0);
            ref_sum[1] += px(x, y, 1);
            ref_sum[2] += px(x, y, 2);
            ++ref_count;
        }
    }

    comp.creature_roi_pixels = roi_count;
    comp.terrain_ref_pixels = ref_count;
    if (roi_count > 0 && ref_count > 0) {
        for (int c = 0; c < 3; ++c) {
            comp.creature_roi_mean[c] = roi_sum[c] / static_cast<double>(roi_count);
            comp.terrain_ref_mean[c] = ref_sum[c] / static_cast<double>(ref_count);
        }
        comp.creature_terrain_color_delta =
            std::abs(comp.creature_roi_mean[0] - comp.terrain_ref_mean[0]) +
            std::abs(comp.creature_roi_mean[1] - comp.terrain_ref_mean[1]) +
            std::abs(comp.creature_roi_mean[2] - comp.terrain_ref_mean[2]);
        comp.valid = true;
    }

    //  emissive glow halo around the glow_bloom stimulus prop. Measures
    // three concentric regions centered on the stimulus screen position: a
    // bright inner CORE disc, a falloff RING annulus, and a far BACKGROUND ring.
    // A real glow/bloom reads core > ring > background (luminance falls off into
    // a halo extending beyond the geometry's bright core).
    if (stimulus_screen_x_from_left >= 0 && stimulus_screen_x_from_left < width &&
        stimulus_screen_y_from_top >= 0 && stimulus_screen_y_from_top < height) {
        const int core_r = std::max(3, std::min(width, height) / 48);
        const int ring_r = core_r * 3; // halo annulus extends ~3x the core
        const int bg_r = core_r * 6;   // far background reference
        double core_sum = 0, ring_sum = 0, bg_sum = 0;
        std::size_t core_n = 0, ring_n = 0, bg_n = 0;
        const int bx0 = std::max(0, stimulus_screen_x_from_left - bg_r);
        const int bx1 = std::min(width - 1, stimulus_screen_x_from_left + bg_r);
        const int by0 = std::max(0, stimulus_screen_y_from_top - bg_r);
        const int by1 = std::min(height - 1, stimulus_screen_y_from_top + bg_r);
        for (int y = by0; y <= by1; ++y) {
            for (int x = bx0; x <= bx1; ++x) {
                const double dx = x - stimulus_screen_x_from_left;
                const double dy = y - stimulus_screen_y_from_top;
                const double dist = std::sqrt(dx * dx + dy * dy);
                const double lum = PixelLuminance(px(x, y, 0), px(x, y, 1), px(x, y, 2));
                if (dist <= core_r) {
                    core_sum += lum;
                    ++core_n;
                } else if (dist <= ring_r) {
                    ring_sum += lum;
                    ++ring_n;
                } else if (dist <= bg_r) {
                    bg_sum += lum;
                    ++bg_n;
                }
            }
        }
        if (core_n > 0 && ring_n > 0 && bg_n > 0) {
            comp.glow_core_luminance = core_sum / static_cast<double>(core_n);
            comp.glow_ring_luminance = ring_sum / static_cast<double>(ring_n);
            comp.glow_background_luminance = bg_sum / static_cast<double>(bg_n);
            comp.glow_measured = true;
        }
    }
    return comp;
}

void WriteCreatureSliceAnalysis(const std::filesystem::path& artifact_dir,
                                const CreatureSliceScene& scene,
                                const CreatureSliceCapture& before,
                                const CreatureSliceCapture& after) {
    const GLDebugRuntimeStats gl_debug = CurrentGLDebugRuntimeStats();

    std::vector<std::string> failures;
    if (!scene.spawned) {
        failures.push_back("spawn_failed:" + scene.failure_reason);
    }
    if (!before.plan.valid || before.plan.action != scene.expected_before_action) {
        failures.push_back("pre_stimulus_plan_mismatch");
    }
    if (!after.plan.valid || after.plan.action != scene.expected_after_action) {
        failures.push_back("post_stimulus_plan_mismatch");
    }
    if (after.plan.plans_executed <= before.plan.plans_executed) {
        failures.push_back("planner_not_replanning");
    }
    if (before.skinned_draws == 0 || after.skinned_draws == 0) {
        failures.push_back("creature_not_rendered");
    }
    if (!scene.stimulus_spawned) {
        failures.push_back("stimulus_never_spawned");
    }
    if (gl_debug.errors != 0) {
        failures.push_back("gl_debug_errors");
    }
    //  composition: a frame that renders the creature but stares at the
    // ground/sky, or camouflages the creature against its terrain, is visually
    // broken even when functionally green. Bounds match the validator gate.
    constexpr double kMinSkyRatio = 0.05;
    constexpr double kMaxSkyRatio = 0.6;
    constexpr double kMinColorDelta = 24.0; // L1 over 0-255 RGB means
    for (const auto* cap : {&before, &after}) {
        const CreatureSliceComposition& c = cap->composition;
        if (!c.valid) {
            failures.push_back("composition_invalid:" + cap->file);
            continue;
        }
        if (c.sky_ratio < kMinSkyRatio || c.sky_ratio > kMaxSkyRatio) {
            failures.push_back("composition_sky_ratio_out_of_band:" + cap->file);
        }
        if (c.creature_terrain_color_delta < kMinColorDelta) {
            failures.push_back("composition_creature_low_contrast:" + cap->file);
        }
    }
    //  emissive glow halo: when the glow_bloom stimulus projects into a
    // capture, its emission produces a bloom HALO - a luminance ring that
    // differs from the far background (design wording: "luminance falloff ring
    // beyond geometry bounds"). The crystal's fresnel-edge emission peaks on the
    // rim, so the halo reads as core/ring/background structure rather than a flat
    // patch. The ENFORCED, machine-independent emissive check is the headless
    // monotonic calibration gate (RenderSmokeTest.EmissiveCalibrationMonotonic);
    // this live-scene halo is asserted as STRUCTURE (the three concentric regions
    // are not all near-equal, which a flat unlit sprite would be) so it stays
    // robust to the exact framing while still proving an on-screen glow gradient.
    constexpr double kGlowStructure = 8.0; // max delta across core/ring/bg
    for (const auto* cap : {&before, &after}) {
        const CreatureSliceComposition& c = cap->composition;
        if (!c.glow_measured)
            continue;
        const double lo =
            std::min({c.glow_core_luminance, c.glow_ring_luminance, c.glow_background_luminance});
        const double hi =
            std::max({c.glow_core_luminance, c.glow_ring_luminance, c.glow_background_luminance});
        if (hi - lo < kGlowStructure) {
            failures.push_back("glow_no_halo_gradient:" + cap->file);
        }
    }
    const bool passed = failures.empty();

    const auto probe_json = [](const CreatureSlicePlanProbe& probe) {
        return nlohmann::json{
            {"valid", probe.valid},
            {"action", probe.action},
            {"target", probe.target},
            {"need", probe.need},
            {"score", probe.score},
            {"checksum", probe.checksum},
            {"plans_executed", probe.plans_executed},
            {"active_clip", probe.active_clip},
            {"creature_position", Vec3ToJson(probe.creature_position)},
            {"camera_position", Vec3ToJson(probe.camera_position)},
        };
    };
    const auto composition_json = [](const CreatureSliceComposition& c) {
        return nlohmann::json{
            {"valid", c.valid},
            {"sky_ratio", c.sky_ratio},
            {"creature_terrain_color_delta", c.creature_terrain_color_delta},
            {"creature_roi_mean",
             {c.creature_roi_mean[0], c.creature_roi_mean[1], c.creature_roi_mean[2]}},
            {"terrain_ref_mean",
             {c.terrain_ref_mean[0], c.terrain_ref_mean[1], c.terrain_ref_mean[2]}},
            {"creature_roi_pixels", c.creature_roi_pixels},
            {"terrain_ref_pixels", c.terrain_ref_pixels},
            {"creature_screen_x", c.creature_screen_x},
            {"creature_screen_y", c.creature_screen_y},
            {"glow_measured", c.glow_measured},
            {"glow_core_luminance", c.glow_core_luminance},
            {"glow_ring_luminance", c.glow_ring_luminance},
            {"glow_background_luminance", c.glow_background_luminance},
            {"stimulus_screen_x", c.stimulus_screen_x},
            {"stimulus_screen_y", c.stimulus_screen_y},
        };
    };
    const auto capture_json = [&probe_json,
                               &composition_json](const CreatureSliceCapture& capture) {
        return nlohmann::json{
            {"file", capture.file},
            {"elapsed_seconds", capture.elapsed_seconds},
            {"plan", probe_json(capture.plan)},
            {"skinned_draws", capture.skinned_draws},
            {"skinned_indices_drawn", capture.skinned_indices_drawn},
            {"composition", composition_json(capture.composition)},
        };
    };

    const nlohmann::json artifact = {
        {"schema", "luminumbra.creature_slice_analysis.v1"},
        {"timestamp_utc", TimestampUtc()},
        {"scenario", "creature_slice_smoke"},
        {"archetype", scene.archetype_name},
        {"archetype_data", scene.archetype.value("__source_path", "")},
        {"scene",
         {
             {"creature_position", Vec3ToJson(scene.creature_position)},
             {"graze_position", Vec3ToJson(scene.graze_position)},
             {"stimulus_position", Vec3ToJson(scene.stimulus_position)},
             {"camera_position", Vec3ToJson(scene.camera_position)},
             {"stimulus_spawned", scene.stimulus_spawned},
         }},
        {"ecology",
         {
             {"locomotion", scene.ecology_locomotion},
             {"scent_emitter", scene.ecology_scent_emitter},
             {"scent_sense", scene.ecology_scent_sense},
             {"perception", scene.ecology_perception},
             {"scent_hash", scene.ecology_scent_hash},
         }},
        {"expected",
         {
             {"before_action", scene.expected_before_action},
             {"after_action", scene.expected_after_action},
         }},
        {"before_stimulus", capture_json(before)},
        {"after_stimulus", capture_json(after)},
        {"gl_debug",
         {
             {"messages", gl_debug.messages},
             {"errors", gl_debug.errors},
             {"warnings", gl_debug.warnings},
             {"notifications", gl_debug.notifications},
         }},
        {"failures", failures},
        {"passed", passed},
    };

    std::error_code ec;
    std::filesystem::create_directories(artifact_dir, ec);
    std::ofstream output(artifact_dir / "creature-slice-analysis.json");
    output << std::setw(2) << artifact << '\n';
}

} // namespace Luminumbra::Client::ScenarioHarness
