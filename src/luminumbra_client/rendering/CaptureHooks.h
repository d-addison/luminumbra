#pragma once

#include <string>

namespace Luminumbra::Rendering {

enum class CaptureBackend {
    MarkerOnly,
    RenderDoc,
    PIX,
    Nsight
};

struct CaptureRequest {
    std::string scenario;
    CaptureBackend preferred_backend = CaptureBackend::RenderDoc;
};

struct CaptureResult {
    std::string backend;
    std::string marker;
    bool capture_started = false;
    std::string diagnostic;
};

std::string CaptureBackendName(CaptureBackend backend);
CaptureResult BuildCaptureReadyMarker(const CaptureRequest& request);

} // namespace Luminumbra::Rendering
