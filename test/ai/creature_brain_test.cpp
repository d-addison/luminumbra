//  creature brain (perception -> IAUS -> action) tests. Deterministic predator/prey
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
    EXPECT_EQ(DecideCreatureAction(
                  prey(/*hunger*/ 0.9f, /*threat*/ 0.95f, /*food*/ 0.9f, /*stamina*/ 0.8f)),
              CreatureAction::Flee);
}

TEST(CreatureBrain, PreyGrazesWhenSafeHungryFoodNear) {
    EXPECT_EQ(DecideCreatureAction(
                  prey(/*hunger*/ 0.9f, /*threat*/ 0.05f, /*food*/ 0.9f, /*stamina*/ 0.9f)),
              CreatureAction::Graze);
}

TEST(CreatureBrain, PreyRestsWhenExhaustedAndSafe) {
    EXPECT_EQ(DecideCreatureAction(
                  prey(/*hunger*/ 0.2f, /*threat*/ 0.05f, /*food*/ 0.1f, /*stamina*/ 0.03f)),
              CreatureAction::Rest);
}

TEST(CreatureBrain, PreyWandersWhenNothingPressing) {
    EXPECT_EQ(DecideCreatureAction(
                  prey(/*hunger*/ 0.15f, /*threat*/ 0.05f, /*food*/ 0.1f, /*stamina*/ 0.9f)),
              CreatureAction::Wander);
}

TEST(CreatureBrain, PredatorHuntsWhenHungryAndPreyNear) {
    CreatureSenses s;
    s.is_predator = true;
    s.hunger = 0.8f;
    s.food_proximity = 0.85f; // prey nearby
    s.threat_proximity = 0.0f;
    s.stamina = 0.9f;
    EXPECT_EQ(DecideCreatureAction(s), CreatureAction::Hunt);
}

TEST(CreatureBrain, Deterministic) {
    const CreatureSenses s = prey(0.6f, 0.4f, 0.5f, 0.7f);
    EXPECT_EQ(DecideCreatureAction(s), DecideCreatureAction(s));
}

// ---------------------------------------------------------------------------
//  ( needs arbitration): Drink/Forage join the IAUS arbiter.
// ---------------------------------------------------------------------------

TEST(CreatureBrain, DrinkWinsWhenParchedSafeNearWater) {
    CreatureSenses s = prey(/*hunger*/ 0.2f, /*threat*/ 0.05f, /*food*/ 0.1f, /*stamina*/ 0.9f);
    s.thirst = 0.95f;
    s.water_proximity = 0.9f;
    EXPECT_EQ(DecideCreatureAction(s), CreatureAction::Drink);
}

// THE ADVERSARIAL FIXTURE the contract names: a parched creature at the water's
// edge STILL flees a near predator — the whole point of moving thirst from the
// out-of-band additive blend (which pulled fleeing creatures toward water) into
// the arbiter, where Flee's weight + steep logistic dominate.
TEST(CreatureBrain, FleeStillDominatesDrink) {
    CreatureSenses s = prey(/*hunger*/ 0.2f, /*threat*/ 0.95f, /*food*/ 0.1f, /*stamina*/ 0.9f);
    s.thirst = 1.0f;
    s.water_proximity = 1.0f;
    EXPECT_EQ(DecideCreatureAction(s), CreatureAction::Flee);
}

TEST(CreatureBrain, PredatorDrinksToo) {
    CreatureSenses s;
    s.is_predator = true;
    s.hunger = 0.1f;         // not worth hunting
    s.food_proximity = 0.0f; // no prey anyway
    s.stamina = 0.9f;
    s.thirst = 0.9f;
    s.water_proximity = 0.85f;
    EXPECT_EQ(DecideCreatureAction(s), CreatureAction::Drink);
}

// The zero-defaults contract: senses WITHOUT thirst/water/availability decide
// exactly as the pre- brain (every earlier fixture above re-proves
// this; this one pins the pathological all-zero case to Wander, not Drink).
TEST(CreatureBrain, DefaultSensesNeverPickTheNewActions) {
    EXPECT_EQ(DecideCreatureAction(prey(0.15f, 0.05f, 0.1f, 0.9f)), CreatureAction::Wander);
}

} // namespace
