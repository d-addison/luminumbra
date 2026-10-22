//  SystemConfig substrate — contract tests derived from spec
// Acceptance Criteria ..005.
// These reference luminumbra::core::SystemConfig, which does not exist yet:
// the test keeps the generated schema and runtime registry aligned.
#include "../support/SystemConfigVariants.h"
#include "../support/UniqueTempPath.h"
#include <gtest/gtest.h>
#include <nlohmann/json.hpp>
#include <nlohmann/json_fwd.hpp>

#include <array>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <ios>
#include <iterator>
#include <sstream>
#include <string>
#include <system_error>
#include <utility>

#include <glm/ext/vector_float3.hpp>
#include <iomanip>

#include "luminumbra_client/rendering/FarWorldCaveSelection.h"
#include "luminumbra_client/rendering/FarWorldRenderer.h"
#include "luminumbra_client/rendering/FarWorldSelection.h"
#include "luminumbra_common/core/FilesystemPath.h"
#include "luminumbra_common/core/FramePacing.h"
#include "luminumbra_common/core/JobSystem.h"
#include "luminumbra_common/core/SystemConfig.h"
#include "luminumbra_common/core/SystemConfigRegistry.gen.h"
#include "luminumbra_common/persistence/WorldPersistenceRoundtrip.h" // StableChecksum
#include "luminumbra_common/persistence/WorldSaveService.h"
#include "luminumbra_common/world/FarVolume.h"
#include "luminumbra_common/world/GameSession.h"

using luminumbra::core::SysKey;
using luminumbra::core::SysParam;
using luminumbra::core::SystemConfig;

