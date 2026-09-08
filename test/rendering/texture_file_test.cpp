#include "luminumbra_client/rendering/RenderPipeline.h"

#include <gtest/gtest.h>

#include <chrono>
#include <fstream>

namespace Luminumbra::Rendering {
struct TextureFileTestPeer {
    static bool Load(const std::filesystem::path& path) {
        RenderPipeline::LtexCpuImage image;
        return RenderPipeline::load_ltex_cpu_image(path, image);
    }
};
} // namespace Luminumbra::Rendering

TEST(TextureFile, RejectsOversizedTruncatedTrailingAndExcessMipPayloads) {
    const auto path =
        std::filesystem::temp_directory_path() /
        ("luminumbra-texture-" +
         std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".ltex");
    const auto write = [&](uint32_t width, uint16_t mips, size_t payload) {
        std::ofstream out(path, std::ios::binary);
        const auto pod = [&](auto value) {
            out.write(reinterpret_cast<const char*>(&value), sizeof(value));
        };
        pod(uint32_t{0x5845544c});
        pod(uint16_t{1});
        pod(mips);
        pod(width);
        pod(uint32_t{1});
        pod(uint8_t{4});
        out << std::string(payload, '\0');
    };
    using Luminumbra::Rendering::TextureFileTestPeer;
    write(1, 1, 4);
    EXPECT_TRUE(TextureFileTestPeer::Load(path));
    write(1, 1, 3);
    EXPECT_FALSE(TextureFileTestPeer::Load(path));
    write(1, 1, 5);
    EXPECT_FALSE(TextureFileTestPeer::Load(path));
    write(1, 2, 8);
    EXPECT_FALSE(TextureFileTestPeer::Load(path));
    write(0xffffffffu, 1, 4);
    EXPECT_FALSE(TextureFileTestPeer::Load(path));
    std::filesystem::remove(path);
}
