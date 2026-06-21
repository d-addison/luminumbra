// Spec 002 Item 1 — the create-world LIVE WORLD-PREVIEW DIORAMA.
//
// These tests drive the REAL engine pipeline headlessly (hidden 4.5-core GL
// window) through the WorldgenPreview controller: build a bounded candidate
// world, render it into the offscreen FBO via renderPipeline.render_frame, and
// assert (a) it renders without crashing, (b) changing params / weather / tod
// changes the FBO pixels, (c) orbit changes the view, (d) the per-frame render
// holds a budget at the preview FBO size, and (e) rapid param changes collapse
// into ONE debounced, latest-wins rebuild. They use a FIXED TerrainGenParams
// literal (NOT default.json — that file is owned/enriched by the concurrent
// worldgen agent), so they never go RED on a preset change.
//
// GL-required: the cases GTEST_SKIP without a GL context, and the UI gate
// promotes any "SKIPPED" line on this target to a ctest FAILURE (spec 002 3a),
// so a real gate run must genuinely render these.

#include "gtest/gtest.h"

#include <glad/glad.h>
#include <GLFW/glfw3.h>

#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <numeric>
#include <vector>

#include "luminumbra_common/systems/SHIELD_WorldSystem.h"
#include "rendering/RenderPipeline.h"
#include "world/WorldgenPreview.h"

namespace fs = std::filesystem;

namespace {

#ifndef LUMINUMBRA_SOURCE_ROOT
#define LUMINUMBRA_SOURCE_ROOT "."
#endif

fs::path SourceRoot() {
    return fs::weakly_canonical(fs::path(LUMINUMBRA_SOURCE_ROOT));
}

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
        m_window = glfwCreateWindow(900, 700, "worldgen_preview_test", nullptr, nullptr);
        if (!m_window) {
            m_error = "glfwCreateWindow failed";
            return;
        }
        glfwMakeContextCurrent(m_window);
        if (!gladLoadGLLoader(reinterpret_cast<GLADloadproc>(glfwGetProcAddress))) {
            m_error = "gladLoadGLLoader failed";
            return;
        }
        m_ready = true;
    }
    ~HiddenGlContext() {
        if (m_window) glfwDestroyWindow(m_window);
        if (m_glfw_initialized) glfwTerminate();
    }
    bool ready() const { return m_ready; }
    const std::string& error() const { return m_error; }

private:
    GLFWwindow* m_window = nullptr;
    bool m_glfw_initialized = false;
    bool m_ready = false;
    std::string m_error;
};

// A self-contained FIXED candidate. NOT default.json. Biomes/structures stay
// off so no data tables are needed; shaping on so there is real relief.
Luminumbra::Systems::TerrainGenParams FixedCandidate() {
    Luminumbra::Systems::TerrainGenParams p;
    p.base_frequency = 0.012f;
    p.base_amplitude = 55.0f;
    p.octaves = 5;
    p.persistence = 0.5f;
    p.lacunarity = 2.0f;
    p.height_offset = 8.0f;
    p.caves_enabled = false;
    p.shaping_enabled = true;
    p.peaks_amplitude = 48.0f;
    p.peaks_frequency = 0.0025f;
    p.domain_warp_amplitude = 18.0f;
    return p;
}

// Read the preview FBO color back as RGBA8.
std::vector<unsigned char> ReadTarget(const Luminumbra::Client::WorldgenPreview& preview) {
    const int w = preview.target_width();
    const int h = preview.target_height();
    std::vector<unsigned char> px(static_cast<std::size_t>(w) * h * 4u);
    glBindTexture(GL_TEXTURE_2D, preview.color_texture());
    glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
    glBindTexture(GL_TEXTURE_2D, 0);
    return px;
}

// Sum of absolute per-byte differences between two equal-size buffers.
std::uint64_t PixelDelta(const std::vector<unsigned char>& a,
                         const std::vector<unsigned char>& b) {
    const std::size_t n = std::min(a.size(), b.size());
    std::uint64_t acc = 0;
    for (std::size_t i = 0; i < n; ++i) {
        acc += static_cast<std::uint64_t>(std::abs(int(a[i]) - int(b[i])));
    }
    return acc;
}

