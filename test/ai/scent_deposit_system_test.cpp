// ScentDepositSystem coverage — emitters lay scent into the field at
// their cell; moving emitters lay a trail; non-emitters/inactive channels no-op.

#include "gtest/gtest.h"

#include "luminumbra_common/ai/ScentDepositSystem.h"
#include "luminumbra_common/ai/ScentField.h"
#include "luminumbra_common/components/CoreComponents.h"
#include "luminumbra_common/components/InstinctComponents.h"

namespace {
using luminumbra::ai::RunScentDepositOnTick;
using luminumbra::ai::ScentField;
using Luminumbra::Components::SensableComponent;
using Luminumbra::Components::TransformComponent;

entt::entity MakeEmitter(entt::registry& r, float x, float z, int channel, float amount) {
    const auto e = r.create();
    r.emplace<TransformComponent>(e).position = Luminumbra::Vec3(x, 0.0f, z);
    auto& s = r.emplace<SensableComponent>(e);
    s.scent_channel = channel;
    s.scent_deposit = amount;
    return e;
}
} // namespace

TEST(ScentDeposit, EmitterLaysScentAtItsCell) {
    ScentField f(20, 20, 1);
    entt::registry r;
    MakeEmitter(r, 5.0f, 7.0f, /*channel=*/0, /*amount=*/10.0f);
    RunScentDepositOnTick(r, f, 0.0f, 0.0f, 1.0f);
    EXPECT_GE(f.Sample(0, 5, 7), 10.0);
    EXPECT_DOUBLE_EQ(f.Sample(0, 0, 0), 0.0); // elsewhere untouched
}

TEST(ScentDeposit, RepeatedDepositsAccumulate) {
    ScentField f(20, 20, 1);
    entt::registry r;
    MakeEmitter(r, 5.0f, 5.0f, 0, 4.0f);
    for (int i = 0; i < 3; ++i)
        RunScentDepositOnTick(r, f, 0.0f, 0.0f, 1.0f);
    EXPECT_GE(f.Sample(0, 5, 5), 12.0); // 3 x 4
}

TEST(ScentDeposit, InactiveOrZeroEmittersDoNothing) {
    ScentField f(20, 20, 1);
    entt::registry r;
    MakeEmitter(r, 5.0f, 5.0f, /*channel=*/-1, 10.0f); // inactive channel
    MakeEmitter(r, 6.0f, 6.0f, /*channel=*/0, 0.0f);   // zero amount
    const auto stats = RunScentDepositOnTick(r, f, 0.0f, 0.0f, 1.0f);
    EXPECT_EQ(stats.emitters, 0u);
    EXPECT_DOUBLE_EQ(f.Sample(0, 5, 5), 0.0);
    EXPECT_DOUBLE_EQ(f.Sample(0, 6, 6), 0.0);
}

TEST(ScentDeposit, MovingEmitterLaysATrail) {
    ScentField f(20, 20, 1);
    entt::registry r;
    auto e = MakeEmitter(r, 2.0f, 2.0f, 0, 5.0f);
    auto& tf = r.get<TransformComponent>(e);
    for (int step = 0; step < 5; ++step) {
        tf.position = Luminumbra::Vec3(2.0f + static_cast<float>(step), 0.0f, 2.0f);
        RunScentDepositOnTick(r, f, 0.0f, 0.0f, 1.0f);
    }
    // Scent present along the walked cells (2..6 on z=2).
    for (int x = 2; x <= 6; ++x)
        EXPECT_GE(f.Sample(0, x, 2), 5.0);
}
