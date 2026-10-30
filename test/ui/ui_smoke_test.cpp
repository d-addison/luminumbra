#include "../support/UniqueTempPath.h"
#include "luminumbra_client/rendering/PixelIo.h"
#include "luminumbra_common/game/PhotoMode.h"
#include "gtest/gtest.h"

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>
#include <glad/glad.h>

#include <SOIL2/SOIL2.h>

#include <RmlUi/Core.h>
#include <RmlUi/Core/Elements/ElementFormControl.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <imgui.h>
#include <imgui_internal.h>
#include <ios>
#include <limits>
#include <nlohmann/json.hpp>
#include <nlohmann/json_fwd.hpp>
#include <optional>
#include <regex>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#ifndef _WIN32
#include <fcntl.h>
#include <unistd.h>
#endif

#include "audio/NullAudioManager.h"
#include "core/FilesystemPath.h"
#include "luminumbra_common/core/CaptureContentPaths.h"
#include "luminumbra_common/core/FramePacing.h"
#include "luminumbra_common/persistence/SavedInputLimits.h"
#include "luminumbra_common/persistence/WorldSaveService.h"
#include "luminumbra_common/world/KnobLayer.h"
#include "luminumbra_common/world/TerrainPresetLoader.h"
#include "ui/Rml_UIManager.h"
#include "ui/UiLayout.h"
#include "ui/components/common/Button.h"
#include "ui/components/common/Input.h"
#include "ui/components/common/Panel.h"
#include "ui/components/game/WorldList.h"
#include "ui/core/UIComponent.h"
#include "ui/core/UIHotReload.h"
#include "ui/core/UIStateManager.h"
#include "world/WorldEntry.h"
#include "world/WorldgenOverride.h"

namespace fs = std::filesystem;

namespace {

class UserPresetPersistence : public testing::Test {
private:
    fs::path m_directory;

protected:
    const fs::path& Directory() const {
        return m_directory;
    }
    void SetUp() override {
        m_directory = Luminumbra::TestSupport::CreateUniqueTempDirectory("user_preset_write_");
    }
    void TearDown() override {
        Luminumbra::Persistence::WorldSaveService::set_interrupt_before_region_replace_for_testing(
            false);
        std::error_code error;
        fs::remove_all(m_directory, error);
    }
    static std::string Read(const fs::path& path) {
        const std::ifstream input(path, std::ios::binary);
        std::ostringstream bytes;
        bytes << input.rdbuf();
        return bytes.str();
    }
    static void Write(const fs::path& path, const std::string& bytes) {
        ASSERT_TRUE(Luminumbra::Persistence::WorldSaveService::write_file_durably(path, bytes));
    }
    void ExpectNoStaging() const {
        for (const auto& entry : fs::recursive_directory_iterator(m_directory))
            EXPECT_EQ(Luminumbra::Filesystem::GenericUtf8PathString(entry.path().filename())
                          .find(".tmp."),
                      std::string::npos);
    }
};

TEST_F(UserPresetPersistence, NativeWatcherPathsReachFilesystemAndEmitUtf8) {
    const auto directory = Directory() / fs::path(u8"ui-\u4e16\u754c");
    fs::create_directory(directory);
    const auto document = directory / fs::path(u8"page-\u03a9.rml");
    ASSERT_NO_FATAL_FAILURE(Write(document, "initial"));
    ASSERT_NO_FATAL_FAILURE(Write(directory / fs::path(u8"other.\u4e16\u754c"), "ignored"));
    Luminumbra::Client::UI::UIHotReload watcher;
    watcher.SetEnabled(true);
    std::vector<std::string> changed;
    watcher.SetReloadCallback([&](const std::string& path) { changed.push_back(path); });
    watcher.WatchDirectory(directory, "rml");
    const auto stamp = fs::last_write_time(document);
    fs::last_write_time(document, stamp + std::chrono::seconds(2));
    // One watcher interval, independent of timestamp resolution and renderer state.
    std::this_thread::sleep_for(std::chrono::milliseconds(1100));
    watcher.Update();
    ASSERT_EQ(changed.size(), 1u);
    EXPECT_EQ(changed.front(), Luminumbra::Filesystem::GenericUtf8PathString(document));
    watcher.StopWatching(document);
    watcher.ClearAllWatches();
    changed.clear();
    watcher.WatchFile(directory / "absent");
    watcher.WatchDirectory(directory / "absent", "rml");
    watcher.Update();
    EXPECT_TRUE(changed.empty());
}

TEST_F(UserPresetPersistence, InterruptedRenamePreservesSourceAndDestinationThenRetries) {
    const auto source = Directory() / "user_source.json";
    const auto destination = Directory() / "user_destination.json";
    for (const bool same_name : {false, true}) {
        SCOPED_TRACE(same_name);
        ASSERT_NO_FATAL_FAILURE(Write(source, R"({"name":"Source","unknown":17})"));
        ASSERT_NO_FATAL_FAILURE(Write(destination, R"({"name":"Existing destination"})"));
        const auto source_bytes = Read(source);
        const auto destination_bytes = Read(destination);
        const auto target = same_name ? source : Directory() / "new_destination.json";
        Luminumbra::Persistence::WorldSaveService::set_interrupt_before_region_replace_for_testing(
            true);
        const bool renamed = Luminumbra::Client::RenameUserPreset(source, target, "Renamed");
        Luminumbra::Persistence::WorldSaveService::set_interrupt_before_region_replace_for_testing(
            false);
        EXPECT_FALSE(renamed);
        EXPECT_EQ(Read(source), source_bytes);
        EXPECT_EQ(Read(destination), destination_bytes);
        ExpectNoStaging();
        ASSERT_TRUE(Luminumbra::Client::RenameUserPreset(source, target, "Renamed"));
        EXPECT_EQ(nlohmann::json::parse(Read(target)).at("name"), "Renamed");
        EXPECT_EQ(nlohmann::json::parse(Read(target)).at("unknown"), 17);
        EXPECT_EQ(fs::exists(source), same_name);
        ExpectNoStaging();
    }
}

TEST_F(UserPresetPersistence, OccupiedDestinationRefusesExactBytesAndThenRetries) {
    const auto source = Directory() / "user_source.json";
    const auto destination = Directory() / "user_destination.json";
    const std::string source_bytes = "{ \"name\": \"Source\", \"unknown\": 17 }\n";
    const std::string occupied_bytes = "{ \"name\": \"Occupied\" }\r\n";
    ASSERT_NO_FATAL_FAILURE(Write(source, source_bytes));
    ASSERT_NO_FATAL_FAILURE(Write(destination, occupied_bytes));
    for (int attempt = 0; attempt < 2; ++attempt) {
        EXPECT_FALSE(Luminumbra::Client::RenameUserPreset(source, destination, "Renamed"));
        EXPECT_EQ(Read(source), source_bytes);
        EXPECT_EQ(Read(destination), occupied_bytes);
        ExpectNoStaging();
    }
    ASSERT_TRUE(fs::remove(destination));
    ASSERT_TRUE(Luminumbra::Client::RenameUserPreset(source, destination, "Renamed"));
    EXPECT_FALSE(fs::exists(source));
    EXPECT_EQ(nlohmann::json::parse(Read(destination)).at("unknown"), 17);
    ExpectNoStaging();
}

TEST_F(UserPresetPersistence, InvalidSourceAndPublicationFailurePreserveExistingFiles) {
    const auto source = Directory() / "user_source.json";
    const auto destination = Directory() / "user_destination.json";
    ASSERT_NO_FATAL_FAILURE(Write(destination, R"({"name":"Existing destination"})"));
    const auto before = Read(destination);
    EXPECT_FALSE(Luminumbra::Client::RenameUserPreset(source, destination, "Renamed"));
    fs::create_directory(source);
    EXPECT_FALSE(Luminumbra::Client::RenameUserPreset(source, destination, "Renamed"));
    EXPECT_EQ(Read(destination), before);
    fs::remove(source);
    for (const auto* bytes : {"", "{", "[]", "null", "{}trailing"}) {
        SCOPED_TRACE(bytes);
        ASSERT_NO_FATAL_FAILURE(Write(source, bytes));
        EXPECT_FALSE(Luminumbra::Client::RenameUserPreset(source, destination, "Renamed"));
        EXPECT_EQ(Read(source), bytes);
        EXPECT_EQ(Read(destination), before);
    }
    ASSERT_NO_FATAL_FAILURE(Write(source, R"({"name":"Source"})"));
    EXPECT_FALSE(Luminumbra::Client::RenameUserPreset(source, destination, std::string(1, '\xff')));
    EXPECT_EQ(nlohmann::json::parse(Read(source)).at("name"), "Source");
    EXPECT_EQ(Read(destination), before);
    const auto blocked = Directory() / "blocked.json";
    fs::create_directory(blocked);
    ASSERT_NO_FATAL_FAILURE(Write(blocked / "keep", "occupied"));
    EXPECT_FALSE(Luminumbra::Client::RenameUserPreset(source, blocked, "Renamed"));
    EXPECT_EQ(nlohmann::json::parse(Read(source)).at("name"), "Source");
    EXPECT_EQ(Read(blocked / "keep"), "occupied");
    ExpectNoStaging();
    fs::remove_all(blocked);
    ASSERT_TRUE(Luminumbra::Client::RenameUserPreset(source, blocked, ""));
    EXPECT_EQ(nlohmann::json::parse(Read(blocked)).at("name"), "Custom");
    EXPECT_FALSE(fs::exists(source));
}

TEST_F(UserPresetPersistence, RenameAdmitsInputAndOutputBeforePublication) {
    using namespace Luminumbra::Persistence;
    const auto source = Directory() / "user_source.json";
    const auto destination = Directory() / "user_destination.json";
    const auto nested = [](std::size_t depth) {
        return R"({"extension":)" + std::string(depth - 1, '[') + "0" +
               std::string(depth - 1, ']') + "}";
    };
    ASSERT_NO_FATAL_FAILURE(Write(source, nested(SavedInputLimits::kJsonNestingDepth)));
    ASSERT_TRUE(Luminumbra::Client::RenameUserPreset(source, destination, "Accepted"));
    EXPECT_FALSE(fs::exists(source));
    const auto before = Read(destination);
    auto oversized = std::string("{}");
    oversized.resize(SavedInputLimits::kPresetBytes + 1, ' ');
    for (const auto& bytes : {nested(SavedInputLimits::kJsonNestingDepth + 1), oversized}) {
        ASSERT_NO_FATAL_FAILURE(Write(source, bytes));
        EXPECT_FALSE(Luminumbra::Client::RenameUserPreset(source, source, "Refused"));
        EXPECT_EQ(Read(source), bytes);
        EXPECT_EQ(Read(destination), before);
        ExpectNoStaging();
    }
    const auto overhead = nlohmann::json{{"name", ""}}.dump(2).size();
    ASSERT_NO_FATAL_FAILURE(Write(source, R"({"name":"Original"})"));
    std::string display_name(SavedInputLimits::kPresetBytes - overhead, 'x');
    ASSERT_TRUE(Luminumbra::Client::RenameUserPreset(source, source, display_name));
    EXPECT_EQ(Read(source).size(), SavedInputLimits::kPresetBytes);
    const auto accepted = Read(source);
    display_name += 'x';
    EXPECT_FALSE(Luminumbra::Client::RenameUserPreset(source, source, display_name));
    display_name.resize(SavedInputLimits::kPresetBytes + 1, 'x');
    EXPECT_FALSE(Luminumbra::Client::RenameUserPreset(source, source, display_name));
    EXPECT_EQ(Read(source), accepted);
    EXPECT_EQ(Read(destination), before);
    ExpectNoStaging();
}

TEST_F(UserPresetPersistence, AliasedDestinationKeepsThePublishedPreset) {
    const auto source = Directory() / "user_source.json";
    ASSERT_NO_FATAL_FAILURE(Write(source, R"({"name":"Source","unknown":17})"));
    const auto destination = Directory() / "." / source.filename();
    ASSERT_NE(source, destination);
    ASSERT_TRUE(Luminumbra::Client::RenameUserPreset(source, destination, "Renamed"));
    EXPECT_EQ(nlohmann::json::parse(Read(source)).at("name"), "Renamed");
    EXPECT_EQ(nlohmann::json::parse(Read(destination)).at("unknown"), 17);
    const auto hard_link = Directory() / "user_hard_link.json";
    fs::create_hard_link(source, hard_link);
    ASSERT_TRUE(Luminumbra::Client::RenameUserPreset(source, hard_link, "Separate entry"));
    EXPECT_FALSE(fs::exists(source));
    EXPECT_EQ(nlohmann::json::parse(Read(hard_link)).at("name"), "Separate entry");
#ifdef _WIN32
    ASSERT_NO_FATAL_FAILURE(Write(source, R"({"name":"Source"})"));
    const auto case_variant = Directory() / "USER_SOURCE.JSON";
    ASSERT_TRUE(Luminumbra::Client::RenameUserPreset(source, case_variant, "Case variant"));
    EXPECT_EQ(nlohmann::json::parse(Read(case_variant)).at("name"), "Case variant");
#endif
    ExpectNoStaging();
}

#ifndef _WIN32
TEST_F(UserPresetPersistence, HardLinkSourceRemovalFailureRestoresAliasAndRetries) {
    const auto source = Directory() / "source.json";
    const auto destination = Directory() / "alias.json";
    const std::string original = R"({"name":"Original","unknown":17})";
    ASSERT_NO_FATAL_FAILURE(Write(source, original));
    fs::create_hard_link(source, destination);
    const int descriptor = ::open(source.c_str(), O_RDONLY);
    ASSERT_GE(descriptor, 0);
    const auto held_source = fs::path("/dev/fd") / std::to_string(descriptor);
    const bool renamed = Luminumbra::Client::RenameUserPreset(held_source, destination, "Renamed");
    EXPECT_EQ(::close(descriptor), 0);
    EXPECT_FALSE(renamed);
    EXPECT_EQ(Read(source), original);
    EXPECT_EQ(Read(destination), original);
    EXPECT_TRUE(fs::equivalent(source, destination));
    ExpectNoStaging();
    ASSERT_TRUE(Luminumbra::Client::RenameUserPreset(source, destination, "Renamed"));
    EXPECT_FALSE(fs::exists(source));
    EXPECT_EQ(nlohmann::json::parse(Read(destination)).at("name"), "Renamed");
    ExpectNoStaging();
}
#endif

TEST_F(UserPresetPersistence, SourceRemovalFailureKeepsBothCompletePresets) {
    const auto source = Directory() / "original/user_source.json";
    const auto destination = Directory() / "user_destination.json";
    ASSERT_NO_FATAL_FAILURE(Write(source, R"({"name":"Source"})"));
#ifdef _WIN32
    // An open CRT read handle denies deletion on Windows.
    std::ifstream held_source(source, std::ios::binary);
    ASSERT_TRUE(held_source);
    const auto removal_source = source;
#else
    // Descriptor entries can be read but not unlinked, even by a privileged runner.
    const int descriptor = ::open(source.c_str(), O_RDONLY);
    ASSERT_GE(descriptor, 0);
    const auto removal_source = fs::path("/dev/fd") / std::to_string(descriptor);
#endif
    const bool renamed =
        Luminumbra::Client::RenameUserPreset(removal_source, destination, "Renamed");
#ifdef _WIN32
    held_source.close();
    EXPECT_FALSE(held_source.fail());
#else
    EXPECT_EQ(::close(descriptor), 0);
#endif
    EXPECT_FALSE(renamed);
    EXPECT_EQ(nlohmann::json::parse(Read(source)).at("name"), "Source");
    EXPECT_FALSE(fs::exists(destination));
    ExpectNoStaging();
    ASSERT_TRUE(Luminumbra::Client::RenameUserPreset(source, destination, "Renamed"));
    EXPECT_FALSE(fs::exists(source));
}

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
    const std::regex stylesheet_regex(R"(<link[^>]*href\s*=\s*\"([^\"]+)\"[^>]*/?>)",
                                      std::regex::icase);
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
    output << "  \"event_bindings_checked\": [\"new_world_btn\", \"load_world_btn\", \"quit_btn\", "
              "\"back_btn\", \"create_btn\"],\n";
    output << "  \"passed\": true\n";
    output << "}\n";
}

