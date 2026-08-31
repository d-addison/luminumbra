#pragma once

//  "full control": resolve each per-system creature tuning from SystemConfig.
// Mirrors EcologyTuningConfig.h. Each Resolve* returns the COMPILED DEFAULTS when its sim key is
// OFF (the shipped baseline) -> byte-identical behaviour + empty config sub-hash. Only the
// client/server include this and feed the results into GameSession::Set*Tuning.

#include "CreatureReproductionSystem.h" // ReproductionTuning
#include "ForagingSystem.h"             // ForagingParams
#include "ScavengingSystem.h"           // ScavengingTuning
#include "ThirstSystem.h"               // ThirstTuning
#include "WildlifeFoliageSystem.h"      // WildlifeFoliageTuning
#include "core/SystemConfig.h"
#include "systems/PollinationSystem.h"

namespace luminumbra::ai {

[[nodiscard]] inline float ResolvePlantMutationRate(const core::SystemConfig& c) {
    constexpr float fallback = foliage::kPollinationMutationFrac;
    return c.enabled(core::SysKey::SimPlantGrowth)
               ? c.param(core::SysParam::PlantMutationRate, fallback)
               : fallback;
}

[[nodiscard]] inline WildlifeFoliageTuning
ResolveWildlifeFoliageTuning(const core::SystemConfig& c) {
    WildlifeFoliageTuning t;
    if (!c.enabled(core::SysKey::SimWildlifeFoliage))
        return t;
    using P = core::SysParam;
    t.graze_radius = c.param(P::WfGrazeRadius, t.graze_radius);
    t.graze_per_creature = c.param(P::WfGrazePerCreature, t.graze_per_creature);
    t.regrow_per_tick = c.param(P::WfRegrowPerTick, t.regrow_per_tick);
    t.feed_per_graze = c.param(P::WfFeedPerGraze, t.feed_per_graze);
    return t;
}

[[nodiscard]] inline ThirstTuning ResolveThirstTuning(const core::SystemConfig& c) {
    ThirstTuning t;
    if (!c.enabled(core::SysKey::SimThirst))
        return t;
    using P = core::SysParam;
    t.rise_rate = c.param(P::ThirstRiseRate, t.rise_rate);
    t.drink_rate = c.param(P::ThirstDrinkRate, t.drink_rate);
    t.seek_threshold = c.param(P::ThirstSeekThreshold, t.seek_threshold);
    return t;
}

[[nodiscard]] inline ScavengingTuning ResolveScavengingTuning(const core::SystemConfig& c) {
    ScavengingTuning t;
    if (!c.enabled(core::SysKey::SimScavenging))
        return t;
    using P = core::SysParam;
    t.hunger_threshold = c.param(P::ScavHungerThreshold, t.hunger_threshold);
    t.feed_radius = c.param(P::ScavFeedRadius, t.feed_radius);
    t.feed_rate = c.param(P::ScavFeedRate, t.feed_rate);
    return t;
}

[[nodiscard]] inline ForagingParams ResolveForagingTuning(const core::SystemConfig& c) {
    ForagingParams t; // defaults {1.0, 8.0, 1.0}
    if (!c.enabled(core::SysKey::SimForaging))
        return t;
    using P = core::SysParam;
    t.deposit = c.param(P::ForagingDeposit, static_cast<float>(t.deposit));
    t.trail_weight = c.param(P::ForagingTrailWeight, static_cast<float>(t.trail_weight));
    t.goal_weight = c.param(P::ForagingGoalWeight, static_cast<float>(t.goal_weight));
    return t;
}

[[nodiscard]] inline ReproductionTuning ResolveReproductionTuning(const core::SystemConfig& c) {
    ReproductionTuning t;
    if (!c.enabled(core::SysKey::SimReproduction))
        return t;
    using P = core::SysParam;
    t.maturity_ticks = static_cast<std::uint32_t>(
        c.param(P::ReproMaturityTicks, static_cast<float>(t.maturity_ticks)));
    t.cooldown_ticks = static_cast<std::uint32_t>(
        c.param(P::ReproCooldownTicks, static_cast<float>(t.cooldown_ticks)));
    t.courtship_ticks = static_cast<std::uint32_t>(
        c.param(P::ReproCourtshipTicks, static_cast<float>(t.courtship_ticks)));
    t.healthy_stamina = c.param(P::ReproHealthyStamina, t.healthy_stamina);
    t.mate_seek_radius = c.param(P::ReproMateSeekRadius, t.mate_seek_radius);
    t.courtship_radius = c.param(P::ReproCourtshipRadius, t.courtship_radius);
    t.spawn_radius = c.param(P::ReproSpawnRadius, t.spawn_radius);
    return t;
}

// render.circadian.amplitude (render-only; never hashed). 1.0 when OFF -> byte-identical.
[[nodiscard]] inline float ResolveCircadianAmplitude(const core::SystemConfig& c) {
    if (!c.enabled(core::SysKey::RenderCircadian))
        return 1.0f;
    return c.param(core::SysParam::CircadianAmplitude, 1.0f);
}

} // namespace luminumbra::ai
