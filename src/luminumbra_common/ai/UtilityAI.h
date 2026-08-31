#pragma once

//  Infinite Axis Utility System (IAUS) — the creature decision ARBITER (the
// research-directed move: Utility AI primary, GOAP demoted). An action's score is the
// product of its CONSIDERATIONS (each a normalized response curve over a game input:
// hunger, threat distance, stamina, ...) times a weight, with Dave Mark's compensation
// factor. The arbiter picks the highest-scoring action.
//
// DETERMINISM (this can feed creature decisions -> sim state -> world_hash): LIBM-FREE.
// Response curves use only +,-,*,/ (no exp/log/pow) so scoring is bit-stable across
// platforms; the "logistic" axis is a smoothstep sigmoid, not 1/(1+e^-x). Selection is a
// pure function with a canonical tie-break (lowest action id) so run==replay holds.
//
// Engine-generic: the curves + considerations are GAME DATA (per-species, per-action).

#include <cstdint>
#include <vector>

namespace luminumbra::ai {

// Clamp to [0,1]. NaN-safe: a NaN input floors to 0 (NOT through — both v<0 and v>1 are false
// for NaN, so the naive form would propagate NaN into UtilityAction::Score and make a NaN-scoring
// action silently unselectable). The leading !(v>0) catches NaN and negatives in one branch.
inline float utility_clamp01(float v) {
    return !(v > 0.0f) ? 0.0f : (v > 1.0f ? 1.0f : v);
}

// Response curve over a normalized input [0,1] -> [0,1].
enum class CurveType : std::uint8_t {
    Linear,    // clamp01(m*(x - c) + b)        — rises with input
    Quadratic, // clamp01(m*(x - c)^2 + b)      — U / hump around c (exponent fixed at 2)
    InvLinear, // clamp01(m*(c - x) + b)        — falls with input
    Logistic,  // libm-free smoothstep sigmoid centered at c, width ~1/m
};

struct Consideration {
    float input = 0.0f; // normalized game input [0,1]
    CurveType curve = CurveType::Linear;
    float m = 1.0f, b = 0.0f, c = 0.0f; // shape params

    [[nodiscard]] float Evaluate() const {
        const float x = utility_clamp01(input);
        switch (curve) {
            case CurveType::Linear:
                return utility_clamp01(m * (x - c) + b);
            case CurveType::InvLinear:
                return utility_clamp01(m * (c - x) + b);
            case CurveType::Quadratic: {
                const float d = x - c;
                return utility_clamp01(m * d * d + b);
            }
            case CurveType::Logistic: {
                const float w = (m <= 0.0f) ? 1.0f : (0.5f / m); // half-width
                const float t = utility_clamp01((x - (c - w)) / (2.0f * w));
                return utility_clamp01(t * t * (3.0f - 2.0f * t) + b); // smoothstep
            }
        }
        return 0.0f;
    }
};

struct UtilityAction {
    int id = 0;
    float weight = 1.0f;
    std::vector<Consideration> considerations;

    // IAUS score: weight * product(considerations) with Dave Mark's compensation factor
    // (counteracts the product shrinking as more considerations multiply in). Deterministic.
    [[nodiscard]] float Score() const {
        const int n = static_cast<int>(considerations.size());
        float score = utility_clamp01(weight);
        if (n == 0)
            return score;
        const float modFactor = 1.0f - (1.0f / static_cast<float>(n));
        for (const Consideration& con : considerations) {
            const float v = utility_clamp01(con.Evaluate());
            const float makeUp = (1.0f - v) * modFactor;
            score *= utility_clamp01(v + makeUp * v); // response*(1 + makeUp)
        }
        return utility_clamp01(score);
    }
};

// Select the highest-scoring action's id (the IAUS arbiter). Ties resolve to the LOWEST id
// (canonical) so run==replay holds. Returns -1 when there are no actions.
[[nodiscard]] inline int SelectAction(const std::vector<UtilityAction>& actions) {
    int bestId = -1;
    float bestScore = -1.0f;
    for (const UtilityAction& a : actions) {
        const float s = a.Score();
        if (bestId < 0 || s > bestScore || (s == bestScore && a.id < bestId)) {
            bestScore = s;
            bestId = a.id;
        }
    }
    return bestId;
}

} // namespace luminumbra::ai
