#include "../GBufferTargets.h"
#include "../RenderContext.h"
#include "../RenderResourceRegistry.h"
#include "../Shader.h"
#include "../passes/LightingPass.h"
#include "../passes/StaticDrawPass.h"
#include <glad/glad.h>
#include <luminumbra/rendering/StaticRenderer.h>
#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <limits>
#include <stdexcept>
#include <thread>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace Luminumbra::Rendering {
namespace {
std::atomic<bool> active{false};
std::string GlText(GLenum name) {
    const auto* value = glGetString(name);
    if (!value)
        throw std::runtime_error("Missing graphics adapter identity");
    return reinterpret_cast<const char*>(value);
}
std::filesystem::path Utf8Path(const std::string& text) {
    return std::filesystem::path(std::u8string(text.begin(), text.end()));
}
} // namespace
struct StaticRenderer::Impl {
    GLFWwindow* window = nullptr;
    bool initialized = false;
    const std::thread::id owner = std::this_thread::get_id();
    RenderResourceRegistry resources;
    GBuffer gbuffer;
    std::unique_ptr<LightingPass> lighting;
    std::unique_ptr<StaticDrawPass> geometry;
    GLuint quad = 0, quad_buffer = 0;
    std::uint32_t width = 0, height = 0;
    std::uint64_t sequence = 0;
    std::string vendor, renderer, version;
    Impl(const std::string& root, bool software) {
        bool expected = false;
        if (!active.compare_exchange_strong(expected, true))
            throw std::runtime_error(
                "Static inspection currently supports one owning view/context");
        try {
            if (!glfwInit())
                throw std::runtime_error("Static preview could not initialize GLFW");
            initialized = true;
            glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 4);
            glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 5);
            glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
            glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
            glfwWindowHint(GLFW_DOUBLEBUFFER, GLFW_FALSE);
            glfwWindowHint(GLFW_SRGB_CAPABLE, GLFW_FALSE);
            window = glfwCreateWindow(64, 64, "Luminumbra static inspection", nullptr, nullptr);
            if (!window)
                throw std::runtime_error("Static preview requires an OpenGL 4.5 core context");
            glfwMakeContextCurrent(window);
            if (!gladLoadGLLoader(reinterpret_cast<GLADloadproc>(glfwGetProcAddress)))
                throw std::runtime_error("Static preview could not load OpenGL functions");
            vendor = GlText(GL_VENDOR);
            renderer = GlText(GL_RENDERER);
            version = GlText(GL_VERSION);
            if (software && renderer.find("llvmpipe") == std::string::npos)
                throw std::runtime_error(
                    "Software qualification requires the actual llvmpipe renderer");
            glClipControl(GL_LOWER_LEFT, GL_ZERO_TO_ONE);
            glDisable(GL_FRAMEBUFFER_SRGB);
            glDisable(GL_BLEND);
            glDisable(GL_DITHER);
            glPixelStorei(GL_PACK_ALIGNMENT, 1);
            glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
            const float vertices[]{-1, -1, 0, 0, 1, -1, 1, 0, -1, 1, 0, 1, 1, 1, 1, 1};
            glGenVertexArrays(1, &quad);
            glBindVertexArray(quad);
            glGenBuffers(1, &quad_buffer);
            glBindBuffer(GL_ARRAY_BUFFER, quad_buffer);
            glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_STATIC_DRAW);
            glEnableVertexAttribArray(0);
            glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), nullptr);
            glEnableVertexAttribArray(1);
            glVertexAttribPointer(1,
                                  2,
                                  GL_FLOAT,
                                  GL_FALSE,
                                  4 * sizeof(float),
                                  reinterpret_cast<void*>(2 * sizeof(float)));
            lighting = std::make_unique<LightingPass>();
            lighting->init_shader(Utf8Path(root));
            lighting->init_environment_brdf(resources);
            if (!lighting->shader() || !lighting->shader()->IsValid())
                throw std::runtime_error("Production lighting shader failed to compile/link");
            geometry = std::make_unique<StaticDrawPass>(Utf8Path(root));
            if (glGetError() != GL_NO_ERROR)
                throw std::runtime_error("Static preview graphics initialization failed");
        } catch (...) {
            Cleanup();
            throw;
        }
    }
    void Cleanup() noexcept {
        if (window) {
            glfwMakeContextCurrent(window);
            geometry.reset();
            lighting.reset();
            resources.destroy_all_owned();
            if (quad)
                glDeleteVertexArrays(1, &quad);
            if (quad_buffer)
                glDeleteBuffers(1, &quad_buffer);
            glfwDestroyWindow(window);
            window = nullptr;
        }
        if (initialized) {
            glfwTerminate();
            initialized = false;
        }
        active = false;
    }
    ~Impl() {
        Cleanup();
    }
    void Resize(std::uint32_t w, std::uint32_t h) {
        if (w == width && h == height)
            return;
        DestroyGBufferTargets(gbuffer, resources);
        lighting->destroy_lighting_fbo(resources);
        CreateGBufferTargets(gbuffer, resources, w, h, true);
        lighting->init_lighting_fbo(resources, w, h);
        if (!gbuffer.fbo_id || !lighting->lighting_fbo().fbo_id)
            throw std::runtime_error("Static preview framebuffer allocation failed");
        width = w;
        height = h;
    }
};
StaticRenderer::StaticRenderer(const std::string& root, bool software)
    : m_impl(std::make_unique<Impl>(root, software)) {}
