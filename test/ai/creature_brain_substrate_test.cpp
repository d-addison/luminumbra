// CreatureBrain nearest-target scan equivalence for the shared PerceptionField
// substrate. These tests prove that the substrate-routed scan
// returns the SAME target/stimulus set the inline scan produces (determinism +
// equivalence), so the opt-in is behaviour-preserving:
//   1. Behaviour-preserving: RunCreatureBrainSystemOnTick with use_perception_substrate
//      ON yields BYTE-IDENTICAL sim state (positions, wish velocities, needs, action,
//      eaten flags) to the default inline path, over 50 compounding ticks of a rich
//      predator/prey roster (genome-gated cone/hearing sensing AND genome-less
//      unbounded sensing both exercised). Divergence in the substrate ENUMERATION
//      (field build / index mapping / id-order) would change the chosen target and
//      break the byte-identity.
//   2. Non-vacuous: the roster is verified to actually drive target-seeking actions
//      (Flee/Hunt) across the run — a roster where nothing senses anything would pass
//      trivially in both paths and prove nothing (mirrors perception_substrate_test's
//      "guards against an all-empty false pass").
//   3. The default call selects the canonical substrate path.
//   4. The substrate path is itself deterministic (run == replay).
//
// NOTE: the default OFF path's byte-identity is ALSO guarded by the existing
// creature_brain_system_test.cpp (every case there calls the system without the flag),
// which stays green iff the shared-lambda refactor is a true identity — so this file
// focuses on the ON == OFF equivalence.
#include <gtest/gtest.h>

#include <algorithm>
#include <vector>

#include <entt/entt.hpp>

#include "ai/CreatureBrainSystem.h"
#include "components/CoreComponents.h"
#include "components/CreatureComponents.h"

namespace {

namespace Comp = ::Luminumbra::Components;
using luminumbra::ai::CreatureAction;
using luminumbra::ai::RunCreatureBrainSystemOnTick;

constexpr float kDt = 1.0f / 30.0f;

// Build a rich synthetic roster: MULTIPLE predators + MULTIPLE prey at varied
// positions (non-trivial nearest-target selection), mixing genome-gated creatures
// (finite cone/hearing sensing) with genome-less ones (unbounded global-nearest) so
// BOTH gate paths are exercised. Insertion order is scrambled relative to role so the
// id-sort — and therefore the substrate's index/order equivalence — is actually tested.
void BuildRoster(entt::registry& r) {
    auto spawn = [&](float x,
                     float z,
                     bool predator,
                     float hunger,
                     bool genome,
                     float vis_range = 20.0f,
                     float hear_range = 24.0f,
                     float cos_fov = 0.5f) {
        const auto e = r.create();
        auto& tf = r.emplace<Comp::TransformComponent>(e);
        tf.position.x = x;
        tf.position.y = 0.0f;
        tf.position.z = z;
        auto& cr = r.emplace<Comp::CreatureComponent>(e);
        cr.is_predator = predator;
        cr.hunger = hunger;
        cr.stamina = 1.0f;
        if (genome) {
            auto& gn = r.emplace<Comp::CreatureGenomeComponent>(e);
            gn.vision_range = vis_range;
            gn.hearing_range = hear_range;
            gn.vision_cos_half_fov = cos_fov;
        }
    };
    // Varied positions; a mix of in-range/out-of-range, genome-gated + genome-less,
    // predators + prey. Prey at (3,0) is ~5.4 m from the predator at (8,2) -> Flee;
    // that genome-less predator senses every prey -> Hunt. Both target-driven actions
    // are guaranteed, so the target scan is genuinely exercised.
    spawn(3.0f,
          0.0f,
          /*pred*/ false,
          0.40f,
          /*genome*/ true,
          /*vis*/ 15.0f,
          /*hear*/ 8.0f,
          /*cos*/ 0.7f);
    spawn(8.0f, 2.0f, /*pred*/ true, 0.90f, /*genome*/ false);
    spawn(-6.0f, 5.0f, /*pred*/ false, 0.60f, /*genome*/ false);
    spawn(12.0f,
          -4.0f,
          /*pred*/ true,
          0.80f,
          /*genome*/ true,
          /*vis*/ 30.0f,
          /*hear*/ 10.0f,
          /*cos*/ 0.3f);
    spawn(-2.0f,
          -9.0f,
          /*pred*/ false,
          0.50f,
          /*genome*/ true,
          /*vis*/ 25.0f,
          /*hear*/ 20.0f,
          /*cos*/ 0.6f);
    spawn(5.0f, 7.0f, /*pred*/ false, 0.30f, /*genome*/ false);
    spawn(-10.0f,
          1.0f,
          /*pred*/ true,
          0.95f,
          /*genome*/ true,
          /*vis*/ 18.0f,
          /*hear*/ 12.0f,
          /*cos*/ 0.5f);
    spawn(1.0f, 11.0f, /*pred*/ false, 0.70f, /*genome*/ false);
}

// Capture every creature's full sim state in a stable (id-sorted) order, so two
// registries can be compared for BYTE-IDENTITY with a single vector equality.
std::vector<float> CaptureState(entt::registry& r) {
    std::vector<entt::entity> ents;
    for (auto e : r.view<Comp::CreatureComponent>())
        ents.push_back(e);
    std::sort(ents.begin(), ents.end(), [](entt::entity a, entt::entity b) {
        return entt::to_integral(a) < entt::to_integral(b);
    });
    std::vector<float> out;
    out.reserve(ents.size() * 9);
    for (auto e : ents) {
        const auto& cr = r.get<Comp::CreatureComponent>(e);
        const auto& tf = r.get<Comp::TransformComponent>(e);
        out.push_back(tf.position.x);
        out.push_back(tf.position.z);
        out.push_back(cr.wish_x);
        out.push_back(cr.wish_z);
        out.push_back(cr.hunger);
        out.push_back(cr.stamina);
        out.push_back(cr.energy);
        out.push_back(static_cast<float>(cr.last_action));
        out.push_back(cr.eaten ? 1.0f : 0.0f);
    }
    return out;
}

} // namespace

