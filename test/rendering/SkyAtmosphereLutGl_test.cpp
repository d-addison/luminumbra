#include "luminumbra_client/rendering/SkyAtmosphereLut.h"

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>
#include <gtest/gtest.h>

#include <cmath>
#include <vector>

namespace {
using Luminumbra::Rendering::SkyAtmosphereLut;

template<typename Function>
class ScopedGlEntry {
public:
    ScopedGlEntry(Function& entry, Function replacement)
        : m_entry(entry)
        , m_original(entry) {
        m_entry = replacement;
    }
    ~ScopedGlEntry() {
        m_entry = m_original;
    }
    ScopedGlEntry(const ScopedGlEntry&) = delete;
    ScopedGlEntry& operator=(const ScopedGlEntry&) = delete;

private:
    Function& m_entry;
    Function m_original;
};

GLenum APIENTRY CompleteReadback(GLsync, GLbitfield, GLuint64) {
    // Finish real compute/copy commands so initialization succeeds independently
    // of scheduling on a loaded software renderer. Only the wait entry is replaced.
    glFinish();
    return GL_ALREADY_SIGNALED;
}

GLenum APIENTRY ExpireReadback(GLsync, GLbitfield, GLuint64) {
    return GL_TIMEOUT_EXPIRED;
}

void APIENTRY RejectShader(GLuint, GLenum, GLint* status) {
    *status = GL_FALSE;
}

class SkyAtmosphereLutGl : public ::testing::Test {
protected:
    void SetUp() override {
        m_initialized = glfwInit() == GLFW_TRUE;
        ASSERT_TRUE(m_initialized);
        glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
        glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 4);
        glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 5);
        glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
        m_window = glfwCreateWindow(64, 64, "sky_atmosphere_lut", nullptr, nullptr);
        ASSERT_NE(m_window, nullptr);
        glfwMakeContextCurrent(m_window);
        m_loaded = gladLoadGLLoader(reinterpret_cast<GLADloadproc>(glfwGetProcAddress)) != 0;
        ASSERT_TRUE(m_loaded);
    }

    void TearDown() override {
        if (m_loaded) {
            EXPECT_EQ(glGetError(), static_cast<GLenum>(GL_NO_ERROR));
        }
        if (m_window != nullptr) {
            glfwDestroyWindow(m_window);
        }
        if (m_initialized) {
            glfwTerminate();
        }
    }

    static std::vector<float> Pixels(const SkyAtmosphereLut& lut) {
        // Fixed RGB sky-view readback: at most 192 * 108 * 3 floats.
        std::vector<float> pixels(static_cast<std::size_t>(SkyAtmosphereLut::kSkyViewWidth) *
                                  SkyAtmosphereLut::kSkyViewHeight * 3u);
        glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
        glBindTexture(GL_TEXTURE_2D, lut.sky_view_texture());
        glPixelStorei(GL_PACK_ALIGNMENT, 4);
        glGetTexImage(GL_TEXTURE_2D, 0, GL_RGB, GL_FLOAT, pixels.data());
        glBindTexture(GL_TEXTURE_2D, 0);
        return pixels;
    }

    static void MatchesCpu(const SkyAtmosphereLut& actual, const SkyAtmosphereLut& reference) {
        ASSERT_TRUE(actual.ready());
        EXPECT_EQ(actual.sky_view_sun_dir(), reference.sky_view_sun_dir());
        const auto ambient = actual.sky_ambient();
        for (int channel = 0; channel < 3; ++channel) {
            EXPECT_TRUE(std::isfinite(ambient[channel]));
            EXPECT_GT(ambient[channel], 0.0f);
            EXPECT_EQ(ambient[channel], reference.sky_ambient()[channel]);
        }
        EXPECT_EQ(actual.sun_transmittance(0.0f), reference.sun_transmittance(0.0f));
        EXPECT_EQ(Pixels(actual), Pixels(reference));
    }

    void GpuSuccessThenFallback(bool deadline) {
        const glm::vec3 noon(0.0f, 1.0f, 0.0f);
        const glm::vec3 dusk = glm::normalize(glm::vec3(-1.0f, 0.01f, -0.2f));
        SkyAtmosphereLut reference;
        ASSERT_TRUE(reference.initialize(dusk));
        SkyAtmosphereLut lut;
        lut.set_gpu_skyview_enabled(true);
        {
            const ScopedGlEntry<PFNGLCLIENTWAITSYNCPROC> complete(glad_glClientWaitSync,
                                                                  CompleteReadback);
            ASSERT_TRUE(lut.initialize(noon));
        }
        // A CPU init fallback would populate this buffer and return extinction
        // below one. Together these checks prove successful GPU-only startup.
        EXPECT_EQ(lut.sun_transmittance(0.0f), glm::vec3(1.0f));
        EXPECT_GT(lut.sky_ambient().x, 0.0f);
        EXPECT_FALSE(lut.refresh_sky_view(noon));
        lut.set_gpu_skyview_enabled(deadline);
        {
            const ScopedGlEntry<PFNGLCLIENTWAITSYNCPROC> expire(glad_glClientWaitSync,
                                                                ExpireReadback);
            ASSERT_TRUE(lut.refresh_sky_view(dusk));
        }
        MatchesCpu(lut, reference);
        EXPECT_LT(lut.sun_transmittance(0.0f).x, 1.0f);

        // Reuse the now-populated base buffers after a window resize and mode
        // change. The atmosphere's pinned resolution does not depend on either.
        glfwSetWindowSize(m_window, 96, 80);
        lut.set_gpu_skyview_enabled(false);
        ASSERT_TRUE(reference.refresh_sky_view(noon));
        ASSERT_TRUE(lut.refresh_sky_view(noon));
        MatchesCpu(lut, reference);
        EXPECT_FALSE(lut.refresh_sky_view(noon));

        lut.destroy();
        EXPECT_FALSE(lut.refresh_sky_view(dusk));
        ASSERT_TRUE(lut.initialize(dusk));
        ASSERT_TRUE(reference.refresh_sky_view(dusk));
        MatchesCpu(lut, reference);
    }