StaticRenderer::~StaticRenderer() = default;
std::string StaticRenderer::ModulePath() {
#ifdef _WIN32
    HMODULE module = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(&active),
                            &module))
        throw std::runtime_error("Cannot locate renderer module");
    std::wstring path(32768, L'\0');
    const DWORD length = GetModuleFileNameW(module, path.data(), path.size());
    if (!length || length >= path.size())
        throw std::runtime_error("Cannot locate renderer module path");
    path.resize(length);
    const auto utf8 = std::filesystem::path(path).u8string();
    return {utf8.begin(), utf8.end()};
#else
    Dl_info info{};
    if (!dladdr(&active, &info) || !info.dli_fname)
        throw std::runtime_error("Cannot locate renderer module path");
    return std::filesystem::canonical(info.dli_fname).string();
#endif
}
StaticFrame StaticRenderer::Render(const RenderView& view,
                                   std::shared_ptr<const StaticDrawSnapshot> scene) {
    auto& impl = *m_impl;
    if (std::this_thread::get_id() != impl.owner)
        throw std::runtime_error("Static renderer must be used on its owning thread");
    if (!scene || impl.sequence == std::numeric_limits<std::uint64_t>::max())
        throw std::invalid_argument("Missing scene or exhausted frame sequence");
    const auto started = std::chrono::steady_clock::now();
    glfwMakeContextCurrent(impl.window);
    impl.Resize(view.description().width, view.description().height);
    glDisable(GL_FRAMEBUFFER_SRGB);
    glDisable(GL_BLEND);
    glDisable(GL_DITHER);
    glViewport(0, 0, impl.width, impl.height);
    glBindFramebuffer(GL_FRAMEBUFFER, impl.gbuffer.fbo_id);
    glClearColor(0, 0, 0, 0);
    glClearDepth(0);
    glDepthMask(GL_TRUE);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    const auto stats = impl.geometry->Render(view, scene, impl.gbuffer);
    RenderContext context;
    context.render_view = &view;
    context.static_studio = true;
    context.screen_width = impl.width;
    context.screen_height = impl.height;
    context.registry = &impl.resources;
    context.screen_quad_vao = impl.quad;
    context.gbuffer_position = adopt_texture(impl.gbuffer.position_texture);
    context.gbuffer_normal = adopt_texture(impl.gbuffer.normal_texture);
    context.gbuffer_albedo = adopt_texture(impl.gbuffer.albedo_texture);
    context.gbuffer_material = adopt_texture(impl.gbuffer.material_texture);
    context.gbuffer_depth = adopt_texture(impl.gbuffer.depth_texture);
    context.authored_surface = adopt_texture(impl.gbuffer.authored_texture);
    context.sun.direction = glm::normalize(glm::vec3(.35f, -.55f, -.75f));
    context.sun.color = glm::vec3(1.0f);
    context.sky_ambient_color = glm::vec3(.22f);
    context.exposure = 1.0f;
    context.moon_illumination = 0;
    impl.lighting->execute(context);
    StaticFrame frame;
    frame.sequence = impl.sequence + 1;
    frame.scene_revision = scene->revision;
    frame.camera = view.description();
    frame.actual_view = view.view();
    frame.actual_projection = view.projection();
    const size_t pixels = static_cast<size_t>(impl.width) * impl.height;
    frame.rgba8.resize(pixels * 4);
    frame.depth32f.resize(pixels);
    frame.coverage8.resize(pixels);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, impl.lighting->lighting_fbo().fbo_id);
    glReadBuffer(GL_COLOR_ATTACHMENT0);
    glReadPixels(0, 0, impl.width, impl.height, GL_RGBA, GL_UNSIGNED_BYTE, frame.rgba8.data());
    glBindFramebuffer(GL_READ_FRAMEBUFFER, impl.gbuffer.fbo_id);
    glReadPixels(
        0, 0, impl.width, impl.height, GL_DEPTH_COMPONENT, GL_FLOAT, frame.depth32f.data());
    if (glGetError() != GL_NO_ERROR)
        throw std::runtime_error("Static frame readback failed");
    for (size_t i = 0; i < pixels; ++i) {
        const float depth = frame.depth32f[i];
        if (!std::isfinite(depth) || depth < 0 || depth > 1)
            throw std::runtime_error("Invalid captured reversed-depth sample");
        const bool covered = depth > 0;
        frame.coverage8[i] = covered ? 1 : 0;
        if (frame.rgba8[i * 4 + 3] != (covered ? 255 : 0))
            throw std::runtime_error("Color/depth coverage mismatch in the production capture");
    }
    frame.draw_count = stats.draws;
    frame.index_count = stats.indices;
    frame.uploaded_meshes = stats.uploaded_meshes;
    frame.uploaded_textures = stats.uploaded_textures;
    frame.updated_instances = stats.updated_instances;
    frame.vendor = impl.vendor;
    frame.renderer = impl.renderer;
    frame.version = impl.version;
    frame.synchronous_render_readback_ms =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started)
            .count();
    ++impl.sequence;
    return frame;
}
} // namespace Luminumbra::Rendering
