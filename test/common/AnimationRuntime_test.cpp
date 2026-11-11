// animation core +  pose-determinism gate.
//
// The  gate hashes the exact float bit patterns of poses sampled from a
// committed fixture rig across one second of 30 Hz ticks, one blend and one
// joint palette. The expectation is a committed constant that must hold in
// BOTH the debug and release presets: AnimationRuntime.cpp is scalar float
// math compiled with -ffp-contract=off, so optimization level cannot change
// the result. Any intentional change to the sampling math requires a
// deliberate update the baseline of kG1PoseChecksum in the same commit.

#include <gtest/gtest.h>

#include <cstdint>

#include "luminumbra_common/animation/AnimationRuntime.h"
#include "luminumbra_common/world/GameSession.h"

namespace {

using luminumbra::animation::AnimationClip;
using luminumbra::animation::AnimationPlayerComponent;
using luminumbra::animation::AnimTargetType;
using luminumbra::animation::BlendPoses;
using luminumbra::animation::ClipTrack;
using luminumbra::animation::ComputeJointPalette;
using luminumbra::animation::FloatSpanChecksum;
using luminumbra::animation::HashJointName;
using luminumbra::animation::MakeBindPose;
using luminumbra::animation::Pose;
using luminumbra::animation::PoseChecksum;
using luminumbra::animation::SamplePose;
using luminumbra::animation::Skeleton;
using luminumbra::animation::SkeletonJoint;

//  pose-determinism checksum. Computed once from the fixture rig
// below and committed; the debug and release presets must both reproduce it.
constexpr uint64_t kG1PoseChecksum = 0x80b8fec87c961238ull;

// Three-joint chain: root -> mid (bind +1 Y) -> tip (bind +1 Y).
Skeleton MakeFixtureSkeleton() {
    Skeleton skeleton;
    skeleton.joints.resize(3);

    skeleton.joints[0].nameHash = HashJointName("root");
    skeleton.joints[0].parentIndex = -1;

    skeleton.joints[1].nameHash = HashJointName("mid");
    skeleton.joints[1].parentIndex = 0;
    skeleton.joints[1].bindPose.translation[1] = 1.0f;
    skeleton.joints[1].inverseBind[13] = -1.0f;

    skeleton.joints[2].nameHash = HashJointName("tip");
    skeleton.joints[2].parentIndex = 1;
    skeleton.joints[2].bindPose.translation[1] = 1.0f;
    skeleton.joints[2].inverseBind[13] = -2.0f;

    return skeleton;
}

// One-second clip: root rises, mid rotates 90 degrees about Z, tip scales x2.
AnimationClip MakeFixtureClip() {
    AnimationClip clip;
    clip.duration = 1.0f;

    ClipTrack rootTranslation;
    rootTranslation.jointNameHash = HashJointName("root");
    rootTranslation.targetType = AnimTargetType::Translation;
    rootTranslation.componentCount = 3;
    rootTranslation.times = {0.0f, 0.5f, 1.0f};
    rootTranslation.values = {
        0.0f,
        0.0f,
        0.0f,
        0.0f,
        0.25f,
        0.0f,
        0.0f,
        0.5f,
        0.0f,
    };
    clip.tracks.push_back(rootTranslation);

    ClipTrack midRotation;
    midRotation.jointNameHash = HashJointName("mid");
    midRotation.targetType = AnimTargetType::Rotation;
    midRotation.componentCount = 4;
    midRotation.times = {0.0f, 1.0f};
    midRotation.values = {
        0.0f,
        0.0f,
        0.0f,
        1.0f,
        0.0f,
        0.0f,
        0.70710678f,
        0.70710678f,
    };
    clip.tracks.push_back(midRotation);

    ClipTrack tipScale;
    tipScale.jointNameHash = HashJointName("tip");
    tipScale.targetType = AnimTargetType::Scale;
    tipScale.componentCount = 3;
    tipScale.times = {0.0f, 1.0f};
    tipScale.values = {
        1.0f,
        1.0f,
        1.0f,
        2.0f,
        2.0f,
        2.0f,
    };
    clip.tracks.push_back(tipScale);

    return clip;
}

uint64_t ComputeG1Checksum() {
    const Skeleton skeleton = MakeFixtureSkeleton();
    const AnimationClip clip = MakeFixtureClip();

    uint64_t hash = 14695981039346656037ull;

    // One second of 30 Hz ticks, inclusive of both endpoints.
    for (int tick = 0; tick <= 30; ++tick) {
        const float time = static_cast<float>(tick) / 30.0f;
        const Pose pose = SamplePose(skeleton, clip, time);
        hash = PoseChecksum(pose, hash);
    }

    // One blend between two sampled poses.
    const Pose quarter = SamplePose(skeleton, clip, 0.25f);
    const Pose threeQuarter = SamplePose(skeleton, clip, 0.75f);
    hash = PoseChecksum(BlendPoses(quarter, threeQuarter, 0.5f), hash);

    // One joint palette (GPU skinning input).
    std::vector<float> palette;
    ComputeJointPalette(skeleton, SamplePose(skeleton, clip, 0.5f), palette);
    hash = FloatSpanChecksum(palette.data(), palette.size(), hash);

    return hash;
}

} // namespace

