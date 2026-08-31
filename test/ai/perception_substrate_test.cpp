// the shared AI PerceptionField substrate. These tests are the
// proving signal that the spatially-bucketed shared scan returns the SAME
// neighbour/opportunity set the inline scans produce (determinism + equivalence):
//   1. Direct: PerceptionField.Query == an independent brute-force reference that
//      mirrors the InstinctSystem inline gather (3-D double Round4 distance,
//      per-source radius gate, id-ordered output) across hand-built + randomized
//      source sets — including positionless sources and a positionless perceiver.
//   2. System: RunInstinctSystemOnTick with the additive substrate flag ON yields
//      a byte-identical plan (checksum, candidates, action plan, stats) to the
//      default inline path, proving the opt-in is behaviour-preserving.

#include "gtest/gtest.h"

#include "luminumbra_common/ai/InstinctSystem.h"
#include "luminumbra_common/ai/PerceptionSubstrate.h"
#include "luminumbra_common/components/CoreComponents.h"
#include "luminumbra_common/components/InstinctComponents.h"

#include <cmath>
#include <cstdint>
#include <initializer_list>
#include <string>
#include <vector>

namespace {

using luminumbra::ai::PerceivedSource;
using luminumbra::ai::PerceptionField;
using luminumbra::ai::PerceptionQueryInput;
using luminumbra::ai::PerceptionSnapshot;
using luminumbra::ai::PerceptionSourceInput;

using Luminumbra::Components::ActionPlanComponent;
using Luminumbra::Components::InstinctAgentComponent;
using Luminumbra::Components::Need;
using Luminumbra::Components::NeedsComponent;
using Luminumbra::Components::OpportunityComponent;
using Luminumbra::Components::TransformComponent;

// ---- Independent brute-force reference (mirrors InstinctSystem.cpp exactly) ----
// Deliberately NOT reusing the substrate's own helpers so the distance value and
// gate are cross-checked, not assumed. `sources` are already in ordinal order.

double RefRound4(double value) {
    return std::round(value * 10000.0) / 10000.0;
}

std::vector<PerceivedSource> BruteForceGather(const PerceptionQueryInput& q,
                                              const std::vector<PerceptionSourceInput>& sources) {
    std::vector<PerceivedSource> out;
    for (const PerceptionSourceInput& s : sources) {
        double distance = 0.0;
        if (q.has_position && s.has_position) {
            const double dx = static_cast<double>(q.x) - static_cast<double>(s.x);
            const double dy = static_cast<double>(q.y) - static_cast<double>(s.y);
            const double dz = static_cast<double>(q.z) - static_cast<double>(s.z);
            distance = RefRound4(std::sqrt(dx * dx + dy * dy + dz * dz));
            if (s.radius > 0.0f && distance > static_cast<double>(s.radius)) {
                continue; // outside this source's influence radius
            }
        }
        out.push_back({s.index, distance});
    }
    return out; // already ascending-index (sources are in ordinal order)
}

void ExpectSnapshotEquals(const PerceptionSnapshot& got,
                          const std::vector<PerceivedSource>& expected) {
    ASSERT_EQ(got.perceived.size(), expected.size());
    for (std::size_t i = 0; i < expected.size(); ++i) {
        EXPECT_EQ(got.perceived[i].index, expected[i].index) << "at " << i;
        // Byte-identical distance: the substrate uses the identical Round4/sqrt.
        EXPECT_DOUBLE_EQ(got.perceived[i].distance, expected[i].distance) << "at " << i;
    }
}

PerceptionSourceInput
Src(std::uint32_t index, float x, float y, float z, float radius, bool has_position = true) {
    PerceptionSourceInput s;
    s.index = index;
    s.x = x;
    s.y = y;
    s.z = z;
    s.radius = radius;
    s.has_position = has_position;
    return s;
}

PerceptionQueryInput Query(float x, float y, float z, bool has_position = true) {
    PerceptionQueryInput q;
    q.x = x;
    q.y = y;
    q.z = z;
    q.has_position = has_position;
    return q;
}

// Deterministic, libm-free LCG so the randomized case is reproducible.
struct Lcg {
    std::uint64_t state = 0x9E3779B97F4A7C15ull;
    std::uint32_t next() {
        state = state * 6364136223846793005ull + 1442695040888963407ull;
        return static_cast<std::uint32_t>(state >> 33);
    }
    // A float in [lo, hi).
    float range(float lo, float hi) {
        const float t = static_cast<float>(next() & 0xFFFFFFu) / static_cast<float>(0x1000000u);
        return lo + t * (hi - lo);
    }
};

// ---- Test-registry builders (shared by the flag-on / flag-off comparison) ----

entt::entity AddAgent(entt::registry& r,
                      const std::string& actor,
                      bool with_transform,
                      float x = 0.0f,
                      float z = 0.0f) {
    const auto e = r.create();
    auto& agent = r.emplace<InstinctAgentComponent>(e);
    agent.actor_id = actor;
    agent.archetype = "test-archetype";
    agent.replan_interval_ticks = 5;
    auto& needs = r.emplace<NeedsComponent>(e);
    needs.needs = {Need{"alpha", 0.9f, 0.0f}, Need{"beta", 0.3f, 0.0f}};
    if (with_transform) {
        r.emplace<TransformComponent>(e).position = Luminumbra::Vec3(x, 0.0f, z);
    }
    return e;
}

entt::entity AddOpportunity(entt::registry& r,
                            const std::string& id,
                            const std::string& need,
                            float satisfaction,
                            float urgency,
                            bool with_transform,
                            float x,
                            float y,
                            float z,
                            float radius) {
    const auto e = r.create();
    auto& opp = r.emplace<OpportunityComponent>(e);
    opp.id = id;
    opp.action = "act-" + id;
    opp.target = "target-" + id;
    opp.need = need;
    opp.satisfaction = satisfaction;
    opp.urgency = urgency;
    opp.radius = radius;
    if (with_transform) {
        r.emplace<TransformComponent>(e).position = Luminumbra::Vec3(x, y, z);
    }
    return e;
}

struct BuiltWorld {
    entt::entity agent_at_origin = entt::null;
    entt::entity agent_no_transform = entt::null;
};

// Populate `r` deterministically. Insertion order is scrambled relative to id so
// the id-sort (and therefore the equivalence) is actually exercised.
BuiltWorld BuildWorld(entt::registry& r) {
    BuiltWorld w;
    w.agent_at_origin = AddAgent(r, "origin-actor", /*with_transform=*/true, 0.0f, 0.0f);
    w.agent_no_transform = AddAgent(r, "floating-actor", /*with_transform=*/false);

    // In-range gated, far gated (excluded for origin agent), ungated-but-far,
    // positionless (global), 3-D-only (Y offset), negative-coord in-range.
    AddOpportunity(r, "delta_far_gated", "alpha", 0.99f, 0.99f, true, 100.0f, 0.0f, 0.0f, 10.0f);
    AddOpportunity(r, "alpha_near_gated", "alpha", 0.50f, 0.50f, true, 3.0f, 0.0f, 4.0f, 10.0f);
    AddOpportunity(r, "charlie_global", "alpha", 0.40f, 0.40f, false, 0.0f, 0.0f, 0.0f, 0.0f);
    AddOpportunity(r, "echo_ungated_far", "beta", 0.70f, 0.30f, true, 500.0f, 0.0f, 0.0f, 0.0f);
    AddOpportunity(r, "bravo_high_y", "alpha", 0.60f, 0.60f, true, 0.0f, 9.0f, 0.0f, 10.0f);
    AddOpportunity(r, "foxtrot_neg", "beta", 0.55f, 0.45f, true, -6.0f, 0.0f, -6.0f, 12.0f);
    return w;
}

} // namespace