bool CaptureUiPng(Luminumbra::Client::Rml_UIManager& ui,
                  GLFWwindow* window,
                  const fs::path& output_path) {
    int width = 0;
    int height = 0;
    glfwGetFramebufferSize(window, &width, &height);
    if (width < 1 || height < 1)
        return false;

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, width, height);
    glClearColor(0.025f, 0.035f, 0.055f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
    ui.Render();
    glFinish();

    std::vector<unsigned char> pixels(static_cast<std::size_t>(width * height * 4));
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadBuffer(GL_BACK);
    glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
    const std::size_t row_bytes = static_cast<std::size_t>(width * 4);
    std::vector<unsigned char> row(row_bytes);
    for (int y = 0; y < height / 2; ++y) {
        unsigned char* top = pixels.data() + static_cast<std::size_t>(y) * row_bytes;
        unsigned char* bottom =
            pixels.data() + static_cast<std::size_t>(height - 1 - y) * row_bytes;
        std::copy(top, top + row_bytes, row.data());
        std::copy(bottom, bottom + row_bytes, top);
        std::copy(row.data(), row.data() + row_bytes, bottom);
    }
    fs::create_directories(output_path.parent_path());
    return SOIL_save_image(
               output_path.string().c_str(), SOIL_SAVE_TYPE_PNG, width, height, 4, pixels.data()) !=
           0;
}