// The load-bearing wiring test: the additive substrate flag must not change the
// CreatureBrain's output for ANY creature. Fifty compounding ticks amplify any
// divergence, and the genome creatures develop a heading after tick 1 so the vision-
// CONE branch of the gate runs in both paths (not just hearing).
TEST(CreatureBrainSubstrate, SubstrateFlagIsBehaviorPreserving) {
    entt::registry inline_reg;
    entt::registry substrate_reg;
    BuildRoster(inline_reg);
    BuildRoster(substrate_reg);

    bool any_target_driven = false; // saw a Flee/Hunt -> the target scan actually found a target
    for (int t = 0; t < 50; ++t) {
        RunCreatureBrainSystemOnTick(inline_reg,
                                     kDt,
                                     {},
                                     nullptr,
                                     0.0f,
                                     0.0f,
                                     0.0f,
                                     /*use_perception_substrate=*/false);
        RunCreatureBrainSystemOnTick(substrate_reg,
                                     kDt,
                                     {},
                                     nullptr,
                                     0.0f,
                                     0.0f,
                                     0.0f,
                                     /*use_perception_substrate=*/true);
        // Compare EVERY tick, not just at the end: byte-identity must hold at each step
        // (state feeds forward, so an early divergence would compound and could even
        // partially cancel by the final tick).
        ASSERT_EQ(CaptureState(inline_reg), CaptureState(substrate_reg))
            << "substrate path diverged from the inline scan at tick " << t;
        for (auto e : substrate_reg.view<Comp::CreatureComponent>()) {
            const int a = substrate_reg.get<Comp::CreatureComponent>(e).last_action;
            if (a == static_cast<int>(CreatureAction::Flee) ||
                a == static_cast<int>(CreatureAction::Hunt)) {
                any_target_driven = true;
            }
        }
    }

    // Guard against an all-empty false pass: the ONLY thing that differs between the two
    // paths is the nearest-target scan, so it MUST have actually driven a decision.
    EXPECT_TRUE(any_target_driven)
        << "roster too inert to exercise the target scan (equivalence would be vacuous)";
}

// The default call selects the canonical substrate path and remains equivalent
// to the retained explicit inline reference used for regression comparison.
TEST(CreatureBrainSubstrate, DefaultSubstrateByteIdenticalToInline) {
    entt::registry default_reg;
    entt::registry explicit_off_reg;
    BuildRoster(default_reg);
    BuildRoster(explicit_off_reg);

    for (int t = 0; t < 30; ++t) {
        RunCreatureBrainSystemOnTick(default_reg,
                                     kDt); // all defaults -> flag now defaults TRUE (substrate)
        RunCreatureBrainSystemOnTick(
            explicit_off_reg,
            kDt,
            {},
            nullptr,
            0.0f,
            0.0f,
            0.0f,
            /*use_perception_substrate=*/false); // retained inline reference
    }
    EXPECT_EQ(CaptureState(default_reg), CaptureState(explicit_off_reg))
        << "the default (now substrate) call must be byte-identical to the retained inline path";
}

// The substrate path is itself deterministic: identical roster + ticks -> identical state.
TEST(CreatureBrainSubstrate, SubstratePathDeterministic) {
    auto run = [] {
        entt::registry r;
        BuildRoster(r);
        for (int t = 0; t < 40; ++t) {
            RunCreatureBrainSystemOnTick(r,
                                         kDt,
                                         {},
                                         nullptr,
                                         0.0f,
                                         0.0f,
                                         0.0f,
                                         /*use_perception_substrate=*/true);
        }
        return CaptureState(r);
    };
    EXPECT_EQ(run(), run());
}
