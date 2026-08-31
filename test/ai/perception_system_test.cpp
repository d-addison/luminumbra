// PerceptionSystem coverage — vision/hearing fusion drives awareness
// over ECS entities, faction filtering, and determinism. Synthetic registry.

#include "gtest/gtest.h"

#include "luminumbra_common/ai/PerceptionSystem.h"
#include "luminumbra_common/components/CoreComponents.h"
#include "luminumbra_common/components/InstinctComponents.h"

namespace {
using luminumbra::ai::AwarenessState;
using luminumbra::ai::RunPerceptionSystemOnTick;
using Luminumbra::Components::AwarenessComponent;
using Luminumbra::Components::PerceptionComponent;
using Luminumbra::Components::SensableComponent;
using Luminumbra::Components::TransformComponent;

constexpr float kDt = 1.0f / 30.0f;

entt::entity MakePredator(entt::registry& r, float x, float z, float fx, float fz) {
    const auto e = r.create();
    r.emplace<TransformComponent>(e).position = Luminumbra::Vec3(x, 0.0f, z);
    auto& p = r.emplace<PerceptionComponent>(e);
    p.vision_cos_half_fov = 0.5f; // ~120 deg
    p.vision_range = 30.0f;
    p.facing_x = fx;
    p.facing_z = fz;
    p.faction = 1;
    r.emplace<AwarenessComponent>(e);
    return e;
}

entt::entity MakePrey(entt::registry& r,
                      float x,
                      float z,
                      std::uint32_t faction = 2,
                      float loud = 0.0f,
                      float pitch = 0.5f) {
    const auto e = r.create();
    r.emplace<TransformComponent>(e).position = Luminumbra::Vec3(x, 0.0f, z);
    auto& s = r.emplace<SensableComponent>(e);
    s.faction = faction;
    s.noise_loudness = loud;
    s.noise_pitch = pitch;
    return e;
}
} // namespace

TEST(PerceptionSystem, PredatorSeesPreyInConeAndEngages) {
    entt::registry r;
    const auto pred = MakePredator(r, 0, 0, 1, 0); // faces +X
    MakePrey(r, 10, 0);                            // dead ahead, visible
    for (int i = 0; i < 60; ++i)
        RunPerceptionSystemOnTick(r, kDt);
    const auto& aw = r.get<AwarenessComponent>(pred).awareness;
    EXPECT_EQ(aw.state, AwarenessState::Engaged);
    EXPECT_TRUE(aw.has_last_known);
    EXPECT_FLOAT_EQ(aw.last_known_x, 10.0f);
}

TEST(PerceptionSystem, SameFactionIsIgnored) {
    entt::registry r;
    const auto pred = MakePredator(r, 0, 0, 1, 0);
    MakePrey(r, 10, 0, /*faction=*/1); // same faction as the perceiver
    for (int i = 0; i < 30; ++i)
        RunPerceptionSystemOnTick(r, kDt);
    EXPECT_EQ(r.get<AwarenessComponent>(pred).awareness.state, AwarenessState::Unaware);
}

TEST(PerceptionSystem, TargetOutsideVisionConeIsNotSeenButCanBeHeard) {
    entt::registry r;
    // Two predators facing +X; a target sits BEHIND them on -X.
    const auto silent_eyes = MakePredator(r, 0, 0, 1, 0);
    MakePrey(r, -10, 0, /*faction=*/2, /*loud=*/0.0f); // behind + silent -> undetectable
    for (int i = 0; i < 40; ++i)
        RunPerceptionSystemOnTick(r, kDt);
    EXPECT_EQ(r.get<AwarenessComponent>(silent_eyes).awareness.state, AwarenessState::Unaware);

    entt::registry r2;
    const auto pred = MakePredator(r2, 0, 0, 1, 0);
    auto& ear = r2.get<PerceptionComponent>(pred).ear;
    ear.range = 30.0f;
    ear.peak_pitch = 0.5f;
    ear.bandwidth = 0.5f;
    MakePrey(r2, -10, 0, /*faction=*/2, /*loud=*/1.0f, /*pitch=*/0.5f); // behind but LOUD
    for (int i = 0; i < 60; ++i)
        RunPerceptionSystemOnTick(r2, kDt);
    // Heard despite being out of the vision cone -> at least suspicious.
    EXPECT_NE(r2.get<AwarenessComponent>(pred).awareness.state, AwarenessState::Unaware);
}

TEST(PerceptionSystem, IsDeterministic) {
    auto run = []() {
        entt::registry r;
        const auto pred = MakePredator(r, 0, 0, 1, 0);
        MakePrey(r, 12, 3);
        for (int i = 0; i < 25; ++i)
            RunPerceptionSystemOnTick(r, kDt);
        return r.get<AwarenessComponent>(pred).awareness.meter;
    };
    EXPECT_EQ(run(), run());
}