void WriteUiScreenshotArtifact(const fs::path& path,
                               const std::vector<std::pair<std::string, fs::path>>& captures) {
    std::ofstream output(path);
    ASSERT_TRUE(output) << path.string();
    output << "{\n";
    output << "  \"schema\": \"luminumbra.ui_screenshots.v1\",\n";
    output << "  \"capture_window\": {\"width\": 800, \"height\": 600, \"visible\": false},\n";
    output << "  \"screenshot_count\": " << captures.size() << ",\n";
    output << "  \"screenshots\": [\n";
    for (std::size_t i = 0; i < captures.size(); ++i) {
        output << "    {\"view\": \"" << captures[i].first << "\", \"file\": \""
               << captures[i].second.filename().string() << "\", \"status\": \"captured\"}"
               << (i + 1 == captures.size() ? "\n" : ",\n");
    }
    output << "  ],\n";
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

void AwaitWorldCatalog(Luminumbra::Client::Rml_UIManager& ui) {
    auto* document = FindDocumentByElementId(ui.GetContext(), "world_selection");
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (document && document->HasAttribute("data-worlds-pending") &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        ui.Update();
    }
    if (document) {
        EXPECT_FALSE(document->HasAttribute("data-worlds-pending"));
    }
}

Rml::ElementDocument* LoadDocumentAndFind(Luminumbra::Client::Rml_UIManager& ui,
                                          const char* path,
                                          const char* required_id) {
    ui.RequestLoadDocument(path);
    ui.Update();
    AwaitWorldCatalog(ui);
    return FindDocumentByElementId(ui.GetContext(), required_id);
}

void ClickAndUpdate(Luminumbra::Client::Rml_UIManager& ui, Rml::Element* element) {
    ASSERT_NE(element, nullptr);
    element->Click();
    ui.Update();
    AwaitWorldCatalog(ui);
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

class ScopedUiShutdown {
public:
    explicit ScopedUiShutdown(Luminumbra::Client::Rml_UIManager& ui)
        : m_ui(ui) {}
    ~ScopedUiShutdown() {
        m_ui.Shutdown();
    }
    ScopedUiShutdown(const ScopedUiShutdown&) = delete;
    ScopedUiShutdown& operator=(const ScopedUiShutdown&) = delete;

private:
    Luminumbra::Client::Rml_UIManager& m_ui;
};

TEST(UiSmokeTest, CapturesMaintainedMenuScreenshots) {
    HiddenGlContext context;
    if (!context.ready()) {
        GTEST_SKIP() << context.error();
    }

    const fs::path source_root = SourceRoot();
    const fs::path capture_root = ArtifactRoot() / "screenshots";
    Luminumbra::Client::Rml_UIManager ui((source_root.string() + "/"));
    ui.Init(context.window(), nullptr);
    const ScopedUiShutdown shutdown(ui);
    ASSERT_NE(ui.GetContext(), nullptr);

    struct CaptureSpec {
        const char* view;
        const char* document;
        const char* required_id;
    };
    const std::vector<CaptureSpec> specs = {
        {"main_menu", "main_menu.rml", "main_menu"},
        {"world_creation", "world_creation.rml", "world_creation"},
        {"world_selection", "world_selection.rml", "world_selection"},
    };
    std::vector<std::pair<std::string, fs::path>> captures;
    for (const CaptureSpec& spec : specs) {
        ASSERT_NE(LoadDocumentAndFind(ui, spec.document, spec.required_id), nullptr)
            << spec.document;
        ui.Update();
        const fs::path output = capture_root / (std::string(spec.view) + ".png");
        ASSERT_TRUE(CaptureUiPng(ui, context.window(), output)) << output.string();
        ASSERT_GT(fs::file_size(output), 128u);
        int width = 0, height = 0;
        glfwGetFramebufferSize(context.window(), &width, &height);
        const auto layout = Luminumbra::Client::UI::CollectUiLayout(
            ui.GetContext(), nullptr, spec.view, width, height);
        const auto required =
            std::find_if(layout.rml_elements.begin(),
                         layout.rml_elements.end(),
                         [&](const auto& element) { return element.id == spec.required_id; });
        ASSERT_NE(required, layout.rml_elements.end()) << spec.view;
        EXPECT_GT(required->rect.w, 0);
        EXPECT_GT(required->rect.h, 0);
        EXPECT_GE(required->rect.x, 0);
        EXPECT_GE(required->rect.y, 0);
        EXPECT_LE(required->rect.x + required->rect.w, width);
        EXPECT_LE(required->rect.y + required->rect.h, height);
        if (std::string(spec.view) == "world_creation") {
            const auto input =
                std::find_if(layout.rml_elements.begin(),
                             layout.rml_elements.end(),
                             [](const auto& element) { return element.id == "world_name"; });
            ASSERT_NE(input, layout.rml_elements.end());
            EXPECT_EQ(input->text, "New World");
        }
        const auto bytes = Luminumbra::Client::UI::SerializeUiLayout(layout);
        ui.Render();
        EXPECT_EQ(bytes,
                  Luminumbra::Client::UI::SerializeUiLayout(Luminumbra::Client::UI::CollectUiLayout(
                      ui.GetContext(), nullptr, spec.view, width, height)));
        auto sidecar = output;
        sidecar.replace_extension(".layout.json");
        ASSERT_TRUE(Luminumbra::Rendering::WriteCaptureText(sidecar, bytes));
        EXPECT_EQ(ReadTextFile(sidecar), bytes);
        captures.emplace_back(spec.view, output);
    }
    WriteUiScreenshotArtifact(ArtifactRoot() / "ui_screenshots.json", captures);
}

class ScopedLayoutImGuiContext {
public:
    ScopedLayoutImGuiContext()
        : m_previous(ImGui::GetCurrentContext())
        , m_context(ImGui::CreateContext()) {
        ImGui::SetCurrentContext(m_context);
        auto& io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.LogFilename = nullptr;
        io.DisplaySize = ImVec2(800, 600);
        io.DisplayFramebufferScale = ImVec2(2, 1.5f);
        io.DeltaTime = 1.0f / 60.0f;
        unsigned char* pixels = nullptr;
        int width = 0, height = 0;
        io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
    }
    ~ScopedLayoutImGuiContext() {
        ImGui::DestroyContext(m_context);
        ImGui::SetCurrentContext(m_previous);
    }
    ScopedLayoutImGuiContext(const ScopedLayoutImGuiContext&) = delete;
    ScopedLayoutImGuiContext& operator=(const ScopedLayoutImGuiContext&) = delete;

private:
    ImGuiContext* m_previous;
    ImGuiContext* m_context;
};

TEST(UiLayoutCollection, NullContextsAndInvalidFramebufferNeedNoGl) {
    using namespace Luminumbra::Client::UI;
    const auto empty = CollectUiLayout(nullptr, nullptr, "empty", 800, 600);
    EXPECT_EQ(empty.screen, "empty");
    EXPECT_EQ(empty.width, 800);
    EXPECT_EQ(empty.height, 600);
    EXPECT_TRUE(empty.rml_elements.empty());
    EXPECT_TRUE(empty.imgui_windows.empty());
    for (const int dimension : {0, -1}) {
        EXPECT_THROW(CollectUiLayout(nullptr, nullptr, "empty", dimension, 600),
                     std::invalid_argument);
        EXPECT_THROW(CollectUiLayout(nullptr, nullptr, "empty", 800, dimension),
                     std::invalid_argument);
    }
    EXPECT_THROW(
        CollectUiLayout(nullptr, nullptr, std::string(kUiLayoutMaxStringBytes + 1, 'x'), 800, 600),
        std::length_error);
}

TEST(UiLayoutCollection, ImGuiWindowsUseDisplayOrderAndFramebufferScaleWithoutGl) {
    const ScopedLayoutImGuiContext context;
    ImGui::NewFrame();
    // Create the tooltip first: its layer must still paint after ordinary windows.
    ImGui::BeginTooltip();
    ImGui::TextUnformatted("layout tooltip");
    ImGui::EndTooltip();
    ImGui::SetNextWindowPos(ImVec2(10, 20), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(200, 100), ImGuiCond_Always);
    ImGui::Begin("layout_first", nullptr, ImGuiWindowFlags_NoSavedSettings);
    ImGui::BeginChild("layout_child", ImVec2(80, 40));
    ImGui::EndChild();
    ImGui::End();
    ImGui::SetNextWindowPos(ImVec2(40, 60), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(150, 90), ImGuiCond_Always);
    ImGui::SetNextWindowCollapsed(true, ImGuiCond_Always);
    ImGui::Begin("layout_second", nullptr, ImGuiWindowFlags_NoSavedSettings);
    ImGui::End();
    ImGui::Render();
    using namespace Luminumbra::Client::UI;
    const auto layout = CollectUiLayout(nullptr, ImGui::GetCurrentContext(), "imgui", 1600, 900);
    const auto find = [&](const std::string& title) {
        return std::find_if(layout.imgui_windows.begin(),
                            layout.imgui_windows.end(),
                            [&](const auto& window) { return window.title == title; });
    };
    const auto first = find("layout_first");
    const auto second = find("layout_second");
    const auto child = std::find_if(
        layout.imgui_windows.begin(), layout.imgui_windows.end(), [](const auto& window) {
            return window.title.find("layout_child") != std::string::npos;
        });
    const auto tooltip =
        std::find_if(layout.imgui_windows.begin(),
                     layout.imgui_windows.end(),
                     [](const auto& window) { return window.title.starts_with("##Tooltip"); });
    ASSERT_NE(first, layout.imgui_windows.end());
    ASSERT_NE(second, layout.imgui_windows.end());
    ASSERT_NE(child, layout.imgui_windows.end());
    ASSERT_NE(tooltip, layout.imgui_windows.end());
    EXPECT_LT(first, second);
    EXPECT_LT(first, child);
    EXPECT_LT(child, second);
    EXPECT_LT(second, tooltip);
    EXPECT_DOUBLE_EQ(first->rect.x, 20);
    EXPECT_DOUBLE_EQ(first->rect.y, 30);
    EXPECT_DOUBLE_EQ(first->rect.w, 400);
    EXPECT_DOUBLE_EQ(first->rect.h, 150);
    EXPECT_FALSE(first->collapsed);
    EXPECT_TRUE(first->visible);
    EXPECT_TRUE(second->collapsed);
    EXPECT_TRUE(second->visible);
    EXPECT_EQ(SerializeUiLayout(layout),
              SerializeUiLayout(
                  CollectUiLayout(nullptr, ImGui::GetCurrentContext(), "imgui", 1600, 900)));
    // Unsubmitted windows remain in the current context and must not be filtered out.
    ImGui::NewFrame();
    ImGui::Render();
    EXPECT_EQ(CollectUiLayout(nullptr, ImGui::GetCurrentContext(), "imgui", 1600, 900)
                  .imgui_windows.size(),
              layout.imgui_windows.size());
}

TEST(UiLayoutCollection, ImGuiNavigationAndHiddenWindowsUseCurrentContextStateWithoutGl) {
    const ScopedLayoutImGuiContext context;
    ImGui::NewFrame();
    for (const auto* title : {"layout_back", "layout_front"}) {
        ImGui::SetNextWindowSize(ImVec2(200, 100), ImGuiCond_Always);
        ImGui::Begin(title, nullptr, ImGuiWindowFlags_NoSavedSettings);
        ImGui::End();
    }
    ImGui::Render();
    auto* imgui = ImGui::GetCurrentContext();
    auto* back = ImGui::FindWindowByName("layout_back");
    auto* front = ImGui::FindWindowByName("layout_front");
    ASSERT_NE(back, nullptr);
    ASSERT_NE(front, nullptr);
    using namespace Luminumbra::Client::UI;
    const auto ordered_before =
        [](const UiLayout& layout, const std::string& left, const std::string& right) {
            const auto find = [&](const std::string& title) {
                return std::find_if(layout.imgui_windows.begin(),
                                    layout.imgui_windows.end(),
                                    [&](const auto& window) { return window.title == title; });
            };
            const auto first = find(left);
            const auto second = find(right);
            return first != layout.imgui_windows.end() && second != layout.imgui_windows.end() &&
                   first < second;
        };
    imgui->NavWindowingTarget = back;
    EXPECT_TRUE(ordered_before(
        CollectUiLayout(nullptr, imgui, "nav", 1600, 900), "layout_front", "layout_back"));
    imgui->NavWindowingListWindow = front;
    EXPECT_TRUE(ordered_before(
        CollectUiLayout(nullptr, imgui, "nav", 1600, 900), "layout_back", "layout_front"));
    imgui->NavWindowingListWindow = nullptr;
    back->Flags |= ImGuiWindowFlags_NoBringToFrontOnFocus;
    EXPECT_TRUE(ordered_before(
        CollectUiLayout(nullptr, imgui, "nav", 1600, 900), "layout_back", "layout_front"));
    front->Hidden = true;
    const auto hidden = CollectUiLayout(nullptr, imgui, "hidden", 1600, 900);
    const auto retained =
        std::find_if(hidden.imgui_windows.begin(),
                     hidden.imgui_windows.end(),
                     [](const auto& window) { return window.title == "layout_front"; });
    ASSERT_NE(retained, hidden.imgui_windows.end());
    EXPECT_FALSE(retained->visible);
    imgui->NavWindowingTarget = nullptr;
    front->Hidden = false;
}

TEST(UiSmokeTest, LayoutIncludesVisibleDocumentsDirectTextClipsAndTree) {
    HiddenGlContext context;
    if (!context.ready()) {
        GTEST_SKIP() << context.error();
    }
    const fs::path source_root = SourceRoot();
    Luminumbra::Client::Rml_UIManager ui((source_root.string() + "/"));
    ui.Init(context.window(), nullptr);
    // Shut RmlUi down before its interfaces and the GL context, even on ASSERT failure.
    const ScopedUiShutdown shutdown(ui);
    ASSERT_NE(ui.GetContext(), nullptr);
    auto* first = ui.GetContext()->LoadDocumentFromMemory(R"(
        <rml><head><style>
        body, div { display: block; }
        body { width: 700px; height: 500px; font-family: Lora; font-size: 16px; }
        #clip { position: absolute; left: 10px; top: 20px;
                width: 100px; height: 80px; overflow: hidden; }
        #text { width: 120px; height: 90px; }
        input { display: inline-block; width: 180px; height: 24px; }
        </style></head><body id="first">
        <div id="clip"><div id="text">  before <span>nested</span> after  </div></div>
        <div id="hidden" style="display: none"><div id="hidden_child" /></div>
        <div id="invisible" style="visibility: hidden" />
        <div id="unclipped" style="position: absolute; left: 200px; top: 20px;
                                   width: 30px; height: 40px; z-index: 7" />
        <input id="input" type="text" value="New World" />
        <input id="empty_input" type="text" value="" />
        </body></rml>)");
    auto* second = ui.GetContext()->LoadDocumentFromMemory(R"(
        <rml><head><style>
        body, div { display: block; }
        body { width: 600px; height: 400px; font-family: Lora; font-size: 16px; }
        </style></head>
        <body id="second"><div id="second_child" /></body></rml>)");
    // RmlUi initializes document visibility in ProcessHeader, on closing <head>.
    auto* hidden_document = ui.GetContext()->LoadDocumentFromMemory(
        "<rml><head></head><body id=\"hidden_document\">"
        "<div id=\"hidden_document_child\" style=\"visibility: visible\" /></body></rml>");
    ASSERT_NE(first, nullptr);
    ASSERT_NE(second, nullptr);
    ASSERT_NE(hidden_document, nullptr);
    EXPECT_FALSE(hidden_document->IsVisible());
    auto* input_element = first->GetElementById("input");
    ASSERT_NE(input_element, nullptr);
    ASSERT_EQ(input_element->GetNumChildren(), 0);
    ASSERT_EQ(input_element->GetNumChildren(true), 3);
    // The text, selected text and selection nodes are all non-DOM children.
    for (int index = 0; index < input_element->GetNumChildren(true); ++index) {
        auto* child = input_element->GetChild(index);
        ASSERT_NE(child, nullptr);
        child->SetId("input_child_" + std::to_string(index));
    }
    first->Show();
    second->Show();
    ui.GetContext()->Update();
    ui.Render();
    using namespace Luminumbra::Client::UI;
    int width = 0, height = 0;
    glfwGetFramebufferSize(context.window(), &width, &height);
    const auto layout = CollectUiLayout(ui.GetContext(), nullptr, "fixture", width, height);
    const auto find = [&](const std::string& id) {
        return std::find_if(layout.rml_elements.begin(),
                            layout.rml_elements.end(),
                            [&](const auto& element) { return element.id == id; });
    };
    for (const auto* id :
         {"hidden", "hidden_child", "invisible", "hidden_document", "hidden_document_child"})
        EXPECT_EQ(find(id), layout.rml_elements.end()) << id;
    const auto text = find("text");
    const auto clip = find("clip");
    const auto unclipped = find("unclipped");
    const auto second_child = find("second_child");
    const auto input = find("input");
    const auto empty_input = find("empty_input");
    ASSERT_NE(text, layout.rml_elements.end());
    ASSERT_NE(clip, layout.rml_elements.end());
    ASSERT_NE(unclipped, layout.rml_elements.end());
    ASSERT_NE(second_child, layout.rml_elements.end());
    ASSERT_NE(input, layout.rml_elements.end());
    ASSERT_NE(empty_input, layout.rml_elements.end());
    EXPECT_EQ(input->text, "New World");
    EXPECT_TRUE(empty_input->text.empty());
    const auto input_index = static_cast<std::size_t>(input - layout.rml_elements.begin());
    for (int index = 0; index < input_element->GetNumChildren(true); ++index) {
        const auto child = find("input_child_" + std::to_string(index));
        ASSERT_NE(child, layout.rml_elements.end());
        EXPECT_EQ(child->parent, std::optional<std::size_t>(input_index));
        EXPECT_EQ(child->depth, input->depth + 1);
        EXPECT_EQ(static_cast<std::size_t>(child - layout.rml_elements.begin()),
                  input_index + 1 + static_cast<std::size_t>(index));
    }
    EXPECT_EQ(find("input_child_0")->text, "New World");
    EXPECT_LT(text, second_child) << "context document order";
    EXPECT_EQ(text->text, "before  after");
    EXPECT_EQ(text->tag, "div");
    ASSERT_TRUE(text->parent.has_value());
    EXPECT_EQ(
        text->parent,
        std::optional<std::size_t>(static_cast<std::size_t>(clip - layout.rml_elements.begin())));
    EXPECT_EQ(text->depth, clip->depth + 1);
    ASSERT_TRUE(text->clip.has_value());
    const auto clip_rect = text->clip.value_or(UiLayoutRect{});
    EXPECT_DOUBLE_EQ(clip_rect.x, 10);
    EXPECT_DOUBLE_EQ(clip_rect.y, 20);
    EXPECT_DOUBLE_EQ(clip_rect.w, 100);
    EXPECT_DOUBLE_EQ(clip_rect.h, 80);
    EXPECT_FALSE(text->clip_masked);
    EXPECT_FALSE(unclipped->clip.has_value());
    EXPECT_FALSE(unclipped->z.has_value()) << "raw CSS z-index is not effective paint order";
    for (const auto& element : layout.rml_elements) {
        EXPECT_GE(element.rect.x, 0);
        EXPECT_GE(element.rect.y, 0);
        EXPECT_LE(element.rect.x + element.rect.w, width);
        EXPECT_LE(element.rect.y + element.rect.h, height);
    }
    const auto bytes = SerializeUiLayout(layout);
    ui.Render();
    EXPECT_EQ(
        bytes,
        SerializeUiLayout(CollectUiLayout(ui.GetContext(), nullptr, "fixture", width, height)));
    auto* clipping_element = first->GetElementById("clip");
    ASSERT_NE(clipping_element, nullptr);
    clipping_element->SetProperty("border-radius", "8px");
    ui.GetContext()->Update();
    ui.Render();
    const auto masked = CollectUiLayout(ui.GetContext(), nullptr, "masked", width, height);
    const auto masked_text = std::find_if(masked.rml_elements.begin(),
                                          masked.rml_elements.end(),
                                          [](const auto& element) { return element.id == "text"; });
    ASSERT_NE(masked_text, masked.rml_elements.end());
    EXPECT_TRUE(masked_text->clip_masked);
    EXPECT_TRUE(masked_text->clip.has_value());
    clipping_element->SetProperty("transform", "translateX(5px)");
    ui.GetContext()->Update();
    ui.Render();
    const auto transformed = CollectUiLayout(ui.GetContext(), nullptr, "masked", width, height);
    const auto transformed_text =
        std::find_if(transformed.rml_elements.begin(),
                     transformed.rml_elements.end(),
                     [](const auto& element) { return element.id == "text"; });
    ASSERT_NE(transformed_text, transformed.rml_elements.end());
    EXPECT_TRUE(transformed_text->clip_masked);
    EXPECT_FALSE(transformed_text->clip.has_value()) << "transformed mask has no scissor";
    // The context also owns debugger documents, including its visible render hook.
    // Hide every document before requiring an empty sidecar; Hide updates visibility immediately.
    // Snapshot the fixture's documents because hiding can reorder them through focus changes.
    std::vector<Rml::ElementDocument*> documents;
    const int document_count = ui.GetContext()->GetNumDocuments();
    for (int index = 0; index < document_count; ++index) {
        auto* document = ui.GetContext()->GetDocument(index);
        ASSERT_NE(document, nullptr);
        documents.push_back(document);
    }
    for (auto* document : documents) {
        document->Hide();
        EXPECT_FALSE(document->IsVisible()) << document->GetId();
    }
    const auto empty = CollectUiLayout(ui.GetContext(), nullptr, "hidden", width, height);
    std::ostringstream remaining_ids;
    for (const auto& element : empty.rml_elements)
        remaining_ids << " [" << element.id << "]";
    EXPECT_TRUE(empty.rml_elements.empty()) << "Remaining element ids:" << remaining_ids.str();
}

static Luminumbra::Persistence::SavedWorldCatalog SavedWorldFixture() {
    Luminumbra::Persistence::SavedWorldCatalog catalog;
    Luminumbra::Persistence::SavedWorld world;
    world.metadata.worldId = "interaction_fixture";
    world.metadata.name = "Interaction Test World";
    world.metadata.worldType = "default";
    world.metadata.seed = "424242";
    catalog.worlds.push_back(world);
    return catalog;
}

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
        {"main_menu.rml",
         {"main_menu",
          "new_world_btn",
          "load_world_btn",
          "settings_btn",
          "quit_btn",
          "notification",
          "notification_text"}},
        {"world_creation.rml",
         {"world_name", "world_seed", "world_type", "back_btn", "create_btn"}},
        {"world_selection.rml",
         {"world_selection",
          "world_list_items",
          "world_list_status",
          "no_worlds",
          "back_btn",
          "load_selected_btn",
          "new_world_btn"}},
        {"settings.rml",
         {"settings",
          "setting_resolution",
          "setting_window_mode",
          "setting_vsync",
          "setting_fov",
          "setting_mouse_sensitivity",
          "setting_audio_master",
          "setting_audio_sfx",
          "setting_audio_music",
          "apply_settings_btn",
          "back_btn",
          "vsync_toggle",
          "perf_hud_toggle",
          "open_developer_console",
          "developer_help_btn",
          "developer_help",
          "window_mode_prev",
          "window_mode_next",
          "window_mode_value"}},
        {"pause.rml", {"pause", "resume_btn", "settings_btn", "gallery_btn", "quit_menu_btn"}},
        {"gallery.rml", {"gallery", "back_btn"}},
        {"hud.rml", {"hud"}},
        {"photo_mode.rml", {"photo_mode"}},
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

    const std::string manager_source =
        ReadTextFile(source_root / "src/luminumbra_client/ui/Rml_UIManager.cpp");
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

    WriteUiArtifact(ArtifactRoot() / "ui_smoke.json",
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

TEST(UiSmokeTest, ComponentLayerOwnsListenersAndImplementsInteractiveState) {
    HiddenGlContext context;
    if (!context.ready()) {
        GTEST_SKIP() << context.error();
    }

    const fs::path source_root = SourceRoot();
    Luminumbra::Client::Rml_UIManager ui((source_root.string() + "/"));
    ui.Init(context.window(), nullptr);
    ASSERT_NE(ui.GetContext(), nullptr);

    Rml::ElementDocument* document = ui.GetContext()->LoadDocumentFromMemory(R"(
        <rml><head><style>body { font-family: Lora; }</style></head><body>
          <button id="component_button"></button>
          <div><input id="component_input" type="text" /></div>
          <div id="component_panel"><p>original</p></div>
          <div>
            <div id="loading_worlds"></div><div id="no_worlds"></div>
            <div id="component_world_list"><div id="world_list_items"></div></div>
          </div>
        </body></rml>)");
    ASSERT_NE(document, nullptr);
    document->Show();
    ui.GetContext()->Update();

    using Luminumbra::Client::UI::Button;
    using Luminumbra::Client::UI::Input;
    using Luminumbra::Client::UI::Panel;
    using Luminumbra::Client::UI::Property;
    using Luminumbra::Client::UI::WorldInfo;
    using Luminumbra::Client::UI::WorldList;

    int clicks = 0;
    Button button("component_button");
    button.SetText("Create");
    button.SetStyle(Button::Style::Accent);
    button.SetSize(Button::Size::Large);
    button.SetClickHandler([&clicks]() { ++clicks; });
    button.Initialize(document);
    ASSERT_TRUE(button.IsValid());
    EXPECT_TRUE(button.HasClass("btn-accent"));
    EXPECT_TRUE(button.HasClass("btn-lg"));
    button.SetText("<em>not markup</em>");
    EXPECT_EQ(button.GetElement()->QuerySelector("em"), nullptr);
    button.GetElement()->Click();
    EXPECT_EQ(clicks, 1);
    button.SetEnabled(false);
    button.GetElement()->Click();
    EXPECT_EQ(clicks, 1);
    button.Destroy();
    document->GetElementById("component_button")->Click();
    EXPECT_EQ(clicks, 1) << "destroyed component left a live event callback";

    Input input("component_input");
    input.SetType(Input::Type::Email);
    input.SetRequired(true);
    input.SetValidationMessage("Enter a valid email");
    input.Initialize(document);
    input.SetValue("invalid");
    EXPECT_FALSE(input.Validate());
    EXPECT_NE(document->GetElementById("component_input_error"), nullptr);
    input.SetValue("player@example.com");
    EXPECT_TRUE(input.Validate());

    Property<bool> expanded{true};
    Panel panel("component_panel");
    panel.SetTitle("Details");
    panel.SetCollapsible(true);
    panel.BindExpanded(expanded);
    panel.Initialize(document);
    ASSERT_NE(document->GetElementById("component_panel_toggle"), nullptr);
    panel.SetTitle("<em>Details</em>");
    EXPECT_EQ(document->GetElementById("component_panel_title")->QuerySelector("em"), nullptr);
    document->GetElementById("component_panel_toggle")->Click();
    EXPECT_FALSE(panel.IsExpanded());
    EXPECT_FALSE(expanded.Get());

    const auto today = std::chrono::year_month_day{
        std::chrono::floor<std::chrono::days>(std::chrono::system_clock::now())};
    std::ostringstream todayText;
    todayText << static_cast<int>(today.year()) << '-' << std::setfill('0') << std::setw(2)
              << static_cast<unsigned>(today.month()) << '-' << std::setw(2)
              << static_cast<unsigned>(today.day());

    WorldList worlds("component_world_list");
    worlds.Initialize(document);
    worlds.SetWorlds({
        WorldInfo{"a", "<em>Alpha</em>", "default", "1", todayText.str(), "2026-01-01", 1024, true},
        WorldInfo{"b", "Beta", "mountains", "2", "2020-01-01", "2020-01-01", 2048, false},
    });
    EXPECT_EQ(worlds.GetWorldCount(), 2u);
    EXPECT_EQ(document->GetElementById("world_list_items")->QuerySelector("em"), nullptr);
    worlds.ShowFavoritesOnly(true);
    EXPECT_EQ(worlds.GetFilteredWorldCount(), 1u);
    worlds.ShowFavoritesOnly(false);
    worlds.ShowRecentOnly(true);
    EXPECT_EQ(worlds.GetFilteredWorldCount(), 1u);
    worlds.SetLoading(true);
    ui.GetContext()->Update();
    EXPECT_EQ(document->GetElementById("loading_worlds")->GetDisplay(), Rml::Style::Display::Block);
    worlds.SetLoading(false);

    ui.Shutdown();
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
        std::vector<Luminumbra::Client::WorldGenParam> params;
    };

    std::optional<CreatedWorld> created_world;
    std::optional<std::string> loaded_world_id;
    ui.SetWorldCreationCallback([&](const std::string& name,
                                    const std::string& seed,
                                    const std::string& type,
                                    const std::vector<Luminumbra::Client::WorldGenParam>& params) {
        created_world = CreatedWorld{name, seed, type, params};
    });
    ui.SetLoadWorldCallback([&](const std::string& world_id) { loaded_world_id = world_id; });
    ui.SetSavedWorldList(SavedWorldFixture);

    int navigation_clicks_checked = 0;
    int form_fields_checked = 0;

    Rml::ElementDocument* main_menu = LoadDocumentAndFind(ui, "main_menu.rml", "main_menu");
    ASSERT_NE(main_menu, nullptr);
    ClickAndUpdate(ui, main_menu->GetElementById("new_world_btn"));
    ++navigation_clicks_checked;

    Rml::ElementDocument* world_creation =
        FindDocumentByElementId(ui.GetContext(), "world_creation");
    ASSERT_NE(world_creation, nullptr);
    SetControlValue(world_creation, "world_name", "Interaction Test World");
    SetControlValue(world_creation, "world_seed", "424242");
    SetControlValue(world_creation, "world_type", "mountains");
    form_fields_checked += 3;
    ClickAndUpdate(ui, world_creation->GetElementById("create_btn"));
    ASSERT_TRUE(created_world.has_value());
    if (!created_world.has_value())
        return;
    EXPECT_EQ(created_world->name, "Interaction Test World");
    EXPECT_EQ(created_world->seed, "424242");
    EXPECT_EQ(created_world->type, "mountains");

    ClickAndUpdate(ui, world_creation->GetElementById("back_btn"));
    ++navigation_clicks_checked;
    main_menu = FindDocumentByElementId(ui.GetContext(), "main_menu");
    ASSERT_NE(main_menu, nullptr);

    ClickAndUpdate(ui, main_menu->GetElementById("load_world_btn"));
    ++navigation_clicks_checked;
    Rml::ElementDocument* world_selection =
        FindDocumentByElementId(ui.GetContext(), "world_selection");
    ASSERT_NE(world_selection, nullptr);
    Rml::Element* first_world = FirstWorldListItem(world_selection);
    ASSERT_NE(first_world, nullptr);
    const std::string expected_world_id =
        first_world->GetAttribute<Rml::String>("data-world-id", "");
    ASSERT_FALSE(expected_world_id.empty());
    ClickAndUpdate(ui, first_world);
    ++navigation_clicks_checked;
    EXPECT_EQ(first_world->GetAttribute<Rml::String>("data-selected", ""), "true");
    Rml::Element* load_selected = world_selection->GetElementById("load_selected_btn");
    ASSERT_NE(load_selected, nullptr);
    EXPECT_FALSE(load_selected->HasAttribute("disabled"));
    ClickAndUpdate(ui, load_selected);
    ASSERT_TRUE(loaded_world_id.has_value());
    if (!loaded_world_id.has_value())
        return;
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

    WriteUiInteractionArtifact(ArtifactRoot() / "ui_interactions.json",
                               navigation_clicks_checked,
                               form_fields_checked,
                               created_world->name,
                               created_world->seed,
                               created_world->type,
                               *loaded_world_id,
                               quit_requested);

    ui.Shutdown();
}

// settings.rml round-trip: the SettingsBridge getters seed the form on load,
// changing a control pushes through the matching setter live, and Apply & Save
// flushes every control and invokes Save. Reaches the screen via the
// main-menu settings_btn navigation path (the menu "open settings" flow).
TEST(UiSmokeTest, SettingsScreenRoundTripsThroughTheBridge) {
    HiddenGlContext context;
    if (!context.ready()) {
        GTEST_SKIP() << context.error();
    }

    const fs::path source_root = SourceRoot();
    Luminumbra::Client::Rml_UIManager ui((source_root.string() + "/"));
    ui.Init(context.window(), nullptr);
    ASSERT_NE(ui.GetContext(), nullptr);

    // In-memory model the bridge reads from / writes to.
    struct Model {
        std::string resolution = "1920x1080";
        std::string window_mode = "borderless";
        bool vsync = true;
        int frame_cap = 120;
        bool perf_hud = true;
        int console_opens = 0;
        float fov = 45.0f;
        float sensitivity = 0.025f;
        float audio_master = 1.0f;
        float audio_sfx = 1.0f;
        float audio_music = 1.0f;
        int save_count = 0;
    } model;

    Luminumbra::Client::SettingsBridge sb;
    sb.GetResolution = [&] {
        return model.resolution;
    };
    sb.SetResolution = [&](const std::string& v) {
        model.resolution = v;
    };
    sb.GetWindowMode = [&] {
        return model.window_mode;
    };
    sb.SetWindowMode = [&](const std::string& v) {
        model.window_mode = v;
    };
    sb.GetPerfHud = [&] {
        return model.perf_hud;
    };
    sb.SetPerfHud = [&](bool visible) {
        model.perf_hud = visible;
        ++model.save_count; // The real setter mutates the registry and saves immediately.
    };
    sb.OpenDeveloperConsole = [&] {
        ++model.console_opens;
    };
    sb.GetDeveloperHelp = [] {
        return std::string("console.toggle = F1\nshader.reload = F5");
    };
    sb.GetVSync = [&] {
        return model.vsync;
    };
    sb.SetVSync = [&](bool v) {
        model.vsync = v;
    };
    sb.GetFrameCap = [&] {
        return model.frame_cap;
    };
    sb.SetFrameCap = [&](int cap) {
        model.frame_cap = cap;
    };
    sb.GetFov = [&] {
        return model.fov;
    };
    sb.SetFov = [&](float v) {
        model.fov = v;
    };
    sb.GetMouseSensitivity = [&] {
        return model.sensitivity;
    };
    sb.SetMouseSensitivity = [&](float v) {
        model.sensitivity = v;
    };
    sb.GetAudioMaster = [&] {
        return model.audio_master;
    };
    sb.SetAudioMaster = [&](float v) {
        model.audio_master = v;
    };
    sb.GetAudioSfx = [&] {
        return model.audio_sfx;
    };
    sb.SetAudioSfx = [&](float v) {
        model.audio_sfx = v;
    };
    sb.GetAudioMusic = [&] {
        return model.audio_music;
    };
    sb.SetAudioMusic = [&](float v) {
        model.audio_music = v;
    };
    sb.Save = [&] {
        ++model.save_count;
        return true;
    };
    ui.SetSettingsBridge(std::move(sb));

    // Open the settings screen via the main-menu "Settings" button.
    Rml::ElementDocument* main_menu = LoadDocumentAndFind(ui, "main_menu.rml", "main_menu");
    ASSERT_NE(main_menu, nullptr);
    ClickAndUpdate(ui, main_menu->GetElementById("settings_btn"));

    Rml::ElementDocument* settings = FindDocumentByElementId(ui.GetContext(), "settings");
    ASSERT_NE(settings, nullptr) << "settings_btn must navigate to settings.rml";

    // PopulateSettingsForm: every control is seeded from the bridge getters.
    auto control_value = [&](const char* id) -> std::string {
        Rml::Element* el = settings->GetElementById(id);
        auto* control = dynamic_cast<Rml::ElementFormControl*>(el);
        return control ? std::string(control->GetValue()) : std::string();
    };
    EXPECT_EQ(control_value("setting_resolution"), "1920x1080");
    EXPECT_EQ(control_value("setting_window_mode"), "borderless");
    EXPECT_EQ(control_value("setting_vsync"), "on");
    EXPECT_EQ(control_value("setting_frame_cap"), "120");
    EXPECT_EQ(std::stof(control_value("setting_fov")), 45.0f);
    EXPECT_NEAR(std::stof(control_value("setting_mouse_sensitivity")), 0.025f, 1e-4f);

    // Live-apply: change a control and dispatch "change"; the matching setter fires.
    auto change_control = [&](const char* id, const std::string& value) {
        Rml::Element* el = settings->GetElementById(id);
        ASSERT_NE(el, nullptr) << id;
        auto* control = dynamic_cast<Rml::ElementFormControl*>(el);
        ASSERT_NE(control, nullptr) << id;
        control->SetValue(value);
        Rml::Dictionary params;
        el->DispatchEvent(Rml::EventId::Change, params);
        ui.Update();
    };

    change_control("setting_window_mode", "fullscreen");
    EXPECT_EQ(model.window_mode, "fullscreen");

    change_control("setting_vsync", "invalid");
    EXPECT_TRUE(model.vsync);
    change_control("setting_vsync", "off");
    EXPECT_FALSE(model.vsync);

    for (const int cap : {30, 60, 120, 144, 165, 240, 0}) {
        change_control("setting_frame_cap", std::to_string(cap));
        EXPECT_EQ(model.frame_cap, cap);
        EXPECT_EQ(control_value("setting_frame_cap"), std::to_string(cap));
    }
    change_control("setting_frame_cap", "144");
    change_control("setting_frame_cap", "61");
    EXPECT_EQ(model.frame_cap, 144);

    // UI edits update the saved choices while each measurement mode keeps
    // presentation uncapped. Leaving measurement uses the latest live edits.
    using luminumbra::core::FramePacingOverrides;
    for (const auto overrides :
         {FramePacingOverrides{
              .benchmark_active = true, .scenario_active = false, .profiling_active = false},
          FramePacingOverrides{
              .benchmark_active = false, .scenario_active = true, .profiling_active = false},
          FramePacingOverrides{
              .benchmark_active = false, .scenario_active = false, .profiling_active = true}}) {
        for (const bool vsync : {true, false}) {
            change_control("setting_vsync", vsync ? "on" : "off");
            change_control("setting_frame_cap", "240");
            EXPECT_EQ(model.vsync, vsync);
            EXPECT_EQ(model.frame_cap, 240);
            const auto pacing =
                luminumbra::core::ResolveFramePacing(model.vsync, model.frame_cap, overrides);
            EXPECT_FALSE(pacing.vsync);
            EXPECT_EQ(pacing.frame_cap, 0);
            const auto restored =
                luminumbra::core::ResolveFramePacing(model.vsync, model.frame_cap);
            EXPECT_EQ(restored.vsync, vsync);
            EXPECT_EQ(restored.frame_cap, 240);
        }
    }

    change_control("setting_fov", "90");
    EXPECT_FLOAT_EQ(model.fov, 90.0f);

    change_control("setting_mouse_sensitivity", "0.5");
    EXPECT_NEAR(model.sensitivity, 0.5f, 1e-4f);

    change_control("setting_audio_master", "0.4");
    EXPECT_NEAR(model.audio_master, 0.4f, 1e-4f);

    // Apply & Save flushes every control then persists via Save.
    EXPECT_EQ(model.save_count, 0);
    ClickAndUpdate(ui, settings->GetElementById("apply_settings_btn"));
    EXPECT_EQ(model.save_count, 1) << "Apply & Save must invoke the bridge Save()";

    auto* perf_toggle = settings->GetElementById("perf_hud_toggle");
    ASSERT_NE(perf_toggle, nullptr);
    EXPECT_TRUE(perf_toggle->IsClassSet("on"));
    ClickAndUpdate(ui, perf_toggle);
    EXPECT_FALSE(model.perf_hud);
    EXPECT_FALSE(perf_toggle->IsClassSet("on"));
    EXPECT_EQ(model.save_count, 2);
    settings = LoadDocumentAndFind(ui, "settings.rml", "settings");
    ASSERT_NE(settings, nullptr);
    perf_toggle = settings->GetElementById("perf_hud_toggle");
    ASSERT_NE(perf_toggle, nullptr);
    EXPECT_FALSE(perf_toggle->IsClassSet("on"));
    ClickAndUpdate(ui, settings->GetElementById("open_developer_console"));
    EXPECT_EQ(model.console_opens, 1);
    auto* help = settings->GetElementById("developer_help");
    ASSERT_NE(help, nullptr);
    EXPECT_NE(help->GetInnerRML().find("console.toggle = F1"), std::string::npos);
    EXPECT_EQ(help->GetDisplay(), Rml::Style::Display::None);
    ClickAndUpdate(ui, settings->GetElementById("developer_help_btn"));
    EXPECT_FALSE(help->IsClassSet("hidden"));
    EXPECT_EQ(help->GetDisplay(), Rml::Style::Display::Block);
    ClickAndUpdate(ui, settings->GetElementById("developer_help_btn"));
    EXPECT_TRUE(help->IsClassSet("hidden"));
    EXPECT_EQ(help->GetDisplay(), Rml::Style::Display::None);
    // A change made in the hub refreshes the ordinary settings value from the same getter.
    model.perf_hud = true;
    ui.RefreshDeveloperSettings();
    EXPECT_TRUE(perf_toggle->IsClassSet("on"));

    ui.Shutdown();
}

TEST(UiSmokeTest, GraphicsControlsUseOneBridgeAndShowSessionValuesAndFailures) {
    using namespace Luminumbra::Client;
    using namespace luminumbra::core;
    HiddenGlContext context;
    ASSERT_TRUE(context.ready()) << context.error();
    Rml_UIManager ui(SourceRoot().string() + "/");
    ui.Init(context.window(), nullptr);
    ASSERT_NE(ui.GetContext(), nullptr);
    GraphicsSettingsState state({}, {{"render_scale", 0.67}});
    int edits = 0;
    SettingsBridge bridge;
    bridge.GetGraphics = [&] {
        return state.Display();
    };
    bridge.EditGraphics = [&](const GraphicsEdit& edit) {
        ++edits;
        if (edit.kind == GraphicsEditKind::DetectApply) {
            GraphicsHardwareInputs inputs;
            inputs.renderer = "<em>unknown GPU</em>";
            EXPECT_TRUE(state.Stage(edit, inputs));
        } else {
            EXPECT_TRUE(state.Stage(edit));
        }
        state.Applied(true, 0.0);
    };
    bridge.SaveGraphics = [&] {
        state.Saved(false);
    };
    bridge.Save = [] {
        return false;
    };
    ui.SetSettingsBridge(std::move(bridge));
    auto* settings = LoadDocumentAndFind(ui, "settings.rml", "settings");
    ASSERT_NE(settings, nullptr);
    auto* scale = settings->GetElementById("graphics_render_scale");
    ASSERT_NE(scale, nullptr);
    ASSERT_NE(dynamic_cast<Rml::ElementFormControl*>(scale), nullptr);
    EXPECT_NEAR(
        std::stof(dynamic_cast<Rml::ElementFormControl*>(scale)->GetValue()), 0.67f, 0.001f);
    EXPECT_TRUE(scale->HasAttribute("disabled"));
    EXPECT_EQ(settings->GetElementById("graphics_render_scale_session")->GetInnerRML(),
              " (session override)");

    const auto change = [&](const char* id, const char* value) {
        auto* element = settings->GetElementById(id);
        ASSERT_NE(element, nullptr);
        auto* control = dynamic_cast<Rml::ElementFormControl*>(element);
        ASSERT_NE(control, nullptr);
        control->SetValue(value);
        Rml::Dictionary params;
        element->DispatchEvent(Rml::EventId::Change, params);
        ui.Update();
    };
    change("graphics_render_scale", "0.5");
    EXPECT_EQ(edits, 0); // Session controls cannot write their effective values to saved choices.
    change("graphics_preset", "high");
    EXPECT_EQ(state.Settings().base_preset, GraphicsPreset::High);
    EXPECT_TRUE(state.Settings().overrides.empty());
    change("graphics_grass_density", "0.5");
    EXPECT_NE(settings->GetElementById("graphics_state")->GetInnerRML().find("Custom (base: High)"),
              std::string::npos);
    EXPECT_DOUBLE_EQ(std::get<double>(state.Settings().overrides.at("grass_density")), 0.5);
    change("graphics_grass_fade_start_m", "30");
    change("graphics_grass_fade_end_m", "60");
    change("graphics_far_terrain_radius_m", "1500");
    change("graphics_shadow_resolution", "1024");
    change("graphics_volumetric_quality", "0");
    change("graphics_grass_enabled", "off");
    change("graphics_ui_blur", "half");
    EXPECT_EQ(state.Settings().overrides.size(), 8u);
    EXPECT_DOUBLE_EQ(state.Display().effective.values.far_outer_clip_m, 1550.0);
    EXPECT_EQ(state.Display().effective.values.shadow_resolution, 1024);
    EXPECT_FALSE(state.Display().effective.values.grass_enabled);
    EXPECT_EQ(state.Display().effective.values.ui_blur, UiBlurQuality::Half);
    ClickAndUpdate(ui, settings->GetElementById("graphics_reset"));
    EXPECT_EQ(state.Settings().base_preset, GraphicsPreset::High);
    EXPECT_TRUE(state.Settings().overrides.empty());
    ClickAndUpdate(ui, settings->GetElementById("graphics_detect"));
    EXPECT_EQ(state.Settings().base_preset, GraphicsPreset::Low);
    EXPECT_TRUE(state.Settings().overrides.empty());
    EXPECT_EQ(settings->GetElementById("graphics_hardware")->QuerySelector("em"), nullptr);
    EXPECT_NE(settings->GetElementById("graphics_hardware")->GetInnerRML().find("Recommended: Low"),
              std::string::npos);
    ClickAndUpdate(ui, settings->GetElementById("graphics_retry_save"));
    EXPECT_EQ(settings->GetElementById("graphics_status")->GetInnerRML(),
              "Save failed; graphics are active for this session");
    EXPECT_TRUE(state.Settings().overrides.empty());
    EXPECT_DOUBLE_EQ(state.Display().effective.values.render_scale, 0.67);
    ClickAndUpdate(ui, settings->GetElementById("back_btn"));
    auto* menu = FindDocumentByElementId(ui.GetContext(), "main_menu");
    ASSERT_NE(menu, nullptr);
    EXPECT_FALSE(menu->GetElementById("notification")->IsClassSet("hidden"));
    EXPECT_EQ(menu->GetElementById("notification_text")->GetInnerRML(),
              "Settings could not be saved. Reopen settings for details.");
    ui.Shutdown();
}

TEST(UiSmokeTest, BackdropClassesFollowLiveQualityAndNewDocumentsWithoutCompounding) {
    using namespace Luminumbra::Client;
    using luminumbra::core::UiBlurQuality;
    HiddenGlContext context;
    ASSERT_TRUE(context.ready()) << context.error();
    Rml_UIManager ui(SourceRoot().string() + "/");
    ui.Init(context.window(), nullptr);
    ui.SetSavedWorldList(SavedWorldFixture);
    for (const auto quality : {UiBlurQuality::Off,
                               UiBlurQuality::Half,
                               UiBlurQuality::Full,
                               UiBlurQuality::Half,
                               UiBlurQuality::Full}) {
        ui.SetBackdropBlurQuality(quality);
        for (const auto& [path, id] : {std::pair{"main_menu.rml", "main_menu"},
                                       std::pair{"settings.rml", "settings"},
                                       std::pair{"world_selection.rml", "world_selection"},
                                       std::pair{"world_creation.rml", "world_creation"},
                                       std::pair{"pause.rml", "pause"}}) {
            auto* document = LoadDocumentAndFind(ui, path, id);
            ASSERT_NE(document, nullptr);
            EXPECT_EQ(document->IsClassSet("backdrop-off"), quality == UiBlurQuality::Off);
            EXPECT_EQ(document->IsClassSet("backdrop-half"), quality == UiBlurQuality::Half);
            ui.SetBackdropBlurQuality(UiBlurQuality::Full);
            EXPECT_FALSE(document->IsClassSet("backdrop-off"));
            EXPECT_FALSE(document->IsClassSet("backdrop-half"));
            ui.SetBackdropBlurQuality(quality);
        }
    }
    ui.Shutdown();
}

TEST(UiSmokeTest, DisabledAudioShowsStatusAndPreservesSavedVolumes) {
    HiddenGlContext context;
    if (!context.ready()) {
        GTEST_SKIP() << context.error();
    }
    Luminumbra::Client::NullAudioManager audio;
    ASSERT_TRUE(audio.Init());
    audio.SetMasterVolume(1.0f);
    Luminumbra::Client::Rml_UIManager ui(SourceRoot().string() + "/");
    ui.Init(context.window(), &audio);
    ASSERT_NE(ui.GetContext(), nullptr);
    int audio_set_calls = 0;
    int save_calls = 0;
    Luminumbra::Client::SettingsBridge bridge;
    bridge.GetAudioMaster = bridge.GetAudioMusic = bridge.GetAudioSfx = [] {
        return 1.0f;
    };
    bridge.SetAudioMaster = bridge.SetAudioMusic = bridge.SetAudioSfx = [&](float) {
        ++audio_set_calls;
    };
    bridge.Save = [&] {
        ++save_calls;
        return true;
    };
    ui.SetSettingsBridge(std::move(bridge));
    auto* menu = LoadDocumentAndFind(ui, "main_menu.rml", "main_menu");
    ASSERT_NE(menu, nullptr);
    ClickAndUpdate(ui, menu->GetElementById("settings_btn"));
    auto* settings = FindDocumentByElementId(ui.GetContext(), "settings");
    ASSERT_NE(settings, nullptr);
    auto* status = settings->GetElementById("setting_audio_status");
    ASSERT_NE(status, nullptr);
    EXPECT_NE(status->GetInnerRML().find("Audio playback disabled or unavailable."),
              std::string::npos);
    for (const char* id : {"setting_audio_master", "setting_audio_music", "setting_audio_sfx"}) {
        auto* control = dynamic_cast<Rml::ElementFormControl*>(settings->GetElementById(id));
        ASSERT_NE(control, nullptr);
        EXPECT_TRUE(control->HasAttribute("disabled"));
        EXPECT_FLOAT_EQ(std::stof(control->GetValue()), 1.0f);
        // Even a programmatic change or Apply & Save cannot edit a disabled level.
        control->SetValue("0.25");
        Rml::Dictionary parameters;
        control->DispatchEvent(Rml::EventId::Change, parameters);
        ui.Update();
    }
    ClickAndUpdate(ui, settings->GetElementById("apply_settings_btn"));
    EXPECT_EQ(audio_set_calls, 0);
    EXPECT_EQ(save_calls, 1);
    EXPECT_FALSE(audio.IsPlaybackEnabled());
    EXPECT_EQ(audio.RecordedRequestCount(), 0u);
    ui.Shutdown();
    audio.Shutdown();
}

// The cinematic redesign's custom widgets are FUNCTIONAL (not just decorative): the vsync
// toggle and window-mode stepper drive the bridge, keybind rows seed from / begin a rebind,
// the create-world preset chips drive the hidden world_type, the customize section expands,
// and worldgen param overrides are collected into the create callback.
TEST(UiSmokeTest, RedesignedControlsAreFunctional) {
    HiddenGlContext context;
    if (!context.ready()) {
        GTEST_SKIP() << context.error();
    }
    const fs::path source_root = SourceRoot();
    Luminumbra::Client::Rml_UIManager ui((source_root.string() + "/"));
    ui.Init(context.window(), nullptr);
    ASSERT_NE(ui.GetContext(), nullptr);

    struct Model {
        bool vsync = true;
        std::string window_mode = "borderless";
        int save_count = 0;
    } model;
    std::string last_rebind_action;
    Luminumbra::Client::SettingsBridge sb;
    sb.GetVSync = [&] {
        return model.vsync;
    };
    sb.SetVSync = [&](bool v) {
        model.vsync = v;
    };
    sb.GetWindowMode = [&] {
        return model.window_mode;
    };
    sb.SetWindowMode = [&](const std::string& v) {
        model.window_mode = v;
    };
    sb.GetKeybind = [&](const std::string& action) -> std::string {
        return action == "Jump" ? "Space" : "?";
    };
    sb.BeginRebind = [&](const std::string& action) {
        last_rebind_action = action;
    };
    sb.Save = [&] {
        ++model.save_count;
        return true;
    };
    ui.SetSettingsBridge(std::move(sb));

    std::optional<std::vector<Luminumbra::Client::WorldGenParam>> created_params;
    std::optional<std::string> created_type;
    ui.SetWorldCreationCallback([&](const std::string&,
                                    const std::string&,
                                    const std::string& type,
                                    const std::vector<Luminumbra::Client::WorldGenParam>& params) {
        created_type = type;
        created_params = params;
    });
    ui.SetWorldParamGetter([&](const std::string&, const std::string& path) -> std::string {
        return path == "terrain.base_amplitude" ? std::string("34") : std::string();
    });

    // --- settings: custom widgets drive the bridge ---
    Rml::ElementDocument* settings = LoadDocumentAndFind(ui, "settings.rml", "settings");
    ASSERT_NE(settings, nullptr);

    // vsync toggle: seeded.on from GetVSync (true); a click flips it and drives SetVSync.
    Rml::Element* vsync_toggle = settings->GetElementById("vsync_toggle");
    ASSERT_NE(vsync_toggle, nullptr);
    EXPECT_TRUE(vsync_toggle->IsClassSet("on")) << "vsync toggle seeds.on from GetVSync";
    ClickAndUpdate(ui, vsync_toggle);
    EXPECT_FALSE(model.vsync);
    EXPECT_FALSE(vsync_toggle->IsClassSet("on"));

    // window-mode stepper: value seeded from GetWindowMode; "next" cycles borderless->fullscreen.
    Rml::Element* wm_value = settings->GetElementById("window_mode_value");
    ASSERT_NE(wm_value, nullptr);
    EXPECT_EQ(wm_value->GetInnerRML(), "borderless");
    ClickAndUpdate(ui, settings->GetElementById("window_mode_next"));
    EXPECT_EQ(model.window_mode, "fullscreen");
    EXPECT_EQ(wm_value->GetInnerRML(), "fullscreen");

    // keybind rows: chip seeded from GetKeybind; clicking the row begins a rebind for that action.
    Rml::ElementList kb_rows;
    settings->GetElementsByClassName(kb_rows, "keybind-rebind");
    ASSERT_FALSE(kb_rows.empty());
    Rml::Element* jump_row = nullptr;
    for (auto* r : kb_rows) {
        if (r->GetAttribute<Rml::String>("data-action", "") == "Jump")
            jump_row = r;
    }
    ASSERT_NE(jump_row, nullptr);
    if (auto* chip = settings->GetElementById("kb_Jump")) {
        EXPECT_EQ(chip->GetInnerRML(), "Space");
    }
    ClickAndUpdate(ui, jump_row);
    EXPECT_EQ(last_rebind_action, "Jump");

    // --- world creation: customize + preset chips + param overrides ---
    Rml::ElementDocument* wc = LoadDocumentAndFind(ui, "world_creation.rml", "world_creation");
    ASSERT_NE(wc, nullptr);

    // amplitude slider seeds from the param getter (34), not its authored default.
    Rml::ElementList wgp;
    wc->GetElementsByClassName(wgp, "worldgen-param");
    ASSERT_FALSE(wgp.empty());
    Rml::Element* amp = nullptr;
    for (auto* el : wgp) {
        if (el->GetAttribute<Rml::String>("data-path", "") == "terrain.base_amplitude")
            amp = el;
    }
    ASSERT_NE(amp, nullptr);
    EXPECT_NEAR(std::stof(dynamic_cast<Rml::ElementFormControl*>(amp)->GetValue()), 34.0f, 0.5f)
        << "amplitude must seed from the param getter (34), not its authored default";

    // customize toggle expands the collapsed body.
    Rml::Element* body = wc->GetElementById("customize_body");
    ASSERT_NE(body, nullptr);
    EXPECT_TRUE(body->IsClassSet("collapsed"));
    ClickAndUpdate(ui, wc->GetElementById("customize_toggle"));
    EXPECT_FALSE(body->IsClassSet("collapsed"));

    // a worldgen-param slider live-updates its SIBLING.param-value label
    // as it moves (the flex [label][slider][value] row restructure must keep the
    // change handler — GetParentNode->.param-value — wired). amp is in the
    // default-active terrain pane.
    {
        auto* amp_fc = dynamic_cast<Rml::ElementFormControl*>(amp);
        ASSERT_NE(amp_fc, nullptr);
        amp_fc->SetValue("77");
        Rml::Dictionary change_params;
        amp->DispatchEvent(Rml::EventId::Change, change_params);
        ui.Update();
        Rml::Element* row = amp->GetParentNode();
        ASSERT_NE(row, nullptr);
        Rml::ElementList vals;
        row->GetElementsByClassName(vals, "param-value");
        ASSERT_FALSE(vals.empty()) << "the value label must be a sibling of the slider";
        EXPECT_NEAR(std::stof(vals[0]->GetInnerRML()), 77.0f, 0.5f)
            << "a param slider must live-update its sibling value label";
    }

    // advanced-param TABS — clicking a tab chip activates ONLY its pane.
    {
        Rml::ElementList tabs;
        wc->GetElementsByClassName(tabs, "param-tab");
        ASSERT_GE(tabs.size(), 4u) << "terrain/water/biomes/features tab chips";
        Rml::Element* water_tab = nullptr;
        for (auto* t : tabs)
            if (t->GetAttribute<Rml::String>("data-tab", "") == "water")
                water_tab = t;
        ASSERT_NE(water_tab, nullptr);
        ClickAndUpdate(ui, water_tab);
        EXPECT_TRUE(water_tab->IsClassSet("active"));
        Rml::ElementList panes;
        wc->GetElementsByClassName(panes, "param-pane");
        ASSERT_GE(panes.size(), 4u);
        int active_panes = 0;
        for (auto* p : panes) {
            const bool is_active = p->IsClassSet("active");
            if (is_active)
                ++active_panes;
            EXPECT_EQ(is_active, p->GetAttribute<Rml::String>("data-pane", "") == "water")
                << "only the clicked tab's pane is active";
        }
        EXPECT_EQ(active_panes, 1) << "exactly one advanced pane shows at a time";
        // Restore the terrain pane so the rest of the test reads the default surface.
        Rml::Element* terrain_tab = nullptr;
        for (auto* t : tabs)
            if (t->GetAttribute<Rml::String>("data-tab", "") == "terrain")
                terrain_tab = t;
        ASSERT_NE(terrain_tab, nullptr);
        ClickAndUpdate(ui, terrain_tab);
        EXPECT_TRUE(terrain_tab->IsClassSet("active"));
    }

    // preset chip drives the hidden world_type select.
    Rml::ElementList chips;
    wc->GetElementsByClassName(chips, "preset-chip");
    Rml::Element* mons = nullptr;
    for (auto* c : chips) {
        if (c->GetAttribute<Rml::String>("data-preset", "") == "mountains")
            mons = c;
    }
    ASSERT_NE(mons, nullptr);
    ClickAndUpdate(ui, mons);
    EXPECT_TRUE(mons->IsClassSet("selected"));
    auto* type_sel = dynamic_cast<Rml::ElementFormControl*>(wc->GetElementById("world_type"));
    ASSERT_NE(type_sel, nullptr);
    EXPECT_EQ(std::string(type_sel->GetValue()), "mountains");

    // override a param then create -> the override reaches the create callback.
    dynamic_cast<Rml::ElementFormControl*>(amp)->SetValue("123");
    ClickAndUpdate(ui, wc->GetElementById("create_btn"));
    ASSERT_TRUE(created_params.has_value());
    ASSERT_TRUE(created_type.has_value());
    if (!created_params.has_value() || !created_type.has_value())
        return;
    EXPECT_EQ(*created_type, "mountains");
    bool found = false;
    for (const auto& p : *created_params) {
        // Range inputs report formatted floats (e.g. "123.000000"); compare numerically.
        if (p.path == "terrain.base_amplitude" && std::abs(std::stof(p.value) - 123.0f) < 0.5f)
            found = true;
    }
    EXPECT_TRUE(found) << "worldgen param override must reach the create callback";

    ui.Shutdown();
}

TEST(UiSmokeTest, CreateThenBackInOneEventPumpCancelsBeforeOpeningWorld) {
    HiddenGlContext context;
    ASSERT_TRUE(context.ready()) << context.error();
    Luminumbra::Client::WorldEntry<std::string> entry;
    Luminumbra::Client::Rml_UIManager ui(SourceRoot().string() + "/");
    ui.Init(context.window(), nullptr);
    ASSERT_NE(ui.GetContext(), nullptr);
    ui.SetNavigationAwayCallback([&] { entry.cancel_input(true); });
    ui.SetWorldCreationCallback([&](const std::string& name,
                                    const std::string&,
                                    const std::string&,
                                    const std::vector<Luminumbra::Client::WorldGenParam>&) {
        EXPECT_TRUE(entry.request(name));
    });
    auto* document = LoadDocumentAndFind(ui, "world_creation.rml", "world_creation");
    ASSERT_NE(document, nullptr);
    auto* create = document->GetElementById("create_btn");
    auto* back = document->GetElementById("back_btn");
    ASSERT_NE(create, nullptr);
    ASSERT_NE(back, nullptr);
    // No host advance or UI update between these two events.
    create->Click();
    ASSERT_EQ(entry.state(), Luminumbra::Client::WorldEntryState::Requested);
    back->Click();
    unsigned stages = 0;
    unsigned opens = 0;
    unsigned drains = 0;
    entry.loading_painted();
    for (std::size_t frame = 0;
         frame <= static_cast<std::size_t>(Luminumbra::Client::WorldEntryStage::Count);
         ++frame) {
        entry.advance(
            [&](Luminumbra::Client::WorldEntryStage stage, const std::string&) {
                ++stages;
                if (stage == Luminumbra::Client::WorldEntryStage::OpenWorld)
                    ++opens;
                return Luminumbra::Client::WorldEntryResult::Complete;
            },
            [&] {
                ++drains;
                return Luminumbra::Client::WorldEntryResult::Complete;
            });
    }
    EXPECT_EQ(entry.state(), Luminumbra::Client::WorldEntryState::Cancelled);
    EXPECT_EQ(stages, 0u);
    EXPECT_EQ(opens, 0u);
    EXPECT_EQ(drains, 1u);
    ui.Update();
    EXPECT_NE(FindDocumentByElementId(ui.GetContext(), "main_menu"), nullptr);
    ui.Shutdown();
}

// Save-as-named-preset: create-world lists saved user presets as selectable chips, and the
// "save preset" control hands the current config (name + base + params) to the saver.
TEST(UiSmokeTest, SaveAndListUserPresetsAreFunctional) {
    HiddenGlContext context;
    if (!context.ready()) {
        GTEST_SKIP() << context.error();
    }
    const fs::path source_root = SourceRoot();
    Luminumbra::Client::Rml_UIManager ui((source_root.string() + "/"));
    ui.Init(context.window(), nullptr);
    ASSERT_NE(ui.GetContext(), nullptr);

    struct Saved {
        std::string name;
        std::string base;
        std::size_t count = 0;
    };
    std::optional<Saved> saved;
    ui.SetWorldPresetSaver([&](const std::string& name,
                               const std::string& base,
                               const std::vector<Luminumbra::Client::WorldGenParam>& p,
                               bool overwrite_confirmed) -> std::string {
        EXPECT_FALSE(overwrite_confirmed);
        saved = Saved{name, base, p.size()};
        return "user_test_type";
    });
    ui.SetWorldPresetList([&]() -> std::vector<std::pair<std::string, std::string>> {
        return {{"My Canyon", "user_my_canyon"}};
    });
    ui.SetWorldParamGetter([&](const std::string&, const std::string&) { return std::string(); });

    Rml::ElementDocument* wc = LoadDocumentAndFind(ui, "world_creation.rml", "world_creation");
    ASSERT_NE(wc, nullptr);

    // The saved user preset was injected as a chip.
    Rml::Element* row = wc->GetElementById("user_presets_row");
    ASSERT_NE(row, nullptr);
    ASSERT_EQ(row->GetNumChildren(), 1) << "one saved user preset chip expected";
    Rml::Element* user_chip = row->GetChild(0);
    ASSERT_NE(user_chip, nullptr);
    EXPECT_EQ(user_chip->GetAttribute<Rml::String>("data-preset", ""), "user_my_canyon");

    // Save the current config under a name -> the saver receives name + base + the param set.
    SetControlValue(wc, "save_preset_name", "Test Type");
    ClickAndUpdate(ui, wc->GetElementById("save_preset_btn"));
    ASSERT_TRUE(saved.has_value());
    if (!saved.has_value())
        return;
    EXPECT_EQ(saved->name, "Test Type");
    EXPECT_EQ(saved->base, "default");
    EXPECT_GT(saved->count, 20u) << "the full customize param set must be handed to the saver";

    // Selecting the injected user chip drives the hidden world_type to its id.
    ClickAndUpdate(ui, row->GetChild(0));
    EXPECT_TRUE(row->GetChild(0)->IsClassSet("selected"));
    auto* type_sel = dynamic_cast<Rml::ElementFormControl*>(wc->GetElementById("world_type"));
    ASSERT_NE(type_sel, nullptr);
    EXPECT_EQ(std::string(type_sel->GetValue()), "user_my_canyon");

    ui.Shutdown();
}

//  pause menu (pause.rml) routes resume/quit back through the PauseActionCallback with
// the right action string, and its settings/gallery buttons navigate. Drives the real manager.
TEST(UiSmokeTest, PauseMenuActionsAndNavigationAreFunctional) {
    HiddenGlContext context;
    if (!context.ready()) {
        GTEST_SKIP() << context.error();
    }
    const fs::path source_root = SourceRoot();
    Luminumbra::Client::Rml_UIManager ui((source_root.string() + "/"));
    ui.Init(context.window(), nullptr);
    ASSERT_NE(ui.GetContext(), nullptr);

    std::vector<std::string> pause_actions;
    ui.SetPauseActionCallback([&](const std::string& act) { pause_actions.push_back(act); });

    Rml::ElementDocument* pause = LoadDocumentAndFind(ui, "pause.rml", "pause");
    ASSERT_NE(pause, nullptr);

    // resume_btn -> "resume".
    ClickAndUpdate(ui, pause->GetElementById("resume_btn"));
    ASSERT_FALSE(pause_actions.empty());
    EXPECT_EQ(pause_actions.back(), "resume") << "resume_btn must route \"resume\"";

    // quit_menu_btn -> "quit".
    ClickAndUpdate(ui, pause->GetElementById("quit_menu_btn"));
    ASSERT_GE(pause_actions.size(), 2u);
    EXPECT_EQ(pause_actions.back(), "quit") << "quit_menu_btn must route \"quit\"";

    // settings_btn navigates to settings.rml.
    ClickAndUpdate(ui, pause->GetElementById("settings_btn"));
    EXPECT_NE(FindDocumentByElementId(ui.GetContext(), "settings"), nullptr)
        << "pause settings_btn must navigate to settings.rml";

    // gallery_btn navigates to gallery.rml (from the pause menu again).
    pause = LoadDocumentAndFind(ui, "pause.rml", "pause");
    ASSERT_NE(pause, nullptr);
    ClickAndUpdate(ui, pause->GetElementById("gallery_btn"));
    EXPECT_NE(FindDocumentByElementId(ui.GetContext(), "gallery"), nullptr)
        << "pause gallery_btn must navigate to gallery.rml";

    ui.Shutdown();
}

//  the gallery (gallery.rml) back-arrow returns to the main menu and the photo wall +
// pager are present.: HERMETIC — the photo wall is driven by the committed
// data/ui/fixtures/captures set (the --ui-fixtures source), not by whatever
// untracked shutter captures happen to exist on this machine (which made the
// test environmentally RED on a clean checkout with 0 photos).
TEST(UiSmokeTest, GalleryBackNavigationAndContentArePresent) {
    HiddenGlContext context;
    if (!context.ready()) {
        GTEST_SKIP() << context.error();
    }
    const fs::path source_root = SourceRoot();
    Luminumbra::Client::Rml_UIManager ui((source_root.string() + "/"));
    ui.Init(context.window(), nullptr);
    ASSERT_NE(ui.GetContext(), nullptr);
    const fs::path fixtures = source_root / "data" / "ui" / "fixtures" / "captures";
    ASSERT_TRUE(fs::exists(fixtures / "cap_1.tga"))
        << "committed gallery fixture set missing: " << fixtures.string();
    ui.SetGalleryCaptureSource(fixtures, "fixtures/captures/");

    Rml::ElementDocument* gallery = LoadDocumentAndFind(ui, "gallery.rml", "gallery");
    ASSERT_NE(gallery, nullptr);

    // Photo cards + pager dots are present (not an empty shell).
    Rml::ElementList cards;
    gallery->GetElementsByClassName(cards, "photo-card");
    EXPECT_GE(cards.size(), 4u) << "gallery must show a photo wall";
    Rml::ElementList dots;
    gallery->GetElementsByClassName(dots, "pager-dot");
    EXPECT_GE(dots.size(), 1u) << "gallery pager must be present";

    // The back arrow returns to the main menu.
    ClickAndUpdate(ui, gallery->GetElementById("back_btn"));
    EXPECT_NE(FindDocumentByElementId(ui.GetContext(), "main_menu"), nullptr)
        << "gallery back arrow must navigate to main_menu.rml";

    ui.Shutdown();
}

namespace {

void CheckCaptureGalleryContentPolicy() {
    class GalleryRoot {
    public:
        GalleryRoot() {
            // At most 32 exclusive attempts.
            for (int attempt = 0; attempt < 32; ++attempt) {
                const auto candidate =
                    fs::temp_directory_path() /
                    ("gallery-content-" +
                     std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) +
                     "-" + std::to_string(attempt));
                if (fs::create_directory(candidate)) {
                    m_path = candidate;
                    break;
                }
            }
        }
        ~GalleryRoot() {
            std::error_code error;
            fs::remove_all(m_path, error);
        }
        const fs::path& path() const {
            return m_path;
        }

    private:
        fs::path m_path;
    };
    const GalleryRoot root;
    // Six thumbnails and four bounded metadata payloads plus one
    // unreadable directory exercise the optional rating inputs.
    ASSERT_FALSE(root.path().empty());
    for (const auto* tree : Luminumbra::Filesystem::kCaptureContentTrees)
        fs::create_directories(root.path() / tree);
    for (const auto* file : {"gallery.rml", "base.rcss", "game_theme.rcss", "pages.rcss"}) {
        fs::create_directories(root.path() / "data/ui");
        fs::copy_file(SourceRoot() / "data/ui" / file, root.path() / "data/ui" / file);
    }
    const auto thumbs = root.path() / "data/ui/captures";
    fs::create_directories(thumbs);
    fs::create_directories(root.path() / "photos");
    int id = 0;
    for (const auto* payload : {"{\"stars\":5}", "", "{\"stars\":", "{\"other\":1}"}) {
        ++id;
        std::ofstream output(root.path() / "photos" /
                             ("photo-" + std::to_string(id) + ".photo.json"));
        output << payload;
        output.close();
        ASSERT_TRUE(output);
    }
    fs::create_directory(root.path() / "photos/photo-5.photo.json");
    for (int number = 1; number <= 6; ++number)
        fs::copy_file(SourceRoot() / "data/ui/fixtures/captures/cap_1.tga",
                      thumbs / ("cap_" + std::to_string(number) + ".tga"));

    for (const auto* name : {"cap_0.tga", "cap_01.tga", "cap_3junk.tga", "cap_2147483648.tga"})
        fs::copy_file(SourceRoot() / "data/ui/fixtures/captures/cap_1.tga", thumbs / name);
    const HiddenGlContext context;
    ASSERT_TRUE(context.ready()) << context.error();
    Luminumbra::Client::Rml_UIManager ui(root.path().string() + "/");
    ui.Init(context.window(), nullptr);
    ASSERT_NE(ui.GetContext(), nullptr);
    auto inspect = [&](std::size_t expected_cards, std::size_t expected_favorites) {
        ui.RequestLoadDocument("gallery.rml");
        ui.Update();
        ui.ReloadActiveDocument();
        auto* gallery = FindDocumentByElementId(ui.GetContext(), "gallery");
        ASSERT_NE(gallery, nullptr);
        Rml::ElementList cards;
        gallery->GetElementsByClassName(cards, "photo-card");
        EXPECT_EQ(cards.size(), expected_cards);
        Rml::ElementList favorites;
        gallery->GetElementsByClassName(favorites, "photo-fav");
        EXPECT_EQ(favorites.size(), expected_favorites);
    };
    ASSERT_TRUE(Luminumbra::Rendering::WritePixelBufferPpm(
        root.path() / "photos/photo-1.ppm", 1, 1, {1, 2, 3}));
    ASSERT_TRUE(Luminumbra::Rendering::WriteCaptureText(root.path() / "photos/photo-1.photo.json",
                                                        "{\"stamp\":\"photo-1\",\"stars\":5}"));
    // Admit complete historical photos as well as published captures; partial
    // legacy artifacts stay hidden.
    luminumbra::game::PhotoSidecar side;
    side.verdict.stars = 5;
    ASSERT_EQ(Luminumbra::Rendering::PersistPhotoCapture(root.path(), 7, 1, 1, {1, 2, 3}, side), 7);
    // A duplicate legacy id must not displace the published photo or its rating.
    ASSERT_TRUE(Luminumbra::Rendering::WritePixelBufferPpm(
        root.path() / "photos/photo-7.ppm", 1, 1, {1, 2, 3}));
    ASSERT_TRUE(Luminumbra::Rendering::WriteCaptureText(root.path() / "photos/photo-7.photo.json",
                                                        "{\"stamp\":\"photo-7\",\"stars\":0}"));
    fs::copy_file(thumbs / "cap_1.tga", thumbs / "cap_7.tga");
    fs::create_directory(thumbs / "cap_8"); // Unpublished newer capture stays hidden.
    inspect(2, 2);
    // A restarted session must place its shutter result ahead of historical ids.
    ASSERT_TRUE(Luminumbra::Rendering::WritePixelBufferPpm(
        root.path() / "photos/photo-100.ppm", 1, 1, {1, 2, 3}));
    ASSERT_TRUE(Luminumbra::Rendering::WriteCaptureText(root.path() / "photos/photo-100.photo.json",
                                                        "{\"stamp\":\"photo-100\",\"stars\":5}"));
    fs::copy_file(thumbs / "cap_1.tga", thumbs / "cap_100.tga");
    Luminumbra::Rendering::PhotoCaptureAllocator restarted_session;
    ASSERT_EQ(restarted_session.Persist(root.path(), 1, 1, {1, 2, 3}, side), 101);
    inspect(4, 4);
    auto* ordered_gallery = FindDocumentByElementId(ui.GetContext(), "gallery");
    ASSERT_NE(ordered_gallery, nullptr);
    Rml::ElementList ordered_thumbnails;
    ordered_gallery->GetElementsByClassName(ordered_thumbnails, "photo-thumb");
    ASSERT_EQ(ordered_thumbnails.size(), 4u);
    EXPECT_EQ(ordered_thumbnails.front()->GetAttribute<Rml::String>("src", ""),
              "captures/cap_101/thumbnail.tga");
    // Discovery is bounded even when almost every entry is unrelated. Keep
    // exactly 65536 entries first, then refuse an incomplete 65537-entry scan.
    constexpr std::size_t kGalleryEntryBound = 65536;
    const auto present = static_cast<std::size_t>(
        std::distance(fs::directory_iterator(thumbs), fs::directory_iterator{}));
    for (std::size_t entry = present; entry < kGalleryEntryBound; ++entry) {
        std::ofstream output(thumbs / ("unrelated-" + std::to_string(entry)));
        ASSERT_TRUE(Luminumbra::Rendering::CloseCaptureFile(output));
    }
    inspect(4, 4);
    ASSERT_TRUE(Luminumbra::Rendering::WriteCaptureText(thumbs / "overflow", "unrelated"));
    inspect(0, 0);
    ASSERT_TRUE(fs::remove(thumbs / "overflow"));
    for (std::size_t entry = present; entry < kGalleryEntryBound; ++entry)
        ASSERT_TRUE(fs::remove(thumbs / ("unrelated-" + std::to_string(entry))));
    ASSERT_TRUE(fs::remove(thumbs / "cap_100.tga"));
    auto& policy = Luminumbra::Filesystem::capture_content_paths();
    policy.enable(root.path());
    inspect(7, 0);
    EXPECT_FALSE(policy.refused());
    fs::remove_all(thumbs);
    fs::create_directory(thumbs);
    inspect(0, 0);
    EXPECT_FALSE(policy.refused());
    // The optional rating omission must never bypass content checks.
    ui.SetGalleryCaptureSource(root.path() / "photos", "captures/");
    inspect(8, 1); // Refusal preserves the authored fallback cards.
    EXPECT_TRUE(policy.refused());
    ui.Shutdown();
}

} // namespace

