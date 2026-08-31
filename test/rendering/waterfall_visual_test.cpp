// Waterfall visual validation.
//
// Two assertions, both required by documented design / regression review:
//   (1) DETERMINISM: waterfall SITE DETECTION is a pure function of the
//       generated world. We build the shipped mountains world (rivers enabled)
//       at the fixed atlas seed, run DetectWaterfalls TWICE, and assert the
//       site vectors are byte-identical and the order-preserving site hash is
//       equal. The per-world cache must return the same set. Two worlds built
//       from the same seed produce the same key + the same sites. Render-only:
//       nothing here touches world_hash (it stays d950a6afc12a5cdc).
//   (2) VISUAL: at a detected site we render the falling SHEET (waterfall.frag)
//       + a SPRAY/mist billboard cloud + plunge-pool FOAM into an offscreen FBO
//       and assert the capture shows all three (a bright cascade body, a mist
//       plume above the pool, and a white foam band at the foot).
//
// The capture is GL; if no GL context is available the visual half SKIPs but
// the determinism half (the gate's load-bearing  assertion) still runs +
// writes the artifact, so the validator always has its determinism evidence.

#include "gtest/gtest.h"

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>
#include <glad/glad.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>

#include "rendering/WaterfallDetect.h"
#include "systems/SHIELD_WorldSystem.h"
#include "world/TerrainPresetLoader.h"

namespace fs = std::filesystem;

using namespace Luminumbra;
using namespace Luminumbra::Systems;
using Luminumbra::Rendering::DetectWaterfalls;
using Luminumbra::Rendering::HashWaterfallSites;
using Luminumbra::Rendering::MakeWaterfallDetectKey;
using Luminumbra::Rendering::WaterfallDetectParams;
using Luminumbra::Rendering::WaterfallSite;
using Luminumbra::Rendering::WaterfallSiteCache;

namespace {

#ifndef LUMINUMBRA_SOURCE_ROOT
#define LUMINUMBRA_SOURCE_ROOT "."
#endif
#ifndef LUMINUMBRA_TEST_ARTIFACT_DIR
#define LUMINUMBRA_TEST_ARTIFACT_DIR "."
#endif

constexpr int kSeed = 424242;
constexpr int kCaptureWidth = 320;
constexpr int kCaptureHeight = 320;

fs::path SourceRoot() {
    return fs::weakly_canonical(fs::path(LUMINUMBRA_SOURCE_ROOT));
}

TerrainGenParams LoadPresetParams(const fs::path& path) {
    const Luminumbra::world::TerrainPresetLoadResult result =
        Luminumbra::world::LoadTerrainPreset(path);
    EXPECT_TRUE(result.ok) << path.string();
    return result.params;
}

std::string ReadTextFile(const fs::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file)
        return {};
    std::stringstream s;
    s << file.rdbuf();
    return s.str();
}

// --- Minimal hidden GL context (mirrors render_capture_test). ---
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
        m_window = glfwCreateWindow(64, 64, "waterfall_visual_test", nullptr, nullptr);
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

GLuint CompileShaderSrc(const std::string& src, GLenum type, std::string& log) {
    const char* p = src.c_str();
    GLuint sh = glCreateShader(type);
    glShaderSource(sh, 1, &p, nullptr);
    glCompileShader(sh);
    GLint ok = GL_FALSE;
    glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
    if (ok != GL_TRUE) {
        GLint len = 0;
        glGetShaderiv(sh, GL_INFO_LOG_LENGTH, &len);
        log.assign(static_cast<std::size_t>(std::max(len, 1)), '\0');
        glGetShaderInfoLog(sh, len, nullptr, log.data());
        glDeleteShader(sh);
        return 0;
    }
    return sh;
}

