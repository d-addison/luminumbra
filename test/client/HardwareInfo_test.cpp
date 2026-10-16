#include "luminumbra_client/core/HardwareInfo.h"

#include <GLFW/glfw3.h>
#include <glad/glad.h>
#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <deque>
#include <limits>
#include <string>
#include <thread>
#include <vector>

struct GLFWwindow {};

namespace {

using Luminumbra::Client::DetectHardwareInfo;
using Luminumbra::Client::FormatHardwareDiagnostics;
using luminumbra::core::GraphicsIdentityRule;
using luminumbra::core::GraphicsPreset;
constexpr std::uint64_t kGiB = 1024ULL * 1024 * 1024;

struct ProbeState {
    std::array<std::string, 3> identities{
        "NVIDIA Corporation", "NVIDIA GeForce RTX 5070 Ti/PCIe/SSE2", "4.6.0"};
    std::array<bool, 3> nullIdentities{};
    std::array<std::vector<GLenum>, 3> identityErrors{};
    std::deque<GLenum> pendingErrors;
    std::vector<GLenum> extensionErrors;
    std::vector<GLenum> memoryErrors;
    bool nvxAdvertised = true;
    GLint memoryKB = 16 * 1024 * 1024;
    int memoryQueries = 0;
    int extensionQueries = 0;
    int identityQueries = 0;
};

ProbeState probes;

class HardwareInfoTest : public testing::Test {
protected:
    void SetUp() override {
        probes = ProbeState{};
    }

    void TearDown() override {
        EXPECT_TRUE(probes.pendingErrors.empty());
    }

