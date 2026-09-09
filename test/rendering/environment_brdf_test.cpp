#include "luminumbra_client/rendering/EnvironmentBrdfLut.gen.h"
#include "luminumbra_client/rendering/RenderResourceRegistry.h"
#include "luminumbra_client/rendering/passes/LightingPass.h"

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>
#include <glad/glad.h>
#include <gtest/gtest.h>

#include <array>

TEST(EnvironmentBrdf, UploadSurvivesFramebufferResizeAndRecreatesAfterCleanup) {
    if (!glfwInit())
        GTEST_SKIP() << "GLFW unavailable";
    struct GlfwLifetime {
        GLFWwindow* window = nullptr;
        ~GlfwLifetime() {
            if (window)
                glfwDestroyWindow(window);
            glfwTerminate();
        }
    } gl;
    glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 4);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 5);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    gl.window = glfwCreateWindow(64, 64, "environment_brdf_test", nullptr, nullptr);
    if (!gl.window)
        GTEST_SKIP() << "OpenGL 4.5 context unavailable";
    glfwMakeContextCurrent(gl.window);
    ASSERT_TRUE(gladLoadGLLoader(reinterpret_cast<GLADloadproc>(glfwGetProcAddress)));

    using namespace Luminumbra::Rendering;
    RenderResourceRegistry registry;
    LightingPass pass;
    for (int lifetime = 0; lifetime < 2; ++lifetime) {
        pass.init_environment_brdf(registry);
        const auto texture = pass.environment_brdf_texture();
        ASSERT_NE(texture, 0u);
        ASSERT_TRUE(registry.owns_texture("environment_brdf"));
        const auto* desc = registry.owned_texture_desc("environment_brdf");
        ASSERT_NE(desc, nullptr);
        EXPECT_EQ(desc->width, 64u);
        EXPECT_EQ(desc->height, 64u);
        EXPECT_EQ(desc->internal_format, static_cast<unsigned>(GL_RG16F));
        EXPECT_EQ(desc->lifetime, ResourceLifetime::Persistent);

        for (const unsigned extent : {32u, 96u}) {
            pass.init_lighting_fbo(registry, extent, extent);
            pass.destroy_lighting_fbo(registry);
            pass.reset_shader(); // shader replacement also leaves the table intact
            registry.clear_adopted();
            ASSERT_EQ(pass.environment_brdf_texture(), texture);
            ASSERT_EQ(glIsTexture(texture), GL_TRUE);
            glBindTexture(GL_TEXTURE_2D, texture);
            std::array<std::uint16_t, kEnvironmentBrdfLut.size()> downloaded{};
            glGetTexImage(GL_TEXTURE_2D, 0, GL_RG, GL_HALF_FLOAT, downloaded.data());
            EXPECT_EQ(downloaded, kEnvironmentBrdfLut);
            GLint filter = 0, wrap = 0;
            glGetTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, &filter);
            glGetTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, &wrap);
            EXPECT_EQ(filter, GL_LINEAR);
            EXPECT_EQ(wrap, GL_CLAMP_TO_EDGE);
            EXPECT_EQ(glGetError(), GL_NO_ERROR);
        }
        pass.destroy_environment_brdf(registry);
        EXPECT_EQ(pass.environment_brdf_texture(), 0u);
        EXPECT_FALSE(registry.owns_texture("environment_brdf"));
        EXPECT_EQ(glIsTexture(texture), GL_FALSE);
    }
    registry.destroy_all_owned();
}