TEST(PerceptionSubstrate, MatchesInlineGatherHandBuilt) {
    // Mixed set: two gated (one in range, one out), one ungated positioned, one
    // positionless global, one gated only reachable via the 3-D metric.
    std::vector<PerceptionSourceInput> sources = {
        Src(0, 3.0f, 0.0f, 4.0f, 10.0f),       // dist 5 <= 10 -> in
        Src(1, 100.0f, 0.0f, 0.0f, 10.0f),     // dist 100 > 10 -> out
        Src(2, 500.0f, 0.0f, 0.0f, 0.0f),      // ungated -> always in
        Src(3, 0.0f, 0.0f, 0.0f, 0.0f, false), // positionless -> always in, dist 0
        Src(4, 6.0f, 8.0f, 0.0f, 11.0f),       // 3-D dist 10 <= 11 -> in (XZ dist 6 only)
        Src(5, 6.0f, 8.0f, 0.0f, 9.0f),        // 3-D dist 10 > 9 -> out (though XZ 6 < 9)
    };

    PerceptionField field;
    field.Build(sources);

    const PerceptionQueryInput q = Query(0.0f, 0.0f, 0.0f);
    PerceptionSnapshot got;
    field.Query(q, got);

    const std::vector<PerceivedSource> expected = BruteForceGather(q, sources);
    // Expect exactly {0, 2, 3, 4} in ascending index order.
    ASSERT_EQ(expected.size(), 4u);
    EXPECT_EQ(expected[0].index, 0u);
    EXPECT_EQ(expected[1].index, 2u);
    EXPECT_EQ(expected[2].index, 3u);
    EXPECT_EQ(expected[3].index, 4u);
    ExpectSnapshotEquals(got, expected);
}

