#include "../fixtures/smoke_artifact/FixedInputs.h"
#include "../support/SimulationContentFixture.h"
#include "../support/UniqueTempPath.h"
#include "luminumbra_common/ai/EcologyHash.h"
#include "luminumbra_common/components/CreatureComponents.h"
#include "luminumbra_common/core/FilesystemPath.h"
#include "luminumbra_common/core/SystemConfig.h"
#include "luminumbra_common/fields/EnergyFieldState.h"
#include "luminumbra_common/replay/RegionScheduleTrace.h"
#include "luminumbra_server/modes/ModeHelpers.h"
#include "luminumbra_server/modes/Modes.h"
#include "luminumbra_server/modes/SmokeArtifact.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <gtest/gtest.h>
#include <iterator>
#include <memory>
#include <nlohmann/json.hpp>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

// Preinclude every implementation dependency globally before wrapping the mode bodies.
// The anonymous checkpoint hooks and refusal fixtures have no linked public interface.
namespace AuthorityIntegrationModes {
#include "../../src/luminumbra_server/modes/Lockstep.cpp"
#include "../../src/luminumbra_server/modes/Replay.cpp"
} // namespace AuthorityIntegrationModes

namespace Luminumbra::Server {
thread_local int capture_refusal_stage = 0;
class CaptureRefusalRunner : public ServerWorldRunner {
public:
    using ServerWorldRunner::ServerWorldRunner;
    ~CaptureRefusalRunner() {
        if (!corruption.empty()) {
            std::error_code error;
            std::filesystem::remove(corruption, error);
        }
    }
    std::string ComputeWorldHash() {
        ++world_captures;
        if ((capture_refusal_stage == 1 && world_captures == 1) ||
            (capture_refusal_stage == 3 && world_captures == 2))
            Corrupt();
        return ServerWorldRunner::ComputeWorldHash();
    }
    void ComputeWorldHashAndSubHashes(std::string& hash,
                                      Persistence::WorldStreamingStateSubHashes& sub) {
        if (capture_refusal_stage == 2)
            Corrupt();
        ServerWorldRunner::ComputeWorldHashAndSubHashes(hash, sub);
    }

private:
    int world_captures = 0;
    std::filesystem::path corruption;
    void Corrupt() {
        corruption =
            Persistence::WorldSaveService::region_file_path(Session()->GetWorldSaveDir(), 40, 40);
        std::filesystem::create_directories(corruption.parent_path());
        std::ofstream output(corruption, std::ios::binary | std::ios::trunc);
        output << "invalid capture authority";
        output.close();
        if (!output.good())
            throw std::runtime_error("Cannot create capture refusal fixture");
    }
};
} // namespace Luminumbra::Server
namespace AuthorityCaptureRefusalModes {
#define ServerWorldRunner CaptureRefusalRunner
#include "../../src/luminumbra_server/modes/Lockstep.cpp"
#include "../../src/luminumbra_server/modes/Replay.cpp"
#undef ServerWorldRunner
} // namespace AuthorityCaptureRefusalModes

