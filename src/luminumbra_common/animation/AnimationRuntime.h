#pragma once

// CPU animation core.
//
// Skeleton/Clip/Pose structs plus pure sampling/blending functions driven from
// the 30 Hz fixed simulation tick (pose sampling is FIRST in the tick order,
// the deterministic runtime contract ). All math is explicit scalar float math — no SIMD
// intrinsics, no fast-math dependence; AnimationRuntime.cpp is compiled with
// -ffp-contract=off so debug and release produce bit-identical poses (the
// pose-determinism gate asserts a committed checksum in both presets).

#include <cstdint>
#include <vector>

#include "entt/entt.hpp"

#include "SkinnedMeshFormat.h"

namespace luminumbra::animation {

struct JointPose {
    float translation[3] = {0.0f, 0.0f, 0.0f};
    float rotation[4] = {0.0f, 0.0f, 0.0f, 1.0f}; // x, y, z, w (unit quaternion)
    float scale[3] = {1.0f, 1.0f, 1.0f};
};

struct Pose {
    std::vector<JointPose> joints;
};

struct SkeletonJoint {
    uint32_t nameHash = 0;
    int32_t parentIndex = -1; // must precede this joint in the array
    float inverseBind[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    JointPose bindPose;
};

struct Skeleton {
    std::vector<SkeletonJoint> joints;
};

struct ClipTrack {
    uint32_t jointNameHash = 0;
    AnimTargetType targetType = AnimTargetType::Translation;
    uint32_t componentCount = 0; // 3 for T/S, 4 for R
    std::vector<float> times;    // ascending keyframe times (seconds)
    std::vector<float> values;   // keyCount * componentCount
};

struct AnimationClip {
    float duration = 0.0f;
    std::vector<ClipTrack> tracks;
};

// Conversions from the on-disk LMS2/.lanim assets.
Skeleton BuildSkeleton(const SkinnedMeshAsset& asset);
AnimationClip BuildClip(const AnimClipAsset& asset);

// Pure: the skeleton's bind-local pose.
Pose MakeBindPose(const Skeleton& skeleton);

// Pure: samples the clip at time t (seconds, clamped to [0, duration]).
// Joints without a track keep their bind-local pose. Keyframes interpolate
// linearly; rotations use neighborhood-corrected normalized lerp.
Pose SamplePose(const Skeleton& skeleton, const AnimationClip& clip, float time);

// Pure: per-joint blend of two same-size poses. alpha = 0 returns a,
// alpha = 1 returns b. Translations/scales lerp, rotations nlerp.
Pose BlendPoses(const Pose& a, const Pose& b, float alpha);

// Joint palette for GPU skinning: per joint a column-major 4x4 matrix
// global(joint) * inverseBind(joint), flattened to 16 floats each. Parents
// must precede children in the skeleton joint array.
void ComputeJointPalette(const Skeleton& skeleton,
                         const Pose& pose,
                         std::vector<float>& outPalette);

// FNV-1a 64-bit checksum over the exact float bit patterns. Used by the
// pose-determinism gate (identical expectation in debug and release).
uint64_t PoseChecksum(const Pose& pose, uint64_t seed = 14695981039346656037ull);
uint64_t
FloatSpanChecksum(const float* data, size_t count, uint64_t seed = 14695981039346656037ull);

// ECS integration: entities carrying this component get their pose sampled
// and joint palette recomputed every fixed simulation tick.
struct AnimationPlayerComponent {
    const Skeleton* skeleton = nullptr;
    const AnimationClip* clip = nullptr;
    double time = 0.0; // seconds into the clip, advanced by fixed_dt
    bool looping = true;
    Pose pose;                  // sampled local pose (output)
    std::vector<float> palette; // 16 floats per joint (output, GPU skinning)
};

// First system in the deterministic tick order (the deterministic runtime contract ).
// Advances each player's clock by fixed_dt, samples its pose and recomputes
// its joint palette.
void SamplePosesOnTick(entt::registry& registry, double fixed_dt);

} // namespace luminumbra::animation
