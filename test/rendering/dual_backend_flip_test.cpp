//  ( ;   / ): the in-process dual-backend
// FLIP parity harness.
//
// The offline path (tools/flip_diff.py) is file-based and run-to-run noisy
// (~0.057 on the same tree), so it cannot gate a per-pass backend port. This
// harness renders a deterministic pass into an in-memory framebuffer and diffs
// two framebuffers produced in ONE process / GL context with the exact same
// perceptual metric (rendering/InProcessFlip.h, a port of flip_diff backend_luma).
//
// What  delivers and proves here:
//   1. ZeroNoiseFloorIsBitIdentical -- the same pass rendered twice in one
//      process is bitwise identical -> FLIP score exactly 0.0, K times, zero
//      within-process variance. Cross-run reproducibility follows from
//      within-process bit-determinism (we do not spawn processes to "observe" it).
//   2. DiscriminationTracksPerturbationMagnitude -- the ACCEPTANCE content: the
//      metric is monotone in a known perturbation magnitude and strictly > 0 for
//      any non-zero difference, and the full render->readback->FLIP pipeline
//      detects a genuine rendered (objectColor) difference. Without this, the
//      "score < per-pass threshold" half of the proving signal would be
//      "0 < anything" -- untested. The recorded magnitude->score curve is what
//      gives  an empirical, non-invented threshold basis.
// Backend parity itself runs in the Diligent-linked rhi_pilot_flip_test target,
// reusing ComputeLumaFlip and the calibration artifact emitted here.
//
// The pass is `basic.vert`/`basic.frag` -- deliberately temporally deterministic
// (no u_time, no ping-pong history, no random), so "render twice" means "same
// inputs" and the zero floor is real. Skybox/particles/TAAU would be non-zero by
// design and must never be used for this calibration.

#include "gtest/gtest.h"

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>
#include <glad/glad.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>

#include "rendering/InProcessFlip.h"

namespace fs = std::filesystem;

namespace {

#ifndef LUMINUMBRA_SOURCE_ROOT
#define LUMINUMBRA_SOURCE_ROOT "."
#endif

#ifndef LUMINUMBRA_TEST_ARTIFACT_DIR
#define LUMINUMBRA_TEST_ARTIFACT_DIR "."
#endif

constexpr int kWidth = 256;
constexpr int kHeight = 256;

// The offline flip_diff luma backend's measured same-tree cross-run floor -- the
// noise the in-process harness exists to eliminate. Recorded for contrast only.
constexpr double kOfflineLumaCrossRunFloorRef = 0.057;

struct MeshVertex {
    float px, py, pz;
    float nx, ny, nz;
};

struct RenderParams {
    glm::vec3 object_color{0.44f, 0.72f, 0.38f};
    glm::vec3 camera_position{3.2f, 2.6f, 4.4f};
    glm::vec3 camera_target{0.0f, 0.0f, 0.0f};
    glm::vec3 light_pos{6.0f, 8.0f, 5.0f};
};

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
        m_window = glfwCreateWindow(64, 64, "dual_backend_flip_test", nullptr, nullptr);
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

fs::path SourceRoot() {
    return fs::weakly_canonical(fs::path(LUMINUMBRA_SOURCE_ROOT));
}

fs::path ArtifactRoot() {
    return fs::path(LUMINUMBRA_TEST_ARTIFACT_DIR) / "dual_backend_flip";
}

std::string ReadTextFile(const fs::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file)
        return {};
    std::stringstream stream;
    stream << file.rdbuf();
    return stream.str();
}

GLuint CompileShader(const fs::path& path, GLenum type) {
    const std::string source = ReadTextFile(path);
    if (source.empty()) {
        ADD_FAILURE() << "Shader source missing or empty: " << path.string();
        return 0;
    }
    const char* src = source.c_str();
    GLuint shader = glCreateShader(type);
    glShaderSource(shader, 1, &src, nullptr);
    glCompileShader(shader);
    GLint ok = GL_FALSE;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (ok != GL_TRUE) {
        GLint len = 0;
        glGetShaderiv(shader, GL_INFO_LOG_LENGTH, &len);
        std::string log(static_cast<std::size_t>(std::max(len, 1)), '\0');
        glGetShaderInfoLog(shader, len, nullptr, log.data());
        ADD_FAILURE() << "Shader failed to compile: " << path.string() << "\n" << log;
        glDeleteShader(shader);
        return 0;
    }
    return shader;
}