// A subprocess owns the process-wide capture policy, so other UI cases retain
// their normal interactive mode even when the entire test binary runs at once.
TEST(UiSmokeDeathTest, CaptureGalleryOmitsPersonalRatingsAndRetainsContentRefusals) {
    ASSERT_EXIT(([] {
                    CheckCaptureGalleryContentPolicy();
                    std::exit(::testing::Test::HasFailure() ? 1 : 0);
                })(),
                ::testing::ExitedWithCode(0),
                "");
}

//  world-select load button must NOT fire the load callback with no selection, and the
// active document hot-reloads via ReloadActiveDocument.
TEST(UiSmokeTest, WorldSelectEmptyGuardAndHotReload) {
    HiddenGlContext context;
    if (!context.ready()) {
        GTEST_SKIP() << context.error();
    }
    const fs::path source_root = SourceRoot();
    Luminumbra::Client::Rml_UIManager ui((source_root.string() + "/"));
    ui.Init(context.window(), nullptr);
    ASSERT_NE(ui.GetContext(), nullptr);

    int load_calls = 0;
    ui.SetLoadWorldCallback([&](const std::string&) { ++load_calls; });
    ui.SetSavedWorldList(SavedWorldFixture);

    Rml::ElementDocument* ws = LoadDocumentAndFind(ui, "world_selection.rml", "world_selection");
    ASSERT_NE(ws, nullptr);

    // Clear any authored default selection so the guard is exercised on an empty selection.
    Rml::ElementList items;
    ws->GetElementsByClassName(items, "list-item");
    for (Rml::Element* item : items) {
        item->RemoveAttribute("data-selected");
        item->SetClass("selected", false);
    }
    // The manager tracks selection internally; reloading the doc resets it to empty.
    ui.ReloadActiveDocument();
    ui.Update();
    AwaitWorldCatalog(ui);
    ws = FindDocumentByElementId(ui.GetContext(), "world_selection");
    ASSERT_NE(ws, nullptr) << "ReloadActiveDocument must re-load the world-selection screen";

    // With nothing selected, clicking load must be a guarded no-op (no callback).
    Rml::Element* load_btn = ws->GetElementById("load_selected_btn");
    ASSERT_NE(load_btn, nullptr);
    ClickAndUpdate(ui, load_btn);
    EXPECT_EQ(load_calls, 0) << "load_selected_btn must not fire the callback with no selection";

    // Selecting an item then loading DOES fire (proves the guard, not a dead button).
    Rml::Element* first = FirstWorldListItem(ws);
    ASSERT_NE(first, nullptr);
    ClickAndUpdate(ui, first);
    ClickAndUpdate(ui, ws->GetElementById("load_selected_btn"));
    EXPECT_EQ(load_calls, 1) << "load fires once a world is selected";

    ui.Shutdown();
}

