// I9-ECO: the pure herd-flocking steering helper — cohesion toward the group centroid,
// separation away from crowding, libm-free + order-independent (run==replay).
#include <gtest/gtest.h>

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
    std::vector<std::pair<float, float>> n = {{1.0f, 0.0f}};  // inside separation_radius (3)
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
    const FlockSteer base = ComputeFlockSteer(0.5f, 0.5f, n);          // positions-only
    FlockParams p;                                                     // alignment_weight == 0
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

}  // namespace
