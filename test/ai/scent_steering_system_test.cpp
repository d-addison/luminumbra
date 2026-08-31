// ScentSteeringSystem coverage — scent gradient biases the locomotion
// wish (track toward / flee from source), faction-free, deterministic.

#include "gtest/gtest.h"

#include "luminumbra_common/ai/ScentField.h"
#include "luminumbra_common/ai/ScentSteeringSystem.h"
#include "luminumbra_common/components/CoreComponents.h"
#include "luminumbra_common/components/InstinctComponents.h"

namespace {
using luminumbra::ai::RunScentSteeringOnTick;
using luminumbra::ai::ScentField;
using luminumbra::ai::WorldToCell;
using Luminumbra::Components::LocomotionIntentComponent;
using Luminumbra::Components::LocomotionProfile;
using Luminumbra::Components::ScentSenseComponent;
using Luminumbra::Components::TransformComponent;

ScentField FieldWithSourceAt10x10() {
    ScentField f(20, 20, 1);
    f.Deposit(0, 10, 10, 100.0);
    f.Step(0.25, 3, 0.0); // spread a smooth gradient
    return f;
}

entt::entity MakeSniffer(entt::registry& r, float x, float z, float sign, int channel = 0) {
    const auto e = r.create();
    r.emplace<TransformComponent>(e).position = Luminumbra::Vec3(x, 0.0f, z);
    auto& s = r.emplace<ScentSenseComponent>(e);
    s.channel = channel;
    s.sign = sign;
    s.strength = 1.0f;
    s.floor = 1e-4f;
    s.weber_k = 5.0f;
    r.emplace<LocomotionIntentComponent>(e); // wish starts (0,0)
    auto& p = r.emplace<LocomotionProfile>(e);
    p.move_speed = 3.0f;
    return e;
}
} // namespace

TEST(ScentSteering, WorldToCellFloorsTowardNegativeInfinity) {
    EXPECT_EQ(WorldToCell(5.0f, 0.0f, 1.0f), 5);
    EXPECT_EQ(WorldToCell(5.9f, 0.0f, 1.0f), 5);
    EXPECT_EQ(WorldToCell(-0.5f, 0.0f, 1.0f), -1); // floors, not truncates
    EXPECT_EQ(WorldToCell(20.0f, 0.0f, 2.0f), 10);
}

TEST(ScentSteering, TrackerBiasesWishTowardSource) {
    auto f = FieldWithSourceAt10x10();
    entt::registry r;
    const auto a = MakeSniffer(r, 5.0f, 10.0f, /*sign=*/+1.0f); // west of source -> track east
    RunScentSteeringOnTick(r, f, /*origin_x=*/0.0f, /*origin_z=*/0.0f, /*cell_size=*/1.0f);
    const auto& w = r.get<LocomotionIntentComponent>(a).wish_xz;
    EXPECT_GT(w.x, 0.0f);                                      // biased toward the +X source
    EXPECT_LE(std::sqrt(w.x * w.x + w.y * w.y), 3.0f + 1e-4f); // within move_speed budget
}

TEST(ScentSteering, FleerBiasesWishAwayFromSource) {
    auto f = FieldWithSourceAt10x10();
    entt::registry r;
    const auto a = MakeSniffer(r, 5.0f, 10.0f, /*sign=*/-1.0f); // flee -> west, away from source
    RunScentSteeringOnTick(r, f, 0.0f, 0.0f, 1.0f);
    EXPECT_LT(r.get<LocomotionIntentComponent>(a).wish_xz.x, 0.0f);
}

TEST(ScentSteering, InactiveChannelLeavesWishUnchanged) {
    auto f = FieldWithSourceAt10x10();
    entt::registry r;
    const auto a = MakeSniffer(r, 5.0f, 10.0f, +1.0f, /*channel=*/-1); // inactive
    RunScentSteeringOnTick(r, f, 0.0f, 0.0f, 1.0f);
    const auto& w = r.get<LocomotionIntentComponent>(a).wish_xz;
    EXPECT_FLOAT_EQ(w.x, 0.0f);
    EXPECT_FLOAT_EQ(w.y, 0.0f);
}

TEST(ScentSteering, IsDeterministic) {
    auto run = []() {
        auto f = FieldWithSourceAt10x10();
        entt::registry r;
        const auto a = MakeSniffer(r, 4.0f, 12.0f, +1.0f);
        RunScentSteeringOnTick(r, f, 0.0f, 0.0f, 1.0f);
        return r.get<LocomotionIntentComponent>(a).wish_xz.x;
    };
    EXPECT_EQ(run(), run());
}
