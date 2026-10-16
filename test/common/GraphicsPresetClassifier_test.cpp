#include "luminumbra_common/core/GraphicsPresetClassifier.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>

namespace {

using namespace luminumbra::core;
constexpr std::uint64_t kGiB = 1024ULL * 1024 * 1024;
constexpr std::array kDistributionKernelReleases{
    "6.8.0-83-generic",
    "6.6.10-arch1-1",
    "6.6.10-rt",
    "6.6.10-rt18",
    "6.8.5-301.fc40.x86_64",
    "6.1.0+deb12-amd64",
    "6.1.0-30+deb12-amd64",
    "6.1.0-18-amd64",
    "6.8.0-1018-azure",
    "6.8.0-1016-aws",
    "6.8.0-1015-gcp",
    "6.8.0-1018-oem",
    "6.6.31+rpt-rpi-v8",
    "6.6.54-gentoo-dist",
    "6.4.0-150600.23.17-default",
    "6.12.0-rc6",
};

GraphicsHardwareInputs ReferenceHardware() {
    return {.vendor = "NVIDIA Corporation",
            .renderer = "NVIDIA GeForce RTX 5070 Ti/PCIe/SSE2",
            .version = "4.6.0 NVIDIA 580.0",
            .logicalProcessorCount = 16,
            .dedicatedVideoMemoryBytes = 16 * kGiB,
            .framebufferWidth = 1920,
            .framebufferHeight = 1080};
}

TEST(GraphicsPresetClassifierTest, EveryAllowlistedModelHasItsApprovedCeiling) {
    struct Case {
        std::string_view renderer;
        GraphicsPreset expected;
    };
    constexpr std::array cases{
        Case{"GeForce GTX 1060", GraphicsPreset::Low},
        Case{"Radeon RX 580", GraphicsPreset::Low},
        Case{"GeForce GTX 1660 Super", GraphicsPreset::Medium},
        Case{"GeForce RTX 2060", GraphicsPreset::Medium},
        Case{"Radeon RX 5600 XT", GraphicsPreset::Medium},
        Case{"GeForce RTX 3060 Ti", GraphicsPreset::High},
        Case{"GeForce RTX 3070", GraphicsPreset::High},
        Case{"Radeon RX 6700 XT", GraphicsPreset::High},
        Case{"GeForce RTX 5070 Ti", GraphicsPreset::Ultra},
    };
    for (const auto& test : cases) {
        SCOPED_TRACE(test.renderer);
        auto inputs = ReferenceHardware();
        inputs.renderer = test.renderer;
        const auto result = ClassifyGraphicsPreset(inputs);
        EXPECT_EQ(result.preset, test.expected);
        EXPECT_EQ(result.reason.gpuCeiling, test.expected);
        EXPECT_EQ(result.reason.identityRule, GraphicsIdentityRule::AllowlistedModel);
        EXPECT_FALSE(result.matchedModel.empty());
        EXPECT_TRUE(result.reason.gpuBinding);
    }
}

TEST(GraphicsPresetClassifierTest, NormalizesVendorTrademarkAndDriverVariants) {
    struct Case {
        std::string_view vendor;
        std::string_view renderer;
        GraphicsPreset expected;
        std::string_view model;
    };
    constexpr std::array cases{
        Case{"NVIDIA", "NVIDIA GeForce(R) GTX 1060 6GB/PCIe/SSE2", GraphicsPreset::Low, "GTX 1060"},
        Case{"nViDiA Corporation",
             " GeForce  GTX 1660 SUPER/PCIe/SSE2 ",
             GraphicsPreset::Medium,
             "GTX 1660 SUPER"},
        Case{
            "AMD", "AMD Radeon(TM) RX 5600 XT (RADV NAVI10)", GraphicsPreset::Medium, "RX 5600 XT"},
        Case{"AMD",
             "AMD Radeon(TM) RX 5600 XT (rAdV\t nAvI10)",
             GraphicsPreset::Medium,
             "RX 5600 XT"},
        Case{"ATI Technologies Inc.",
             "AMD Radeon (TM) RX 6700 XT (LLVM 15.0.7, DRM 3.49)",
             GraphicsPreset::High,
             "RX 6700 XT"},
        Case{"Advanced Micro Devices, Inc.",
             "AMD Radeon RX 580 Series",
             GraphicsPreset::Low,
             "RX 580"},
        Case{"Google Inc. (NVIDIA)",
             "ANGLE (NVIDIA, NVIDIA GeForce RTX 3060 Ti Direct3D11 vs_5_0 ps_5_0, D3D11)",
             GraphicsPreset::High,
             "RTX 3060 TI"},
        Case{"Google Inc. (ATI Technologies Inc.)",
             "ANGLE (AMD, AMD Radeon(TM) RX 6700 XT Direct3D11 vs_5_0 ps_5_0)",
             GraphicsPreset::High,
             "RX 6700 XT"},
        Case{"NVIDIA Corporation",
             "NVIDIA GeForce RTX 3070 GPU/PCIe/SSE2",
             GraphicsPreset::High,
             "RTX 3070"},
    };
    for (const auto& test : cases) {
        SCOPED_TRACE(test.renderer);
        auto inputs = ReferenceHardware();
        inputs.vendor = test.vendor;
        inputs.renderer = test.renderer;
        const auto result = ClassifyGraphicsPreset(inputs);
        EXPECT_EQ(result.preset, test.expected);
        EXPECT_EQ(result.matchedModel, test.model);
        EXPECT_EQ(result.reason.identityRule, GraphicsIdentityRule::AllowlistedModel);
    }
    auto inputs = ReferenceHardware();
    inputs.vendor = " nViDiA\t Corporation\n";
    inputs.renderer = "GeForce(R) RTX (TM) 5070 ti/PCIe/SSE2";
    const auto result = ClassifyGraphicsPreset(inputs);
    EXPECT_EQ(result.normalizedVendor, "NVIDIA CORPORATION");
    EXPECT_EQ(result.normalizedRenderer, "GEFORCE RTX 5070 TI PCIE SSE2");
    EXPECT_EQ(result.normalizedVersion, "4 6 0 NVIDIA 580 0");
    EXPECT_EQ(result.preset, GraphicsPreset::Ultra);
}

TEST(GraphicsPresetClassifierTest, MesaDesktopDecorationsPreserveApprovedCeilingsWithoutNvx) {
    struct Case {
        std::string_view renderer;
        GraphicsPreset expected;
        std::string_view model;
    };
    constexpr std::array cases{
        Case{"AMD Radeon RX 6700 XT (navi22, LLVM 15.0.7, DRM 3.49, 6.1.0)",
             GraphicsPreset::High,
             "RX 6700 XT"},
        Case{"AMD Radeon RX 6700 XT (radeonsi, navi22, LLVM 17.0.6, DRM 3.54, 6.6.10)",
             GraphicsPreset::High,
             "RX 6700 XT"},
        Case{"AMD Radeon RX 5600 XT (navi10, LLVM 15.0.7, DRM 3.49, 6.1.0)",
             GraphicsPreset::Medium,
             "RX 5600 XT"},
        Case{"AMD Radeon RX 5600 XT (radeonsi, navi10, LLVM 17.0.6, DRM 3.54, 6.6.10)",
             GraphicsPreset::Medium,
             "RX 5600 XT"},
        Case{"AMD Radeon RX 580 Series (polaris10, LLVM 15.0.7, DRM 3.49, 6.1.0)",
             GraphicsPreset::Low,
             "RX 580"},
        Case{"AMD Radeon RX 580 Series (radeonsi, polaris10, LLVM 17.0.6, DRM 3.54, 6.6.10)",
             GraphicsPreset::Low,
             "RX 580"},
    };
    for (const auto& test : cases) {
        SCOPED_TRACE(test.renderer);
        auto inputs = ReferenceHardware();
        inputs.vendor = "AMD";
        inputs.renderer = test.renderer;
        inputs.version = "4.6 (Core Profile) Mesa 22.3.6";
        inputs.dedicatedVideoMemoryBytes.reset();
        const auto result = ClassifyGraphicsPreset(inputs);
        EXPECT_EQ(result.preset, test.expected);
        EXPECT_EQ(result.reason.gpuCeiling, test.expected);
        EXPECT_EQ(result.reason.cpuCeiling, GraphicsPreset::Ultra);
        EXPECT_EQ(result.reason.identityRule, GraphicsIdentityRule::AllowlistedModel);
        EXPECT_EQ(result.matchedModel, test.model);
        EXPECT_FALSE(result.reason.vramCeiling.has_value());
        EXPECT_TRUE(result.reason.gpuBinding);
        EXPECT_FALSE(result.reason.vramBinding);
    }
}

TEST(GraphicsPresetClassifierTest, MesaDistributionKernelsPreserveApprovedCeilingsWithoutNvx) {
    struct Case {
        std::string_view model;
        std::string_view chip;
        GraphicsPreset expected;
    };
    constexpr std::array cases{
        Case{"RX 6700 XT", "navi22", GraphicsPreset::High},
        Case{"RX 5600 XT", "navi10", GraphicsPreset::Medium},
        Case{"RX 580 Series", "polaris10", GraphicsPreset::Low},
    };
    for (const auto* kernel : kDistributionKernelReleases) {
        for (const auto& test : cases) {
            for (const auto* driver : {"", "radeonsi, "}) {
                auto inputs = ReferenceHardware();
                inputs.vendor = "AMD";
                inputs.renderer = "AMD Radeon " + std::string(test.model) + " (" + driver +
                                  std::string(test.chip) + ", LLVM 17.0.6, DRM 3.54, " + kernel +
                                  ")";
                inputs.version = "4.6 (Core Profile) Mesa 24.0.9";
                inputs.dedicatedVideoMemoryBytes.reset();
                SCOPED_TRACE(inputs.renderer);
                const auto result = ClassifyGraphicsPreset(inputs);
                EXPECT_EQ(result.preset, test.expected);
                EXPECT_EQ(result.reason.gpuCeiling, test.expected);
                EXPECT_EQ(result.reason.cpuCeiling, GraphicsPreset::Ultra);
                EXPECT_EQ(result.reason.identityRule, GraphicsIdentityRule::AllowlistedModel);
                EXPECT_EQ(result.matchedModel,
                          test.model == "RX 580 Series" ? "RX 580" : test.model);
                EXPECT_FALSE(result.reason.vramCeiling.has_value());
                EXPECT_TRUE(result.reason.gpuBinding);
                EXPECT_FALSE(result.reason.vramBinding);
            }
        }
    }
}

TEST(GraphicsPresetClassifierTest, KernelSuffixWordsAreNeverSoftwareRendererIdentity) {
    for (const auto* suffix : {"generic",
                               "software",
                               "llvmpipe",
                               "softpipe",
                               "SwiftShader",
                               "Microsoft.Basic.Render",
                               "Microsoft.Basic.Display"}) {
        auto inputs = ReferenceHardware();
        inputs.vendor = "AMD";
        inputs.renderer =
            std::string("AMD Radeon RX 6700 XT (navi22, LLVM 17.0.6, DRM 3.54, 6.8.0-") + suffix +
            ")";
        inputs.dedicatedVideoMemoryBytes.reset();
        SCOPED_TRACE(inputs.renderer);
        const auto result = ClassifyGraphicsPreset(inputs);
        EXPECT_EQ(result.preset, GraphicsPreset::High);
        EXPECT_EQ(result.reason.identityRule, GraphicsIdentityRule::AllowlistedModel);
        EXPECT_EQ(result.matchedModel, "RX 6700 XT");
    }
}

TEST(GraphicsPresetClassifierTest, MesaParsingRetainsCompleteNormalizedDiagnostics) {
    auto inputs = ReferenceHardware();
    inputs.vendor = "AMD";
    inputs.renderer = " AMD Radeon(TM) RX 6700 XT ( RaDeOnSi , NaVi22 , LLVM 17.0.6 , "
                      "DRM 3.54 , 6.8.0-83-generic ) \t";
    inputs.dedicatedVideoMemoryBytes.reset();
    const auto result = ClassifyGraphicsPreset(inputs);
    EXPECT_EQ(result.preset, GraphicsPreset::High);
    EXPECT_EQ(result.reason.identityRule, GraphicsIdentityRule::AllowlistedModel);
    EXPECT_EQ(result.normalizedRenderer,
              "AMD RADEON RX 6700 XT RADEONSI NAVI22 LLVM 17 0 6 DRM 3 54 6 8 0 83 GENERIC");
}

TEST(GraphicsPresetClassifierTest, SoftwareRendererIdentityStaysLowWithKernelMetadata) {
    for (const auto* renderer : {"llvmpipe", "softpipe", "SwiftShader", "Microsoft Basic Render"}) {
        for (const auto* kernel : kDistributionKernelReleases) {
            auto inputs = ReferenceHardware();
            inputs.vendor = "AMD";
            inputs.renderer = std::string(renderer) +
                              " RX 6700 XT (navi22, LLVM 17.0.6, DRM 3.54, " + kernel + ")";
            inputs.dedicatedVideoMemoryBytes.reset();
            SCOPED_TRACE(inputs.renderer);
            const auto result = ClassifyGraphicsPreset(inputs);
            EXPECT_EQ(result.preset, GraphicsPreset::Low);
            EXPECT_EQ(result.reason.identityRule, GraphicsIdentityRule::SoftwareOrGeneric);
            EXPECT_TRUE(result.matchedModel.empty());
        }
    }
}

TEST(GraphicsPresetClassifierTest, MesaKernelMetadataDoesNotAdmitModelVariants) {
    for (const auto* kernel : kDistributionKernelReleases) {
        for (const auto* variant : {"Laptop", "Mobile", "Max-Q", "M", "OEM"}) {
            const std::string decoration =
                std::string(" (radeonsi, navi22, LLVM 17.0.6, DRM 3.54, ") + kernel + ")";
            const std::array renderers{
                std::string("AMD Radeon RX 6700 XT ") + variant + decoration,
                "AMD Radeon RX 6700 XT" + decoration + " " + variant,
                std::string("AMD Radeon ") + variant + " RX 6700 XT" + decoration,
                "AMD Radeon RX 6700M XT" + decoration,
                "AMD Radeon RX 6700 XTM" + decoration,
            };
            for (const auto& renderer : renderers) {
                auto inputs = ReferenceHardware();
                inputs.vendor = "AMD";
                inputs.renderer = renderer;
                SCOPED_TRACE(renderer);
                const auto result = ClassifyGraphicsPreset(inputs);
                EXPECT_EQ(result.preset, GraphicsPreset::Low);
                EXPECT_EQ(result.reason.identityRule, GraphicsIdentityRule::UnknownModel);
                EXPECT_TRUE(result.matchedModel.empty());
            }
        }
    }
}

TEST(GraphicsPresetClassifierTest, MesaDecorationsRequireStructuredFields) {
    constexpr std::array decorations{
        "navi22 LLVM 17.0.6 DRM 3.54 6.8.0",
        "navi22, DRM 3.54, LLVM 17.0.6, 6.8.0",
        "LLVM 17.0.6, DRM 3.54, 6.8.0",
        "radeonsi, LLVM 17.0.6, DRM 3.54, 6.8.0",
        "navi22, LLVM 17.0, DRM 3.54, 6.8.0",
        "navi22, LLVM 17..6, DRM 3.54, 6.8.0",
        "navi22, LLVM 17.0.6, DRM 3, 6.8.0",
        "navi22, LLVM 17.0.6, DRM 3..54, 6.8.0",
        "navi22, LLVM 17.0.6, DRM 3.54, generic",
        "navi22, LLVM 17.0.6, DRM 3.54, 6.8.0 generic",
        "navi22, LLVM 17.0.6, DRM 3.54, 6.8.0-83-generic OEM",
        "navi22, LLVM 17.0.6, DRM 3.54, 6.8.0/83-generic",
        "navi22, LLVM 17.0.6, DRM 3.54, 6.8.0-83-generic, OEM",
        "navi22, LLVM 17.0.6, DRM 3.54, 6.8.0-83-generic,",
        "navi22, LLVM 17.0.6, DRM 3.54,",
        "navi22, LLVM 17.0.6, DRM 3.54, 6.8.0-83-generic)",
    };
    for (const auto* decoration : decorations) {
        auto inputs = ReferenceHardware();
        inputs.renderer = std::string("AMD Radeon RX 6700 XT (") + decoration + ")";
        SCOPED_TRACE(inputs.renderer);
        const auto result = ClassifyGraphicsPreset(inputs);
        EXPECT_EQ(result.preset, GraphicsPreset::Low);
        EXPECT_EQ(result.reason.identityRule, GraphicsIdentityRule::UnknownModel);
        EXPECT_TRUE(result.matchedModel.empty());
    }
}

TEST(GraphicsPresetClassifierTest, NvidiaAndAngleDesktopDecorationsPreserveApprovedCeilings) {
    struct Case {
        std::string_view vendor;
        std::string_view renderer;
        GraphicsPreset expected;
        std::string_view model;
    };
    constexpr std::array cases{
        Case{"NVIDIA Corporation",
             "NVIDIA GeForce RTX 3070/PCIe/SSE2",
             GraphicsPreset::High,
             "RTX 3070"},
        Case{"NVIDIA Corporation",
             "NVIDIA GeForce RTX 5070 Ti/PCIe/SSE2",
             GraphicsPreset::Ultra,
             "RTX 5070 TI"},
        Case{"Google Inc. (NVIDIA)",
             "ANGLE (NVIDIA, NVIDIA GeForce RTX 3060 Ti (0x00002489) Direct3D11 vs_5_0 ps_5_0, "
             "D3D11)",
             GraphicsPreset::High,
             "RTX 3060 TI"},
        Case{"Google Inc. (AMD)",
             "ANGLE (AMD, AMD Radeon RX 6700 XT (0x000073DF) Direct3D11 vs_5_0 ps_5_0, D3D11)",
             GraphicsPreset::High,
             "RX 6700 XT"},
    };
    for (const auto& test : cases) {
        SCOPED_TRACE(test.renderer);
        auto inputs = ReferenceHardware();
        inputs.vendor = test.vendor;
        inputs.renderer = test.renderer;
        inputs.dedicatedVideoMemoryBytes.reset();
        const auto result = ClassifyGraphicsPreset(inputs);
        EXPECT_EQ(result.preset, test.expected);
        EXPECT_EQ(result.reason.gpuCeiling, test.expected);
        EXPECT_EQ(result.reason.identityRule, GraphicsIdentityRule::AllowlistedModel);
        EXPECT_EQ(result.matchedModel, test.model);
        EXPECT_FALSE(result.reason.vramCeiling.has_value());
    }
}

TEST(GraphicsPresetClassifierTest, MalformedDriverDecorationsDoNotAdmitUnlistedModels) {
    constexpr std::array renderers{
        "AMD Radeon RX 6700 XT (navi22M, LLVM 15.0.7, DRM 3.49, 6.1.0)",
        "AMD Radeon RX 6700 XT (radeonsi, navi22 OEM, LLVM 15.0.7, DRM 3.49, 6.1.0)",
        "AMD Radeon RX 580 Series (polaris10Mobile, LLVM 15.0.7, DRM 3.49, 6.1.0)",
        "AMD Radeon RX 6700 XT (navi, LLVM 15.0.7, DRM 3.49, 6.1.0)",
        "AMD Radeon RX 580 Series (polaris, LLVM 15.0.7, DRM 3.49, 6.1.0)",
        "ANGLE (NVIDIA, NVIDIA GeForce RTX 3060 Ti (0x00002489M) Direct3D11 vs_5_0 ps_5_0)",
        "ANGLE (NVIDIA, NVIDIA GeForce RTX 3060 Ti (0x0000248Z) Direct3D11 vs_5_0 ps_5_0)",
        "ANGLE (NVIDIA, NVIDIA GeForce RTX 3060 Ti (0x2489) Direct3D11 vs_5_0 ps_5_0)",
        "ANGLE (NVIDIA, NVIDIA GeForce RTX 3060 Ti (0x00002489) OEM Direct3D11 vs_5_0 ps_5_0)",
        "ANGLE (NVIDIA, NVIDIA GeForce RTX 3060 Ti (0x00002489))",
    };
    for (const auto* renderer : renderers) {
        SCOPED_TRACE(renderer);
        auto inputs = ReferenceHardware();
        inputs.renderer = renderer;
        const auto result = ClassifyGraphicsPreset(inputs);
        EXPECT_EQ(result.preset, GraphicsPreset::Low);
        EXPECT_EQ(result.reason.gpuCeiling, GraphicsPreset::Low);
        EXPECT_EQ(result.reason.identityRule, GraphicsIdentityRule::UnknownModel);
        EXPECT_TRUE(result.matchedModel.empty());
    }
}

TEST(GraphicsPresetClassifierTest, SoftwareAndGenericRenderersAlwaysUseLow) {
    constexpr std::array renderers{
        "llvmpipe (LLVM 15.0.7, 256 bits)",
        "softpipe",
        "Google SwiftShader",
        "ANGLE (Google, Vulkan 1.3.0 (SwiftShader Device (Subzero)), SwiftShader driver)",
        "Microsoft Basic Render Driver",
        "Microsoft Basic Display Adapter",
        "GDI Generic",
        "Generic Renderer",
        "Software Rasterizer RTX 5070 Ti",
        "llvmpipe RTX 5070 Ti",
    };
    for (const auto* renderer : renderers) {
        SCOPED_TRACE(renderer);
        auto inputs = ReferenceHardware();
        inputs.renderer = renderer;
        const auto result = ClassifyGraphicsPreset(inputs);
        EXPECT_EQ(result.preset, GraphicsPreset::Low);
        EXPECT_EQ(result.reason.identityRule, GraphicsIdentityRule::SoftwareOrGeneric);
        EXPECT_TRUE(result.reason.gpuBinding);
        EXPECT_TRUE(result.matchedModel.empty());
    }
    auto inputs = ReferenceHardware();
    inputs.vendor = "Software";
    EXPECT_EQ(ClassifyGraphicsPreset(inputs).reason.identityRule,
              GraphicsIdentityRule::SoftwareOrGeneric);
}

TEST(GraphicsPresetClassifierTest, UnknownOrMissingIdentityUsesLow) {
    auto inputs = ReferenceHardware();
    inputs.renderer = "GeForce RTX 5090";
    auto result = ClassifyGraphicsPreset(inputs);
    EXPECT_EQ(result.preset, GraphicsPreset::Low);
    EXPECT_EQ(result.reason.identityRule, GraphicsIdentityRule::UnknownModel);
    for (int field = 0; field < 3; ++field) {
        inputs = ReferenceHardware();
        if (field == 0) {
            inputs.vendor.clear();
        } else if (field == 1) {
            inputs.renderer.clear();
        } else {
            inputs.version.clear();
        }
        result = ClassifyGraphicsPreset(inputs);
        EXPECT_EQ(result.preset, GraphicsPreset::Low);
        EXPECT_EQ(result.reason.identityRule, GraphicsIdentityRule::MissingIdentity);
    }
    EXPECT_EQ(ClassifyGraphicsPreset({}).preset, GraphicsPreset::Low);
    inputs = ReferenceHardware();
    inputs.renderer = " \t()/\n";
    EXPECT_EQ(ClassifyGraphicsPreset(inputs).reason.identityRule,
              GraphicsIdentityRule::MissingIdentity);
}

TEST(GraphicsPresetClassifierTest, RequiresWholeModelTokensAndExactVariant) {
    constexpr std::array unknownModels{
        "RTX 3060",
        "RTX 3060 Titanium",
        "RTX 3060 TiX",
        "RTX 30600 Ti",
        "RTX 3060 Ti Super",
        "GTX 10600",
        "GTX 1060X",
        "AGTX 1060",
        "RTX 3070 Ti",
        "RTX 2060 Super",
        "RX 5800",
        "RX 6700",
        "RX 6700 XTX",
        "RTX 5070",
        "GTX 1660",
        "GTX 1660 SuperX",
    };
    for (const auto* renderer : unknownModels) {
        SCOPED_TRACE(renderer);
        auto inputs = ReferenceHardware();
        inputs.renderer = renderer;
        const auto result = ClassifyGraphicsPreset(inputs);
        EXPECT_EQ(result.preset, GraphicsPreset::Low);
        EXPECT_EQ(result.reason.identityRule, GraphicsIdentityRule::UnknownModel);
        EXPECT_TRUE(result.matchedModel.empty());
    }
    auto inputs = ReferenceHardware();
    inputs.renderer = "RTX 3060 Ti";
    EXPECT_EQ(ClassifyGraphicsPreset(inputs).preset, GraphicsPreset::High);
    inputs.renderer = "RTX 3060 Ti/PCIe/SSE2";
    EXPECT_EQ(ClassifyGraphicsPreset(inputs).matchedModel, "RTX 3060 TI");
    inputs.renderer = "Unknown GPU";
    inputs.version = "RTX 5070 Ti";
    EXPECT_EQ(ClassifyGraphicsPreset(inputs).reason.identityRule,
              GraphicsIdentityRule::UnknownModel);
}

void ExpectUnlistedVariantsUseLow(std::string_view variant) {
    constexpr std::array models{"GTX 1060",
                                "RX 580",
                                "GTX 1660 Super",
                                "RTX 2060",
                                "RX 5600 XT",
                                "RTX 3060 Ti",
                                "RTX 3070",
                                "RX 6700 XT",
                                "RTX 5070 Ti"};
    for (const auto* model : models) {
        const std::string base(model);
        const std::string suffix(variant);
        const std::array renderers{
            base + " " + suffix,
            "NVIDIA GeForce " + base + " " + suffix + " GPU/PCIe/SSE2",
            "AMD Radeon " + suffix + " " + base,
            base + " GPU " + suffix,
            base + "/PCIe/SSE2 " + suffix,
            "ANGLE (NVIDIA, NVIDIA GeForce " + base + " " + suffix +
                " Direct3D11 vs_5_0 ps_5_0, D3D11)",
            "ANGLE (NVIDIA, NVIDIA GeForce " + base + " " + suffix +
                " (0x00002489) Direct3D11 vs_5_0 ps_5_0, D3D11)",
            "ANGLE (NVIDIA, NVIDIA GeForce " + base + " (0x00002489) Direct3D11 vs_5_0 ps_5_0, " +
                suffix + ", D3D11)",
            "AMD Radeon " + base + " " + suffix + " (navi22, LLVM 15.0.7, DRM 3.49, 6.1.0)",
            "AMD Radeon " + base + " " + suffix +
                " (radeonsi, navi22, LLVM 17.0.6, DRM 3.54, 6.6.10)",
            "AMD Radeon " + base + " (radeonsi, navi22, LLVM 17.0.6, DRM 3.54, 6.6.10) " + suffix,
            "AMD Radeon " + base + " (polaris10, LLVM 15.0.7, DRM 3.49, 6.1.0) " + suffix,
            "RTX 5070 Ti / " + base + " " + suffix,
        };
        for (const auto& renderer : renderers) {
            SCOPED_TRACE(renderer);
            auto inputs = ReferenceHardware();
            inputs.renderer = renderer;
            const auto result = ClassifyGraphicsPreset(inputs);
            EXPECT_EQ(result.preset, GraphicsPreset::Low);
            EXPECT_EQ(result.reason.gpuCeiling, GraphicsPreset::Low);
            EXPECT_EQ(result.reason.identityRule, GraphicsIdentityRule::UnknownModel);
            EXPECT_TRUE(result.matchedModel.empty());
            EXPECT_TRUE(result.reason.gpuBinding);
        }
    }
}

TEST(GraphicsPresetClassifierTest, UnlistedLaptopVariantsUseLow) {
    ExpectUnlistedVariantsUseLow("Laptop");
}

TEST(GraphicsPresetClassifierTest, UnlistedMobileVariantsUseLow) {
    ExpectUnlistedVariantsUseLow("Mobile");
}

TEST(GraphicsPresetClassifierTest, UnlistedMaxQVariantsUseLow) {
    ExpectUnlistedVariantsUseLow("Max-Q");
    ExpectUnlistedVariantsUseLow("Max Q Design");
    ExpectUnlistedVariantsUseLow("MaxQ");
}

TEST(GraphicsPresetClassifierTest, UnlistedMSuffixVariantsUseLow) {
    ExpectUnlistedVariantsUseLow("M");
    constexpr std::array models{"GTX 1060M",
                                "RX 580M",
                                "GTX 1660M Super",
                                "RTX 2060M",
                                "RX 5600M XT",
                                "RTX 3060M Ti",
                                "RTX 3070M",
                                "RX 6700M XT",
                                "RTX 5070M Ti",
                                "GTX 1660 SuperM",
                                "RTX 3060 TiM",
                                "RX 5600 XTM",
                                "RX 6700 XTM",
                                "RTX 5070 TiM"};
    for (const auto* model : models) {
        const std::string base(model);
        const std::array renderers{
            base,
            "NVIDIA GeForce " + base + "/PCIe/SSE2",
            "AMD Radeon " + base + " (navi22, LLVM 15.0.7, DRM 3.49, 6.1.0)",
            "AMD Radeon " + base + " (radeonsi, navi22, LLVM 17.0.6, DRM 3.54, 6.6.10)",
            "ANGLE (NVIDIA, NVIDIA GeForce " + base +
                " (0x00002489) Direct3D11 vs_5_0 ps_5_0, D3D11)",
        };
        for (const auto& renderer : renderers) {
            SCOPED_TRACE(renderer);
            auto inputs = ReferenceHardware();
            inputs.renderer = renderer;
            const auto result = ClassifyGraphicsPreset(inputs);
            EXPECT_EQ(result.preset, GraphicsPreset::Low);
            EXPECT_EQ(result.reason.identityRule, GraphicsIdentityRule::UnknownModel);
            EXPECT_TRUE(result.matchedModel.empty());
        }
    }
}

TEST(GraphicsPresetClassifierTest, UnlistedOemAndOtherVariantsUseLow) {
    for (const auto* variant : {"OEM",
                                "OEM Edition",
                                "Dell",
                                "ASUS",
                                "Founders Edition",
                                "Engineering Sample",
                                "SE",
                                "LE",
                                "Refresh"}) {
        ExpectUnlistedVariantsUseLow(variant);
    }
}

TEST(GraphicsPresetClassifierTest, BoundsIdentityWithoutAcceptingTruncatedModels) {
    auto inputs = ReferenceHardware();
    inputs.renderer = "RTX 5070 Ti";
    inputs.renderer.resize(kMaxGraphicsIdentityBytes, ' ');
    EXPECT_EQ(ClassifyGraphicsPreset(inputs).preset, GraphicsPreset::Ultra);
    inputs.renderer += 'X';
    const auto result = ClassifyGraphicsPreset(inputs);
    EXPECT_EQ(result.preset, GraphicsPreset::Low);
    EXPECT_EQ(result.reason.identityRule, GraphicsIdentityRule::MissingIdentity);
    EXPECT_TRUE(result.normalizedRenderer.empty());
    inputs = ReferenceHardware();
    inputs.vendor.resize(kMaxGraphicsIdentityBytes + 1, ' ');
    EXPECT_EQ(ClassifyGraphicsPreset(inputs).preset, GraphicsPreset::Low);
    inputs = ReferenceHardware();
    inputs.version.resize(kMaxGraphicsIdentityBytes + 1, ' ');
    EXPECT_EQ(ClassifyGraphicsPreset(inputs).preset, GraphicsPreset::Low);
}

TEST(GraphicsPresetClassifierTest, ZeroConcurrencyAndAllCpuBoundaries) {
    struct Case {
        unsigned int count;
        GraphicsPreset expected;
    };
    constexpr std::array cases{
        Case{0, GraphicsPreset::Low},
        Case{1, GraphicsPreset::Low},
        Case{4, GraphicsPreset::Low},
        Case{5, GraphicsPreset::Medium},
        Case{7, GraphicsPreset::Medium},
        Case{8, GraphicsPreset::High},
        Case{15, GraphicsPreset::High},
        Case{16, GraphicsPreset::Ultra},
        Case{std::numeric_limits<unsigned int>::max(), GraphicsPreset::Ultra},
    };
    for (const auto& test : cases) {
        SCOPED_TRACE(test.count);
        auto inputs = ReferenceHardware();
        inputs.logicalProcessorCount = test.count;
        const auto result = ClassifyGraphicsPreset(inputs);
        EXPECT_EQ(result.preset, test.expected);
        EXPECT_EQ(result.reason.cpuCeiling, test.expected);
        EXPECT_TRUE(result.reason.cpuBinding);
    }
}

TEST(GraphicsPresetClassifierTest, VramByteBoundariesUseBinaryGiB) {
    struct Case {
        std::uint64_t bytes;
        GraphicsPreset expected;
    };
    constexpr std::array cases{
        Case{0, GraphicsPreset::Low},
        Case{3 * kGiB - 1, GraphicsPreset::Low},
        Case{3 * kGiB, GraphicsPreset::Medium},
        Case{6 * kGiB - 1, GraphicsPreset::Medium},
        Case{6 * kGiB, GraphicsPreset::High},
        Case{8 * kGiB - 1, GraphicsPreset::High},
        Case{8 * kGiB, GraphicsPreset::Ultra},
        Case{std::numeric_limits<std::uint64_t>::max(), GraphicsPreset::Ultra},
    };
    for (const auto& test : cases) {
        SCOPED_TRACE(test.bytes);
        auto inputs = ReferenceHardware();
        inputs.dedicatedVideoMemoryBytes = test.bytes;
        const auto result = ClassifyGraphicsPreset(inputs);
        EXPECT_EQ(result.preset, test.expected);
        EXPECT_EQ(result.reason.vramCeiling, test.expected);
        EXPECT_TRUE(result.reason.vramBinding);
    }
}

TEST(GraphicsPresetClassifierTest, ConvertsNvxKilobytesBeforeClassifying) {
    constexpr std::int64_t kibPerGiB = 1024 * 1024;
    for (const std::int64_t boundary : {3, 6, 8}) {
        auto inputs = ReferenceHardware();
        inputs.dedicatedVideoMemoryBytes =
            DedicatedVideoMemoryBytesFromNvx(true, boundary * kibPerGiB);
        ASSERT_TRUE(inputs.dedicatedVideoMemoryBytes.has_value());
        if (inputs.dedicatedVideoMemoryBytes.has_value()) {
            EXPECT_EQ(*inputs.dedicatedVideoMemoryBytes,
                      static_cast<std::uint64_t>(boundary) * kGiB);
        }
        EXPECT_EQ(ClassifyGraphicsPreset(inputs).preset,
                  boundary == 3 ? GraphicsPreset::Medium
                                : (boundary == 6 ? GraphicsPreset::High : GraphicsPreset::Ultra));
        inputs.dedicatedVideoMemoryBytes =
            DedicatedVideoMemoryBytesFromNvx(true, boundary * kibPerGiB - 1);
        ASSERT_TRUE(inputs.dedicatedVideoMemoryBytes.has_value());
        if (inputs.dedicatedVideoMemoryBytes.has_value()) {
            EXPECT_EQ(*inputs.dedicatedVideoMemoryBytes,
                      static_cast<std::uint64_t>(boundary) * kGiB - 1024);
        }
        EXPECT_EQ(ClassifyGraphicsPreset(inputs).preset,
                  boundary == 3 ? GraphicsPreset::Low
                                : (boundary == 6 ? GraphicsPreset::Medium : GraphicsPreset::High));
    }
    EXPECT_EQ(DedicatedVideoMemoryBytesFromNvx(true, 1), 1024u);
    EXPECT_EQ(DedicatedVideoMemoryBytesFromNvx(true, std::numeric_limits<std::int32_t>::max()),
              static_cast<std::uint64_t>(std::numeric_limits<std::int32_t>::max()) * 1024);
    EXPECT_FALSE(DedicatedVideoMemoryBytesFromNvx(true, 0).has_value());
    EXPECT_FALSE(DedicatedVideoMemoryBytesFromNvx(true, -1).has_value());
    EXPECT_FALSE(DedicatedVideoMemoryBytesFromNvx(true, std::numeric_limits<std::int64_t>::max())
                     .has_value());
}

TEST(GraphicsPresetClassifierTest, MissingExtensionAddsNoVramCeiling) {
    auto inputs = ReferenceHardware();
    // A small free-pool value from another extension is not dedicated capacity.
    inputs.dedicatedVideoMemoryBytes = DedicatedVideoMemoryBytesFromNvx(false, 1024);
    const auto result = ClassifyGraphicsPreset(inputs);
    EXPECT_FALSE(inputs.dedicatedVideoMemoryBytes.has_value());
    EXPECT_EQ(result.preset, GraphicsPreset::Ultra);
    EXPECT_FALSE(result.reason.vramCeiling.has_value());
    EXPECT_FALSE(result.reason.vramBinding);
    EXPECT_FALSE(DedicatedVideoMemoryBytesFromNvx(false, 16 * 1024 * 1024).has_value());
}

TEST(GraphicsPresetClassifierTest, TakesMinimumAcrossAllCeilingCombinations) {
    constexpr std::array models{"GTX 1060", "RTX 2060", "RTX 3070", "RTX 5070 Ti"};
    constexpr std::array counts{4u, 7u, 15u, 16u};
    constexpr std::array memory{2 * kGiB, 4 * kGiB, 6 * kGiB, 8 * kGiB};
    for (std::size_t gpu = 0; gpu < models.size(); ++gpu) {
        for (std::size_t cpu = 0; cpu < counts.size(); ++cpu) {
            for (std::size_t vram = 0; vram < memory.size(); ++vram) {
                SCOPED_TRACE(testing::Message() << gpu << ',' << cpu << ',' << vram);
                auto inputs = ReferenceHardware();
                inputs.renderer = models[gpu];
                inputs.logicalProcessorCount = counts[cpu];
                inputs.dedicatedVideoMemoryBytes = memory[vram];
                const auto result = ClassifyGraphicsPreset(inputs);
                const std::size_t expected = std::min({gpu, cpu, vram});
                EXPECT_EQ(result.preset, static_cast<GraphicsPreset>(expected));
                EXPECT_EQ(result.reason.gpuBinding, gpu == expected);
                EXPECT_EQ(result.reason.cpuBinding, cpu == expected);
                EXPECT_EQ(result.reason.vramBinding, vram == expected);
            }
        }
    }
}

TEST(GraphicsPresetClassifierTest, RepeatedCallsReturnIdenticalStructuredReasons) {
    const auto inputs = ReferenceHardware();
    const auto expected = ClassifyGraphicsPreset(inputs);
    EXPECT_EQ(expected.rulesRevision, 1u);
    EXPECT_EQ(expected.rulesRevision, kGraphicsPresetRulesRevision);
    for (int iteration = 0; iteration < 100; ++iteration) {
        auto unrelated = ReferenceHardware();
        unrelated.logicalProcessorCount = 0;
        EXPECT_EQ(ClassifyGraphicsPreset(unrelated).preset, GraphicsPreset::Low);
        EXPECT_EQ(ClassifyGraphicsPreset(inputs), expected);
    }
    EXPECT_EQ(inputs.vendor, "NVIDIA Corporation");
    EXPECT_EQ(inputs.renderer, "NVIDIA GeForce RTX 5070 Ti/PCIe/SSE2");
}

TEST(GraphicsPresetClassifierTest, FramebufferSizeIsDiagnosticOnly) {
    auto inputs = ReferenceHardware();
    const auto expected = ClassifyGraphicsPreset(inputs);
    for (const int size : {0, 1, 1080, 2160, std::numeric_limits<int>::max()}) {
        inputs.framebufferWidth = size;
        inputs.framebufferHeight = size;
        EXPECT_EQ(ClassifyGraphicsPreset(inputs), expected);
    }
}

TEST(GraphicsPresetClassifierTest, AmbiguousMultiModelIdentityUsesLowestKnownTier) {
    auto inputs = ReferenceHardware();
    inputs.renderer = "RTX 5070 Ti / GTX 1660 Super";
    EXPECT_EQ(ClassifyGraphicsPreset(inputs).preset, GraphicsPreset::Medium);
    EXPECT_EQ(ClassifyGraphicsPreset(inputs).matchedModel, "GTX 1660 SUPER");
    inputs.renderer = "GTX 1660 Super / RTX 5070 Ti";
    EXPECT_EQ(ClassifyGraphicsPreset(inputs).preset, GraphicsPreset::Medium);
}

TEST(GraphicsPresetClassifierTest, SuppliesStableDisplayNames) {
    EXPECT_EQ(GraphicsPresetName(GraphicsPreset::Low), "Low");
    EXPECT_EQ(GraphicsPresetName(GraphicsPreset::Medium), "Medium");
    EXPECT_EQ(GraphicsPresetName(GraphicsPreset::High), "High");
    EXPECT_EQ(GraphicsPresetName(GraphicsPreset::Ultra), "Ultra");
    EXPECT_EQ(GraphicsIdentityRuleName(GraphicsIdentityRule::MissingIdentity), "missing_identity");
    EXPECT_EQ(GraphicsIdentityRuleName(GraphicsIdentityRule::SoftwareOrGeneric),
              "software_or_generic");
    EXPECT_EQ(GraphicsIdentityRuleName(GraphicsIdentityRule::UnknownModel), "unknown_model");
    EXPECT_EQ(GraphicsIdentityRuleName(GraphicsIdentityRule::AllowlistedModel),
              "allowlisted_model");
}

} // namespace
