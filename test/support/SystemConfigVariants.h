#pragma once

#include <cmath>
#include <cstddef>
#include <limits>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "luminumbra_common/core/SystemConfig.h"
#include "luminumbra_common/world/GameSession.h"

namespace Luminumbra::TestSupport {

// Change each input independently from the shipping defaults, including the circadian
// setting that is consumed by GameSession simulation despite its render namespace.
inline std::vector<std::pair<std::string, luminumbra::core::SystemConfig>>
SimulationConfigVariants() {
    using luminumbra::core::SysKey;
    using luminumbra::core::SystemConfig;
    constexpr const char* sections[]{
#define CONFIG_SECTION(ENUM, SECTION, JSON_SECTION, NAME, RESIDENCY) JSON_SECTION,
        LUMIN_CONFIG_KEY_TABLE(CONFIG_SECTION)
#undef CONFIG_SECTION
    };
    constexpr const char* names[]{
#define CONFIG_NAME(ENUM, SECTION, JSON_SECTION, NAME, RESIDENCY) NAME,
        LUMIN_CONFIG_KEY_TABLE(CONFIG_NAME)
#undef CONFIG_NAME
    };
    const auto relevant = [&sections](SysKey key) {
        return std::string(sections[static_cast<std::size_t>(key)]) == "sim" ||
               key == SysKey::RenderCircadian;
    };
    std::vector<std::pair<std::string, SystemConfig>> variants;
    for (unsigned i = 0; i < static_cast<unsigned>(SysKey::Count); ++i) {
        if (!relevant(static_cast<SysKey>(i)))
            continue;
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

// Exercise the live-session API rather than document identities. Tuning-only OFF
// flags may resolve to exactly the defaults and are deliberately omitted here.
inline std::vector<std::pair<std::string, std::string>> EffectiveIdentityMismatches() {
    world::GameSession baseline;
    const auto identity = baseline.SystemConfigIdentity();
    std::vector<std::pair<std::string, std::string>> variants;
    for (const auto& [name, config] : SimulationConfigVariants()) {
        world::GameSession session;
        session.ApplySystemConfig(config);
        auto changed = session.SystemConfigIdentity();
        if (changed != identity)
            variants.emplace_back(name, std::move(changed));
    }
    world::GameSession session;
    session.ApplySystemConfig(luminumbra::core::SystemConfig::Defaults());
    auto ecology = session.GetEcologyTuning();
    ecology.herd_weight = 0.9f;
    session.SetEcologyTuning(ecology);
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
    luminumbra::ai::ReproductionTuning reproduction;
    reproduction.maturity_ticks = std::numeric_limits<std::uint32_t>::max();
    session.SetReproductionTuning(reproduction);
    variants.emplace_back("u32 maximum setter", session.SystemConfigIdentity());
    --reproduction.maturity_ticks;
    session.SetReproductionTuning(reproduction);
    variants.emplace_back("u32 adjacent setter", session.SystemConfigIdentity());
    return variants;
}

} // namespace Luminumbra::TestSupport
