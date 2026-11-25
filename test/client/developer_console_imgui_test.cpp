#include "app/RegisterDeveloperWidgets.h"
#include "debug/DeveloperConsole.h"
#include "debug/widgets/DeveloperWidgetUi.h"
#include "debug/widgets/PassiveDeveloperWidgets.h"
#include "debug/widgets/RenderControlWidgets.h"

#include <gtest/gtest.h>
#include <imgui.h>
#include <imgui_internal.h>

#include <limits>

using namespace Luminumbra::Client::App;
using namespace Luminumbra::Client::Debug;

namespace {
// Restores an existing context even when an ASSERT exits a test early.
class ScopedImGuiContext {
public:
    ScopedImGuiContext()
        : m_previous(ImGui::GetCurrentContext())
        , m_context(ImGui::CreateContext()) {
        ImGui::SetCurrentContext(m_context);
        auto& io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.LogFilename = nullptr;
        io.DisplaySize = ImVec2(1280, 900);
        io.DeltaTime = 1.0f / 60.0f;
        unsigned char* pixels = nullptr;
        int width = 0, height = 0;
        io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
        NextFrame();
    }
    ~ScopedImGuiContext() {
        ImGui::EndFrame();
        ImGui::DestroyContext(m_context);
        ImGui::SetCurrentContext(m_previous);
    }
    ScopedImGuiContext(const ScopedImGuiContext&) = delete;
    ScopedImGuiContext& operator=(const ScopedImGuiContext&) = delete;
    void NextFrame() {
        if (m_started)
            ImGui::EndFrame();
        ImGui::NewFrame();
        m_started = true;
    }

private:
    ImGuiContext* m_previous;
    ImGuiContext* m_context;
    bool m_started = false;
};

struct UiFixture : testing::Test {
    DeveloperConsole console;
    DeveloperFrameAdapter frame;
    int effects = 0, saves = 0;
    std::map<std::string, int> draws, queries;
    luminumbra::core::InspectorRegistry inspectors;
    std::optional<luminumbra::core::InspectorPanelHandle> perf_inspector;
    void SetUp() override {
        perf_inspector = RegisterPerfHudInspector(inspectors);
        ASSERT_TRUE(perf_inspector);
        if (!perf_inspector)
            return;
        DeveloperHost host;
        host.save = [&] {
            ++saves;
            return true;
        };
        host.execute = [&](const std::string&, const CommandArguments&) {
            ++effects;
            return CommandResult{};
        };
        host.renderer_available = [] {
            return Availability{};
        };
        DeveloperWidgetServices services;
        for (const auto& def : BuiltinDeveloperWidgets()) {
            const auto id = def.id;
            services[id] = {[&, id] {
                                ++queries[id];
                                return Availability{id != "perf.gpu", "No renderer"};
                            },
                            [&, id](DeveloperFrameAdapter&) {
                                ++draws[id];
                                if (id == "perf.hud") {
                                    ASSERT_TRUE(perf_inspector);
                                    if (!perf_inspector)
                                        return;
                                    ASSERT_TRUE(PublishPerfHudInspector(
                                        inspectors, *perf_inspector, {0.2, 0.75, 12, 1}));
                                    DrawPerfHudContents(inspectors.Read(*perf_inspector, 1));
                                } else if (id == "runtime.time_indicator")
                                    DrawTimeIndicatorContents(
                                        0.5f, FormatKeyChord(console.Bindings().Get("time.reset")));
                                else if (id == "runtime.time")
                                    DrawTimeControlContents(console, 1.0f);
                                else if (id == "render.debug_views")
                                    DrawDebugViewContents(console, 0);
                                else if (id == "render.wireframe")
                                    DrawWireframeContents(console, false);
                                else
                                    ImGui::TextUnformatted("Fake current service");
                            }};
        }
        console.Initialize(std::move(host), RegisterDeveloperWidgets(services));
        frame.draw = [&](const WidgetDef& def, const DrawFn& contents) {
            DrawDeveloperWidget(console.Widgets(), frame, def, contents);
        };
    }
};

TEST(PerfHudInspector, CopiesCompletedObservationsAndPreservesUnknownProducerAge) {
    using namespace luminumbra::core;
    InspectorRegistry registry;
    const auto handle = RegisterPerfHudInspector(registry);
    ASSERT_TRUE(handle);
    if (!handle)
        return;
    ASSERT_TRUE(PublishPerfHudInspector(registry, *handle, {0.2, 0.75, 0, 42}));
    const auto snapshot = registry.Read(*handle, 42);
    ASSERT_TRUE(snapshot);
    if (!snapshot)
        return;
    EXPECT_EQ(snapshot->metrics[0].value, 200.0);
    EXPECT_EQ(snapshot->metrics[0].age_frames, 0u);
    EXPECT_EQ(snapshot->metrics[1].value, 0.75);
    EXPECT_FALSE(snapshot->metrics[1].age_frames);
    EXPECT_EQ(snapshot->metrics[2].value, 0.0);
    EXPECT_EQ(snapshot->metrics[2].availability, MetricAvailability::Available);
    EXPECT_FALSE(snapshot->metrics[2].age_frames);
    EXPECT_TRUE(registry.Find("perf.hud"));
    EXPECT_FALSE(RegisterPerfHudInspector(registry));
    const auto stale = registry.Read(*handle, 43);
    ASSERT_TRUE(stale);
    if (!stale)
        return;
    EXPECT_EQ(stale->metrics[0].availability, MetricAvailability::Stale);
    EXPECT_FALSE(stale->metrics[0].value);
    ASSERT_TRUE(registry.Unregister(*handle));
    EXPECT_FALSE(PublishPerfHudInspector(registry, *handle, {0.2, 0.75, 0, 42}));
}

TEST(PerfHudInspector, MissingAndInvalidInputsHaveNoNumericValue) {
    using namespace luminumbra::core;
    InspectorRegistry registry;
    const auto handle = RegisterPerfHudInspector(registry);
    ASSERT_TRUE(handle);
    if (!handle)
        return;
    ASSERT_TRUE(PublishPerfHudInspector(registry, *handle, {0.0, {}, {}, {}}));
    const auto missing = registry.Read(*handle, 0);
    ASSERT_TRUE(missing);
    if (!missing)
        return;
    EXPECT_EQ(missing->metrics[0].availability, MetricAvailability::Pending);
    EXPECT_EQ(missing->metrics[1].availability, MetricAvailability::Unavailable);
    EXPECT_EQ(missing->metrics[2].availability, MetricAvailability::Unavailable);
    EXPECT_FALSE(missing->metrics[0].value);
    EXPECT_FALSE(missing->metrics[1].value);
    EXPECT_FALSE(missing->metrics[2].value);
    for (const double invalid : {-1.0,
                                 std::numeric_limits<double>::infinity(),
                                 std::numeric_limits<double>::quiet_NaN(),
                                 std::numeric_limits<double>::max()}) {
        ASSERT_TRUE(PublishPerfHudInspector(registry, *handle, {invalid, -1.0, {}, 1}));
        const auto snapshot = registry.Read(*handle, 1);
        ASSERT_TRUE(snapshot);
        if (!snapshot)
            return;
        EXPECT_EQ(snapshot->metrics[0].availability, MetricAvailability::Unavailable);
        EXPECT_FALSE(snapshot->metrics[0].value);
    }
    for (const double invalid : {0.0,
                                 -1.0,
                                 std::numeric_limits<double>::infinity(),
                                 std::numeric_limits<double>::quiet_NaN()}) {
        ASSERT_TRUE(PublishPerfHudInspector(registry, *handle, {0.2, invalid, {}, {}}));
        const auto snapshot = registry.Read(*handle, 1);
        ASSERT_TRUE(snapshot);
        if (!snapshot)
            return;
        EXPECT_EQ(snapshot->metrics[1].availability, MetricAvailability::Unavailable);
        EXPECT_FALSE(snapshot->metrics[1].value);
        EXPECT_FALSE(snapshot->metrics[0].age_frames);
    }
    if (std::numeric_limits<std::size_t>::max() > (std::uint64_t{1} << 53)) {
        const auto exact_limit = static_cast<std::size_t>(std::uint64_t{1} << 53);
        ASSERT_TRUE(PublishPerfHudInspector(registry, *handle, {0.2, {}, exact_limit, {}}));
        const auto exact = registry.Read(*handle, 1);
        ASSERT_TRUE(exact);
        if (!exact)
            return;
        EXPECT_EQ(exact->metrics[2].value, static_cast<double>(exact_limit));
        ASSERT_TRUE(PublishPerfHudInspector(
            registry, *handle, {0.2, {}, std::numeric_limits<std::size_t>::max(), {}}));
        const auto snapshot = registry.Read(*handle, 1);
        ASSERT_TRUE(snapshot);
        if (!snapshot)
            return;
        EXPECT_EQ(snapshot->metrics[2].availability, MetricAvailability::Unavailable);
        EXPECT_FALSE(snapshot->metrics[2].value);
    }
}

std::string LoggedPerfHud(const std::optional<luminumbra::core::InspectorPanelSnapshot>& snapshot) {
    ImGui::Begin("Inspector presentation");
    ImGui::LogToBuffer();
    DrawPerfHudContents(snapshot);
    const std::string text(ImGui::GetCurrentContext()->LogBuffer.c_str());
    ImGui::LogFinish();
    ImGui::End();
    return text;
}

TEST(PerfHudInspector, PresentationReportsSourceAgeAndEveryAvailabilityWithoutFalseZeros) {
    using namespace luminumbra::core;
    ScopedImGuiContext context;
    InspectorRegistry registry;
    const auto handle = RegisterPerfHudInspector(registry);
    ASSERT_TRUE(handle);
    if (!handle)
        return;
    for (const auto status : {MetricAvailability::Pending,
                              MetricAvailability::Unsupported,
                              MetricAvailability::Unavailable}) {
        const std::array samples{InspectorMetricSample{status, {}, {}},
                                 InspectorMetricSample{status, {}, {}},
                                 InspectorMetricSample{status, {}, {}}};
        ASSERT_TRUE(registry.Publish(*handle, samples));
        const auto text = LoggedPerfHud(registry.Read(*handle, 1));
        EXPECT_NE(text.find(std::string("unavailable (") + MetricAvailabilityName(status) + ")"),
                  std::string::npos);
        EXPECT_NE(text.find("source: CPU observed frame interval; age: unknown"),
                  std::string::npos);
        EXPECT_EQ(text.find("0.000"), std::string::npos);
    }
    ASSERT_TRUE(PublishPerfHudInspector(registry, *handle, {0.2, 0.75, 0, 42}));
    const auto available = LoggedPerfHud(registry.Read(*handle, 42));
    EXPECT_NE(available.find("5.0 FPS"), std::string::npos);
    EXPECT_NE(available.find("Terrain draws: 0.000 draws (available)"), std::string::npos);
    EXPECT_NE(available.find("source: CPU observed frame interval; age: 0 frames"),
              std::string::npos);
    const auto stale = LoggedPerfHud(registry.Read(*handle, 43));
    EXPECT_NE(stale.find("Frame interval: unavailable (stale)"), std::string::npos);
    EXPECT_NE(stale.find("age: 1 frames"), std::string::npos);
    EXPECT_EQ(stale.find("200.000"), std::string::npos);
    EXPECT_EQ(stale.find("FPS"), std::string::npos);
    ASSERT_TRUE(PublishPerfHudInspector(
        registry, *handle, {std::numeric_limits<double>::denorm_min(), {}, {}, 43}));
    EXPECT_EQ(LoggedPerfHud(registry.Read(*handle, 43)).find("FPS"), std::string::npos);
    EXPECT_NE(LoggedPerfHud(std::nullopt).find("Frame performance unavailable"), std::string::npos);
}

TEST_F(UiFixture, DefaultPassiveHudRendersWithoutShellOrFocusAndCanBeDisabled) {
    ScopedImGuiContext context;
    console.Draw(frame);
    EXPECT_EQ(ImGui::FindWindowByName("Developer console"), nullptr);
    auto* hud = ImGui::FindWindowByName("Frame performance###perf.hud");
    ASSERT_NE(hud, nullptr);
    EXPECT_TRUE(hud->Active);
    EXPECT_TRUE(hud->Flags & ImGuiWindowFlags_NoInputs);
    EXPECT_GT(hud->DrawList->VtxBuffer.Size, 0);
    EXPECT_FALSE(console.HasInteraction());
    EXPECT_EQ(draws["perf.hud"], 1);
    EXPECT_EQ(effects, 0);
    EXPECT_EQ(saves, 0);
    console.Widgets().SetVisible("perf.hud", false);
    const auto before = queries["perf.hud"];
    context.NextFrame();
    console.Draw(frame);
    EXPECT_FALSE(hud->Active);
    EXPECT_EQ(draws["perf.hud"], 1);
    EXPECT_EQ(queries["perf.hud"], before);
    EXPECT_EQ(saves, 1);
}

TEST_F(UiFixture, ShellListsCompleteInventoryAndDoesNotPollHiddenAdaptersEveryFrame) {
    ScopedImGuiContext context;
    console.SetOpen(true);
    console.Draw(frame);
    auto* shell = ImGui::FindWindowByName("Developer console");
    ASSERT_NE(shell, nullptr);
    EXPECT_TRUE(shell->Active);
    EXPECT_TRUE(console.HasInteraction());
    EXPECT_EQ(console.Widgets().List().size(), 16u);
    const auto hiddenQueries = queries["world.generation"];
    const auto unavailableQueries = queries["perf.gpu"];
    context.NextFrame();
    console.Draw(frame);
    EXPECT_EQ(queries["world.generation"], hiddenQueries);
    EXPECT_EQ(queries["perf.gpu"], unavailableQueries);
    EXPECT_EQ(draws["world.generation"], 0);
    EXPECT_EQ(draws["perf.gpu"], 0);
    EXPECT_EQ(effects, 0);
    EXPECT_EQ(saves, 1); // The shell geometry was saved once, rather than on every frame.
    console.SetOpen(false);
    context.NextFrame();
    console.Draw(frame);
    EXPECT_FALSE(shell->Active);
    EXPECT_EQ(draws["perf.hud"], 3);
}

TEST_F(UiFixture, PanelsHelpAndPassiveWidgetsDrawWithFakeServicesWithoutActions) {
    ScopedImGuiContext context;
    for (const auto& def : console.Widgets().List())
        console.Widgets().SetVisible(def.id, true);
    const int saved = saves;
    console.Draw(frame);
    EXPECT_NE(ImGui::FindWindowByName("Help###console.help"), nullptr);
    EXPECT_EQ(draws["perf.gpu"], 0);
    EXPECT_EQ(draws["world.generation"], 1);
    EXPECT_EQ(draws["runtime.time"], 1);
    EXPECT_EQ(draws["render.debug_views"], 1);
    EXPECT_EQ(draws["render.wireframe"], 1);
    EXPECT_EQ(effects, 0);   // Opening or displaying controls never performs their actions.
    EXPECT_GT(saves, saved); // Newly displayed panels persist their initial layout.
    const int layout_saves = saves;
    context.NextFrame();
    console.Draw(frame);
    EXPECT_EQ(saves, layout_saves); // Stable geometry does not write settings again.
    EXPECT_NE(console.HelpText().find("widget show"), std::string::npos);
    EXPECT_NE(console.HelpText().find("F1"), std::string::npos);
}

TEST_F(UiFixture, HiddenInventoryTracksAvailabilityOnlyAfterAnExplicitRefresh) {
    ScopedImGuiContext context;
    console.SetOpen(true);
    console.Draw(frame);
    const auto before = queries["player.state"];
    context.NextFrame();
    console.Draw(frame);
    EXPECT_EQ(queries["player.state"], before);
    console.RefreshAvailability(); // Current service owners changed on a world/menu transition.
    context.NextFrame();
    console.Draw(frame);
    EXPECT_EQ(queries["player.state"], before + 1);
    EXPECT_EQ(draws["player.state"], 0);
}

TEST_F(UiFixture, TypedConsoleToggleClosesShellAndPersistsClosedLayout) {
    ScopedImGuiContext context;
    console.SetOpen(true);
    console.Draw(frame);
    context.NextFrame();
    console.Draw(frame); // Apply the command field's requested keyboard focus.
    auto* shell = ImGui::FindWindowByName("Developer console");
    ASSERT_NE(shell, nullptr);
    ASSERT_EQ(ImGui::GetActiveID(), shell->GetID("Command"));
    auto& io = ImGui::GetIO();
    io.AddInputCharactersUTF8("console toggle");
    context.NextFrame();
    console.Draw(frame);
    io.AddKeyEvent(ImGuiKey_Enter, true);
    context.NextFrame();
    console.Draw(frame);
    EXPECT_FALSE(console.IsOpen());
    const auto layout = console.Layout();
    ASSERT_TRUE(layout.has_value());
    if (!layout.has_value())
        return;
    EXPECT_FALSE(layout->open);
    io.AddKeyEvent(ImGuiKey_Enter, false);
    context.NextFrame();
    console.Draw(frame);
    EXPECT_FALSE(shell->Active);
}

TEST_F(UiFixture, TitleBarCloseControlClosesShellAndPersistsClosedLayout) {
    ScopedImGuiContext context;
    console.SetOpen(true);
    console.Draw(frame);
    context.NextFrame();
    console.Draw(frame);
    auto* shell = ImGui::FindWindowByName("Developer console");
    ASSERT_NE(shell, nullptr);
    const auto& style = ImGui::GetStyle();
    const float radius = ImGui::GetFontSize() * 0.5f;
    auto& io = ImGui::GetIO();
    io.AddMousePosEvent(shell->Pos.x + shell->Size.x - style.FramePadding.x - radius,
                        shell->Pos.y + style.FramePadding.y + radius);
    context.NextFrame();
    console.Draw(frame);
    io.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
    context.NextFrame();
    console.Draw(frame);
    io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
    context.NextFrame();
    console.Draw(frame);
    EXPECT_FALSE(console.IsOpen());
    const auto layout = console.Layout();
    ASSERT_TRUE(layout.has_value());
    if (!layout.has_value())
        return;
    EXPECT_FALSE(layout->open);
}

TEST_F(UiFixture, PanelGeometryRestoresFromUserLayoutAndIgnoresImGuiIni) {
    ScopedImGuiContext context;
    ASSERT_EQ(ImGui::GetIO().IniFilename, nullptr);
    const DeveloperPanelLayout saved{180, 110, 610, 430, true};
    ASSERT_TRUE(console.Widgets().SetVisible("render.shaders", true));
    ASSERT_TRUE(console.Widgets().SetLayout("render.shaders", saved));
    console.Draw(frame);
    auto* panel = ImGui::FindWindowByName("Shaders###render.shaders");
    ASSERT_NE(panel, nullptr);
    EXPECT_FLOAT_EQ(panel->Pos.x, saved.x);
    EXPECT_FLOAT_EQ(panel->Pos.y, saved.y);
    EXPECT_FLOAT_EQ(panel->Size.x, saved.width);
    EXPECT_FLOAT_EQ(panel->Size.y, saved.height);
    EXPECT_TRUE(panel->Flags & ImGuiWindowFlags_NoSavedSettings);
    ImGui::SetWindowPos(panel, ImVec2(240, 160));
    ImGui::SetWindowSize(panel, ImVec2(700, 450));
    context.NextFrame();
    console.Draw(frame);
    const auto moved = console.Widgets().Layout("render.shaders");
    ASSERT_TRUE(moved.has_value());
    if (!moved.has_value())
        return;
    EXPECT_FLOAT_EQ(moved->x, 240);
    EXPECT_FLOAT_EQ(moved->width, 700);
    ImGui::SetWindowCollapsed(panel, true);
    context.NextFrame();
    console.Draw(frame);
    const auto collapsed = console.Widgets().Layout("render.shaders");
    ASSERT_TRUE(collapsed.has_value());
    if (!collapsed.has_value())
        return;
    EXPECT_FLOAT_EQ(collapsed->height, 450);
    ImGui::SetWindowCollapsed(panel, false);
    console.Widgets().SetVisible("render.shaders", false);
    context.NextFrame();
    console.Draw(frame);
    const auto closed = console.Widgets().Layout("render.shaders");
    ASSERT_TRUE(closed.has_value());
    if (!closed.has_value())
        return;
    EXPECT_FALSE(closed->open);
    console.Widgets().SetVisible("render.shaders", true);
    context.NextFrame();
    console.Draw(frame);
    EXPECT_FLOAT_EQ(panel->Pos.x, 240);
    EXPECT_FLOAT_EQ(panel->Size.x, 700);
    std::size_t ini_size = 0;
    const std::string ini = ImGui::SaveIniSettingsToMemory(&ini_size);
    EXPECT_EQ(ini.find("render.shaders"), std::string::npos);
}

TEST(DeveloperConsoleUi, ScopedContextRestoresPreviousContext) {
    auto* previous = ImGui::GetCurrentContext();
    {
        ScopedImGuiContext context;
        ASSERT_NE(ImGui::GetCurrentContext(), nullptr);
        EXPECT_NE(ImGui::GetCurrentContext(), previous);
    }
    EXPECT_EQ(ImGui::GetCurrentContext(), previous);
}
} // namespace
