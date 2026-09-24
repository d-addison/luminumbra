#include <gtest/gtest.h>

#include "luminumbra_client/core/scenarios/SkinnedMeshTestAssets.h"
#include "luminumbra_common/animation/AnimationRuntime.h"
#include "luminumbra_common/animation/SkinnedMeshFormat.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <ios>
#include <iterator>
#include <string>
#include <system_error>
#include <vector>

namespace {

namespace anim = luminumbra::animation;

class SmokeAssetDirectory {
public:
    SmokeAssetDirectory()
        : path(std::filesystem::temp_directory_path() /
               ("luminumbra_skinned_smoke_" +
                std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()))) {
        std::filesystem::create_directory(path);
    }
    ~SmokeAssetDirectory() {
        std::error_code error;
        std::filesystem::remove_all(path, error);
    }

    const std::filesystem::path& Path() const {
        return path;
    }

private:
    const std::filesystem::path path;
};

} // namespace

TEST(SkinnedTestAssets, RealSmokeWriterRetainsLegacyWireAndRuntimePlayback) {
    const SmokeAssetDirectory directory;
    const auto mesh_path = directory.Path() / "rig.lmesh";
    const auto clip_path = directory.Path() / "rig.lanim";
    ASSERT_TRUE(Luminumbra::Client::ScenarioHarness::WriteSkinnedTestAssets(mesh_path, clip_path));

    // Independent committed LANM1 wire: this is the original smoke's 60-second
    // arm rotation, not serialized through the format structs under test.
    const std::vector<uint8_t> legacy_clip = {
        0x4c, 0x41, 0x4e, 0x4d, 0x01, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x70,
        0x42, 0xb5, 0x27, 0x6f, 0x38, 0x01, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x04, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x70, 0x42, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x80, 0x3f, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0xd7, 0xb3, 0x5d, 0x3f, 0x00, 0x00, 0x00, 0x3f,
    };
    std::ifstream input(clip_path, std::ios::binary);
    ASSERT_TRUE(input);
    const std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(input)), {});
    EXPECT_EQ(bytes, legacy_clip);

    anim::SkinnedMeshAsset mesh;
    anim::AnimClipAsset clip;
    ASSERT_TRUE(anim::LoadSkinnedMeshAsset(mesh_path.string(), mesh));
    ASSERT_TRUE(anim::LoadAnimClipAsset(clip_path.string(), clip));
    EXPECT_EQ(mesh.vertices.size(), 48u);
    EXPECT_EQ(mesh.indices.size(), 72u);
    ASSERT_EQ(mesh.joints.size(), 2u);
    EXPECT_EQ(clip.header.version, anim::kLanimLegacyVersion);
    ASSERT_EQ(clip.tracks.size(), 1u);
    EXPECT_EQ(clip.tracks[0].interpolation, anim::AnimInterpolation::LegacyLinear);

    const auto skeleton = anim::BuildSkeleton(mesh);
    const auto runtime_clip = anim::BuildClip(clip);
    const auto pose = anim::SamplePose(skeleton, runtime_clip, 15.0f);
    // At a quarter of a 120-degree rotation, legacy nlerp differs visibly from
    // slerp (whose z component is sin(15 degrees), approximately 0.258819).
    EXPECT_NEAR(pose.joints[1].rotation[2], 0.24019223f, 1e-6f);
    EXPECT_NEAR(pose.joints[1].rotation[3], 0.97072536f, 1e-6f);
    EXPECT_FLOAT_EQ(pose.joints[1].translation[1], 2.0f);

    std::vector<float> palette;
    anim::ComputeJointPalette(skeleton, anim::MakeBindPose(skeleton), palette);
    ASSERT_EQ(palette.size(), 32u);
    for (size_t joint = 0; joint < 2; ++joint)
        for (size_t element = 0; element < 16; ++element)
            EXPECT_FLOAT_EQ(palette[joint * 16 + element], element % 5 == 0 ? 1.0f : 0.0f);
}
