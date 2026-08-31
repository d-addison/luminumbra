// UI page-load test.
//
// Loads every shipped RmlUi document the same way ui_smoke_test does: through a
// live Rml_UIManager backed by a hidden, real OpenGL context. For each document
// we assert LoadDocument returned a non-null ElementDocument and that its <body>
// element parsed at least one child (i.e. the markup was parsed without error).
//
// Approach: LIVE RmlUi context (not a parse-level fallback). This mirrors the
// existing ui_smoke_test harness exactly — a hidden GLFW window provides the GL
// render interface RmlUi needs, and the test GTEST_SKIPs when no GL context can
// be created (e.g. headless CI without a GPU/driver), so it never hard-fails for
// environmental reasons.

#include "gtest/gtest.h"

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>
#include <glad/glad.h>

#include <RmlUi/Core.h>

#include <filesystem>
#include <string>
#include <vector>

#include "ui/Rml_UIManager.h"

namespace fs = std::filesystem;

namespace {

#ifndef LUMINUMBRA_SOURCE_ROOT
#define LUMINUMBRA_SOURCE_ROOT "."
#endif

// Hidden, real OpenGL context — identical setup to ui_smoke_test so RmlUi's GL
// render interface initializes. ready == false means GL is unavailable and the
// test should skip rather than fail.
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

        m_window = glfwCreateWindow(800, 600, "ui_page_load_test", nullptr, nullptr);
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
        if (m_window) {
            glfwDestroyWindow(m_window);
        }
        if (m_glfw_initialized) {
            glfwTerminate();
        }
    }

    bool ready() const {
        return m_ready;
    }
    const std::string& error() const {
        return m_error;
    }
    GLFWwindow* window() const {
        return m_window;
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

// Locate the document the manager just loaded by its <body> id. Every shipped
// document sets a stable id on its body (main_menu, settings, hud, ...).
Rml::ElementDocument* FindDocumentByBodyId(Rml::Context* context, const char* body_id) {
    if (!context) {
        return nullptr;
    }
    for (int i = 0; i < context->GetNumDocuments(); ++i) {
        Rml::ElementDocument* candidate = context->GetDocument(i);
        if (candidate && candidate->GetElementById(body_id)) {
            return candidate;
        }
    }
    return nullptr;
}

} // namespace

// Every shipped document loads into a live RmlUi context and parses a populated
// <body>. The body id is the document's stable root id (matches the authored
// `<body id="...">`); a non-null lookup + a populated child list proves the
// document parsed without a fatal markup error.
TEST(UiPageLoadTest, EveryShippedDocumentLoadsWithAPopulatedBody) {
    HiddenGlContext context;
    if (!context.ready()) {
        GTEST_SKIP() << context.error();
    }

    const fs::path source_root = SourceRoot();
    Luminumbra::Client::Rml_UIManager ui((source_root.string() + "/"));
    ui.Init(context.window(), nullptr);
    ASSERT_NE(ui.GetContext(), nullptr);

    struct PageSpec {
        const char* path;    // relative to data/ui
        const char* body_id; // authored <body id="...">
        bool optional;       // skip the check if the file is not shipped
    };

    const std::vector<PageSpec> pages = {
        {"main_menu.rml", "main_menu", false},
        {"settings.rml", "settings", false},
        {"world_creation.rml", "world_creation", false},
        {"world_selection.rml", "world_selection", false},
        {"hud.rml", "hud", true},
        {"photo_mode.rml", "photo_mode", true},
        {"pause.rml", "pause", true},
        {"gallery.rml", "gallery", true},
    };

    int documents_loaded = 0;
    for (const PageSpec& page : pages) {
        const fs::path document_path = source_root / "data/ui" / page.path;
        if (!fs::exists(document_path)) {
            if (page.optional) {
                continue; // not shipped — nothing to check
            }
            ADD_FAILURE() << "required document missing: " << document_path.string();
            continue;
        }

        ui.RequestLoadDocument(page.path);
        ui.Update();

        Rml::ElementDocument* document = FindDocumentByBodyId(ui.GetContext(), page.body_id);
        ASSERT_NE(document, nullptr) << page.path << " — LoadDocument returned null or body #"
                                     << page.body_id << " was not parsed";

        // The document IS the <body> element in RmlUi's tree. A parsed page must
        // contain at least one child element.
        EXPECT_GT(document->GetNumChildren(), 0)
            << page.path << " — body has no children (markup parsed empty)";

        ++documents_loaded;

        // Unload so the next document's body-id lookup is unambiguous.
        ui.GetContext()->UnloadDocument(document);
        ui.Update();
    }

    EXPECT_GE(documents_loaded, 4) << "at least the four core documents must load";

    ui.Shutdown();
}
