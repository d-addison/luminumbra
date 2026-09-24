#include <gtest/gtest.h>

#include "luminumbra_common/animation/SkinnedMeshFormat.h"
#include <algorithm>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <ios>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include "luminumbra_common/animation/AnimationRuntime.h"

namespace {

using namespace luminumbra::animation;

class ClipFile {
public:
    ClipFile()
        : path(std::filesystem::temp_directory_path() /
               ("luminumbra_clip_" +
                std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) +
                ".lanim")) {}
    ~ClipFile() {
        std::error_code error;
        std::filesystem::remove(path, error);
    }
    const std::filesystem::path& Path() const {
        return path;
    }

private:
    std::filesystem::path path;

public:
    void Write(const std::vector<uint8_t>& bytes) const {
        std::ofstream out(path, std::ios::binary);
        out.write(reinterpret_cast<const char*>(bytes.data()),
                  static_cast<std::streamsize>(bytes.size()));
    }
};

void U32(std::vector<uint8_t>& bytes, uint32_t value) {
    for (unsigned shift = 0; shift < 32; shift += 8)
        bytes.push_back(static_cast<uint8_t>(value >> shift));
}

void SetU32(std::vector<uint8_t>& bytes, size_t offset, uint32_t value) {
    for (unsigned i = 0; i < 4; ++i)
        bytes[offset + i] = static_cast<uint8_t>(value >> (i * 8));
}

// Independent little-endian wire fixture, not serialized with the reader's structs.
std::vector<uint8_t> RotationWire(uint32_t version) {
    std::vector<uint8_t> bytes;
    for (const uint32_t value : {0x4d4e414cu,
                                 version,
                                 1u,
                                 std::bit_cast<uint32_t>(1.0f),
                                 HashJointName("root"),
                                 1u,
                                 2u,
                                 4u})
        U32(bytes, value);
    if (version == 2)
        U32(bytes, 2);
    for (const float value :
         {0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.70710678f, 0.70710678f})
        U32(bytes, std::bit_cast<uint32_t>(value));
    return bytes;
}

Skeleton RootSkeleton() {
    Skeleton result;
    result.joints.resize(1);
    result.joints[0].nameHash = HashJointName("root");
    return result;
}

} // namespace

TEST(AnimationClipFormat, ReadsLegacyBytesWithOriginalQuaternionInterpolation) {
    const ClipFile file;
    file.Write(RotationWire(1));
    AnimClipAsset asset;
    ASSERT_TRUE(LoadAnimClipAsset(file.Path().string(), asset));
    ASSERT_EQ(asset.tracks.size(), 1u);
    EXPECT_EQ(asset.tracks[0].interpolation, AnimInterpolation::LegacyLinear);
    const auto pose = SamplePose(RootSkeleton(), BuildClip(asset), 0.25f);
    EXPECT_FLOAT_EQ(pose.joints[0].rotation[2], 0.18736556f);
    EXPECT_FLOAT_EQ(pose.joints[0].rotation[3], 0.98229021f);
}

TEST(AnimationClipFormat, ReadsV2WithSphericalInterpolation) {
    const ClipFile file;
    file.Write(RotationWire(2));
    AnimClipAsset asset;
    ASSERT_TRUE(LoadAnimClipAsset(file.Path().string(), asset));
    EXPECT_EQ(asset.tracks[0].interpolation, AnimInterpolation::Linear);
    const auto pose = SamplePose(RootSkeleton(), BuildClip(asset), 0.25f);
    EXPECT_NEAR(pose.joints[0].rotation[2], 0.19509032f, 1e-4f);
    EXPECT_NEAR(pose.joints[0].rotation[3], 0.98078528f, 1e-4f);
}