GLuint LinkBasicProgram() {
    const fs::path shader_root = SourceRoot() / "res/shaders";
    GLuint vs = CompileShader(shader_root / "basic.vert", GL_VERTEX_SHADER);
    GLuint fsh = CompileShader(shader_root / "basic.frag", GL_FRAGMENT_SHADER);
    if (vs == 0 || fsh == 0) {
        if (vs)
            glDeleteShader(vs);
        if (fsh)
            glDeleteShader(fsh);
        return 0;
    }
    GLuint program = glCreateProgram();
    glAttachShader(program, vs);
    glAttachShader(program, fsh);
    glLinkProgram(program);
    glDeleteShader(vs);
    glDeleteShader(fsh);
    GLint ok = GL_FALSE;
    glGetProgramiv(program, GL_LINK_STATUS, &ok);
    if (ok != GL_TRUE) {
        glDeleteProgram(program);
        ADD_FAILURE() << "basic program failed to link";
        return 0;
    }
    return program;
}

// A deterministic unit cube with per-face normals: three faces visible from the
// camera give distinct diffuse levels plus silhouette edges against the clear
// colour, so the metric's luma, gradient, and colour terms are all exercised.
std::vector<MeshVertex> BuildCubeMesh() {
    constexpr float h = 1.2f;
    struct Face {
        glm::vec3 normal;
        std::array<glm::vec3, 4> corners;
    };
    const std::array<Face, 6> faces = {{
        {{1, 0, 0}, {{{h, -h, -h}, {h, h, -h}, {h, h, h}, {h, -h, h}}}},
        {{-1, 0, 0}, {{{-h, -h, h}, {-h, h, h}, {-h, h, -h}, {-h, -h, -h}}}},
        {{0, 1, 0}, {{{-h, h, -h}, {-h, h, h}, {h, h, h}, {h, h, -h}}}},
        {{0, -1, 0}, {{{-h, -h, h}, {-h, -h, -h}, {h, -h, -h}, {h, -h, h}}}},
        {{0, 0, 1}, {{{-h, -h, h}, {h, -h, h}, {h, h, h}, {-h, h, h}}}},
        {{0, 0, -1}, {{{h, -h, -h}, {-h, -h, -h}, {-h, h, -h}, {h, h, -h}}}},
    }};
    std::vector<MeshVertex> verts;
    verts.reserve(faces.size() * 6);
    for (const Face& f : faces) {
        const std::array<int, 6> order = {0, 1, 2, 0, 2, 3};
        for (int idx : order) {
            const glm::vec3& c = f.corners[static_cast<std::size_t>(idx)];
            verts.push_back({c.x, c.y, c.z, f.normal.x, f.normal.y, f.normal.z});
        }
    }
    return verts;
}

// Render the cube with the given params into a 256x256 RGBA8 FBO and read it
// back. glReadPixels from the bound FBO is synchronous (it blocks until the draw
// completes); glFinish is added as belt-and-suspenders. No fence/glClientWaitSync
// is used, so GL_SYNC_FLUSH_COMMANDS_BIT does not apply here.
std::vector<std::uint8_t>
RenderCubeRawGl(GLuint program, const std::vector<MeshVertex>& mesh, const RenderParams& params) {
    GLuint color_tex = 0, depth_rb = 0, fbo = 0, vao = 0, vbo = 0;
    glGenTextures(1, &color_tex);
    glBindTexture(GL_TEXTURE_2D, color_tex);
    glTexImage2D(
        GL_TEXTURE_2D, 0, GL_RGBA8, kWidth, kHeight, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);

    glGenRenderbuffers(1, &depth_rb);
    glBindRenderbuffer(GL_RENDERBUFFER, depth_rb);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, kWidth, kHeight);

    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, color_tex, 0);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, depth_rb);
    EXPECT_EQ(glCheckFramebufferStatus(GL_FRAMEBUFFER), GL_FRAMEBUFFER_COMPLETE);

    glGenVertexArrays(1, &vao);
    glGenBuffers(1, &vbo);
    glBindVertexArray(vao);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER,
                 static_cast<GLsizeiptr>(mesh.size() * sizeof(MeshVertex)),
                 mesh.data(),
                 GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0,
                          3,
                          GL_FLOAT,
                          GL_FALSE,
                          sizeof(MeshVertex),
                          reinterpret_cast<void*>(offsetof(MeshVertex, px)));
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1,
                          3,
                          GL_FLOAT,
                          GL_FALSE,
                          sizeof(MeshVertex),
                          reinterpret_cast<void*>(offsetof(MeshVertex, nx)));

    glViewport(0, 0, kWidth, kHeight);
    glEnable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glClearColor(0.04f, 0.06f, 0.08f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    const glm::mat4 model(1.0f);
    const glm::mat4 view =
        glm::lookAt(params.camera_position, params.camera_target, glm::vec3{0.0f, 1.0f, 0.0f});
    const glm::mat4 projection =
        glm::perspective(glm::radians(45.0f), static_cast<float>(kWidth) / kHeight, 0.1f, 128.0f);
    const glm::mat3 normal_matrix(1.0f);

    glUseProgram(program);
    glUniformMatrix4fv(glGetUniformLocation(program, "model"), 1, GL_FALSE, glm::value_ptr(model));
    glUniformMatrix4fv(glGetUniformLocation(program, "view"), 1, GL_FALSE, glm::value_ptr(view));
    glUniformMatrix4fv(
        glGetUniformLocation(program, "projection"), 1, GL_FALSE, glm::value_ptr(projection));
    glUniformMatrix3fv(
        glGetUniformLocation(program, "normalMatrix"), 1, GL_FALSE, glm::value_ptr(normal_matrix));
    glUniform3f(glGetUniformLocation(program, "lightPos"),
                params.light_pos.x,
                params.light_pos.y,
                params.light_pos.z);
    glUniform3f(glGetUniformLocation(program, "viewPos"),
                params.camera_position.x,
                params.camera_position.y,
                params.camera_position.z);
    glUniform3f(glGetUniformLocation(program, "lightColor"), 1.0f, 1.0f, 1.0f);
    glUniform3f(glGetUniformLocation(program, "objectColor"),
                params.object_color.x,
                params.object_color.y,
                params.object_color.z);
    glDrawArrays(GL_TRIANGLES, 0, static_cast<GLsizei>(mesh.size()));
    glFinish();

    std::vector<std::uint8_t> pixels(static_cast<std::size_t>(kWidth) * kHeight * 4u);
    glReadPixels(0, 0, kWidth, kHeight, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());

    glDeleteBuffers(1, &vbo);
    glDeleteVertexArrays(1, &vao);
    glDeleteFramebuffers(1, &fbo);
    glDeleteRenderbuffers(1, &depth_rb);
    glDeleteTextures(1, &color_tex);
    return pixels;
}