namespace {

class RuntimeSystemConfig : public testing::Test {
protected:
    const std::filesystem::path& Directory() const {
        return directory;
    }
    const std::filesystem::path& Runtime() const {
        return runtime;
    }
    void SetUp() override {
        previous_cwd = std::filesystem::current_path();
        directory = Luminumbra::TestSupport::CreateUniqueTempDirectory("runtime-config-");
        runtime = directory / std::filesystem::path(u8"runtime-\u4e16\u754c-\u03a9");
        std::filesystem::create_directories(runtime / "data/common");
        const auto unrelated = directory / "launch";
        std::filesystem::create_directories(unrelated / "data/common");
        Write(unrelated / "data/common/systems.json",
              R"({"sim":{"erosion":{"enabled":false}},"user":{"video":{"fov":60}}})");
        std::filesystem::current_path(unrelated);
    }
    void TearDown() override {
        std::error_code error;
        std::filesystem::current_path(previous_cwd, error);
        std::filesystem::remove_all(directory, error);
    }
    static void Write(const std::filesystem::path& path, const std::string& bytes) {
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        output << bytes;
        ASSERT_TRUE(output.good());
    }

private:
    std::filesystem::path directory;
    std::filesystem::path runtime;
    std::filesystem::path previous_cwd;
};

TEST_F(RuntimeSystemConfig, OverlayReplacementFailurePreservesBytesAndRetryOverwrites) {
    const auto path = Directory() / "settings.json";
    auto config = SystemConfig::FromJsonString(R"({"user":{"video":{"fov":80}}})");
    ASSERT_TRUE(config.SaveUserOverlay(path));
    std::string before;
    {
        const std::ifstream input(path, std::ios::binary);
        std::ostringstream bytes;
        bytes << input.rdbuf();
        before = bytes.str();
    }
    config.user().fov = 100.0f;
    Luminumbra::Persistence::WorldSaveService::set_interrupt_before_region_replace_for_testing(
        true);
    const bool saved = config.SaveUserOverlay(path);
    Luminumbra::Persistence::WorldSaveService::set_interrupt_before_region_replace_for_testing(
        false);
    EXPECT_FALSE(saved);
    {
        const std::ifstream input(path, std::ios::binary);
        std::ostringstream bytes;
        bytes << input.rdbuf();
        EXPECT_EQ(bytes.str(), before);
    }
    for (const auto& entry : std::filesystem::directory_iterator(Directory()))
        EXPECT_EQ(
            Luminumbra::Filesystem::GenericUtf8PathString(entry.path().filename()).find(".tmp."),
            std::string::npos);
    ASSERT_TRUE(config.SaveUserOverlay(path));
    EXPECT_FLOAT_EQ(SystemConfig::LoadFromFile(path).user().fov, 100.0f);
    config.user().fov = 110.0f;
    ASSERT_TRUE(config.SaveUserOverlay(path));
    EXPECT_FLOAT_EQ(SystemConfig::LoadFromFile(path).user().fov, 110.0f);
}

TEST_F(RuntimeSystemConfig, DefaultsAndUserOverlayComeFromRuntimeRoot) {
    Write(Runtime() / "data/common/systems.json",
          R"({"sim":{"plant_growth":{"enabled":false}},"user":{"video":{"fov":80}}})");
    const auto defaults = SystemConfig::LoadFromRuntimeRoot(Runtime());
    EXPECT_TRUE(defaults.enabled(SysKey::SimErosion));
    EXPECT_FALSE(defaults.enabled(SysKey::SimPlantGrowth));
    EXPECT_FLOAT_EQ(defaults.user().fov, 80.0f);

    const auto overlay = Directory() / "settings.json";
    Write(overlay, R"({"sim":{"plant_growth":{"enabled":true}},"user":{"video":{"fov":100}}})");
    const auto layered = SystemConfig::LoadFromRuntimeRoot(Runtime(), overlay);
    EXPECT_TRUE(layered.enabled(SysKey::SimErosion));
    EXPECT_FALSE(layered.enabled(SysKey::SimPlantGrowth));
    EXPECT_FLOAT_EQ(layered.user().fov, 100.0f);
}

TEST_F(RuntimeSystemConfig, NativePathsPreserveLayeringAndExplicitFailure) {
    const auto defaults = Runtime() / "data/common/systems.json";
    const auto overlay = Runtime() / std::filesystem::path(u8"settings-\u4e16\u754c.json");
    const auto explicit_path = Runtime() / std::filesystem::path(u8"override-\u03a9.json");
    Write(defaults, R"({"sim":{"plant_growth":{"enabled":false}},"user":{"video":{"fov":80}}})");
    Write(overlay, R"({"sim":{"plant_growth":{"enabled":true}},"user":{"video":{"fov":100}}})");
    Write(explicit_path, R"({"sim":{"erosion":{"enabled":false}},"user":{"video":{"fov":60}}})");
    EXPECT_FALSE(SystemConfig::LoadFromFile(defaults).enabled(SysKey::SimPlantGrowth));
    auto config = SystemConfig::LoadLayered(defaults, overlay);
    EXPECT_FALSE(config.enabled(SysKey::SimPlantGrowth));
    EXPECT_TRUE(config.enabled(SysKey::SimErosion));
    EXPECT_FLOAT_EQ(config.user().fov, 100.0f);
    EXPECT_EQ(config.ComputeConfigSubHash(),
              SystemConfig::LoadFromRuntimeRoot(Runtime(), overlay).ComputeConfigSubHash());
    std::string error;
    ASSERT_TRUE(SystemConfig::TryLoadFromFile(explicit_path, config, error)) << error;
    EXPECT_FALSE(config.enabled(SysKey::SimErosion));
    EXPECT_FLOAT_EQ(config.user().fov, 100.0f);
    const auto hash = config.ComputeConfigSubHash();
    for (const auto* invalid : {"", "{", "[]", "null", "{} trailing"}) {
        Write(explicit_path, invalid);
        EXPECT_FALSE(SystemConfig::TryLoadFromFile(explicit_path, config, error));
        EXPECT_EQ(config.ComputeConfigSubHash(), hash);
        EXPECT_FLOAT_EQ(config.user().fov, 100.0f);
    }
    std::filesystem::remove(explicit_path);
    EXPECT_FALSE(SystemConfig::TryLoadFromFile(explicit_path, config, error));
    EXPECT_EQ(config.ComputeConfigSubHash(), hash);
    ASSERT_TRUE(config.SaveUserOverlay(overlay));
    EXPECT_FLOAT_EQ(SystemConfig::LoadLayered(defaults, overlay).user().fov, 100.0f);
    std::filesystem::remove(overlay);
    EXPECT_TRUE(SystemConfig::LoadLayered(defaults, overlay)
                    .settings_load_report()
                    .needs_first_launch_detection);
    std::filesystem::create_directory(overlay);
    EXPECT_EQ(SystemConfig::LoadLayered(defaults, overlay).settings_load_report().state,
              luminumbra::core::SettingsLoadState::Recovery);
    const auto blocked_parent = Runtime() / std::filesystem::path(u8"blocked-\u4e16\u754c");
    Write(blocked_parent, "unchanged blocker bytes");
    EXPECT_FALSE(config.SaveUserOverlay(blocked_parent / "settings.json"));
    const std::ifstream blocker(blocked_parent, std::ios::binary);
    std::ostringstream blocker_bytes;
    blocker_bytes << blocker.rdbuf();
    EXPECT_EQ(blocker_bytes.str(), "unchanged blocker bytes");
}

TEST_F(RuntimeSystemConfig, UnavailableOrPartialRuntimeDefaultsNeverUseLaunchDirectory) {
    const auto file = Runtime() / "data/common/systems.json";
    const auto expect_defaults = [&] {
        const auto config = SystemConfig::LoadFromRuntimeRoot(Runtime());
        EXPECT_TRUE(config.enabled(SysKey::SimErosion));
        EXPECT_EQ(config.ComputeConfigSubHash(), SystemConfig::Defaults().ComputeConfigSubHash());
        EXPECT_FLOAT_EQ(config.user().fov, SystemConfig::Defaults().user().fov);
    };
    expect_defaults(); // Missing defaults.
    std::filesystem::create_directory(file);
    expect_defaults(); // A directory cannot supply a configuration document.
    std::filesystem::remove(file);
    for (const auto* bytes : {"", " ", "{\"sim\":"}) {
        Write(file, bytes);
        expect_defaults();
    }
}

constexpr const char* kSimulationGateNames[]{
#define SIM_GATE_Sim(NAME) NAME,
#define SIM_GATE_Render(NAME)
#define SIM_GATE_NAME(ENUM, SECTION, JSON_SECTION, NAME, RESIDENCY) SIM_GATE_##SECTION(NAME)
    LUMIN_CONFIG_KEY_TABLE(SIM_GATE_NAME)
#undef SIM_GATE_NAME
#undef SIM_GATE_Render
#undef SIM_GATE_Sim
};

TEST(SystemConfig, SimulationArtifactReportsEveryRegistryGate) {
    const auto gates = SystemConfig::Defaults().SimulationGates();
    std::size_t count = 0;
    for (const char* name : kSimulationGateNames) {
        SCOPED_TRACE(name);
        ++count;
        ASSERT_TRUE(gates.contains(name));
        EXPECT_TRUE(gates.at(name));
    }
    EXPECT_EQ(gates.size(), count);
}

TEST(SystemConfig, SimulationArtifactReportsEveryExplicitOptOut) {
    const auto gates = SystemConfig::Defaults().SimulationGates();
    for (const char* name : kSimulationGateNames) {
        SCOPED_TRACE(name);
        nlohmann::json document;
        document["sim"][name]["enabled"] = false;
        Luminumbra::world::GameSession session;
        session.ApplySystemConfig(SystemConfig::FromJsonString(document.dump()));
        const auto effective = session.EffectiveSimulationGates();
        EXPECT_EQ(effective.size(), gates.size());
        EXPECT_FALSE(effective.at(name));
    }
}

std::string ConfigIdentity(const SystemConfig& config) {
    Luminumbra::world::GameSession session;
    session.ApplySystemConfig(config);
    return session.SystemConfigIdentity();
}

TEST(SystemConfig, SimulationIdentityPinsShippingDefaults) {
    // Exact little-endian scalar serialization, independently computed with stdlib Python.
    // See docs/development/system-config-identity.md for the complete computation.
    EXPECT_EQ(Luminumbra::world::GameSession{}.LegacyRevision3SystemConfigIdentity(),
              "60ab3c242d54ce89");
    EXPECT_EQ(ConfigIdentity(SystemConfig::Defaults()), "2a0d6f2dc4028dda");
    const auto path = std::filesystem::path(__FILE__).parent_path().parent_path().parent_path() /
                      "data/common/systems.json";
    ASSERT_TRUE(std::filesystem::exists(path));
    Luminumbra::world::GameSession legacy;
    legacy.ApplySystemConfig(SystemConfig::LoadFromFile(path));
    EXPECT_EQ(legacy.LegacyRevision3SystemConfigIdentity(), "60ab3c242d54ce89");
    EXPECT_EQ(ConfigIdentity(SystemConfig::LoadFromFile(path)), "2a0d6f2dc4028dda");
    EXPECT_EQ(luminumbra::core::SimulationIdentityValues{}.ComputeIdentity(),
              ConfigIdentity(SystemConfig::Defaults()));
}

TEST(SystemConfig, FrozenHistoricalIdentityVectorsCoverEveryAppliedSetting) {
    // Fixed reference digests from the frozen 297-byte revision-3 projection and
    // independent 301-byte revision-4 encoding, for both active-region modes.
    struct Vector {
        const char* name;
        std::array<const char*, 4> hashes;
    };
    constexpr Vector vectors[]{
        {"hydrology_weather",
         {"b1975c398a0d70f1", "8fd023c18661d6f2", "23be26e2546a0e7a", "6a93bb2c664e6749"}},
        {"weather_events",
         {"979cacb197b79b45", "ddc11ac9ab2edbd6", "455e323c2d5080de", "e0b183a9c7fc50bd"}},
        {"aether_state",
         {"4da37f266978b979", "ec9bff5a8c96f20a", "a9f11c36b15d4702", "bdc521f40205bf21"}},
        {"water_high_res",
         {"f19ddc19ff363221", "fee6c60cfcf39382", "77bc1aa8d3d992aa", "a1571bc82dd99e19"}},
        {"active_regions",
         {"0820e02bb23ece12", "8830afa562b7cab1", "60ab3c242d54ce89", "2a0d6f2dc4028dda"}},
        {"mutation_rate",
         {"685d3dd6e3bacd77", "684927e833c5681c", "87c6f0914eb60e88", "424a8ca0f34125f3"}},
        {"energy_drain_per_second",
         {"f308a2a1a5d112a9", "af52df8cd63da97e", "2049da7218c8c4b2", "78a3c2cb5b3b5365"}},
        {"energy_rest_recover",
         {"f316851aa23f4334", "cda65360f3475aef", "5c667288b978f003", "0dc7b04327ac77c8"}},
        {"energy_sleep_recover",
         {"07d7dc3b1cb8e286", "5cd57480deb4afbd", "10afe978edc705ad", "59e50ba0c1e73ad6"}},
        {"hunger_growth_per_second",
         {"789f49bc80b89f48", "7963c2863f5ac077", "fcded29c6381fc37", "87672226cd92f2b0"}},
        {"hunger_graze_sate",
         {"e0c54945f81bebd2", "aeea71a54a4ca1f1", "bfa151bde98c6f49", "48f086841169c71a"}},
        {"stamina_rest_recover",
         {"1d8582c805590483", "5f379c8b3df1fcbc", "7fbbf8d5d02193b4", "56068c71f5173b93"}},
        {"stamina_move_drain",
         {"32e3f58cad39f794", "71ae70081ff22dfb", "3670057f1b14c3e3", "cebc65e5f742b224"}},
        {"herd_weight",
         {"01da4d6dacc5ce11", "76894f289f28e346", "0cd5ce25eea4171a", "aa2159dc16318ead"}},
        {"alignment_weight",
         {"8548338bfa873dd2", "a06be1d753771f71", "64243c03ebf7c149", "0cbcfbbf21ba589a"}},
        {"catch_radius",
         {"9c9a064a0d815352", "17852f563d00ff71", "77592945e93dc0c9", "83d6493e0b44389a"}},
        {"catch_satiation",
         {"ff92cc970fa81e81", "dd94ee14295a1ace", "f4b51fb6cf0e578a", "3e23f4671550de35"}},
        {"flock_neighbor_radius",
         {"66d35a8ed48e9382", "1c81880e19ed58e1", "57846f6b4646eff9", "04b9f9272dc1aaca"}},
        {"flock_separation_radius",
         {"f36052321e998752", "ac7e9431e63dbef1", "ce1f752dfa55f4c9", "4684a910ad5ae41a"}},
        {"flock_cohesion_weight",
         {"35799529611c3ef7", "8435306e6b28e434", "6419052d9ac49008", "d6be819bce9149cb"}},
        {"flock_separation_weight",
         {"fc70e688455b7206", "7931bb1b786ca2ed", "7b895025fac53f2d", "6def33c8b1d87786"}},
        {"graze_radius",
         {"43327ef3839a8cd2", "cec85584da2403f1", "220e876b750b1049", "68ce6a63a141291a"}},
        {"graze_per_creature",
         {"fc072684a572ed4b", "562e828facfd8680", "4821155bbaf8af5c", "5ebd891abaa80547"}},
        {"regrow_per_tick",
         {"8829887f8c91cf3d", "80c679a6475c751a", "a26425a9c07e3cd6", "e6c064c7803f4ff1"}},
        {"feed_per_graze",
         {"6a01bedb4fea3e78", "8186704e10a399ff", "4b8920bf3f9be767", "a7d1804df46a8418"}},
        {"rise_rate_per_second",
         {"d58e9289ead43be3", "af9c0aa590a96294", "d20282977cf96f94", "c81de6be959c8cab"}},
        {"drink_rate_per_second",
         {"23959a73c2d11e52", "2f77044abf8faef1", "fe54bd6f9e8d8bc9", "c97d192986acd41a"}},
        {"seek_threshold",
         {"a4818c84cd83cc33", "9160b6e06e512d04", "77c086e95acb7d64", "14a1fe492a77fb5b"}},
        {"hunger_threshold",
         {"187dca69ca9a513b", "d6dfc7dbf4472664", "0e2a413a6b9f490c", "3be4cb8f2427293b"}},
        {"feed_radius",
         {"23cb6d3f5fd7e78b", "f722bbd9153302ec", "3262959885cf209c", "d9997ca0adf1f543"}},
        {"feed_rate_per_tick",
         {"2634683024d49a3b", "4f8e682b60a6a6d8", "1be0df00c5d9920c", "d75fac61c38823bf"}},
        {"maturity_ticks",
         {"aa5addef1bd017a3", "e67957a136a0bb10", "8da2c96e50024954", "bddd4f0445a96e57"}},
        {"cooldown_ticks",
         {"9855901802c6298b", "b87b2cc98534e930", "a6ecb87128bd629c", "01998aec0a50b2f7"}},
        {"healthy_stamina",
         {"6f47172c8a7d2fd2", "2fd2fbb8c7729f71", "4e231fa47bedb349", "9c2415a095b5d89a"}},
        {"mate_seek_radius",
         {"f2dc3640d6074196", "9570975264c968ad", "ea4ca76611a910fd", "61d88c9eedc0bd46"}},
        {"courtship_radius",
         {"75bdd1c6e3c940d2", "c0feb311fb69d4f1", "5499da3ed539c449", "5b04c7f0c286fa1a"}},
        {"courtship_ticks",
         {"bdf689fe074c9c27", "bc31f7cafa93815c", "1c0427bf58fe8038", "3ddc984123f9a033"}},
        {"spawn_radius",
         {"9d31f8449f67230e", "d6d54968861ec191", "cfe1b76e5757ddb5", "ecee797a474fec3a"}},
        {"deposit",
         {"7568296522c4fe1b", "1b323d26a7cb735c", "2857e1634040ad6c", "9cdcdd9cd1319233"}},
        {"trail_weight",
         {"5f470b8621134834", "82fa208bcdc9543f", "c896f8f4384cf503", "4134653f795e2b58"}},
        {"goal_weight",
         {"39b325ecef65bad3", "9f53c23606c499b4", "05629935822ec304", "5c1d04bbcbe3294b"}},
        {"amplitude",
         {"a01007e699175c93", "63e8ca7f33e1d9c4", "fdcc2987aac02fdc", "80bbdb54812b1793"}},
    };
    const auto variants = Luminumbra::TestSupport::SimulationConfigVariants();
    ASSERT_EQ(variants.size(), std::size(vectors));
    for (std::size_t i = 0; i < variants.size(); ++i) {
        ASSERT_EQ(variants[i].first, vectors[i].name);
        for (const bool regions : {false, true}) {
            SCOPED_TRACE(variants[i].first + (regions ? " regions on" : " regions off"));
            Luminumbra::world::GameSession session;
            session.ApplySystemConfig(variants[i].second);
            session.SetActiveRegionsEnabled(regions);
            const std::size_t offset = regions ? 2u : 0u;
            EXPECT_EQ(session.LegacyRevision3SystemConfigIdentity(), vectors[i].hashes[offset]);
            EXPECT_EQ(session.SystemConfigIdentity(), vectors[i].hashes[offset + 1]);
        }
    }
}

TEST(SystemConfig, FrozenIdentitySessionOptionsCoverBothRegionProfiles) {
    constexpr std::array<const char*, 2> expected[]{
        {"6940021d07bb62cf", "35e66f9ff9909b7c"},
        {"dc39d8a56618c7ff", "af9c68017d02d168"},
        {"0017f78957d1631d", "0d4c1ac65f32c5ee"},
        {"c66b76418c25cab5", "7db3826a535894e6"},
        {"fb71ff0134ef6715", "02129ef0ae61d086"},
        {"0820df2bb23ecc5f", "62240625eac98b88"},
        {"973a17daa4bfe1cc", "58fd791b596366af"},
        {"e49a4f3da30a6690", "a91cf301396b179f"},
        {"8426406d0826954e", "c123fc31bd99c6ad"},
        {"acd293f874d98fe6", "751bb18f901f06b5"},
        {"6d5a1d4eaaa43586", "afd64067dbb45115"},
        {"60ab39242d54c970", "1d33ec45f86107ff"},
    };
    std::size_t index = 0;
    for (const bool regions : {false, true}) {
        for (int option = 0; option < 6; ++option) {
            Luminumbra::world::GameSession session;
            session.SetActiveRegionsEnabled(regions);
            auto scheduler = Luminumbra::world::RegionSchedulerConfig{};
            switch (option) {
                case 0:
                    session.SetFiniteHydrologyEnabled(false);
                    break;
                case 1:
                    session.SetWeatherRainEnabled(true, 26);
                    break;
                case 2:
                    session.SetWeatherRainEnabled(true, std::numeric_limits<std::int32_t>::min());
                    break;
                case 3:
                    --scheduler.work_limit;
                    session.SetRegionSchedulerConfig(scheduler);
                    break;
                case 4:
                    ++scheduler.hold_ticks;
                    session.SetRegionSchedulerConfig(scheduler);
                    break;
                case 5:
                    ++scheduler.reduced_shift;
                    session.SetRegionSchedulerConfig(scheduler);
                    break;
            }
            EXPECT_EQ(session.LegacyRevision3SystemConfigIdentity(), expected[index][0]);
            EXPECT_EQ(session.SystemConfigIdentity(), expected[index][1]);
            ++index;
        }
    }
    EXPECT_EQ(index, std::size(expected));
}

TEST(SystemConfig, SimulationIdentityCoversEveryAppliedInput) {
    const auto baseline = ConfigIdentity(SystemConfig::Defaults());
    for (const auto& [name, identity] : Luminumbra::TestSupport::EffectiveIdentityMismatches()) {
        SCOPED_TRACE(name);
        EXPECT_NE(identity, baseline);
    }
}

TEST(SystemConfig, SimulationIdentityUsesResolvedValuesAndIgnoresPresentation) {
    const auto baseline = ConfigIdentity(SystemConfig::Defaults());
    const auto explicit_defaults = SystemConfig::FromJsonString(R"({
        "user":{"video":{"fov":90}},
        "render":{"moonlight":{"enabled":false,"params":{"strength":0.75}}},
        "sim":{"ecology":{"params":{"herd_weight":0.8,"energy_drain_per_second":0.006}},
               "plant_growth":{"params":{"mutation_rate":0.05}}}})");
    EXPECT_EQ(ConfigIdentity(explicit_defaults), baseline);
    const auto first = SystemConfig::FromJsonString(R"({"sim":{
        "ecology":{"params":{"herd_weight":0.9,"catch_radius":2.5}},
        "plant_growth":{"params":{"mutation_rate":0.2}}}})");
    const auto reordered = SystemConfig::FromJsonString(R"({"sim":{
        "plant_growth":{"params":{"mutation_rate":0.2}},
        "ecology":{"params":{"catch_radius":2.5,"herd_weight":0.9}}}})");
    EXPECT_EQ(ConfigIdentity(first), ConfigIdentity(reordered));
    const auto disabled = SystemConfig::FromJsonString(R"({"sim":{"ecology":{"enabled":false}}})");
    const auto ignored = SystemConfig::FromJsonString(
        R"({"sim":{"ecology":{"enabled":false,"params":{"herd_weight":0.9}}}})");
    EXPECT_EQ(ConfigIdentity(disabled), ConfigIdentity(ignored));
    const auto positive_zero = SystemConfig::FromJsonString(
        R"({"sim":{"plant_growth":{"params":{"mutation_rate":0.0}}}})");
    const auto negative_zero = SystemConfig::FromJsonString(
        R"({"sim":{"plant_growth":{"params":{"mutation_rate":-0.0}}}})");
    EXPECT_NE(ConfigIdentity(positive_zero), ConfigIdentity(negative_zero));
}

