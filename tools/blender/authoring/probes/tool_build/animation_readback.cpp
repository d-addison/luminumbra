#include "luminumbra_common/animation/AnimationRuntime.h"
#include "luminumbra_common/animation/SkinnedMeshFormat.h"
#include <cmath>
#include <cstddef>
#include <iostream>
#include <vector>

int main(int argc, char** argv) {
    using namespace luminumbra::animation;
    if (argc < 2 || argc > 3)
        return 2;
    SkinnedMeshAsset asset;
    if (!LoadSkinnedMeshAsset(argv[1], asset))
        return 3;
    const auto skeleton = BuildSkeleton(asset);
    auto pose = MakeBindPose(skeleton);
    if (argc == 3) {
        AnimClipAsset clip;
        if (!LoadAnimClipAsset(argv[2], clip))
            return 4;
        pose = SamplePose(skeleton, BuildClip(clip), 0.5f);
    }
    std::vector<float> palette;
    ComputeJointPalette(skeleton, pose, palette);
    std::cout << "{\"joint_count\":" << skeleton.joints.size() << ",\"translations\":[";
    for (std::size_t j = 0; j < pose.joints.size(); ++j) {
        if (j)
            std::cout << ',';
        const auto& p = pose.joints[j];
        std::cout << '[' << p.translation[0] << ',' << p.translation[1] << ',' << p.translation[2]
                  << ']';
    }
    std::cout << "],\"palette\":[";
    for (std::size_t i = 0; i < palette.size(); ++i) {
        if (i)
            std::cout << ',';
        if (!std::isfinite(palette[i]))
            return 5;
        std::cout << palette[i];
    }
    std::cout << "]}\n";
    return 0;
}
