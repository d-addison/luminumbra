#pragma once

#include "luminumbra_common/core/GraphicsPresetClassifier.h"

#include <string>

struct GLFWwindow;

namespace Luminumbra::Client {

struct HardwareInfo {
    luminumbra::core::GraphicsHardwareInputs inputs;
    luminumbra::core::GraphicsPresetRecommendation recommendation;
    bool nvxMemoryInfoAdvertised = false;
};

// Call once on the context thread after GL loading, before allocating render targets.
// Reads diagnostics and recommends a preset; it never changes or persists settings.
// Drains stale and probe GL errors; failed probes leave the affected inputs unavailable.
[[nodiscard]] HardwareInfo DetectHardwareInfo(GLFWwindow* window);

// Empty by default; only an explicit opt-in formats diagnostics for logging.
// Collection and the stored recommendation are independent of this output policy.
[[nodiscard]] std::string FormatHardwareDiagnostics(const HardwareInfo& info, bool enabled = false);

} // namespace Luminumbra::Client
