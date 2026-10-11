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

#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <future>
#include <mutex>
#include <string>
#include <thread>
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

static void AwaitWorldCatalog(Luminumbra::Client::Rml_UIManager& ui,
                              Rml::ElementDocument* document) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (document && document->HasAttribute("data-worlds-pending") &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        ui.Update();
    }
    ASSERT_NE(document, nullptr);
    EXPECT_FALSE(document->HasAttribute("data-worlds-pending"));
}

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

TEST(UiPageLoadTest, SavedWorldSelectionUsesRealIdsAndReportsRefusals) {
    HiddenGlContext context;
    if (!context.ready())
        GTEST_SKIP() << context.error();
    Luminumbra::Client::Rml_UIManager ui(SourceRoot().string() + "/");
    ui.Init(context.window(), nullptr);
    Luminumbra::Persistence::SavedWorldCatalog catalog;
    std::string loaded;
    int enumerations = 0;
    ui.SetSavedWorldList([&]() {
        ++enumerations;
        return catalog;
    });
    ui.SetLoadWorldCallback([&](const std::string& id) { loaded = id; });
    auto open = [&]() {
        ui.RequestLoadDocument("world_selection.rml");
        ui.Update();
        auto* document = FindDocumentByBodyId(ui.GetContext(), "world_selection");
        AwaitWorldCatalog(ui, document);
        return document;
    };
    auto* doc = open();
    ASSERT_NE(doc, nullptr);
    EXPECT_FALSE(doc->GetElementById("no_worlds")->IsClassSet("hidden"));
    EXPECT_EQ(doc->GetElementById("world_list_items")->GetNumChildren(), 0);
    EXPECT_TRUE(doc->GetElementById("load_selected_btn")->HasAttribute("disabled"));

    Luminumbra::Persistence::SavedWorld current;
    current.metadata.worldId = "world_real_123";
    current.metadata.name = "My <button id='injected'> world & sky";
    current.metadata.seed = "424242";
    current.metadata.worldType = "default";
    catalog.worlds.push_back(current);
    current.metadata.worldId = "world_obsolete";
    current.error = "This world predates the v0.3.0 format and cannot be opened.";
    catalog.worlds.push_back(current);
    ui.RequestLoadDocument("main_menu.rml");
    ui.Update();
    doc = open();
    ASSERT_NE(doc, nullptr);
    EXPECT_EQ(enumerations, 2);
    EXPECT_TRUE(doc->GetElementById("no_worlds")->IsClassSet("hidden"));
    auto* items = doc->GetElementById("world_list_items");
    ASSERT_EQ(items->GetNumChildren(), 2);
    auto* card = items->GetChild(0);
    auto* description = card->GetChild(1);
    ASSERT_NE(description, nullptr);
    EXPECT_NE(description->GetInnerRML().find("424242"), std::string::npos);
    EXPECT_LE(description->GetAbsoluteOffset().y + description->GetClientHeight(),
              card->GetAbsoluteOffset().y + card->GetClientHeight());
    EXPECT_EQ(doc->GetElementById("injected"), nullptr);
    auto* load = doc->GetElementById("load_selected_btn");
    EXPECT_FALSE(load->IsClassSet("hidden"));
    items->GetChild(0)->DispatchEvent("click", {});
    EXPECT_FALSE(load->HasAttribute("disabled"));
    load->DispatchEvent("click", {});
    EXPECT_EQ(loaded, "world_real_123");
    loaded.clear();
    items->GetChild(1)->DispatchEvent("click", {});
    EXPECT_TRUE(load->HasAttribute("disabled"));
    load->DispatchEvent("click", {});
    EXPECT_TRUE(loaded.empty());
    EXPECT_FALSE(doc->GetElementById("notification")->IsClassSet("hidden"));
    EXPECT_NE(doc->GetElementById("notification_text")->GetInnerRML().find("predates"),
              std::string::npos);

    ui.ShowMessage("Could not open <missing> save.");
    EXPECT_EQ(doc->GetElementById("missing"), nullptr);
    catalog.worlds.clear();
    catalog.error = "Saved worlds unavailable: access denied.";
    ui.RequestLoadDocument("main_menu.rml");
    ui.Update();
    doc = open();
    ASSERT_NE(doc, nullptr);
    EXPECT_TRUE(doc->GetElementById("no_worlds")->IsClassSet("hidden"));
    EXPECT_FALSE(doc->GetElementById("world_list_status")->IsClassSet("hidden"));
    ui.Shutdown();
}

