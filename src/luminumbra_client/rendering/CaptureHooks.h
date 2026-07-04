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

// The marker-only fallback: emits the "luminumbra.capture.ready:<scenario>:<backend>"
// handshake string without triggering an SDK. Always available, headless-safe.
CaptureResult BuildCaptureReadyMarker(const CaptureRequest& request);

// -----------------------------------------------------------------------------
// GPU-06 (spec 021; charter FR-E-003): real in-app capture-SDK trigger.
//
// The RenderDoc in-app API is loaded at runtime from the already-injected module
// (GetModuleHandle / dlopen RTLD_NOLOAD) -- there is no link-time dependency on
// RenderDoc, and nothing is loaded uninvited: if the process is not running under
// RenderDoc the module is absent and the calls fall back to the marker path. A
// real .rdc is only produced when the app runs under RenderDoc.
//
// PIX and Nsight are NOT linked (their programmatic in-app triggers need the
// heavyweight PIX runtime / NGFX injection SDK) -- IsCaptureSdkAvailable reports
// false for them honestly rather than silently claiming support. Tracked as a
// follow-up (see the backlog Nsight/PIX SDK item).
// -----------------------------------------------------------------------------

// An in-progress frame capture bracket. active == a real SDK capture was started;
// otherwise the caller should emit the marker (BuildCaptureReadyMarker) instead.
struct FrameCaptureSession {
    bool active = false;
    std::string backend;      // "RenderDoc" when an SDK started it, else the requested backend name
    std::string marker;       // the capture-ready marker (always populated)
    std::string diagnostic;
};

struct FrameCaptureResult {
    bool capture_started = false;  // true iff a real SDK capture ran end to end
    std::string backend;
    std::string marker;
    std::string capture_file;      // absolute path to the produced .rdc, else empty
    std::string diagnostic;
};

// Whether a real in-app capture SDK for `backend` is present in THIS process.
bool IsCaptureSdkAvailable(CaptureBackend backend);

// Begin a real frame capture if the requested SDK is available (setting the
// capture-file path template to `capture_dir`/<scenario>). If none is available,
// the returned session has active == false and carries the marker to emit. Draw
// the frame to capture, then call EndFrameCapture.
FrameCaptureSession BeginFrameCapture(const CaptureRequest& request,
                                      const std::string& capture_dir);

// End the capture started by BeginFrameCapture and report the result (including
// the .rdc path via the SDK). Safe to call when !session.active (returns a
// marker-only result with capture_started == false).
FrameCaptureResult EndFrameCapture(FrameCaptureSession& session);

namespace detail {
// Test seam (GPU-06): inject a fake RENDERDOC_API_1_x_x* (passed as void* so the
// public header stays free of the RenderDoc header) so the capture integration
// logic runs against a double, exercising the exact production Begin/End code
// path without a real RenderDoc DLL. Pass nullptr to reset to real loading.
void SetRenderDocApiForTesting(void* renderdoc_api);
} // namespace detail

} // namespace Luminumbra::Rendering