// Number of non-black (foreground) pixels — proves the diorama actually drew.
std::size_t Foreground(const std::vector<unsigned char>& px) {
    std::size_t fg = 0;
    for (std::size_t i = 0; i + 3 < px.size(); i += 4) {
        if (px[i] > 4 || px[i + 1] > 4 || px[i + 2] > 4) ++fg;
    }
    return fg;
}

constexpr int kPreviewW = 640;
constexpr int kPreviewH = 480;

struct PreviewFixture {
    Luminumbra::Rendering::RenderPipeline pipeline;
    Luminumbra::Client::WorldgenPreview preview;
    bool ok = false;

    PreviewFixture() {
        ok = pipeline.startup(kPreviewW, kPreviewH, SourceRoot());
        if (!ok) return;
        preview.set_active(true);
        preview.ensure_target(kPreviewW, kPreviewH);
        preview.set_params(FixedCandidate(), /*seed*/ 4242);
        // Build synchronously now (bypass the debounce) so the first render has a
        // world: tick with a dt past the debounce window.
        preview.tick(1.0f);
    }
};

} // namespace

TEST(WorldgenPreviewTest, RendersCandidateDioramaToTargetWithoutCrashing) {
    HiddenGlContext ctx;
    if (!ctx.ready()) GTEST_SKIP() << ctx.error();

    PreviewFixture fx;
    ASSERT_TRUE(fx.ok) << "RenderPipeline startup failed";
    ASSERT_TRUE(fx.preview.world_ready());

    ASSERT_TRUE(fx.preview.render(fx.pipeline, 1.0f / 60.0f));
    const std::vector<unsigned char> px = ReadTarget(fx.preview);
    // A real lit diorama fills a meaningful fraction of the frame (terrain + sky).
    EXPECT_GT(Foreground(px),
              static_cast<std::size_t>(kPreviewW * kPreviewH / 20));
    // The offscreen redirect must be cleared after render (default-0 path).
    EXPECT_FALSE(fx.pipeline.has_offscreen_target());
}

TEST(WorldgenPreviewTest, ChangingParamsChangesTargetPixels) {
    HiddenGlContext ctx;
    if (!ctx.ready()) GTEST_SKIP() << ctx.error();

    PreviewFixture fx;
    ASSERT_TRUE(fx.ok);
    ASSERT_TRUE(fx.preview.render(fx.pipeline, 1.0f / 60.0f));
    const std::vector<unsigned char> before = ReadTarget(fx.preview);

    // A markedly different relief profile (taller, rougher) => different terrain.
    Luminumbra::Systems::TerrainGenParams p = FixedCandidate();
    p.base_amplitude = 120.0f;
    p.peaks_amplitude = 110.0f;
    p.height_offset = 30.0f;
    const unsigned gen0 = fx.preview.rebuild_generation();
    fx.preview.set_params(p, 4242);
    fx.preview.tick(1.0f); // past debounce -> rebuild
    EXPECT_EQ(fx.preview.rebuild_generation(), gen0 + 1u);
    ASSERT_TRUE(fx.preview.render(fx.pipeline, 1.0f / 60.0f));
    const std::vector<unsigned char> after = ReadTarget(fx.preview);

    EXPECT_GT(PixelDelta(before, after), static_cast<std::uint64_t>(kPreviewW * kPreviewH));
}

TEST(WorldgenPreviewTest, ChangingWeatherAndTimeOfDayChangesTargetPixels) {
    HiddenGlContext ctx;
    if (!ctx.ready()) GTEST_SKIP() << ctx.error();

    PreviewFixture fx;
    ASSERT_TRUE(fx.ok);

    fx.preview.set_time_of_day(0.05f); // near-noon, bright
    fx.preview.set_weather(Luminumbra::Client::WorldgenPreview::Weather::Clear);
    ASSERT_TRUE(fx.preview.render(fx.pipeline, 1.0f / 60.0f));
    const std::vector<unsigned char> noon = ReadTarget(fx.preview);

    fx.preview.set_time_of_day(0.24f); // golden dusk
    fx.preview.set_weather(Luminumbra::Client::WorldgenPreview::Weather::Storm);
    ASSERT_TRUE(fx.preview.render(fx.pipeline, 1.0f / 60.0f));
    const std::vector<unsigned char> dusk_storm = ReadTarget(fx.preview);

    EXPECT_GT(PixelDelta(noon, dusk_storm), static_cast<std::uint64_t>(kPreviewW * kPreviewH));
}

