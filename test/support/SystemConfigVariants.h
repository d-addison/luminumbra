#pragma once

#include <cmath>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <limits>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include "UniqueTempPath.h"
#include "luminumbra_common/core/SystemConfig.h"
#include "luminumbra_common/world/GameSession.h"
#include "luminumbra_common/world/TerrainPresetLoader.h"

namespace Luminumbra::TestSupport {

inline nlohmann::json PresetIdentityFixture() {
    return nlohmann::json::parse(R"({"schema_rev":7,"generation_params":{
        "terrain":{"base_frequency":0.01,"base_amplitude":12.0,"octaves":4,
                   "persistence":0.5,"lacunarity":2.0,"height_offset":20.0},
        "features":{"caves_enabled":true,"cave_frequency":0.02}}})");
}

inline std::string PresetIdentityFixtureHash() {
    return world::TerrainPresetContentHash(PresetIdentityFixture());
}

// Hermetic runtime content for admission/replay regressions. Each caller owns
// its reserved root; filenames and bytes, never that root, enter the identity.
class SpeciesIdentityFixture {
public:
    SpeciesIdentityFixture()
        : storage_("species-identity-", ".json") {
        const auto preset_path = Root() / "worlds/atlas/presets/default.json";
        std::filesystem::create_directories(preset_path.parent_path());
        std::ofstream preset_file(preset_path, std::ios::binary);
        preset_file << Preset().dump();
        preset_file.close();
        EXPECT_TRUE(preset_file.good());
        Write("deer.json", OriginalBytes());
    }

    static nlohmann::json Preset() {
        auto preset = PresetIdentityFixture();
        preset["schema_rev"] = 6; // keep terrain-only work small; species always apply
        return preset;
    }

    static std::string OriginalBytes() {
        return R"({"id":"deer","brain":{"wander_weight":0.25},"genome_ranges":{"move_speed":[1,2]}})";
    }

    std::filesystem::path Root() const {
        return storage_.Path().parent_path();
    }

    std::filesystem::path Directory() const {
        return Root() / "data/common/creatures/species";
    }

    void Write(const std::filesystem::path& filename, const std::string& bytes) const {
        const auto path = Directory() / filename;
        std::filesystem::create_directories(path.parent_path());
        std::ofstream file(path, std::ios::binary | std::ios::trunc);
        file << bytes;
        file.close();
        EXPECT_TRUE(file.good());
    }

    world::TerrainPresetLoadResult Resolve() const {
        return world::LoadTerrainPresetFromJson(Preset(), Root() / "data");
    }

private:
    ScopedTempFile storage_;
};

// Expected effective inputs, defined from the application contract, never by
// asking the identity whether a mutation mattered. Tuning-only flags select
// compiled defaults, so only these five flags independently change the rules.
inline std::vector<std::pair<std::string, luminumbra::core::SystemConfig>>
SimulationConfigVariants() {
    using luminumbra::core::SysKey;
    using luminumbra::core::SystemConfig;
    static constexpr const char* sections[]{
#define CONFIG_SECTION(ENUM, SECTION, JSON_SECTION, NAME, RESIDENCY) JSON_SECTION,
        LUMIN_CONFIG_KEY_TABLE(CONFIG_SECTION)
#undef CONFIG_SECTION
    };
    constexpr const char* names[]{
#define CONFIG_NAME(ENUM, SECTION, JSON_SECTION, NAME, RESIDENCY) NAME,
        LUMIN_CONFIG_KEY_TABLE(CONFIG_NAME)
#undef CONFIG_NAME
    };
    const auto relevant = [](SysKey key) {
        return std::string(sections[static_cast<std::size_t>(key)]) == "sim" ||
               key == SysKey::RenderCircadian;
    };
    std::vector<std::pair<std::string, SystemConfig>> variants;
    constexpr SysKey gates[]{SysKey::SimHydrologyWeather,
                             SysKey::SimWeatherEvents,
                             SysKey::SimAetherState,
                             SysKey::SimWaterHighRes,
                             SysKey::SimActiveRegions};
    for (const auto gate : gates) {
        const auto i = static_cast<std::size_t>(gate);
        nlohmann::json document;
        document[sections[i]][names[i]]["enabled"] = false;
        variants.emplace_back(names[i], SystemConfig::FromJsonString(document.dump()));
    }
#define CONFIG_PARAM(ENUM, OWNER, NAME, VEC, DEFAULT, X, Y, Z)                                     \
    if (relevant(SysKey::OWNER)) {                                                                 \
        const auto owner = static_cast<std::size_t>(SysKey::OWNER);                                \
        nlohmann::json document;                                                                   \
        document[sections[owner]][names[owner]]["params"][NAME] =                                  \
            VEC ? nlohmann::json::array({X + 1.0f, Y, Z}) : nlohmann::json(DEFAULT + 1.0f);        \
        variants.emplace_back(NAME, SystemConfig::FromJsonString(document.dump()));                \
    }
    LUMIN_CONFIG_PARAM_TABLE(CONFIG_PARAM)
#undef CONFIG_PARAM
    return variants;
}