GLuint LinkProgram(const fs::path& vert, const fs::path& frag, std::string& err) {
    const std::string vs = ReadTextFile(vert);
    const std::string fsrc = ReadTextFile(frag);
    if (vs.empty()) {
        err = "missing vert " + vert.string();
        return 0;
    }
    if (fsrc.empty()) {
        err = "missing frag " + frag.string();
        return 0;
    }
    std::string vlog, flog;
    GLuint v = CompileShaderSrc(vs, GL_VERTEX_SHADER, vlog);
    GLuint f = CompileShaderSrc(fsrc, GL_FRAGMENT_SHADER, flog);
    if (!v || !f) {
        err = vlog + flog;
        if (v)
            glDeleteShader(v);
        if (f)
            glDeleteShader(f);
        return 0;
    }
    GLuint prog = glCreateProgram();
    glAttachShader(prog, v);
    glAttachShader(prog, f);
    glLinkProgram(prog);
    glDeleteShader(v);
    glDeleteShader(f);
    GLint ok = GL_FALSE;
    glGetProgramiv(prog, GL_LINK_STATUS, &ok);
    if (ok != GL_TRUE) {
        GLint len = 0;
        glGetProgramiv(prog, GL_INFO_LOG_LENGTH, &len);
        std::string log(static_cast<std::size_t>(std::max(len, 1)), '\0');
        glGetProgramInfoLog(prog, len, nullptr, log.data());
        err = log;
        glDeleteProgram(prog);
        return 0;
    }
    return prog;
}

struct Vert {
    glm::vec3 pos;
    glm::vec3 nrm;
};

// Classifies the captured pixels into the three required signatures:
//  - cascade body: bright-ish blue-white falling water (the sheet),
//  - foam: near-white high-luma pixels (crest froth + plunge band + spray cores),
//  - spray: bright pixels in the UPPER region above the pool (the rising mist).
struct WaterfallPixelStats {
    std::uint64_t total = 0;
    std::uint64_t cascade_pixels = 0; // sheet body (blue-led, mid-bright)
    std::uint64_t foam_pixels = 0;    // near-white froth
    std::uint64_t spray_pixels = 0;   // bright pixels in the mist band (upper third)
    double cascade_ratio = 0.0;
    double foam_ratio = 0.0;
    double spray_ratio = 0.0;
};

WaterfallPixelStats AnalyzeWaterfall(const std::vector<unsigned char>& rgba, int w, int h) {
    WaterfallPixelStats s;
    const int upper_band =
        h / 3; // top third in OpenGL bottom-up == top of frame after flip; we read raw
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const std::size_t i = (static_cast<std::size_t>(y) * w + x) * 4u;
            const int r = rgba[i], g = rgba[i + 1], b = rgba[i + 2];
            const int luma = (r * 54 + g * 183 + b * 19) >> 8;
            // Skip the dark clear background.
            if (r + g + b <= 24)
                continue;
            ++s.total;
            const bool near_white = r > 200 && g > 210 && b > 215;
            const bool bluish = b >= g && g >= r && luma > 70;
            if (near_white) {
                ++s.foam_pixels;
            } else if (bluish && luma > 60) {
                ++s.cascade_pixels;
            }
            // Spray: bright pixels in the UPPER band (raw y near h-1 is top of
            // the frame in GL bottom-up; the mist plume sits above the pool).
            if (y >= h - upper_band && (near_white || (bluish && luma > 90))) {
                ++s.spray_pixels;
            }
        }
    }
    const double denom = static_cast<double>(std::max<std::uint64_t>(s.total, 1));
    s.cascade_ratio = static_cast<double>(s.cascade_pixels) / denom;
    s.foam_ratio = static_cast<double>(s.foam_pixels) / denom;
    s.spray_ratio = static_cast<double>(s.spray_pixels) / denom;
    return s;
}

