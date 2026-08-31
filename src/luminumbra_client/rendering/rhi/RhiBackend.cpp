#include "rendering/rhi/RhiBackend.h"
#include "luminumbra_common/core/Environment.h"

#include <string>

namespace Luminumbra::Rendering::Rhi {

namespace {

std::string ToLowerAscii(const char* value) {
    std::string out;
    if (value == nullptr) {
        return out;
    }
    for (const char* p = value; *p != '\0'; ++p) {
        char c = *p;
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c - 'A' + 'a');
        }
        out.push_back(c);
    }
    return out;
}

} // namespace

Backend ParseRhiBackend(const char* value) {
    const std::string v = ToLowerAscii(value);
    if (v == "vulkan" || v == "vk") {
        return Backend::Vulkan;
    }
    // "gl", "opengl", empty, and anything unrecognized fall through to the default.
    return Backend::Gl;
}

const char* BackendName(Backend backend) {
    switch (backend) {
        case Backend::Vulkan:
            return "vulkan";
        case Backend::Gl:
        default:
            return "gl";
    }
}

Backend SelectedBackendFromEnv() {
    const auto value = Core::ReadEnvironment("LUMIN_RHI");
    return ParseRhiBackend(value ? value->c_str() : nullptr);
}

} // namespace Luminumbra::Rendering::Rhi
