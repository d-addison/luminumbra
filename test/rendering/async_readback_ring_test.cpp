// the async GPU readback ring contract, exercised against a
// REAL headless GL context (HiddenGlContext, mirroring render_smoke_test.cpp). No
// GL mocking: the ring's fence/persistent-map path runs on real GPU commands.
//
// The contract under test:
//   * submit issues the GPU copy + a fence and returns immediately (never waits).
//   * consume is a NON-BLOCKING poll: it returns the newest COMPLETED result, or
//     an ordinary "no result yet" state — it never stalls on the current frame.
//   * a stale (older) result stays available until its slot is reused.

#include "luminumbra_client/rendering/AsyncReadbackRing.h"

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>
#include <glad/glad.h>
#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace {

// Minimal hidden GL 4.5 context (same shape as render_smoke_test.cpp's helper).
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
        m_window = glfwCreateWindow(64, 64, "async_readback_ring_test", nullptr, nullptr);
        if (!m_window) {
            m_error = "glfwCreateWindow failed (no GL 4.5 context available)";
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

// A GPU-resident source SSBO holding `bytes`.
GLuint make_source_ssbo(const std::vector<std::uint8_t>& bytes) {
    GLuint ssbo = 0;
    glGenBuffers(1, &ssbo);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, ssbo);
    glBufferData(GL_SHADER_STORAGE_BUFFER,
                 static_cast<GLsizeiptr>(bytes.size()),
                 bytes.data(),
                 GL_STATIC_DRAW);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
    return ssbo;
}

using Luminumbra::Rendering::AsyncReadbackRing;

TEST(AsyncReadbackRing, SubmitNonBlockingThenConsumeResult) {
    HiddenGlContext ctx;
    if (!ctx.ready()) {
        GTEST_SKIP() << "no GL context: " << ctx.error();
    }

    constexpr std::size_t kN = 256;
    std::vector<std::uint8_t> src(kN);
    for (std::size_t i = 0; i < kN; ++i) {
        src[i] = static_cast<std::uint8_t>((i * 7 + 3) & 0xFF);
    }
    GLuint src_ssbo = make_source_ssbo(src);

    AsyncReadbackRing ring;
    ASSERT_TRUE(ring.ensure(kN, 3)) << "ring allocation failed";
    EXPECT_TRUE(ring.initialized());
    EXPECT_EQ(ring.depth(), 3);

    // A fresh ring has no completed result.
    const void* p = nullptr;
    std::size_t n = 0;
    EXPECT_FALSE(ring.consume(&p, &n)) << "fresh ring must report no result";

    // Submit a readback. submit must return immediately (no blocking wait).
    ASSERT_TRUE(ring.begin());
    ring.copy_region(src_ssbo, 0, 0, kN);
    ring.submit();

    // A NON-DESTRUCTIVE poll on the submit frame is non-blocking: it may report
    // pending OR (if the tiny copy already finished) ready — but it must NEVER
    // hang, and it must not consume the result. We don't hard-assert "pending"
    // (that would race the GPU). poll leaves the result for consume below.
    (void)ring.poll();

    // Force GPU completion, then the result must be available with the right bytes.
    glFinish();
    EXPECT_TRUE(ring.poll()) << "after completion, poll() must report ready";
    p = nullptr;
    n = 0;
    ASSERT_TRUE(ring.consume(&p, &n)) << "completed readback must be consumable";
    ASSERT_NE(p, nullptr);
    ASSERT_GE(n, kN);
    EXPECT_EQ(0, std::memcmp(p, src.data(), kN)) << "readback bytes must match source";

    // No new submit -> consume returns false (no NEWER result than the last).
    EXPECT_FALSE(ring.consume(&p, &n)) << "consume must not re-deliver the same result";

    glDeleteBuffers(1, &src_ssbo);
}

TEST(AsyncReadbackRing, SlotRotationDeliversNewestResult) {
    HiddenGlContext ctx;
    if (!ctx.ready()) {
        GTEST_SKIP() << "no GL context: " << ctx.error();
    }

    constexpr std::size_t kN = 128;
    AsyncReadbackRing ring;
    ASSERT_TRUE(ring.ensure(kN, 3));

    std::vector<std::uint8_t> a(kN, 0xAA), b(kN, 0xBB);
    GLuint sa = make_source_ssbo(a);
    GLuint sb = make_source_ssbo(b);

    const void* p = nullptr;
    std::size_t n = 0;

    // First readback -> 0xAA.
    ASSERT_TRUE(ring.begin());
    ring.copy_region(sa, 0, 0, kN);
    ring.submit();
    glFinish();
    ASSERT_TRUE(ring.consume(&p, &n));
    EXPECT_EQ(static_cast<const std::uint8_t*>(p)[0], 0xAA);

    // Second readback into the NEXT slot -> 0xBB; consume returns the newer one.
    ASSERT_TRUE(ring.begin());
    ring.copy_region(sb, 0, 0, kN);
    ring.submit();
    glFinish();
    ASSERT_TRUE(ring.consume(&p, &n));
    EXPECT_EQ(static_cast<const std::uint8_t*>(p)[0], 0xBB)
        << "consume must deliver the newest completed slot";

    glDeleteBuffers(1, &sa);
    glDeleteBuffers(1, &sb);
}

