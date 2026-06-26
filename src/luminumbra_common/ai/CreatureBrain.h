#pragma once

// I9-ECO: creature decision brain — turns sensed inputs (from PerceptionSystem: hunger,
// threat distance, food distance, stamina, role) into an ACTION via the Utility-AI (IAUS)
// arbiter. This is the "Utility AI is the primary arbiter" application: each candidate
// action's score is built from considerations over the senses, and the highest wins.
//
// DETERMINISM: pure function, libm-free (UtilityAI curves) -> safe on the sim path. The
// action SET is engine-generic predator/prey behaviour; the curve TUNING constants here are
// the natural seam to externalise to per-species game data (archetypes) later.

#include "UtilityAI.h"

#include <vector>

namespace luminumbra::ai {

// Normalized senses for one creature this tick (the PerceptionSystem fills these).
struct CreatureSenses {
    float hunger = 0.0f;            // 0 sated .. 1 starving
    float threat_proximity = 0.0f; // 0 none .. 1 predator adjacent
    float food_proximity = 0.0f;   // 0 none .. 1 food/prey adjacent
    float stamina = 1.0f;          // 0 exhausted .. 1 fresh
    float energy = 1.0f;           // 0 exhausted .. 1 rested (Spec 011: long-term sleep need;
                                   // plumbed now, consumed by Sleep utility in Phase E/F)
    bool is_predator = false;      // role
};

// Canonical action ids (also the IAUS tie-break order).
enum class CreatureAction : int {
    Wander = 0,
    Graze = 1,   // prey: eat plants
    Flee = 2,    // prey: run from a predator
    Hunt = 3,    // predator: chase prey
    Rest = 4,    // recover stamina
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

// Build the IAUS action set for this creature, select the best, and return the action.
[[nodiscard]] inline CreatureAction DecideCreatureAction(const CreatureSenses& s) {
    using detail::axis;
    std::vector<UtilityAction> actions;

    // Wander — a low constant baseline so a creature with nothing pressing still moves.
    actions.push_back({static_cast<int>(CreatureAction::Wander), 0.20f, {}});

    // Rest — recover when exhausted AND safe.
    actions.push_back({static_cast<int>(CreatureAction::Rest), 0.9f,
                       {axis(s.stamina, CurveType::InvLinear, 1.0f, 0.0f, 1.0f),     // low stamina -> high
                        axis(s.threat_proximity, CurveType::InvLinear, 1.0f, 0.0f, 1.0f)}});  // safe

    if (s.is_predator) {
        // Hunt — hungry AND prey nearby.
        actions.push_back({static_cast<int>(CreatureAction::Hunt), 1.0f,
                           {axis(s.hunger, CurveType::Linear),
                            axis(s.food_proximity, CurveType::Linear),
                            axis(s.stamina, CurveType::Linear)}});  // need stamina to chase
    } else {
        // Flee — a near predator dominates everything (weight + steep curve).
        actions.push_back({static_cast<int>(CreatureAction::Flee), 1.1f,
                           {axis(s.threat_proximity, CurveType::Logistic, 2.0f, 0.0f, 0.45f)}});
        // Graze — hungry AND safe AND food nearby.
        actions.push_back({static_cast<int>(CreatureAction::Graze), 1.0f,
                           {axis(s.hunger, CurveType::Linear),
                            axis(s.threat_proximity, CurveType::InvLinear, 1.0f, 0.0f, 1.0f),
                            axis(s.food_proximity, CurveType::Linear)}});
    }

    const int id = SelectAction(actions);
    return static_cast<CreatureAction>(id < 0 ? 0 : id);
}

}  // namespace luminumbra::ai