// missing/empty config yields all-defaults, no crash.
TEST(SystemConfig, ShippingDefaultsAllOn) {
    const SystemConfig cfg = SystemConfig::Defaults();
    for (unsigned key = 0; key < static_cast<unsigned>(SysKey::Count); ++key) {
        const auto id = static_cast<SysKey>(key);
        EXPECT_TRUE(cfg.enabled(id)) << key;
    }
    EXPECT_TRUE(cfg.enabled(SysKey::SimPlantGrowth));
    EXPECT_TRUE(cfg.enabled(SysKey::SimErosion));
    EXPECT_TRUE(cfg.enabled(SysKey::RenderMoonlight));
    EXPECT_TRUE(cfg.enabled(SysKey::RenderTreeWind));
    EXPECT_FLOAT_EQ(cfg.param(SysParam::PlantMutationRate, 0.123f), 0.123f);
    EXPECT_FLOAT_EQ(cfg.param(SysParam::MoonlightStrength, 0.5f), 0.5f);
}

TEST(SystemConfig, DefaultsRequestFarWorldLadder) {
    const SystemConfig cfg = SystemConfig::Defaults();
    EXPECT_TRUE(cfg.enabled(SysKey::SimPlantGrowth));
    EXPECT_TRUE(cfg.enabled(SysKey::SimErosion));
    EXPECT_TRUE(cfg.enabled(SysKey::RenderMoonlight));
    EXPECT_TRUE(cfg.enabled(SysKey::RenderTreeWind));
    EXPECT_TRUE(cfg.enabled(SysKey::RenderFarWorldLadder));
    EXPECT_FLOAT_EQ(cfg.param(SysParam::PlantMutationRate, 0.123f), 0.123f);
    EXPECT_FLOAT_EQ(cfg.param(SysParam::MoonlightStrength, 0.5f), 0.5f);
}