//  every <img src> authored in the world-select + gallery screens must resolve to an
// existing TGA on disk (RmlUi images are TGA-only; a typo'd thumb path renders nothing).
TEST(WorldgenOverrideTest, MenuImageSourcesResolveToExistingTga) {
    const fs::path root = SourceRoot();
    const std::regex img_regex(R"(<img[^>]*src\s*=\s*\"([^\"]+)\")", std::regex::icase);
    int checked = 0;
    for (const char* doc : {"world_selection.rml", "gallery.rml"}) {
        const std::string rml = ReadTextFile(root / "data/ui" / doc);
        ASSERT_FALSE(rml.empty()) << doc;
        for (std::sregex_iterator it(rml.begin(), rml.end(), img_regex), end; it != end; ++it) {
            const std::string src = (*it)[1].str();
            const fs::path resolved = root / "data/ui" / src;
            EXPECT_TRUE(fs::exists(resolved)) << doc << " <img src> does not resolve: " << src;
            EXPECT_EQ(fs::path(src).extension(), ".tga") << "RmlUi images must be TGA: " << src;
            ++checked;
        }
    }
    EXPECT_GE(checked, 4) << "expected several authored thumbnails across the two screens";
}

//  user-preset MANAGEMENT — delete, rename, and the overwrite-collision confirm. The
// saver silently overwrites on slug collision, so the UI must gate it behind a confirm modal.
TEST(UiSmokeTest, UserPresetDeleteRenameAndOverwriteConfirmAreFunctional) {
    HiddenGlContext context;
    if (!context.ready()) {
        GTEST_SKIP() << context.error();
    }
    const fs::path source_root = SourceRoot();
    Luminumbra::Client::Rml_UIManager ui((source_root.string() + "/"));
    ui.Init(context.window(), nullptr);
    ASSERT_NE(ui.GetContext(), nullptr);

    // In-memory preset store the bridges operate on (display name -> world-type id).
    std::vector<std::pair<std::string, std::string>> store = {{"My Canyon", "user_my_canyon"}};
    auto slug = [](const std::string& name) {
        std::string s;
        for (char ch : name) {
            if (std::isalnum(static_cast<unsigned char>(ch)))
                s += static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
            else if ((ch == ' ' || ch == '-' || ch == '_') && !s.empty() && s.back() != '_')
                s += '_';
        }
        while (!s.empty() && s.back() == '_')
            s.pop_back();
        return s.empty() ? std::string("preset") : s.substr(0, 32);
    };

    int save_count = 0;
    std::string last_saved_name;
    ui.SetWorldParamGetter([&](const std::string&, const std::string&) { return std::string(); });
    ui.SetWorldPresetList([&]() { return store; });
    ui.SetWorldPresetSaver([&](const std::string& name,
                               const std::string&,
                               const std::vector<Luminumbra::Client::WorldGenParam>&,
                               bool overwrite_confirmed) -> std::string {
        const std::string type = "user_" + slug(name);
        // overwrite-or-insert by id (mirrors the on-disk saver).
        for (auto& e : store)
            if (e.second == type) {
                EXPECT_TRUE(overwrite_confirmed);
                e.first = name;
                ++save_count;
                last_saved_name = name;
                return type;
            }
        EXPECT_FALSE(overwrite_confirmed);
        store.emplace_back(name, type);
        ++save_count;
        last_saved_name = name;
        return type;
    });
    ui.SetWorldPresetExists([&](const std::string& name) -> bool {
        const std::string type = "user_" + slug(name);
        for (const auto& e : store)
            if (e.second == type)
                return true;
        return false;
    });
    ui.SetWorldPresetDeleter([&](const std::string& worldType) -> bool {
        for (auto it = store.begin(); it != store.end(); ++it) {
            if (it->second == worldType) {
                store.erase(it);
                return true;
            }
        }
        return false;
    });
    ui.SetWorldPresetRenamer(
        [&](const std::string& worldType, const std::string& newName) -> std::string {
            const std::string newType = "user_" + slug(newName);
            for (auto& e : store)
                if (e.second == worldType) {
                    e.first = newName;
                    e.second = newType;
                    return newType;
                }
            return "";
        });

    Rml::ElementDocument* wc = LoadDocumentAndFind(ui, "world_creation.rml", "world_creation");
    ASSERT_NE(wc, nullptr);

    Rml::Element* row = wc->GetElementById("user_presets_row");
    ASSERT_NE(row, nullptr);
    ASSERT_EQ(row->GetNumChildren(), 1) << "one seeded user preset chip";

    // --- OVERWRITE-COLLISION CONFIRM ---
    // Saving under the existing "My Canyon" name must NOT save immediately — it shows the modal.
    SetControlValue(wc, "save_preset_name", "My Canyon");
    ClickAndUpdate(ui, wc->GetElementById("save_preset_btn"));
    EXPECT_EQ(save_count, 0) << "a colliding name must not silently overwrite";
    Rml::Element* ow_modal = wc->GetElementById("preset_overwrite_modal");
    ASSERT_NE(ow_modal, nullptr);
    EXPECT_FALSE(ow_modal->IsClassSet("hidden"))
        << "overwrite-confirm modal must be shown on collision";

    // Cancel dismisses without saving.
    ClickAndUpdate(ui, wc->GetElementById("cancel_overwrite_btn"));
    EXPECT_TRUE(ow_modal->IsClassSet("hidden"));
    EXPECT_EQ(save_count, 0);

    // Re-trigger and CONFIRM -> the save proceeds (the overwrite the user opted into).
    ClickAndUpdate(ui, wc->GetElementById("save_preset_btn"));
    EXPECT_FALSE(ow_modal->IsClassSet("hidden"));
    SetControlValue(wc, "save_preset_name", "Unconfirmed name");
    ClickAndUpdate(ui, wc->GetElementById("confirm_overwrite_btn"));
    EXPECT_EQ(last_saved_name, "My Canyon");
    EXPECT_TRUE(ow_modal->IsClassSet("hidden"));
    EXPECT_EQ(save_count, 1) << "confirming overwrite must commit the save";
    EXPECT_EQ(store.size(), 1u) << "overwriting must not add a second preset";

    // A NON-colliding name saves directly (no modal).
    SetControlValue(wc, "save_preset_name", "Fresh Vista");
    ClickAndUpdate(ui, wc->GetElementById("save_preset_btn"));
    EXPECT_TRUE(ow_modal->IsClassSet("hidden")) << "a non-colliding name must not show the modal";
    EXPECT_EQ(save_count, 2);
    EXPECT_EQ(store.size(), 2u);

    // --- RENAME ---
    // Select the user preset, type a new name, rename.
    wc = FindDocumentByElementId(ui.GetContext(), "world_creation");
    ASSERT_NE(wc, nullptr);
    row = wc->GetElementById("user_presets_row");
    ASSERT_NE(row, nullptr);
    ASSERT_GE(row->GetNumChildren(), 1);
    // Select the "My Canyon" chip.
    Rml::Element* canyon_chip = nullptr;
    for (int i = 0; i < row->GetNumChildren(); ++i) {
        if (row->GetChild(i)->GetAttribute<Rml::String>("data-preset", "") == "user_my_canyon")
            canyon_chip = row->GetChild(i);
    }
    ASSERT_NE(canyon_chip, nullptr);
    ClickAndUpdate(ui, canyon_chip);
    auto* type_sel = dynamic_cast<Rml::ElementFormControl*>(wc->GetElementById("world_type"));
    ASSERT_NE(type_sel, nullptr);
    EXPECT_EQ(std::string(type_sel->GetValue()), "user_my_canyon");
    SetControlValue(wc, "save_preset_name", "Grand Gorge");
    ClickAndUpdate(ui, wc->GetElementById("rename_preset_btn"));
    bool found_renamed = false;
    for (const auto& e : store)
        if (e.first == "Grand Gorge" && e.second == "user_grand_gorge")
            found_renamed = true;
    EXPECT_TRUE(found_renamed) << "rename must update the saved preset's name + id";
    for (const auto& e : store)
        EXPECT_NE(e.second, "user_my_canyon") << "old id must be gone after rename";

    // --- DELETE ---
    wc = FindDocumentByElementId(ui.GetContext(), "world_creation");
    ASSERT_NE(wc, nullptr);
    row = wc->GetElementById("user_presets_row");
    ASSERT_NE(row, nullptr);
    const std::size_t before_delete = store.size();
    // Click a chip's delete affordance -> the delete-confirm modal appears with the pending id.
    Rml::Element* del_ctrl = wc->GetElementById("del_user_grand_gorge");
    ASSERT_NE(del_ctrl, nullptr) << "each user chip has a delete affordance";
    ClickAndUpdate(ui, del_ctrl);
    Rml::Element* del_modal = wc->GetElementById("preset_delete_modal");
    ASSERT_NE(del_modal, nullptr);
    EXPECT_FALSE(del_modal->IsClassSet("hidden")) << "delete must require a confirm";
    EXPECT_EQ(del_modal->GetAttribute<Rml::String>("data-pending", ""), "user_grand_gorge");
    // Cancel keeps it.
    ClickAndUpdate(ui, wc->GetElementById("cancel_delete_preset_btn"));
    EXPECT_TRUE(del_modal->IsClassSet("hidden"));
    EXPECT_EQ(store.size(), before_delete);
    // Confirm deletes.
    ClickAndUpdate(ui, del_ctrl);
    ClickAndUpdate(ui, wc->GetElementById("confirm_delete_preset_btn"));
    EXPECT_EQ(store.size(), before_delete - 1) << "confirming delete removes the preset";
    for (const auto& e : store)
        EXPECT_NE(e.second, "user_grand_gorge");

    ui.Shutdown();
}