// Additively shift every channel of every pixel by delta (saturating), leaving
// alpha. A uniform shift moves the luma and colour terms predictably with the
// magnitude while the gradient term stays ~flat -- a clean monotone calibration.
std::vector<std::uint8_t> ShiftedCopy(const std::vector<std::uint8_t>& src, int delta) {
    std::vector<std::uint8_t> out = src;
    for (std::size_t i = 0; i + 3 < out.size(); i += 4) {
        for (int c = 0; c < 3; ++c) {
            const int v = static_cast<int>(out[i + static_cast<std::size_t>(c)]) + delta;
            out[i + static_cast<std::size_t>(c)] = static_cast<std::uint8_t>(std::clamp(v, 0, 255));
        }
    }
    return out;
}

class DualBackendFlipInProcessGpu : public ::testing::Test {
protected:
    static HiddenGlContext* s_context;
    static GLuint s_program;

    static void SetUpTestSuite() {
        s_context = new HiddenGlContext();
        if (s_context->ready()) {
            s_program = LinkBasicProgram();
        }
    }
    static void TearDownTestSuite() {
        if (s_program)
            glDeleteProgram(s_program);
        s_program = 0;
        delete s_context;
        s_context = nullptr;
    }
    void SetUp() override {
        if (!s_context || !s_context->ready()) {
            GTEST_SKIP() << (s_context ? s_context->error() : "no GL context");
        }
        if (s_program == 0) {
            GTEST_SKIP() << "basic shader program failed to link";
        }
    }
};

HiddenGlContext* DualBackendFlipInProcessGpu::s_context = nullptr;
GLuint DualBackendFlipInProcessGpu::s_program = 0;

} // namespace

// (1) Zero-noise floor: the same pass rendered twice in one process is bitwise
// identical -> FLIP score exactly 0.0, with zero variance across K repeats. This
// is what makes the in-process harness usable where the offline ~0.057 is not.
TEST_F(DualBackendFlipInProcessGpu, ZeroNoiseFloorIsBitIdentical) {
    const std::vector<MeshVertex> mesh = BuildCubeMesh();
    ASSERT_FALSE(mesh.empty());
    const RenderParams params;

    constexpr int kRepeats = 8;
    std::vector<std::vector<std::uint8_t>> frames;
    frames.reserve(kRepeats);
    for (int i = 0; i < kRepeats; ++i) {
        frames.push_back(RenderCubeRawGl(s_program, mesh, params));
        ASSERT_EQ(frames.back().size(), static_cast<std::size_t>(kWidth) * kHeight * 4u);
    }

    // Sanity: the pass actually drew something (not a uniform clear).
    bool any_foreground = false;
    for (std::size_t i = 0; i + 3 < frames[0].size(); i += 4) {
        if (frames[0][i] != 10 || frames[0][i + 1] != 15 || frames[0][i + 2] != 20) {
            any_foreground = true;
            break;
        }
    }
    EXPECT_TRUE(any_foreground) << "cube pass produced a blank frame -- metric would be vacuous";

    for (int i = 1; i < kRepeats; ++i) {
        EXPECT_EQ(std::memcmp(frames[0].data(),
                              frames[static_cast<std::size_t>(i)].data(),
                              frames[0].size()),
                  0)
            << "raw-GL render " << i << " was not bit-identical to render 0";
        const auto flip = Luminumbra::Rendering::InProcessFlip::ComputeLumaFlip(
            frames[0].data(), frames[static_cast<std::size_t>(i)].data(), kWidth, kHeight);
        EXPECT_EQ(flip.score, 0.0) << "in-process FLIP score " << i << " was non-zero";
        EXPECT_EQ(flip.max_error, 0.0) << "in-process FLIP max_error " << i << " was non-zero";
    }
}

