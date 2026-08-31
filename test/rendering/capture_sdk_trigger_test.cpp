#include "gtest/gtest.h"

#include <filesystem>
#include <string>

// the PIX + Nsight trigger legs under test. Neither
// SDK is installed on the gate box, so (exactly like 's RenderDoc double
// in render_capture_test.cpp) these tests inject fake api structs through the
// production seams and run the EXACT production Begin/End code path against
// them -- zero skips. The untested remainder is only the real module-detection
// success branch (needs a live PIX / Nsight injection), documented, not skipped.
#include "rendering/CaptureHooks.h"

namespace fs = std::filesystem;

namespace {

#ifndef LUMINUMBRA_TEST_ARTIFACT_DIR
#define LUMINUMBRA_TEST_ARTIFACT_DIR "."
#endif

std::string ArtifactDir() {
    return (fs::path(LUMINUMBRA_TEST_ARTIFACT_DIR) / "capture_sdk_trigger").string();
}

// --- PIX double: implements the exact PixCaptureApi ABI the production leg
// calls through (the header's minimal local declaration IS the shared ABI). ---

struct FakePixState {
    int begin_calls = 0;
    int end_calls = 0;
    unsigned long last_flags = 0;
    int last_discard = -1;
    std::wstring last_file;
    long begin_result = 0; // S_OK
    long end_result = 0;   // S_OK
};

FakePixState g_fake_pix;

long FakePixBeginCapture(unsigned long flags,
                         const Luminumbra::Rendering::PixCaptureParameters* params) {
    ++g_fake_pix.begin_calls;
    g_fake_pix.last_flags = flags;
    if (params != nullptr && params->gpu_capture_file_name != nullptr) {
        g_fake_pix.last_file = params->gpu_capture_file_name;
    }
    return g_fake_pix.begin_result;
}

long FakePixEndCapture(int discard) {
    ++g_fake_pix.end_calls;
    g_fake_pix.last_discard = discard;
    return g_fake_pix.end_result;
}

// --- Nsight double: the header's NsightCaptureApi bracket shape (1 == ok). ---

struct FakeNsightState {
    int begin_calls = 0;
    int end_calls = 0;
    unsigned int begin_result = 1u;
    unsigned int end_result = 1u;
};

FakeNsightState g_fake_nsight;

unsigned int FakeNsightBeginCapture() {
    ++g_fake_nsight.begin_calls;
    return g_fake_nsight.begin_result;
}

unsigned int FakeNsightEndCapture() {
    ++g_fake_nsight.end_calls;
    return g_fake_nsight.end_result;
}

} // namespace

TEST(RenderCaptureSdkTrigger, PixTriggerLive) {
    namespace R = Luminumbra::Rendering;
    g_fake_pix = FakePixState{};
    R::PixCaptureApi fake;
    fake.BeginCapture = &FakePixBeginCapture;
    fake.EndCapture = &FakePixEndCapture;
    R::detail::SetPixApiForTesting(&fake);

    const bool available = R::IsCaptureSdkAvailable(R::CaptureBackend::PIX);

    R::CaptureRequest request;
    request.scenario = "pix trigger live";
    request.preferred_backend = R::CaptureBackend::PIX;
    R::FrameCaptureSession session = R::BeginFrameCapture(request, ArtifactDir());
    const bool began_active = session.active;
    const std::string began_backend = session.backend;
    R::FrameCaptureResult result = R::EndFrameCapture(session);

    // Reset the injection while `fake` is still alive (a stack local); every
    // assertion below runs against captured values, so no early return can
    // leave a dangling injected pointer.
    R::detail::SetPixApiForTesting(nullptr);

    EXPECT_TRUE(available) << "injected API should report the SDK available";
    EXPECT_TRUE(began_active);
    EXPECT_EQ(began_backend, "PIX");
    EXPECT_TRUE(result.capture_started);
    EXPECT_EQ(result.backend, "PIX");
    EXPECT_EQ(g_fake_pix.begin_calls, 1);
    EXPECT_EQ(g_fake_pix.end_calls, 1);
    EXPECT_EQ(g_fake_pix.last_flags, 1ul); // PIX_CAPTURE_GPU
    EXPECT_EQ(g_fake_pix.last_discard, 0); // keep the capture, don't discard
    // The reported capture file is the sanitized.wpix target handed to Begin
    // (PIX reports no path back at End), and the widened filename PIX saw
    // matches it 1:1.
    EXPECT_NE(result.capture_file.find("pix_trigger_live"), std::string::npos)
        << result.capture_file;
    EXPECT_NE(result.capture_file.find(".wpix"), std::string::npos) << result.capture_file;
    EXPECT_EQ(g_fake_pix.last_file,
              std::wstring(result.capture_file.begin(), result.capture_file.end()));
}

