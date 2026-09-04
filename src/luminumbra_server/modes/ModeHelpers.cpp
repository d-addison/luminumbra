#include "ModeHelpers.h"

Luminumbra::Server::ServerWorldRunnerConfig RunnerConfigFrom(const ServerCliOptions& options) {
    Luminumbra::Server::ServerWorldRunnerConfig config;
    config.root_path = options.root;
    config.seed = options.seed;
    config.preset = options.preset;
    config.world_id = options.world_id;
    config.surface_radius = options.surface_radius;
    config.collision_radius = options.collision_radius;
    config.autosave_interval_ticks = options.autosave_ticks;
    config.avatar_count = options.avatars;
    config.ecology_roster = options.ecology_roster;
    config.planted_roster = options.planted_roster;
    config.moving_anchor = options.moving;
    config.availability_trace = options.availability_trace;
    config.water_hash_trace = options.water_hash_trace;
    config.water_smoke = options.water_smoke;
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