    Luminumbra::Client::HardwareInfo Detect() {
        return DetectHardwareInfo(&m_window);
    }

private:
    GLFWwindow m_window;
};

TEST_F(HardwareInfoTest, CollectsIdentityCapacityAndFramebufferThroughPublicEntryPoint) {
    const auto info = Detect();
    EXPECT_EQ(info.inputs.vendor, probes.identities[0]);
    EXPECT_EQ(info.inputs.renderer, probes.identities[1]);
    EXPECT_EQ(info.inputs.version, probes.identities[2]);
    EXPECT_EQ(info.inputs.logicalProcessorCount, std::thread::hardware_concurrency());
    EXPECT_EQ(info.inputs.framebufferWidth, 2560);
    EXPECT_EQ(info.inputs.framebufferHeight, 1440);
    EXPECT_TRUE(info.nvxMemoryInfoAdvertised);
    EXPECT_EQ(info.inputs.dedicatedVideoMemoryBytes, 16 * kGiB);
    EXPECT_EQ(info.recommendation.normalizedRenderer, "NVIDIA GEFORCE RTX 5070 TI PCIE SSE2");
    EXPECT_EQ(info.recommendation.reason.identityRule, GraphicsIdentityRule::AllowlistedModel);
    EXPECT_EQ(info.recommendation.reason.vramCeiling, GraphicsPreset::Ultra);
    EXPECT_EQ(probes.identityQueries, 3);
    EXPECT_EQ(probes.extensionQueries, 1);
    EXPECT_EQ(probes.memoryQueries, 1);
    EXPECT_EQ(glGetError(), GL_NO_ERROR);
}

TEST_F(HardwareInfoTest, DiagnosticsDefaultOffWhileCollectionAndRecommendationRemainAvailable) {
    const auto info = Detect();
    const auto recommendation = info.recommendation;
    EXPECT_TRUE(FormatHardwareDiagnostics(info).empty());
    EXPECT_TRUE(FormatHardwareDiagnostics(info, false).empty());
    EXPECT_EQ(info.inputs.renderer, probes.identities[1]);
    EXPECT_EQ(info.inputs.dedicatedVideoMemoryBytes, 16 * kGiB);
    EXPECT_EQ(info.recommendation.reason.identityRule, GraphicsIdentityRule::AllowlistedModel);
    EXPECT_EQ(info.recommendation, recommendation);
    EXPECT_EQ(probes.identityQueries, 3);
    EXPECT_EQ(probes.extensionQueries, 1);
    EXPECT_EQ(probes.memoryQueries, 1);
}

TEST_F(HardwareInfoTest, ExplicitDiagnosticOptInFormatsCollectedRecommendationWithoutNewProbes) {
    auto info = Detect();
    // Make the expected message independent of the machine's processor count.
    info.inputs.logicalProcessorCount = 16;
    info.recommendation = luminumbra::core::ClassifyGraphicsPreset(info.inputs);
    const auto recommendation = info.recommendation;
    EXPECT_EQ(FormatHardwareDiagnostics(info, true),
              "Hardware: vendor='NVIDIA CORPORATION' renderer='NVIDIA GEFORCE RTX 5070 TI PCIE "
              "SSE2' version='4 6 0' logical_processors=16 nvx_memory_info=true "
              "dedicated_vram_bytes=17179869184 framebuffer=2560x1440 recommended_preset=Ultra "
              "rules_revision=1 identity_rule=allowlisted_model model='RTX 5070 TI' "
              "ceilings(gpu=Ultra,cpu=Ultra,vram=Ultra) binding(gpu=true,cpu=true,vram=true)");
    EXPECT_EQ(info.recommendation, recommendation);
    EXPECT_TRUE(FormatHardwareDiagnostics(info).empty());
    EXPECT_EQ(probes.identityQueries, 3);
    EXPECT_EQ(probes.extensionQueries, 1);
    EXPECT_EQ(probes.memoryQueries, 1);
}

TEST_F(HardwareInfoTest, OptedInDiagnosticsRepresentUnavailableMemoryAsUnknown) {
    probes.nvxAdvertised = false;
    const auto info = Detect();
    const auto diagnostics = FormatHardwareDiagnostics(info, true);
    EXPECT_NE(diagnostics.find("nvx_memory_info=false dedicated_vram_bytes=unknown"),
              std::string::npos);
    EXPECT_NE(diagnostics.find(",vram=unknown)"), std::string::npos);
    EXPECT_NE(diagnostics.find(",vram=false)"), std::string::npos);
    EXPECT_FALSE(info.inputs.dedicatedVideoMemoryBytes.has_value());
    EXPECT_FALSE(info.recommendation.reason.vramCeiling.has_value());
    EXPECT_EQ(probes.memoryQueries, 0);
}

TEST_F(HardwareInfoTest, CollectsMesaDesktopIdentityWithoutNvxMemory) {
    probes.identities = {"AMD",
                         "AMD Radeon RX 6700 XT (navi22, LLVM 15.0.7, DRM 3.49, 6.1.0)",
                         "4.6 (Core Profile) Mesa 22.3.6"};
    probes.nvxAdvertised = false;
    const auto info = Detect();
    EXPECT_EQ(info.inputs.renderer, probes.identities[1]);
    EXPECT_EQ(info.recommendation.reason.identityRule, GraphicsIdentityRule::AllowlistedModel);
    EXPECT_EQ(info.recommendation.reason.gpuCeiling, GraphicsPreset::High);
    EXPECT_EQ(info.recommendation.matchedModel, "RX 6700 XT");
    EXPECT_FALSE(info.inputs.dedicatedVideoMemoryBytes.has_value());
    EXPECT_FALSE(info.recommendation.reason.vramCeiling.has_value());
    EXPECT_EQ(probes.memoryQueries, 0);
    EXPECT_TRUE(FormatHardwareDiagnostics(info).empty());
}

TEST_F(HardwareInfoTest, DrainsAllStaleErrorsBeforeProbing) {
    probes.pendingErrors = {GL_INVALID_ENUM, GL_INVALID_OPERATION, GL_INVALID_VALUE};
    const auto info = Detect();
    EXPECT_EQ(info.inputs.dedicatedVideoMemoryBytes, 16 * kGiB);
    EXPECT_EQ(info.recommendation.reason.identityRule, GraphicsIdentityRule::AllowlistedModel);
    EXPECT_EQ(glGetError(), GL_NO_ERROR);
}

TEST_F(HardwareInfoTest, FailedMemoryProbeRejectsEvenPositiveOutputAndDrainsEveryError) {
    for (const auto& errors : {std::vector<GLenum>{GL_INVALID_ENUM},
                               std::vector<GLenum>{GL_INVALID_ENUM, GL_INVALID_OPERATION}}) {
        probes.memoryErrors = errors;
        const auto info = Detect();
        EXPECT_TRUE(info.nvxMemoryInfoAdvertised);
        EXPECT_FALSE(info.inputs.dedicatedVideoMemoryBytes.has_value());
        EXPECT_FALSE(info.recommendation.reason.vramCeiling.has_value());
        EXPECT_EQ(glGetError(), GL_NO_ERROR);
        EXPECT_TRUE(probes.pendingErrors.empty());
    }
}

TEST_F(HardwareInfoTest, MissingNvxAdvertisementNeverQueriesCapacity) {
    probes.nvxAdvertised = false;
    const auto info = Detect();
    EXPECT_FALSE(info.nvxMemoryInfoAdvertised);
    EXPECT_EQ(probes.memoryQueries, 0);
    EXPECT_FALSE(info.inputs.dedicatedVideoMemoryBytes.has_value());
    EXPECT_FALSE(info.recommendation.reason.vramCeiling.has_value());
    EXPECT_EQ(glGetError(), GL_NO_ERROR);
}

TEST_F(HardwareInfoTest, FailedExtensionProbeCannotAuthorizeMemoryQuery) {
    for (const bool advertised : {false, true}) {
        probes.nvxAdvertised = advertised;
        probes.extensionErrors = {GL_INVALID_ENUM, GL_INVALID_OPERATION};
        const auto info = Detect();
        EXPECT_FALSE(info.nvxMemoryInfoAdvertised);
        EXPECT_EQ(probes.memoryQueries, 0);
        EXPECT_FALSE(info.inputs.dedicatedVideoMemoryBytes.has_value());
        EXPECT_EQ(glGetError(), GL_NO_ERROR);
        EXPECT_TRUE(probes.pendingErrors.empty());
    }
}

TEST_F(HardwareInfoTest, FailedIdentityProbeIsUnavailableAndDoesNotPoisonLaterProbes) {
    for (std::size_t field = 0; field < probes.identities.size(); ++field) {
        probes.identityErrors = {};
        probes.identityErrors[field] = {GL_INVALID_ENUM, GL_INVALID_OPERATION};
        const auto info = Detect();
        const std::array collected{info.inputs.vendor, info.inputs.renderer, info.inputs.version};
        for (std::size_t index = 0; index < collected.size(); ++index) {
            SCOPED_TRACE(testing::Message() << field << ',' << index);
            EXPECT_EQ(collected[index], index == field ? std::string{} : probes.identities[index]);
        }
        EXPECT_EQ(info.recommendation.preset, GraphicsPreset::Low);
        EXPECT_EQ(info.recommendation.reason.identityRule, GraphicsIdentityRule::MissingIdentity);
        EXPECT_EQ(info.inputs.dedicatedVideoMemoryBytes, 16 * kGiB);
        EXPECT_EQ(glGetError(), GL_NO_ERROR);
    }
}

TEST_F(HardwareInfoTest, NullIdentityUsesLowWithoutPreventingOtherCollection) {
    for (std::size_t field = 0; field < probes.identities.size(); ++field) {
        probes.nullIdentities = {};
        probes.nullIdentities[field] = true;
        const auto info = Detect();
        EXPECT_EQ(info.recommendation.preset, GraphicsPreset::Low);
        EXPECT_EQ(info.recommendation.reason.identityRule, GraphicsIdentityRule::MissingIdentity);
        EXPECT_EQ(info.inputs.dedicatedVideoMemoryBytes, 16 * kGiB);
        EXPECT_EQ(glGetError(), GL_NO_ERROR);
    }
}

TEST_F(HardwareInfoTest, IdentityCopyAcceptsBoundAndRejectsOverlongFields) {
    const auto originals = probes.identities;
    for (std::size_t field = 0; field < probes.identities.size(); ++field) {
        probes.identities = originals;
        probes.identities[field].resize(luminumbra::core::kMaxGraphicsIdentityBytes, ' ');
        const auto bounded = Detect();
        const std::array collected{
            bounded.inputs.vendor, bounded.inputs.renderer, bounded.inputs.version};
        EXPECT_EQ(collected[field], probes.identities[field]);
        EXPECT_EQ(bounded.recommendation.reason.identityRule,
                  GraphicsIdentityRule::AllowlistedModel);
        probes.identities[field] += 'X';
        const auto overlong = Detect();
        EXPECT_EQ(overlong.recommendation.preset, GraphicsPreset::Low);
        EXPECT_EQ(overlong.recommendation.reason.identityRule,
                  GraphicsIdentityRule::MissingIdentity);
    }
}

TEST_F(HardwareInfoTest, InvalidCapacityIsUnavailableAndLargeCapacityConvertsWithoutOverflow) {
    for (const GLint value : {0, -1}) {
        probes.memoryKB = value;
        EXPECT_FALSE(Detect().inputs.dedicatedVideoMemoryBytes.has_value());
    }
    probes.memoryKB = std::numeric_limits<GLint>::max();
    EXPECT_EQ(Detect().inputs.dedicatedVideoMemoryBytes,
              static_cast<std::uint64_t>(std::numeric_limits<GLint>::max()) * 1024);
}

TEST_F(HardwareInfoTest, RepeatedDetectionRecoversWithoutReusingFailedCapacity) {
    EXPECT_EQ(Detect().inputs.dedicatedVideoMemoryBytes, 16 * kGiB);
    probes.memoryErrors = {GL_INVALID_ENUM};
    EXPECT_FALSE(Detect().inputs.dedicatedVideoMemoryBytes.has_value());
    probes.memoryErrors.clear();
    probes.memoryKB = 8 * 1024 * 1024;
    EXPECT_EQ(Detect().inputs.dedicatedVideoMemoryBytes, 8 * kGiB);
    EXPECT_EQ(glGetError(), GL_NO_ERROR);
}

} // namespace