namespace {
namespace W = Luminumbra::world;
namespace P = Luminumbra::Persistence;
namespace S = Luminumbra::Server;
namespace C = Luminumbra::Components;
namespace N = Luminumbra::Net;
namespace R = Luminumbra::Replay;
namespace fs = std::filesystem;
std::string Path(const fs::path& value) {
    return Luminumbra::Filesystem::GenericPathString(value);
}
std::string Read(const fs::path& file) {
    std::ifstream input(file, std::ios::binary);
    if (!input)
        throw std::runtime_error("Cannot read integration fixture");
    return {std::istreambuf_iterator<char>(input), {}};
}
void Write(const fs::path& file, std::string_view bytes) {
    fs::create_directories(file.parent_path());
    std::ofstream output(file, std::ios::binary | std::ios::trunc);
    output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    output.close();
    if (!output.good())
        throw std::runtime_error("Cannot write integration fixture");
}
class AuthorityHashIntegration : public testing::Test {
protected:
    fs::path root;
    void SetUp() override {
        root = Luminumbra::TestSupport::CreateUniqueTempDirectory("authority_integration_");
        Luminumbra::TestSupport::WriteSimulationContentFixture(root);
        for (const auto* species : {"grovestrider", "ridgeback_stalker"})
            Write(root / "data/common/creatures/species" / (std::string(species) + ".json"),
                  "{\"id\":\"" + std::string(species) + "\"}");
        Write(root / "data/common/systems.json",
              R"({"sim":{"active_regions":{"enabled":true},"aether_state":{"enabled":true}}})");
        Write(root / "worlds/atlas/presets/default.json", R"({
            "schema_rev":6,
            "generation_params":{
                "terrain":{"base_frequency":0.01,"base_amplitude":0.0,"octaves":1,
                    "persistence":0.5,"lacunarity":2.0,"height_offset":8.0,
                    "shaping":{"peaks_amplitude":0.0,"domain_warp_amplitude":0.0,
                        "continental_spline":[[-1.0,0.0],[1.0,0.0]]},
                    "hydro":{"enabled":false}},
                "features":{"caves_enabled":false,"cave_frequency":0.02}}})");
    }
    void TearDown() override {
        std::error_code error;
        fs::remove_all(root, error);
    }
    ServerCliOptions Options() const {
        ServerCliOptions options;
        options.root = Path(root) + "/";
        options.seed = "1337";
        options.ticks = 30;
        options.surface_radius = 1;
        options.collision_radius = 1;
        options.test_streaming_radius_cap = 1;
        options.record_path = Path(root / "record.lrec1");
        options.artifact_path = Path(root / "artifact.json");
        options.lockstep_dump_path = Path(root / "desync.lrec1");
        return options;
    }
    void Configure(W::GameSession& session, Luminumbra::JobSystem& jobs) const {
        session.SetRootPath(Path(root) + "/");
        session.SetJobSystem(&jobs);
        session.SetActiveRegionsEnabled(true);
        session.SetAetherStateEnabled(true);
    }
};
void Same(const P::WorldStreamingStateSubHashes& a, const P::WorldStreamingStateSubHashes& b) {
    EXPECT_EQ(a.active_regions, b.active_regions);
    EXPECT_EQ(a.terrain, b.terrain);
    EXPECT_EQ(a.water, b.water);
    EXPECT_EQ(a.entities, b.entities);
    EXPECT_EQ(a.wind, b.wind);
    EXPECT_EQ(a.weather, b.weather);
    EXPECT_EQ(a.aether, b.aether);
    EXPECT_EQ(a.aether_state, b.aether_state);
    EXPECT_EQ(a.regions, b.regions);
    EXPECT_EQ(a.creatures, b.creatures);
    EXPECT_EQ(a.items, b.items);
    EXPECT_EQ(a.pages, b.pages);
    EXPECT_EQ(a.scents, b.scents);
    EXPECT_EQ(a.ecology, b.ecology);
    EXPECT_EQ(a.plants, b.plants);
}