TEST(AnimationRuntime, G1PoseChecksumMatchesCommittedBaseline) {
    const uint64_t checksum = ComputeG1Checksum();
    RecordProperty("g1_pose_checksum", std::to_string(checksum));
    EXPECT_EQ(checksum, kG1PoseChecksum)
        << " pose checksum drifted: 0x" << std::hex << checksum
        << ". Pose sampling math changed; re-bless deliberately in the same commit.";
}

TEST(AnimationRuntime, SampleClampsOutsideClipRange) {
    const Skeleton skeleton = MakeFixtureSkeleton();
    const AnimationClip clip = MakeFixtureClip();

    const Pose atEnd = SamplePose(skeleton, clip, clip.duration);
    const Pose beyond = SamplePose(skeleton, clip, clip.duration + 5.0f);
    EXPECT_EQ(PoseChecksum(atEnd), PoseChecksum(beyond));

    const Pose atStart = SamplePose(skeleton, clip, 0.0f);
    const Pose before = SamplePose(skeleton, clip, -1.0f);
    EXPECT_EQ(PoseChecksum(atStart), PoseChecksum(before));
}

TEST(AnimationRuntime, BlendEndpointsReturnInputPoses) {
    const Skeleton skeleton = MakeFixtureSkeleton();
    const AnimationClip clip = MakeFixtureClip();

    const Pose a = SamplePose(skeleton, clip, 0.0f);
    const Pose b = SamplePose(skeleton, clip, 1.0f);

    // Translations and scales pass through exactly at the endpoints. Blended
    // rotations are renormalized by nlerp, so they match the inputs to within
    // one normalization step rather than bitwise.
    const Pose atZero = BlendPoses(a, b, 0.0f);
    const Pose atOne = BlendPoses(a, b, 1.0f);
    ASSERT_EQ(atZero.joints.size(), a.joints.size());
    ASSERT_EQ(atOne.joints.size(), b.joints.size());
    for (size_t j = 0; j < a.joints.size(); ++j) {
        for (int c = 0; c < 3; ++c) {
            EXPECT_FLOAT_EQ(atZero.joints[j].translation[c], a.joints[j].translation[c]);
            EXPECT_FLOAT_EQ(atZero.joints[j].scale[c], a.joints[j].scale[c]);
            EXPECT_FLOAT_EQ(atOne.joints[j].translation[c], b.joints[j].translation[c]);
            EXPECT_FLOAT_EQ(atOne.joints[j].scale[c], b.joints[j].scale[c]);
        }
        for (int c = 0; c < 4; ++c) {
            EXPECT_NEAR(atZero.joints[j].rotation[c], a.joints[j].rotation[c], 1e-6f);
            EXPECT_NEAR(atOne.joints[j].rotation[c], b.joints[j].rotation[c], 1e-6f);
        }
    }
}

TEST(AnimationRuntime, BindPosePaletteIsIdentity) {
    const Skeleton skeleton = MakeFixtureSkeleton();

    std::vector<float> palette;
    ComputeJointPalette(skeleton, MakeBindPose(skeleton), palette);
    ASSERT_EQ(palette.size(), skeleton.joints.size() * 16);

    for (size_t j = 0; j < skeleton.joints.size(); ++j) {
        for (int col = 0; col < 4; ++col) {
            for (int row = 0; row < 4; ++row) {
                const float expected = (col == row) ? 1.0f : 0.0f;
                EXPECT_FLOAT_EQ(palette[j * 16 + col * 4 + row], expected)
                    << "joint " << j << " col " << col << " row " << row;
            }
        }
    }
}

TEST(AnimationRuntime, FixedTickDrivesPoseSamplingFirstInTickOrder) {
    const Skeleton skeleton = MakeFixtureSkeleton();
    const AnimationClip clip = MakeFixtureClip();

    Luminumbra::world::GameSession session;
    const entt::entity entity = session.GetRegistry().create();
    auto& player = session.GetRegistry().emplace<AnimationPlayerComponent>(entity);
    player.skeleton = &skeleton;
    player.clip = &clip;

    // 0.1 s of frame time is 3 possible 30 Hz ticks, but GameSession clamps catch-up at 2
    // ticks/frame (GameSession.h spike guard), so 2 ticks run this frame.
    const std::uint32_t ticks = session.TickSimulation(0.1);
    EXPECT_EQ(ticks, 2u);
    EXPECT_EQ(session.GetSimulationTickCount(), 2u);

    const double fixedDt = session.GetSimulationClock().fixed_dt();
    EXPECT_DOUBLE_EQ(player.time, 2.0 * fixedDt);

    // The sampled pose must equal a direct SamplePose at the tick time and the
    // palette must be populated for GPU skinning.
    const Pose expected = SamplePose(skeleton, clip, static_cast<float>(player.time));
    EXPECT_EQ(PoseChecksum(player.pose), PoseChecksum(expected));
    ASSERT_EQ(player.palette.size(), skeleton.joints.size() * 16);
}

TEST(AnimationRuntime, LoopingPlayerWrapsClipTime) {
    const Skeleton skeleton = MakeFixtureSkeleton();
    const AnimationClip clip = MakeFixtureClip();

    entt::registry registry;
    const entt::entity entity = registry.create();
    auto& player = registry.emplace<AnimationPlayerComponent>(entity);
    player.skeleton = &skeleton;
    player.clip = &clip;
    player.time = 0.99;

    luminumbra::animation::SamplePosesOnTick(registry, 1.0 / 30.0);
    EXPECT_LT(player.time, static_cast<double>(clip.duration));
    EXPECT_GT(player.time, 0.0);
}
