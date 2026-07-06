#pragma once

// I9-ECO: creature decision brain — turns sensed inputs (from PerceptionSystem: hunger,
// threat distance, food distance, stamina, role) into an ACTION via the Utility-AI (IAUS)
// arbiter. This is the "Utility AI is the primary arbiter" application: each candidate
// action's score is built from considerations over the senses, and the highest wins.
//
// DETERMINISM: pure function, libm-free (UtilityAI curves) -> safe on the sim path. The
// action SET is engine-generic predator/prey behaviour; the curve TUNING constants are
// externalised through CreatureBrainParams (spec-021 INSTINCT-12): per-species game data
// (species JSON "brain" overrides) may replace them, and the defaults reproduce the compiled
// constants byte-for-byte.

#include "UtilityAI.h"

#include <vector>

namespace luminumbra::ai {

// Normalized senses for one creature this tick (the PerceptionSystem fills these).
struct CreatureSenses {
    float hunger = 0.0f;            // 0 sated .. 1 starving
    float threat_proximity = 0.0f; // 0 none .. 1 predator adjacent
    float food_proximity = 0.0f;   // 0 none .. 1 food/prey adjacent
    float stamina = 1.0f;          // 0 exhausted .. 1 fresh
    float energy = 1.0f;           // 0 exhausted .. 1 rested (Spec 011: long-term sleep need)
    // Circadian activity in [0,1] (CircadianComponent.activity): 1 = peak active phase,
    // 0 = the creature's off-phase (night for diurnal, day for nocturnal). Defaults to 1.0 so
    // a creature with NO CircadianComponent is always "fully active" -> Sleep utility is 0 ->
    // it never sleeps and its decisions are byte-identical to before this field existed.
    float circadian_activity = 1.0f;
    bool is_predator = false;      // role
    // INSTINCT-05 (Wave H I2.4): the needs the arbiter was missing. All default to
    // values that score their actions at ZERO, so a creature without the matching
    // components decides byte-identically to the pre-INSTINCT-05 brain.
    float thirst = 0.0f;           // 0 quenched .. 1 parched (ThirstComponent)
    float water_proximity = 0.0f;  // 0 none known .. 1 at the water hole
    float food_availability = 0.0f; // 0 none known .. 1 abundant (the Forage seam)
};

// Canonical action ids (also the IAUS tie-break order).
enum class CreatureAction : int {
    Wander = 0,
    Graze = 1,   // prey: eat plants
    Flee = 2,    // prey: run from a predator
    Hunt = 3,    // predator: chase prey
    Rest = 4,    // recover stamina
    Sleep = 5,   // Spec 011: deep rest at the off-phase of the day (circadian-gated); recovers energy
    // INSTINCT-05 (append-only — the enum is the tie-break order; existing ids never move):
    Drink = 6,   // walk to / drink at the nearest water hole (thirst, IN the arbiter now)
    Forage = 7,  // seek food that is not adjacent (the food_availability seam)
};

namespace detail {
inline Consideration axis(float input, CurveType curve, float m = 1.0f, float b = 0.0f, float c = 0.0f) {
    Consideration k;
    k.input = input;
    k.curve = curve;
    k.m = m;
    k.b = b;
    k.c = c;
    return k;
}
}  // namespace detail

// Spec-021 INSTINCT-12: the per-species IAUS CURVE/WEIGHT overrides — the externalisation
// seam this header has always named ("the curve TUNING constants here are the natural seam to
// externalise to per-species game data"). Every field defaults to the constant that was
// previously an inline literal in DecideCreatureAction, so a default-constructed
// CreatureBrainParams reproduces today's decisions BYTE-IDENTICALLY (same float values through
// the same arithmetic; the EcologyTuning pattern). Species JSON
// (data/common/creatures/species/*.json, key "brain") overrides these per species via
// CreatureSpeciesRegistry; a JSON with the key absent — or matching these defaults — loads a
// table equal to the compiled constants.
struct CreatureBrainParams {
    float wander_weight   = 0.20f;  // constant baseline so an idle creature still moves
    float rest_weight     = 0.9f;   // recover when exhausted AND safe
    float sleep_weight    = 1.0f;   // circadian deep rest (kept under flee: never sleep through a predator)
    float hunt_weight     = 1.0f;   // predator: hungry AND prey nearby AND has stamina
    float flee_weight     = 1.1f;   // prey: a near predator dominates everything
    float graze_weight    = 1.0f;   // prey: hungry AND safe AND food nearby
    // Flee threat response curve (the one non-default curve shape in the action set):
    // Logistic(m = steepness, c = center) over threat_proximity.
    float flee_logistic_m = 2.0f;
    float flee_logistic_c = 0.45f;
    // INSTINCT-05: Drink sits UNDER Flee (1.1) so a threatened creature never
    // detours to water (the out-of-band blend's exact defect); Forage under Graze.
    float drink_weight  = 1.0f;
    float forage_weight = 0.8f;
};

// Build the IAUS action set for this creature, select the best, and return the action.
// `p` defaults to the compiled constants (CreatureBrainParams{}), so existing callers are
// byte-identical; per-species data may pass a species' overrides.
[[nodiscard]] inline CreatureAction DecideCreatureAction(const CreatureSenses& s,
                                                         const CreatureBrainParams& p = {}) {
    using detail::axis;
    std::vector<UtilityAction> actions;

    // Wander — a low constant baseline so a creature with nothing pressing still moves.
    actions.push_back({static_cast<int>(CreatureAction::Wander), p.wander_weight, {}});

    // Rest — recover when exhausted AND safe.
    actions.push_back({static_cast<int>(CreatureAction::Rest), p.rest_weight,
                       {axis(s.stamina, CurveType::InvLinear, 1.0f, 0.0f, 1.0f),     // low stamina -> high
                        axis(s.threat_proximity, CurveType::InvLinear, 1.0f, 0.0f, 1.0f)}});  // safe

    // Sleep — deep rest in the circadian OFF-phase when tired AND safe. The off-phase term
    // (InvLinear(activity)) is 0 at activity==1.0, so a creature with no CircadianComponent
    // (activity defaults to 1.0) scores Sleep at 0 and never sleeps -> byte-identical until a
    // creature is actually stamped circadian. Weight just under Flee (1.1) so a threat always
    // wins -- nothing sleeps through a predator.
    actions.push_back({static_cast<int>(CreatureAction::Sleep), p.sleep_weight,
                       {axis(s.circadian_activity, CurveType::InvLinear, 1.0f, 0.0f, 1.0f),  // off-phase -> high
                        axis(s.energy, CurveType::InvLinear, 1.0f, 0.0f, 1.0f),              // tired -> high
                        axis(s.threat_proximity, CurveType::InvLinear, 1.0f, 0.0f, 1.0f)}}); // safe

    // INSTINCT-05: Drink — parched AND water known AND safe (both roles thirst).
    // Every axis defaults to 0 in CreatureSenses, so a creature with no
    // ThirstComponent scores Drink 0 and decides byte-identically to before.
    actions.push_back({static_cast<int>(CreatureAction::Drink), p.drink_weight,
                       {axis(s.thirst, CurveType::Linear),
                        axis(s.water_proximity, CurveType::Linear),
                        axis(s.threat_proximity, CurveType::InvLinear, 1.0f, 0.0f, 1.0f)}});

    if (s.is_predator) {
        // Hunt — hungry AND prey nearby.
        actions.push_back({static_cast<int>(CreatureAction::Hunt), p.hunt_weight,
                           {axis(s.hunger, CurveType::Linear),
                            axis(s.food_proximity, CurveType::Linear),
                            axis(s.stamina, CurveType::Linear)}});  // need stamina to chase
    } else {
        // Flee — a near predator dominates everything (weight + steep curve).
        actions.push_back({static_cast<int>(CreatureAction::Flee), p.flee_weight,
                           {axis(s.threat_proximity, CurveType::Logistic,
                                 p.flee_logistic_m, 0.0f, p.flee_logistic_c)}});
        // Graze — hungry AND safe AND food nearby.
        actions.push_back({static_cast<int>(CreatureAction::Graze), p.graze_weight,
                           {axis(s.hunger, CurveType::Linear),
                            axis(s.threat_proximity, CurveType::InvLinear, 1.0f, 0.0f, 1.0f),
                            axis(s.food_proximity, CurveType::Linear)}});
        // INSTINCT-05: Forage — hungry AND food KNOWN-but-not-adjacent AND safe.
        // food_availability defaults 0 (nothing wires it yet), so Forage scores 0
        // and the action set is byte-identical until availability sensing lands —
        // this is the arbiter SEAM the charter asks for.
        actions.push_back({static_cast<int>(CreatureAction::Forage), p.forage_weight,
                           {axis(s.hunger, CurveType::Linear),
                            axis(s.food_availability, CurveType::Linear),
                            axis(s.threat_proximity, CurveType::InvLinear, 1.0f, 0.0f, 1.0f)}});
    }

    const int id = SelectAction(actions);
    return static_cast<CreatureAction>(id < 0 ? 0 : id);
}

}  // namespace luminumbra::ai