TEST(AnimationClipFormat, RejectsMalformedWireWithoutReplacingLoadedClip) {
    const ClipFile file;
    const auto valid = RotationWire(2);
    const std::vector<std::pair<size_t, uint32_t>> corruptions = {
        {4, 99},
        {8, UINT32_MAX},
        {12, 0x7fc00000},
        {20, 99},
        {24, UINT32_MAX},
        {28, 99},
        {32, 99},
        {40, 0},
        {44, 0x7fc00000},
        {56, 0},
        {56, std::bit_cast<uint32_t>(2.0f)},
    };
    for (const auto& [offset, value] : corruptions) {
        SCOPED_TRACE(offset);
        auto bytes = valid;
        SetU32(bytes, offset, value);
        file.Write(bytes);
        AnimClipAsset previous;
        previous.header.duration = 42;
        EXPECT_FALSE(LoadAnimClipAsset(file.Path().string(), previous));
        EXPECT_FLOAT_EQ(previous.header.duration, 42);
    }
    for (size_t size = 0; size < valid.size(); ++size) {
        file.Write(
            std::vector<uint8_t>(valid.begin(), valid.begin() + static_cast<std::ptrdiff_t>(size)));
        AnimClipAsset asset;
        EXPECT_FALSE(LoadAnimClipAsset(file.Path().string(), asset)) << size;
    }
    auto trailing = valid;
    trailing.push_back(0);
    file.Write(trailing);
    AnimClipAsset asset;
    EXPECT_FALSE(LoadAnimClipAsset(file.Path().string(), asset));
}

TEST(AnimationClipFormat, V2RotationStaysWithinDeclaredSphericalError) {
    const auto skeleton = RootSkeleton();
    float maximumError = 0;
    uint64_t checksum = 14695981039346656037ull;
    for (int degrees = 1; degrees <= 180; ++degrees) {
        const double angle = degrees * 3.14159265358979323846 / 180;
        AnimationClip clip;
        clip.duration = 2;
        ClipTrack track;
        track.jointNameHash = HashJointName("root");
        track.targetType = AnimTargetType::Rotation;
        track.componentCount = 4;
        track.interpolation = AnimInterpolation::Linear;
        track.times = {0, 2};
        track.values = {0,
                        0,
                        0,
                        1,
                        0,
                        0,
                        static_cast<float>(std::sin(angle / 2)),
                        static_cast<float>(std::cos(angle / 2))};
        clip.tracks.push_back(track);
        for (int tick = 0; tick <= 20; ++tick) {
            const auto pose = SamplePose(skeleton, clip, static_cast<float>(tick) / 10);
            const auto& q = pose.joints[0].rotation;
            const double expected = angle * tick / 20;
            const double actual = 2 * std::atan2(q[2], q[3]);
            maximumError = std::max(maximumError, static_cast<float>(std::abs(actual - expected)));
            checksum = PoseChecksum(pose, checksum);
        }
    }
    EXPECT_LT(maximumError, 0.001f); // Radians; engine deterministic trig approximation.
    RecordProperty("max_angular_error_radians", maximumError);
    // Source keys above use libm only for independent accuracy measurement;
    // deterministic runtime checks use fixed float keys in the next fixture.
    EXPECT_NE(checksum, 0u);
}

TEST(AnimationClipFormat, V2FixedKeysProduceStablePoseChecksum) {
    const auto skeleton = RootSkeleton();
    AnimationClip clip;
    clip.duration = 2;
    ClipTrack track;
    track.jointNameHash = HashJointName("root");
    track.targetType = AnimTargetType::Rotation;
    track.componentCount = 4;
    track.interpolation = AnimInterpolation::Linear;
    track.times = {0, 2};
    track.values = {0, 0, 0, 1, 0, 0, -0.8660254f, -0.5f};
    clip.tracks.push_back(track);
    ClipTrack step;
    step.jointNameHash = track.jointNameHash;
    step.targetType = AnimTargetType::Translation;
    step.componentCount = 3;
    step.interpolation = AnimInterpolation::Step;
    step.times = {0, 1, 2};
    step.values = {0, 0, 0, 1, 2, 3, -1, -2, -3};
    clip.tracks.push_back(step);
    ClipTrack cubic;
    cubic.jointNameHash = track.jointNameHash;
    cubic.targetType = AnimTargetType::Scale;
    cubic.componentCount = 3;
    cubic.interpolation = AnimInterpolation::CubicSpline;
    cubic.times = {0, 2};
    cubic.values = {0, 0, 0, 1, 1, 1, 1, 0, 0, 0, -0.5f, 0, 2, 2, 2, 0, 0, 0};
    clip.tracks.push_back(cubic);
    uint64_t checksum = 14695981039346656037ull;
    for (int tick = 0; tick <= 60; ++tick)
        checksum =
            PoseChecksum(SamplePose(skeleton, clip, static_cast<float>(tick) / 30), checksum);
    RecordProperty("v2_pose_checksum", std::to_string(checksum));
    EXPECT_EQ(checksum, 15637013004113304019ull);
}
