#include "luminumbra_client/rendering/Camera.h"
#include "luminumbra_client/rendering/passes/PassGlHelpers.h"

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>
#include <glad/glad.h>
#include <gtest/gtest.h>

#include <array>
#include <cmath>
#include <utility>

namespace {
using namespace Luminumbra::Rendering;

class ReversedZDepth : public testing::Test {
protected:
    void SetUp() override {
        ASSERT_TRUE(glfwInit());
        glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
        glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 4);
        glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 5);
        glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
        glfwWindowHint(GLFW_OPENGL_DEBUG_CONTEXT, GLFW_TRUE);
        window = glfwCreateWindow(32, 32, "reversed depth", nullptr, nullptr);
        ASSERT_NE(window, nullptr) << "OpenGL 4.5 is required; this test must not skip";
        glfwMakeContextCurrent(window);
        ASSERT_TRUE(gladLoadGLLoader(reinterpret_cast<GLADloadproc>(glfwGetProcAddress)));
        glEnable(GL_DEBUG_OUTPUT);
        glEnable(GL_DEBUG_OUTPUT_SYNCHRONOUS);
        glDebugMessageCallback(
            [](GLenum, GLenum type, GLuint, GLenum, GLsizei, const GLchar* message, const void*) {
                if (type == GL_DEBUG_TYPE_ERROR)
                    ADD_FAILURE() << "GL debug error: " << message;
            },
            nullptr);
        glClipControl(GL_LOWER_LEFT, GL_ZERO_TO_ONE);
        glDepthFunc(GL_GREATER);
        glClearDepth(0.0);
        glEnable(GL_DEPTH_TEST);
        glGenFramebuffers(1, &fbo);
        glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        glGenTextures(2, textures.data());
        glBindTexture(GL_TEXTURE_2D, textures[0]);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 32, 32, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, textures[0], 0);
        glBindTexture(GL_TEXTURE_2D, textures[1]);
        glTexImage2D(GL_TEXTURE_2D,
                     0,
                     GL_DEPTH_COMPONENT32F,
                     32,
                     32,
                     0,
                     GL_DEPTH_COMPONENT,
                     GL_FLOAT,
                     nullptr);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, textures[1], 0);
        ASSERT_EQ(glCheckFramebufferStatus(GL_FRAMEBUFFER), GL_FRAMEBUFFER_COMPLETE);
        glViewport(0, 0, 32, 32);
        glGenVertexArrays(1, &vao);
        glBindVertexArray(vao);
        const char* vertex = R"(#version 450 core
uniform mat4 projection;
uniform float distanceMetres;
void main() {
    const vec2 corners[6] = vec2[6](vec2(-1,-1), vec2(1,-1), vec2(1,1),
                                   vec2(-1,-1), vec2(1,1), vec2(-1,1));
    gl_Position = projection * vec4(corners[gl_VertexID] * distanceMetres,
                                    -distanceMetres, 1);
})";
        const char* fragment = R"(#version 450 core
uniform vec4 color;
layout(location=0) out vec4 result;
void main() { result = color; }
)";
        program = glCreateProgram();
        for (const auto& stage :
             {std::pair{GL_VERTEX_SHADER, vertex}, std::pair{GL_FRAGMENT_SHADER, fragment}}) {
            GLuint shader = glCreateShader(stage.first);
            glShaderSource(shader, 1, &stage.second, nullptr);
            glCompileShader(shader);
            GLint compiled = 0;
            glGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
            ASSERT_EQ(compiled, GL_TRUE);
            glAttachShader(program, shader);
            glDeleteShader(shader);
        }
        glLinkProgram(program);
        GLint linked = 0;
        glGetProgramiv(program, GL_LINK_STATUS, &linked);
        ASSERT_EQ(linked, GL_TRUE);
        glUseProgram(program);
    }

    void TearDown() override {
        if (program) {
            glDeleteProgram(program);
            glDeleteVertexArrays(1, &vao);
            glDeleteTextures(2, textures.data());
            glDeleteFramebuffers(1, &fbo);
            EXPECT_EQ(glGetError(), GL_NO_ERROR);
        }
        if (window)
            glfwDestroyWindow(window);
        glfwTerminate();
    }

    void projection(float far_plane) {
        const auto matrix = ReversedZPerspective(glm::radians(90.0f), 1.0f, NEAR_PLANE, far_plane);
        glUniformMatrix4fv(glGetUniformLocation(program, "projection"), 1, GL_FALSE, &matrix[0][0]);
    }

    void clear() {
        glClearColor(0, 0, 0, 1);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    }

    void draw(float distance, bool near) {
        glUniform1f(glGetUniformLocation(program, "distanceMetres"), distance);
        glUniform4f(glGetUniformLocation(program, "color"), near ? 1 : 0, near ? 0 : 1, 0, 1);
        glDrawArrays(GL_TRIANGLES, 0, 6);
    }

    void expect_color(unsigned char red, unsigned char green) {
        std::array<unsigned char, 16 * 16 * 4> pixels{};
        glReadPixels(8, 8, 16, 16, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
        int matching = 0;
        for (std::size_t i = 0; i < pixels.size(); i += 4)
            matching += pixels[i] == red && pixels[i + 1] == green && pixels[i + 2] == 0;
        EXPECT_EQ(matching, 256) << "matching pixels in the interior of the quads";
    }

    float depth() {
        float result = 0;
        glReadPixels(16, 16, 1, 1, GL_DEPTH_COMPONENT, GL_FLOAT, &result);
        return result;
    }

    void ordering(float distance, float separation, float far_plane) {
        projection(far_plane);
        clear();
        draw(distance + separation, false);
        expect_color(0, 255); // Both quads must really rasterize, including at 16 km.
        const float farther_depth = depth();
        ASSERT_GT(farther_depth, 0);
        clear();
        draw(distance, true);
        expect_color(255, 0);
        const float nearer_depth = depth();
        EXPECT_GT(nearer_depth, farther_depth) << "D32F must preserve the separation";
        for (bool near_first : {false, true}) {
            SCOPED_TRACE(near_first);
            clear();
            draw(near_first ? distance : distance + separation, near_first);
            draw(near_first ? distance + separation : distance, !near_first);
            expect_color(255, 0);
            EXPECT_FLOAT_EQ(depth(), nearer_depth);
        }
    }

    GLFWwindow* window = nullptr;
    GLuint fbo = 0, vao = 0, program = 0;
    std::array<GLuint, 2> textures{};
};