TEST(SystemConfig, MissingFileIsDefaults) {
    const SystemConfig cfg = SystemConfig::LoadFromFile("this/path/does/not/exist.json");
    for (unsigned key = 0; key < static_cast<unsigned>(SysKey::Count); ++key)
        EXPECT_TRUE(cfg.enabled(static_cast<SysKey>(key))) << key;
    EXPECT_TRUE(cfg.enabled(SysKey::RenderMoonlight));
    EXPECT_TRUE(cfg.enabled(SysKey::RenderFarWorldLadder));
    EXPECT_FLOAT_EQ(cfg.param(SysParam::MoonlightStrength, 0.7f), 0.7f);
}

TEST(SystemConfig, ExplicitOverrideLoadsHistoricalConfiguration) {
    const auto path = std::filesystem::path(__FILE__).parent_path().parent_path() /
                      "fixtures/historical-defaults-off.json";
    SystemConfig config;
    std::string error = "previous error";
    ASSERT_TRUE(SystemConfig::TryLoadFromFile(path, config, error)) << error;
    EXPECT_TRUE(error.empty());
#define EXPECT_HISTORICAL_KEY(ENUM, SECTION, JSON_SECTION, JSON_NAME, RESIDENCY)                   \
    if (std::string(JSON_SECTION) == "sim") {                                                      \
        EXPECT_FALSE(config.enabled(SysKey::ENUM)) << (JSON_NAME);                                 \
    }
    LUMIN_CONFIG_KEY_TABLE(EXPECT_HISTORICAL_KEY)
#undef EXPECT_HISTORICAL_KEY
    EXPECT_TRUE(config.ComputeConfigSubHash().empty());
    EXPECT_TRUE(config.enabled(SysKey::RenderFarWorldLadder));
    EXPECT_FALSE(config.enabled(SysKey::RenderCircadian));
    EXPECT_NE(ConfigIdentity(config), ConfigIdentity(SystemConfig::Defaults()));
    EXPECT_EQ(ConfigIdentity(config), ConfigIdentity(SystemConfig::LoadFromFile(path)));
}

TEST(SystemConfig, ExplicitOverrideRefusesInvalidFilesWithoutChangingSnapshot) {
    const auto directory =
        Luminumbra::TestSupport::CreateUniqueTempDirectory("lumin-systemconfig-override-");
    const auto path = directory / "override.json";
    std::error_code remove_error;
    SystemConfig config =
        SystemConfig::FromJsonString(R"({"sim":{"water_high_res":{"enabled":false}}})");
    const auto identity = ConfigIdentity(config);
    std::string error;
    EXPECT_FALSE(SystemConfig::TryLoadFromFile(path, config, error));
    EXPECT_FALSE(error.empty());
    EXPECT_EQ(ConfigIdentity(config), identity);
    for (const char* text : {"",
                             "{",
                             "[]",
                             "null",
                             "{} trailing",
                             R"({"render":{"moonlight":{"params":{"color":[1,"bad",3]}}}})"}) {
        SCOPED_TRACE(text);
        {
            std::ofstream file(path);
            ASSERT_TRUE(file.good());
            file << text;
        }
        EXPECT_FALSE(SystemConfig::TryLoadFromFile(path, config, error));
        EXPECT_FALSE(error.empty());
        EXPECT_EQ(ConfigIdentity(config), identity);
    }
    std::filesystem::remove_all(directory, remove_error);
}

