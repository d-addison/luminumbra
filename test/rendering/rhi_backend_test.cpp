//   /   — the LUMIN_RHI parse leg of the RHI bring-up
// proving signal. Pure string logic: no GPU, no GL context, no Diligent. This half
// of RhiDeviceBringupGpu is toolchain-independent and passes on any box; the
// device/swapchain-creation leg lives in the GPU ctest.

#include "rendering/rhi/RhiBackend.h"

#include <gtest/gtest.h>

namespace {

using Luminumbra::Rendering::Rhi::Backend;
using Luminumbra::Rendering::Rhi::BackendName;
using Luminumbra::Rendering::Rhi::ParseRhiBackend;

TEST(RhiBackendParse, DefaultsToGlOnNullEmptyOrGarbage) {
    EXPECT_EQ(ParseRhiBackend(nullptr), Backend::Gl);
    EXPECT_EQ(ParseRhiBackend(""), Backend::Gl);
    EXPECT_EQ(ParseRhiBackend("   "), Backend::Gl);
    EXPECT_EQ(ParseRhiBackend("metal"), Backend::Gl);
    EXPECT_EQ(ParseRhiBackend("d3d11"), Backend::Gl);
}

TEST(RhiBackendParse, ParsesCanonicalValues) {
    EXPECT_EQ(ParseRhiBackend("gl"), Backend::Gl);
    EXPECT_EQ(ParseRhiBackend("vulkan"), Backend::Vulkan);
}

TEST(RhiBackendParse, IsCaseInsensitiveAndAcceptsAliases) {
    EXPECT_EQ(ParseRhiBackend("GL"), Backend::Gl);
    EXPECT_EQ(ParseRhiBackend("OpenGL"), Backend::Gl);
    EXPECT_EQ(ParseRhiBackend("VK"), Backend::Vulkan);
    EXPECT_EQ(ParseRhiBackend("Vulkan"), Backend::Vulkan);
    EXPECT_EQ(ParseRhiBackend("D3D12"), Backend::Gl);
}

TEST(RhiBackendParse, BackendNameRoundTrips) {
    for (Backend b : {Backend::Gl, Backend::Vulkan}) {
        EXPECT_EQ(ParseRhiBackend(BackendName(b)), b) << "round-trip failed for " << BackendName(b);
    }
    EXPECT_STREQ(BackendName(Backend::Gl), "gl");
    EXPECT_STREQ(BackendName(Backend::Vulkan), "vulkan");
}

} // namespace