TEST_F(AuthorityHashIntegration, AllHashEntryPointsUseSameAuthorityCapture) {
    auto config = RunnerConfigFrom(Options());
    config.avatar_count = 1;
    config.ecology_roster = true;
    config.planted_roster = true;
    S::ServerWorldRunner runner(config);
    ASSERT_TRUE(runner.Boot());
    ASSERT_EQ(runner.RunFixedTicks(3).ticks_executed, 3u);
    auto* session = runner.Session();
    ASSERT_TRUE(session);
    const auto avatars = Luminumbra::Ecs::SerializeEntityRegistrySnapshotJson(
        Luminumbra::World::BuildAvatarEntitySnapshot(runner.Avatars()));
    const auto capture = session->CaptureAuthorityState(avatars);
    ASSERT_TRUE(capture.state) << capture.error;
    std::string world;
    P::WorldStreamingStateSubHashes sub;
    runner.ComputeWorldHashAndSubHashes(world, sub);
    ASSERT_FALSE(world.empty());
    ASSERT_GT(runner.CreatureCount(), 0u);
    ASSERT_FALSE(sub.plants.empty());
    EXPECT_EQ(runner.ComputeWorldHash(), world);
    Same(runner.ComputeWorldSubHashes(), sub);
    std::string common;
    P::WorldStreamingStateSubHashes common_sub;
    ASSERT_TRUE(session->ComputeAuthorityWorldHashes(common, common_sub, avatars));
    EXPECT_EQ(common, world);
    Same(common_sub, sub);
    std::string persistence_hash;
    P::WorldStreamingStateSubHashes persistence_sub;
    P::ComputeAuthorityWorldHashes(
        *capture.state, session->GetMetadata().spawnPoint, persistence_hash, persistence_sub);
    EXPECT_EQ(persistence_hash, world);
    Same(persistence_sub, sub);
    std::string without_avatars;
    P::WorldStreamingStateSubHashes without_avatar_sub;
    ASSERT_TRUE(session->ComputeAuthorityWorldHashes(without_avatars, without_avatar_sub));
    EXPECT_NE(without_avatars, world);
    EXPECT_NE(without_avatar_sub.entities, sub.entities);
    EXPECT_EQ(sub.terrain, capture.state->terrain_hash());
    EXPECT_EQ(sub.water, capture.state->water_hash());
    EXPECT_EQ(sub.entities, P::StableChecksum(avatars));
    EXPECT_EQ(sub.ecology,
              capture.state->FoldEcology(
                  luminumbra::ai::ComputeEcologySubHash(session->GetRegistry(), true)));
    const auto checkpoint = AuthorityIntegrationModes::CaptureCheckpoint(3, runner);
    ASSERT_TRUE(checkpoint);
    EXPECT_EQ(checkpoint->world_hash, world);
    AuthorityIntegrationModes::LockstepPeerContext context{&runner, 0, 0};
    N::HashMsg network;
    AuthorityIntegrationModes::LockstepCaptureHashes(3, network, &context);
    EXPECT_EQ(network.world_hash, world);
    EXPECT_EQ(network.terrain, checkpoint->terrain);
    EXPECT_EQ(network.water, checkpoint->water);
    EXPECT_EQ(network.entities, checkpoint->entities);

    // Failure after a completed tick must clear reused outputs, latch and publish no checkpoint.
    const auto corrupt = P::WorldSaveService::region_file_path(session->GetWorldSaveDir(), 40, 40);
    Write(corrupt, "invalid authority");
    runner.ComputeWorldHashAndSubHashes(world, sub);
    EXPECT_TRUE(world.empty());
    EXPECT_TRUE(sub.regions.empty());
    EXPECT_TRUE(sub.terrain.empty());
    ASSERT_TRUE(session->GetRegionSchedulerFault());
    EXPECT_FALSE(AuthorityIntegrationModes::CaptureCheckpoint(3, runner));
    EXPECT_THROW(AuthorityIntegrationModes::LockstepCaptureHashes(3, network, &context),
                 std::runtime_error);
    EXPECT_THROW(AuthorityIntegrationModes::LockstepCaptureHashes(3, network, nullptr),
                 std::runtime_error);
    EXPECT_TRUE(network.world_hash.empty());
    EXPECT_EQ(runner.RunFixedTicks(1).ticks_executed, 0u);
    auto [transport, remote] = N::MakeLoopbackPair();
    N::LockstepConfig lockstep_config;
    lockstep_config.hash_cadence_ticks = 1;
    N::LockstepHooks hooks;
    hooks.apply_and_step = [](std::uint64_t, const std::vector<std::uint8_t>&, void*) {
        return true;
    };
    hooks.capture_hashes = [](std::uint64_t, N::HashMsg&, void*) {
        throw std::runtime_error("capture refusal");
    };
    N::LockstepSession lockstep(lockstep_config, transport.get(), hooks);
    N::HelloMsg hello;
    hello.seed = lockstep_config.seed;
    hello.preset = lockstep_config.preset;
    hello.tick_rate_hz = lockstep_config.tick_rate_hz;
    hello.client_id = lockstep_config.peer_client_id;
    remote->SendFrame(N::EncodeHello(hello));
    ASSERT_TRUE(lockstep.Handshake());
    remote->SendFrame(N::EncodeInput({1, lockstep_config.peer_client_id, {}}));
    const auto outcome = lockstep.PumpTick(1);
    EXPECT_EQ(outcome.outcome, N::TickOutcome::PeerDisconnected);
    EXPECT_FALSE(outcome.ran_hash_exchange);
    EXPECT_EQ(lockstep.PumpTick(2).outcome, N::TickOutcome::PeerDisconnected);
    bool received_bye = false;
    std::vector<std::uint8_t> frame;
    while (remote->TryReceiveFrame(frame)) {
        N::MessageType type{};
        ASSERT_TRUE(N::PeekMessageType(frame, type));
        EXPECT_NE(type, N::MessageType::Hash);
        if (type == N::MessageType::Bye) {
            N::ByeMsg bye;
            ASSERT_TRUE(N::DecodeBye(frame, bye));
            EXPECT_EQ(bye.tick, 1u);
            received_bye = true;
        }
    }
    EXPECT_TRUE(received_bye);
    EXPECT_FALSE(remote->IsPeerConnected());
    ASSERT_TRUE(fs::remove(corrupt));
    // Initial, checkpoint and final capture refusals preserve existing successful output.
    struct ResetRefusal {
        ~ResetRefusal() {
            S::capture_refusal_stage = 0;
        }
    } reset_refusal;
    for (const int stage : {1, 2, 3}) {
        SCOPED_TRACE(stage);
        auto options = Options();
        options.ticks = stage == 2 ? 30 : 1;
        Write(options.record_path, "existing recording");
        Write(options.record_path + ".regions.json", "existing sidecar");
        S::capture_refusal_stage = stage;
        EXPECT_EQ(AuthorityCaptureRefusalModes::RunRecord(options), 1);
        EXPECT_EQ(Read(options.record_path), "existing recording");
        EXPECT_EQ(Read(options.record_path + ".regions.json"), "existing sidecar");
    }
    auto final_options = Options();
    final_options.ticks = 1;
    Write(final_options.artifact_path, "existing lockstep artifact");
    S::capture_refusal_stage = 1;
    EXPECT_EQ(AuthorityCaptureRefusalModes::RunLockstepLoopback(final_options), 1);
    EXPECT_EQ(Read(final_options.artifact_path), "existing lockstep artifact");
}