TEST(SystemConfig, EmptyAndBlankJsonAreDefaults) {
    for (const char* text : {"",
                             "{}",
                             "   ",
                             "not json at all",
                             R"({"render":{"far_world_ladder":{}}})",
                             R"({"render":{"far_world_ladder":{"enabled":null}}})",
                             R"({"render":{"far_world_ladder":{"enabled":"false"}}})"}) {
        const SystemConfig cfg = SystemConfig::FromJsonString(text);
        for (unsigned key = 0; key < static_cast<unsigned>(SysKey::Count); ++key)
            EXPECT_TRUE(cfg.enabled(static_cast<SysKey>(key)))
                << "input: " << text << " key: " << key;
        EXPECT_TRUE(cfg.enabled(SysKey::SimPlantGrowth)) << "input: " << text;
        EXPECT_TRUE(cfg.enabled(SysKey::RenderFarWorldLadder)) << "input: " << text;
    }
}

// param parse + defaults round-trip; unknown keys ignored.
TEST(SystemConfig, RoundTripNamedValuesAndFallbacks) {
    const std::string json = R"({
      "render": {
        "moonlight": { "enabled": true, "params": { "strength": 0.42, "color": [0.6, 0.7, 1.0] } }
      },
      "sim": {
        "plant_growth": { "enabled": false, "params": { "mutation_rate": 0.08 } }
      }
    })";
    const SystemConfig cfg = SystemConfig::FromJsonString(json);
    EXPECT_TRUE(cfg.enabled(SysKey::RenderMoonlight));
    EXPECT_FALSE(cfg.enabled(SysKey::SimPlantGrowth));
    EXPECT_FLOAT_EQ(cfg.param(SysParam::MoonlightStrength, -1.0f), 0.42f);
    EXPECT_FLOAT_EQ(cfg.param(SysParam::PlantMutationRate, -1.0f), 0.08f);
    const glm::vec3 color = cfg.param3(SysParam::MoonlightColor, glm::vec3(0.0f));
    EXPECT_FLOAT_EQ(color.r, 0.6f);
    EXPECT_FLOAT_EQ(color.g, 0.7f);
    EXPECT_FLOAT_EQ(color.b, 1.0f);
    // unnamed param falls back
    EXPECT_FLOAT_EQ(cfg.param(SysParam::PlantMutationRate, 9.0f), 0.08f);
}

TEST(SystemConfig, UnknownKeysIgnoredNoThrow) {
    const std::string json = R"({
      "render": { "moonlight": { "enabled": true }, "bogus_system": { "enabled": true } },
      "sim": { "not_a_real_key": { "params": { "x": 1.0 } } },
      "weird_top_level": 42
    })";
    SystemConfig cfg = SystemConfig::Defaults();
    ASSERT_NO_THROW({ cfg = SystemConfig::FromJsonString(json); });
    EXPECT_TRUE(cfg.enabled(SysKey::RenderMoonlight));
}

// Shipping simulation defaults contribute to the config sub-hash.
TEST(SystemConfig, ConfigSubHashPresentAtDefaults) {
    EXPECT_FALSE(SystemConfig::Defaults().ComputeConfigSubHash().empty());
}

// sim non-default flag/param moves the sub-hash; render flags never do.
TEST(SystemConfig, RenderFlagDoesNotMoveConfigSubHash) {
    nlohmann::json on, off;
    // Exercise every render gate with opposite values and identical sim settings.
    // Keep render parameters different too, to cover both exclusion contracts.
#define CHECK_RENDER_Sim(ENUM, NAME)
#define CHECK_RENDER_Render(ENUM, NAME)                                                            \
    on["render"][NAME]["enabled"] = true;                                                          \
    off["render"][NAME]["enabled"] = false;
#define CHECK_RENDER_KEY(ENUM, SECTION, JSON_SECTION, NAME, RESIDENCY)                             \
    CHECK_RENDER_##SECTION(ENUM, NAME)
    LUMIN_CONFIG_KEY_TABLE(CHECK_RENDER_KEY)
#undef CHECK_RENDER_KEY
#undef CHECK_RENDER_Render
#undef CHECK_RENDER_Sim
    on["render"]["moonlight"]["params"]["strength"] = 9.9;
    off["render"]["moonlight"]["params"]["strength"] = 0.1;
    const auto enabled = SystemConfig::FromJsonString(on.dump());
    const auto disabled = SystemConfig::FromJsonString(off.dump());
    EXPECT_TRUE(enabled.enabled(SysKey::RenderMoonlight));
    EXPECT_FALSE(disabled.enabled(SysKey::RenderMoonlight));
    EXPECT_TRUE(enabled.enabled(SysKey::RenderTreeWind));
    EXPECT_FALSE(disabled.enabled(SysKey::RenderTreeWind));
    EXPECT_FALSE(enabled.ComputeConfigSubHash().empty());
    EXPECT_EQ(enabled.ComputeConfigSubHash(), disabled.ComputeConfigSubHash());
}

TEST(SystemConfig, SimFlagMovesConfigSubHash) {
    const std::string json = R"({ "sim": { "plant_growth": { "enabled": false } } })";
    const SystemConfig cfg = SystemConfig::FromJsonString(json);
    EXPECT_NE(cfg.ComputeConfigSubHash(), SystemConfig::Defaults().ComputeConfigSubHash());
}

TEST(SystemConfig, SimParamMovesConfigSubHash) {
    const std::string enabled_only = R"({ "sim": { "plant_growth": { "enabled": true } } })";
    const std::string with_param =
        R"({ "sim": { "plant_growth": { "enabled": true, "params": { "mutation_rate": 0.2 } } } })";
    const auto h0 = SystemConfig::FromJsonString(enabled_only).ComputeConfigSubHash();
    const auto h1 = SystemConfig::FromJsonString(with_param).ComputeConfigSubHash();
    EXPECT_NE(h0, std::string{});
    EXPECT_NE(h1, std::string{});
    EXPECT_NE(h0, h1);
}

TEST(SystemConfig, ConfigSubHashIsOrderIndependentAndStable) {
    // Same non-default sim set, different JSON key order -> identical hash.
    const std::string a = R"({ "sim": {
        "plant_growth": { "enabled": true, "params": { "mutation_rate": 0.2 } },
        "erosion": { "enabled": true } } })";
    const std::string b = R"({ "sim": {
        "erosion": { "enabled": true },
        "plant_growth": { "params": { "mutation_rate": 0.2 }, "enabled": true } } })";
    const auto ha = SystemConfig::FromJsonString(a).ComputeConfigSubHash();
    const auto hb = SystemConfig::FromJsonString(b).ComputeConfigSubHash();
    EXPECT_FALSE(ha.empty());
    EXPECT_EQ(ha, hb);
}

//   /  — the SCHEMA-GENERATED kKeys/kParams registries must produce a
// config:v1: serialization that is BYTE-IDENTICAL to the hand-authored contract. We reconstruct
// the canonical serialization independently here (same prefix, same setprecision(17), same float
// literals as the schema defaults) and assert ComputeConfigSubHash ==
// StableChecksum(reconstructed). This pins BOTH the serialization format AND each generated default
// value, so a drift in either (e.g. a regenerated header with a changed default, or a serializer
// edit) fails this test rather than silently moving an enabled-system hash. Defaults are never
// exercised by `--smoke` (which enables no sim system → empty hash), so this is the real guard for
// the schema-codegen migration.
std::string ExpectedSubHash(const std::string& canonical_body) {
    std::ostringstream bytes;
    bytes << "config:v1:" << std::setprecision(17) << canonical_body;
    return Luminumbra::Persistence::StableChecksum(bytes.str());
}