// Every expected mutation is returned and asserted, including omissions in the
// encoder. Never filter this list by its computed identity.
inline std::vector<std::pair<std::string, std::string>> EffectiveIdentityMismatches() {
    world::GameSession baseline;
    const auto identity = baseline.SystemConfigIdentity();
    std::vector<std::pair<std::string, std::string>> variants;
    for (const auto& [name, config] : SimulationConfigVariants()) {
        world::GameSession session;
        session.ApplySystemConfig(config);
        auto changed = session.SystemConfigIdentity();
        variants.emplace_back(name, std::move(changed));
    }
    world::GameSession session;
    session.ApplySystemConfig(luminumbra::core::SystemConfig::Defaults());
    auto ecology = session.GetEcologyTuning();
    ecology.herd_weight = 0.9f;
    session.SetEcologyTuning(ecology);
    variants.emplace_back("previous water scheduler rules",
                          world::GameSession{}.LegacyRevision3SystemConfigIdentity());
    variants.emplace_back("historical authority rules", "60ab3c242d54ce89");
    variants.emplace_back("previous authority rules", "49f2cb57730f3caf");
    variants.emplace_back("previous column ownership rules", "6a02c73e706e06fc");
    variants.emplace_back("ecology setter", session.SystemConfigIdentity());
    session.ApplySystemConfig(luminumbra::core::SystemConfig::Defaults());
    session.SetCircadianAmplitude(0.5f);
    variants.emplace_back("circadian setter", session.SystemConfigIdentity());
    session.ApplySystemConfig(luminumbra::core::SystemConfig::Defaults());
    session.SetWeatherEventsEnabled(false);
    variants.emplace_back("flag setter", session.SystemConfigIdentity());
    session.ApplySystemConfig(luminumbra::core::SystemConfig::Defaults());
    luminumbra::ai::ForagingParams foraging;
    foraging.deposit = std::nextafter(1.0, 2.0);
    session.SetForagingTuning(foraging);
    variants.emplace_back("binary64 setter", session.SystemConfigIdentity());
    session.ApplySystemConfig(luminumbra::core::SystemConfig::Defaults());
    session.SetFiniteHydrologyEnabled(false);
    variants.emplace_back("finite hydrology setter", session.SystemConfigIdentity());
    session.SetFiniteHydrologyEnabled(true);
    session.SetWeatherRainEnabled(true, 26);
    variants.emplace_back("weather rain scale setter", session.SystemConfigIdentity());
    session.SetWeatherRainEnabled(true, std::numeric_limits<std::int32_t>::min());
    variants.emplace_back("signed weather rain scale setter", session.SystemConfigIdentity());
    session.ApplySystemConfig(luminumbra::core::SystemConfig::Defaults());
    world::RegionSchedulerConfig scheduler;
    --scheduler.work_limit;
    session.SetRegionSchedulerConfig(scheduler);
    variants.emplace_back("region work limit setter", session.SystemConfigIdentity());
    scheduler = {};
    ++scheduler.hold_ticks;
    session.SetRegionSchedulerConfig(scheduler);
    variants.emplace_back("region hold ticks setter", session.SystemConfigIdentity());
    scheduler = {};
    ++scheduler.reduced_shift;
    session.SetRegionSchedulerConfig(scheduler);
    variants.emplace_back("region reduced shift setter", session.SystemConfigIdentity());
    session.SetRegionSchedulerConfig({});
    session.ApplySystemConfig(luminumbra::core::SystemConfig::Defaults());
    luminumbra::ai::ReproductionTuning reproduction;
    reproduction.maturity_ticks = std::numeric_limits<std::uint32_t>::max();
    session.SetReproductionTuning(reproduction);
    variants.emplace_back("u32 maximum setter", session.SystemConfigIdentity());
    --reproduction.maturity_ticks;
    session.SetReproductionTuning(reproduction);
    variants.emplace_back("u32 adjacent setter", session.SystemConfigIdentity());
    for (const auto& [name, changed] : variants) {
        EXPECT_NE(changed, identity) << "identity omitted applied input: " << name;
    }
    return variants;
}

} // namespace Luminumbra::TestSupport
