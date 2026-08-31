#include "AnimationRuntime.h"

#include <cmath>
#include <cstring>

// Determinism note ( gate): this translation unit is compiled with
// -ffp-contract=off (see sources.cmake) so the compiler cannot fuse
// multiply-add chains differently between debug and release. All math below
// is plain scalar IEEE-754 float arithmetic.

namespace luminumbra::animation {

namespace {

float Lerp(float a, float b, float u) {
    return a * (1.0f - u) + b * u;
}

void LerpVec3(const float a[3], const float b[3], float u, float out[3]) {
    for (int c = 0; c < 3; ++c) out[c] = Lerp(a[c], b[c], u);
}

// Neighborhood-corrected normalized lerp (shortest arc), scalar math only.
void NlerpQuat(const float a[4], const float b[4], float u, float out[4]) {
    const float dot = a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3];
    const float sign = (dot < 0.0f) ? -1.0f : 1.0f;

    float blended[4];
    for (int c = 0; c < 4; ++c) blended[c] = Lerp(a[c], sign * b[c], u);

    const float lengthSq = blended[0] * blended[0] + blended[1] * blended[1] +
                           blended[2] * blended[2] + blended[3] * blended[3];
    if (lengthSq > 0.0f) {
        const float invLength = 1.0f / std::sqrt(lengthSq);
        for (int c = 0; c < 4; ++c) out[c] = blended[c] * invLength;
    } else {
        out[0] = 0.0f; out[1] = 0.0f; out[2] = 0.0f; out[3] = 1.0f;
    }
}

// Column-major 4x4 = translation * rotation * scale.
void MatrixFromJointPose(const JointPose& pose, float out[16]) {
    const float x = pose.rotation[0];
    const float y = pose.rotation[1];
    const float z = pose.rotation[2];
    const float w = pose.rotation[3];

    const float xx = x * x, yy = y * y, zz = z * z;
    const float xy = x * y, xz = x * z, yz = y * z;
    const float wx = w * x, wy = w * y, wz = w * z;

    const float sx = pose.scale[0];
    const float sy = pose.scale[1];
    const float sz = pose.scale[2];

    // Column 0
    out[0] = (1.0f - 2.0f * (yy + zz)) * sx;
    out[1] = (2.0f * (xy + wz)) * sx;
    out[2] = (2.0f * (xz - wy)) * sx;
    out[3] = 0.0f;
    // Column 1
    out[4] = (2.0f * (xy - wz)) * sy;
    out[5] = (1.0f - 2.0f * (xx + zz)) * sy;
    out[6] = (2.0f * (yz + wx)) * sy;
    out[7] = 0.0f;
    // Column 2
    out[8] = (2.0f * (xz + wy)) * sz;
    out[9] = (2.0f * (yz - wx)) * sz;
    out[10] = (1.0f - 2.0f * (xx + yy)) * sz;
    out[11] = 0.0f;
    // Column 3
    out[12] = pose.translation[0];
    out[13] = pose.translation[1];
    out[14] = pose.translation[2];
    out[15] = 1.0f;
}

// Column-major out = a * b.
void MatrixMultiply(const float a[16], const float b[16], float out[16]) {
    for (int col = 0; col < 4; ++col) {
        for (int row = 0; row < 4; ++row) {
            float sum = 0.0f;
            for (int k = 0; k < 4; ++k) {
                sum += a[k * 4 + row] * b[col * 4 + k];
            }
            out[col * 4 + row] = sum;
        }
    }
}

// Samples one track at time t into out (componentCount floats).
void SampleTrack(const ClipTrack& track, float time, float* out) {
    const size_t keyCount = track.times.size();
    const uint32_t comps = track.componentCount;
    if (keyCount == 0) return;

    if (time <= track.times.front() || keyCount == 1) {
        for (uint32_t c = 0; c < comps; ++c) out[c] = track.values[c];
        return;
    }
    if (time >= track.times.back()) {
        const size_t base = (keyCount - 1) * comps;
        for (uint32_t c = 0; c < comps; ++c) out[c] = track.values[base + c];
        return;
    }

    size_t next = 1;
    while (next < keyCount - 1 && track.times[next] <= time) ++next;
    const size_t prev = next - 1;

    const float t0 = track.times[prev];
    const float t1 = track.times[next];
    const float span = t1 - t0;
    const float u = (span > 0.0f) ? ((time - t0) / span) : 0.0f;

    const float* a = &track.values[prev * comps];
    const float* b = &track.values[next * comps];
    if (track.targetType == AnimTargetType::Rotation) {
        NlerpQuat(a, b, u, out);
    } else {
        LerpVec3(a, b, u, out);
    }
}

int32_t FindJointIndex(const Skeleton& skeleton, uint32_t nameHash) {
    for (size_t j = 0; j < skeleton.joints.size(); ++j) {
        if (skeleton.joints[j].nameHash == nameHash) return static_cast<int32_t>(j);
    }
    return -1;
}

} // namespace

Skeleton BuildSkeleton(const SkinnedMeshAsset& asset) {
    Skeleton skeleton;
    skeleton.joints.resize(asset.joints.size());
    for (size_t j = 0; j < asset.joints.size(); ++j) {
        const Lms2Joint& src = asset.joints[j];
        SkeletonJoint& dst = skeleton.joints[j];
        dst.nameHash = src.nameHash;
        dst.parentIndex = src.parentIndex;
        std::memcpy(dst.inverseBind, src.inverseBind, sizeof(dst.inverseBind));
        std::memcpy(dst.bindPose.translation, src.localTranslation, sizeof(dst.bindPose.translation));
        std::memcpy(dst.bindPose.rotation, src.localRotation, sizeof(dst.bindPose.rotation));
        std::memcpy(dst.bindPose.scale, src.localScale, sizeof(dst.bindPose.scale));
    }
    return skeleton;
}