TEST(UiPageLoadTest, PendingWorldValidationAllowsNavigationAndCannotPublishAfterReopening) {
    HiddenGlContext context;
    if (!context.ready())
        GTEST_SKIP() << context.error();
    std::promise<std::thread::id> started;
    Luminumbra::Client::Rml_UIManager ui(SourceRoot().string() + "/");
    ui.Init(context.window(), nullptr);
    int loads = 0;
    ui.SetLoadWorldCallback([&](const std::string&) { ++loads; });
    ui.SetSavedWorldList([&](const std::stop_token& stop) {
        started.set_value(std::this_thread::get_id());
        std::mutex mutex;
        std::condition_variable_any wake;
        std::unique_lock lock(mutex);
        wake.wait_for(lock, stop, std::chrono::seconds(5), [] { return false; });
        Luminumbra::Persistence::SavedWorldCatalog obsolete;
        obsolete.error = "obsolete scan result";
        return obsolete;
    });
    ui.RequestLoadDocument("world_selection.rml");
    ui.Update();
    auto* document = FindDocumentByBodyId(ui.GetContext(), "world_selection");
    ASSERT_NE(document, nullptr);
    auto thread = started.get_future();
    ASSERT_EQ(thread.wait_for(std::chrono::seconds(5)), std::future_status::ready);
    EXPECT_NE(thread.get(), std::this_thread::get_id());
    EXPECT_TRUE(document->HasAttribute("data-worlds-pending"));
    EXPECT_TRUE(document->GetElementById("no_worlds")->IsClassSet("hidden"));
    EXPECT_FALSE(document->GetElementById("world_list_status")->IsClassSet("hidden"));
    auto* load = document->GetElementById("load_selected_btn");
    ASSERT_NE(load, nullptr);
    EXPECT_TRUE(load->HasAttribute("disabled"));
    load->DispatchEvent("click", {});
    EXPECT_EQ(loads, 0);
    document->GetElementById("back_btn")->DispatchEvent("click", {});
    ui.Update();
    ASSERT_NE(FindDocumentByBodyId(ui.GetContext(), "main_menu"), nullptr);
    ui.SetSavedWorldList([](const std::stop_token&) {
        Luminumbra::Persistence::SavedWorldCatalog latest;
        Luminumbra::Persistence::SavedWorld world;
        world.metadata.worldId = "latest";
        world.metadata.name = "Latest world";
        latest.worlds.push_back(world);
        return latest;
    });
    ui.RequestLoadDocument("world_selection.rml");
    ui.Update();
    document = FindDocumentByBodyId(ui.GetContext(), "world_selection");
    AwaitWorldCatalog(ui, document);
    ASSERT_NE(document, nullptr);
    EXPECT_TRUE(document->GetElementById("world_list_status")->IsClassSet("hidden"));
    auto* items = document->GetElementById("world_list_items");
    ASSERT_EQ(items->GetNumChildren(), 1);
    EXPECT_EQ(items->GetChild(0)->GetAttribute<Rml::String>("data-world-id", ""), "latest");
    ui.Shutdown();
}

TEST(UiPageLoadTest, CreationCanScrollToItsActionAtSmallWindowSizes) {
    HiddenGlContext context;
    if (!context.ready())
        GTEST_SKIP() << context.error();
    Luminumbra::Client::Rml_UIManager ui(SourceRoot().string() + "/");
    ui.Init(context.window(), nullptr);
    ui.RequestLoadDocument("world_creation.rml");
    ui.Update();
    auto* doc = FindDocumentByBodyId(ui.GetContext(), "world_creation");
    ASSERT_NE(doc, nullptr);
    auto* create = doc->GetElementById("create_btn");
    ASSERT_NE(create, nullptr);
    for (const auto size : {Rml::Vector2i(800, 600), Rml::Vector2i(1280, 720)}) {
        glfwSetWindowSize(context.window(), size.x, size.y);
        ui.Update();
        create->ScrollIntoView(false);
        ui.Update();
        const auto pos = create->GetAbsoluteOffset(Rml::BoxArea::Border);
        EXPECT_GE(pos.x, 0.0f);
        EXPECT_GE(pos.y, 0.0f);
        EXPECT_LE(pos.x + create->GetClientWidth(), static_cast<float>(size.x));
        EXPECT_LE(pos.y + create->GetClientHeight(), static_cast<float>(size.y));
        EXPECT_GT(create->GetClientWidth(), 100.0f);
        auto* name = doc->GetElementById("world_name");
        EXPECT_GT(name->GetClientWidth(), 200.0f);
    }
    ui.Shutdown();
}