// token-based Subscribe/Unsubscribe on Property<T> (no GL needed).
TEST(UiSmokeTest, PropertyTokenUnsubscribeStopsCallbacks) {
    using Luminumbra::Client::UI::Property;
    using Luminumbra::Client::UI::ScopedSubscription;
    using Luminumbra::Client::UI::SubscriptionToken;

    Property<int> property(0);
    int first_calls = 0;
    int second_calls = 0;

    const SubscriptionToken first =
        property.Subscribe([&](const int&, const int&) { ++first_calls; });
    const SubscriptionToken second =
        property.Subscribe([&](const int&, const int&) { ++second_calls; });
    EXPECT_NE(first, second);
    EXPECT_EQ(property.SubscriberCount(), 2u);

    property.Set(1);
    EXPECT_EQ(first_calls, 1);
    EXPECT_EQ(second_calls, 1);

    EXPECT_TRUE(property.Unsubscribe(first));
    EXPECT_FALSE(property.Unsubscribe(first)) << "double unsubscribe must be a safe no-op";
    EXPECT_EQ(property.SubscriberCount(), 1u);

    property.Set(2);
    EXPECT_EQ(first_calls, 1) << "unsubscribed callback must not fire";
    EXPECT_EQ(second_calls, 2);

    // RAII handle unsubscribes when it goes out of scope.
    {
        ScopedSubscription scoped(property, second);
        EXPECT_TRUE(scoped.Active());
    }
    EXPECT_EQ(property.SubscriberCount(), 0u);
    property.Set(3);
    EXPECT_EQ(second_calls, 2);
}