TEST_F(AuthorityHashIntegration, CaptureRefusalDisconnectReachesPeer) {
    auto [transport, remote] = N::MakeLoopbackPair();
    N::LockstepConfig host_config;
    host_config.hash_cadence_ticks = 1;
    N::LockstepConfig peer_config = host_config;
    peer_config.local_client_id = host_config.peer_client_id;
    peer_config.peer_client_id = host_config.local_client_id;
    N::LockstepHooks hooks;
    hooks.capture_hashes = [](std::uint64_t, N::HashMsg&, void*) {
        throw std::runtime_error("capture refusal");
    };
    N::LockstepSession host(host_config, transport.get(), hooks);
    N::LockstepSession peer(peer_config, remote.get(), {});
    N::HelloMsg hello;
    hello.seed = peer_config.seed;
    hello.preset = peer_config.preset;
    hello.tick_rate_hz = peer_config.tick_rate_hz;
    hello.client_id = peer_config.local_client_id;
    ASSERT_TRUE(remote->SendFrame(N::EncodeHello(hello)));
    ASSERT_TRUE(host.Handshake());
    ASSERT_TRUE(peer.Handshake());
    const auto refused = host.PumpTick(2);
    ASSERT_EQ(refused.outcome, N::TickOutcome::PeerDisconnected);
    EXPECT_FALSE(refused.ran_hash_exchange);
    host.Disconnect(); // Repeated shutdown must remain harmless.
    const auto observed = peer.PumpTick(2);
    EXPECT_EQ(observed.outcome, N::TickOutcome::PeerDisconnected);
    EXPECT_EQ(observed.tick, 1u);
    EXPECT_FALSE(remote->IsPeerConnected());
    EXPECT_TRUE(peer.Status().peer_disconnected);
    EXPECT_FALSE(peer.Status().desynced);
    EXPECT_EQ(peer.Status().agreed_tick, 0u);
}

TEST_F(AuthorityHashIntegration, NetworkCheckpointPeersShareEffectiveConfiguration) {
    using Config = luminumbra::core::SystemConfig;
    const auto configuration = std::make_shared<const Config>(Config::FromJsonString(R"({
        "sim": {
            "active_regions":{"enabled":true}, "aether_state":{"enabled":true},
            "water_high_res":{"enabled":true}, "hydrology_weather":{"enabled":true},
            "weather_events":{"enabled":true},
            "ecology":{"enabled":true,"params":{"energy_drain_per_second":0.25}},
            "thirst":{"enabled":true,"params":{"rise_rate_per_second":0.125}},
            "wildlife_foliage":{"enabled":true,"params":{"graze_radius":1.25}},
            "scavenging":{"enabled":true,"params":{"feed_radius":2.25}},
            "foraging":{"enabled":true,"params":{"deposit":1.25}},
            "reproduction":{"enabled":true,"params":{"maturity_ticks":42}},
            "plant_growth":{"enabled":true,"params":{"mutation_rate":0.0625}}
        }})"));
    // Every override must be parsed, including tunings without a session getter.
    using Param = luminumbra::core::SysParam;
    EXPECT_FLOAT_EQ(configuration->param(Param::EcoEnergyDrain, -1.0f), 0.25f);
    EXPECT_FLOAT_EQ(configuration->param(Param::ThirstRiseRate, -1.0f), 0.125f);
    EXPECT_FLOAT_EQ(configuration->param(Param::WfGrazeRadius, -1.0f), 1.25f);
    EXPECT_FLOAT_EQ(configuration->param(Param::ScavFeedRadius, -1.0f), 2.25f);
    EXPECT_FLOAT_EQ(configuration->param(Param::ForagingDeposit, -1.0f), 1.25f);
    EXPECT_FLOAT_EQ(configuration->param(Param::ReproMaturityTicks, -1.0f), 42.0f);
    EXPECT_FLOAT_EQ(configuration->param(Param::PlantMutationRate, -1.0f), 0.0625f);
    Luminumbra::JobSystem jobs;
    jobs.startup(1);
    W::GameSession client;
    client.SetRootPath(Path(root) + "/");
    client.SetJobSystem(&jobs);
    client.ConfigureSimulation(configuration);
    ASSERT_TRUE(client.CreateWorld("Network client", "1337", "default"));
    ASSERT_TRUE(client.LoadWorldState());
    ASSERT_TRUE(client.GetWorldSystem());
    ASSERT_TRUE(client.GetPhysicsSystem());
    ASSERT_TRUE(client.GetWorldSystem()->EnsureSurfaceReadyNear(
        client.GetMetadata().spawnPoint, client.GetPhysicsSystem(), 1, 1));
    const W::GameSession::NetworkHostConfig host_config{Path(root) + "/", "1337", "default", 1, 1};
    std::string failure_reason = "previous failure";
    auto host = W::GameSession::CreateNetworkHost(client, jobs, host_config, failure_reason);
    ASSERT_TRUE(host) << failure_reason;
    EXPECT_TRUE(failure_reason.empty());
    ASSERT_EQ(client.GetSimulationConfiguration(), configuration);
    // Removing the factory's configuration forwarding fails this assertion and the enabled
    // checkpoint capture below. Begin uses this same complete host-construction function.
    ASSERT_EQ(host->GetSimulationConfiguration(), client.GetSimulationConfiguration());
    ASSERT_TRUE(host->ActiveRegionsEnabled());
    ASSERT_TRUE(client.ActiveRegionsEnabled());
    ASSERT_TRUE(host->GetEnergyFieldState());
    ASSERT_TRUE(client.GetEnergyFieldState());
    EXPECT_FLOAT_EQ(host->GetEcologyTuning().energy_drain_per_second, 0.25f);
    EXPECT_THROW(host->ConfigureSimulationFromPeer(client), std::logic_error);
    EXPECT_EQ(host->GetSimulationConfiguration(), configuration);
    EXPECT_THROW(host->ConfigureSimulation(nullptr), std::logic_error);
    EXPECT_THROW(client.ConfigureSimulation(std::make_shared<const Config>(Config::Defaults())),
                 std::logic_error);
    for (std::uint64_t tick = 0; tick <= 3; ++tick) {
        SCOPED_TRACE(tick);
        if (tick != 0) {
            for (auto* session : {host.get(), &client}) {
                session->SetReplicatedSimulationAnchors({});
                ASSERT_EQ(session->TickSimulation(1.0 / 30.0), 1u);
                ASSERT_FALSE(session->GetRegionSchedulerFault());
            }
        }
        ASSERT_EQ(host->GetSimulationTickCount(), tick);
        ASSERT_EQ(client.GetSimulationTickCount(), tick);
        std::string host_hash, client_hash;
        P::WorldStreamingStateSubHashes host_sub, client_sub;
        // Use the shipping checkpoint recipe without replacement lockstep hooks.
        ASSERT_TRUE(host->ComputeNetworkWorldHashes(*configuration, host_hash, host_sub));
        ASSERT_TRUE(client.ComputeNetworkWorldHashes(*configuration, client_hash, client_sub));
        ASSERT_FALSE(host_hash.empty());
        ASSERT_TRUE(host_sub.active_regions);
        EXPECT_EQ(host_hash, client_hash);
        Same(host_sub, client_sub);
        std::string authority_hash;
        P::WorldStreamingStateSubHashes authority_sub;
        ASSERT_TRUE(host->ComputeAuthorityWorldHashes(authority_hash, authority_sub));
        EXPECT_EQ(host_hash, authority_hash);
        Same(host_sub, authority_sub);
        // A contradictory shared recipe refuses rather than silently selecting local state.
        EXPECT_FALSE(host->ComputeNetworkWorldHashes(Config::Defaults(), host_hash, host_sub));
        EXPECT_TRUE(host_hash.empty());
        EXPECT_TRUE(host_sub.regions.empty());
        EXPECT_TRUE(host_sub.terrain.empty());
        ASSERT_TRUE(host->ComputeNetworkWorldHashes(*configuration, host_hash, host_sub));
        EXPECT_EQ(host_hash, client_hash);
        EXPECT_FALSE(host->GetRegionSchedulerFault());
    }
}

