#include "rendering/CaptureHooks.h"

#include <algorithm>

namespace Luminumbra::Rendering {

namespace {

std::string sanitize_scenario_name(std::string name) {
    if (name.empty()) {
        return "unnamed";
    }
    std::replace_if(name.begin(), name.end(), [](char value) {
        return !(value >= 'a' && value <= 'z') &&
               !(value >= 'A' && value <= 'Z') &&
               !(value >= '0' && value <= '9') &&
               value != '_' &&
               value != '-';
    }, '_');
    return name;
}

} // namespace

std::string CaptureBackendName(CaptureBackend backend) {
    switch (backend) {
    case CaptureBackend::RenderDoc:
        return "RenderDoc";
    case CaptureBackend::PIX:
        return "PIX";
    case CaptureBackend::Nsight:
        return "Nsight";
    case CaptureBackend::MarkerOnly:
    default:
        return "MarkerOnly";
    }
}

CaptureResult BuildCaptureReadyMarker(const CaptureRequest& request) {
    const std::string scenario = sanitize_scenario_name(request.scenario);
    CaptureResult result;
    result.backend = CaptureBackendName(request.preferred_backend);
    result.marker = "luminumbra.capture.ready:" + scenario + ":" + result.backend;
    result.capture_started = false;
    result.diagnostic = result.backend + " SDK trigger is not linked; emitted capture-ready marker";
    return result;
}

} // namespace Luminumbra::Rendering
