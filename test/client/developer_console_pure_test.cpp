#include "app/DeveloperUiPolicy.h"
#include "app/GameplayBindingCapture.h"
#include "app/MenuPreviewInput.h"
#include "app/RegisterDeveloperWidgets.h"
#include "app/UiInputRouter.h"
#include "core/LogDispatcherSink.h"
#include "debug/DeveloperConsole.h"

#include <spdlog/logger.h>

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <map>
#include <thread>

using namespace Luminumbra::Client::Debug;
using namespace Luminumbra::Client::App;

namespace {
struct ConsoleFixture : testing::Test {
    DeveloperConsole console;
    int effects = 0;
    int saves = 0;
    bool save_ok = true;
    std::string last_effect;
    CommandArguments last_arguments;
    void SetUp() override {
        DeveloperHost host;
        host.renderer_available = [] {
            return Availability{};
        };
        host.shader_names = [] {
            return std::vector<std::string>{"all", "terrain"};
        };
        host.execute = [&](const std::string& name, const CommandArguments& args) {
            ++effects;
            last_effect = name;
            last_arguments = args;
            return CommandResult{};
        };
        host.save = [&] {
            ++saves;
            return save_ok;
        };
        console.Initialize(std::move(host));
    }
};

TEST_F(ConsoleFixture, CompleteInventoryDefaultsBindingsAndUnavailableRows) {
    const std::vector<std::string> ids{"perf.hud",
                                       "perf.gpu",
                                       "render.shaders",
                                       "shader.reload",
                                       "render.debug_views",
                                       "render.wireframe",
                                       "world.generation",
                                       "client.settings",
                                       "runtime.time",
                                       "runtime.time_indicator",
                                       "player.state",
                                       "farm.debug",
                                       "world.creature_labels",
                                       "ui.data_stream",
                                       "ui.debugger",
                                       "console.help"};
    const auto& defs = console.Widgets().List();
    ASSERT_EQ(defs.size(), ids.size());
    for (std::size_t i = 0; i < ids.size(); ++i) {
        EXPECT_EQ(defs[i].id, ids[i]);
        EXPECT_EQ(defs[i].persistence_key, ids[i]);
        EXPECT_EQ(console.Widgets().Visible(ids[i]),
                  ids[i] == "perf.hud" || ids[i] == "runtime.time_indicator");
        ASSERT_TRUE(defs[i].available);
        EXPECT_FALSE(defs[i].description.empty());
        if (ids[i] != "console.help" && ids[i] != "shader.reload") {
            EXPECT_FALSE(defs[i].available().available);
            EXPECT_FALSE(defs[i].available().reason.empty());
        }
    }
    EXPECT_EQ(console.Bindings().Get("console.toggle"), (KeyChord{290, 0}));
    EXPECT_EQ(console.Bindings().Get("perf.gpu"), (KeyChord{292, 0}));
    EXPECT_EQ(console.Bindings().Get("render.shaders"), (KeyChord{299, 0}));
    EXPECT_EQ(console.Bindings().Get("world.generation"), (KeyChord{296, 0}));
    EXPECT_EQ(console.Bindings().Get("client.settings"), (KeyChord{297, 0}));
    EXPECT_EQ(console.Bindings().Get("shader.reload"), (KeyChord{294, 0}));
    EXPECT_EQ(console.Bindings().Get("render.debug-cycle"), (KeyChord{295, 0}));
    EXPECT_EQ(console.Bindings().Get("render.wireframe-toggle"), (KeyChord{298, 0}));
    EXPECT_EQ(console.Bindings().Get("time.slower"), (KeyChord{91, 0}));
    EXPECT_EQ(console.Bindings().Get("time.faster"), (KeyChord{93, 0}));
    EXPECT_EQ(console.Bindings().Get("time.reset"), (KeyChord{92, 0}));
    EXPECT_FALSE(console.Bindings().Get("perf.hud"));
    EXPECT_FALSE(console.HasInteraction());
    EXPECT_TRUE(console.Submit("widgets").message.find("ui.debugger") != std::string::npos);
}

TEST(DeveloperWidgets, DuplicateSealIndependentVisibilityAndDeferredDrawMutations) {
    DeveloperBindings bindings;
    DeveloperWidgetRegistry widgets(bindings);
    int calls = 0, hidden_calls = 0;
    WidgetDef first;
    first.id = "first";
    first.default_visible = true;
    first.draw = [&](DeveloperFrameAdapter&) {
        ++calls;
        widgets.SetVisible("second", true);
        widgets.Toggle("first");
        widgets.Toggle("first");
    };
    WidgetDef second;
    second.id = "second";
    second.draw = [&](DeveloperFrameAdapter&) {
        ++hidden_calls;
    };
    ASSERT_TRUE(widgets.Register(first));
    EXPECT_FALSE(widgets.Register(first));
    ASSERT_TRUE(widgets.Register(second));
    DeveloperFrameAdapter frame;
    widgets.DrawVisible(frame, Presentation::ConsolePanel);
    EXPECT_EQ(calls, 1);
    EXPECT_EQ(hidden_calls, 0);
    EXPECT_TRUE(widgets.Visible("first"));
    EXPECT_TRUE(widgets.Visible("second"));
    WidgetDef late;
    late.id = "late";
    EXPECT_FALSE(widgets.Register(late));
    widgets.SetVisible("first", false);
    widgets.SetVisible("second", false);
    widgets.DrawVisible(frame, Presentation::ConsolePanel);
    EXPECT_EQ(calls, 1);
    EXPECT_EQ(hidden_calls, 0);
    widgets.RestoreDefaults();
    EXPECT_TRUE(widgets.Visible("first"));
    EXPECT_FALSE(widgets.Visible("second"));
    EXPECT_FALSE(widgets.Toggle("unknown"));
}

TEST(DeveloperWidgets, PassiveAndUnavailableRowsNeverAcquireFocusOrDrawHidden) {
    DeveloperBindings bindings;
    DeveloperWidgetRegistry widgets(bindings);
    int calls = 0, availability_calls = 0;
    WidgetDef def;
    def.id = "passive";
    def.default_visible = true;
    def.presentation = Presentation::PassiveOverlay;
    def.focus_policy = FocusPolicy::Passive;
    def.available = [&] {
        ++availability_calls;
        return Availability{};
    };
    def.draw = [&](DeveloperFrameAdapter&) {
        ++calls;
    };
    ASSERT_TRUE(widgets.Register(def));
    DeveloperFrameAdapter frame;
    EXPECT_FALSE(widgets.HasInteractivePanel());
    widgets.DrawVisible(frame, Presentation::PassiveOverlay);
    EXPECT_EQ(calls, 1);
    widgets.SetVisible("passive", false);
    const int previous = availability_calls;
    widgets.DrawVisible(frame, Presentation::PassiveOverlay);
    EXPECT_EQ(calls, 1);
    EXPECT_EQ(availability_calls, previous);
}

TEST(DeveloperWidgets, EscapeClosesMostRecentlyFocusedPanelRatherThanRegistrationOrder) {
    DeveloperBindings bindings;
    DeveloperWidgetRegistry widgets(bindings);
    WidgetDef first;
    first.id = "first";
    WidgetDef second;
    second.id = "second";
    ASSERT_TRUE(widgets.Register(first));
    ASSERT_TRUE(widgets.Register(second));
    widgets.SetVisible("second", true);
    widgets.SetVisible("first", true);
    EXPECT_TRUE(widgets.CloseTopInteraction());
    EXPECT_FALSE(widgets.Visible("first"));
    EXPECT_TRUE(widgets.Visible("second"));
    widgets.SetVisible("first", true);
    widgets.FocusInteraction("second");
    EXPECT_TRUE(widgets.CloseTopInteraction());
    EXPECT_TRUE(widgets.Visible("first"));
    EXPECT_FALSE(widgets.Visible("second"));
}

TEST(DeveloperWidgets, BindingMutationsDuringDrawAreValidatedThenDeferred) {
    DeveloperBindings bindings;
    DeveloperWidgetRegistry widgets(bindings);
    WidgetDef first;
    first.id = "first";
    first.default_visible = true;
    first.default_binding = KeyChord{300, 0};
    first.draw = [&](DeveloperFrameAdapter&) {
        std::string error;
        EXPECT_TRUE(widgets.Rebind("first", KeyChord{302, 0}, error));
        EXPECT_FALSE(widgets.Rebind("second", KeyChord{302, 0}, error));
        EXPECT_EQ(bindings.Get("first"), (KeyChord{300, 0}));
        widgets.SetVisible("first", false);
        EXPECT_TRUE(widgets.Visible("first"));
    };
    WidgetDef second;
    second.id = "second";
    second.default_binding = KeyChord{301, 0};
    ASSERT_TRUE(widgets.Register(first));
    ASSERT_TRUE(widgets.Register(second));
    DeveloperFrameAdapter frame;
    widgets.DrawVisible(frame, Presentation::ConsolePanel);
    EXPECT_EQ(bindings.Get("first"), (KeyChord{302, 0}));
    EXPECT_FALSE(widgets.Visible("first"));
    EXPECT_EQ(bindings.Get("second"), (KeyChord{301, 0}));
    widgets.RestoreDefaults();
    EXPECT_EQ(bindings.Get("first"), (KeyChord{300, 0}));
}

TEST(DeveloperBindings, KeyChordParsingFormattingAndInvalidValues) {
    for (const auto& text : {"F1", "Ctrl+F12", "Shift+Alt+A", "[", "\\", "Space", "Ctrl+320"}) {
        const auto chord = ParseKeyChord(text);
        ASSERT_TRUE(chord.has_value()) << text;
        EXPECT_EQ(ParseKeyChord(FormatKeyChord(chord)), chord);
    }
    for (const auto& text :
         {"", "Ctrl+", "F0", "F26", "200", "340", "-1", "Ctrl+Ctrl+F1", "unknown"})
        EXPECT_FALSE(ParseKeyChord(text)) << text;
    EXPECT_EQ(FormatKeyChord(std::nullopt), "Unbound");
}

TEST_F(ConsoleFixture, RebindUnbindResetAndPersistenceFailureRetry) {
    auto& bindings = console.Bindings();
    std::string error;
    ASSERT_TRUE(console.Widgets().Rebind("perf.gpu", KeyChord{300, 2}, error));
    EXPECT_EQ(bindings.Get("perf.gpu"), (KeyChord{300, 2}));
    EXPECT_FALSE(console.Dirty());
    EXPECT_EQ(saves, 1);
    ASSERT_TRUE(console.Submit("unbind perf.gpu").success);
    EXPECT_FALSE(bindings.Get("perf.gpu"));
    save_ok = false;
    ASSERT_TRUE(console.Submit("widget hide perf.hud").success);
    EXPECT_TRUE(console.Dirty());
    EXPECT_FALSE(console.Widgets().Visible("perf.hud"));
    EXPECT_FALSE(console.Submit("settings save").success);
    save_ok = true;
    EXPECT_TRUE(console.Submit("settings save").success);
    EXPECT_FALSE(console.Dirty());
    const int before = saves;
    EXPECT_TRUE(console.Submit("settings defaults developer").success);
    EXPECT_EQ(saves, before + 1);
    EXPECT_TRUE(console.Widgets().Visible("perf.hud"));
    EXPECT_EQ(bindings.Get("perf.gpu"), (KeyChord{292, 0}));
    EXPECT_EQ(effects, 0);
}

TEST(DeveloperBindings, ScopeConflictsReservationsAndPressOnly) {
    DeveloperBindings bindings;
    ASSERT_TRUE(bindings.Register({"game", KeyChord{91, 0}, Gameplay}));
    ASSERT_TRUE(bindings.Register({"photo", KeyChord{93, 0}, Photo}));
    EXPECT_FALSE(bindings.Register({"game", std::nullopt, Gameplay}));
    bindings.Reserve({256, 0}, Global | Gameplay | Photo, "Escape");
    bindings.Reserve({300, 0}, Gameplay, "gameplay");
    bindings.Reserve({300, 4}, Global | Gameplay | Photo, "window");
    std::string error;
    EXPECT_TRUE(bindings.Rebind("photo", KeyChord{91, 0}, error));
    EXPECT_FALSE(bindings.Rebind("game", KeyChord{256, 0}, error));
    EXPECT_FALSE(bindings.Rebind("game", KeyChord{300, 0}, error));
    EXPECT_FALSE(bindings.Rebind("photo", KeyChord{300, 4}, error));
    EXPECT_TRUE(bindings.Rebind("photo", KeyChord{300, 0}, error));
    EXPECT_FALSE(bindings.Match({91, 0}, 2, Gameplay));
    EXPECT_FALSE(bindings.Match({91, 0}, 0, Gameplay));
    EXPECT_EQ(bindings.Match({91, 0}, 1, Gameplay), "game");
    EXPECT_EQ(bindings.Match({300, 0}, 1, Photo), "photo");
    bindings.external_conflict = [](KeyChord chord, unsigned) {
        return chord.key == 301 ? "current gameplay key" : "";
    };
    EXPECT_FALSE(bindings.Rebind("game", KeyChord{301, 0}, error));
}

TEST_F(ConsoleFixture, ModifierCaptureWaitsEscapeCancelsAndConflictsDoNotMutate) {
    console.BeginRebind("console.toggle");
    for (const int key : {340, 341, 342, 343, 344, 345, 346, 347}) {
        console.CaptureKey(key, 15);
        EXPECT_TRUE(console.IsCapturing());
        EXPECT_EQ(saves, 0);
    }
    console.CaptureKey(256, 0);
    EXPECT_FALSE(console.IsCapturing());
    EXPECT_EQ(saves, 0);
    console.BeginRebind("console.toggle");
    console.CaptureKey(292, 0);
    EXPECT_EQ(console.Bindings().Get("console.toggle"), (KeyChord{290, 0}));
    console.BeginRebind("console.toggle");
    console.CaptureKey(301, 2);
    EXPECT_EQ(console.Bindings().Get("console.toggle"), (KeyChord{301, 2}));
}

void ExpectGameplayModifierCapture(int actionIndex, int key) {
    UiInputRouter router;
    UiInputOwnership ownership;
    ownership.binding_capture = true;
    router.SetOwnership(ownership);
    ASSERT_EQ(router.Key(key, 1, false, false), KeyRoute::Capture);
    int pendingAction = actionIndex;
    const auto captured = CaptureGameplayBinding(pendingAction, key, 1);
    ASSERT_TRUE(captured.has_value());
    EXPECT_EQ(captured->actionIndex, actionIndex);
    EXPECT_EQ(captured->key, key);
    EXPECT_EQ(pendingAction, -1);
}

TEST(GameplayBindingCapture, StandaloneLeftAndRightModifiersCompleteCapture) {
    // Crouch/Sprint occupy indices 5/6 and default to GLFW Control/Shift (341/340).
    for (const int actionIndex : {5, 6})
        for (const int key : {340, 341, 342, 343, 344, 345, 346, 347})
            ExpectGameplayModifierCapture(actionIndex, key);
}

TEST(GameplayBindingCapture, ReleasesRepeatsAndEscapePreserveBindingSemantics) {
    int pendingAction = 5;
    EXPECT_FALSE(CaptureGameplayBinding(pendingAction, 341, 0));
    EXPECT_EQ(pendingAction, 5);
    EXPECT_FALSE(CaptureGameplayBinding(pendingAction, 341, 2));
    EXPECT_EQ(pendingAction, 5);
    EXPECT_FALSE(CaptureGameplayBinding(pendingAction, 256, 1));
    EXPECT_EQ(pendingAction, -1);
    EXPECT_FALSE(CaptureGameplayBinding(pendingAction, 340, 1));
    EXPECT_EQ(pendingAction, -1);
}

TEST_F(ConsoleFixture, InvalidTypedCommandsHaveNoEffects) {
    for (const std::string line : {"unknown",
                                   "time scale",
                                   "time scale nan",
                                   "time scale inf",
                                   "time scale -1",
                                   "time scale 17",
                                   "time scale 2x",
                                   "time scale 1 extra",
                                   "render debug bogus",
                                   "render wireframe 1",
                                   "shader watch yes",
                                   "shader reload missing",
                                   "shader reload terrain extra",
                                   "widget show missing",
                                   "widget show perf.hud extra",
                                   "bind missing F2",
                                   "bind perf.hud nonsense",
                                   "bind perf.hud Ctrl+",
                                   "unbind missing",
                                   "log level missing",
                                   "help missing",
                                   "time scale 1\n",
                                   "help \"unterminated"}) {
        EXPECT_FALSE(console.Submit(line).success) << line;
    }
    EXPECT_FALSE(console.Submit(std::string(4097, 'x')).success);
    EXPECT_EQ(effects, 0);
    EXPECT_EQ(saves, 0);
    EXPECT_TRUE(console.Widgets().Visible("perf.hud"));
}

TEST_F(ConsoleFixture, TextButtonsAndBindingsDispatchTheSameOperation) {
    EXPECT_TRUE(console.Submit("shader reload all").success);
    EXPECT_EQ(last_effect, "shader reload");
    EXPECT_TRUE(console.Commands().Dispatch("shader reload", {"all"}).success);
    EXPECT_TRUE(console.InvokeAction("shader.reload").success);
    EXPECT_EQ(effects, 3);
    EXPECT_EQ(last_effect, "shader reload");
    EXPECT_TRUE(console.Submit("shader reload \"terrain\"").success);
    ASSERT_EQ(last_arguments.size(), 1u);
    EXPECT_EQ(std::get<std::string>(last_arguments[0]), "terrain");
    EXPECT_TRUE(console.Submit("time scale 0").success);
    EXPECT_EQ(std::get<float>(last_arguments[0]), 0.0f);
    EXPECT_TRUE(console.Submit("time scale 16").success);
    EXPECT_EQ(std::get<float>(last_arguments[0]), 16.0f);
    console.Commands().Dispatch("widget show", {"perf.gpu"});
    EXPECT_TRUE(console.Widgets().Visible("perf.gpu"));
    console.InvokeAction("perf.gpu");
    EXPECT_FALSE(console.Widgets().Visible("perf.gpu"));
    const auto match = console.Bindings().Match({292, 0}, 1, Gameplay);
    ASSERT_TRUE(match.has_value());
    console.InvokeAction(*match);
    EXPECT_TRUE(console.Widgets().Visible("perf.gpu"));
    EXPECT_TRUE(console.Submit("help \"time scale\"").message.find("time scale") !=
                std::string::npos);
    EXPECT_TRUE(console.Submit("bind console.toggle Ctrl+F12").success);
    EXPECT_TRUE(console.Submit("bindings").message.find("Ctrl+F12") != std::string::npos);
}

TEST_F(ConsoleFixture, HelpListsScopesForDefaultReboundAndUnboundTimeShortcuts) {
    const auto expect_help =
        [&](const std::string& slower, const std::string& faster, const std::string& reset) {
            for (const auto& help : {console.HelpText(),
                                     console.Submit("help").message,
                                     console.Submit("bindings").message}) {
                EXPECT_NE(help.find("time.slower = " + slower + " (scopes: gameplay)\n"),
                          std::string::npos);
                EXPECT_NE(help.find("time.faster = " + faster + " (scopes: gameplay)\n"),
                          std::string::npos);
                EXPECT_NE(help.find("time.reset = " + reset + " (scopes: gameplay, photo mode)\n"),
                          std::string::npos);
                EXPECT_NE(help.find("console.toggle = F1 (scopes: global, gameplay, photo mode)\n"),
                          std::string::npos);
                EXPECT_NE(help.find("perf.hud = Unbound (scopes: global, gameplay, photo mode)\n"),
                          std::string::npos);
            }
        };
    expect_help("[", "]", "\\");
    ASSERT_TRUE(console.Submit("bind time.slower Ctrl+A").success);
    ASSERT_TRUE(console.Submit("bind time.faster Ctrl+B").success);
    ASSERT_TRUE(console.Submit("bind time.reset Ctrl+C").success);
    expect_help("Ctrl+A", "Ctrl+B", "Ctrl+C");
    ASSERT_TRUE(console.Submit("unbind time.slower").success);
    ASSERT_TRUE(console.Submit("unbind time.faster").success);
    ASSERT_TRUE(console.Submit("unbind time.reset").success);
    expect_help("Unbound", "Unbound", "Unbound");
}

TEST(DeveloperCommands, UnavailableExecutionAndDuplicateDefinitions) {
    DeveloperBindings bindings;
    DeveloperWidgetRegistry widgets(bindings);
    DeveloperCommandRegistry commands(widgets, bindings);
    int effects = 0;
    CommandDef def{"effect",
                   "Unavailable effect",
                   {},
                   [] { return Availability{false, "No renderer"}; },
                   [&](const CommandArguments&) {
                       ++effects;
                       return CommandResult{};
                   }};
    EXPECT_TRUE(commands.Register(def));
    EXPECT_FALSE(commands.Register(def));
    EXPECT_FALSE(commands.Submit("effect").success);
    EXPECT_EQ(effects, 0);
    EXPECT_TRUE(commands.Help().find("No renderer") != std::string::npos);
    commands.Seal();
    def.name = "late";
    EXPECT_FALSE(commands.Register(def));
}

TEST(DeveloperHistory, BoundariesDuplicatesBlankAndEditedDraftRestoration) {
    DeveloperHistory history;
    EXPECT_EQ(history.Up("draft"), "draft");
    EXPECT_EQ(history.Down(), "draft");
    history.Submit("  ");
    history.Submit("one");
    history.Submit("one");
    history.Submit("two");
    ASSERT_EQ(history.Entries().size(), 2u);
    EXPECT_EQ(history.Up("edited draft"), "two");
    EXPECT_EQ(history.Up("changed entry"), "one");
    EXPECT_EQ(history.Up(""), "one");
    EXPECT_EQ(history.Down(), "two");
    EXPECT_EQ(history.Down(), "edited draft");
    EXPECT_EQ(history.Down(), "edited draft");
    for (int i = 0; i < 140; ++i)
        history.Submit(std::to_string(i));
    ASSERT_EQ(history.Entries().size(), 128u);
    EXPECT_EQ(history.Entries().front(), "12");
    history.Submit(std::string(4097, 'x'));
    EXPECT_EQ(history.Entries().back(), "139");
}

TEST(DeveloperLog, CountBytesTruncationSeverityTimestampSequenceAndNewestRetention) {
    DeveloperLogSink log;
    for (std::size_t i = 0; i < DeveloperLogSink::kRecordLimit + 7; ++i)
        log.Push(LogSeverity::Info, std::to_string(i));
    auto snapshot = log.Snapshot();
    ASSERT_EQ(snapshot.size(), DeveloperLogSink::kRecordLimit);
    EXPECT_EQ(log.Evictions(), 7u);
    EXPECT_EQ(snapshot.front().sequence, 8u);
    log.Clear();
    for (int i = 0; i < 70; ++i)
        log.Push(LogSeverity::Warning, std::string(8192, 'a'));
    EXPECT_EQ(log.RetainedBytes(), DeveloperLogSink::kByteLimit);
    EXPECT_EQ(log.Snapshot().size(), 64u);
    const auto timestamp = std::chrono::system_clock::time_point(std::chrono::seconds(123));
    log.Push(LogSeverity::Error, std::string(9000, 'b'), timestamp);
    snapshot = log.Snapshot(LogSeverity::Error);
    ASSERT_EQ(snapshot.size(), 1u);
    EXPECT_EQ(snapshot[0].message.size(), 8192u);
    EXPECT_EQ(snapshot[0].timestamp, timestamp);
    EXPECT_EQ(log.Truncations(), 1u);
    EXPECT_LE(log.RetainedBytes(), DeveloperLogSink::kByteLimit);
    EXPECT_TRUE(log.Snapshot(LogSeverity::Critical).empty());
}

TEST(DeveloperLog, ConcurrentProducersSnapshotsAndDetachment) {
    auto log = std::make_shared<DeveloperLogSink>();
    std::atomic<bool> start{false};
    std::vector<std::thread> producers;
    for (int t = 0; t < 4; ++t)
        producers.emplace_back([log, &start] {
            while (!start.load())
                std::this_thread::yield();
            for (int i = 0; i < 3000; ++i)
                log->Push(LogSeverity::Info, "record");
        });
    start = true;
    for (int i = 0; i < 8; ++i) {
        const auto records = log->Snapshot();
        EXPECT_LE(records.size(), 2048u);
    }
    log->Close();
    for (auto& producer : producers)
        producer.join();
    const auto records = log->Snapshot();
    log->Push(LogSeverity::Critical, "after teardown");
    EXPECT_EQ(log->Snapshot().size(), records.size());
    for (std::size_t i = 1; i < records.size(); ++i)
        EXPECT_LT(records[i - 1].sequence, records[i].sequence);
}

TEST_F(ConsoleFixture, ConsoleLogFilterClearAndSafeProducerLifetime) {
    auto producer = console.Log();
    producer->Push(LogSeverity::Trace, "retained");
    console.Submit("log level error");
    EXPECT_FALSE(producer->Snapshot().empty()); // display filter never changes ingestion
    console.Submit("log clear");
    EXPECT_TRUE(producer->Snapshot().empty());
    console.Shutdown();
    producer->Push(LogSeverity::Error, "detached");
    EXPECT_TRUE(producer->Snapshot().empty());
}

UiInputOwnership World() {
    UiInputOwnership state;
    state.in_world = true;
    return state;
}

TEST(DeveloperRouting, CapturedTypingBlocksEveryGameplayPollAndTimeShortcut) {
    UiInputRouter router;
    auto state = World();
    router.SetOwnership(state);
    state.console_open = true;
    state.imgui_text_input = true;
    router.SetOwnership(state);
    for (const int key : {87, 32, 70, 71, 72, 74, 82, 84, 257, 91, 93, 92}) {
        EXPECT_EQ(router.Key(key, 1, false, key >= 91 && key <= 93), KeyRoute::Ignore);
        EXPECT_FALSE(router.GameplayKeyAllowed(key));
    }
    EXPECT_FALSE(router.GameplayMouseAllowed());
    EXPECT_FALSE(router.CursorCaptured());
    state.console_open = false;
    state.imgui_text_input = false;
    router.SetOwnership(state);
    EXPECT_FALSE(router.GameplayKeyAllowed(87));
    EXPECT_EQ(router.Key(87, 2, false, false), KeyRoute::Ignore);
    router.Key(87, 0, false, false);
    EXPECT_TRUE(router.GameplayKeyAllowed(87));
    EXPECT_EQ(router.Key(87, 1, false, false), KeyRoute::Gameplay);
}

void ExpectPreviewMouseBlocked(UiInputOwnership ownership) {
    UiInputRouter router;
    router.SetOwnership(ownership);
    MenuPreviewState preview;
    preview.dragging = true;
    preview.lastCursorX = 120.0;
    preview.lastCursorY = 80.0;
    EXPECT_FALSE(preview.UpdateInputOwnership(router));
    EXPECT_FALSE(preview.dragging);
    EXPECT_DOUBLE_EQ(preview.lastCursorX, 120.0);
    EXPECT_DOUBLE_EQ(preview.lastCursorY, 80.0);
    router.SetOwnership({});
    EXPECT_TRUE(preview.UpdateInputOwnership(router));
    EXPECT_FALSE(preview.dragging); // Returning ownership does not resume the old drag.
}

TEST(DeveloperRouting, PreviewMouseOwnershipCancelsDragForDeveloperUiAndFocusLoss) {
    UiInputRouter router;
    MenuPreviewState preview;
    EXPECT_TRUE(preview.UpdateInputOwnership(router));
    preview.dragging = true;
    EXPECT_TRUE(preview.UpdateInputOwnership(router));
    EXPECT_TRUE(preview.dragging);
    for (const auto flag : {&UiInputOwnership::console_open,
                            &UiInputOwnership::interactive_panel,
                            &UiInputOwnership::binding_capture,
                            &UiInputOwnership::imgui_mouse,
                            &UiInputOwnership::imgui_keyboard,
                            &UiInputOwnership::imgui_text_input}) {
        UiInputOwnership ownership;
        ownership.*flag = true;
        ExpectPreviewMouseBlocked(ownership);
    }
    UiInputOwnership unfocused;
    unfocused.window_focused = false;
    ExpectPreviewMouseBlocked(unfocused);
}

TEST(DeveloperRouting, CaptureToggleEscapePriorityRepeatAndUnicodeDelivery) {
    UiInputRouter router;
    auto state = World();
    state.binding_capture = true;
    state.console_open = true;
    router.SetOwnership(state);
    EXPECT_EQ(router.Key(290, 1, true, true), KeyRoute::Capture);
    EXPECT_EQ(router.Key(256, 1, false, false), KeyRoute::Capture);
    state.binding_capture = false;
    router.SetOwnership(state);
    EXPECT_EQ(router.Key(290, 1, true, true), KeyRoute::ConsoleToggle);
    EXPECT_EQ(router.Key(290, 2, true, true), KeyRoute::Ignore);
    EXPECT_EQ(router.Key(256, 1, false, false), KeyRoute::DeveloperEscape);
    EXPECT_EQ(router.Character(0x1f33f), InputOwner::Developer);
    EXPECT_EQ(router.Character(0xd800), InputOwner::Unfocused);
    EXPECT_EQ(router.Character(0x110000), InputOwner::Unfocused);
    state.console_open = false;
    state.rml_interaction = true;
    router.SetOwnership(state);
    EXPECT_EQ(router.Character(0x00e9), InputOwner::RmlUi);
    EXPECT_EQ(router.Key(65, 1, false, false), KeyRoute::RmlUi);
}

TEST(DeveloperRouting, ApertureCollisionAndPressVersusRepeat) {
    EXPECT_FALSE(UiInputRouter::TimeShortcutAllowed(true, 91));
    EXPECT_FALSE(UiInputRouter::TimeShortcutAllowed(true, 93));
    EXPECT_TRUE(UiInputRouter::TimeShortcutAllowed(true, 92));
    EXPECT_TRUE(UiInputRouter::TimeShortcutAllowed(false, 91));
    DeveloperConsole console;
    console.Initialize({});
    EXPECT_FALSE(console.Bindings().Match({91, 0}, 1, Photo));
    EXPECT_TRUE(console.Bindings().Match({91, 0}, 1, Gameplay));
    EXPECT_TRUE(console.Bindings().Match({92, 0}, 1, Photo));
    EXPECT_FALSE(console.Bindings().Match({91, 0}, 2, Gameplay));
    UiInputRouter router;
    router.SetOwnership(World());
    EXPECT_EQ(router.Key(91, 1, false, false), KeyRoute::Gameplay);
    EXPECT_EQ(router.Key(91, 2, false, false), KeyRoute::Gameplay);
}

TEST(DeveloperRouting, OverlappingPanelsImmediateCaptureCursorEscapeAndTransitions) {
    DeveloperConsole console;
    console.Initialize({});
    UiInputRouter router;
    auto state = World();
    router.SetOwnership(state);
    EXPECT_TRUE(router.CursorCaptured());
    router.Key(70, 1, false, false);
    console.SetOpen(true);
    console.Widgets().SetVisible("console.help", true);
    state.console_open = console.IsOpen();
    state.interactive_panel = console.Widgets().HasInteractivePanel();
    const auto revision = router.Revision();
    EXPECT_TRUE(router.SetOwnership(state));
    EXPECT_GT(router.Revision(), revision);
    EXPECT_FALSE(router.GameplayKeyAllowed(70));
    EXPECT_FALSE(router.CursorCaptured());
    EXPECT_TRUE(console.CloseTopInteraction());
    EXPECT_TRUE(console.IsOpen());
    state.interactive_panel = console.Widgets().HasInteractivePanel();
    router.SetOwnership(state);
    EXPECT_FALSE(router.CursorCaptured());
    EXPECT_TRUE(console.CloseTopInteraction());
    EXPECT_FALSE(console.IsOpen());
    state.console_open = false;
    router.SetOwnership(state);
    EXPECT_TRUE(router.CursorCaptured());
    EXPECT_FALSE(router.GameplayKeyAllowed(70));
    router.Key(70, 0, false, false);
    EXPECT_TRUE(router.GameplayKeyAllowed(70));
    state.in_world = false;
    router.SetOwnership(state);
    EXPECT_EQ(router.Owner(), InputOwner::RmlUi);
    EXPECT_FALSE(router.CursorCaptured());
    state.in_world = true;
    state.window_focused = false;
    router.SetOwnership(state);
    EXPECT_EQ(router.Key(87, 1, false, false), KeyRoute::Ignore);
    state.window_focused = true;
    router.SetOwnership(state);
    EXPECT_FALSE(router.GameplayKeyAllowed(87));
    router.Key(87, 0, false, false);
    EXPECT_TRUE(router.CursorCaptured());
}

TEST(DeveloperLog, DispatcherAttachDetachAgainstConcurrentLoggerCallsAndFlushes) {
    class Target final : public spdlog::sinks::base_sink<std::mutex> {
    public:
        DeveloperLogSink records;

    protected:
        void sink_it_(const spdlog::details::log_msg& message) override {
            records.Push(LogSeverity::Info,
                         std::string(message.payload.data(), message.payload.size()));
        }
        void flush_() override {}
    };
    auto dispatcher = std::make_shared<LogDispatcherSink>();
    spdlog::logger logger("dispatcher-test", dispatcher);
    std::atomic<bool> start{false};
    std::vector<std::thread> producers;
    for (int t = 0; t < 4; ++t)
        producers.emplace_back([&] {
            while (!start.load())
                std::this_thread::yield();
            for (int i = 0; i < 3000; ++i) {
                logger.info("record {}", i);
                logger.flush();
            }
        });
    start = true;
    for (int i = 0; i < 1000; ++i) {
        auto target = std::make_shared<Target>();
        dispatcher->Attach(target);
        logger.info("attached");
        EXPECT_FALSE(target->records.Snapshot().empty());
        dispatcher->Detach(target);
        const auto records = target->records.Snapshot();
        logger.info("detached");
        EXPECT_EQ(target->records.Snapshot().size(), records.size());
    }
    for (auto& producer : producers)
        producer.join();
    auto old = std::make_shared<Target>();
    auto current = std::make_shared<Target>();
    dispatcher->Attach(old);
    dispatcher->Attach(current);
    dispatcher->Detach(old); // stale owner must not detach a replacement
    logger.info("replacement");
    EXPECT_EQ(current->records.Snapshot().size(), 1u);
    EXPECT_TRUE(old->records.Snapshot().empty());
    ASSERT_EQ(logger.sinks().size(), 1u);
    EXPECT_EQ(logger.sinks().front(), dispatcher);
    dispatcher->Detach(current);
}

TEST(DeveloperLog, OversizedAndReservedProducerCapacityIsNeverRetained) {
    DeveloperLogSink log;
    std::string oversized(4 * DeveloperLogSink::kByteLimit, 'x');
    log.Push(LogSeverity::Info, std::move(oversized));
    ASSERT_EQ(log.Snapshot().size(), 1u);
    EXPECT_EQ(log.RetainedBytes(), DeveloperLogSink::kMessageLimit);
    EXPECT_LE(log.RetainedCapacity(), 2 * DeveloperLogSink::kMessageLimit);
    EXPECT_EQ(log.Truncations(), 1u);
    log.Clear();
    for (int i = 0; i < 32; ++i) {
        std::string reserved = "short";
        reserved.reserve(4 * DeveloperLogSink::kByteLimit);
        log.Push(LogSeverity::Info, std::move(reserved));
    }
    EXPECT_EQ(log.RetainedBytes(), 32u * 5u);
    EXPECT_LE(log.RetainedCapacity(), 32u * 32u);
    EXPECT_EQ(log.Truncations(), 1u);
}

TEST(DeveloperWidgets, CompleteAdaptersShareRowsCommandsShortcutsAndCloseButtons) {
    DeveloperWidgetServices services;
    std::map<std::string, int> draws, availability;
    for (const auto& def : BuiltinDeveloperWidgets()) {
        const auto id = def.id;
        services[id] = {[&, id] {
                            ++availability[id];
                            return Availability{};
                        },
                        [&, id](DeveloperFrameAdapter&) {
                            ++draws[id];
                        }};
    }
    DeveloperConsole console;
    console.Initialize({}, RegisterDeveloperWidgets(services));
    DeveloperFrameAdapter frame;
    frame.draw = [&](const WidgetDef&, const DrawFn& contents) {
        contents(frame);
    };
    ASSERT_EQ(console.Widgets().List().size(), 16u);
    console.Widgets().DrawVisible(frame, Presentation::PassiveOverlay);
    EXPECT_EQ(draws["perf.hud"], 1);
    EXPECT_EQ(draws["runtime.time_indicator"], 1);
    for (const auto& def : console.Widgets().List()) {
        if (def.id == "console.help")
            continue;
        EXPECT_EQ(static_cast<bool>(def.draw), def.presentation != Presentation::ActionOnly);
        EXPECT_TRUE(def.available || services.contains(def.id));
        if (!def.default_visible) {
            EXPECT_EQ(draws[def.id], 0);
            EXPECT_EQ(availability[def.id], 0);
        }
    }
    for (const auto& id : {"perf.gpu", "world.generation", "client.settings", "render.shaders"}) {
        EXPECT_TRUE(console.Submit(std::string("widget show ") + id).success);
        EXPECT_TRUE(console.Widgets().Visible(id));
        console.Widgets().DrawVisible(frame, Presentation::ConsolePanel);
        EXPECT_EQ(draws[id], 1);
        EXPECT_TRUE(console.Widgets().SetVisible(id, false)); // shared window close operation
        EXPECT_FALSE(console.Widgets().Visible(id));
        const auto chord = console.Bindings().Get(id);
        ASSERT_TRUE(chord.has_value());
        const auto shortcut = console.Bindings().Match(*chord, 1, Gameplay);
        ASSERT_TRUE(shortcut.has_value());
        UiInputRouter router;
        router.SetOwnership(World());
        ASSERT_EQ(router.Key(chord->key, 1, false, true), KeyRoute::DeveloperShortcut);
        EXPECT_TRUE(console.InvokeAction(*shortcut).success);
        EXPECT_TRUE(console.Widgets().Visible(id));
        EXPECT_TRUE(console.Widgets().HasInteractivePanel());
        auto ownership = World();
        ownership.interactive_panel = true;
        router.SetOwnership(ownership);
        EXPECT_EQ(router.Key(chord->key, 1, false, true), KeyRoute::DeveloperShortcut);
        EXPECT_TRUE(console.InvokeAction(*shortcut).success);
        EXPECT_FALSE(console.Widgets().Visible(id));
        const int before = draws[id], queries = availability[id];
        console.Widgets().DrawVisible(frame, Presentation::ConsolePanel);
        EXPECT_EQ(draws[id], before);
        EXPECT_EQ(availability[id], queries);
    }
}

TEST(DeveloperWidgets, EveryModuleCanRebindUnbindAndResetWithoutDuplicateVisibilityBindings) {
    DeveloperWidgetServices services;
    for (const auto& def : BuiltinDeveloperWidgets())
        services[def.id] = {{}, [](DeveloperFrameAdapter&) {
                            }};
    DeveloperHost host;
    host.renderer_available = [] {
        return Availability{};
    };
    host.execute = [](const std::string&, const CommandArguments&) {
        return CommandResult{};
    };
    DeveloperConsole console;
    console.Initialize(std::move(host), RegisterDeveloperWidgets(services));
    int key = 65;
    for (const auto& def : console.Widgets().List()) {
        const bool panel = def.presentation != Presentation::ActionOnly;
        EXPECT_EQ(console.Submit("widget show " + def.id).success, panel);
        EXPECT_EQ(console.Widgets().Visible(def.id), panel);
        EXPECT_EQ(console.Submit("widget hide " + def.id).success, panel);
        EXPECT_FALSE(console.Widgets().Visible(def.id));
        for (const auto& binding : def.BindingDefinitions()) {
            std::string error;
            const KeyChord chord{key++, 2};
            EXPECT_TRUE(console.Bindings().Rebind(binding.id, chord, error)) << error;
            const auto matched = console.Bindings().Match(chord, 1, Gameplay);
            ASSERT_TRUE(matched.has_value());
            EXPECT_EQ(*matched, binding.id);
            EXPECT_TRUE(console.InvokeAction(*matched).success);
            EXPECT_EQ(console.Widgets().Visible(def.id), panel && def.shortcuts.empty());
            if (panel) {
                EXPECT_TRUE(console.Submit("widget toggle " + def.id).success);
                console.Widgets().SetVisible(def.id, false);
            }
            EXPECT_TRUE(console.Submit("unbind " + binding.id).success);
            EXPECT_FALSE(console.Bindings().Get(binding.id));
        }
    }
    for (const auto* id : {"render.debug_views", "render.wireframe", "runtime.time"})
        EXPECT_FALSE(console.Bindings().Contains(id));
    console.Widgets().RestoreDefaults();
    for (const auto& def : console.Widgets().List()) {
        EXPECT_EQ(console.Widgets().Visible(def.id), def.default_visible);
        for (const auto& binding : def.BindingDefinitions())
            EXPECT_EQ(console.Bindings().Get(binding.id), binding.default_binding);
    }
}

TEST(DeveloperWidgets, MissingServicesStayListedAndResolveCurrentOwners) {
    int current = 1, observed = 0;
    DeveloperWidgetServices services;
    services["player.state"] = {[&] { return Availability{current != 0, "No player"}; },
                                [&](DeveloperFrameAdapter&) {
                                    observed = current;
                                }};
    DeveloperConsole console;
    console.Initialize({}, RegisterDeveloperWidgets(services));
    EXPECT_EQ(console.Widgets().List().size(), 16u);
    const auto* missing = console.Widgets().Find("perf.gpu");
    ASSERT_NE(missing, nullptr);
    EXPECT_FALSE(missing->available().available);
    EXPECT_FALSE(missing->available().reason.empty());
    EXPECT_TRUE(console.InvokeAction("perf.gpu").success);
    EXPECT_FALSE(console.Widgets().HasInteractivePanel());
    DeveloperFrameAdapter frame;
    frame.draw = [&](const WidgetDef&, const DrawFn& contents) {
        contents(frame);
    };
    console.Widgets().SetVisible("player.state", true);
    console.Widgets().DrawVisible(frame, Presentation::ConsolePanel);
    EXPECT_EQ(observed, 1);
    current = 0;
    console.Widgets().DrawVisible(frame, Presentation::ConsolePanel);
    EXPECT_EQ(observed, 1);
    current = 2;
    console.Widgets().DrawVisible(frame, Presentation::ConsolePanel);
    EXPECT_EQ(observed, 2);
}

TEST(UiInputRouter, ExternalDebuggerBlocksGameplayAndRetainsDeveloperEscapeOrdering) {
    UiInputRouter router;
    auto ownership = World();
    ownership.rml_debugger = ownership.rml_interaction = true;
    router.SetOwnership(ownership);
    EXPECT_EQ(router.Owner(), InputOwner::RmlUi);
    EXPECT_FALSE(router.GameplayAllowed());
    EXPECT_FALSE(router.GameplayMouseAllowed());
    EXPECT_FALSE(router.CursorCaptured());
    EXPECT_EQ(router.Key(256, 1, false, false), KeyRoute::DeveloperEscape);
    EXPECT_EQ(router.Key(290, 1, true, false), KeyRoute::ConsoleToggle);
    ownership.console_open = true;
    router.SetOwnership(ownership);
    EXPECT_EQ(router.Owner(), InputOwner::Developer);
}

TEST(DeveloperBindings, CompleteDefaultsValidateAtStartupAndResetWithGameplayWinning) {
    DeveloperConsole console;
    DeveloperHost host;
    host.save = [] {
        return true;
    };
    console.Initialize(std::move(host));
    int changes = 0;
    console.Bindings().changed = [&] {
        ++changes;
    };
    console.Bindings().external_conflict = [](KeyChord chord, unsigned) {
        return chord.key == 294 ? "Conflicts with gameplay: MoveForward" : "";
    };
    console.Bindings().Validate();
    EXPECT_FALSE(console.Bindings().Get("shader.reload"));
    EXPECT_EQ(console.Bindings().Get("console.toggle"), (KeyChord{290, 0}));
    EXPECT_EQ(changes, 1);
    const auto warning = console.Log()->Snapshot(LogSeverity::Warning);
    ASSERT_EQ(warning.size(), 1u);
    EXPECT_NE(warning[0].message.find("shader.reload"), std::string::npos);
    EXPECT_NE(warning[0].message.find("F5"), std::string::npos);
    EXPECT_NE(warning[0].message.find("unbound"), std::string::npos);
    EXPECT_NE(warning[0].message.find("MoveForward"), std::string::npos);
    std::string error;
    ASSERT_TRUE(console.Bindings().Rebind("shader.reload", KeyChord{301, 0}, error));
    EXPECT_TRUE(console.Submit("settings defaults developer").success);
    EXPECT_FALSE(console.Bindings().Get("shader.reload"));
    EXPECT_FALSE(console.Bindings().Match({294, 0}, 1, Gameplay));
    EXPECT_EQ(console.Log()->Snapshot(LogSeverity::Warning).size(), 2u);
    console.Bindings().Validate();
    EXPECT_EQ(console.Log()->Snapshot(LogSeverity::Warning).size(), 2u);
}

TEST(DeveloperBindings, ResetDuringDrawAlsoValidatesDefaultsAndReservations) {
    DeveloperBindings bindings;
    DeveloperWidgetRegistry widgets(bindings);
    int warnings = 0;
    WidgetDef def;
    def.id = "panel";
    def.default_visible = true;
    def.default_binding = KeyChord{294, 0};
    def.draw = [&](DeveloperFrameAdapter&) {
        widgets.RestoreDefaults();
    };
    ASSERT_TRUE(widgets.Register(def));
    bindings.conflict_reported = [&](const std::string&) {
        ++warnings;
    };
    bindings.Reserve({294, 0}, Gameplay, "gameplay");
    DeveloperFrameAdapter frame;
    widgets.DrawVisible(frame, Presentation::ConsolePanel);
    EXPECT_FALSE(bindings.Get("panel"));
    EXPECT_EQ(warnings, 1);
}

TEST(DeveloperRouting, CallbackSequencePreservesPrintableRmlTextAndSuppressesToggleCharacters) {
    DeveloperConsole console;
    console.Initialize({});
    std::string error;
    ASSERT_TRUE(console.Bindings().Rebind("console.toggle", KeyChord{96, 0}, error));
    UiInputRouter router;
    auto state = World();
    state.rml_interaction = true;
    state.rml_text_input = true;
    state.imgui_keyboard = true; // stale backend capture cannot override explicit RmlUi typing
    router.SetOwnership(state);
    // Same key -> ownership refresh -> char route used by the application callbacks.
    EXPECT_EQ(router.Key(96, 1, true, true), KeyRoute::RmlUi);
    router.SetOwnership(state);
    EXPECT_EQ(router.Character('`'), InputOwner::RmlUi);
    EXPECT_FALSE(console.IsOpen());
    EXPECT_EQ(router.Key(96, 2, true, false), KeyRoute::RmlUi);
    EXPECT_EQ(router.Character('`'), InputOwner::RmlUi);
    router.Key(96, 0, true, false);
    state.rml_text_input = false; // a button has focus instead
    router.SetOwnership(state);
    ASSERT_EQ(router.Key(96, 1, true, true), KeyRoute::ConsoleToggle);
    console.InvokeAction("console.toggle");
    state.console_open = console.IsOpen();
    router.SetOwnership(state);
    EXPECT_EQ(router.Character('`'), InputOwner::Unfocused);      // no UI backend receives it
    EXPECT_EQ(router.Key(96, 2, false, false), KeyRoute::Ignore); // modifier changed while held
    EXPECT_EQ(router.Character('`'), InputOwner::Unfocused);
    router.Key(96, 0, true, false);
    EXPECT_EQ(router.Key(65, 1, false, false), KeyRoute::Ignore);
    EXPECT_EQ(router.Character('a'), InputOwner::Developer); // ordinary console typing
    ASSERT_EQ(router.Key(96, 1, true, true), KeyRoute::ConsoleToggle);
    console.InvokeAction("console.toggle");
    state.console_open = console.IsOpen();
    router.SetOwnership(state);
    EXPECT_EQ(router.Character('`'), InputOwner::Unfocused); // closing toggle also suppressed
    router.Key(96, 0, true, false);
    ASSERT_TRUE(console.Bindings().Rebind("console.toggle", KeyChord{290, 0}, error));
    state.rml_text_input = true;
    router.SetOwnership(state);
    EXPECT_EQ(router.Key(290, 1, true, true), KeyRoute::ConsoleToggle); // F1 still available
}

void ExpectPanelShortcut(DeveloperConsole& console, UiInputRouter& router, const char* id) {
    const auto chord = console.Bindings().Get(id);
    ASSERT_TRUE(chord.has_value());
    const auto matched = console.Bindings().Match(*chord, 1, Gameplay);
    ASSERT_TRUE(matched.has_value());
    ASSERT_EQ(router.Key(chord->key, 1, false, true), KeyRoute::DeveloperShortcut) << id;
    EXPECT_TRUE(console.InvokeAction(*matched).success) << id;
    EXPECT_EQ(router.Character(static_cast<unsigned>(chord->key)), InputOwner::Unfocused);
    EXPECT_EQ(router.Key(chord->key, 2, false, true), KeyRoute::Ignore);
    router.Key(chord->key, 0, false, false);
    EXPECT_FALSE(router.GameplayAllowed());
}

TEST_F(ConsoleFixture, PanelOwnershipAllowsDeveloperActionsAndReboundPanelClose) {
    UiInputRouter router;
    auto state = World();
    state.interactive_panel = true;
    state.imgui_keyboard = true;
    router.SetOwnership(state);
    for (const auto* id : {"shader.reload",
                           "render.debug-cycle",
                           "render.wireframe-toggle",
                           "time.slower",
                           "time.faster",
                           "time.reset"})
        ExpectPanelShortcut(console, router, id);
    EXPECT_EQ(effects, 6);
    std::string error;
    ASSERT_TRUE(console.Bindings().Rebind("render.shaders", KeyChord{75, 2}, error));
    ASSERT_TRUE(console.Widgets().SetVisible("render.shaders", true));
    const auto matched = console.Bindings().Match({75, 2}, 1, Gameplay);
    ASSERT_TRUE(matched.has_value());
    ASSERT_EQ(router.Key(75, 1, false, true), KeyRoute::DeveloperShortcut);
    EXPECT_TRUE(console.InvokeAction(*matched).success);
    EXPECT_FALSE(console.Widgets().Visible("render.shaders"));
    EXPECT_EQ(router.Character('k'), InputOwner::Unfocused);
}

TEST(DeveloperRouting, TextCaptureBlocksFunctionActionsTimeControlsAndReboundPanels) {
    for (const int text_owner : {0, 1, 2}) {
        UiInputRouter router;
        auto state = World();
        state.interactive_panel = text_owner != 2;
        state.console_open = text_owner == 0;
        state.imgui_text_input = text_owner != 2;
        state.rml_text_input = text_owner == 2;
        state.rml_interaction = text_owner == 2;
        router.SetOwnership(state);
        for (const int key : {294, 295, 298, 91, 93, 92, 75}) {
            EXPECT_EQ(router.Key(key, 1, false, true),
                      text_owner == 2 ? KeyRoute::RmlUi : KeyRoute::Ignore);
            EXPECT_FALSE(router.GameplayAllowed());
            EXPECT_EQ(router.Character('x'),
                      text_owner == 2 ? InputOwner::RmlUi : InputOwner::Developer);
            router.Key(key, 0, false, false);
        }
    }
}

TEST(DeveloperCaptureSuppression, NormalPlayAndEveryAutomatedOverride) {
    DeveloperUiPolicy normal;
    EXPECT_TRUE(DeveloperUiAllowed(normal));
    auto no_ui = normal;
    no_ui.imgui_enabled = false;
    EXPECT_FALSE(DeveloperUiAllowed(no_ui));
    const std::vector<bool DeveloperUiPolicy::*> overrides{&DeveloperUiPolicy::scenario_active,
                                                           &DeveloperUiPolicy::timelapse,
                                                           &DeveloperUiPolicy::scene,
                                                           &DeveloperUiPolicy::frame_scan,
                                                           &DeveloperUiPolicy::render_parity,
                                                           &DeveloperUiPolicy::render_benchmark,
                                                           &DeveloperUiPolicy::survey,
                                                           &DeveloperUiPolicy::bake_impostor,
                                                           &DeveloperUiPolicy::ui_screenshot,
                                                           &DeveloperUiPolicy::ui_thumbnails};
    for (const auto flag : overrides) {
        auto capture = normal;
        capture.*flag = true;
        EXPECT_FALSE(DeveloperUiAllowed(capture));
        capture.*flag = false;
        EXPECT_TRUE(DeveloperUiAllowed(capture));
    }
}

TEST_F(ConsoleFixture, PanelLayoutFollowsVisibilityAndRejectsPassiveOrInvalidGeometry) {
    EXPECT_FALSE(console.Widgets().Layout("render.shaders"));
    ASSERT_TRUE(console.Widgets().SetVisible("render.shaders", true));
    const DeveloperPanelLayout layout{120, 80, 640, 480, true};
    ASSERT_TRUE(console.Widgets().SetLayout("render.shaders", layout));
    EXPECT_EQ(console.Widgets().Layout("render.shaders"), layout);
    const int saved = saves;
    EXPECT_TRUE(console.Widgets().SetLayout("render.shaders", layout));
    EXPECT_EQ(saves, saved);
    ASSERT_TRUE(console.Widgets().SetVisible("render.shaders", false));
    const auto closed_panel = console.Widgets().Layout("render.shaders");
    ASSERT_TRUE(closed_panel.has_value());
    EXPECT_FALSE(closed_panel->open);
    EXPECT_EQ(closed_panel->width, 640);
    EXPECT_FALSE(console.Widgets().SetLayout("perf.hud", layout));
    EXPECT_FALSE(console.Widgets().SetLayout("shader.reload", layout));
    auto invalid = layout;
    invalid.width = -1;
    EXPECT_FALSE(console.Widgets().SetLayout("render.shaders", invalid));
    console.RestoreLayout(layout);
    EXPECT_TRUE(console.IsOpen());
    console.SetOpen(false);
    const auto closed_console = console.Layout();
    ASSERT_TRUE(closed_console.has_value());
    EXPECT_FALSE(closed_console->open);
    EXPECT_EQ(closed_console->x, 120);
}

TEST_F(ConsoleFixture, ModuleHelpIncludesDescriptionsAndResolvedActionHotkeys) {
    for (const auto& def : console.Widgets().List()) {
        const auto help = console.Submit("widgets").message;
        EXPECT_NE(help.find(def.description), std::string::npos);
        for (const auto& binding : def.BindingDefinitions())
            EXPECT_NE(
                help.find(binding.id + " = " + FormatKeyChord(console.Bindings().Get(binding.id))),
                std::string::npos);
    }
    const auto* reload = console.Widgets().Find("shader.reload");
    ASSERT_NE(reload, nullptr);
    EXPECT_EQ(reload->presentation, Presentation::ActionOnly);
    EXPECT_FALSE(reload->draw);
    EXPECT_FALSE(console.Submit("widget show shader.reload").success);
    EXPECT_TRUE(console.Submit("bind shader.reload Ctrl+F5").success);
    EXPECT_NE(console.Submit("widgets").message.find("shader.reload = Ctrl+F5"), std::string::npos);
}
} // namespace
