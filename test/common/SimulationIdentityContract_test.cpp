// Independent planned-contract oracle. This TU deliberately uses no production
// codec/projection field list: later mutation tests must cover this inventory.
#include <gtest/gtest.h>

#include "luminumbra_common/core/SystemConfig.h"
#include "luminumbra_common/net/LockstepSession.h"
#include "luminumbra_common/net/ReplicationAdmission.h"
#include "luminumbra_common/world/GameSession.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <string>
#include <string_view>
#include <vector>

namespace {

TEST(SimulationIdentityContract, AmbientActivationRulesChangeEnabledAdmission) {
    namespace net = Luminumbra::Net;
    using Values = luminumbra::core::SimulationIdentityValues;
    const Luminumbra::world::GameSession current;
    const auto identity = current.SystemConfigIdentity();
    ASSERT_EQ(identity, "c1cfd4e42aa58f8d");
    EXPECT_EQ(current.EffectiveIdentityValues().EffectiveAuthorityRulesRevision(),
              Values::kRegionalActivationAuthorityRulesRevision);
    EXPECT_EQ(Values::kRegionalActivationAuthorityRulesRevision, 9u);
    // Enabled recordings from both earlier authority profiles must refuse.
    for (const std::string previous : {"45664f7615d96904", "bf48b15682a969bd"}) {
        ASSERT_NE(identity, previous);
        for (const bool previous_local : {false, true}) {
            net::LockstepConfig config;
            config.system_config_identity = previous_local ? previous : identity;
            config.preset_content_hash = "unchanged-preset-content";
            auto peer_config = config;
            peer_config.local_client_id = config.peer_client_id;
            peer_config.peer_client_id = config.local_client_id;
            peer_config.system_config_identity = previous_local ? identity : previous;
            const auto local_hello = net::MakeLockstepHello(config);
            const auto peer_hello = net::MakeLockstepHello(peer_config);
            EXPECT_TRUE(net::HelloMatches(local_hello, local_hello));
            EXPECT_FALSE(net::HelloMatches(local_hello, peer_hello));

            auto [local, peer] = net::MakeLoopbackPair();
            ASSERT_TRUE(peer->SendFrame(net::EncodeHello(peer_hello)));
            unsigned steps = 0;
            net::LockstepHooks hooks;
            hooks.apply_and_step = [](std::uint64_t, const std::vector<std::uint8_t>&, void* user) {
                ++*static_cast<unsigned*>(user);
                return true;
            };
            hooks.user = &steps;
            net::LockstepSession lockstep(config, local.get(), hooks);
            EXPECT_FALSE(lockstep.Handshake());
            lockstep.PumpTick(1);
            EXPECT_FALSE(lockstep.Status().handshaken);
            EXPECT_EQ(lockstep.AgreedTick(), 0u);
            EXPECT_EQ(steps, 0u);

            // Both remote-replication roles use the same identity before forwarding.
            for (const auto role :
                 {net::ReplicationAdmissionRole::Host, net::ReplicationAdmissionRole::Joiner}) {
                auto mine = local_hello;
                auto theirs = peer_hello;
                mine.client_id = role == net::ReplicationAdmissionRole::Host ? 0u : 1u;
                theirs.client_id = role == net::ReplicationAdmissionRole::Host ? 1u : 0u;
                net::ReplicationAdmissionState admission(role, mine, theirs.client_id);
                admission.Begin(0);
                if (role == net::ReplicationAdmissionRole::Joiner)
                    admission.OnHelloSendResult(true, 0);
                EXPECT_EQ(admission.OnFrame(net::EncodeHello(theirs), net::FrameDelivery::Reliable),
                          net::ReplicationAdmissionAction::Discard);
                EXPECT_TRUE(admission.failed());
                EXPECT_FALSE(admission.admitted());
                EXPECT_EQ(admission.reason(), "SystemConfig identity mismatch");
            }
        }
    }
    EXPECT_EQ(current.GetSimulationTickCount(), 0u);
}

TEST(SimulationIdentityContract, AmbientActivationPreservesApprovedOffIdentity) {
    using luminumbra::core::SystemConfig;
    Luminumbra::world::GameSession session;
    session.SetActiveRegionsEnabled(false);
    EXPECT_EQ(session.EffectiveIdentityValues().EffectiveAuthorityRulesRevision(), 8u);
    EXPECT_EQ(session.SystemConfigIdentity(), "bafe22eabc5fc385");
    EXPECT_EQ(session.LegacyRevision3SystemConfigIdentity(), "0820e02bb23ece12");
    Luminumbra::world::GameSession configured;
    configured.ApplySystemConfig(
        SystemConfig::FromJsonString(R"({"sim":{"active_regions":{"enabled":false}}})"));
    EXPECT_EQ(configured.SystemConfigIdentity(), session.SystemConfigIdentity());
    session.SetActiveRegionsEnabled(true);
    EXPECT_EQ(session.SystemConfigIdentity(), "c1cfd4e42aa58f8d");
    session.SetActiveRegionsEnabled(false);
    EXPECT_EQ(session.SystemConfigIdentity(), "bafe22eabc5fc385");
}

std::string ReadContract(const char* relative_path) {
    std::ifstream input(std::filesystem::path(LUMINUMBRA_SOURCE_ROOT) / relative_path);
    EXPECT_TRUE(input.is_open()) << relative_path;
    // Contract documents are bounded to 128 KiB; reaching the bound is a failure.
    std::string contents(128U * 1024U, '\0');
    input.read(contents.data(), static_cast<std::streamsize>(contents.size()));
    EXPECT_TRUE(input.eof()) << relative_path;
    EXPECT_FALSE(input.bad()) << relative_path;
    contents.resize(static_cast<std::size_t>(input.gcount()));
    std::erase(contents, '\r');
    return contents;
}

TEST(SimulationIdentityContract, PreparationRemainsPlannedAcrossGuides) {
    for (const char* path : {"docs/distant-world.md",
                             "docs/active-regions.md",
                             "docs/development/system-config-identity.md"}) {
        const auto document = ReadContract(path);
        EXPECT_NE(document.find("PLANNED for v0.3.3 (in preparation), issue #629"),
                  std::string::npos)
            << path;
        EXPECT_NE(document.find("luminumbra.plants.v2"), std::string::npos) << path;
        EXPECT_NE(document.find("kAuthorityRulesRevision"), std::string::npos) << path;
        EXPECT_NE(document.find("issue #137"), std::string::npos) << path;
    }
}

TEST(SimulationIdentityContract, SharedCursorRefusalAndSessionScopeAreExplicit) {
    const auto document = ReadContract("docs/distant-world.md");
    for (const std::string_view requirement :
         {"IDs are in `[1, 2^63)`, next cursor in `[1, 2^63]`",
          "issued IDs are never recycled, including after extinction",
          "render-only dressing consumes no IDs",
          "including an empty roster with the consumed shared cursor",
          "Existing saves with v1, a sidecar, an unknown version or no v2 envelope refuse",
          "no converter exists",
          "any saved ledger allocator TLV must equal the envelope cursor",
          "at most `2^20` plant records and 17 component kinds per record",
          "mixed owned/unowned plants are valid",
          "retaining the greater live/saved cursor",
          "session-only until issue #137",
          "wildlife recreated after reopening is a new incarnation with new IDs",
          "then-current authority revision plus one",
          "TCP on Windows/POSIX, loopback, Steam, GNS and replay"}) {
        EXPECT_NE(document.find(requirement), std::string::npos) << requirement;
    }
}

TEST(SimulationIdentityContract, IndependentPlantFieldInventoryIsComplete) {
    constexpr std::string_view expected = R"(| Component | Fields required by codec and projection |
|---|---|
| `TransformComponent` | `position.x`, `position.y`, `position.z` |
| `PlantGenomeComponent` | `genes[0..7]` in `PlantGene` order |
| `PlantGrowthComponent` | `species_id`, `stage`, `quality`, `growth_points`, `stress_points`, `tended`, `planted_tick`, `last_tick` |
| `SoilFeederComponent` | `uptake`, `absorbed` |
| `PlantHealthComponent` | `infection`, `resistance`, `state`, `infected_ticks` |
| `PollinationComponent` | `pollinated`, `last_pollen_tick`, `crosses`, `next_genome.genes[0..7]` |
| `CropLifecycleComponent` | `perennial`, `ripe_ticks`, `lifespan_ticks`, `generations`, `species_id` |
| `PlantRegionComponent` | `owner_x`, `owner_z`, `region_stamp`; identity moves to the common record field |
| `PollinationTag` | presence only |
| `CombustibleComponent` | `fuel_milli`, `moisture_milli`, `burn_state`, `burn_ticks_remaining`, `ignition_radius` |
| `MortalComponent` | `age_ticks`, `lifespan_ticks`, `dead` |
| `GrazeableComponent` | `biomass`, `last_grazed_tick` |
| `WaterSourceComponent` | `output` |
| `IgnitionSourceComponent` | `radius`, `intensity` |
| `FlowingWaterSourceComponent` | `flow_rate` |
| `DecayComponent` | `decay_ticks`, `decay_duration`, `nutrient_released_milli`, `fully_decomposed` |
| `FieldEmitterComponent` | `channel`, `rate_raw_per_tick`, `radius_cells` |)";
    const auto document = ReadContract("docs/distant-world.md");
    const auto start = document.find("| Component | Fields required by codec and projection |");
    ASSERT_NE(start, std::string::npos);
    const auto end = document.find("\n\n### Planned ecology projection inventory", start);
    ASSERT_NE(end, std::string::npos);
    EXPECT_EQ(std::string_view(document).substr(start, end - start), expected);
}

TEST(SimulationIdentityContract, EcologyProjectionAndPersistenceBoundaryAreExplicit) {
    const auto document = ReadContract("docs/distant-world.md");
    for (const std::string_view requirement :
         {"PLANNED `ecology:v3`",
          "this hash inventory does not implement creature persistence",
          "Creature and genome",
          "Mortality, decay and needs",
          "Social and movement state",
          "Perception and planning",
          "Sources and participation",
          "References and exclusions",
          "distinct null/invalid markers",
          "independent completeness oracle",
          "a separate mutation regression for every field and component presence"}) {
        EXPECT_NE(document.find(requirement), std::string::npos) << requirement;
    }
}

} // namespace
