#include "luminumbra_client/rendering/SkyAtmosphereLut.h"

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <vector>

namespace {
using Luminumbra::Rendering::SkyAtmosphereLut;

TEST(SkyLutPublicationPolicy, OnlyKnownInteractivePlayDefers) {
    using Publication = SkyAtmosphereLut::Publication;
    EXPECT_EQ(SkyAtmosphereLut::select_publication({}), Publication::SameFrame);
    // Exhaust all combinations, including overlapping deterministic modes.
    for (unsigned int mask = 0; mask < 64u; ++mask) {
        const SkyAtmosphereLut::RefreshPolicy policy{.interactive = (mask & 1u) != 0,
                                                     .replay = (mask & 2u) != 0,
                                                     .capture = (mask & 4u) != 0,
                                                     .benchmark = (mask & 8u) != 0,
                                                     .headless = (mask & 16u) != 0,
                                                     .test_harness = (mask & 32u) != 0};
        EXPECT_EQ(SkyAtmosphereLut::select_publication(policy),
                  mask == 1u ? Publication::Deferred : Publication::SameFrame)
            << mask;
    }
}

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

struct ReadbackDriver {
    GLenum status = GL_TIMEOUT_EXPIRED;
    unsigned int checks = 0;
    PFNGLCOPYBUFFERSUBDATAPROC copy = nullptr;
    unsigned int copies = 0;
    GLsizeiptr bytes = 0;
};

ReadbackDriver& Driver() {
    static ReadbackDriver driver;
    return driver;
}

GLenum APIENTRY PollReadback(GLsync, GLbitfield flags, GLuint64 timeout) {
    EXPECT_EQ(flags, 0u);
    EXPECT_EQ(timeout, 0u);
    ++Driver().checks;
    return Driver().status;
}

void APIENTRY CopyReadback(GLenum source,
                           GLenum destination,
                           GLintptr source_offset,
                           GLintptr destination_offset,
                           GLsizeiptr bytes) {
    ++Driver().copies;
    Driver().bytes = bytes;
    const auto copy_buffer = Driver().copy;
    if (copy_buffer == nullptr) {
        ADD_FAILURE() << "Readback copy entry was not installed";
        return;
    }
    copy_buffer(source, destination, source_offset, destination_offset, bytes);
}

GLsync APIENTRY RejectFence(GLenum, GLbitfield) {
    return nullptr;
}

void* APIENTRY RejectMapping(GLenum, GLintptr, GLsizeiptr, GLbitfield) {
    return nullptr;
}

void APIENTRY RejectBuffer(GLsizei count, GLuint* buffers) {
    std::fill_n(buffers, count, 0u);
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
        Driver() = ReadbackDriver{};
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

TEST_F(SkyAtmosphereLutGl, DeferredReadbackPublishesIdenticalBytesOnlyOnSignal) {
    const glm::vec3 noon(0.0f, 1.0f, 0.0f);
    const glm::vec3 dusk = glm::normalize(glm::vec3(-1.0f, 0.01f, -0.2f));
    SkyAtmosphereLut reference;
    SkyAtmosphereLut lut;
    ASSERT_TRUE(reference.initialize(noon));
    ASSERT_TRUE(lut.initialize(noon));
    const auto old_pixels = Pixels(lut);
    const auto old_ambient = lut.sky_ambient();
    reference.set_gpu_skyview_enabled(true);
    {
        const ScopedGlEntry<PFNGLCLIENTWAITSYNCPROC> complete(glad_glClientWaitSync,
                                                              CompleteReadback);
        ASSERT_TRUE(reference.refresh_sky_view(dusk));
    }
    lut.set_gpu_skyview_enabled(true);
    Driver().copy = glad_glCopyBufferSubData;
    const ScopedGlEntry<PFNGLCOPYBUFFERSUBDATAPROC> copy(glad_glCopyBufferSubData, CopyReadback);
    const ScopedGlEntry<PFNGLCLIENTWAITSYNCPROC> poll(glad_glClientWaitSync, PollReadback);
    constexpr auto deferred = SkyAtmosphereLut::Publication::Deferred;
    double refresh_cost = -1.0;
    EXPECT_FALSE(lut.refresh_sky_view(dusk, &refresh_cost, deferred));
    EXPECT_TRUE(lut.readback_pending());
    EXPECT_EQ(refresh_cost, -1.0);
    EXPECT_EQ(Driver().checks, 0u);
    EXPECT_EQ(Driver().copies, 1u);
    EXPECT_EQ(Driver().bytes, 248832);
    // Repeated pending calls, including a changed input, neither resubmit nor
    // run the CPU fallback. Publication stays at the previous complete input.
    for (int frame = 0; frame < 4; ++frame) {
        EXPECT_FALSE(lut.refresh_sky_view(noon, nullptr, deferred));
        EXPECT_EQ(lut.sky_view_sun_dir(), noon);
        EXPECT_EQ(lut.sky_ambient(), old_ambient);
        EXPECT_EQ(Pixels(lut), old_pixels);
    }
    EXPECT_EQ(Driver().checks, 4u);
    EXPECT_EQ(Driver().copies, 1u);
    // Complete the real copy outside the call under test. The fake driver then
    // exposes its signal without waiting inside the refresh.
    glFinish();
    Driver().status = GL_CONDITION_SATISFIED;
    ASSERT_TRUE(lut.refresh_sky_view(noon, &refresh_cost, deferred));
    EXPECT_FALSE(lut.readback_pending());
    EXPECT_NE(refresh_cost, -1.0);
    EXPECT_EQ(Driver().checks, 5u);
    EXPECT_EQ(Driver().copies, 1u);
    EXPECT_EQ(lut.sky_view_sun_dir(), reference.sky_view_sun_dir());
    EXPECT_EQ(lut.sky_ambient(), reference.sky_ambient());
    EXPECT_EQ(Pixels(lut), Pixels(reference));
    EXPECT_FALSE(lut.refresh_sky_view(dusk, nullptr, deferred));
    EXPECT_EQ(Driver().checks, 5u);
    // Reuse the PBO for another exact input and adopt on ALREADY_SIGNALED too.
    ASSERT_FALSE(lut.refresh_sky_view(noon, nullptr, deferred));
    EXPECT_EQ(Driver().copies, 2u);
    glFinish();
    Driver().status = GL_ALREADY_SIGNALED;
    ASSERT_TRUE(lut.refresh_sky_view(noon, nullptr, deferred));
    EXPECT_EQ(lut.sky_view_sun_dir(), noon);
}

TEST_F(SkyAtmosphereLutGl, DeferredWaitFailureFallsBackForSubmittedInput) {
    const glm::vec3 noon(0.0f, 1.0f, 0.0f);
    const glm::vec3 dusk = glm::normalize(glm::vec3(-1.0f, 0.01f, -0.2f));
    SkyAtmosphereLut reference;
    SkyAtmosphereLut lut;
    ASSERT_TRUE(reference.initialize(dusk));
    ASSERT_TRUE(lut.initialize(noon));
    lut.set_gpu_skyview_enabled(true);
    const ScopedGlEntry<PFNGLCLIENTWAITSYNCPROC> poll(glad_glClientWaitSync, PollReadback);
    constexpr auto deferred = SkyAtmosphereLut::Publication::Deferred;
    ASSERT_FALSE(lut.refresh_sky_view(dusk, nullptr, deferred));
    Driver().status = GL_WAIT_FAILED;
    ASSERT_TRUE(lut.refresh_sky_view(noon, nullptr, deferred));
    EXPECT_EQ(Driver().checks, 1u);
    MatchesCpu(lut, reference);
    // Failure releases the slot; the next input can start a new readback.
    ASSERT_FALSE(lut.refresh_sky_view(noon, nullptr, deferred));
    lut.destroy();
    EXPECT_FALSE(lut.ready());
    EXPECT_FALSE(lut.refresh_sky_view(noon, nullptr, deferred));
    lut.set_gpu_skyview_enabled(false);
    ASSERT_TRUE(lut.initialize(dusk));
    EXPECT_FALSE(lut.refresh_sky_view(dusk, nullptr, deferred));
}

TEST_F(SkyAtmosphereLutGl, DeferredResourceFailuresMatchCpu) {
    const glm::vec3 noon(0.0f, 1.0f, 0.0f);
    const glm::vec3 dusk = glm::normalize(glm::vec3(-1.0f, 0.01f, -0.2f));
    SkyAtmosphereLut reference;
    ASSERT_TRUE(reference.initialize(dusk));
    constexpr auto deferred = SkyAtmosphereLut::Publication::Deferred;
    // Each genuine failure must publish the CPU result in this invocation.
    for (int failure = 0; failure < 4; ++failure) {
        SkyAtmosphereLut lut;
        ASSERT_TRUE(lut.initialize(noon));
        lut.set_gpu_skyview_enabled(true);
        if (failure == 0) {
            const ScopedGlEntry<PFNGLGENBUFFERSPROC> reject(glad_glGenBuffers, RejectBuffer);
            ASSERT_TRUE(lut.refresh_sky_view(dusk, nullptr, deferred));
        } else if (failure == 1) {
            const ScopedGlEntry<PFNGLMAPBUFFERRANGEPROC> reject(glad_glMapBufferRange,
                                                                RejectMapping);
            ASSERT_TRUE(lut.refresh_sky_view(dusk, nullptr, deferred));
        } else if (failure == 2) {
            const ScopedGlEntry<PFNGLFENCESYNCPROC> reject(glad_glFenceSync, RejectFence);
            ASSERT_TRUE(lut.refresh_sky_view(dusk, nullptr, deferred));
        } else {
            const ScopedGlEntry<PFNGLGETSHADERIVPROC> reject(glad_glGetShaderiv, RejectShader);
            ASSERT_TRUE(lut.refresh_sky_view(dusk, nullptr, deferred));
        }
        MatchesCpu(lut, reference);
        // Every refusal leaves the next deferred request usable.
        ASSERT_FALSE(lut.refresh_sky_view(noon, nullptr, deferred));
    }
}

TEST_F(SkyAtmosphereLutGl, DeferredModeChangesDiscardPendingInput) {
    const glm::vec3 noon(0.0f, 1.0f, 0.0f);
    const glm::vec3 dusk = glm::normalize(glm::vec3(-1.0f, 0.01f, -0.2f));
    const glm::vec3 dawn = glm::normalize(glm::vec3(1.0f, 0.1f, 0.2f));
    constexpr auto deferred = SkyAtmosphereLut::Publication::Deferred;
    for (const bool cpu_mode : {false, true}) {
        SkyAtmosphereLut lut;
        ASSERT_TRUE(lut.initialize(noon));
        const auto old_pixels = Pixels(lut);
        const auto old_ambient = lut.sky_ambient();
        lut.set_gpu_skyview_enabled(true);
        const ScopedGlEntry<PFNGLCLIENTWAITSYNCPROC> poll(glad_glClientWaitSync, PollReadback);
        ASSERT_FALSE(lut.refresh_sky_view(dusk, nullptr, deferred));
        if (cpu_mode)
            lut.set_gpu_skyview_enabled(false);
        EXPECT_FALSE(lut.refresh_sky_view(
            noon, nullptr, cpu_mode ? deferred : SkyAtmosphereLut::Publication::SameFrame));
        EXPECT_EQ(lut.sky_view_sun_dir(), noon);
        EXPECT_EQ(lut.sky_ambient(), old_ambient);
        EXPECT_EQ(Pixels(lut), old_pixels);
        EXPECT_EQ(Driver().checks, 0u);

        // A new current input follows the newly selected policy, rather than
        // adopting the discarded dusk input when its old fence later signals.
        lut.set_gpu_skyview_enabled(true);
        ASSERT_FALSE(lut.refresh_sky_view(dusk, nullptr, deferred));
        if (cpu_mode) {
            lut.set_gpu_skyview_enabled(false);
            SkyAtmosphereLut reference;
            ASSERT_TRUE(reference.initialize(dawn));
            ASSERT_TRUE(lut.refresh_sky_view(dawn, nullptr, deferred));
            MatchesCpu(lut, reference);
        } else {
            SkyAtmosphereLut reference;
            ASSERT_TRUE(reference.initialize(noon));
            reference.set_gpu_skyview_enabled(true);
            const ScopedGlEntry<PFNGLCLIENTWAITSYNCPROC> complete(glad_glClientWaitSync,
                                                                  CompleteReadback);
            ASSERT_TRUE(reference.refresh_sky_view(dawn));
            ASSERT_TRUE(lut.refresh_sky_view(dawn));
            EXPECT_EQ(lut.sky_view_sun_dir(), reference.sky_view_sun_dir());
            EXPECT_EQ(lut.sky_ambient(), reference.sky_ambient());
            EXPECT_EQ(Pixels(lut), Pixels(reference));
        }
        EXPECT_EQ(Driver().checks, 0u);
    }
}

TEST_F(SkyAtmosphereLutGl, DeferredCancellationAndReinitializationDiscardPendingInput) {
    const glm::vec3 noon(0.0f, 1.0f, 0.0f);
    const glm::vec3 dusk = glm::normalize(glm::vec3(-1.0f, 0.01f, -0.2f));
    constexpr auto deferred = SkyAtmosphereLut::Publication::Deferred;
    for (int cancellation = 0; cancellation < 4; ++cancellation) {
        SkyAtmosphereLut lut;
        ASSERT_TRUE(lut.initialize(noon));
        const auto old_pixels = Pixels(lut);
        const auto old_ambient = lut.sky_ambient();
        lut.set_gpu_skyview_enabled(true);
        const ScopedGlEntry<PFNGLCLIENTWAITSYNCPROC> poll(glad_glClientWaitSync, PollReadback);
        const auto checks_before = Driver().checks;
        ASSERT_FALSE(lut.refresh_sky_view(dusk, nullptr, deferred));
        EXPECT_TRUE(lut.readback_pending());
        if (cancellation == 0) {
            // Graphics settings cancel without replacing the published sky.
            lut.discard_pending_readback();
            lut.discard_pending_readback(); // repeated cancellation is harmless
        } else if (cancellation == 1) {
            // Switching back before another refresh must not revive old work.
            lut.set_gpu_skyview_enabled(false);
            lut.set_gpu_skyview_enabled(true);
        } else {
            if (cancellation == 2)
                lut.destroy();
            // Direct reinitialization also discards pending work. Simulate GPU
            // initialization refusal so the rebuilt reference remains CPU-exact.
            const ScopedGlEntry<PFNGLGETSHADERIVPROC> reject(glad_glGetShaderiv, RejectShader);
            ASSERT_TRUE(lut.initialize(noon));
        }
        EXPECT_FALSE(lut.readback_pending());
        EXPECT_FALSE(lut.refresh_sky_view(noon, nullptr, deferred));
        EXPECT_EQ(Driver().checks, checks_before);
        EXPECT_EQ(lut.sky_view_sun_dir(), noon);
        EXPECT_EQ(lut.sky_ambient(), old_ambient);
        EXPECT_EQ(Pixels(lut), old_pixels);
        // Cancellation releases the slot for a new input without a fence wait.
        ASSERT_FALSE(lut.refresh_sky_view(dusk, nullptr, deferred));
        EXPECT_EQ(Driver().checks, checks_before);
    }
}
} // namespace