TEST_F(ReversedZDepth, OrdersQuadsAt3000Metres) {
    ordering(3000.0f, 0.125f, FAR_PLANE);
}

TEST_F(ReversedZDepth, OrdersQuadsAt16000Metres) {
    // Future-distance qualification only. The production camera stays at 3200 m.
    ordering(16000.0f, 0.125f, 17408.0f);
}

TEST_F(ReversedZDepth, OrdersQuadsAt30Metres) {
    ordering(30.0f, 0.01f, FAR_PLANE);
}

TEST_F(ReversedZDepth, RuntimeClipPlanesRemainPointOneAnd3200Metres) {
    EXPECT_FLOAT_EQ(NEAR_PLANE, 0.1f);
    EXPECT_FLOAT_EQ(FAR_PLANE, 3200.0f);
    projection(FAR_PLANE);
    for (float distance : {0.05f, 3201.0f, 16000.0f}) {
        SCOPED_TRACE(distance);
        clear();
        draw(distance, true);
        expect_color(0, 0);
        EXPECT_FLOAT_EQ(depth(), 0);
    }
}

TEST(ReversedZProjection, FrustumPlanesMatchZeroToOneClipBounds) {
    Camera camera(glm::vec3(90, 20, -50));
    const glm::mat4 vp = camera.GetProjectionMatrix(1200, 800) * camera.GetViewMatrix();
    glm::vec4 planes[6];
    PassGl::ExtractFrustumPlanes(vp, planes);
    for (const glm::vec3 view_point : {glm::vec3(0, 0, -0.05f),
                                       glm::vec3(0, 0, -0.11f),
                                       glm::vec3(0, 0, -3199),
                                       glm::vec3(0, 0, -3201),
                                       glm::vec3(100, 0, -10),
                                       glm::vec3(0, 100, -10),
                                       glm::vec3(-100, 0, -10),
                                       glm::vec3(0, -100, -10),
                                       glm::vec3(0, 0, 10)}) {
        const auto world = glm::inverse(camera.GetViewMatrix()) * glm::vec4(view_point, 1);
        const auto clip = vp * world;
        const bool inside_clip = std::abs(clip.x) <= clip.w && std::abs(clip.y) <= clip.w &&
                                 clip.z >= 0 && clip.z <= clip.w;
        bool inside_planes = true;
        for (const auto& plane : planes)
            inside_planes &= glm::dot(plane, world) >= 0;
        EXPECT_EQ(inside_planes, inside_clip) << "view z = " << view_point.z;
    }
}

TEST(ReversedZProjection, CascadeCornersUseReversedClipEndpoints) {
    for (const auto& range : {std::pair{0.1f, 15.0f},
                              std::pair{15.0f, 40.0f},
                              std::pair{40.0f, 100.0f},
                              std::pair{100.0f, 250.0f}}) {
        const auto inverse = glm::inverse(
            ReversedZPerspective(glm::radians(45.0f), 1.5f, range.first, range.second));
        for (int z = 0; z < 2; ++z)
            for (int y = 0; y < 2; ++y)
                for (int x = 0; x < 2; ++x) {
                    const auto corner = inverse * glm::vec4(2 * x - 1, 2 * y - 1, 1 - z, 1);
                    EXPECT_NEAR(-corner.z / corner.w, z ? range.second : range.first, 0.0001f);
                }
    }
}
} // namespace