TEST(PerceptionSubstrate, PositionlessPerceiverPerceivesEverySource) {
    std::vector<PerceptionSourceInput> sources = {
        Src(0, 3.0f, 0.0f, 4.0f, 1.0f),   // radius 1 would exclude at range...
        Src(1, 500.0f, 0.0f, 0.0f, 2.0f), //...but a positionless perceiver skips the gate
        Src(2, 0.0f, 0.0f, 0.0f, 0.0f, false),
    };
    PerceptionField field;
    field.Build(sources);

    const PerceptionQueryInput q = Query(0.0f, 0.0f, 0.0f, /*has_position=*/false);
    PerceptionSnapshot got;
    field.Query(q, got);

    const std::vector<PerceivedSource> expected = BruteForceGather(q, sources);
    ASSERT_EQ(expected.size(), 3u); // all three, all at distance 0
    for (const auto& p : expected)
        EXPECT_DOUBLE_EQ(p.distance, 0.0);
    ExpectSnapshotEquals(got, expected);
}

TEST(PerceptionSubstrate, GridPruneMatchesBruteForceRandomized) {
    // Many sources across a wide XZ span with varied radii (some 0 = ungated, a
    // few positionless), queried from many perceiver positions. The 3x3 bucket
    // query MUST still return every source the brute-force full scan accepts.
    Lcg rng;
    std::vector<PerceptionSourceInput> sources;
    const std::uint32_t kSourceCount = 400;
    sources.reserve(kSourceCount);
    for (std::uint32_t i = 0; i < kSourceCount; ++i) {
        const std::uint32_t roll = rng.next() % 10u;
        const bool positionless = (roll == 0u);          // ~10% global
        const bool ungated = (roll == 1u || roll == 2u); // ~20% ungated
        const float radius = ungated ? 0.0f : rng.range(0.5f, 25.0f);
        sources.push_back(Src(i,
                              rng.range(-200.0f, 200.0f),
                              rng.range(-5.0f, 5.0f),
                              rng.range(-200.0f, 200.0f),
                              radius,
                              !positionless));
    }

    PerceptionField field;
    field.Build(sources);

    PerceptionSnapshot got;
    for (int qi = 0; qi < 200; ++qi) {
        const PerceptionQueryInput q =
            Query(rng.range(-200.0f, 200.0f), rng.range(-5.0f, 5.0f), rng.range(-200.0f, 200.0f));
        field.Query(q, got);
        const std::vector<PerceivedSource> expected = BruteForceGather(q, sources);
        ExpectSnapshotEquals(got, expected);
    }
}

