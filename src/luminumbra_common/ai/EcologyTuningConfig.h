#pragma once

//  resolve the data-driven creature-brain tuning from SystemConfig.
// Kept in its own header so CreatureBrainSystem.h need not depend on core/SystemConfig.h —
// only the client/server (which own the SystemConfig) include this and feed the result into
// GameSession::SetEcologyTuning. When sim.ecology is OFF (the default) this returns the compiled
// defaults, so behaviour — and the config sub-hash — are byte-identical to before.

#include "CreatureBrainSystem.h" // EcologyTuning + the k* defaults
#include "core/SystemConfig.h"

namespace luminumbra::ai {

[[nodiscard]] inline EcologyTuning ResolveEcologyTuning(const core::SystemConfig& cfg) {
    EcologyTuning t; // field defaults == CreatureBrainSystem.h constants
    if (!cfg.enabled(core::SysKey::SimEcology)) {
        return t; // sim.ecology OFF -> compiled defaults (byte-identical; hash stays empty)
    }
    using P = core::SysParam;
    t.energy_drain_per_second = cfg.param(P::EcoEnergyDrain, t.energy_drain_per_second);
    t.energy_rest_recover = cfg.param(P::EcoEnergyRestRecover, t.energy_rest_recover);
    t.energy_sleep_recover = cfg.param(P::EcoEnergySleepRecover, t.energy_sleep_recover);
    t.hunger_growth_per_second = cfg.param(P::EcoHungerGrowth, t.hunger_growth_per_second);
    t.hunger_graze_sate = cfg.param(P::EcoHungerGrazeSate, t.hunger_graze_sate);
    t.stamina_rest_recover = cfg.param(P::EcoStaminaRestRecover, t.stamina_rest_recover);
    t.stamina_move_drain = cfg.param(P::EcoStaminaMoveDrain, t.stamina_move_drain);
    t.herd_weight = cfg.param(P::EcoHerdWeight, t.herd_weight);
    t.alignment_weight = cfg.param(P::EcoAlignmentWeight, t.alignment_weight);
    t.catch_radius = cfg.param(P::EcoCatchRadius, t.catch_radius);
    t.catch_satiation = cfg.param(P::EcoCatchSatiation, t.catch_satiation);
    t.flock_neighbor_radius = cfg.param(P::EcoFlockNeighborRadius, t.flock_neighbor_radius);
    t.flock_separation_radius = cfg.param(P::EcoFlockSeparationRadius, t.flock_separation_radius);
    t.flock_cohesion_weight = cfg.param(P::EcoFlockCohesionWeight, t.flock_cohesion_weight);
    t.flock_separation_weight = cfg.param(P::EcoFlockSeparationWeight, t.flock_separation_weight);
    return t;
}

} // namespace luminumbra::ai