TEST(RenderCaptureSdkTrigger, NsightTriggerLive) {
    namespace R = Luminumbra::Rendering;
    g_fake_nsight = FakeNsightState{};
    R::NsightCaptureApi fake;
    fake.BeginCapture = &FakeNsightBeginCapture;
    fake.EndCapture = &FakeNsightEndCapture;
    R::detail::SetNsightApiForTesting(&fake);

    const bool available = R::IsCaptureSdkAvailable(R::CaptureBackend::Nsight);

    R::CaptureRequest request;
    request.scenario = "nsight trigger live";
    request.preferred_backend = R::CaptureBackend::Nsight;
    R::FrameCaptureSession session = R::BeginFrameCapture(request, ArtifactDir());
    const bool began_active = session.active;
    const std::string began_backend = session.backend;
    R::FrameCaptureResult result = R::EndFrameCapture(session);

    R::detail::SetNsightApiForTesting(nullptr);

    EXPECT_TRUE(available) << "injected API should report the SDK available";
    EXPECT_TRUE(began_active);
    EXPECT_EQ(began_backend, "Nsight");
    EXPECT_TRUE(result.capture_started);
    EXPECT_EQ(result.backend, "Nsight");
    EXPECT_EQ(g_fake_nsight.begin_calls, 1);
    EXPECT_EQ(g_fake_nsight.end_calls, 1);
    // Nsight owns its capture output location; no path is reported back.
    EXPECT_TRUE(result.capture_file.empty()) << result.capture_file;
}

TEST(RenderCaptureSdkTrigger, FallbackWhenAbsent) {
    namespace R = Luminumbra::Rendering;
    R::detail::SetPixApiForTesting(nullptr);
    R::detail::SetNsightApiForTesting(nullptr);
    // With no injection this exercises the REAL detection path: neither
    // WinPixGpuCapturer.dll nor an Nsight interception module is loaded into
    // this gtest process -- an honest false, not a skip.
    EXPECT_FALSE(R::IsCaptureSdkAvailable(R::CaptureBackend::PIX));
    EXPECT_FALSE(R::IsCaptureSdkAvailable(R::CaptureBackend::Nsight));

    for (R::CaptureBackend backend : {R::CaptureBackend::PIX, R::CaptureBackend::Nsight}) {
        const std::string name = R::CaptureBackendName(backend);
        R::CaptureRequest request;
        request.scenario = "headless fallback";
        request.preferred_backend = backend;
        R::FrameCaptureSession session = R::BeginFrameCapture(request, ArtifactDir());
        EXPECT_FALSE(session.active) << name;
        EXPECT_NE(session.marker.find("luminumbra.capture.ready:headless_fallback:" + name),
                  std::string::npos)
            << session.marker;

        R::FrameCaptureResult result = R::EndFrameCapture(session);
        EXPECT_FALSE(result.capture_started) << name;
        EXPECT_TRUE(result.capture_file.empty()) << name;
        EXPECT_NE(result.diagnostic.find("not linked"), std::string::npos) << result.diagnostic;
    }
}
