//  /  (  — the rendering pilot-gate ownership
// leg): the registry's OWNED-resource contract against a REAL hidden GL
// context (HiddenGlContext, mirroring async_readback_ring_test):
//   * owned entries SURVIVE frame boundaries (clear_adopted never touches
//     them) and win name lookups over adopted entries;
//   * resize respecifies storage per desc, keeps the GL name, re-attaches and
//     re-verifies every owned FBO referencing the texture, and the target
//     remains functionally renderable (clear + readback);
//   * destroy releases exactly the owned objects and forgets the names.
#include "luminumbra_client/rendering/RenderResourceRegistry.h"

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>
#include <glad/glad.h>
#include <gtest/gtest.h>

#include <array>
#include <string>

namespace {

using Luminumbra::Rendering::FboAttachment;
using Luminumbra::Rendering::FboDesc;
using Luminumbra::Rendering::RenderbufferDesc;
using Luminumbra::Rendering::RenderResourceRegistry;
using Luminumbra::Rendering::ResourceLifetime;
using Luminumbra::Rendering::TextureDesc;

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
        m_window = glfwCreateWindow(64, 64, "registry_ownership_test", nullptr, nullptr);
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

TextureDesc ColorTargetDesc(Luminumbra::u32 w, Luminumbra::u32 h) {
    TextureDesc desc;
    desc.width = w;
    desc.height = h;
    desc.internal_format = GL_RGBA8;
    desc.format = GL_RGBA;
    desc.type = GL_UNSIGNED_BYTE;
    desc.min_filter = GL_NEAREST;
    desc.mag_filter = GL_NEAREST;
    desc.wrap_s = GL_CLAMP_TO_EDGE;
    desc.wrap_t = GL_CLAMP_TO_EDGE;
    desc.lifetime = ResourceLifetime::Persistent;
    desc.debug_label = "registry_ownership_test.color";
    return desc;
}

TEST(RegistryOwnership, OwnedEntriesSurviveFrameBoundariesAndWinLookups) {
    HiddenGlContext gl;
    if (!gl.ready())
        GTEST_SKIP() << gl.error();

    RenderResourceRegistry registry;
    const auto tex = registry.create_texture("owned_color", ColorTargetDesc(32, 32));
    ASSERT_TRUE(static_cast<bool>(tex));

    FboDesc fbo_desc;
    fbo_desc.attachments.push_back(FboAttachment{GL_COLOR_ATTACHMENT0, "owned_color"});
    fbo_desc.draw_buffers = {GL_COLOR_ATTACHMENT0};
    fbo_desc.debug_label = "registry_ownership_test.fbo";
    const auto fbo = registry.create_fbo("owned_fbo", fbo_desc);
    ASSERT_TRUE(static_cast<bool>(fbo));

    // Frame boundaries: adopted entries churn, owned entries persist.
    for (int frame = 0; frame < 3; ++frame) {
        registry.adopt_texture("per_frame_thing", 12345u + static_cast<Luminumbra::u32>(frame));
        registry.clear_adopted();
        EXPECT_EQ(registry.texture("owned_color").id, tex.id)
            << "owned texture lost across frame boundary " << frame;
        EXPECT_EQ(registry.fbo("owned_fbo").id, fbo.id)
            << "owned FBO lost across frame boundary " << frame;
    }

    // Owned wins over a same-named adopt (migration safety: a stale re-adopt
    // cannot shadow the registry-owned target).
    registry.adopt_texture("owned_color", 999999u);
    EXPECT_EQ(registry.texture("owned_color").id, tex.id);

    registry.destroy_all_owned();
}

TEST(RegistryOwnership, ResizeRespecifiesReattachesAndStaysRenderable) {
    HiddenGlContext gl;
    if (!gl.ready())
        GTEST_SKIP() << gl.error();

    RenderResourceRegistry registry;
    const auto tex = registry.create_texture("resize_color", ColorTargetDesc(16, 16));
    ASSERT_TRUE(static_cast<bool>(tex));
    FboDesc fbo_desc;
    fbo_desc.attachments.push_back(FboAttachment{GL_COLOR_ATTACHMENT0, "resize_color"});
    fbo_desc.draw_buffers = {GL_COLOR_ATTACHMENT0};
    const auto fbo = registry.create_fbo("resize_fbo", fbo_desc);
    ASSERT_TRUE(static_cast<bool>(fbo));

    ASSERT_TRUE(registry.resize_texture("resize_color", 48, 48));
    // Same GL name (in-place respecify), desc updated.
    EXPECT_EQ(registry.texture("resize_color").id, tex.id);
    ASSERT_NE(registry.owned_texture_desc("resize_color"), nullptr);
    EXPECT_EQ(registry.owned_texture_desc("resize_color")->width, 48u);

    // Functional: clear through the owned FBO at the new size and read back.
    glBindFramebuffer(GL_FRAMEBUFFER, fbo.id);
    ASSERT_EQ(glCheckFramebufferStatus(GL_FRAMEBUFFER),
              static_cast<GLenum>(GL_FRAMEBUFFER_COMPLETE));
    glViewport(0, 0, 48, 48);
    glClearColor(0.0f, 1.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    std::array<unsigned char, 4> pixel{0, 0, 0, 0};
    glReadPixels(40, 40, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel.data());
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    EXPECT_EQ(pixel[1], 255u) << "resized owned target did not take a clear";
    EXPECT_EQ(glGetError(), static_cast<GLenum>(GL_NO_ERROR));

    registry.destroy_all_owned();
}

TEST(RegistryOwnership, DestroyReleasesAndForgets) {
    HiddenGlContext gl;
    if (!gl.ready())
        GTEST_SKIP() << gl.error();

    RenderResourceRegistry registry;
    const auto tex = registry.create_texture("doomed", ColorTargetDesc(8, 8));
    ASSERT_TRUE(static_cast<bool>(tex));
    EXPECT_TRUE(registry.owns_texture("doomed"));

    registry.destroy_owned("doomed");
    EXPECT_FALSE(registry.owns_texture("doomed"));
    EXPECT_FALSE(static_cast<bool>(registry.texture("doomed")));
    // Duplicate-create is now legal again under the freed name.
    EXPECT_TRUE(static_cast<bool>(registry.create_texture("doomed", ColorTargetDesc(8, 8))));
    registry.destroy_all_owned();
}

// the lighting family needs a renderbuffer-backed depth
// attachment. This pins that path: an owned color texture + owned depth
// renderbuffer share an owned FBO that is complete and depth-tests correctly,
// and destroy releases the renderbuffer and forgets its name.
TEST(RegistryOwnership, RenderbufferDepthAttachmentIsCompleteAndReleases) {
    HiddenGlContext gl;
    if (!gl.ready())
        GTEST_SKIP() << gl.error();

    RenderResourceRegistry registry;
    const auto color = registry.create_texture("rb_color", ColorTargetDesc(32, 32));
    ASSERT_TRUE(static_cast<bool>(color));

    RenderbufferDesc depth_desc;
    depth_desc.width = 32;
    depth_desc.height = 32;
    depth_desc.internal_format = GL_DEPTH_COMPONENT24;
    depth_desc.debug_label = "registry_ownership_test.depth_rb";
    const auto depth = registry.create_renderbuffer("rb_depth", depth_desc);
    ASSERT_TRUE(static_cast<bool>(depth));
    EXPECT_TRUE(registry.owns_renderbuffer("rb_depth"));

    FboDesc fbo_desc;
    fbo_desc.attachments.push_back(FboAttachment{GL_COLOR_ATTACHMENT0, "rb_color"});
    fbo_desc.attachments.push_back(FboAttachment{GL_DEPTH_ATTACHMENT, "rb_depth"});
    fbo_desc.draw_buffers = {GL_COLOR_ATTACHMENT0};
    fbo_desc.debug_label = "registry_ownership_test.rb_fbo";
    const auto fbo = registry.create_fbo("rb_fbo", fbo_desc);
    ASSERT_TRUE(static_cast<bool>(fbo))
        << "FBO with an owned renderbuffer depth attachment must be complete";

    // Functional: depth-test through the owned FBO. A near quad clears green;
    // a farther clear must NOT overwrite it once depth-test is on.
    glBindFramebuffer(GL_FRAMEBUFFER, fbo.id);
    ASSERT_EQ(glCheckFramebufferStatus(GL_FRAMEBUFFER),
              static_cast<GLenum>(GL_FRAMEBUFFER_COMPLETE));
    glViewport(0, 0, 32, 32);
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LESS);
    glClearDepth(1.0);
    glClearColor(0.0f, 1.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    std::array<unsigned char, 4> pixel{0, 0, 0, 0};
    glReadPixels(16, 16, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, pixel.data());
    glDisable(GL_DEPTH_TEST);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    EXPECT_EQ(pixel[1], 255u) << "depth-attached owned FBO did not take a clear";
    EXPECT_EQ(glGetError(), static_cast<GLenum>(GL_NO_ERROR));

    registry.destroy_owned("rb_depth");
    EXPECT_FALSE(registry.owns_renderbuffer("rb_depth"));
    registry.destroy_all_owned();
}

} // namespace