TEST_F(AuthorityHashIntegration, NetworkPeerConfigurationRefusesMissingSnapshot) {
    W::GameSession host, unconfigured_peer;
    using Config = luminumbra::core::SystemConfig;
    const auto configuration = std::make_shared<const Config>(
        Config::FromJsonString(R"({"sim":{"active_regions":{"enabled":true}}})"));
    host.ConfigureSimulation(configuration);
    EXPECT_THROW(host.ConfigureSimulationFromPeer(unconfigured_peer), std::logic_error);
    EXPECT_EQ(host.GetSimulationConfiguration(), configuration);
    EXPECT_TRUE(host.ActiveRegionsEnabled());
    EXPECT_FALSE(unconfigured_peer.GetSimulationConfiguration());
    Luminumbra::JobSystem jobs;
    jobs.startup(1);
    const W::GameSession::NetworkHostConfig host_config{Path(root) + "/", "1337", "default", 1, 1};
    std::string failure_reason;
    EXPECT_FALSE(
        W::GameSession::CreateNetworkHost(unconfigured_peer, jobs, host_config, failure_reason));
    EXPECT_EQ(failure_reason, "simulation_configuration_missing");
    EXPECT_FALSE(unconfigured_peer.GetSimulationConfiguration());
    EXPECT_FALSE(unconfigured_peer.GetWorldSystem());
    EXPECT_EQ(host.GetSimulationConfiguration(), configuration);
    EXPECT_FALSE(fs::exists(root / "worlds/saves"));
}