void WritePpm(const fs::path& path, int w, int h, const std::vector<unsigned char>& rgba) {
    std::ofstream out(path, std::ios::binary);
    if (!out)
        return;
    out << "P6\n" << w << " " << h << "\n255\n";
    for (int y = h - 1; y >= 0; --y) {
        for (int x = 0; x < w; ++x) {
            const std::size_t i = (static_cast<std::size_t>(y) * w + x) * 4u;
            const unsigned char rgb[3] = {rgba[i], rgba[i + 1], rgba[i + 2]};
            out.write(reinterpret_cast<const char*>(rgb), 3);
        }
    }
}

} // namespace

TEST(WaterfallVisualTest, SiteDetectionDeterministicAndDressed) {
    const fs::path preset = SourceRoot() / "worlds/atlas/presets/mountains.json";
    ASSERT_TRUE(fs::exists(preset)) << preset.string();
    const TerrainGenParams params = LoadPresetParams(preset);
    ASSERT_TRUE(params.rivers_enabled) << "mountains must opt into rivers";

    // --- (1) DETERMINISM (regression review). ---
    SHIELD_WorldSystem world_a(nullptr, nullptr, params, kSeed);
    SHIELD_WorldSystem world_b(nullptr, nullptr, params, kSeed);
    const WaterfallDetectParams dp;

    const std::vector<WaterfallSite> sites_a1 = DetectWaterfalls(world_a, dp);
    const std::vector<WaterfallSite> sites_a2 = DetectWaterfalls(world_a, dp);
    const std::vector<WaterfallSite> sites_b1 = DetectWaterfalls(world_b, dp);

    const std::uint64_t hash_a1 = HashWaterfallSites(sites_a1);
    const std::uint64_t hash_a2 = HashWaterfallSites(sites_a2);
    const std::uint64_t hash_b1 = HashWaterfallSites(sites_b1);

    // Byte-identical across repeated calls on the same world.
    ASSERT_EQ(sites_a1.size(), sites_a2.size());
    const bool bytes_equal_run =
        sites_a1.size() == sites_a2.size() &&
        std::memcmp(sites_a1.data(), sites_a2.data(), sites_a1.size() * sizeof(WaterfallSite)) == 0;
    EXPECT_TRUE(bytes_equal_run) << "same world -> sites must be byte-identical";
    EXPECT_EQ(hash_a1, hash_a2);
    // Same seed, separate world -> same sites (the  contract).
    EXPECT_EQ(hash_a1, hash_b1) << "same seed must yield the same sites for every replay";
    EXPECT_EQ(sites_a1.size(), sites_b1.size());

    // The cache keys on (seed, window) and returns the same set.
    WaterfallSiteCache cache;
    const std::vector<WaterfallSite>& cached = cache.sites_for(world_a, dp);
    EXPECT_EQ(HashWaterfallSites(cached), hash_a1);
    // A second query hits the cache (no recompute) and stays identical.
    const std::vector<WaterfallSite>& cached2 = cache.sites_for(world_a, dp);
    EXPECT_EQ(HashWaterfallSites(cached2), hash_a1);
    EXPECT_EQ(cache.cached_world_count(), 1u);
    EXPECT_TRUE(MakeWaterfallDetectKey(world_a, dp) == MakeWaterfallDetectKey(world_b, dp));

    // The mountains preset (rivers through steep relief) must yield falls.
    ASSERT_GT(sites_a1.size(), 0u)
        << "no waterfall sites detected on the mountains preset (rivers carve toward "
           "SEA_LEVEL through 70 m relief; if this fires, the drop/steepness thresholds "
           "in WaterfallDetectParams need retuning for the shipped preset)";

    // Every detected site is a genuine steep drop.
    for (const WaterfallSite& site : sites_a1) {
        EXPECT_GE(site.drop_height, dp.min_drop);
        EXPECT_GE(site.steepness, dp.min_steepness);
        EXPECT_GT(site.crest.y, site.foot.y);
    }

    // Pick the steepest+tallest site for the dressing capture.
    const WaterfallSite* best = &sites_a1.front();
    for (const WaterfallSite& s : sites_a1) {
        if (s.drop_height > best->drop_height)
            best = &s;
    }

    // --- (2) VISUAL dressing capture. ---
    bool capture_written = false;
    WaterfallPixelStats stats;
    std::string capture_relpath;
    std::string gl_skip_reason;
    {
        HiddenGlContext ctx;
        if (!ctx.ready()) {
            gl_skip_reason = ctx.error();
        } else {
            std::string err;
            const fs::path shader_root = SourceRoot() / "res/shaders";
            GLuint sheet_prog =
                LinkProgram(shader_root / "basic.vert", shader_root / "waterfall.frag", err);
            ASSERT_NE(sheet_prog, 0u) << "waterfall.frag failed: " << err;
            // The spray/foam billboards reuse the same sheet shader so the
            // capture needs only one program; the foam quads sit at fall_t~1.
            GLuint fbo = 0, color_tex = 0, depth_rb = 0;
            glGenTextures(1, &color_tex);
            glBindTexture(GL_TEXTURE_2D, color_tex);
            glTexImage2D(GL_TEXTURE_2D,
                         0,
                         GL_RGBA8,
                         kCaptureWidth,
                         kCaptureHeight,
                         0,
                         GL_RGBA,
                         GL_UNSIGNED_BYTE,
                         nullptr);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
            glGenRenderbuffers(1, &depth_rb);
            glBindRenderbuffer(GL_RENDERBUFFER, depth_rb);
            glRenderbufferStorage(
                GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, kCaptureWidth, kCaptureHeight);
            glGenFramebuffers(1, &fbo);
            glBindFramebuffer(GL_FRAMEBUFFER, fbo);
            glFramebufferTexture2D(
                GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, color_tex, 0);
            glFramebufferRenderbuffer(
                GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, depth_rb);
            ASSERT_EQ(glCheckFramebufferStatus(GL_FRAMEBUFFER), GL_FRAMEBUFFER_COMPLETE);

            // Build the dressing in a CLEAN LOCAL frame (curtain in the XY plane,
            // facing +Z) using the REAL detected drop height + channel width, so
            // the framing is robust regardless of the site's world-space flow
            // azimuth. The shader keys its crest/foot foam bands on world Y, so we
            // place the crest at y = drop and the foot at y = 0 and pass those as
            // u_crest_y / u_foot_y. (The live RenderPipeline draws the curtain at
            // the real world site; this capture only needs a frontal view of one.)
            const float drop = std::max(best->drop_height, 4.0f);
            const float width = std::max(best->width, 4.0f);
            const float half_w = width * 0.5f;
            const float crest_y = drop;
            const float foot_y = 0.0f;

            std::vector<Vert> verts;
            std::vector<unsigned int> idx;
            auto add_quad = [&](const glm::vec3& a,
                                const glm::vec3& b,
                                const glm::vec3& c,
                                const glm::vec3& d) {
                const unsigned int base = static_cast<unsigned int>(verts.size());
                const glm::vec3 n(0, 0, 1);
                verts.push_back({a, n});
                verts.push_back({b, n});
                verts.push_back({c, n});
                verts.push_back({d, n});
                idx.insert(idx.end(), {base, base + 1, base + 2, base + 2, base + 3, base});
            };
            // Sheet curtain: top edge at the crest, bottom edge at the foot.
            add_quad(glm::vec3(-half_w, crest_y, 0.0f),
                     glm::vec3(half_w, crest_y, 0.0f),
                     glm::vec3(half_w, foot_y, 0.0f),
                     glm::vec3(-half_w, foot_y, 0.0f));
            // Plunge-pool FOAM band: a short quad straddling the foot (fall_t ~1,
            // where the shader paints plunge foam), pushed slightly toward the
            // camera so it composites over the sheet base.
            add_quad(glm::vec3(-half_w * 1.3f, foot_y + 0.6f, 0.3f),
                     glm::vec3(half_w * 1.3f, foot_y + 0.6f, 0.3f),
                     glm::vec3(half_w * 1.3f, foot_y - 0.8f, 0.3f),
                     glm::vec3(-half_w * 1.3f, foot_y - 0.8f, 0.3f));
            // SPRAY/mist plume: stacked quads rising ABOVE the pool, in front of
            // the sheet, narrowing with height (the upward fan of mist). These sit
            // near fall_t~1 so they read as bright froth.
            for (int m = 0; m < 4; ++m) {
                const float y0 = foot_y + 0.3f + static_cast<float>(m) * 0.9f;
                const float y1 = y0 + 1.1f;
                const float sw = half_w * (1.2f - 0.18f * static_cast<float>(m));
                add_quad(glm::vec3(-sw, y0, 0.6f),
                         glm::vec3(sw, y0, 0.6f),
                         glm::vec3(sw, y1, 0.6f),
                         glm::vec3(-sw, y1, 0.6f));
            }

            GLuint vao = 0, vbo = 0, ebo = 0;
            glGenVertexArrays(1, &vao);
            glGenBuffers(1, &vbo);
            glGenBuffers(1, &ebo);
            glBindVertexArray(vao);
            glBindBuffer(GL_ARRAY_BUFFER, vbo);
            glBufferData(GL_ARRAY_BUFFER,
                         static_cast<GLsizeiptr>(verts.size() * sizeof(Vert)),
                         verts.data(),
                         GL_STATIC_DRAW);
            glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ebo);
            glBufferData(GL_ELEMENT_ARRAY_BUFFER,
                         static_cast<GLsizeiptr>(idx.size() * sizeof(unsigned int)),
                         idx.data(),
                         GL_STATIC_DRAW);
            glEnableVertexAttribArray(0);
            glVertexAttribPointer(0,
                                  3,
                                  GL_FLOAT,
                                  GL_FALSE,
                                  sizeof(Vert),
                                  reinterpret_cast<void*>(offsetof(Vert, pos)));
            glEnableVertexAttribArray(1);
            glVertexAttribPointer(1,
                                  3,
                                  GL_FLOAT,
                                  GL_FALSE,
                                  sizeof(Vert),
                                  reinterpret_cast<void*>(offsetof(Vert, nrm)));

            glViewport(0, 0, kCaptureWidth, kCaptureHeight);
            glDisable(GL_DEPTH_TEST); // single translucent layer; no depth needed
            glDepthMask(GL_FALSE);
            glEnable(GL_BLEND);
            glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
            glDisable(GL_CULL_FACE);
            glClearColor(0.04f, 0.05f, 0.07f, 1.0f);
            glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

            // Frame the curtain: camera straight in front along +Z, looking at
            // the sheet mid-height.
            const glm::vec3 mid(0.0f, 0.5f * (crest_y + foot_y), 0.0f);
            const float dist = std::max(drop, 6.0f) * 1.6f + width;
            const glm::vec3 cam = mid + glm::vec3(0.0f, 0.0f, dist);
            const glm::mat4 model(1.0f);
            const glm::mat4 view = glm::lookAt(cam, mid, glm::vec3(0, 1, 0));
            const glm::mat4 proj =
                glm::perspective(glm::radians(55.0f),
                                 static_cast<float>(kCaptureWidth) / kCaptureHeight,
                                 0.1f,
                                 512.0f);
            const glm::mat3 nmat(1.0f);

            glUseProgram(sheet_prog);
            glUniformMatrix4fv(
                glGetUniformLocation(sheet_prog, "model"), 1, GL_FALSE, glm::value_ptr(model));
            glUniformMatrix4fv(
                glGetUniformLocation(sheet_prog, "view"), 1, GL_FALSE, glm::value_ptr(view));
            glUniformMatrix4fv(
                glGetUniformLocation(sheet_prog, "projection"), 1, GL_FALSE, glm::value_ptr(proj));
            glUniformMatrix3fv(glGetUniformLocation(sheet_prog, "normalMatrix"),
                               1,
                               GL_FALSE,
                               glm::value_ptr(nmat));
            glUniform1f(glGetUniformLocation(sheet_prog, "u_time"), 1.25f);
            glUniform3f(glGetUniformLocation(sheet_prog, "u_camera_pos"), cam.x, cam.y, cam.z);
            glUniform1f(glGetUniformLocation(sheet_prog, "u_crest_y"), crest_y);
            glUniform1f(glGetUniformLocation(sheet_prog, "u_foot_y"), foot_y);
            glUniform3f(glGetUniformLocation(sheet_prog, "u_sun_color"), 1.0f, 0.98f, 0.92f);
            glDrawElements(
                GL_TRIANGLES, static_cast<GLsizei>(idx.size()), GL_UNSIGNED_INT, nullptr);
            glFinish();

            std::vector<unsigned char> pixels(static_cast<std::size_t>(kCaptureWidth) *
                                              static_cast<std::size_t>(kCaptureHeight) * 4u);
            glReadPixels(
                0, 0, kCaptureWidth, kCaptureHeight, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
            stats = AnalyzeWaterfall(pixels, kCaptureWidth, kCaptureHeight);

            const fs::path art_dir =
                fs::path(LUMINUMBRA_TEST_ARTIFACT_DIR) / "render" / "waterfall";
            fs::create_directories(art_dir);
            WritePpm(art_dir / "waterfall-site.ppm", kCaptureWidth, kCaptureHeight, pixels);
            capture_relpath = "render/waterfall/waterfall-site.ppm";
            capture_written = true;

            glDeleteBuffers(1, &ebo);
            glDeleteBuffers(1, &vbo);
            glDeleteVertexArrays(1, &vao);
            glDeleteProgram(sheet_prog);
            glDeleteFramebuffers(1, &fbo);
            glDeleteRenderbuffers(1, &depth_rb);
            glDeleteTextures(1, &color_tex);
        }
    }

    if (capture_written) {
        // The capture must show the sheet body, the froth/foam, and the spray.
        EXPECT_GT(stats.cascade_pixels, 0u) << "no falling-sheet cascade body in the capture";
        EXPECT_GT(stats.foam_pixels, 0u) << "no foam/froth in the capture";
        EXPECT_GT(stats.spray_pixels, 0u) << "no spray/mist plume in the capture";
    }

    // --- Write the WaterfallVisual gate artifact. ---
    const fs::path out_dir = fs::path(LUMINUMBRA_TEST_ARTIFACT_DIR) / "render" / "waterfall";
    fs::create_directories(out_dir);
    std::ofstream out(out_dir / "waterfall-visual.json");
    ASSERT_TRUE(out.is_open());
    const bool determinism_ok = bytes_equal_run && hash_a1 == hash_a2 && hash_a1 == hash_b1;
    const bool dressing_ok = !capture_written || (stats.cascade_pixels > 0 &&
                                                  stats.foam_pixels > 0 && stats.spray_pixels > 0);
    out << "{\n";
    out << "  \"schema\": \"luminumbra.waterfall_visual.v1\",\n";
    out << "  \"preset\": \"mountains\",\n";
    out << "  \"seed\": " << kSeed << ",\n";
    out << "  \"site_count\": " << sites_a1.size() << ",\n";
    out << "  \"site_hash_run_a\": " << hash_a1 << ",\n";
    out << "  \"site_hash_run_a2\": " << hash_a2 << ",\n";
    out << "  \"site_hash_world_b\": " << hash_b1 << ",\n";
    out << "  \"determinism_byte_equal\": " << (bytes_equal_run ? "true" : "false") << ",\n";
    out << "  \"determinism_same_seed_same_sites\": " << (hash_a1 == hash_b1 ? "true" : "false")
        << ",\n";
    out << "  \"best_drop_height\": " << (best ? best->drop_height : 0.0f) << ",\n";
    out << "  \"best_steepness\": " << (best ? best->steepness : 0.0f) << ",\n";
    out << "  \"best_crest\": [" << best->crest.x << ", " << best->crest.y << ", " << best->crest.z
        << "],\n";
    out << "  \"best_foot\": [" << best->foot.x << ", " << best->foot.y << ", " << best->foot.z
        << "],\n";
    out << "  \"capture_written\": " << (capture_written ? "true" : "false") << ",\n";
    out << "  \"capture\": \"" << capture_relpath << "\",\n";
    out << "  \"gl_skip_reason\": \"" << gl_skip_reason << "\",\n";
    out << "  \"cascade_pixels\": " << stats.cascade_pixels << ",\n";
    out << "  \"foam_pixels\": " << stats.foam_pixels << ",\n";
    out << "  \"spray_pixels\": " << stats.spray_pixels << ",\n";
    out << "  \"sheet_present\": " << (stats.cascade_pixels > 0 ? "true" : "false") << ",\n";
    out << "  \"spray_present\": " << (stats.spray_pixels > 0 ? "true" : "false") << ",\n";
    out << "  \"foam_present\": " << (stats.foam_pixels > 0 ? "true" : "false") << ",\n";
    out << "  \"passed\": "
        << (determinism_ok && dressing_ok && sites_a1.size() > 0 ? "true" : "false") << "\n";
    out << "}\n";
    out.close();

    std::cout << "[ WATERFALL ] sites=" << sites_a1.size() << " hash_a=" << hash_a1
              << " hash_b=" << hash_b1 << " best_drop=" << (best ? best->drop_height : 0.0f)
              << " cascade=" << stats.cascade_pixels << " foam=" << stats.foam_pixels
              << " spray=" << stats.spray_pixels
              << (capture_written ? "" : (" [GL skipped: " + gl_skip_reason + "]")) << "\n";
}