// Pin the serialization contract independently of shipping activation defaults.
SystemConfig WithAllOtherSimFlagsOff(const std::string& text) {
    nlohmann::json json;
#define DISABLE_KEY(ENUM, SECTION, JSON_SECTION, JSON_NAME, RESIDENCY)                             \
    if (std::string(JSON_SECTION) == "sim")                                                        \
        json[JSON_SECTION][JSON_NAME]["enabled"] = false;
    LUMIN_CONFIG_KEY_TABLE(DISABLE_KEY)
#undef DISABLE_KEY
    json.merge_patch(nlohmann::json::parse(text));
    return SystemConfig::FromJsonString(json.dump());
}

TEST(SystemConfig, ConfigSubHashEmptyWithAllSimFlagsDisabled) {
    EXPECT_EQ(WithAllOtherSimFlagsOff("{}").ComputeConfigSubHash(), std::string{});
}

TEST(SystemConfig, ConfigSubHashByteIdenticalSnapshot) {
    // Single sim key with one scalar param at its compiled default.
    {
        std::ostringstream body;
        body << std::setprecision(17) << "plant_growth:en=1;mutation_rate=" << 0.05f << ';';
        const auto cfg =
            WithAllOtherSimFlagsOff(R"({ "sim": { "plant_growth": { "enabled": true } } })");
        EXPECT_EQ(cfg.ComputeConfigSubHash(), ExpectedSubHash(body.str()));
    }
    // Sim key with NO owned params still emits the enabled marker and nothing after it.
    {
        const auto cfg =
            WithAllOtherSimFlagsOff(R"({ "sim": { "erosion": { "enabled": true } } })");
        EXPECT_EQ(cfg.ComputeConfigSubHash(), ExpectedSubHash("erosion:en=1;"));
    }
    // Sim key owning multiple scalar params: every owned default is emitted in kParams order.
    {
        std::ostringstream body;
        body << std::setprecision(17) << "foraging:en=1;deposit=" << 1.0f
             << ";trail_weight=" << 8.0f << ";goal_weight=" << 1.0f << ';';
        const auto cfg =
            WithAllOtherSimFlagsOff(R"({ "sim": { "foraging": { "enabled": true } } })");
        EXPECT_EQ(cfg.ComputeConfigSubHash(), ExpectedSubHash(body.str()));
    }
    // Multiple enabled sim keys serialize in kKeys (schema) order: plant_growth precedes erosion.
    {
        std::ostringstream body;
        body << std::setprecision(17) << "plant_growth:en=1;mutation_rate=" << 0.05f
             << ";erosion:en=1;";
        const auto cfg = WithAllOtherSimFlagsOff(
            R"({ "sim": { "erosion": { "enabled": true },
                          "plant_growth": { "enabled": true } } })");
        EXPECT_EQ(cfg.ComputeConfigSubHash(), ExpectedSubHash(body.str()));
    }
    // An explicitly-set non-default param overrides the generated default in the same format.
    {
        std::ostringstream body;
        body << std::setprecision(17) << "plant_growth:en=1;mutation_rate=" << 0.2f << ';';
        const auto cfg = WithAllOtherSimFlagsOff(
            R"({ "sim": { "plant_growth": { "enabled": true, "params": { "mutation_rate": 0.2 } } } })");
        EXPECT_EQ(cfg.ComputeConfigSubHash(), ExpectedSubHash(body.str()));
    }
    // Explicitly disabling all sim flags retains the empty compatibility baseline.
    EXPECT_EQ(WithAllOtherSimFlagsOff("{}").ComputeConfigSubHash(), std::string{});
}

// enabled is a cheap, deterministic hot-path query (structural O(1);
// functional smoke that it is side-effect-free and stable across many calls).
TEST(SystemConfig, EnabledHotPathStable) {
    const SystemConfig cfg =
        SystemConfig::FromJsonString(R"({ "sim": { "erosion": { "enabled": true } } })");
    bool acc = false;
    for (int i = 0; i < 1'000'000; ++i) {
        acc ^= cfg.enabled(SysKey::SimErosion);
    }
    // 1e6 XORs of `true` -> false; the point is no crash/alloc and a stable answer.
    EXPECT_FALSE(acc);
    EXPECT_TRUE(cfg.enabled(SysKey::SimErosion));
}

// ---- Addendum A: user.* settings extension (-101..105) ----

// -101 — user.video/audio round-trip; unnamed fields keep defaults.
TEST(SystemConfig, UserVideoAudioRoundTrip) {
    const std::string json = R"({
      "user": {
        "video": { "resolution": "3840x1600", "window_mode": "fullscreen", "vsync": false,
                   "fov": 70.0, "render_scale": 0.85, "mouse_sensitivity": 0.22 },
        "audio": { "master": 0.8, "music": 0.5 }
      }
    })";
    const SystemConfig cfg = SystemConfig::FromJsonString(json);
    const auto& u = cfg.user();
    EXPECT_EQ(u.resolution, "3840x1600");
    EXPECT_EQ(u.window_mode, "fullscreen");
    EXPECT_FALSE(u.vsync);
    EXPECT_FLOAT_EQ(u.fov, 70.0f);
    EXPECT_FLOAT_EQ(u.render_scale, 0.85f);
    EXPECT_FLOAT_EQ(u.mouse_sensitivity, 0.22f);
    EXPECT_FLOAT_EQ(u.audio_master, 0.8f);
    EXPECT_FLOAT_EQ(u.audio_music, 0.5f);
    EXPECT_FLOAT_EQ(u.audio_sfx, 1.0f); // unnamed -> default
}

// Default look sensitivity is 25% of the prior 0.1 (2026-06-18).
// Shipping VSync defaults ON; saved user settings remain authoritative.
TEST(SystemConfig, UserVideoDefaults) {
    const auto config = SystemConfig::Defaults();
    const auto& u = config.user();
    EXPECT_FLOAT_EQ(u.mouse_sensitivity, 0.025f);
    EXPECT_TRUE(u.vsync);
    EXPECT_EQ(u.frame_cap, 0);
}

TEST(SystemConfig, FramePacingPersistsEverySupportedChoice) {
    for (const int cap : {0, 30, 60, 120, 144, 165, 240}) {
        for (const bool vsync : {false, true}) {
            SCOPED_TRACE(cap);
            SCOPED_TRACE(vsync);
            const Luminumbra::TestSupport::ScopedTempFile file("frame_pacing_", ".json");
            const nlohmann::json document = {
                {"user", {{"video", {{"vsync", vsync}, {"frame_cap", cap}}}}}};
            const auto config = SystemConfig::FromJsonString(document.dump());
            EXPECT_EQ(config.user().vsync, vsync);
            EXPECT_EQ(config.user().frame_cap, cap);
            ASSERT_TRUE(config.SaveUserOverlay(file.Path()));
            const auto reloaded = SystemConfig::LoadLayered("", file.Path());
            EXPECT_EQ(reloaded.user().vsync, vsync);
            EXPECT_EQ(reloaded.user().frame_cap, cap);
            EXPECT_EQ(reloaded.ComputeConfigSubHash(),
                      SystemConfig::Defaults().ComputeConfigSubHash());
        }
    }
}