private:
    GLFWwindow* m_window = nullptr;
    bool m_initialized = false;
    bool m_loaded = false;
};

TEST_F(SkyAtmosphereLutGl, GpuInitializationThenReadbackDeadlineMatchesCpu) {
    GpuSuccessThenFallback(true);
}

TEST_F(SkyAtmosphereLutGl, GpuInitializationThenCpuModeMatchesCpu) {
    GpuSuccessThenFallback(false);
}

TEST_F(SkyAtmosphereLutGl, InitializationShaderFailureMatchesCpu) {
    const glm::vec3 dusk = glm::normalize(glm::vec3(-1.0f, 0.01f, -0.2f));
    SkyAtmosphereLut reference;
    ASSERT_TRUE(reference.initialize(dusk));
    SkyAtmosphereLut lut;
    lut.set_gpu_skyview_enabled(true);
    {
        const ScopedGlEntry<PFNGLGETSHADERIVPROC> reject(glad_glGetShaderiv, RejectShader);
        ASSERT_TRUE(lut.initialize(dusk));
    }
    MatchesCpu(lut, reference);
}

TEST_F(SkyAtmosphereLutGl, InitializationReadbackDeadlineMatchesCpu) {
    const glm::vec3 dusk = glm::normalize(glm::vec3(-1.0f, 0.01f, -0.2f));
    SkyAtmosphereLut reference;
    ASSERT_TRUE(reference.initialize(dusk));
    SkyAtmosphereLut lut;
    lut.set_gpu_skyview_enabled(true);
    {
        const ScopedGlEntry<PFNGLCLIENTWAITSYNCPROC> expire(glad_glClientWaitSync, ExpireReadback);
        ASSERT_TRUE(lut.initialize(dusk));
    }
    MatchesCpu(lut, reference);
}
} // namespace
