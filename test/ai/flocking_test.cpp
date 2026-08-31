//  the pure herd-flocking steering helper — cohesion toward the group centroid,
// separation away from crowding, libm-free + order-independent (run==replay).
#include <gtest/gtest.h>

#include <cmath>
#include <utility>
#include <vector>

#include "ai/Flocking.h"

namespace {

using luminumbra::ai::ComputeFlockSteer;
using luminumbra::ai::FlockParams;
using luminumbra::ai::FlockSteer;

// No neighbours -> no steer.
TEST(Flocking, NoNeighborsIsZero) {
    const FlockSteer s = ComputeFlockSteer(0.0f, 0.0f, {});
    EXPECT_FLOAT_EQ(s.x, 0.0f);
    EXPECT_FLOAT_EQ(s.z, 0.0f);
}

// Cohesion: a cluster off to +x (but beyond the separation radius) pulls the loner toward +x.
TEST(Flocking, CohesionPullsTowardGroup) {
    std::vector<std::pair<float, float>> n = {{8.0f, 0.0f}, {9.0f, 1.0f}, {9.0f, -1.0f}};
    const FlockSteer s = ComputeFlockSteer(0.0f, 0.0f, n);
    EXPECT_GT(s.x, 0.0f) << "should be pulled toward the group on +x";
    EXPECT_NEAR(s.z, 0.0f, 0.2f) << "group is symmetric on z";
}

// Separation dominates up close: a single neighbour right on top pushes the creature away,
// overriding the cohesion pull toward it.
TEST(Flocking, SeparationPushesOffCrowding) {
    std::vector<std::pair<float, float>> n = {{1.0f, 0.0f}}; // inside separation_radius (3)
    const FlockSteer s = ComputeFlockSteer(0.0f, 0.0f, n);
    EXPECT_LT(s.x, 0.0f) << "a too-close neighbour on +x should push toward -x";
}

// Order independence: shuffling the neighbour list yields the identical steer (run==replay).
TEST(Flocking, OrderIndependent) {
    std::vector<std::pair<float, float>> a = {{8.0f, 0.0f}, {2.0f, 1.0f}, {-5.0f, 4.0f}};
    std::vector<std::pair<float, float>> b = {{-5.0f, 4.0f}, {8.0f, 0.0f}, {2.0f, 1.0f}};
    const FlockSteer sa = ComputeFlockSteer(0.5f, 0.5f, a);
    const FlockSteer sb = ComputeFlockSteer(0.5f, 0.5f, b);
    EXPECT_FLOAT_EQ(sa.x, sb.x);
    EXPECT_FLOAT_EQ(sa.z, sb.z);
}

// Far-apart neighbours beyond the cohesion radius contribute nothing.
TEST(Flocking, BeyondRadiusIgnored) {
    FlockParams p;
    p.neighbor_radius = 5.0f;
    p.separation_radius = 2.0f;
    std::vector<std::pair<float, float>> n = {{100.0f, 0.0f}};
    const FlockSteer s = ComputeFlockSteer(0.0f, 0.0f, n, p);
    EXPECT_FLOAT_EQ(s.x, 0.0f);
    EXPECT_FLOAT_EQ(s.z, 0.0f);
}

// Alignment OFF (default weight 0) is byte-identical whether or not headings are passed — so the
// CreatureBrainSystem call that now always passes headings stays exact vs the old positions-only
// steer (canonical roster + 1v1 goldens unaffected).
TEST(Flocking, AlignmentOffIsByteIdenticalToNoHeadings) {
    std::vector<std::pair<float, float>> n = {{8.0f, 0.0f}, {2.0f, 1.0f}, {-5.0f, 4.0f}};
    std::vector<std::pair<float, float>> headings = {{0.0f, 1.0f}, {0.0f, 1.0f}, {0.0f, 1.0f}};
    const FlockSteer base = ComputeFlockSteer(0.5f, 0.5f, n); // positions-only
    FlockParams p;                                            // alignment_weight == 0
    const FlockSteer withH = ComputeFlockSteer(0.5f, 0.5f, n, p, &headings);
    EXPECT_FLOAT_EQ(base.x, withH.x);
    EXPECT_FLOAT_EQ(base.z, withH.z);
}

// Alignment ON: neighbours all heading +z make the steer gain a +z component vs the
// alignment-off baseline (the 3rd Reynolds term pulls toward the group's mean heading).
TEST(Flocking, AlignmentMatchesMeanHeading) {
    std::vector<std::pair<float, float>> n = {{8.0f, 0.0f}, {9.0f, 1.0f}, {9.0f, -1.0f}};
    std::vector<std::pair<float, float>> headings = {{0.0f, 2.0f}, {0.0f, 3.0f}, {0.0f, 2.5f}};
    const FlockSteer off = ComputeFlockSteer(0.0f, 0.0f, n);
    FlockParams p;
    p.alignment_weight = 1.0f;
    const FlockSteer on = ComputeFlockSteer(0.0f, 0.0f, n, p, &headings);
    EXPECT_GT(on.z, off.z + 0.5f) << "alignment should bias the steer toward the +z mean heading";
}

//  CONVERGENCE ( acceptance: "a herd converges to a common heading"). This is the
// dynamic the CreatureBrainSystem now enables by tuning kAlignmentWeight on (0.5). We isolate the
// alignment term (cohesion/separation weights 0) over a tight static cluster and iterate each
// agent's heading toward the alignment steer: with alignment ON the differently-headed agents
// converge to a common direction (concentration R -> ~1); with alignment OFF the headings never
// change (R stays at its spread-out initial value). Deterministic (fixed-point reduction).
TEST(Flocking, AlignmentConvergesHeadingsOverIterations) {
    // 5 agents in a cluster, all within the 12 m neighbour radius of each other.
    const std::vector<std::pair<float, float>> pos = {{0, 0}, {4, 0}, {0, 4}, {4, 4}, {2, 6}};
    // Initial unit headings spread around the circle (low concentration).
    const std::vector<std::pair<float, float>> head0 = {
        {1, 0}, {0, 1}, {-1, 0}, {0, -1}, {0.70710678f, 0.70710678f}};

    // Concentration R = |mean of unit headings| in [0,1] (1 = perfectly aligned, 0 = uniform
    // spread).
    auto concentration = [](const std::vector<std::pair<float, float>>& h) {
        float sx = 0.0f, sz = 0.0f;
        for (const auto& v : h) {
            const float m = std::sqrt(v.first * v.first + v.second * v.second);
            if (m > 1e-6f) {
                sx += v.first / m;
                sz += v.second / m;
            }
        }
        return std::sqrt(sx * sx + sz * sz) / static_cast<float>(h.size());
    };

    // Iterate: each agent rotates its heading toward the alignment steer over its neighbours.
    auto settle = [&](float align_w) {
        std::vector<std::pair<float, float>> h = head0;
        for (int it = 0; it < 40; ++it) {
            std::vector<std::pair<float, float>> next = h;
            for (std::size_t i = 0; i < pos.size(); ++i) {
                std::vector<std::pair<float, float>> np, nh;
                for (std::size_t j = 0; j < pos.size(); ++j) {
                    if (j == i)
                        continue;
                    np.push_back(pos[j]);
                    nh.push_back(h[j]);
                }
                FlockParams p;
                p.cohesion_weight = 0.0f; // isolate alignment
                p.separation_weight = 0.0f;
                p.alignment_weight = align_w;
                const FlockSteer s = ComputeFlockSteer(pos[i].first, pos[i].second, np, p, &nh);
                const float bx = h[i].first + s.x, bz = h[i].second + s.z;
                const float m = std::sqrt(bx * bx + bz * bz);
                if (m > 1e-5f)
                    next[i] = {bx / m, bz / m};
            }
            h = next;
        }
        return concentration(h);
    };

    const float off = settle(0.0f); // alignment off: headings frozen at the spread-out start
    const float on = settle(0.6f);  // alignment on: headings converge toward a common direction
    EXPECT_GT(on, off + 0.2f) << "alignment should concentrate the herd toward a common heading";
    EXPECT_GT(on, 0.8f) << "the herd should be strongly aligned after settling";
}

// Order independence WITH alignment on: shuffling neighbours + their headings together yields the
// identical steer (the heading sum is reduced in fixed point, like cohesion/separation).
TEST(Flocking, AlignmentOrderIndependent) {
    std::vector<std::pair<float, float>> na = {{8.0f, 0.0f}, {2.0f, 1.0f}, {-5.0f, 4.0f}};
    std::vector<std::pair<float, float>> ha = {{1.0f, 2.0f}, {0.5f, 1.0f}, {-1.0f, 3.0f}};
    std::vector<std::pair<float, float>> nb = {{-5.0f, 4.0f}, {8.0f, 0.0f}, {2.0f, 1.0f}};
    std::vector<std::pair<float, float>> hb = {{-1.0f, 3.0f}, {1.0f, 2.0f}, {0.5f, 1.0f}};
    FlockParams p;
    p.alignment_weight = 0.7f;
    const FlockSteer sa = ComputeFlockSteer(0.5f, 0.5f, na, p, &ha);
    const FlockSteer sb = ComputeFlockSteer(0.5f, 0.5f, nb, p, &hb);
    EXPECT_FLOAT_EQ(sa.x, sb.x);
    EXPECT_FLOAT_EQ(sa.z, sb.z);
}

} // namespace
