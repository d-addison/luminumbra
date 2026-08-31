//  Utility-AI (IAUS) arbiter tests. The decision system is a pure, libm-free,
// deterministic function of the considerations -> safe on the sim path.
#include <gtest/gtest.h>

#include <vector>

#include "ai/UtilityAI.h"

namespace {

using luminumbra::ai::Consideration;
using luminumbra::ai::CurveType;
using luminumbra::ai::SelectAction;
using luminumbra::ai::UtilityAction;

Consideration con(float input, CurveType curve, float m = 1.0f, float b = 0.0f, float c = 0.0f) {
    Consideration k;
    k.input = input;
    k.curve = curve;
    k.m = m;
    k.b = b;
    k.c = c;
    return k;
}

// Every curve stays in [0,1] across the input range and is deterministic (same in == same out).
TEST(UtilityAI, CurvesBoundedAndDeterministic) {
    const CurveType curves[] = {
        CurveType::Linear, CurveType::InvLinear, CurveType::Quadratic, CurveType::Logistic};
    for (CurveType cv : curves) {
        for (int i = 0; i <= 10; ++i) {
            const float x = static_cast<float>(i) / 10.0f;
            const float a = con(x, cv, 1.2f, 0.05f, 0.5f).Evaluate();
            const float b = con(x, cv, 1.2f, 0.05f, 0.5f).Evaluate();
            EXPECT_EQ(a, b) << "curve not deterministic";
            EXPECT_GE(a, 0.0f);
            EXPECT_LE(a, 1.0f);
        }
    }
}

// Headline: the IAUS arbiter picks the higher-utility action for the situation.
TEST(UtilityAI, PicksHigherUtilityAction) {
    auto build = [](float threat, float hunger) {
        UtilityAction flee;
        flee.id = 1;
        flee.weight = 1.0f;
        flee.considerations = {con(threat, CurveType::Linear)}; // flee when threatened

        UtilityAction graze;
        graze.id = 2;
        graze.weight = 1.0f;
        graze.considerations = {
            con(hunger, CurveType::Linear),
            con(threat, CurveType::InvLinear, 1.0f, 0.0f, 1.0f)}; // safe = 1 - threat
        return std::vector<UtilityAction>{flee, graze};
    };

    // Predator near -> flee dominates (graze's safety axis collapses it).
    EXPECT_EQ(SelectAction(build(/*threat=*/0.9f, /*hunger=*/0.8f)), 1);
    // Safe + hungry -> graze dominates (flee's threat axis is low).
    EXPECT_EQ(SelectAction(build(/*threat=*/0.1f, /*hunger=*/0.9f)), 2);
}

// A tie resolves to the lowest action id (canonical -> run==replay holds).
TEST(UtilityAI, TieResolvesToLowestId) {
    UtilityAction a;
    a.id = 5;
    a.weight = 0.5f;
    UtilityAction b;
    b.id = 2;
    b.weight = 0.5f; // identical score
    EXPECT_EQ(SelectAction({a, b}), 2);
    EXPECT_EQ(SelectAction({b, a}), 2); // order-independent
}

// With equal considerations, the higher-weight action wins.
TEST(UtilityAI, HigherWeightWins) {
    UtilityAction lo;
    lo.id = 1;
    lo.weight = 0.3f;
    lo.considerations = {con(0.8f, CurveType::Linear)};
    UtilityAction hi;
    hi.id = 2;
    hi.weight = 0.9f;
    hi.considerations = {con(0.8f, CurveType::Linear)};
    EXPECT_EQ(SelectAction({lo, hi}), 2);
}

// Compensation keeps a multi-consideration action viable (the product doesn't collapse to ~0
// just because it has several high considerations).
TEST(UtilityAI, CompensationKeepsMultiConsiderationViable) {
    UtilityAction many;
    many.id = 1;
    many.weight = 1.0f;
    many.considerations = {con(0.8f, CurveType::Linear),
                           con(0.8f, CurveType::Linear),
                           con(0.8f, CurveType::Linear),
                           con(0.8f, CurveType::Linear)};
    EXPECT_GT(many.Score(), 0.5f) << "compensation factor failed; product collapsed";
    EXPECT_LE(many.Score(), 1.0f);
}

TEST(UtilityAI, EmptyReturnsNegativeOne) {
    EXPECT_EQ(SelectAction({}), -1);
}

} // namespace