// Mirrors the foliage consumer EXACTLY: two copy_regions into one slot
// -- a count at byte offset 8 of a "count" SSBO -> slot+0, then a blade array ->
// slot+4 -- then reconstruct count from slot+0 and blades from slot+4. This is the
// offset arithmetic / multi-region pattern the (currently-broken) FoliageInstancing
// gate cannot exercise, so it is validated here instead.
TEST(AsyncReadbackRing, FoliageStyleTwoRegionReadback) {
    HiddenGlContext ctx;
    if (!ctx.ready()) {
        GTEST_SKIP() << "no GL context: " << ctx.error();
    }

    // "count" SSBO laid out like FoliagePass m_count_ssbo:
    // [append@0][vtx=12@4][instanceCount@8][first@12][base@16].
    const std::uint32_t kBlades = 5;
    std::uint32_t count_buf[5] = {kBlades, 12u, kBlades, 0u, 0u};
    GLuint count_ssbo = 0;
    glGenBuffers(1, &count_ssbo);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, count_ssbo);
    glBufferData(GL_SHADER_STORAGE_BUFFER, sizeof(count_buf), count_buf, GL_STATIC_DRAW);

    // "blade" SSBO of known 4-byte records (stand-in for InstanceRecord).
    std::vector<std::uint32_t> blades(kBlades);
    for (std::uint32_t i = 0; i < kBlades; ++i)
        blades[i] = 0xB1AD0000u | i;
    GLuint blade_ssbo = 0;
    glGenBuffers(1, &blade_ssbo);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, blade_ssbo);
    glBufferData(GL_SHADER_STORAGE_BUFFER,
                 static_cast<GLsizeiptr>(blades.size() * sizeof(std::uint32_t)),
                 blades.data(),
                 GL_STATIC_DRAW);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);

    constexpr std::size_t kCountBytes = sizeof(std::uint32_t);
    const std::size_t slot_bytes = kCountBytes + blades.size() * sizeof(std::uint32_t);

    AsyncReadbackRing ring;
    ASSERT_TRUE(ring.ensure(slot_bytes, 3));
    ASSERT_TRUE(ring.begin());
    // count: read draw_instance_count at byte offset 8 -> slot+0 (matches foliage).
    ring.copy_region(count_ssbo, sizeof(std::uint32_t) * 2, 0, kCountBytes);
    // blades: whole array -> slot + kCountBytes (matches foliage).
    ring.copy_region(blade_ssbo,
                     0,
                     static_cast<std::ptrdiff_t>(kCountBytes),
                     blades.size() * sizeof(std::uint32_t));
    ring.submit();
    glFinish();

    const void* slot = nullptr;
    std::size_t n = 0;
    ASSERT_TRUE(ring.consume(&slot, &n));
    std::uint32_t got_count = 0;
    std::memcpy(&got_count, slot, kCountBytes);
    EXPECT_EQ(got_count, kBlades) << "count must reconstruct from slot+0";
    const std::uint32_t* got_blades =
        reinterpret_cast<const std::uint32_t*>(static_cast<const char*>(slot) + kCountBytes);
    for (std::uint32_t i = 0; i < kBlades; ++i) {
        EXPECT_EQ(got_blades[i], blades[i]) << "blade " << i << " must match";
    }

    glDeleteBuffers(1, &count_ssbo);
    glDeleteBuffers(1, &blade_ssbo);
}

// Exercises the NON-blocking poll path WITHOUT glFinish, so the
// GL_SYNC_FLUSH_COMMANDS_BIT flush-on-poll (the fix for offscreen renders that
// never SwapBuffers) is actually covered -- the other cases force completion with
// glFinish and would mask a missing flush.
TEST(AsyncReadbackRing, PollProgressesWithoutExplicitFinish) {
    HiddenGlContext ctx;
    if (!ctx.ready()) {
        GTEST_SKIP() << "no GL context: " << ctx.error();
    }
    constexpr std::size_t kN = 64;
    std::vector<std::uint8_t> src(kN, 0x5Au);
    GLuint ssbo = make_source_ssbo(src);

    AsyncReadbackRing ring;
    ASSERT_TRUE(ring.ensure(kN, 3));
    ASSERT_TRUE(ring.begin());
    ring.copy_region(ssbo, 0, 0, kN);
    ring.submit();

    // No glFinish: poll must eventually report ready purely via its own
    // flush-on-wait. Bounded loop so a genuinely stuck fence fails instead of hangs.
    bool ready = false;
    for (int i = 0; i < 10000 && !ready; ++i) {
        ready = ring.poll();
    }
    ASSERT_TRUE(ready) << "poll never completed without glFinish (flush bit missing?)";
    const void* p = nullptr;
    std::size_t n = 0;
    ASSERT_TRUE(ring.consume(&p, &n));
    EXPECT_EQ(static_cast<const std::uint8_t*>(p)[0], 0x5Au);

    glDeleteBuffers(1, &ssbo);
}