TEST(WorldgenPreviewTest, OrbitChangesTheView) {
    HiddenGlContext ctx;
    if (!ctx.ready()) GTEST_SKIP() << ctx.error();

    PreviewFixture fx;
    ASSERT_TRUE(fx.ok);

    fx.preview.reset_view();
    ASSERT_TRUE(fx.preview.render(fx.pipeline, 1.0f / 60.0f));
    const std::vector<unsigned char> a = ReadTarget(fx.preview);
    const float yaw0 = fx.preview.orbit_yaw();

    fx.preview.orbit(/*dyaw*/ 70.0f, /*dpitch*/ 12.0f);
    EXPECT_NE(fx.preview.orbit_yaw(), yaw0);
    ASSERT_TRUE(fx.preview.render(fx.pipeline, 1.0f / 60.0f));
    const std::vector<unsigned char> b = ReadTarget(fx.preview);

    EXPECT_GT(PixelDelta(a, b), static_cast<std::uint64_t>(kPreviewW * kPreviewH));
}

TEST(WorldgenPreviewTest, RebuildIsDebouncedAndLatestWins) {
    HiddenGlContext ctx;
    if (!ctx.ready()) GTEST_SKIP() << ctx.error();

    PreviewFixture fx;
    ASSERT_TRUE(fx.ok);
    const unsigned gen0 = fx.preview.rebuild_generation();

    // 5 rapid param changes, each within the debounce window. Tiny dt ticks keep
    // re-arming the timer, so NONE of them rebuild yet.
    for (int i = 0; i < 5; ++i) {
        Luminumbra::Systems::TerrainGenParams p = FixedCandidate();
        p.base_amplitude = 40.0f + static_cast<float>(i) * 12.0f;
        fx.preview.set_params(p, 4242);
        fx.preview.tick(0.01f); // well under the 0.25 s debounce
    }
    EXPECT_EQ(fx.preview.rebuild_generation(), gen0) << "no rebuild should fire mid-burst";

    // Let the debounce window elapse -> exactly ONE rebuild for the whole burst.
    fx.preview.tick(1.0f);
    EXPECT_EQ(fx.preview.rebuild_generation(), gen0 + 1u);
    // And no further rebuild once settled.
    fx.preview.tick(1.0f);
    EXPECT_EQ(fx.preview.rebuild_generation(), gen0 + 1u);
}

TEST(WorldgenPreviewTest, PerFrameRenderHoldsPreviewBudget) {
    HiddenGlContext ctx;
    if (!ctx.ready()) GTEST_SKIP() << ctx.error();

    PreviewFixture fx;
    ASSERT_TRUE(fx.ok);
    ASSERT_TRUE(fx.preview.world_ready());

    // Warm-up render (shader/state/pipeline warm), then time a few frames. The
    // preview re-renders only when something changed, so a steady-state frame at
    // the small FBO must hold a generous create-screen budget. We measure the
    // wall cost of render_frame-to-FBO + the GL flush.
    fx.preview.render(fx.pipeline, 1.0f / 60.0f);
    glFinish();

    constexpr int kFrames = 8;
    double worst_ms = 0.0;
    for (int i = 0; i < kFrames; ++i) {
        // Nudge the camera each frame so the render path actually re-runs.
        fx.preview.orbit(3.0f, 0.0f);
        const auto t0 = std::chrono::steady_clock::now();
        fx.preview.render(fx.pipeline, 1.0f / 60.0f);
        glFinish();
        const auto t1 = std::chrono::steady_clock::now();
        const double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
        worst_ms = std::max(worst_ms, ms);
    }
    // Budget: the preview is small (640x480) + bounded radius. Debug + software
    // GL on a CI box is slow, so this is a generous ceiling that still catches a
    // gross regression (e.g. accidentally streaming the full world).
    EXPECT_LT(worst_ms, 250.0) << "preview frame too expensive: " << worst_ms << " ms";
}
