#include "ModeHelpers.h"

#include <cstdlib>
#include <filesystem>
#include <system_error>

#include "luminumbra_common/core/SystemConfig.h"
#include "luminumbra_common/world/TerrainPresetLoader.h"
#include <nlohmann/json.hpp>

Luminumbra::Net::HelloMsg RuntimeHelloFrom(const ServerCliOptions& options) {
    namespace fs = std::filesystem;
    const auto root = options.root.empty() ? fs::path(".") : fs::path(options.root);
    auto path = root / "data/common/systems.json";
    std::error_code error;
    if (!fs::exists(path, error))
        path = fs::path("data/common/systems.json");
    Luminumbra::Net::HelloMsg hello;
    hello.seed = std::strtoull(options.seed.c_str(), nullptr, 10);
    hello.preset = options.preset;
    hello.client_id = LocalNetworkPlayerId(options);
    Luminumbra::world::GameSession session;
    session.ApplySystemConfig(options.systems_config_override
                                  ? *options.systems_config_override
                                  : luminumbra::core::SystemConfig::LoadFromFile(path.string()));
    const auto runner_config = RunnerConfigFrom(options);
    session.SetFiniteHydrologyEnabled(runner_config.finite_hydrology);
    if (runner_config.region_scheduler)
        session.SetRegionSchedulerConfig(*runner_config.region_scheduler);
    hello.system_config_identity = session.SystemConfigIdentity();
    const auto preset =
        options.preset_json_override
            ? Luminumbra::world::LoadTerrainPresetFromJson(
                  nlohmann::json::parse(*options.preset_json_override), root / "data")
            : Luminumbra::world::LoadTerrainPreset(
                  root / "worlds/atlas/presets" / (options.preset + ".json"), root / "data");
    hello.preset_content_hash = preset.preset_content_hash;
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
    config.parked_bytes_limit = options.parked_bytes_limit;
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
