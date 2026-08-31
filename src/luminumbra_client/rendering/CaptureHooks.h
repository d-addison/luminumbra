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
//  (; contract ): real in-app capture-SDK trigger.
//
// The RenderDoc in-app API is loaded at runtime from the already-injected module
// (GetModuleHandle / dlopen RTLD_NOLOAD) -- there is no link-time dependency on
// RenderDoc, and nothing is loaded uninvited: if the process is not running under
// RenderDoc the module is absent and the calls fall back to the marker path. A
// real.rdc is only produced when the app runs under RenderDoc.
//
// adds the PIX and Nsight trigger legs with the same
// discipline. Neither SDK is vendored, so their entry-point surfaces are the
// minimal locally-declared structs below (not pix3.h / the NGFX SDK), and
// detection is GetModuleHandle on the ALREADY-injected module only -- nothing
// is ever LoadLibrary'd uninvited. IsCaptureSdkAvailable reports each backend
// honestly: true only when that backend's Begin/End bracket can actually run.
// -----------------------------------------------------------------------------

// An in-progress frame capture bracket. active == a real SDK capture was started;
// otherwise the caller should emit the marker (BuildCaptureReadyMarker) instead.
struct FrameCaptureSession {
    bool active = false;
    std::string backend; // the SDK name when an SDK started it, else the requested backend name
    std::string marker;  // the capture-ready marker (always populated)
    std::string diagnostic;
    // which SDK's End must close this bracket (MarkerOnly when !active).
    CaptureBackend sdk_backend = CaptureBackend::MarkerOnly;
    // the capture target handed to the SDK at Begin. PIX reports no path
    // back at End (unlike RenderDoc's GetCapture), so this is its only record.
    std::string requested_capture_file;
};

struct FrameCaptureResult {
    bool capture_started = false; // true iff a real SDK capture ran end to end
    std::string backend;
    std::string marker;
    std::string capture_file; // absolute path to the produced.rdc, else empty
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
// the.rdc path via the SDK). Safe to call when !session.active (returns a
// marker-only result with capture_started == false).
FrameCaptureResult EndFrameCapture(FrameCaptureSession& session);

// -----------------------------------------------------------------------------
// minimal locally-declared PIX / Nsight trigger surfaces. These are OUR
// declarations (the SDK headers are not vendored), shared with the tests so the
// injected doubles match the exact ABI the production code calls through.
// -----------------------------------------------------------------------------

// Minimal local mirror of pix3.h's PIXCaptureParameters GPU-capture member.
// Only GpuCaptureParameters.FileName is read under PIX_CAPTURE_GPU; the pad
// keeps this at least as large as the SDK union so the callee never reads past
// our storage.
struct PixCaptureParameters {
    const wchar_t* gpu_capture_file_name = nullptr;
    unsigned long long reserved_pad[8] = {};
};

// WinPixGpuCapturer.dll programmatic-capture entry points (PIXBeginCapture2 /
// PIXEndCapture), spelled with plain types so this header stays windows.h-free:
// HRESULT -> long, DWORD -> unsigned long, BOOL -> int. The only Windows target
// is x64, which has a single calling convention (WINAPI is a no-op there).
// PIX GPU capture targets D3D: on today's GL client the trigger seam + module
// detection handshake is what is wired and tested; a real.wpix capture needs
// the DX12 backend.
struct PixCaptureApi {
    long (*BeginCapture)(unsigned long flags, const PixCaptureParameters* params) = nullptr;
    long (*EndCapture)(int discard) = nullptr;
};

// The Nsight trigger bracket shape (there is no public in-process ABI to
// mirror: the injected interception layer exports no documented programmatic
// trigger -- that needs the NGFX Injection SDK, not vendored). The real leg is
// module detection only; this struct is what the test seam injects and what a
// future NGFX wire-up fills in. Returns 1 on success (RenderDoc's convention).
struct NsightCaptureApi {
    unsigned int (*BeginCapture)() = nullptr;
    unsigned int (*EndCapture)() = nullptr;
};

namespace detail {
// Test seam: inject a fake RENDERDOC_API_1_x_x* (passed as void* so the
// public header stays free of the RenderDoc header) so the capture integration
// logic runs against a double, exercising the exact production Begin/End code
// path without a real RenderDoc DLL. Pass nullptr to reset to real loading.
void SetRenderDocApiForTesting(void* renderdoc_api);
// Test seams, cloning the RenderDoc one: inject a fake PixCaptureApi*
// / NsightCaptureApi* (as void*, matching the seam shape above) so the full
// Begin/End bracket runs against a double without either SDK installed. Pass
// nullptr to reset to real module detection.
void SetPixApiForTesting(void* pix_api);
void SetNsightApiForTesting(void* nsight_api);
} // namespace detail

} // namespace Luminumbra::Rendering
