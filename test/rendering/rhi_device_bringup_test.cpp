//   /   -- the device-creation leg of the
// RhiDeviceBringupGpu proving signal: Diligent creates GL AND Vulkan devices
// HEADLESS under the ucrt64 GCC 15 toolchain, and LUMIN_RHI parses all three
// backends. Compile-green is not enough; these are live devices on the box.
//
// GPU lane: needs a real driver (GL 4.5 context + a Vulkan device). The GL leg
// skips loudly if a headless GL context cannot be created; the parse leg always
// runs.

#include "rendering/rhi/Device.h"
#include "rendering/rhi/RhiBackend.h"

#include <GLFW/glfw3.h>
#include <gtest/gtest.h>

#include <string>

namespace {

using Luminumbra::Rendering::Rhi::Backend;
using Luminumbra::Rendering::Rhi::CreateHeadlessDevice;
using Luminumbra::Rendering::Rhi::ParseRhiBackend;

// Minimal hidden GL context (mirrors dual_backend_flip_test's HiddenGlContext,
// minus glad -- Diligent loads GL entry points itself; we only owe it a current
// context on this thread for AttachToActiveGLContext).
class HiddenGlContext {
public:
    HiddenGlContext() {
        if (!glfwInit()) {
            m_error = "glfwInit failed";
            return;
        }
        m_glfw_initialized = true;
        glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
        glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 4);
        glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 5);
        glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
        m_window = glfwCreateWindow(64, 64, "rhi_device_bringup_test", nullptr, nullptr);
        if (!m_window) {
            m_error = "glfwCreateWindow failed";
            return;
        }
        glfwMakeContextCurrent(m_window);
        m_ready = true;
    }
    ~HiddenGlContext() {
        if (m_window)
            glfwDestroyWindow(m_window);
        if (m_glfw_initialized)
            glfwTerminate();
    }
    bool ready() const {
        return m_ready;
    }
    const std::string& error() const {
        return m_error;
    }

private:
    GLFWwindow* m_window = nullptr;
    bool m_glfw_initialized = false;
    bool m_ready = false;
    std::string m_error;
};

class RhiDeviceBringupGpu : public ::testing::Test {
protected:
    static HiddenGlContext* s_context;
    static void SetUpTestSuite() {
        s_context = new HiddenGlContext();
    }
    static void TearDownTestSuite() {
        delete s_context;
        s_context = nullptr;
    }
};

HiddenGlContext* RhiDeviceBringupGpu::s_context = nullptr;

TEST_F(RhiDeviceBringupGpu, GlDeviceCreatesHeadless) {
    if (s_context == nullptr || !s_context->ready()) {
        GTEST_SKIP() << "no headless GL context: "
                     << (s_context ? s_context->error() : "context not constructed");
    }
    const auto result = CreateHeadlessDevice(Backend::Gl);
    ASSERT_TRUE(result.created) << "GL device not created: " << result.diagnostic;
    EXPECT_EQ(result.backend, "gl");
    EXPECT_FALSE(result.adapter.empty()) << "adapter description empty -> vacuous device";
}

TEST_F(RhiDeviceBringupGpu, VulkanDeviceCreatesHeadless) {
    // Vulkan creates its own instance/device; no GL context needed.
    const auto result = CreateHeadlessDevice(Backend::Vulkan);
    ASSERT_TRUE(result.created) << "Vulkan device not created: " << result.diagnostic;
    EXPECT_EQ(result.backend, "vulkan");
    EXPECT_FALSE(result.adapter.empty()) << "adapter description empty -> vacuous device";
}

TEST_F(RhiDeviceBringupGpu, LuminRhiParsesSupportedBackends) {
    EXPECT_EQ(ParseRhiBackend("gl"), Backend::Gl);
    EXPECT_EQ(ParseRhiBackend("vulkan"), Backend::Vulkan);
}

} // namespace
