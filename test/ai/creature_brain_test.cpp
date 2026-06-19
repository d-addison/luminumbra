// I9-ECO: creature brain (perception -> IAUS -> action) tests. Deterministic predator/prey
// behaviour emerging from the utility scores.
#include <gtest/gtest.h>

#include "ai/CreatureBrain.h"

namespace {

using luminumbra::ai::CreatureAction;
using luminumbra::ai::CreatureSenses;
using luminumbra::ai::DecideCreatureAction;

CreatureSenses prey(float hunger, float threat, float food, float stamina) {
    CreatureSenses s;
    s.is_predator = false;
    s.hunger = hunger;
    s.threat_proximity = threat;
    s.food_proximity = food;
    s.stamina = stamina;
    return s;
}

TEST(CreatureBrain, PreyFleesNearPredator) {
    // Even hungry with food next to it, a near predator -> flee.
    EXPECT_EQ(DecideCreatureAction(prey(/*hunger*/ 0.9f, /*threat*/ 0.95f, /*food*/ 0.9f, /*stamina*/ 0.8f)),
              CreatureAction::Flee);
}

TEST(CreatureBrain, PreyGrazesWhenSafeHungryFoodNear) {
    EXPECT_EQ(DecideCreatureAction(prey(/*hunger*/ 0.9f, /*threat*/ 0.05f, /*food*/ 0.9f, /*stamina*/ 0.9f)),
              CreatureAction::Graze);
}

TEST(CreatureBrain, PreyRestsWhenExhaustedAndSafe) {
    EXPECT_EQ(DecideCreatureAction(prey(/*hunger*/ 0.2f, /*threat*/ 0.05f, /*food*/ 0.1f, /*stamina*/ 0.03f)),
              CreatureAction::Rest);
}

TEST(CreatureBrain, PreyWandersWhenNothingPressing) {
    EXPECT_EQ(DecideCreatureAction(prey(/*hunger*/ 0.15f, /*threat*/ 0.05f, /*food*/ 0.1f, /*stamina*/ 0.9f)),
              CreatureAction::Wander);
}

TEST(CreatureBrain, PredatorHuntsWhenHungryAndPreyNear) {
    CreatureSenses s;
    s.is_predator = true;
    s.hunger = 0.8f;
    s.food_proximity = 0.85f;  // prey nearby
    s.threat_proximity = 0.0f;
    s.stamina = 0.9f;
    EXPECT_EQ(DecideCreatureAction(s), CreatureAction::Hunt);
}

TEST(CreatureBrain, Deterministic) {
    const CreatureSenses s = prey(0.6f, 0.4f, 0.5f, 0.7f);
    EXPECT_EQ(DecideCreatureAction(s), DecideCreatureAction(s));
}

}  // namespace
