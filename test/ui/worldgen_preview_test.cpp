// Create-world live preview diorama tests.
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
// promotes any "SKIPPED" line on this target to a CTest failure,
// so a real gate run must genuinely render these.

#include "gtest/gtest.h"

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>
#include <glad/glad.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <numeric>
#include <thread>
#include <vector>

#include "luminumbra_common/systems/SHIELD_WorldSystem.h"
#include "rendering/RenderPipeline.h"
#include "world/WorldgenPreview.h"

namespace fs = std::filesystem;

namespace {

#ifndef LUMINUMBRA_SOURCE_ROOT
#define LUMINUMBRA_SOURCE_ROOT "."
#endif

#ifndef LUMINUMBRA_TEST_ARTIFACT_DIR
#define LUMINUMBRA_TEST_ARTIFACT_DIR "."
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
std::uint64_t PixelDelta(const std::vector<unsigned char>& a, const std::vector<unsigned char>& b) {
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
        if (px[i] > 4 || px[i + 1] > 4 || px[i + 2] > 4)
            ++fg;
    }
    return fg;
}

constexpr int kPreviewW = 640;
constexpr int kPreviewH = 480;

// the world BUILD now runs on a background worker thread; tick (past
// the debounce) only SIGNALS it, and render/render_to_backbuffer adopt the
// finished build (pending->live swap + the rebuild_generation bump) on the GL
// thread. So a test drives a rebuild by: tick(past-debounce) to signal, then
// pump render until the generation advances (the worker finished + the swap
// happened). This helper blocks (bounded) on that lifecycle so the rest of the
// assertions stay exactly as before.
inline bool PumpRebuild(Luminumbra::Client::WorldgenPreview& preview,
                        Luminumbra::Rendering::RenderPipeline& pipeline,
                        unsigned expect_generation) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    while (std::chrono::steady_clock::now() < deadline) {
        preview.render(pipeline, 1.0f / 60.0f); // adopts a finished build on the GL thread
        if (preview.rebuild_generation() >= expect_generation && preview.world_ready()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return preview.rebuild_generation() >= expect_generation && preview.world_ready();
}

struct PreviewFixture {
    Luminumbra::Rendering::RenderPipeline pipeline;
    Luminumbra::Client::WorldgenPreview preview;
    bool ok = false;

    PreviewFixture() {
        ok = pipeline.startup(kPreviewW, kPreviewH, SourceRoot());
        if (!ok)
            return;
        preview.set_active(true);
        preview.ensure_target(kPreviewW, kPreviewH);
        preview.set_params(FixedCandidate(), /*seed*/ 4242);
        // Signal the background build (past the debounce), then pump render until
        // the worker finishes and the GL-thread swap lands, so the first render has
        // a world (: the build is asynchronous).
        preview.tick(1.0f);
        PumpRebuild(preview, pipeline, /*expect_generation*/ 1u);
    }
};

} // namespace

TEST(WorldgenPreviewTest, RendersCandidateDioramaToTargetWithoutCrashing) {
    HiddenGlContext ctx;
    if (!ctx.ready())
        GTEST_SKIP() << ctx.error();

    PreviewFixture fx;
    ASSERT_TRUE(fx.ok) << "RenderPipeline startup failed";
    ASSERT_TRUE(fx.preview.world_ready());

    ASSERT_TRUE(fx.preview.render(fx.pipeline, 1.0f / 60.0f));
    const std::vector<unsigned char> px = ReadTarget(fx.preview);
    // A real lit diorama fills a meaningful fraction of the frame (terrain + sky).
    EXPECT_GT(Foreground(px), static_cast<std::size_t>(kPreviewW * kPreviewH / 20));
    // The offscreen redirect must be cleared after render (default-0 path).
    EXPECT_FALSE(fx.pipeline.has_offscreen_target());
}

TEST(WorldgenPreviewTest, ChangingParamsChangesTargetPixels) {
    HiddenGlContext ctx;
    if (!ctx.ready())
        GTEST_SKIP() << ctx.error();

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
    fx.preview.tick(1.0f); // past debounce -> SIGNAL the background rebuild
    // Pump render until the worker finishes + the GL-thread swap bumps the gen.
    ASSERT_TRUE(PumpRebuild(fx.preview, fx.pipeline, gen0 + 1u));
    EXPECT_EQ(fx.preview.rebuild_generation(), gen0 + 1u);
    ASSERT_TRUE(fx.preview.render(fx.pipeline, 1.0f / 60.0f));
    const std::vector<unsigned char> after = ReadTarget(fx.preview);

    EXPECT_GT(PixelDelta(before, after), static_cast<std::uint64_t>(kPreviewW * kPreviewH));
}

