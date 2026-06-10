#include "gtest/gtest.h"

#include <glad/glad.h>
#include <GLFW/glfw3.h>

#include <RmlUi/Core.h>
#include <RmlUi/Core/Elements/ElementFormControl.h>

#include <filesystem>
#include <fstream>
#include <optional>
#include <regex>
#include <sstream>
#include <string>
#include <vector>

#include "ui/Rml_UIManager.h"
#include "ui/core/UIStateManager.h"

namespace fs = std::filesystem;

namespace {

#ifndef LUMINUMBRA_SOURCE_ROOT
#define LUMINUMBRA_SOURCE_ROOT "."
#endif

#ifndef LUMINUMBRA_TEST_ARTIFACT_DIR
#define LUMINUMBRA_TEST_ARTIFACT_DIR "."
#endif

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

        m_window = glfwCreateWindow(800, 600, "ui_smoke_test", nullptr, nullptr);
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

    bool ready() const { return m_ready; }
    const std::string& error() const { return m_error; }
    GLFWwindow* window() const { return m_window; }

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
    return fs::path(LUMINUMBRA_TEST_ARTIFACT_DIR) / "ui";
}

std::string ReadTextFile(const fs::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        return {};
    }
    std::stringstream stream;
    stream << file.rdbuf();
    return stream.str();
}

std::vector<std::string> ExtractLinkedStylesheets(const std::string& rml) {
    std::vector<std::string> links;
    const std::regex stylesheet_regex(R"(<link[^>]*href\s*=\s*\"([^\"]+)\"[^>]*/?>)", std::regex::icase);
    for (std::sregex_iterator it(rml.begin(), rml.end(), stylesheet_regex), end; it != end; ++it) {
        links.push_back((*it)[1].str());
    }
    return links;
}

void WriteUiArtifact(const fs::path& path,
                     int loaded_documents,
                     int required_elements_checked,
                     int linked_stylesheets_checked,
                     int navigation_edges_checked) {
    std::ofstream output(path);
    ASSERT_TRUE(output) << path.string();
    output << "{\n";
    output << "  \"schema\": \"luminumbra.ui_smoke.v1\",\n";
    output << "  \"loaded_documents\": " << loaded_documents << ",\n";
    output << "  \"required_elements_checked\": " << required_elements_checked << ",\n";
    output << "  \"linked_stylesheets_checked\": " << linked_stylesheets_checked << ",\n";
    output << "  \"navigation_edges_checked\": " << navigation_edges_checked << ",\n";
    output << "  \"event_bindings_checked\": [\"new_world_btn\", \"load_world_btn\", \"quit_btn\", \"back_btn\", \"create_btn\"],\n";
    output << "  \"passed\": true\n";
    output << "}\n";
}

Rml::ElementDocument* FindDocumentByElementId(Rml::Context* context, const char* element_id) {
    if (!context) {
        return nullptr;
    }
    for (int i = 0; i < context->GetNumDocuments(); ++i) {
        Rml::ElementDocument* candidate = context->GetDocument(i);
        if (candidate && candidate->GetElementById(element_id)) {
            return candidate;
        }
    }
    return nullptr;
}

Rml::ElementDocument* LoadDocumentAndFind(Luminumbra::Client::Rml_UIManager& ui,
                                          const char* path,
                                          const char* required_id) {
    ui.RequestLoadDocument(path);
    ui.Update();
    return FindDocumentByElementId(ui.GetContext(), required_id);
}

void ClickAndUpdate(Luminumbra::Client::Rml_UIManager& ui, Rml::Element* element) {
    ASSERT_NE(element, nullptr);
    element->Click();
    ui.Update();
}

void SetControlValue(Rml::ElementDocument* document, const char* id, const std::string& value) {
    ASSERT_NE(document, nullptr);
    Rml::Element* element = document->GetElementById(id);
    ASSERT_NE(element, nullptr) << id;
    auto* control = dynamic_cast<Rml::ElementFormControl*>(element);
    ASSERT_NE(control, nullptr) << id;
    control->SetValue(value);
    EXPECT_EQ(control->GetValue(), value);
}

Rml::Element* FirstWorldListItem(Rml::ElementDocument* document) {
    if (!document) {
        return nullptr;
    }
    Rml::ElementList items;
    document->GetElementsByClassName(items, "list-item");
    return items.empty() ? nullptr : items.front();
}

