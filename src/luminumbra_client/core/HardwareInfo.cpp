#include "HardwareInfo.h"

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>
#include <glad/glad.h>

#include <locale>
#include <sstream>
#include <string>
#include <thread>

namespace Luminumbra::Client {
namespace {

bool ConsumeGlErrors() {
    bool succeeded = true;
    while (glGetError() != GL_NO_ERROR) {
        succeeded = false;
    }
    return succeeded;
}

std::string ReadIdentity(GLenum name) {
    const auto* text = reinterpret_cast<const char*>(glGetString(name));
    if (!ConsumeGlErrors() || text == nullptr) {
        return {};
    }
    // Scan at most 1024 bytes plus a terminator; copy at most 1024 bytes per field.
    for (std::size_t length = 0; length <= luminumbra::core::kMaxGraphicsIdentityBytes; ++length) {
        if (text[length] == '\0') {
            return std::string(text, length);
        }
    }
    return {};
}

} // namespace

HardwareInfo DetectHardwareInfo(GLFWwindow* window) {
    HardwareInfo info;
    // Isolate each startup probe from stale errors and from the following renderer work.
    (void)ConsumeGlErrors();
    info.inputs.vendor = ReadIdentity(GL_VENDOR);
    info.inputs.renderer = ReadIdentity(GL_RENDERER);
    info.inputs.version = ReadIdentity(GL_VERSION);
    info.inputs.logicalProcessorCount = std::thread::hardware_concurrency();
    glfwGetFramebufferSize(window, &info.inputs.framebufferWidth, &info.inputs.framebufferHeight);
    const bool nvxAdvertised = glfwExtensionSupported("GL_NVX_gpu_memory_info") == GLFW_TRUE;
    const bool extensionQuerySucceeded = ConsumeGlErrors();
    info.nvxMemoryInfoAdvertised = nvxAdvertised && extensionQuerySucceeded;
    if (info.nvxMemoryInfoAdvertised) {
        // Dedicated capacity in KB. ATI_meminfo exposes free pools, never capacity.
        // Keep the enumerant local: the loader need not generate this optional extension.
        constexpr GLenum dedicatedVideoMemoryNvx = 0x9047;
        GLint dedicatedMemoryKB = 0;
        glGetIntegerv(dedicatedVideoMemoryNvx, &dedicatedMemoryKB);
        if (ConsumeGlErrors()) {
            info.inputs.dedicatedVideoMemoryBytes =
                luminumbra::core::DedicatedVideoMemoryBytesFromNvx(true, dedicatedMemoryKB);
        }
    }
    info.recommendation = luminumbra::core::ClassifyGraphicsPreset(info.inputs);
    return info;
}

std::string FormatHardwareDiagnostics(const HardwareInfo& info, bool enabled) {
    if (!enabled) {
        return {};
    }
    using luminumbra::core::GraphicsIdentityRuleName;
    using luminumbra::core::GraphicsPresetDisplayName;
    const auto& inputs = info.inputs;
    const auto& recommendation = info.recommendation;
    const auto& reason = recommendation.reason;
    std::ostringstream output;
    output.imbue(std::locale::classic());
    output << std::boolalpha << "Hardware: vendor='" << recommendation.normalizedVendor
           << "' renderer='" << recommendation.normalizedRenderer << "' version='"
           << recommendation.normalizedVersion
           << "' logical_processors=" << inputs.logicalProcessorCount
           << " nvx_memory_info=" << info.nvxMemoryInfoAdvertised << " dedicated_vram_bytes=";
    if (inputs.dedicatedVideoMemoryBytes) {
        output << *inputs.dedicatedVideoMemoryBytes;
    } else {
        output << "unknown";
    }
    output << " framebuffer=" << inputs.framebufferWidth << 'x' << inputs.framebufferHeight
           << " recommended_preset=" << GraphicsPresetDisplayName(recommendation.preset)
           << " rules_revision=" << recommendation.rulesRevision
           << " identity_rule=" << GraphicsIdentityRuleName(reason.identityRule) << " model='"
           << recommendation.matchedModel
           << "' ceilings(gpu=" << GraphicsPresetDisplayName(reason.gpuCeiling)
           << ",cpu=" << GraphicsPresetDisplayName(reason.cpuCeiling) << ",vram="
           << (reason.vramCeiling ? GraphicsPresetDisplayName(*reason.vramCeiling) : "unknown")
           << ") binding(gpu=" << reason.gpuBinding << ",cpu=" << reason.cpuBinding
           << ",vram=" << reason.vramBinding << ')';
    return output.str();
}

} // namespace Luminumbra::Client