TEST(PerceptionSubstrate, QueryIsDeterministic) {
    std::vector<PerceptionSourceInput> sources = {
        Src(0, 3.0f, 0.0f, 4.0f, 10.0f),
        Src(1, -12.0f, 1.0f, 7.0f, 20.0f),
        Src(2, 0.0f, 0.0f, 0.0f, 0.0f, false),
    };
    PerceptionField field;
    field.Build(sources);
    const PerceptionQueryInput q = Query(1.0f, 0.0f, -2.0f);
    PerceptionSnapshot a, b;
    field.Query(q, a);
    field.Query(q, b);
    ASSERT_EQ(a.perceived.size(), b.perceived.size());
    for (std::size_t i = 0; i < a.perceived.size(); ++i) {
        EXPECT_EQ(a.perceived[i].index, b.perceived[i].index);
        EXPECT_DOUBLE_EQ(a.perceived[i].distance, b.perceived[i].distance);
    }
}

// The load-bearing wiring test: the additive substrate flag must not change the
// InstinctSystem's output for ANY agent (positioned or positionless perceiver).
TEST(PerceptionSubstrate, InstinctSystemSubstrateFlagIsBehaviorPreserving) {
    entt::registry inline_reg;
    entt::registry substrate_reg;
    const BuiltWorld inline_world = BuildWorld(inline_reg);
    const BuiltWorld substrate_world = BuildWorld(substrate_reg);
    // Same build order -> identical entity handles, so compare per-handle.
    ASSERT_EQ(inline_world.agent_at_origin, substrate_world.agent_at_origin);
    ASSERT_EQ(inline_world.agent_no_transform, substrate_world.agent_no_transform);

    const auto inline_stats = luminumbra::ai::RunInstinctSystemOnTick(
        inline_reg, /*tick=*/1, /*stimulus=*/nullptr, /*use_perception_substrate=*/false);
    const auto substrate_stats = luminumbra::ai::RunInstinctSystemOnTick(
        substrate_reg, /*tick=*/1, /*stimulus=*/nullptr, /*use_perception_substrate=*/true);

    EXPECT_EQ(inline_stats.agents_seen, substrate_stats.agents_seen);
    EXPECT_EQ(inline_stats.agents_replanned, substrate_stats.agents_replanned);
    EXPECT_EQ(inline_stats.opportunities_considered, substrate_stats.opportunities_considered);
    // Sanity: the origin agent excludes the far gated opportunity; the floating
    // agent perceives everything -> the two agents differ, so the counts are
    // non-trivial (guards against an all-empty false pass).
    EXPECT_GT(substrate_stats.opportunities_considered, 0u);

    for (const entt::entity agent :
         {inline_world.agent_at_origin, inline_world.agent_no_transform}) {
        const auto& inline_agent = inline_reg.get<InstinctAgentComponent>(agent);
        const auto& substrate_agent = substrate_reg.get<InstinctAgentComponent>(agent);

        const auto& inline_plan = inline_agent.current_plan;
        const auto& substrate_plan = substrate_agent.current_plan;
        EXPECT_EQ(inline_plan.checksum, substrate_plan.checksum) << inline_agent.actor_id;
        EXPECT_EQ(inline_plan.selected_index, substrate_plan.selected_index);
        ASSERT_EQ(inline_plan.candidates.size(), substrate_plan.candidates.size())
            << inline_agent.actor_id;
        for (std::size_t i = 0; i < inline_plan.candidates.size(); ++i) {
            const auto& a = inline_plan.candidates[i];
            const auto& b = substrate_plan.candidates[i];
            EXPECT_EQ(a.id, b.id) << "candidate " << i;
            EXPECT_EQ(a.rank, b.rank);
            EXPECT_DOUBLE_EQ(a.score, b.score) << "candidate " << a.id;
        }

        // The winning action + resolved target entity must match too.
        const bool inline_has_plan = inline_reg.all_of<ActionPlanComponent>(agent);
        const bool substrate_has_plan = substrate_reg.all_of<ActionPlanComponent>(agent);
        ASSERT_EQ(inline_has_plan, substrate_has_plan);
        if (inline_has_plan) {
            const auto& ip = inline_reg.get<ActionPlanComponent>(agent).plan;
            const auto& sp = substrate_reg.get<ActionPlanComponent>(agent).plan;
            ASSERT_EQ(ip.size(), sp.size());
            for (std::size_t i = 0; i < ip.size(); ++i) {
                EXPECT_EQ(ip[i].name, sp[i].name);
                EXPECT_EQ(ip[i].target, sp[i].target);
            }
        }
    }
}