// destroy-then-mutate regression — a destroyed UIComponent must not
// be reachable from later Property::Set calls (use-after-free guard).
TEST(UiSmokeTest, DestroyedComponentReceivesNoPropertyMutations) {
    using Luminumbra::Client::UI::Property;
    using Luminumbra::Client::UI::UIComponent;

    HiddenGlContext context;
    if (!context.ready()) {
        GTEST_SKIP() << context.error();
    }

    const fs::path source_root = SourceRoot();
    Luminumbra::Client::Rml_UIManager ui((source_root.string() + "/"));
    ui.Init(context.window(), nullptr);
    ASSERT_NE(ui.GetContext(), nullptr);

    Rml::ElementDocument* main_menu = LoadDocumentAndFind(ui, "main_menu.rml", "main_menu");
    ASSERT_NE(main_menu, nullptr);
    Rml::Element* notification_text = main_menu->GetElementById("notification_text");
    ASSERT_NE(notification_text, nullptr);

    Property<std::string> text_property(std::string("bound-initial"));
    int callback_fires = 0;
    const auto counter_token =
        text_property.Subscribe([&](const std::string&, const std::string&) { ++callback_fires; });

    {
        auto component = std::make_unique<UIComponent>("notification_text");
        component->Initialize(main_menu);
        ASSERT_TRUE(component->IsValid());

        component->BindText(text_property);
        EXPECT_EQ(text_property.SubscriberCount(), 2u) << "counter + component binding";
        EXPECT_EQ(notification_text->GetInnerRML(), "bound-initial");

        // Callback fires while the component is alive.
        text_property.Set("before-destroy");
        EXPECT_EQ(callback_fires, 1);
        EXPECT_EQ(notification_text->GetInnerRML(), "before-destroy");

        // Destruction alone (no explicit Destroy call) must unsubscribe.
        component.reset();
    }
    EXPECT_EQ(text_property.SubscriberCount(), 1u)
        << "component binding must be gone after destruction";

    // Mutating after destroy must not crash and must not touch the element
    // through the dead component's binding.
    text_property.Set("after-destroy");
    EXPECT_EQ(callback_fires, 2);
    EXPECT_EQ(notification_text->GetInnerRML(), "before-destroy");

    // Callback count returns to zero once the remaining subscriber leaves.
    EXPECT_TRUE(text_property.Unsubscribe(counter_token));
    EXPECT_EQ(text_property.SubscriberCount(), 0u);
    text_property.Set("nobody-listens");
    EXPECT_EQ(callback_fires, 2);

    ui.Shutdown();
}

// BuildCustomPreset is the merge below the create callback (no GL): it must apply ONLY the
// overrides that differ from the base, leave the rest byte-identical, and special-case the
// biomes toggle (which has no flag — it maps to biomes.table). This is the unit that the e2e
// can only observe up to the callback boundary.
TEST(WorldgenOverrideTest, BuildCustomPresetAppliesOnlyRealDeltas) {
    using Luminumbra::Client::BuildCustomPreset;
    using P = Luminumbra::Client::WorldGenParam;

    nlohmann::json base = {
        {"generation_params",
         {
             {"terrain", {{"base_amplitude", 34.0}, {"octaves", 5}}},
             {"features", {{"caves_enabled", true}}},
             {"biomes", {{"table", "common/biomes.json"}}},
         }},
    };

    // amplitude unchanged (34), octaves 5->8, caves on->off, biomes on->off (clears table).
    std::vector<P> params = {
        {"terrain.base_amplitude", "34.000000", "float"},
        {"terrain.octaves", "8.000000", "int"},
        {"features.caves_enabled", "false", "bool"},
        {"biomes.enabled", "false", "bool"},
    };
    auto r = BuildCustomPreset(base, params);
    EXPECT_TRUE(r.changed);
    EXPECT_EQ(r.applied, 3) << "the unchanged amplitude must not count as an override";
    EXPECT_DOUBLE_EQ(r.json["generation_params"]["terrain"]["base_amplitude"].get<double>(), 34.0);
    EXPECT_EQ(r.json["generation_params"]["terrain"]["octaves"].get<int>(), 8);
    EXPECT_EQ(r.json["generation_params"]["features"]["caves_enabled"].get<bool>(), false);
    EXPECT_EQ(r.json["generation_params"]["biomes"]["table"].get<std::string>(), "");

    // Nothing actually different from the base -> no customization at all.
    std::vector<P> noop = {
        {"terrain.base_amplitude", "34.0", "float"},
        {"terrain.octaves", "5", "int"},
        {"features.caves_enabled", "true", "bool"},
        {"biomes.enabled", "true", "bool"},
    };
    EXPECT_FALSE(BuildCustomPreset(base, noop).changed) << "an untouched form must not customize";

    // Unparseable numeric is skipped, not silently zeroed.
    std::vector<P> bad = {{"terrain.base_amplitude", "not-a-number", "float"}};
    auto rb = BuildCustomPreset(base, bad);
    EXPECT_FALSE(rb.changed);
    EXPECT_EQ(rb.skipped, 1);
}

