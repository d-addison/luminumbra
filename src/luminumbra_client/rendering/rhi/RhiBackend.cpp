#include "rendering/rhi/RhiBackend.h"

#include <cstdlib>
#include <cstring>
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

}  // namespace

Backend ParseRhiBackend(const char* value) {
    const std::string v = ToLowerAscii(value);
    if (v == "vulkan" || v == "vk") {
        return Backend::Vulkan;
    }
    if (v == "dx12" || v == "d3d12") {
        return Backend::Dx12;
    }
    // "gl", "opengl", empty, and anything unrecognized fall through to the default.
    return Backend::Gl;
}

const char* BackendName(Backend backend) {
    switch (backend) {
        case Backend::Vulkan:
            return "vulkan";
        case Backend::Dx12:
            return "dx12";
        case Backend::Gl:
        default:
            return "gl";
    }
}

Backend SelectedBackendFromEnv() {
    return ParseRhiBackend(std::getenv("LUMIN_RHI"));
}

}  // namespace Luminumbra::Rendering::Rhi
