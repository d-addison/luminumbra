#include "rendering/CaptureHooks.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <string>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <dlfcn.h>
#endif

#include "renderdoc/renderdoc_app.h"

namespace Luminumbra::Rendering {

namespace {

std::string sanitize_scenario_name(std::string name) {
    if (name.empty()) {
        return "unnamed";
    }
    std::replace_if(
        name.begin(),
        name.end(),
        [](char value) {
            return !(value >= 'a' && value <= 'z') && !(value >= 'A' && value <= 'Z') &&
                   !(value >= '0' && value <= '9') && value != '_' && value != '-';
        },
        '_');
    return name;
}

//  test seam (see detail::SetRenderDocApiForTesting): when set, this fake
// RENDERDOC_API_1_x_x* is used instead of the real load, so the exact production
// Begin/End path runs against a double without a real RenderDoc DLL.
void* g_injected_renderdoc_api = nullptr;

// Load the RenderDoc in-app API from the ALREADY-injected module only. We never
// LoadLibrary/dlopen-load renderdoc uninvited (RTLD_NOLOAD requires it to be
// present); if the process is not running under RenderDoc this returns nullptr
// and callers fall back to the marker path.
RENDERDOC_API_1_6_0* load_real_renderdoc_api() {
#if defined(_WIN32)
    HMODULE module = GetModuleHandleA("renderdoc.dll");
    if (module == nullptr) {
        return nullptr;
    }
    FARPROC raw = GetProcAddress(module, "RENDERDOC_GetAPI");
#elif defined(__APPLE__)
    void* module = dlopen("librenderdoc.dylib", RTLD_NOW | RTLD_NOLOAD);
    if (module == nullptr) {
        return nullptr;
    }
    void* raw = dlsym(module, "RENDERDOC_GetAPI");
#else
    void* module = dlopen("librenderdoc.so", RTLD_NOW | RTLD_NOLOAD);
    if (module == nullptr) {
        return nullptr;
    }
    void* raw = dlsym(module, "RENDERDOC_GetAPI");
#endif
    if (raw == nullptr) {
        return nullptr;
    }
    // memcpy the raw address into the typed pointer to avoid a function/object
    // pointer cast diagnostic (-Wcast-function-type / -Wpedantic) under -Werror.
    pRENDERDOC_GetAPI get_api = nullptr;
    std::memcpy(&get_api, &raw, sizeof(get_api));

    RENDERDOC_API_1_6_0* api = nullptr;
    if (get_api(eRENDERDOC_API_Version_1_6_0, reinterpret_cast<void**>(&api)) != 1) {
        return nullptr;
    }
    return api;
}

RENDERDOC_API_1_6_0* get_renderdoc_api() {
    if (g_injected_renderdoc_api != nullptr) {
        return static_cast<RENDERDOC_API_1_6_0*>(g_injected_renderdoc_api);
    }
    // Load the real module once; a null result (no RenderDoc) is cached too.
    static RENDERDOC_API_1_6_0* real_api = load_real_renderdoc_api();
    return real_api;
}

//  test seams (see detail::SetPixApiForTesting / SetNsightApiForTesting):
// same shape as the RenderDoc seam above.
void* g_injected_pix_api = nullptr;
void* g_injected_nsight_api = nullptr;

// Resolve the PIX programmatic-capture entry points from the ALREADY-injected
// WinPixGpuCapturer.dll only (the PIX launcher injects it; we never LoadLibrary
// it uninvited -- same discipline as the RenderDoc leg). PIXBeginCapture2 is
// the DLL export behind pix3.h's PIXBeginCapture inline.
PixCaptureApi* load_real_pix_api() {
#if defined(_WIN32)
    HMODULE module = GetModuleHandleA("WinPixGpuCapturer.dll");
    if (module == nullptr) {
        return nullptr;
    }
    FARPROC begin_raw = GetProcAddress(module, "PIXBeginCapture2");
    if (begin_raw == nullptr) {
        begin_raw = GetProcAddress(module, "PIXBeginCapture");
    }
    FARPROC end_raw = GetProcAddress(module, "PIXEndCapture");
    if (begin_raw == nullptr || end_raw == nullptr) {
        return nullptr;
    }
    // memcpy for the same -Wcast-function-type reason as the RenderDoc leg.
    static PixCaptureApi api;
    std::memcpy(&api.BeginCapture, &begin_raw, sizeof(api.BeginCapture));
    std::memcpy(&api.EndCapture, &end_raw, sizeof(api.EndCapture));
    return &api;
#else
    return nullptr; // PIX is Windows-only
#endif
}

PixCaptureApi* get_pix_api() {
    if (g_injected_pix_api != nullptr) {
        return static_cast<PixCaptureApi*>(g_injected_pix_api);
    }
    static PixCaptureApi* real_api = load_real_pix_api();
    return real_api;
}

// Nsight Graphics injection detection (detection ONLY -- never LoadLibrary'd):
// the frame-debugger interception layer and the NGFX Injection SDK loader.
bool nsight_injection_detected() {
#if defined(_WIN32)
    constexpr const char* kNsightModules[] = {
        "Nvda.Graphics.Interception.dll",
        "NGFX_Injection.dll",
    };
    for (const char* name : kNsightModules) {
        if (GetModuleHandleA(name) != nullptr) {
            return true;
        }
    }
#endif
    return false;
}

NsightCaptureApi* get_nsight_api() {
    if (g_injected_nsight_api != nullptr) {
        return static_cast<NsightCaptureApi*>(g_injected_nsight_api);
    }
    // The injected interception layer exports no documented begin/end trigger
    // (programmatic capture needs the NGFX Injection SDK, not vendored), so no
    // real api is ever materialized -- the real leg is detection-only, surfaced
    // via nsight_injection_detected in the Begin diagnostic.
    return nullptr;
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

bool IsCaptureSdkAvailable(CaptureBackend backend) {
    // Honest per-backend report: true only when that backend's Begin/End bracket
    // can actually run (an Nsight injection WITHOUT a trigger api stays false).
    switch (backend) {
        case CaptureBackend::RenderDoc:
            return get_renderdoc_api() != nullptr;
        case CaptureBackend::PIX:
            return get_pix_api() != nullptr;
        case CaptureBackend::Nsight:
            return get_nsight_api() != nullptr;
        case CaptureBackend::MarkerOnly:
        default:
            return false;
    }
}

FrameCaptureSession BeginFrameCapture(const CaptureRequest& request,
                                      const std::string& capture_dir) {
    const std::string scenario = sanitize_scenario_name(request.scenario);
    FrameCaptureSession session;
    session.backend = CaptureBackendName(request.preferred_backend);
    session.marker = "luminumbra.capture.ready:" + scenario + ":" + session.backend;

    switch (request.preferred_backend) {
        case CaptureBackend::RenderDoc: {
            RENDERDOC_API_1_6_0* api = get_renderdoc_api();
            if (api == nullptr) {
                break;
            }
            if (api->SetCaptureFilePathTemplate != nullptr) {
                const std::string path_template =
                    capture_dir.empty() ? scenario : (capture_dir + "/" + scenario);
                api->SetCaptureFilePathTemplate(path_template.c_str());
            }
            api->StartFrameCapture(nullptr, nullptr);
            session.active = true;
            session.sdk_backend = CaptureBackend::RenderDoc;
            session.backend = "RenderDoc";
            session.diagnostic = "RenderDoc in-app API frame capture started";
            return session;
        }
        case CaptureBackend::PIX: {
            PixCaptureApi* api = get_pix_api();
            if (api == nullptr || api->BeginCapture == nullptr || api->EndCapture == nullptr) {
                break;
            }
            // PIX reports no capture path back at End (unlike RenderDoc GetCapture):
            // the target handed to Begin is the only record of it.
            const std::string capture_file =
                (capture_dir.empty() ? scenario : (capture_dir + "/" + scenario)) + ".wpix";
            // ASCII-by-construction widening (the scenario is sanitized; capture
            // dirs are repo/artifact paths). PIX copies the params at the call.
            const std::wstring wide_path(capture_file.begin(), capture_file.end());
            PixCaptureParameters params;
            params.gpu_capture_file_name = wide_path.c_str();
            // 1ul == PIX_CAPTURE_GPU (pix3.h). PIX GPU capture targets D3D -- on the
            // GL client a real WinPixGpuCapturer fails this HRESULT, reported
            // honestly below (a real capture needs the DX12 backend,  ).
            const long begin_hr = api->BeginCapture(1ul, &params);
            if (begin_hr != 0) {
                session.diagnostic = "PIXBeginCapture failed (hr=" + std::to_string(begin_hr) +
                                     "); emitted capture-ready marker";
                return session;
            }
            session.active = true;
            session.sdk_backend = CaptureBackend::PIX;
            session.backend = "PIX";
            session.requested_capture_file = capture_file;
            session.diagnostic = "PIX programmatic GPU capture started";
            return session;
        }
        case CaptureBackend::Nsight: {
            NsightCaptureApi* api = get_nsight_api();
            if (api == nullptr || api->BeginCapture == nullptr || api->EndCapture == nullptr) {
                if (nsight_injection_detected()) {
                    session.diagnostic =
                        "Nsight injection detected but exports no programmatic trigger "
                        "(needs the NGFX Injection SDK); emitted capture-ready marker";
                    return session;
                }
                break;
            }
            if (api->BeginCapture() != 1u) {
                session.diagnostic =
                    "Nsight BeginCapture reported failure; emitted capture-ready marker";
                return session;
            }
            session.active = true;
            session.sdk_backend = CaptureBackend::Nsight;
            session.backend = "Nsight";
            session.diagnostic = "Nsight trigger frame capture started";
            return session;
        }
        case CaptureBackend::MarkerOnly:
        default:
            break;
    }

    session.active = false;
    session.diagnostic =
        session.backend + " SDK trigger is not linked; emitted capture-ready marker";
    return session;
}

FrameCaptureResult EndFrameCapture(FrameCaptureSession& session) {
    FrameCaptureResult result;
    result.backend = session.backend;
    result.marker = session.marker;

    if (!session.active) {
        result.capture_started = false;
        result.diagnostic = session.diagnostic;
        return result;
    }
    session.active = false;

    switch (session.sdk_backend) {
        case CaptureBackend::RenderDoc: {
            RENDERDOC_API_1_6_0* api = get_renderdoc_api();
            if (api == nullptr) {
                break;
            }
            const uint32_t ended_ok = api->EndFrameCapture(nullptr, nullptr);
            result.capture_started = (ended_ok == 1u);
            if (result.capture_started && api->GetNumCaptures != nullptr &&
                api->GetCapture != nullptr) {
                const uint32_t count = api->GetNumCaptures();
                if (count > 0u) {
                    uint32_t path_length = 0u;
                    if (api->GetCapture(count - 1u, nullptr, &path_length, nullptr) == 1u &&
                        path_length > 0u) {
                        std::string path(path_length, '\0');
                        if (api->GetCapture(count - 1u, path.data(), &path_length, nullptr) == 1u) {
                            // RenderDoc reports the length including the trailing null.
                            if (!path.empty() && path.back() == '\0') {
                                path.pop_back();
                            }
                            result.capture_file = path;
                        }
                    }
                }
            }
            result.diagnostic = result.capture_started
                                    ? "RenderDoc in-app API capture completed"
                                    : "RenderDoc EndFrameCapture reported failure";
            return result;
        }
        case CaptureBackend::PIX: {
            PixCaptureApi* api = get_pix_api();
            if (api == nullptr || api->EndCapture == nullptr) {
                break;
            }
            const long end_hr = api->EndCapture(0 /* discard == FALSE: keep the capture */);
            result.capture_started = (end_hr == 0);
            if (result.capture_started) {
                // PIX reports no path back; this is the target handed to Begin.
                result.capture_file = session.requested_capture_file;
            }
            result.diagnostic = result.capture_started
                                    ? "PIX programmatic GPU capture completed"
                                    : "PIXEndCapture failed (hr=" + std::to_string(end_hr) + ")";
            return result;
        }
        case CaptureBackend::Nsight: {
            NsightCaptureApi* api = get_nsight_api();
            if (api == nullptr || api->EndCapture == nullptr) {
                break;
            }
            // Nsight owns its capture output location; no path is reported back.
            result.capture_started = (api->EndCapture() == 1u);
            result.diagnostic = result.capture_started ? "Nsight trigger capture completed"
                                                       : "Nsight EndCapture reported failure";
            return result;
        }
        case CaptureBackend::MarkerOnly:
        default:
            break;
    }

    result.capture_started = false;
    result.diagnostic = "capture SDK became unavailable mid-capture";
    return result;
}

namespace detail {
void SetRenderDocApiForTesting(void* renderdoc_api) {
    g_injected_renderdoc_api = renderdoc_api;
}
//  seams, cloning the RenderDoc one: pass a PixCaptureApi* /
// NsightCaptureApi* as void*; nullptr resets to real module detection.
void SetPixApiForTesting(void* pix_api) {
    g_injected_pix_api = pix_api;
}
void SetNsightApiForTesting(void* nsight_api) {
    g_injected_nsight_api = nsight_api;
}
} // namespace detail

} // namespace Luminumbra::Rendering