TEST(AsyncReadbackRing, EnsureIsIdempotent) {
    HiddenGlContext ctx;
    if (!ctx.ready()) {
        GTEST_SKIP() << "no GL context: " << ctx.error();
    }
    AsyncReadbackRing ring;
    ASSERT_TRUE(ring.ensure(512, 3));
    // Same-or-smaller geometry is a no-op and must keep working.
    EXPECT_TRUE(ring.ensure(256, 3));
    EXPECT_TRUE(ring.ensure(512, 2));
    EXPECT_EQ(ring.depth(), 3);
    EXPECT_GE(ring.slot_bytes(), 512u);
}

//  (/,  ) — the ExposureMeter gate: the SHIPPED
// res/shaders/luminance_reduce.comp kernel, on a constant-luminance input,
// reduces to that exact luminance (the geometric mean of a constant is the
// constant), and the value round-trips through the ring WITHOUT any blocking
// call (bounded zero-timeout polls only — the  contract the metering
// servo relies on).
TEST(AsyncReadbackRing, ExposureMeterKernelReducesConstantSceneNonBlocking) {
    HiddenGlContext ctx;
    if (!ctx.ready()) {
        GTEST_SKIP() << "no GL context: " << ctx.error();
    }

    // Compile the shipped kernel from source root.
    const std::string comp_path =
        std::string(LUMINUMBRA_SOURCE_ROOT) + "/res/shaders/luminance_reduce.comp";
    std::ifstream file(comp_path);
    ASSERT_TRUE(file.is_open()) << "missing " << comp_path;
    std::stringstream source;
    source << file.rdbuf();
    const std::string src_str = source.str();
    const char* src_c = src_str.c_str();
    GLuint shader = glCreateShader(GL_COMPUTE_SHADER);
    glShaderSource(shader, 1, &src_c, nullptr);
    glCompileShader(shader);
    GLint ok = 0;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    char info_log[1024] = {};
    if (!ok)
        glGetShaderInfoLog(shader, sizeof(info_log), nullptr, info_log);
    ASSERT_TRUE(ok) << "luminance_reduce.comp failed to compile:\n" << info_log;
    GLuint program = glCreateProgram();
    glAttachShader(program, shader);
    glLinkProgram(program);
    glGetProgramiv(program, GL_LINK_STATUS, &ok);
    ASSERT_TRUE(ok) << "luminance_reduce.comp failed to link";
    glDeleteShader(shader);

    // A constant mid-grey scene: every sample's luminance == dot(c, Rec709) == c.
    constexpr float kGrey = 0.5f;
    GLuint scene = 0;
    glGenTextures(1, &scene);
    glBindTexture(GL_TEXTURE_2D, scene);
    std::vector<float> texels(static_cast<std::size_t>(64) * 64 * 4, kGrey);
    for (std::size_t i = 3; i < texels.size(); i += 4)
        texels[i] = 1.0f;
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, 64, 64, 0, GL_RGBA, GL_FLOAT, texels.data());
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

    GLuint ssbo = 0;
    glGenBuffers(1, &ssbo);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, ssbo);
    glBufferData(GL_SHADER_STORAGE_BUFFER, sizeof(float), nullptr, GL_DYNAMIC_COPY);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);

    glUseProgram(program);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, scene);
    glUniform1i(glGetUniformLocation(program, "u_scene"), 0);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, ssbo);
    glDispatchCompute(1, 1, 1);
    glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT | GL_BUFFER_UPDATE_BARRIER_BIT);

    AsyncReadbackRing ring;
    ASSERT_TRUE(ring.ensure(sizeof(float), 3));
    ASSERT_TRUE(ring.begin());
    ring.copy_region(ssbo, 0, 0, sizeof(float));
    ring.submit(); // returns immediately ()

    // Bounded NON-blocking poll loop (no glFinish, no client waits).
    bool ready = false;
    for (int i = 0; i < 10000 && !ready; ++i) {
        ready = ring.poll();
    }
    ASSERT_TRUE(ready) << "the metering readback never completed non-blockingly";
    const void* p = nullptr;
    std::size_t n = 0;
    ASSERT_TRUE(ring.consume(&p, &n));
    ASSERT_GE(n, sizeof(float));
    float measured = 0.0f;
    std::memcpy(&measured, p, sizeof(float));
    // Rec.709 luma of constant grey == the grey; geometric mean of a constant ==
    // the constant. RGBA16F storage + log/exp round-trip: allow a small epsilon.
    EXPECT_NEAR(measured, kGrey, 0.005f)
        << "the reduce kernel drifted from the geometric-mean-luminance contract";

    glDeleteBuffers(1, &ssbo);
    glDeleteTextures(1, &scene);
    glDeleteProgram(program);
}

} // namespace