TEST(WorldgenOverrideTest, NumericOverridesRejectPartialNonfiniteAndOutOfRangeValues) {
    using Luminumbra::Client::BuildCustomPreset;
    using namespace Luminumbra::world;
    const nlohmann::json base = {
        {"generation_params", {{"terrain", {{"base_amplitude", 34.0}, {"octaves", 5}}}}}};
    const auto unchanged = BuildCustomPreset(base, {}).json;
    const auto knobs = NeutralKnobVector();
    const auto resolved_base = ResolveEffectivePreset(base, TerrainPresetUse::NewWorld);
    // The persisted layer snapshots generation_params; schema_rev belongs to the preset.
    const auto baseline = ApplyKnobLayer(resolved_base, knobs).at("generation_params");
    const auto reopened = [&](const char* path, const char* value, const char* type) {
        nlohmann::json persisted;
        WriteKnobLayer(persisted, resolved_base, knobs, {{path, value, type}});
        EXPECT_EQ(persisted.at("schema_rev"), resolved_base.at("schema_rev"));
        const auto layer = ReadKnobLayer(nlohmann::json::parse(persisted.dump()));
        EXPECT_TRUE(layer.present);
        const auto resolved = ResolveKnobLayer(layer.baseline, layer.knobs, layer.overrides);
        auto saved_params = persisted.at("generation_params");
        saved_params.erase("knob_layer");
        EXPECT_EQ(saved_params, resolved.at("generation_params"));
        return resolved;
    };
    for (const auto* value : {"", "0suffix", "nan", "inf", "-inf", "1e999"}) {
        SCOPED_TRACE(value);
        for (const auto* type : {"float", "int"}) {
            SCOPED_TRACE(type);
            const auto result = BuildCustomPreset(base, {{"terrain.base_amplitude", value, type}});
            EXPECT_FALSE(result.changed);
            EXPECT_EQ(result.applied, 0);
            EXPECT_EQ(result.skipped, 1);
            EXPECT_EQ(result.json, unchanged);
            EXPECT_EQ(reopened("terrain.base_amplitude", value, type).at("generation_params"),
                      baseline);
        }
    }
    for (const auto* value : {"1e38",
                              "-1e38",
                              "1e1",
                              "6.5",
                              "6.",
                              "9223372036854775808",
                              "-9223372036854775809",
                              "9223372036854775808.0"}) {
        SCOPED_TRACE(value);
        const auto result = BuildCustomPreset(base, {{"terrain.octaves", value, "int"}});
        EXPECT_FALSE(result.changed);
        EXPECT_EQ(result.skipped, 1);
        EXPECT_EQ(result.json, unchanged);
        EXPECT_EQ(reopened("terrain.octaves", value, "int").at("generation_params"), baseline);
    }
    for (const auto& [value, expected] :
         {std::pair{"0", 0LL},
          std::pair{"0.000000", 0LL},
          std::pair{"8.000000", 8LL},
          std::pair{"123456789.000000", 123456789LL},
          std::pair{"9223372036854775807", std::numeric_limits<long long>::max()},
          std::pair{"9223372036854775807.0", std::numeric_limits<long long>::max()},
          std::pair{"-9223372036854775808", std::numeric_limits<long long>::lowest()}}) {
        SCOPED_TRACE(value);
        const auto result = BuildCustomPreset(base, {{"terrain.octaves", value, "int"}});
        EXPECT_TRUE(result.changed);
        EXPECT_EQ(result.applied, 1);
        EXPECT_EQ(result.skipped, 0);
        EXPECT_EQ(result.json["generation_params"]["terrain"]["octaves"].get<long long>(),
                  expected);
        EXPECT_EQ(
            reopened("terrain.octaves", value, "int")["generation_params"]["terrain"]["octaves"]
                .get<long long>(),
            expected);
    }
    const auto zero = BuildCustomPreset(base, {{"terrain.base_amplitude", "0", "float"}});
    EXPECT_TRUE(zero.changed);
    EXPECT_EQ(zero.json["generation_params"]["terrain"]["base_amplitude"], 0.0);
    EXPECT_EQ(reopened("terrain.base_amplitude",
                       "0",
                       "float")["generation_params"]["terrain"]["base_amplitude"],
              0.0);
}

TEST(WorldgenOverrideTest, NumericBaselineComparisonIsExactAndRangeChecked) {
    using Luminumbra::Client::BuildCustomPreset;
    for (const auto& initial : {nlohmann::json(),
                                nlohmann::json("unknown"),
                                nlohmann::json(5.5),
                                nlohmann::json(1e38),
                                nlohmann::json(-1e38),
                                nlohmann::json(std::numeric_limits<unsigned long long>::max())}) {
        SCOPED_TRACE(initial.dump());
        const nlohmann::json base = {{"generation_params", {{"custom_integer", initial}}}};
        const auto result = BuildCustomPreset(base, {{"custom_integer", "5", "int"}});
        EXPECT_TRUE(result.changed);
        EXPECT_EQ(result.json["generation_params"]["custom_integer"], 5);
    }
    for (const auto& initial : {nlohmann::json(5LL), nlohmann::json(5ULL), nlohmann::json(5.0)}) {
        const nlohmann::json base = {{"generation_params", {{"custom_integer", initial}}}};
        EXPECT_FALSE(BuildCustomPreset(base, {{"custom_integer", "5", "int"}}).changed);
    }
    const auto maximum = std::to_string(std::numeric_limits<long long>::max());
    const auto added =
        BuildCustomPreset(nlohmann::json::object(), {{"custom_integer", maximum, "int"}});
    EXPECT_TRUE(added.changed);
    EXPECT_EQ(added.json["generation_params"]["custom_integer"].get<long long>(),
              std::numeric_limits<long long>::max());
    for (const auto& initial :
         {nlohmann::json("unknown"), nlohmann::json(std::numeric_limits<double>::quiet_NaN())}) {
        const nlohmann::json base = {{"generation_params", {{"custom_float", initial}}}};
        const auto result = BuildCustomPreset(base, {{"custom_float", "1e38", "float"}});
        EXPECT_TRUE(result.changed);
        EXPECT_EQ(result.applied, 1);
        EXPECT_EQ(result.json["generation_params"]["custom_float"].get<float>(), 1e38f);
    }
}

TEST(WorldgenOverrideTest, KnobsRejectInvalidNumbersAndRetainFiniteClamping) {
    using Luminumbra::Client::BuildKnobResolvedPreset;
    const nlohmann::json base = {
        {"generation_params", {{"terrain", {{"base_amplitude", 34.0}, {"octaves", 5}}}}}};
    const auto neutral = BuildKnobResolvedPreset(base, {});
    for (const auto* value : {"", "0suffix", "nan", "inf", "-inf", "1e999"}) {
        SCOPED_TRACE(value);
        const auto result =
            BuildKnobResolvedPreset(base, {{"knob.mountainousness", value, "knob"}});
        EXPECT_FALSE(result.changed);
        EXPECT_EQ(result.knob_count, 0);
        EXPECT_EQ(result.json, neutral.json);
    }
    for (const auto* value : {"0", "1", "-4", "4"}) {
        SCOPED_TRACE(value);
        const auto result =
            BuildKnobResolvedPreset(base, {{"knob.mountainousness", value, "knob"}});
        EXPECT_TRUE(result.changed);
        EXPECT_EQ(result.knob_count, 1);
        const auto expected = BuildKnobResolvedPreset(
            base,
            {{"knob.mountainousness", value[0] == '-' || value[0] == '0' ? "0" : "1", "knob"}});
        EXPECT_EQ(result.json, expected.json);
    }
}

TEST(WorldgenOverrideTest, CustomizationKeepsExplicitSamplingAndItsSchemaGate) {
    for (const int revision : {1, 2}) {
        const nlohmann::json base = {{"schema_rev", 7},
                                     {"generation_params",
                                      {{"terrain", {{"surface_sampling_revision", revision}}},
                                       {"features", nlohmann::json::object()}}}};
        const auto custom = Luminumbra::Client::BuildCustomPreset(base, {});
        const auto knobs = Luminumbra::Client::BuildKnobResolvedPreset(base, {});
        for (const auto* preset : {&custom.json, &knobs.json}) {
            EXPECT_EQ(preset->at("schema_rev"), revision == 1 ? 7 : 8);
            EXPECT_EQ(preset->at("generation_params").at("terrain").at("surface_sampling_revision"),
                      revision);
        }
    }
}

// Drift guard: every worldgen-param data-path authored in world_creation.rml must be a real
// generation_params key (present in some curated preset, or a known default-only key). Catches a
// typo'd path that would silently no-op both seeding and the override.
TEST(WorldgenOverrideTest, EveryCustomizeParamPathIsAKnownWorldgenKey) {
    const fs::path root = SourceRoot();

    std::set<std::string> known;
    std::function<void(const nlohmann::json&, const std::string&)> walk =
        [&](const nlohmann::json& node, const std::string& prefix) {
            if (node.is_object()) {
                for (auto it = node.begin(); it != node.end(); ++it) {
                    walk(it.value(), prefix.empty() ? it.key() : prefix + "." + it.key());
                }
            } else if (!prefix.empty()) {
                known.insert(prefix);
            }
        };
    for (const auto& entry : fs::directory_iterator(root / "worlds/atlas/presets")) {
        if (entry.path().extension() != ".json")
            continue;
        std::ifstream f(entry.path());
        if (!f)
            continue;
        nlohmann::json j;
        try {
            f >> j;
        } catch (...) {
            continue;
        }
        if (j.contains("generation_params"))
            walk(j["generation_params"], "");
    }
    // Keys the loader reads with defaults that the curated presets may omit, plus the synthetic
    // biomes toggle (maps to biomes.table).
    for (const char* k : {"terrain.island_mask_enabled",
                          "terrain.hydro.iterations",
                          "terrain.hydro.thermal_rate",
                          "features.lakes_enabled",
                          "features.lake_depth",
                          "features.lake_frequency",
                          "features.cliffs_enabled",
                          "features.cliff_step",
                          "features.cliff_frequency",
                          "biomes.relief_enabled",
                          "biomes.relief_strength",
                          "biomes.enabled"}) {
        known.insert(k);
    }

    const std::string rml = ReadTextFile(root / "data/ui/world_creation.rml");
    ASSERT_FALSE(rml.empty());
    const std::regex path_regex(R"(data-path\s*=\s*\"([^\"]+)\")");
    int checked = 0;
    for (std::sregex_iterator it(rml.begin(), rml.end(), path_regex), end; it != end; ++it) {
        const std::string path = (*it)[1].str();
        EXPECT_GT(known.count(path), 0u) << "RML data-path is not a known worldgen key: " << path;
        ++checked;
    }
    EXPECT_GE(checked, 25) << "expected the full customize param set";
}

//   e2e: the SEMANTIC KNOBS are the default surface. They seed
// from the preset's persisted knob layer via the WorldParamGetter (neutral 0.5
// when a curated preset carries none), moving a knob reaches the create callback
// as a "knob.<id>" entry, and the knob is exposed in the live-preview state so
// the diorama regenerates. Drives the REAL Rml_UIManager headless.
TEST(UiSmokeTest, SemanticKnobsSeedDriveCallbackAndPreview) {
    HiddenGlContext context;
    if (!context.ready()) {
        GTEST_SKIP() << context.error();
    }
    const fs::path source_root = SourceRoot();
    Luminumbra::Client::Rml_UIManager ui((source_root.string() + "/"));
    ui.Init(context.window(), nullptr);
    ASSERT_NE(ui.GetContext(), nullptr);

    // The getter serves one knob ("wetness"=0.8) from a "persisted" layer; every
    // other knob has no persisted value -> the control keeps its NEUTRAL 0.5.
    ui.SetWorldParamGetter([&](const std::string&, const std::string& path) -> std::string {
        if (path == "knob.wetness")
            return "0.8";
        return std::string(); // curated -> neutral
    });
    std::optional<std::vector<Luminumbra::Client::WorldGenParam>> created_params;
    ui.SetWorldCreationCallback([&](const std::string&,
                                    const std::string&,
                                    const std::string&,
                                    const std::vector<Luminumbra::Client::WorldGenParam>& params) {
        created_params = params;
    });

    Rml::ElementDocument* wc = LoadDocumentAndFind(ui, "world_creation.rml", "world_creation");
    ASSERT_NE(wc, nullptr);

    // Six knobs render as the default surface.
    Rml::ElementList knobs;
    wc->GetElementsByClassName(knobs, "worldgen-knob");
    ASSERT_EQ(knobs.size(), 6u) << "exactly the six outcome knobs are the default surface";

    auto knob_by_id = [&](const std::string& id) -> Rml::Element* {
        for (auto* el : knobs)
            if (el->GetAttribute<Rml::String>("data-knob", "") == id)
                return el;
        return nullptr;
    };

    // wetness seeds from the persisted layer (0.8); a knob with no persisted value
    // stays neutral (0.5) — NEVER inverse-lerped.
    Rml::Element* wetness = knob_by_id("wetness");
    Rml::Element* mountains = knob_by_id("mountainousness");
    ASSERT_NE(wetness, nullptr);
    ASSERT_NE(mountains, nullptr);
    EXPECT_NEAR(std::stof(dynamic_cast<Rml::ElementFormControl*>(wetness)->GetValue()), 0.8f, 0.01f)
        << "wetness must seed from the persisted knob layer";
    EXPECT_NEAR(
        std::stof(dynamic_cast<Rml::ElementFormControl*>(mountains)->GetValue()), 0.5f, 0.01f)
        << "an unset knob stays neutral, not inverse-lerped";

    // Move the mountainousness knob, then create -> a knob.mountainousness entry
    // with the moved value reaches the callback (bound through the bridge, not raw).
    dynamic_cast<Rml::ElementFormControl*>(mountains)->SetValue("0.9");
    ClickAndUpdate(ui, wc->GetElementById("create_btn"));
    ASSERT_TRUE(created_params.has_value());
    if (!created_params.has_value())
        return;
    bool found_mtn = false, found_wet = false;
    for (const auto& p : *created_params) {
        if (p.path == "knob.mountainousness") {
            EXPECT_EQ(p.type, "knob");
            EXPECT_NEAR(std::stof(p.value), 0.9f, 0.01f);
            found_mtn = true;
        }
        if (p.path == "knob.wetness" && std::abs(std::stof(p.value) - 0.8f) < 0.01f)
            found_wet = true;
    }
    EXPECT_TRUE(found_mtn) << "the moved knob must reach the create callback as knob.<id>";
    EXPECT_TRUE(found_wet) << "the seeded knob must travel too";

    // The knob is also exposed in the live-preview state, so the host rebuilds the
    // diorama when it moves (this is what regenerates the preview).
    auto pv = ui.GetWorldCreationPreviewState();
    EXPECT_TRUE(pv.active);
    bool preview_has_knob = false;
    for (const auto& p : pv.params)
        if (p.path == "knob.mountainousness" && std::abs(std::stof(p.value) - 0.9f) < 0.01f)
            preview_has_knob = true;
    EXPECT_TRUE(preview_has_knob) << "the knob must drive the live preview state";

    ui.Shutdown();
}

TEST(WorldgenOverrideTest, OmittedShippingBiomesCanBeExplicitlyDisabled) {
    nlohmann::json base = {
        {"schema_rev", 7},
        {"generation_params",
         {{"terrain", nlohmann::json::object()}, {"features", nlohmann::json::object()}}}};
    const std::vector<Luminumbra::Client::WorldGenParam> off = {
        {"biomes.enabled", "false", "bool"}};
    const auto custom = Luminumbra::Client::BuildCustomPreset(base, off);
    EXPECT_TRUE(custom.changed);
    EXPECT_TRUE(
        custom.json.at("generation_params").at("biomes").at("table").get<std::string>().empty());
    const auto knobs = Luminumbra::Client::BuildKnobResolvedPreset(base, off);
    EXPECT_TRUE(knobs.changed);
    EXPECT_EQ(knobs.override_count, 1);
    EXPECT_TRUE(
        knobs.json.at("generation_params").at("biomes").at("table").get<std::string>().empty());
}