// ============================================================================
//  ( T.1): LIVE-WATER waterfall response.
// A LIVE session (real GameSession + streamed water grids): find live standing
// water at a gridded crest, assert the live factor reads WET; then dig the
// crest bed 8 m down (diverting the flow - the water epoch advances, the site
// cache re-keys) and assert the factor EXTINGUISHES. The whole scenario runs
// TWICE and must produce identical readings (run==replay for the live-water
// render response). CPU-only: no GL needed.
// ============================================================================
#include "luminumbra_common/core/JobSystem.h"
#include "luminumbra_common/world/GameSession.h"

namespace {

struct LiveWaterRun {
    bool found = false;
    float factor_wet = -1.0f;
    float factor_dammed = -1.0f;
    std::uint64_t epoch_before = 0;
    std::uint64_t epoch_after = 0;
    Luminumbra::Rendering::WaterfallDetectKey key_before;
    Luminumbra::Rendering::WaterfallDetectKey key_after;
};

LiveWaterRun RunLiveWaterScenario(const std::string& root) {
    LiveWaterRun r;
    Luminumbra::JobSystem jobs;
    jobs.startup();
    {
        Luminumbra::world::GameSession session;
        session.SetJobSystem(&jobs);
        session.SetRootPath(root);
        EXPECT_TRUE(session.CreateWorld("WfLive", "777", "default"));
        SHIELD_WorldSystem* world = session.GetWorldSystem();
        auto* physics = session.GetPhysicsSystem();
        const Vec3 spawn = session.GetMetadata().spawnPoint;
        const Vec3 anchor(spawn.x, world->GetTerrainHeightAt(spawn.x, spawn.z) + 2.0f, spawn.z);
        auto tick = [&] {
            world->update(session.GetRegistry(), {anchor}, physics);
            world->wait_for_streaming_jobs();
        };
        for (int t = 0; t < 24; ++t)
            tick();

        // Find LIVE standing water on a gridded chunk (the y=0 column probe).
        float cx = spawn.x, cz = spawn.z;
        for (int gz = -12; gz <= 12 && !r.found; ++gz) {
            for (int gx = -12; gx <= 12 && !r.found; ++gx) {
                const float px = spawn.x + static_cast<float>(gx) * 16.0f;
                const float pz = spawn.z + static_cast<float>(gz) * 16.0f;
                if (!world->debug_water_grid_at(px, pz))
                    continue;
                if (world->live_water_surface_at(px, pz) >
                    world->GetTerrainHeightAt(px, pz) + 0.10f) {
                    cx = px;
                    cz = pz;
                    r.found = true;
                }
            }
        }
        if (r.found) {
            WaterfallSite site;
            site.crest = glm::vec3(cx, world->GetTerrainHeightAt(cx, cz), cz);
            site.foot = site.crest - glm::vec3(0.0f, 4.0f, 0.0f);
            site.drop_height = 4.0f;

            r.factor_wet = Luminumbra::Rendering::LiveWaterFactorAt(*world, site);
            r.epoch_before = world->water_epoch();
            r.key_before = MakeWaterfallDetectKey(*world, WaterfallDetectParams{});

            // DIG the crest bed 8 m down: the surface (bed+depth) falls far below
            // the voxel terrain -> the fall starves. The epoch must advance.
            world->EditTerrainBed(Vec3(cx, 0.0f, cz), -8000, 6.0f);
            for (int t = 0; t < 8; ++t)
                tick();

            r.factor_dammed = Luminumbra::Rendering::LiveWaterFactorAt(*world, site);
            r.epoch_after = world->water_epoch();
            r.key_after = MakeWaterfallDetectKey(*world, WaterfallDetectParams{});
        }
    }
    jobs.shutdown();
    return r;
}

} // namespace

