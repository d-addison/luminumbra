#include "ModeHelpers.h"

#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>

#include "../ServerCliOptions.h"
#include "../ServerWorldRunner.h"
#include "luminumbra_common/core/SystemConfig.h"
#include "luminumbra_common/net/LockstepSession.h"
#include "luminumbra_common/network/NetworkLoopbackAuthority.h"
#include "luminumbra_common/replay/ReplayStream.h"
#include "luminumbra_common/simulation/SimBudgetProfile.h"
#include "luminumbra_common/world/GameSession.h"
#include "luminumbra_common/world/SurfaceWorkBudget.h"
#include "luminumbra_common/world/TerrainPresetLoader.h"
#include <nlohmann/json.hpp>
#include <nlohmann/json_fwd.hpp>

Luminumbra::Net::HelloMsg RuntimeHelloFrom(const ServerCliOptions& options) {
    namespace fs = std::filesystem;
    const auto root = options.root.empty() ? fs::path(".") : fs::path(options.root);
    Luminumbra::Net::HelloMsg hello;
    hello.seed = std::strtoull(options.seed.c_str(), nullptr, 10);
    hello.preset = options.preset;
    hello.client_id = LocalNetworkPlayerId(options);
    Luminumbra::world::GameSession session;
    session.ApplySystemConfig(options.systems_config_override
                                  ? *options.systems_config_override
                                  : luminumbra::core::SystemConfig::LoadFromRuntimeRoot(root));
    const auto runner_config = RunnerConfigFrom(options);
    session.SetFiniteHydrologyEnabled(runner_config.finite_hydrology);
    if (runner_config.region_scheduler)
        session.SetRegionSchedulerConfig(*runner_config.region_scheduler);
    // Network hosts create a new world with this calendar, even when active regions are off.
    hello.system_config_identity = session.SystemConfigIdentity(Luminumbra::world::WorldCalendar{});
    try {
        nlohmann::json source;
        if (options.preset_json_override) {
            source = nlohmann::json::parse(*options.preset_json_override);
        } else {
            std::ifstream input(root / "worlds/atlas/presets" / (options.preset + ".json"));
            if (!input)
                return hello;
            source = nlohmann::json::parse(input);
        }
        const auto preset = Luminumbra::world::LoadTerrainPresetFromJson(
            Luminumbra::world::ResolveEffectivePreset(
                source, Luminumbra::world::TerrainPresetUse::NewWorld),
            root / "data");
        hello.preset_content_hash = preset.preset_content_hash;
    } catch (const nlohmann::json::exception&) {
        // Join and soak clients have no prior world validation. Keep the empty
        // identity on parse/resolution failure so shared admission refuses it.
        return hello;
    }
    return hello;
}

Luminumbra::Server::ServerWorldRunnerConfig RunnerConfigFrom(const ServerCliOptions& options) {
    Luminumbra::Server::ServerWorldRunnerConfig config;
    config.root_path = options.root;
    config.systems_config_override = options.systems_config_override;
    config.preset_json_override = options.preset_json_override;
    config.seed = options.seed;
    config.preset = options.preset;
    config.world_id = options.world_id;
    config.surface_radius = options.surface_radius;
    config.collision_radius = options.collision_radius;
    config.test_streaming_radius_cap = options.test_streaming_radius_cap;
    config.autosave_interval_ticks = options.autosave_ticks;
    config.avatar_count = options.avatars;
    config.ecology_roster = options.ecology_roster;
    config.planted_roster = options.planted_roster;
    config.moving_anchor = options.moving;
    config.availability_trace = options.availability_trace;
    config.water_hash_trace = options.water_hash_trace;
    config.water_smoke = options.water_smoke;
    config.sim_budget = options.sim_budget;
    if (options.sim_budget_profile && options.sim_budget_profile->region_scheduler)
        config.region_scheduler =
            luminumbra::simulation::ToRegionSchedulerConfig(*options.sim_budget_profile);
    return config;
}

std::optional<Luminumbra::Server::ServerWorldRunnerConfig>
ReplayRunnerConfigFrom(const ServerCliOptions& options,
                       const Luminumbra::Replay::ReplayHeader& header) {
    namespace fs = std::filesystem;
    using Luminumbra::world::TerrainPresetUse;
    if (!Luminumbra::World::SurfaceRadiiSupported(header.surface_radius, header.collision_radius))
        return std::nullopt;
    Luminumbra::Server::ServerWorldRunnerConfig config;
    config.root_path = options.root;
    config.seed = header.seed_string;
    config.preset = header.preset;
    config.world_id.clear();
    config.world_name = "Replay Playback";
    config.preset_use = TerrainPresetUse::Historical;
    config.surface_radius = static_cast<int>(header.surface_radius);
    config.collision_radius = static_cast<int>(header.collision_radius);
    config.test_streaming_radius_cap = options.test_streaming_radius_cap;
    config.autosave_interval_ticks = 0;
    config.region_scheduler = RunnerConfigFrom(options).region_scheduler;
    const auto root = options.root.empty() ? fs::path(".") : fs::path(options.root);
    // Freeze the configuration used for identity admission and subsequent boot.
    const auto systems_config = options.systems_config_override
                                    ? *options.systems_config_override
                                    : luminumbra::core::SystemConfig::LoadFromRuntimeRoot(root);
    config.systems_config_override = systems_config;
    Luminumbra::world::GameSession identity_session;
    identity_session.ApplySystemConfig(systems_config);
    identity_session.SetFiniteHydrologyEnabled(config.finite_hydrology);
    if (config.region_scheduler)
        identity_session.SetRegionSchedulerConfig(*config.region_scheduler);
    try {
        nlohmann::json source;
        if (options.preset_json_override) {
            source = nlohmann::json::parse(*options.preset_json_override);
        } else {
            const auto validation = Luminumbra::world::GameSession::ValidateWorldConfig(
                options.root, header.preset, {});
            if (!validation.ok)
                return std::nullopt;
            std::ifstream input(validation.preset_path);
            if (!input)
                return std::nullopt;
            source = nlohmann::json::parse(input);
        }
        // Exactly two supported resolutions, historical first. Never substitute
        // an arbitrary hash or rewrite an explicit selector to obtain a match.
        for (const auto use : {TerrainPresetUse::Historical, TerrainPresetUse::NewWorld}) {
            const auto resolved = Luminumbra::world::ResolveEffectivePreset(source, use);
            const auto preset =
                Luminumbra::world::LoadTerrainPresetFromJson(resolved, root / "data");
            if (preset.ok &&
                Luminumbra::Replay::ConfigurationMatches(
                    header,
                    identity_session.SystemConfigIdentity(Luminumbra::world::WorldCalendar{}),
                    preset.preset_content_hash)) {
                config.preset_json_override = resolved.dump();
                config.required_preset_content_hash = header.preset_content_hash;
                return config;
            }
        }
    } catch (const nlohmann::json::exception&) {
        return std::nullopt;
    }
    return std::nullopt;
}

std::uint32_t ExpectedNetworkClients(const ServerCliOptions& options) {
    return options.clients > 0 ? static_cast<std::uint32_t>(options.clients) : 1u;
}

std::uint32_t LocalNetworkPlayerId(const ServerCliOptions& options) {
    return options.player_id == 0u ? 1u : options.player_id;
}

bool ResolveNetworkClientPort(const std::uint16_t base_port,
                              const std::uint32_t client_id,
                              std::uint16_t& out_port) {
    return luminumbra::network::TryNetworkMultiClientAcceptPortForClient(
        base_port, client_id, out_port);
}