// (2) The acceptance content: the metric is monotone in a known perturbation
// magnitude and strictly positive for any non-zero difference, and the full
// render->readback->FLIP pipeline detects a genuine rendered (objectColor)
// difference. The recorded curve gives  a non-invented threshold basis.
TEST_F(DualBackendFlipInProcessGpu, DiscriminationTracksPerturbationMagnitude) {
    const std::vector<MeshVertex> mesh = BuildCubeMesh();
    ASSERT_FALSE(mesh.empty());
    const RenderParams params;

    const std::vector<std::uint8_t> baseline = RenderCubeRawGl(s_program, mesh, params);

    using Luminumbra::Rendering::InProcessFlip::ComputeLumaFlip;

    // In-memory magnitude sweep: known additive deltas -> monotone increasing score.
    const std::array<int, 4> deltas = {1, 4, 16, 64};
    std::array<double, 4> scores{};
    double prev = 0.0;
    for (std::size_t i = 0; i < deltas.size(); ++i) {
        const std::vector<std::uint8_t> perturbed = ShiftedCopy(baseline, deltas[i]);
        const auto flip = ComputeLumaFlip(baseline.data(), perturbed.data(), kWidth, kHeight);
        scores[i] = flip.score;
        EXPECT_GT(flip.score, 0.0) << "delta " << deltas[i] << " produced a zero score";
        EXPECT_GT(flip.score, prev) << "score not strictly increasing at delta " << deltas[i];
        prev = flip.score;
    }

    // End-to-end: a genuinely different render (objectColor greenish -> reddish)
    // is caught by the full pipeline, well above the zero floor.
    RenderParams recolored = params;
    recolored.object_color = glm::vec3{0.85f, 0.30f, 0.25f};
    const std::vector<std::uint8_t> recolored_frame = RenderCubeRawGl(s_program, mesh, recolored);
    const auto color_flip =
        ComputeLumaFlip(baseline.data(), recolored_frame.data(), kWidth, kHeight);
    EXPECT_GT(color_flip.score, 0.001)
        << "render-level colour change was not detected above the zero floor";

    // Self-diff of the recolored frame is still exactly 0 (determinism holds for
    // any pass, not just the baseline colour).
    const auto self_flip =
        ComputeLumaFlip(recolored_frame.data(), recolored_frame.data(), kWidth, kHeight);
    EXPECT_EQ(self_flip.score, 0.0);

    // Record the calibration so  reads its per-pass threshold basis rather
    // than inventing one.
    fs::create_directories(ArtifactRoot());
    std::ofstream out(ArtifactRoot() / "dual_backend_flip.json");
    ASSERT_TRUE(out);
    out << "{\n";
    out << "  \"schema\": \"luminumbra.dual_backend_flip.v1\",\n";
    out << "  \"metric_backend\": \"" << Luminumbra::Rendering::InProcessFlip::BackendName()
        << "\",\n";
    out << "  \"metric_source\": \"C++ port of tools/flip_diff.py backend_luma\",\n";
    out << "  \"pass\": \"basic\",\n";
    out << "  \"resolution\": [" << kWidth << ", " << kHeight << "],\n";
    out << "  \"zero_floor_score\": 0.0,\n";
    out << "  \"within_process_variance\": 0.0,\n";
    out << "  \"offline_luma_crossrun_floor_ref\": " << kOfflineLumaCrossRunFloorRef << ",\n";
    out << "  \"magnitude_calibration\": [\n";
    for (std::size_t i = 0; i < deltas.size(); ++i) {
        out << "    {\"delta_levels\": " << deltas[i] << ", \"score\": " << scores[i] << "}"
            << (i + 1 == deltas.size() ? "\n" : ",\n");
    }
    out << "  ],\n";
    out << "  \"render_level_color_diff_score\": " << color_flip.score << ",\n";
    out << "  \"second_backend\": \"validated by RhiPilotParityGpu\"\n";
    out << "}\n";
}
