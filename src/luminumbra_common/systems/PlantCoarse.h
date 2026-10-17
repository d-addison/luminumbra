#pragma once

#include "PlantGrowthSystem.h"

#include <limits>
#include <stdexcept>
#include <utility>

namespace luminumbra::foliage {
// Exact k single growth ticks for a fixed sampled environment and infection.
// The caller samples once at the due tick; varying-environment trajectories are
// qualified separately. Tending is consumed tick-by-tick (at most 255 iterations).
// The constant remainder is bulk integer addition; a potential unsigned wrap
// falls back to bounded single steps, preserving early stage crossings exactly.
inline std::uint32_t AdvancePlantGrowthCoarse(Comp::PlantGrowthComponent& growth,
                                              const Comp::PlantGenomeComponent& genome,
                                              const PlantEnvSample& env,
                                              std::uint16_t infection,
                                              std::uint32_t elapsed,
                                              std::uint64_t current_tick) {
    if (elapsed > 36000 || infection > Comp::kDiseaseUnitScale)
        throw std::invalid_argument("Invalid coarse plant window");
    if (elapsed == 0)
        return 0;
    const auto ph = ExpressGenome(genome);
    const auto base = Suitability(ph, env);
    const auto multiplier =
        1.0f - kBlightSuitPenalty *
                   (static_cast<float>(infection) / static_cast<float>(Comp::kDiseaseUnitScale));
    std::uint32_t advanced = 0;
    const auto update_stage = [&] {
        while (growth.stage + 1u < Comp::kPlantStageCount &&
               growth.growth_points >= StageThreshold(growth.stage)) {
            ++growth.stage;
            ++advanced;
        }
    };
    const auto amounts = [&](std::uint8_t tended) {
        const float bonus = static_cast<float>(tended) * (1.0f / 255.0f) * 0.45f;
        const float suit = clamp01(clamp01(base + bonus) * multiplier);
        return std::pair{static_cast<std::uint32_t>(ph.growth_per_tick * suit),
                         static_cast<std::uint32_t>((1.0f - suit) * 6.0f)};
    };
    while (elapsed && growth.tended) {
        const auto [inc, stress] = amounts(growth.tended);
        --growth.tended;
        growth.growth_points += inc;
        growth.stress_points += stress;
        update_stage();
        --elapsed;
    }
    if (elapsed) {
        const auto [inc, stress] = amounts(0);
        const auto growth_total = static_cast<std::uint64_t>(inc) * elapsed;
        const auto stress_total = static_cast<std::uint64_t>(stress) * elapsed;
        constexpr auto limit = std::numeric_limits<std::uint32_t>::max();
        if (growth_total <= limit - growth.growth_points &&
            stress_total <= limit - growth.stress_points) {
            growth.growth_points += static_cast<std::uint32_t>(growth_total);
            growth.stress_points += static_cast<std::uint32_t>(stress_total);
            update_stage();
        } else {
            for (std::uint32_t i = 0; i < elapsed; ++i) {
                growth.growth_points += inc;
                growth.stress_points += stress;
                update_stage();
            }
        }
    }
    const int quality = 100 - static_cast<int>(growth.stress_points / 50u);
    growth.quality = static_cast<std::uint8_t>(quality < 0 ? 0 : quality);
    growth.last_tick = current_tick;
    return advanced;
}
} // namespace luminumbra::foliage