TEST(SystemConfig, InvalidFramePacingValuesKeepDefaultsOrPreviousOverlay) {
    const auto invalid_caps = nlohmann::json::parse(
        R"([null,true,false,"30","60","165","unlimited",30.0,60.0,60.5,165.0,
            -1,1,29,31,61,164,166,300,[],{},4294967326,4294967356,4294967461,
            18446744073709551615,-9223372036854775808])");
    for (const auto& cap : invalid_caps) {
        SCOPED_TRACE(cap.dump());
        const nlohmann::json document = {{"user", {{"video", {{"frame_cap", cap}}}}}};
        EXPECT_EQ(SystemConfig::FromJsonString(document.dump()).user().frame_cap, 0);
        auto config = SystemConfig::Defaults();
        config.OverlayUserFromJsonString(R"({"user":{"video":{"frame_cap":144}}})");
        config.OverlayUserFromJsonString(document.dump());
        EXPECT_EQ(config.user().frame_cap, 144);
    }
    const auto invalid_vsync = nlohmann::json::parse(R"([null,0,1,"on","off",[],{}])");
    for (const auto& vsync : invalid_vsync) {
        SCOPED_TRACE(vsync.dump());
        const nlohmann::json document = {{"user", {{"video", {{"vsync", vsync}}}}}};
        EXPECT_TRUE(SystemConfig::FromJsonString(document.dump()).user().vsync);
        auto config = SystemConfig::Defaults();
        config.OverlayUserFromJsonString(R"({"user":{"video":{"vsync":false}}})");
        config.OverlayUserFromJsonString(document.dump());
        EXPECT_FALSE(config.user().vsync);
    }
    auto config = SystemConfig::Defaults();
    config.OverlayUserFromJsonString(R"({"user":{"video":{"vsync":false,"frame_cap":240}}})");
    config.OverlayUserFromJsonString(R"({"user":{"video":{"unknown_setting":120}}})");
    config.OverlayUserFromJsonString(R"({"user":{"video":{}}})");
    EXPECT_FALSE(config.user().vsync);
    EXPECT_EQ(config.user().frame_cap, 240);
}

TEST(SystemConfig, InvalidInMemoryFrameCapSavesAsUnlimited) {
    const Luminumbra::TestSupport::ScopedTempFile file("frame_pacing_", ".json");
    auto config = SystemConfig::Defaults();
    config.user().frame_cap = 61;
    ASSERT_TRUE(config.SaveUserOverlay(file.Path()));
    EXPECT_EQ(SystemConfig::LoadFromFile(file.Path()).user().frame_cap, 0);
}

// -105 — keybind map resolves action->key; missing action -> fallback.
TEST(SystemConfig, UserControlsKeybindRoundTrip) {
    const std::string json = R"({
      "user": { "controls": { "Move_Forward": 87, "Jump": 32 } }
    })";
    const SystemConfig cfg = SystemConfig::FromJsonString(json);
    EXPECT_EQ(cfg.keybind("Move_Forward", -1), 87);
    EXPECT_EQ(cfg.keybind("Jump", -1), 32);
    EXPECT_EQ(cfg.keybind("Crouch", 67), 67); // unbound -> fallback (compiled default)
}

// -102 — user.* never moves the config sub-hash.
TEST(SystemConfig, UserSettingsDoNotHash) {
    const std::string json = R"({
      "user": { "video": { "fov": 120.0, "vsync": false },
                "controls": { "Jump": 32 } }
    })";
    const SystemConfig cfg = SystemConfig::FromJsonString(json);
    EXPECT_EQ(cfg.ComputeConfigSubHash(), SystemConfig::Defaults().ComputeConfigSubHash());
}

// -103 — SaveUserOverlay round-trips and writes ONLY user.* (no sim/render leakage).
TEST(SystemConfig, UserOverlaySaveReload) {
    const std::filesystem::path dir =
        std::filesystem::temp_directory_path() / "lumin_systemconfig_test";
    std::error_code rm_ec;
    std::filesystem::remove_all(dir, rm_ec); // non-throwing pre-clean
    const std::filesystem::path path = dir / "settings.json";

    const SystemConfig cfg = SystemConfig::FromJsonString(R"({
      "sim": { "erosion": { "enabled": true } },
      "user": { "video": { "fov": 95.0, "window_mode": "windowed" },
                "controls": { "Sprint": 340 } }
    })");
    ASSERT_TRUE(cfg.SaveUserOverlay(path));

    // The written file must contain only user.* — no sim/render keys. Scope the stream so
    // its handle is closed before cleanup (Windows can't delete an open file).
    std::string text;
    {
        const std::ifstream in(path);
        std::ostringstream ss;
        ss << in.rdbuf();
        text = ss.str();
    }
    EXPECT_NE(text.find("\"user\""), std::string::npos);
    EXPECT_EQ(text.find("\"sim\""), std::string::npos);
    EXPECT_EQ(text.find("\"render\""), std::string::npos);

    const SystemConfig reloaded = SystemConfig::LoadFromFile(path);
    EXPECT_FLOAT_EQ(reloaded.user().fov, 95.0f);
    EXPECT_EQ(reloaded.user().window_mode, "windowed");
    EXPECT_EQ(reloaded.keybind("Sprint", -1), 340);
    EXPECT_TRUE(reloaded.enabled(SysKey::SimErosion)); // overlay carries no sim flags

    std::filesystem::remove_all(dir, rm_ec); // non-throwing teardown
}

// -104 — overlay merge precedence: user.* from overlay, sim/render from defaults;
// a sim.* in the overlay is ignored.
TEST(SystemConfig, OverlayMergePrecedence) {
    SystemConfig cfg = SystemConfig::FromJsonString(R"({
      "sim": { "plant_growth": { "enabled": true } },
      "user": { "video": { "fov": 45.0 } }
    })");
    EXPECT_TRUE(cfg.enabled(SysKey::SimPlantGrowth));
    EXPECT_FLOAT_EQ(cfg.user().fov, 45.0f);

    // Overlay only changes user.*; its sim flag must be ignored.
    cfg.OverlayUserFromJsonString(R"({
      "sim": { "plant_growth": { "enabled": false } },
      "user": { "video": { "fov": 100.0 } }
    })");
    EXPECT_TRUE(cfg.enabled(SysKey::SimPlantGrowth)); // unchanged by overlay
    EXPECT_FLOAT_EQ(cfg.user().fov, 100.0f);          // overlay wins for user.*
}

TEST(SystemConfig, MalformedOverlayKeepsSettings) {
    SystemConfig cfg = SystemConfig::FromJsonString(R"({ "user": { "video": { "fov": 60.0 } } })");
    cfg.OverlayUserFromJsonString("}{ not json");
    EXPECT_FLOAT_EQ(cfg.user().fov, 60.0f); // unchanged
}

TEST(SystemConfig, EnabledMissingTrueFalseAndMalformed) {
    for (const auto& entry : {"{}",
                              "{\"enabled\":true}",
                              "{\"enabled\":false}",
                              "{\"enabled\":0}",
                              "{\"enabled\":null}",
                              "false"}) {
        auto json = nlohmann::json::parse(entry);
        const nlohmann::json document = {{"sim", {{"erosion", json}}}};
        const auto cfg = SystemConfig::FromJsonString(document.dump());
        const bool expected = !(json.is_object() && json.contains("enabled") &&
                                json["enabled"].is_boolean() && !json["enabled"].get<bool>());
        EXPECT_EQ(cfg.enabled(SysKey::SimErosion), expected) << entry;
    }
}