TEST_F(AuthorityHashIntegration, NetworkHostCreationFailurePreservesPeer) {
    using Config = luminumbra::core::SystemConfig;
    const auto configuration = std::make_shared<const Config>(
        Config::FromJsonString(R"({"sim":{"active_regions":{"enabled":true}}})"));
    Luminumbra::JobSystem jobs;
    jobs.startup(1);
    W::GameSession client;
    client.SetRootPath(Path(root) + "/");
    client.SetJobSystem(&jobs);
    client.ConfigureSimulation(configuration);
    ASSERT_TRUE(client.CreateWorld("Existing peer", "1337", "default"));
    std::string before;
    P::WorldStreamingStateSubHashes before_sub;
    ASSERT_TRUE(client.ComputeNetworkWorldHashes(*configuration, before, before_sub));
    const auto save_dir = client.GetWorldSaveDir();
    const W::GameSession::NetworkHostConfig invalid_config{
        Path(root) + "/", "1337", "missing-network-preset", 1, 1};
    std::string failure_reason;
    EXPECT_FALSE(W::GameSession::CreateNetworkHost(client, jobs, invalid_config, failure_reason));
    EXPECT_EQ(failure_reason, "host_create_world_failed");
    EXPECT_EQ(client.GetSimulationConfiguration(), configuration);
    EXPECT_EQ(client.GetWorldSaveDir(), save_dir);
    EXPECT_FALSE(client.GetRegionSchedulerFault());
    std::string after;
    P::WorldStreamingStateSubHashes after_sub;
    ASSERT_TRUE(client.ComputeNetworkWorldHashes(*configuration, after, after_sub));
    EXPECT_EQ(after, before);
    Same(after_sub, before_sub);
}

TEST_F(AuthorityHashIntegration, NamedHashDomainMutationMatrix) {
    Luminumbra::JobSystem jobs;
    jobs.startup(1);
    W::GameSession session;
    Configure(session, jobs);
    ASSERT_TRUE(session.CreateWorld("Named domains", "1337", "default"));
    const auto creature = session.GetRegistry().create();
    session.GetRegistry().emplace<C::TransformComponent>(creature).position = {8, 100, 8};
    session.GetRegistry().emplace<C::CreatureComponent>(creature).species_id =
        C::CreatureSpeciesId16("fixture_creature");
    ASSERT_TRUE(session.AdmitCreature(creature));
    const auto item = session.SpawnGroundObject(1, {8, 100, 8}, 0, 7);
    ASSERT_TRUE(item);
    using Member = std::string P::WorldStreamingStateSubHashes::*;
    const std::array<Member, 4> domains{&P::WorldStreamingStateSubHashes::regions,
                                        &P::WorldStreamingStateSubHashes::creatures,
                                        &P::WorldStreamingStateSubHashes::items,
                                        &P::WorldStreamingStateSubHashes::pages};
    const auto check = [&](std::size_t changed, const std::function<void()>& mutate) {
        std::string before, after;
        P::WorldStreamingStateSubHashes first, second;
        ASSERT_TRUE(session.ComputeAuthorityWorldHashes(before, first));
        mutate();
        ASSERT_TRUE(session.ComputeAuthorityWorldHashes(after, second));
        EXPECT_NE(before, after);
        for (std::size_t i = 0; i < domains.size(); ++i) {
            SCOPED_TRACE(i);
            if (i == changed)
                EXPECT_NE(first.*domains[i], second.*domains[i]);
            else
                EXPECT_EQ(first.*domains[i], second.*domains[i]);
        }
    };
    check(0, [&] { session.GetSimulationEventBus().publish(100, "future", "payload", -2); });
    check(1, [&] { session.GetRegistry().get<C::CreatureComponent>(creature).hunger = 0.75f; });
    check(2, [&] {
        auto& objects = const_cast<W::GroundObjectStore&>(session.GetGroundObjects());
        auto changed = objects.objects().at(item);
        ASSERT_TRUE(objects.erase(item));
        changed.payload = 8;
        ASSERT_TRUE(objects.insert(changed));
    });
    check(3, [&] { ASSERT_TRUE(session.SetFoliageCell({8, 100, 8}, true, 123)); });
    ASSERT_TRUE(session.GetEnergyFieldState());
    check(3, [&] {
        ASSERT_TRUE(session.GetEnergyFieldState()->QueueRegionalDeposit(9, 0, 0, 0, 10));
    });
    // Diagnostic reads preserve complete immutable authority.
    const auto first = session.CaptureAuthorityState();
    ASSERT_TRUE(first.state);
    const auto empty_avatars =
        session.CaptureAuthorityState(Luminumbra::Ecs::SerializeEntityRegistrySnapshotJson(
            Luminumbra::Ecs::EntityRegistrySnapshot{}));
    ASSERT_TRUE(empty_avatars.state);
    EXPECT_EQ(empty_avatars.state->chunk_hash(), first.state->chunk_hash());
    EXPECT_EQ(empty_avatars.state->sub_hashes().entities, first.state->sub_hashes().entities);
    std::string hash;
    P::WorldStreamingStateSubHashes sub;
    ASSERT_TRUE(session.ComputeAuthorityWorldHashes(hash, sub));
    const auto second = session.CaptureAuthorityState();
    ASSERT_TRUE(second.state);
    EXPECT_EQ(first.state->authority_bytes(), second.state->authority_bytes());
    ASSERT_TRUE(session.SaveWorldState());
    W::GameSession restored;
    Configure(restored, jobs);
    ASSERT_TRUE(restored.CreateWorld("Restored named domains", "1337", "default"));
    ASSERT_TRUE(restored.LoadWorldStateFrom(session.GetWorldSaveDir()));
    std::string restored_hash;
    P::WorldStreamingStateSubHashes restored_sub;
    ASSERT_TRUE(restored.ComputeAuthorityWorldHashes(restored_hash, restored_sub));
    EXPECT_EQ(restored_hash, hash);
    Same(restored_sub, sub);
}

