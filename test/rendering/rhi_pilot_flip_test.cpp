//  + leg B: the RHI pilot's backend-fidelity
// go/no-go. Renders the  calibration cube two ways in ONE process/GL context
// -- raw GL (the golden) and GL-via-Diligent (the candidate) -- and FLIP-compares
// them with rendering/InProcessFlip.h, the SAME metric  calibrated.
//
// Pre-registered threshold, anchored on dual_backend_flip.json's
// delta=1 = 0.0027451, not the ~0.08 offline full-frame
// gate): two backends running math-identical shaders on identical geometry must be
// near-bit-identical, so GO requires score < 0.0027451 (below the smallest calibrated
// sub-perceptual perturbation). Both row orientations are measured and reported: GL
// glReadPixels is bottom-up and Diligent may normalise to top-left, so exposing the
// convention (rather than silently flipping to force a match) is the honest form.
//
// This is leg B only (Diligent's GL backend fidelity). Leg A (Slang shader-port on
// raw GL) and leg C (native-Vulkan render vs raw-GL golden) are separate. Neither
// leg tests Slang-output THROUGH Diligent -- that composition is staged to the mass
// port, not this pilot go/no-go.

#include "gtest/gtest.h"

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>
#include <glad/glad.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>

#include "rendering/InProcessFlip.h"

#include "basic_cube_harness.h"
#include "dual_backend_diligent.h"

namespace fs = std::filesystem;

using luminumbra_test::BuildCubeMesh;
using luminumbra_test::FlipRowsVertically;
using luminumbra_test::kClearB;
using luminumbra_test::kClearG;
using luminumbra_test::kClearR;
using luminumbra_test::kCubeHeight;
using luminumbra_test::kCubeWidth;
using luminumbra_test::MeshVertex;
using luminumbra_test::RenderParams;

