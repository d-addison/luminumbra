#include "rendering/CaptureHooks.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <string>

#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#else
#  include <dlfcn.h>
#endif

#include "renderdoc/renderdoc_app.h"

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

// GPU-06 test seam (see detail::SetRenderDocApiForTesting): when set, this fake
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
    if (backend == CaptureBackend::RenderDoc) {
        return get_renderdoc_api() != nullptr;
    }
    // PIX / Nsight / MarkerOnly have no in-app SDK trigger linked (see header).
    return false;
}

FrameCaptureSession BeginFrameCapture(const CaptureRequest& request,
                                      const std::string& capture_dir) {
    const std::string scenario = sanitize_scenario_name(request.scenario);
    FrameCaptureSession session;
    session.backend = CaptureBackendName(request.preferred_backend);
    session.marker = "luminumbra.capture.ready:" + scenario + ":" + session.backend;

    RENDERDOC_API_1_6_0* api =
        (request.preferred_backend == CaptureBackend::RenderDoc) ? get_renderdoc_api() : nullptr;
    if (api == nullptr) {
        session.active = false;
        session.diagnostic =
            session.backend + " SDK trigger is not linked; emitted capture-ready marker";
        return session;
    }

    if (api->SetCaptureFilePathTemplate != nullptr) {
        const std::string path_template =
            capture_dir.empty() ? scenario : (capture_dir + "/" + scenario);
        api->SetCaptureFilePathTemplate(path_template.c_str());
    }
    api->StartFrameCapture(nullptr, nullptr);
    session.active = true;
    session.backend = "RenderDoc";
    session.diagnostic = "RenderDoc in-app API frame capture started";
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

    RENDERDOC_API_1_6_0* api = get_renderdoc_api();
    session.active = false;
    if (api == nullptr) {
        result.capture_started = false;
        result.diagnostic = "capture SDK became unavailable mid-capture";
        return result;
    }

    const uint32_t ended_ok = api->EndFrameCapture(nullptr, nullptr);
    result.capture_started = (ended_ok == 1u);
    if (result.capture_started && api->GetNumCaptures != nullptr && api->GetCapture != nullptr) {
        const uint32_t count = api->GetNumCaptures();
        if (count > 0u) {
            uint32_t path_length = 0u;
            if (api->GetCapture(count - 1u, nullptr, &path_length, nullptr) == 1u && path_length > 0u) {
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

namespace detail {
void SetRenderDocApiForTesting(void* renderdoc_api) {
    g_injected_renderdoc_api = renderdoc_api;
}
} // namespace detail

} // namespace Luminumbra::Rendering