AnimationClip BuildClip(const AnimClipAsset& asset) {
    AnimationClip clip;
    clip.duration = asset.header.duration;
    clip.tracks.reserve(asset.tracks.size());
    for (const AnimTrack& src : asset.tracks) {
        ClipTrack track;
        track.jointNameHash = src.header.jointNameHash;
        track.targetType = static_cast<AnimTargetType>(src.header.targetType);
        track.componentCount = src.header.componentCount;
        track.times = src.times;
        track.values = src.values;
        clip.tracks.push_back(std::move(track));
    }
    return clip;
}

Pose MakeBindPose(const Skeleton& skeleton) {
    Pose pose;
    pose.joints.resize(skeleton.joints.size());
    for (size_t j = 0; j < skeleton.joints.size(); ++j) {
        pose.joints[j] = skeleton.joints[j].bindPose;
    }
    return pose;
}

Pose SamplePose(const Skeleton& skeleton, const AnimationClip& clip, float time) {
    Pose pose = MakeBindPose(skeleton);

    float clamped = time;
    if (clamped < 0.0f) clamped = 0.0f;
    if (clamped > clip.duration) clamped = clip.duration;

    for (const ClipTrack& track : clip.tracks) {
        const int32_t jointIndex = FindJointIndex(skeleton, track.jointNameHash);
        if (jointIndex < 0) continue;

        JointPose& joint = pose.joints[static_cast<size_t>(jointIndex)];
        switch (track.targetType) {
            case AnimTargetType::Translation:
                SampleTrack(track, clamped, joint.translation);
                break;
            case AnimTargetType::Rotation:
                SampleTrack(track, clamped, joint.rotation);
                break;
            case AnimTargetType::Scale:
                SampleTrack(track, clamped, joint.scale);
                break;
        }
    }
    return pose;
}

Pose BlendPoses(const Pose& a, const Pose& b, float alpha) {
    Pose out;
    const size_t count = (a.joints.size() < b.joints.size()) ? a.joints.size() : b.joints.size();
    out.joints.resize(count);
    for (size_t j = 0; j < count; ++j) {
        LerpVec3(a.joints[j].translation, b.joints[j].translation, alpha, out.joints[j].translation);
        NlerpQuat(a.joints[j].rotation, b.joints[j].rotation, alpha, out.joints[j].rotation);
        LerpVec3(a.joints[j].scale, b.joints[j].scale, alpha, out.joints[j].scale);
    }
    return out;
}

void ComputeJointPalette(const Skeleton& skeleton, const Pose& pose,
                         std::vector<float>& outPalette) {
    const size_t jointCount = skeleton.joints.size();
    outPalette.assign(jointCount * 16, 0.0f);

    std::vector<float> globals(jointCount * 16);
    for (size_t j = 0; j < jointCount; ++j) {
        float local[16];
        MatrixFromJointPose(pose.joints[j], local);

        const int32_t parent = skeleton.joints[j].parentIndex;
        if (parent >= 0 && static_cast<size_t>(parent) < j) {
            MatrixMultiply(&globals[static_cast<size_t>(parent) * 16], local, &globals[j * 16]);
        } else {
            std::memcpy(&globals[j * 16], local, sizeof(local));
        }

        MatrixMultiply(&globals[j * 16], skeleton.joints[j].inverseBind, &outPalette[j * 16]);
    }
}

uint64_t FloatSpanChecksum(const float* data, size_t count, uint64_t seed) {
    uint64_t hash = seed;
    for (size_t i = 0; i < count; ++i) {
        uint32_t bits;
        std::memcpy(&bits, &data[i], sizeof(bits));
        for (int byte = 0; byte < 4; ++byte) {
            hash ^= (bits >> (byte * 8)) & 0xFFu;
            hash *= 1099511628211ull;
        }
    }
    return hash;
}

uint64_t PoseChecksum(const Pose& pose, uint64_t seed) {
    uint64_t hash = seed;
    for (const JointPose& joint : pose.joints) {
        hash = FloatSpanChecksum(joint.translation, 3, hash);
        hash = FloatSpanChecksum(joint.rotation, 4, hash);
        hash = FloatSpanChecksum(joint.scale, 3, hash);
    }
    return hash;
}

void SamplePosesOnTick(entt::registry& registry, double fixed_dt) {
    const auto view = registry.view<AnimationPlayerComponent>();
    for (const entt::entity entity : view) {
        AnimationPlayerComponent& player = view.get<AnimationPlayerComponent>(entity);
        if (!player.skeleton || !player.clip) continue;

        player.time += fixed_dt;
        if (player.looping && player.clip->duration > 0.0f) {
            while (player.time >= static_cast<double>(player.clip->duration)) {
                player.time -= static_cast<double>(player.clip->duration);
            }
        }

        player.pose = SamplePose(*player.skeleton, *player.clip, static_cast<float>(player.time));
        ComputeJointPalette(*player.skeleton, player.pose, player.palette);
    }
}

} // namespace luminumbra::animation