TEST_F(AuthorityHashIntegration, RecordReplayAndLockstepEnabledHashIdentity) {
    auto options = Options();
    options.avatars = 1;
    options.ecology_roster = true;
    options.planted_roster = true;
    ASSERT_EQ(AuthorityIntegrationModes::RunRecord(options), 0);
    const auto contents = R::ReadReplay(options.record_path);
    ASSERT_TRUE(contents);
    ASSERT_EQ(contents->checkpoints.size(), 1u);
    options.replay_path = options.record_path;
    ASSERT_EQ(AuthorityIntegrationModes::RunReplay(options), 0);
    const auto replay = nlohmann::json::parse(Read(options.artifact_path));
    EXPECT_TRUE(replay.at("passed").get<bool>());
    EXPECT_EQ(replay.at("end_world_hash"), contents->checkpoints.front().world_hash);
    ASSERT_EQ(AuthorityIntegrationModes::RunLockstepLoopback(options), 0);
    const auto lockstep = nlohmann::json::parse(Read(options.artifact_path));
    EXPECT_TRUE(lockstep.at("passed").get<bool>());
    EXPECT_EQ(lockstep.at("host").at("world_hash"), contents->checkpoints.front().world_hash);
    EXPECT_EQ(lockstep.at("peer").at("world_hash"), contents->checkpoints.front().world_hash);
}

TEST_F(AuthorityHashIntegration, FrameGroupingPreservesCheckpointHashes) {
    std::vector<std::string> reference;
    for (const std::uint32_t group : {1u, 2u, 3u}) {
        Luminumbra::JobSystem jobs;
        jobs.startup(1);
        W::GameSession session;
        Configure(session, jobs);
        ASSERT_TRUE(session.CreateWorld("Grouped frames", "1337", "default"));
        session.SetReplicatedSimulationAnchors({});
        ASSERT_TRUE(session.SpawnGroundObject(1, {8, 100, 8}, 0, 7));
        session.GetSimulationEventBus().publish(100, "future", "continuation");
        R::ReplayWriter writer;
        const auto path = Path(root / ("group-" + std::to_string(group) + ".lrec1"));
        ASSERT_TRUE(writer.Open(path, R::ReplayHeader{}));
        for (std::uint64_t tick = 0; tick < 30; tick += group) {
            ASSERT_EQ(session.TickSimulation(static_cast<double>(group) / 30.0 + 1e-12), group);
            if ((session.GetSimulationTickCount() % 6) != 0)
                continue;
            R::CheckpointRecord checkpoint;
            checkpoint.tick = session.GetSimulationTickCount();
            P::WorldStreamingStateSubHashes sub;
            ASSERT_TRUE(session.ComputeAuthorityWorldHashes(checkpoint.world_hash, sub));
            checkpoint.terrain = sub.terrain;
            checkpoint.water = sub.water;
            checkpoint.entities = sub.entities;
            writer.RecordCheckpoint(checkpoint);
        }
        ASSERT_TRUE(writer.Finalize(30));
        const auto decoded = R::ReadReplay(path);
        ASSERT_TRUE(decoded);
        ASSERT_EQ(decoded->checkpoints.size(), 5u);
        std::vector<std::string> hashes;
        for (const auto& checkpoint : decoded->checkpoints)
            hashes.push_back(checkpoint.world_hash + checkpoint.terrain + checkpoint.water +
                             checkpoint.entities);
        if (group == 1)
            reference = hashes;
        else
            EXPECT_EQ(hashes, reference);
    }
}