extern "C" GLenum glGetError() {
    if (probes.pendingErrors.empty()) {
        return GL_NO_ERROR;
    }
    const GLenum error = probes.pendingErrors.front();
    probes.pendingErrors.pop_front();
    return error;
}

extern "C" const GLubyte* glGetString(GLenum name) {
    EXPECT_TRUE(probes.pendingErrors.empty());
    ++probes.identityQueries;
    std::size_t field = 0;
    if (name == GL_RENDERER) {
        field = 1;
    } else if (name == GL_VERSION) {
        field = 2;
    } else {
        EXPECT_EQ(name, GL_VENDOR);
    }
    for (const GLenum error : probes.identityErrors[field]) {
        probes.pendingErrors.push_back(error);
    }
    return probes.nullIdentities[field]
               ? nullptr
               : reinterpret_cast<const GLubyte*>(probes.identities[field].c_str());
}

extern "C" void glGetIntegerv(GLenum name, GLint* value) {
    EXPECT_TRUE(probes.pendingErrors.empty());
    // No free-memory or unrelated GL query may be treated as dedicated capacity.
    EXPECT_EQ(name, 0x9047u);
    ++probes.memoryQueries;
    *value = probes.memoryKB;
    for (const GLenum error : probes.memoryErrors) {
        probes.pendingErrors.push_back(error);
    }
}

extern "C" int glfwExtensionSupported(const char* extension) {
    EXPECT_TRUE(probes.pendingErrors.empty());
    EXPECT_STREQ(extension, "GL_NVX_gpu_memory_info");
    ++probes.extensionQueries;
    for (const GLenum error : probes.extensionErrors) {
        probes.pendingErrors.push_back(error);
    }
    return probes.nvxAdvertised ? GLFW_TRUE : GLFW_FALSE;
}

extern "C" void glfwGetFramebufferSize(GLFWwindow* window, int* width, int* height) {
    EXPECT_NE(window, nullptr);
    *width = 2560;
    *height = 1440;
}