namespace {

#ifndef LUMINUMBRA_TEST_ARTIFACT_DIR
#define LUMINUMBRA_TEST_ARTIFACT_DIR "."
#endif

// The pre-registered go thresholds:
//   leg B (GL-via-Diligent, same driver as raw GL): delta=1 = 0.0027451 (near-bit).
//   leg C (native Vulkan, different rasterizer/compiler + NDC): delta=16 = 0.0439216
//     ("no structural/perceptual difference" once conventions are normalised).
constexpr double kGoThreshold = 0.0027451;
constexpr double kGoThresholdVk = 0.0439216;

fs::path ArtifactRoot() {
    return fs::path(LUMINUMBRA_TEST_ARTIFACT_DIR) / "rhi_pilot_flip";
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
        m_window = glfwCreateWindow(64, 64, "rhi_pilot_flip_test", nullptr, nullptr);
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

std::string ReadTextFile(const fs::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file)
        return {};
    std::string s((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    return s;
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

// The raw-GL golden -- byte-identical to dual_backend_flip_test's RenderCubeRawGl (so
// the  calibration applies), lifted here against the shared harness header.
std::vector<std::uint8_t>
RenderCubeRawGl(GLuint program, const std::vector<MeshVertex>& mesh, const RenderParams& params) {
    GLuint color_tex = 0, depth_rb = 0, fbo = 0, vao = 0, vbo = 0;
    glGenTextures(1, &color_tex);
    glBindTexture(GL_TEXTURE_2D, color_tex);
    glTexImage2D(
        GL_TEXTURE_2D, 0, GL_RGBA8, kCubeWidth, kCubeHeight, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);

    glGenRenderbuffers(1, &depth_rb);
    glBindRenderbuffer(GL_RENDERBUFFER, depth_rb);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, kCubeWidth, kCubeHeight);

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

    glViewport(0, 0, kCubeWidth, kCubeHeight);
    glEnable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glClearColor(kClearR, kClearG, kClearB, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    const glm::mat4 model(1.0f);
    const glm::mat4 view =
        glm::lookAt(params.camera_position, params.camera_target, glm::vec3{0.0f, 1.0f, 0.0f});
    const glm::mat4 projection = glm::perspective(
        glm::radians(45.0f), static_cast<float>(kCubeWidth) / kCubeHeight, 0.1f, 128.0f);
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

    std::vector<std::uint8_t> pixels(static_cast<std::size_t>(kCubeWidth) * kCubeHeight * 4u);
    glReadPixels(0, 0, kCubeWidth, kCubeHeight, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());

    glDeleteBuffers(1, &vbo);
    glDeleteVertexArrays(1, &vao);
    glDeleteFramebuffers(1, &fbo);
    glDeleteRenderbuffers(1, &depth_rb);
    glDeleteTextures(1, &color_tex);
    return pixels;
}

GLuint LinkBasicProgram() {
    const fs::path shader_root = fs::path(LUMINUMBRA_SOURCE_ROOT) / "res/shaders";
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

} // namespace

// The pilot go/no-go: raw-GL golden vs GL-via-Diligent candidate, in-process FLIP.
TEST(RhiPilotParityGpu, GlViaDiligentMatchesRawGl) {
    HiddenGlContext ctx;
    if (!ctx.ready()) {
        GTEST_SKIP() << "no headless GL context: " << ctx.error();
    }

    const std::vector<MeshVertex> mesh = BuildCubeMesh();
    ASSERT_FALSE(mesh.empty());
    const RenderParams params;

    // Raw-GL golden FIRST (clean GL state), then the Diligent candidate on the same
    // context. Diligent attaches to the active context and loads its own GL entry
    // points; rendering the golden first avoids any Diligent state leaking into it.
    GLuint program = LinkBasicProgram();
    ASSERT_NE(program, 0u);
    const std::vector<std::uint8_t> golden = RenderCubeRawGl(program, mesh, params);
    glDeleteProgram(program);
    ASSERT_EQ(golden.size(), static_cast<std::size_t>(kCubeWidth) * kCubeHeight * 4u);

    const luminumbra_test::DiligentRenderResult diligent =
        luminumbra_test::RenderCubeDiligentGl(mesh, params);
    ASSERT_TRUE(diligent.available) << "Diligent GL render unavailable: " << diligent.diagnostic;
    ASSERT_EQ(diligent.pixels.size(), golden.size());

    using Luminumbra::Rendering::InProcessFlip::ComputeLumaFlip;
    const auto flip_same =
        ComputeLumaFlip(golden.data(), diligent.pixels.data(), kCubeWidth, kCubeHeight);
    const std::vector<std::uint8_t> diligent_flipped =
        FlipRowsVertically(diligent.pixels, kCubeWidth, kCubeHeight);
    const auto flip_flip =
        ComputeLumaFlip(golden.data(), diligent_flipped.data(), kCubeWidth, kCubeHeight);

    // Whichever orientation is the apples-to-apples one is a fixed convention fact
    // (GL bottom-up vs Diligent top-left), decided by which score is tiny -- NOT a
    // threshold fudge. Report both; the pilot verdict reads the smaller.
    const bool flipped_is_aligned = flip_flip.score < flip_same.score;
    const auto& aligned = flipped_is_aligned ? flip_flip : flip_same;

    std::cout << "[RhiPilotParityGpu] FLIP unflipped score=" << flip_same.score
              << " max=" << flip_same.max_error << " | flipped score=" << flip_flip.score
              << " max=" << flip_flip.max_error
              << " | aligned=" << (flipped_is_aligned ? "flipped" : "unflipped")
              << " | go_threshold=" << kGoThreshold << std::endl;

    // Record the verdict artifact (mirrors dual_backend_flip.json's shape).
    fs::create_directories(ArtifactRoot());
    std::ofstream out(ArtifactRoot() / "rhi_pilot_flip.json");
    if (out) {
        out << "{\n";
        out << "  \"schema\": \"luminumbra.rhi_pilot_flip.v1\",\n";
        out << "  \"leg\": \"B: GL-via-Diligent vs raw-GL\",\n";
        out << "  \"pass\": \"basic\",\n";
        out << "  \"resolution\": [" << kCubeWidth << ", " << kCubeHeight << "],\n";
        out << "  \"metric_backend\": \"" << Luminumbra::Rendering::InProcessFlip::BackendName()
            << "\",\n";
        out << "  \"go_threshold\": " << kGoThreshold << ",\n";
        out << "  \"score_unflipped\": " << flip_same.score << ",\n";
        out << "  \"score_flipped\": " << flip_flip.score << ",\n";
        out << "  \"aligned_orientation\": \"" << (flipped_is_aligned ? "flipped" : "unflipped")
            << "\",\n";
        out << "  \"aligned_score\": " << aligned.score << ",\n";
        out << "  \"aligned_max_error\": " << aligned.max_error << ",\n";
        out << "  \"verdict\": \"" << (aligned.score < kGoThreshold ? "GO" : "NO-GO") << "\"\n";
        out << "}\n";
    }

    EXPECT_LT(aligned.score, kGoThreshold)
        << "GL-via-Diligent is not pixel-faithful to raw-GL at the pre-registered "
           "sub-perceptual threshold (aligned orientation="
        << (flipped_is_aligned ? "flipped" : "unflipped") << "). unflipped=" << flip_same.score
        << " flipped=" << flip_flip.score;
}

// Leg C: native-Vulkan render vs the raw-GL golden. The first render-through-Vulkan
// in the project (VK_LAYER_KHRONOS_validation on). Looser threshold than leg B by
// necessity -- a different rasterizer/compiler and NDC (depth 0..1 + Y-flip), so
// edge/FP differences are expected; a structural difference is not.
TEST(RhiPilotParityGpu, NativeVulkanMatchesRawGl) {
    HiddenGlContext ctx;
    if (!ctx.ready()) {
        GTEST_SKIP() << "no headless GL context (needed for the raw-GL golden): " << ctx.error();
    }

    const std::vector<MeshVertex> mesh = BuildCubeMesh();
    ASSERT_FALSE(mesh.empty());
    const RenderParams params;

    GLuint program = LinkBasicProgram();
    ASSERT_NE(program, 0u);
    const std::vector<std::uint8_t> golden = RenderCubeRawGl(program, mesh, params);
    glDeleteProgram(program);
    ASSERT_EQ(golden.size(), static_cast<std::size_t>(kCubeWidth) * kCubeHeight * 4u);

    const luminumbra_test::DiligentRenderResult vk =
        luminumbra_test::RenderCubeDiligentVk(mesh, params);
    ASSERT_TRUE(vk.available) << "native Vulkan render unavailable: " << vk.diagnostic;
    ASSERT_EQ(vk.pixels.size(), golden.size());

    using Luminumbra::Rendering::InProcessFlip::ComputeLumaFlip;
    const auto flip_same =
        ComputeLumaFlip(golden.data(), vk.pixels.data(), kCubeWidth, kCubeHeight);
    const std::vector<std::uint8_t> vk_flipped =
        FlipRowsVertically(vk.pixels, kCubeWidth, kCubeHeight);
    const auto flip_flip =
        ComputeLumaFlip(golden.data(), vk_flipped.data(), kCubeWidth, kCubeHeight);

    const bool flipped_is_aligned = flip_flip.score < flip_same.score;
    const auto& aligned = flipped_is_aligned ? flip_flip : flip_same;

    std::cout << "[RhiPilotParityGpu-Vk] FLIP unflipped score=" << flip_same.score
              << " max=" << flip_same.max_error << " | flipped score=" << flip_flip.score
              << " max=" << flip_flip.max_error
              << " | aligned=" << (flipped_is_aligned ? "flipped" : "unflipped")
              << " | go_threshold=" << kGoThresholdVk << std::endl;

    fs::create_directories(ArtifactRoot());
    std::ofstream out(ArtifactRoot() / "rhi_pilot_flip_vk.json");
    if (out) {
        out << "{\n";
        out << "  \"schema\": \"luminumbra.rhi_pilot_flip.v1\",\n";
        out << "  \"leg\": \"C: native-Vulkan vs raw-GL\",\n";
        out << "  \"pass\": \"basic\",\n";
        out << "  \"resolution\": [" << kCubeWidth << ", " << kCubeHeight << "],\n";
        out << "  \"metric_backend\": \"" << Luminumbra::Rendering::InProcessFlip::BackendName()
            << "\",\n";
        out << "  \"validation_layers\": \"VK_LAYER_KHRONOS_validation\",\n";
        out << "  \"go_threshold\": " << kGoThresholdVk << ",\n";
        out << "  \"score_unflipped\": " << flip_same.score << ",\n";
        out << "  \"score_flipped\": " << flip_flip.score << ",\n";
        out << "  \"aligned_orientation\": \"" << (flipped_is_aligned ? "flipped" : "unflipped")
            << "\",\n";
        out << "  \"aligned_score\": " << aligned.score << ",\n";
        out << "  \"aligned_max_error\": " << aligned.max_error << ",\n";
        out << "  \"verdict\": \"" << (aligned.score < kGoThresholdVk ? "GO" : "NO-GO") << "\"\n";
        out << "}\n";
    }

    EXPECT_LT(aligned.score, kGoThresholdVk)
        << "native-Vulkan render is not perceptually faithful to raw-GL at the "
           "pre-registered leg-C threshold (aligned orientation="
        << (flipped_is_aligned ? "flipped" : "unflipped") << "). unflipped=" << flip_same.score
        << " flipped=" << flip_flip.score;
}