TEST(WaterfallLiveWater, DammingUpstreamExtinguishesSiteDeterministically) {
    // Temp root (the WaterDeterminism HeadlessRoot pattern): the session writes
    // its throwaway world saves OUTSIDE the repo.
    const fs::path tmp = fs::temp_directory_path() / "luminumbra_waterfall_live_test";
    fs::remove_all(tmp);
    fs::create_directories(tmp / "worlds" / "atlas" / "presets");
    fs::create_directories(tmp / "data" / "common");
    fs::copy_file(SourceRoot() / "worlds" / "atlas" / "presets" / "default.json",
                  tmp / "worlds" / "atlas" / "presets" / "default.json");
    std::error_code ec;
    fs::copy_file(SourceRoot() / "data" / "common" / "biomes.json",
                  tmp / "data" / "common" / "biomes.json",
                  ec);
    const std::string root =
        tmp.string() + std::string(1, static_cast<char>(fs::path::preferred_separator));
    const LiveWaterRun a = RunLiveWaterScenario(root);
    const LiveWaterRun b = RunLiveWaterScenario(root);
    ASSERT_TRUE(a.found)
        << "no LIVE standing water on a gridded chunk within 192 m of spawn (vacuous)";
    EXPECT_GT(a.factor_wet, 0.5f) << "live factor should read WET at live standing water (got "
                                  << a.factor_wet << ")";
    EXPECT_LT(a.factor_dammed, 0.02f)
        << "digging the crest away did not extinguish the fall (got " << a.factor_dammed << ")";
    EXPECT_GT(a.epoch_after, a.epoch_before)
        << "the terraform bed edit did not advance the water epoch";
    EXPECT_FALSE(a.key_after == a.key_before)
        << "the site-cache key did not re-key on the epoch change";
    // run==replay for the whole live-water response.
    EXPECT_EQ(a.factor_wet, b.factor_wet);
    EXPECT_EQ(a.factor_dammed, b.factor_dammed);
    EXPECT_EQ(a.epoch_after, b.epoch_after);
}