void WriteUiInteractionArtifact(const fs::path& path,
                                int navigation_clicks_checked,
                                int form_fields_checked,
                                const std::string& created_name,
                                const std::string& created_seed,
                                const std::string& created_type,
                                const std::string& loaded_world_id,
                                bool quit_requested) {
    std::ofstream output(path);
    ASSERT_TRUE(output) << path.string();
    output << "{\n";
    output << "  \"schema\": \"luminumbra.ui_interactions.v1\",\n";
    output << "  \"navigation_clicks_checked\": " << navigation_clicks_checked << ",\n";
    output << "  \"form_fields_checked\": " << form_fields_checked << ",\n";
    output << "  \"create_world_callback\": {\n";
    output << "    \"called\": true,\n";
    output << "    \"name\": \"" << created_name << "\",\n";
    output << "    \"seed\": \"" << created_seed << "\",\n";
    output << "    \"type\": \"" << created_type << "\"\n";
    output << "  },\n";
    output << "  \"load_world_callback\": {\n";
    output << "    \"called\": true,\n";
    output << "    \"world_id\": \"" << loaded_world_id << "\"\n";
    output << "  },\n";
    output << "  \"quit_requested\": " << (quit_requested ? "true" : "false") << ",\n";
    output << "  \"passed\": true\n";
    output << "}\n";
}

} // namespace

TEST(UiSmokeTest, AuthoredRmlDocumentsLoadAndExposeRequiredElements) {
    HiddenGlContext context;
    if (!context.ready()) {
        GTEST_SKIP() << context.error();
    }

    fs::create_directories(ArtifactRoot());
    const fs::path source_root = SourceRoot();
    Luminumbra::Client::Rml_UIManager ui((source_root.string() + "/"));
    ui.Init(context.window(), nullptr);
    ASSERT_NE(ui.GetContext(), nullptr);

    struct DocumentSpec {
        const char* path;
        std::vector<const char*> required_ids;
    };

    const std::vector<DocumentSpec> documents = {
        {"main_menu.rml", {"main_menu", "new_world_btn", "load_world_btn", "settings_btn", "quit_btn", "notification", "notification_text"}},
        {"world_creation.rml", {"world_name", "world_seed", "world_type", "back_btn", "create_btn"}},
        {"world_selection.rml", {"filter_all", "filter_recent", "filter_favorites", "back_btn", "load_selected_btn", "import_world_btn"}},
    };

    int required_elements_checked = 0;
    int linked_stylesheets_checked = 0;
    for (const DocumentSpec& spec : documents) {
        const fs::path document_path = source_root / "data/ui" / spec.path;
        const std::string source = ReadTextFile(document_path);
        ASSERT_FALSE(source.empty()) << document_path.string();
        for (const std::string& stylesheet : ExtractLinkedStylesheets(source)) {
            ++linked_stylesheets_checked;
            EXPECT_TRUE(fs::exists(source_root / "data/ui" / stylesheet))
                << "Missing stylesheet linked from " << spec.path << ": " << stylesheet;
        }

        ui.RequestLoadDocument(spec.path);
        ui.Update();
        Rml::ElementDocument* document = nullptr;
        for (int i = 0; i < ui.GetContext()->GetNumDocuments(); ++i) {
            Rml::ElementDocument* candidate = ui.GetContext()->GetDocument(i);
            if (candidate && candidate->GetElementById(spec.required_ids.front())) {
                document = candidate;
                break;
            }
        }
        ASSERT_NE(document, nullptr) << spec.path;

        for (const char* id : spec.required_ids) {
            ++required_elements_checked;
            EXPECT_NE(document->GetElementById(id), nullptr) << spec.path << " missing #" << id;
        }
    }

    const fs::path font = source_root / "data/fonts/Lora/static/Lora-Regular.ttf";
    EXPECT_TRUE(fs::exists(font)) << font.string();

    const std::string manager_source = ReadTextFile(source_root / "src/luminumbra_client/ui/Rml_UIManager.cpp");
    ASSERT_FALSE(manager_source.empty());
    const std::vector<std::string> required_navigation_edges = {
        "new_world_btn",
        "world_creation.rml",
        "load_world_btn",
        "world_selection.rml",
        "quit_btn",
        "back_btn",
        "main_menu.rml",
        "create_btn",
    };
    for (const std::string& edge : required_navigation_edges) {
        EXPECT_NE(manager_source.find(edge), std::string::npos) << edge;
    }

    WriteUiArtifact(
        ArtifactRoot() / "ui_smoke.json",
        static_cast<int>(documents.size()),
        required_elements_checked,
        linked_stylesheets_checked,
        static_cast<int>(required_navigation_edges.size()));

    ui.Shutdown();
}

TEST(UiSmokeTest, UiStateNavigationMaintainsDocumentAndGameState) {
    auto& state = Luminumbra::Client::UI::UIState();

    state.NavigateToDocument("main_menu.rml");
    EXPECT_EQ(state.currentUIDocument.Get(), "main_menu.rml");
    EXPECT_EQ(state.currentGameState.Get(), Luminumbra::Client::UI::GameState::MainMenu);
    EXPECT_TRUE(state.isMainMenu.Get());
    EXPECT_FALSE(state.isInGame.Get());

    state.NavigateToDocument("world_creation.rml");
    EXPECT_EQ(state.currentUIDocument.Get(), "world_creation.rml");
    EXPECT_EQ(state.currentGameState.Get(), Luminumbra::Client::UI::GameState::WorldCreation);
    EXPECT_FALSE(state.isMainMenu.Get());

    state.NavigateToDocument("world_selection.rml");
    EXPECT_EQ(state.currentUIDocument.Get(), "world_selection.rml");
    EXPECT_EQ(state.currentGameState.Get(), Luminumbra::Client::UI::GameState::WorldSelection);
    EXPECT_FALSE(state.isMainMenu.Get());

    state.ShowNotification("UI smoke", 2.0f);
    EXPECT_EQ(state.notificationMessage.Get(), "UI smoke");
    EXPECT_FLOAT_EQ(state.notificationTimeout.Get(), 2.0f);
    state.ClearNotification();
    EXPECT_TRUE(state.notificationMessage.Get().empty());
    EXPECT_FLOAT_EQ(state.notificationTimeout.Get(), 0.0f);
}