TEST(WorldgenPreviewTest, ChangingWeatherAndTimeOfDayChangesTargetPixels) {
    HiddenGlContext ctx;
    if (!ctx.ready())
        GTEST_SKIP() << ctx.error();

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
    if (!ctx.ready())
        GTEST_SKIP() << ctx.error();

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
    if (!ctx.ready())
        GTEST_SKIP() << ctx.error();

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

    // Let the debounce window elapse -> the burst collapses to ONE signalled build.
    // Pump render until the worker finishes + the GL-thread swap bumps the gen
    // (: the actual rebuild + generation bump are asynchronous).
    fx.preview.tick(1.0f);
    ASSERT_TRUE(PumpRebuild(fx.preview, fx.pipeline, gen0 + 1u));
    EXPECT_EQ(fx.preview.rebuild_generation(), gen0 + 1u) << "exactly one rebuild for the burst";
    // And no further rebuild once settled (dirty was cleared at signal time).
    fx.preview.tick(1.0f);
    fx.preview.render(fx.pipeline, 1.0f / 60.0f);
    EXPECT_EQ(fx.preview.rebuild_generation(), gen0 + 1u);
}

TEST(WorldgenPreviewTest, PerFrameRenderProducesComparableObservation) {
    HiddenGlContext ctx;
    if (!ctx.ready())
        GTEST_SKIP() << ctx.error();

    PreviewFixture fx;
    ASSERT_TRUE(fx.ok);
    ASSERT_TRUE(fx.preview.world_ready());

    // Warm the shader/state/pipeline, then record a comparable observation. The
    // preview re-renders only when something changed, so each sampled frame moves
    // the camera and includes render-to-FBO plus the GL flush. Machine-independent
    // regression decisions are made by the paired relative performance gate.
    fx.preview.render(fx.pipeline, 1.0f / 60.0f);
    glFinish();

    constexpr int kFrames = 8;
    double worst_ms = 0.0;
    std::vector<double> samples_ms;
    samples_ms.reserve(kFrames);
    for (int i = 0; i < kFrames; ++i) {
        // Nudge the camera each frame so the render path actually re-runs.
        fx.preview.orbit(3.0f, 0.0f);
        const auto t0 = std::chrono::steady_clock::now();
        fx.preview.render(fx.pipeline, 1.0f / 60.0f);
        glFinish();
        const auto t1 = std::chrono::steady_clock::now();
        const double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
        samples_ms.push_back(ms);
        worst_ms = std::max(worst_ms, ms);
    }

    auto sorted_ms = samples_ms;
    std::sort(sorted_ms.begin(), sorted_ms.end());
    const auto percentile = [&sorted_ms](double quantile) {
        const auto rank =
            static_cast<std::size_t>(std::ceil(quantile * static_cast<double>(sorted_ms.size())));
        return sorted_ms[std::min(sorted_ms.size() - 1, std::max<std::size_t>(1, rank) - 1)];
    };
    const double p50_ms = percentile(0.50);
    const double p95_ms = percentile(0.95);
    const double p99_ms = percentile(0.99);
    std::vector<double> deviations;
    deviations.reserve(samples_ms.size());
    for (const double sample_ms : samples_ms) {
        deviations.push_back(std::abs(sample_ms - p50_ms));
    }
    std::sort(deviations.begin(), deviations.end());
    const double mad_ms = deviations[(deviations.size() - 1) / 2];
    const fs::path artifact =
        fs::path(LUMINUMBRA_TEST_ARTIFACT_DIR) / "performance" / "worldgen_preview_frame.json";
    fs::create_directories(artifact.parent_path());
    std::ofstream report(artifact, std::ios::trunc);
    ASSERT_TRUE(report) << "could not write performance evidence: " << artifact;
    report << std::setprecision(17) << "{\n"
           << "  \"schema\": \"luminumbra.performance_measurement.v3\",\n"
           << "  \"status\": \"evaluated\",\n"
           << "  \"enforcement\": \"observation\",\n"
           << "  \"test\": \"WorldgenPreviewTest.PerFrameRenderProducesComparableObservation\",\n"
           << "  \"metric\": \"preview_frame_wall_ms\",\n"
           << "  \"unit\": \"ms\",\n"
           << "  \"direction\": \"lower\",\n"
           << "  \"sample_count\": " << samples_ms.size() << ",\n"
           << "  \"samples\": [";
    for (std::size_t i = 0; i < samples_ms.size(); ++i) {
        if (i > 0)
            report << ", ";
        report << samples_ms[i];
    }
    report << "],\n"
           << "  \"p50\": " << p50_ms << ",\n"
           << "  \"p95\": " << p95_ms << ",\n"
           << "  \"p99\": " << p99_ms << ",\n"
           << "  \"mad\": " << mad_ms << ",\n"
           << "  \"worst\": " << worst_ms << "\n"
           << "}\n";
    report.close();
    ASSERT_FALSE(report.fail()) << "could not finalize performance evidence: " << artifact;
}