// Independent byte construction pins the unchanged little-endian wire contracts.
void Integer(std::string& bytes, std::uint64_t value, unsigned width) {
    for (unsigned i = 0; i < width; ++i)
        bytes += static_cast<char>((value >> (8 * i)) & 255);
}
void Text(std::string& bytes, const std::string& text) {
    Integer(bytes, text.size(), 4);
    bytes += text;
}
TEST_F(AuthorityHashIntegration, HashMsgAndLREC1EncodingUnchanged) {
    const N::HashMsg message{7, "world", "terrain", "water", "entities"};
    std::string payload;
    Integer(payload, 7, 8);
    for (const auto* text : {"world", "terrain", "water", "entities"})
        Text(payload, text);
    std::string expected;
    Integer(expected, 3, 1);
    Integer(expected, payload.size(), 4);
    expected += payload;
    const auto encoded = N::EncodeHash(message);
    EXPECT_EQ(std::string(encoded.begin(), encoded.end()), expected);
    N::HashMsg decoded;
    ASSERT_TRUE(N::DecodeHash(encoded, decoded));
    EXPECT_EQ(decoded.entities, message.entities);
    for (std::size_t length = 0; length < encoded.size(); ++length) {
        const std::vector<std::uint8_t> truncated(
            encoded.begin(), encoded.begin() + static_cast<std::ptrdiff_t>(length));
        EXPECT_FALSE(N::DecodeHash(truncated, decoded));
    }
    R::ReplayHeader header;
    header.seed = 11;
    header.preset_hash = 13;
    header.seed_string = "11";
    header.preset = "default";
    header.start_world_hash = "start";
    header.engine_version = "fixture";
    header.surface_radius = 1;
    header.collision_radius = 1;
    R::ReplayWriter writer;
    const auto path = Path(root / "wire.lrec1");
    ASSERT_TRUE(writer.Open(path, header));
    writer.RecordCheckpoint(
        {7, message.world_hash, message.terrain, message.water, message.entities});
    ASSERT_TRUE(writer.Finalize(7));
    expected = "LREC1";
    Integer(expected, 0, 1);
    Integer(expected, 1, 2);
    Integer(expected, 30, 2);
    Integer(expected, 7, 8);
    Integer(expected, 11, 8);
    Integer(expected, 13, 8);
    Integer(expected, 1, 4);
    Integer(expected, 1, 4);
    for (const auto* text : {"11", "default", "start", "fixture"})
        Text(expected, text);
    payload.clear();
    for (const auto* text : {"world", "terrain", "water", "entities"})
        Text(payload, text);
    Integer(expected, 2, 1);
    Integer(expected, 7, 8);
    Integer(expected, payload.size(), 4);
    expected += payload;
    expected += "LRECEND1";
    Integer(expected, 7, 8);
    Integer(expected, 0, 8);
    Integer(expected, 1, 8);
    EXPECT_EQ(Read(path), expected);
}

TEST_F(AuthorityHashIntegration, FlagOffProductionArtifactBytesUnchanged) {
    for (int scenario = 0; scenario < 3; ++scenario) {
        ServerCliOptions options;
        S::SmokeRunResult first, replay;
        FixedSmokeInputs(scenario, options, first, replay);
        options.artifact_path = Path(root / "smoke.json");
        // Stale in-memory diagnostics must not leak into flag-off production bytes.
        first.sub_hashes.regions = replay.sub_hashes.regions = "stale";
        first.sub_hashes.creatures = replay.sub_hashes.creatures = "stale";
        first.sub_hashes.items = replay.sub_hashes.items = "stale";
        first.sub_hashes.pages = replay.sub_hashes.pages = "stale";
        ASSERT_EQ(S::WriteSmokeArtifact(options, first, replay), scenario == 2 ? 1 : 0);
        auto expected = Read(fs::path(LUMINUMBRA_SOURCE_ROOT) / "test/fixtures/smoke_artifact" /
                             ("devel-" + std::to_string(scenario) + ".json"));
#ifdef _WIN32
        std::string native;
        for (const char character : expected) {
            if (character == '\n')
                native += '\r';
            native += character;
        }
        expected = std::move(native);
#endif
        EXPECT_EQ(Read(options.artifact_path), expected);
    }
    ServerCliOptions options;
    S::SmokeRunResult first, replay;
    FixedSmokeInputs(0, options, first, replay);
    options.artifact_path = Path(root / "enabled-smoke.json");
    first.sub_hashes.active_regions = replay.sub_hashes.active_regions = true;
    first.sub_hashes.regions = "regions";
    first.sub_hashes.creatures = "creatures";
    first.sub_hashes.items = "items";
    first.sub_hashes.pages = "pages";
    replay.sub_hashes = first.sub_hashes;
    ASSERT_EQ(S::WriteSmokeArtifact(options, first, replay), 0);
    const auto artifact = nlohmann::json::parse(Read(options.artifact_path));
    for (const auto* key : {"regions", "creatures", "items", "pages"}) {
        EXPECT_EQ(artifact.at("sub_hashes").at(key), key);
        EXPECT_EQ(artifact.at("runs").at(0).at("sub_hashes").at(key), key);
        EXPECT_EQ(artifact.at("sub_hashes_replay").at(key), key);
    }
    for (auto member : {&P::WorldStreamingStateSubHashes::regions,
                        &P::WorldStreamingStateSubHashes::creatures,
                        &P::WorldStreamingStateSubHashes::items,
                        &P::WorldStreamingStateSubHashes::pages}) {
        const auto original = replay.sub_hashes.*member;
        replay.sub_hashes.*member = "diverged";
        EXPECT_EQ(S::WriteSmokeArtifact(options, first, replay), 1);
        (replay.sub_hashes.*member).clear();
        EXPECT_EQ(S::WriteSmokeArtifact(options, first, replay), 1);
        replay.sub_hashes.*member = original;
    }
    replay.sub_hashes.active_regions = false;
    EXPECT_EQ(S::WriteSmokeArtifact(options, first, replay), 1);
}
} // namespace