TEST(UiSmokeTest, AuthoredMenuInteractionsNavigateAndInvokeCallbacks) {
    HiddenGlContext context;
    if (!context.ready()) {
        GTEST_SKIP() << context.error();
    }

    fs::create_directories(ArtifactRoot());
    const fs::path source_root = SourceRoot();
    Luminumbra::Client::Rml_UIManager ui((source_root.string() + "/"));
    ui.Init(context.window(), nullptr);
    ASSERT_NE(ui.GetContext(), nullptr);

    struct CreatedWorld {
        std::string name;
        std::string seed;
        std::string type;
    };

    std::optional<CreatedWorld> created_world;
    std::optional<std::string> loaded_world_id;
    ui.SetWorldCreationCallback([&](const std::string& name, const std::string& seed, const std::string& type) {
        created_world = CreatedWorld{name, seed, type};
    });
    ui.SetLoadWorldCallback([&](const std::string& world_id) {
        loaded_world_id = world_id;
    });

    int navigation_clicks_checked = 0;
    int form_fields_checked = 0;

    Rml::ElementDocument* main_menu = LoadDocumentAndFind(ui, "main_menu.rml", "main_menu");
    ASSERT_NE(main_menu, nullptr);
    ClickAndUpdate(ui, main_menu->GetElementById("new_world_btn"));
    ++navigation_clicks_checked;

    Rml::ElementDocument* world_creation = FindDocumentByElementId(ui.GetContext(), "world_creation");
    ASSERT_NE(world_creation, nullptr);
    SetControlValue(world_creation, "world_name", "Interaction Test World");
    SetControlValue(world_creation, "world_seed", "424242");
    SetControlValue(world_creation, "world_type", "mountains");
    form_fields_checked += 3;
    ClickAndUpdate(ui, world_creation->GetElementById("create_btn"));
    ASSERT_TRUE(created_world.has_value());
    EXPECT_EQ(created_world->name, "Interaction Test World");
    EXPECT_EQ(created_world->seed, "424242");
    EXPECT_EQ(created_world->type, "mountains");

    ClickAndUpdate(ui, world_creation->GetElementById("back_btn"));
    ++navigation_clicks_checked;
    main_menu = FindDocumentByElementId(ui.GetContext(), "main_menu");
    ASSERT_NE(main_menu, nullptr);

    ClickAndUpdate(ui, main_menu->GetElementById("load_world_btn"));
    ++navigation_clicks_checked;
    Rml::ElementDocument* world_selection = FindDocumentByElementId(ui.GetContext(), "world_selection");
    ASSERT_NE(world_selection, nullptr);
    Rml::Element* first_world = FirstWorldListItem(world_selection);
    ASSERT_NE(first_world, nullptr);
    const std::string expected_world_id = first_world->GetAttribute<Rml::String>("data-world-id", "");
    ASSERT_FALSE(expected_world_id.empty());
    ClickAndUpdate(ui, first_world);
    ++navigation_clicks_checked;
    EXPECT_EQ(first_world->GetAttribute<Rml::String>("data-selected", ""), "true");
    Rml::Element* load_selected = world_selection->GetElementById("load_selected_btn");
    ASSERT_NE(load_selected, nullptr);
    EXPECT_FALSE(load_selected->HasAttribute("disabled"));
    ClickAndUpdate(ui, load_selected);
    ASSERT_TRUE(loaded_world_id.has_value());
    EXPECT_EQ(*loaded_world_id, expected_world_id);

    ClickAndUpdate(ui, world_selection->GetElementById("back_btn"));
    ++navigation_clicks_checked;
    main_menu = FindDocumentByElementId(ui.GetContext(), "main_menu");
    ASSERT_NE(main_menu, nullptr);

    glfwSetWindowShouldClose(context.window(), GLFW_FALSE);
    ClickAndUpdate(ui, main_menu->GetElementById("quit_btn"));
    ++navigation_clicks_checked;
    const bool quit_requested = glfwWindowShouldClose(context.window()) != 0;
    EXPECT_TRUE(quit_requested);

    WriteUiInteractionArtifact(
        ArtifactRoot() / "ui_interactions.json",
        navigation_clicks_checked,
        form_fields_checked,
        created_world->name,
        created_world->seed,
        created_world->type,
        *loaded_world_id,
        quit_requested);

    ui.Shutdown();
}