TEST(SystemConfig, PackagedFlagsAgreeWithCompiledDefaults) {
    const auto path = std::filesystem::path(__FILE__).parent_path().parent_path().parent_path() /
                      "data/common/systems.json";
    ASSERT_TRUE(std::filesystem::exists(path));
    std::ifstream file(path);
    const auto document = nlohmann::json::parse(file);
#define CHECK_PACKAGED_FLAG(name, section, json_section, json_name, residency)                     \
    EXPECT_EQ(document.at(json_section).at(json_name).at("enabled").get<bool>(),                   \
              SystemConfig::Defaults().enabled(SysKey::name))                                      \
        << (json_name);
    LUMIN_CONFIG_KEY_TABLE(CHECK_PACKAGED_FLAG)
#undef CHECK_PACKAGED_FLAG
    const auto packaged = SystemConfig::LoadFromFile(path);
    for (unsigned key = 0; key < static_cast<unsigned>(SysKey::Count); ++key)
        EXPECT_EQ(packaged.enabled(static_cast<SysKey>(key)),
                  SystemConfig::Defaults().enabled(static_cast<SysKey>(key)))
            << key;
}

TEST(SystemConfig, SavedFalseVsyncRemainsAuthoritative) {
    auto cfg = SystemConfig::Defaults();
    cfg.OverlayUserFromJsonString(R"({"user":{"video":{"vsync":false}},
        "sim":{"erosion":{"enabled":false}},"render":{"taau":{"enabled":false}}})");
    EXPECT_FALSE(cfg.user().vsync);
    EXPECT_TRUE(cfg.enabled(SysKey::SimErosion));
    EXPECT_TRUE(cfg.enabled(SysKey::RenderTaau));
}

TEST(SystemConfig, FarWorldCaveFieldDefaultsAndExactNumericCodes) {
    using namespace Luminumbra::Rendering;
    using Luminumbra::World::FarCaveMode;
    const auto defaults = SystemConfig::Defaults();
    EXPECT_TRUE(defaults.enabled(SysKey::RenderFarWorldLadder));
    EXPECT_EQ(ResolveFarWorldCaveField(
                  defaults.param(SysParam::FarWorldCaveField, kDefaultFarWorldCaveFieldCode)),
              FarCaveMode::BandLimited);
    // The compiled schema default and the selector fallback must agree.
    constexpr float schema_defaults[]{
#define SCHEMA_SCALAR_DEFAULT(ENUM, OWNER, NAME, VEC, DEFAULT, X, Y, Z) DEFAULT,
        LUMIN_CONFIG_PARAM_TABLE(SCHEMA_SCALAR_DEFAULT)
#undef SCHEMA_SCALAR_DEFAULT
    };
    EXPECT_EQ(schema_defaults[static_cast<std::size_t>(SysParam::FarWorldCaveField)],
              kDefaultFarWorldCaveFieldCode);
    for (const auto& [code, expected] : {std::pair{"0", FarCaveMode::BandLimited},
                                         std::pair{"-0.0", FarCaveMode::BandLimited},
                                         std::pair{"1", FarCaveMode::BoxFiltered},
                                         std::pair{"1.0", FarCaveMode::BoxFiltered}}) {
        const auto cfg = SystemConfig::FromJsonString(
            std::string(
                R"({"render":{"far_world_ladder":{"enabled":true,"params":{"cave_field":)") +
            code + "}}}}");
        EXPECT_TRUE(cfg.enabled(SysKey::RenderFarWorldLadder));
        EXPECT_EQ(ResolveFarWorldCaveField(cfg.param(SysParam::FarWorldCaveField, -1)), expected);
    }
}

TEST(SystemConfig, InvalidCaveCodesCannotRoundOrUnderflowToValidEnumCodes) {
    using namespace Luminumbra::Rendering;
    for (const auto code : {"-1",
                            "2",
                            "255",
                            "0.5",
                            "1.5",
                            "1.00000001",
                            "0.99999999",
                            "1e-46",
                            "-1e-46",
                            "1e39",
                            "-1e39"}) {
        SCOPED_TRACE(code);
        const auto cfg = SystemConfig::FromJsonString(
            std::string(
                R"({"render":{"far_world_ladder":{"enabled":true,"params":{"cave_field":)") +
            code + "}}}}");
        EXPECT_TRUE(cfg.enabled(SysKey::RenderFarWorldLadder));
        EXPECT_FALSE(ResolveFarWorldCaveField(
            cfg.param(SysParam::FarWorldCaveField, kDefaultFarWorldCaveFieldCode)));
    }
}

TEST(SystemConfig, ExplicitNonNumericCaveFieldsRefuseLadderWithoutChangingOtherParameters) {
    using namespace Luminumbra::Rendering;
    for (const auto value : {"true", "false", R"("BoxFiltered")", "null", "[]", "{}"}) {
        SCOPED_TRACE(value);
        const auto cfg = SystemConfig::FromJsonString(
            std::string(
                R"({"render":{"far_world_ladder":{"enabled":true,"params":{"cave_field":)") +
            value + R"(}},"moonlight":{"params":{"strength":)" + value + "}}}}");
        const auto requested = ResolveFarWorldCaveField(
            cfg.param(SysParam::FarWorldCaveField, kDefaultFarWorldCaveFieldCode));
        EXPECT_FALSE(requested); // Also selects startup's invalid-code warning.
        EXPECT_FLOAT_EQ(cfg.param(SysParam::MoonlightStrength, .75f), .75f);
        FarWorldRenderer renderer(RequestedFarWorldMode(cfg.enabled(SysKey::RenderFarWorldLadder)));
        renderer.SetRequestedCavePolicy(requested);
        const auto selection = renderer.Selection(FarWorldPurpose::World);
        EXPECT_EQ(selection.refusal, FarWorldRefusal::InvalidCavePolicy);
        EXPECT_EQ(selection.effective, FarWorldMode::Legacy);
        EXPECT_FALSE(renderer.CavePolicy(selection).effective);
        Luminumbra::JobSystem jobs;
        EXPECT_EQ(renderer.BindFixture(jobs, {}, 1, 1, FarWorldDevelopmentLimits()),
                  FarWorldRefusal::InvalidCavePolicy);
    }
    const auto absent = SystemConfig::FromJsonString(
        R"({"render":{"far_world_ladder":{"enabled":true,"params":{}}}})");
    EXPECT_EQ(ResolveFarWorldCaveField(
                  absent.param(SysParam::FarWorldCaveField, kDefaultFarWorldCaveFieldCode)),
              Luminumbra::World::FarCaveMode::BandLimited);
}

TEST(SystemConfig, CaveFieldIsRenderOnlyAndIgnoredByUserOverlay) {
    using namespace Luminumbra::Rendering;
    using Luminumbra::World::FarCaveMode;
    const auto sim = SystemConfig::FromJsonString(R"({"sim":{"erosion":{"enabled":true}}})");
    for (const auto enabled : {"false", "true"}) {
        for (const auto code : {"0", "1", "0.5"}) {
            auto cfg = SystemConfig::FromJsonString(
                std::string(
                    R"({"sim":{"erosion":{"enabled":true}},"render":{"far_world_ladder":{"enabled":)") +
                enabled + R"(,"params":{"cave_field":)" + code + "}}}}");
            EXPECT_EQ(cfg.ComputeConfigSubHash(), sim.ComputeConfigSubHash());
            cfg.OverlayUserFromJsonString(R"({"render":{"far_world_ladder":{
                "enabled":true,"params":{"cave_field":1}}}})");
            if (std::string(code) == "0")
                EXPECT_EQ(ResolveFarWorldCaveField(cfg.param(SysParam::FarWorldCaveField, -1)),
                          FarCaveMode::BandLimited);
            else if (std::string(code) == "1")
                EXPECT_EQ(ResolveFarWorldCaveField(cfg.param(SysParam::FarWorldCaveField, -1)),
                          FarCaveMode::BoxFiltered);
            else
                EXPECT_FALSE(ResolveFarWorldCaveField(cfg.param(SysParam::FarWorldCaveField, -1)));
            EXPECT_EQ(cfg.enabled(SysKey::RenderFarWorldLadder), std::string(enabled) == "true");
        }
    }
}

} // namespace
