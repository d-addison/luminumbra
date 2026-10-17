#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

#include "../support/SimulationContentFixture.h"
#include "../support/UniqueTempPath.h"
#include "luminumbra_client/app/DebugOverlays.h"
#include "luminumbra_client/app/InputCallbacks.h"
#include "luminumbra_client/app/RegionPanel.h"
#include "luminumbra_client/player/PlayerController.h"
#include "luminumbra_client/rendering/Camera.h"
#include "luminumbra_client/ui/Rml_UIManager.h"
#include "luminumbra_common/components/CreatureComponents.h"
#include "luminumbra_common/components/PlantComponents.h"
#include "luminumbra_common/core/FilesystemPath.h"
#include "luminumbra_common/fields/EnergyFieldState.h"
#include "luminumbra_common/foliage/SpeciesRegistry.h"
#include "luminumbra_common/replay/ReplayStream.h"
#include "luminumbra_common/systems/PhysicsSystem.h"
#include "luminumbra_common/systems/WaterSystem.h"
#include "luminumbra_common/world/Chunk.h"
#include "luminumbra_common/world/GameSession.h"
#include "luminumbra_server/HeavyAuthorityReference.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include <imgui.h>
#include <imgui_internal.h>
#include <iterator>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace {
namespace W = Luminumbra::world;
namespace A = Luminumbra::Client::App;
namespace C = Luminumbra::Components;
namespace P = Luminumbra::Persistence;
namespace R = Luminumbra::Replay;
namespace fs = std::filesystem;
using RawAuthority = Luminumbra::Server::HeavyAuthorityReference;

thread_local std::array<int, static_cast<std::size_t>(GLFW_KEY_LAST) + 1> polled_keys{};
int ReadPolledKey(GLFWwindow*, int key_code) {
    if (key_code < 0 || key_code > GLFW_KEY_LAST)
        return GLFW_RELEASE;
    return polled_keys[static_cast<std::size_t>(key_code)];
}

std::string Path(const fs::path& path) {
    return Luminumbra::Filesystem::GenericPathString(path);
}
std::string Read(const fs::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input)
        throw std::runtime_error("Cannot read panel fixture");
    return {std::istreambuf_iterator<char>(input), {}};
}
void Write(const fs::path& path, std::string_view bytes) {
    fs::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    output.close();
    if (!output.good())
        throw std::runtime_error("Cannot write panel fixture");
}
std::map<std::string, std::string> Tree(const fs::path& directory) {
    std::map<std::string, std::string> files;
    for (const auto& entry : fs::recursive_directory_iterator(directory))
        if (entry.is_regular_file())
            files.emplace(Path(entry.path().lexically_relative(directory)), Read(entry.path()));
    return files;
}
thread_local std::size_t panel_region_reads = 0;
class ObservePanelReads {
public:
    ObservePanelReads() {
        panel_region_reads = 0;
        P::WorldSaveService::set_region_read_hook_for_testing(
            [](const fs::path&) { ++panel_region_reads; });
    }
    ~ObservePanelReads() {
        P::WorldSaveService::set_region_read_hook_for_testing(nullptr);
    }
};
class HiddenInputWindow {
public:
    GLFWwindow* window = nullptr;
    bool initialized = false;
    HiddenInputWindow() {
        initialized = glfwInit() == GLFW_TRUE;
        if (initialized) {
            glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
            glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
            window = glfwCreateWindow(1280, 900, "Region input", nullptr, nullptr);
        }
    }
    ~HiddenInputWindow() {
        if (window)
            glfwDestroyWindow(window);
        if (initialized)
            glfwTerminate();
    }
};

class AttachedPanelInput {
public:
    HiddenInputWindow input;
    luminumbra::core::SystemConfig configuration =
        luminumbra::core::SystemConfig::FromJsonString("{}");
    A::WindowState window_state;
    std::unique_ptr<Luminumbra::Client::PlayerController> player;
    std::unique_ptr<Luminumbra::Client::Rml_UIManager> ui;
    A::InputCallbackBindings bindings;
    GameStateManager game_state;

    explicit AttachedPanelInput(A::ClientAppContext& app) {
        if (!input.window)
            return;
        bindings.app = &app;
        bindings.systemConfig = &configuration;
        bindings.windowState = &window_state;
        bindings.playerController = &player;
        bindings.uiManager = &ui;
        glfwSetWindowUserPointer(input.window, &bindings);
        glfwSetKeyCallback(input.window, A::key_callback);
        A::SetGameState(input.window, game_state, GameState::IN_GAME);
        InstallCallbacks();
    }
    static void Button(GLFWwindow*, int, int, int) {}
    static void Cursor(GLFWwindow*, double, double) {}
    static void Scroll(GLFWwindow*, double, double) {}
    void InstallCallbacks() const {
        glfwSetMouseButtonCallback(input.window, Button);
        glfwSetCursorPosCallback(input.window, Cursor);
        glfwSetScrollCallback(input.window, Scroll);
    }
    void ExpectRestored() const {
        EXPECT_EQ(glfwSetKeyCallback(input.window, A::key_callback), A::key_callback);
        EXPECT_EQ(glfwSetMouseButtonCallback(input.window, Button), Button);
        EXPECT_EQ(glfwSetCursorPosCallback(input.window, Cursor), Cursor);
        EXPECT_EQ(glfwSetScrollCallback(input.window, Scroll), Scroll);
        EXPECT_EQ(glfwGetInputMode(input.window, GLFW_CURSOR), GLFW_CURSOR_DISABLED);
        EXPECT_FALSE(bindings.app->overlay.region_panel_input_attached);
        EXPECT_EQ(bindings.app->overlay.region_panel_previous_mouse_button, nullptr);
        EXPECT_EQ(bindings.app->overlay.region_panel_previous_cursor, nullptr);
        EXPECT_EQ(bindings.app->overlay.region_panel_previous_scroll, nullptr);
        EXPECT_TRUE(bindings.app->input.firstMouse);
    }
};

class ProductionRegionPanel : public testing::Test {
protected:
    fs::path root;
    Luminumbra::JobSystem jobs;
    ImGuiContext* previous_context = nullptr;
    ImGuiContext* context = nullptr;

    void SetUp() override {
        root = Luminumbra::TestSupport::CreateUniqueTempDirectory("region_panel_");
        Luminumbra::TestSupport::WriteSimulationContentFixture(root);
        Write(root / "worlds/atlas/presets/default.json", R"({
            "schema_rev":6,"generation_params":{
                "terrain":{"base_frequency":0.01,"base_amplitude":0.0,"octaves":1,
                    "persistence":0.5,"lacunarity":2.0,"height_offset":8.0,
                    "shaping":{"peaks_amplitude":0.0,"domain_warp_amplitude":0.0,
                        "continental_spline":[[-1.0,0.0],[1.0,0.0]]},
                    "hydro":{"enabled":false}},
                "features":{"caves_enabled":false,"cave_frequency":0.02}}})");
        jobs.startup(1);
        previous_context = ImGui::GetCurrentContext();
        context = ImGui::CreateContext();
        ASSERT_NE(context, nullptr);
        ImGui::SetCurrentContext(context);
        auto& io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.LogFilename = nullptr;
        io.DisplaySize = ImVec2(1280.0f, 900.0f);
        io.DeltaTime = 1.0f / 60.0f;
        unsigned char* pixels = nullptr;
        int width = 0, height = 0;
        io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
        ASSERT_NE(pixels, nullptr);
        ASSERT_GT(width, 0);
        ASSERT_GT(height, 0);
    }
    void TearDown() override {
        if (context)
            ImGui::DestroyContext(context);
        ImGui::SetCurrentContext(previous_context);
        std::error_code error;
        fs::remove_all(root, error);
    }
    void Configure(W::GameSession& session,
                   bool enabled = true,
                   W::RegionSchedulerConfig scheduler = {}) {
        session.SetRootPath(Path(root) + "/");
        session.SetJobSystem(&jobs);
        session.SetActiveRegionsEnabled(enabled);
        session.SetAetherStateEnabled(enabled);
        session.SetRegionSchedulerConfig(scheduler);
    }
    void Populate(W::GameSession& session) {
        session.SetReplicatedSimulationAnchors({});
        ASSERT_NE(session.SpawnGroundObject(1, {8, 100, 8}, 0, 7), 0u);
        auto& registry = session.GetRegistry();
        const auto creature = registry.create();
        registry.emplace<C::TransformComponent>(creature).position = {8, 100, 8};
        registry.emplace<C::CreatureComponent>(creature).species_id =
            C::CreatureSpeciesId16("fixture_creature");
        ASSERT_TRUE(session.AdmitCreature(creature));
        const auto plant = registry.create();
        registry.emplace<C::TransformComponent>(plant).position = {520, 100, 8};
        registry.emplace<C::PlantTag>(plant);
        registry.emplace<C::PlantGrowthComponent>(plant).species_id =
            luminumbra::foliage::SpeciesId16("fixture_plant");
        registry.emplace<C::PlantGenomeComponent>(plant);
        ASSERT_TRUE(session.AdmitPlant(plant));
        ASSERT_TRUE(session.SetFoliageCell({520, 100, 8}, true, 123));
        ASSERT_NE(session.GetEnergyFieldState(), nullptr);
        ASSERT_TRUE(session.GetEnergyFieldState()->QueueRegionalDeposit(9, 0, 0, 0, 10));
        session.GetSimulationEventBus().publish(1000, "future", "continuation", -2);
    }
    std::string Draw(W::GameSession& session, A::ClientAppContext& app) {
        ObservePanelReads observe;
        ImGui::NewFrame();
        ImGui::LogToBuffer();
        Luminumbra::Client::ScenarioHarness::RuntimeScenarioConfig scenario;
        scenario.scenario = "region-diagnostics"; // Suppress the unrelated farming HUD.
        A::DrawFrameStatusOverlays(app, GameState::IN_GAME, scenario, nullptr, &session);
        const std::string text = context->LogBuffer.c_str();
        ImGui::LogFinish();
        ImGui::Render();
        EXPECT_EQ(panel_region_reads, 0u);
        return text;
    }
    void ExpectSameHashes(W::GameSession& session,
                          const std::string& expected,
                          const P::WorldStreamingStateSubHashes& hashes) {
        std::string actual;
        P::WorldStreamingStateSubHashes current;
        ASSERT_TRUE(session.ComputeAuthorityWorldHashes(actual, current));
        EXPECT_EQ(actual, expected);
        // All named domains and checkpoint diagnostics, rather than a schedule digest.
        EXPECT_EQ(current.regions, hashes.regions);
        EXPECT_EQ(current.creatures, hashes.creatures);
        EXPECT_EQ(current.items, hashes.items);
        EXPECT_EQ(current.pages, hashes.pages);
        EXPECT_EQ(current.terrain, hashes.terrain);
        EXPECT_EQ(current.water, hashes.water);
        EXPECT_EQ(current.entities, hashes.entities);
        EXPECT_EQ(current.active_regions, hashes.active_regions);
        EXPECT_EQ(current.mesh, hashes.mesh);
        EXPECT_EQ(current.wind, hashes.wind);
        EXPECT_EQ(current.weather, hashes.weather);
        EXPECT_EQ(current.aether, hashes.aether);
        EXPECT_EQ(current.aether_state, hashes.aether_state);
        EXPECT_EQ(current.scents, hashes.scents);
        EXPECT_EQ(current.ecology, hashes.ecology);
        EXPECT_EQ(current.plants, hashes.plants);
    }
};

TEST_F(ProductionRegionPanel, ProductionPanelOpenClosedAuthorityIdentity) {
    W::GameSession session;
    Configure(session);
    ASSERT_TRUE(session.CreateWorld("Region panel", "1337", "default"));
    ASSERT_NO_FATAL_FAILURE(Populate(session));
    ASSERT_EQ(session.TickSimulation(1.0 / 30.0 + 1e-12), 1u);
    ASSERT_TRUE(session.SaveWorldState());
    const auto source = session.GetWorldSaveDir();
    const auto first_save = root / "first-save", second_save = root / "second-save";
    fs::copy(source, first_save, fs::copy_options::recursive);
    fs::copy(source, second_save, fs::copy_options::recursive);
    ASSERT_TRUE(session.SaveWorldStateTo(first_save));
    const auto files = Tree(root);
    const auto raw = RawAuthority::Capture(session);
    const auto canonical = session.CaptureAuthorityState();
    ASSERT_TRUE(canonical.state) << canonical.error;
    const auto allocator = session.GetActiveRegionLedger().next_persistent_id();
    std::string hash;
    P::WorldStreamingStateSubHashes hashes;
    ASSERT_TRUE(session.ComputeAuthorityWorldHashes(hash, hashes));
    const auto cached = session.GetCachedRegionHashes();

    HiddenInputWindow input;
    ASSERT_TRUE(input.initialized);
    ASSERT_NE(input.window, nullptr);
    A::ClientAppContext app;
    auto configuration = luminumbra::core::SystemConfig::FromJsonString("{}");
    A::WindowState window_state;
    std::unique_ptr<Luminumbra::Client::PlayerController> player;
    std::unique_ptr<Luminumbra::Client::Rml_UIManager> ui;
    A::InputCallbackBindings bindings;
    bindings.app = &app;
    bindings.systemConfig = &configuration;
    bindings.windowState = &window_state;
    bindings.playerController = &player;
    bindings.uiManager = &ui;
    glfwSetWindowUserPointer(input.window, &bindings);
    A::key_callback(input.window, GLFW_KEY_F4, 0, GLFW_PRESS, 0);
    EXPECT_FALSE(app.overlay.region_panel.open); // No modal input capture in menus.
    GameStateManager game_state;
    A::SetGameState(input.window, game_state, GameState::IN_GAME);
    const auto previous_button = +[](GLFWwindow*, int, int, int) {
    };
    const auto previous_cursor = +[](GLFWwindow*, double, double) {
    };
    const auto previous_scroll = +[](GLFWwindow*, double, double) {
    };
    glfwSetMouseButtonCallback(input.window, previous_button);
    glfwSetCursorPosCallback(input.window, previous_cursor);
    glfwSetScrollCallback(input.window, previous_scroll);
    EXPECT_TRUE(Draw(session, app).empty());
    for (unsigned frame = 0; frame < 8; ++frame) {
        A::key_callback(input.window, GLFW_KEY_F4, 0, GLFW_PRESS, 0);
        EXPECT_EQ(app.overlay.region_panel.open, frame % 2 == 0);
        const auto mouse_button = glfwSetMouseButtonCallback(input.window, nullptr);
        const auto cursor = glfwSetCursorPosCallback(input.window, nullptr);
        const auto scroll = glfwSetScrollCallback(input.window, nullptr);
        ASSERT_NE(mouse_button, nullptr);
        ASSERT_NE(cursor, nullptr);
        ASSERT_NE(scroll, nullptr);
        glfwSetMouseButtonCallback(input.window, mouse_button);
        glfwSetCursorPosCallback(input.window, cursor);
        glfwSetScrollCallback(input.window, scroll);
        if (app.overlay.region_panel.open) {
            EXPECT_NE(mouse_button, previous_button);
            EXPECT_EQ(cursor, A::mouse_callback);
            EXPECT_EQ(scroll, A::scroll_callback);
            const auto last_x = app.input.lastX, last_y = app.input.lastY;
            cursor(input.window, 100.0, 120.0);
            scroll(input.window, 0.0, 1.0);
            mouse_button(input.window, GLFW_MOUSE_BUTTON_LEFT, GLFW_PRESS, 0);
            mouse_button(input.window, GLFW_MOUSE_BUTTON_LEFT, GLFW_RELEASE, 0);
            A::key_callback(input.window, GLFW_KEY_LEFT_BRACKET, 0, GLFW_PRESS, 0);
            EXPECT_EQ(app.capture.timeScale, 1.0f);
            EXPECT_EQ(app.input.lastX, last_x);
            EXPECT_EQ(app.input.lastY, last_y);
        } else {
            EXPECT_EQ(mouse_button, previous_button);
            EXPECT_EQ(cursor, previous_cursor);
            EXPECT_EQ(scroll, previous_scroll);
        }
        Draw(session, app);
    }
    A::key_callback(input.window, GLFW_KEY_F4, 0, GLFW_PRESS, 0);
    ASSERT_TRUE(app.overlay.region_panel.open);
    A::SetGameState(input.window, game_state, GameState::MAIN_MENU);
    EXPECT_FALSE(app.overlay.region_panel.open);
    EXPECT_FALSE(app.overlay.region_panel_available);
    EXPECT_EQ(session.GetCachedRegionHashes(), cached);
    EXPECT_EQ(RawAuthority::Capture(session), raw);
    const auto after = session.CaptureAuthorityState();
    ASSERT_TRUE(after.state) << after.error;
    EXPECT_EQ(after.state->authority_bytes(), canonical.state->authority_bytes());
    EXPECT_EQ(session.GetActiveRegionLedger().next_persistent_id(), allocator);
    EXPECT_EQ(Tree(root), files);
    ASSERT_NO_FATAL_FAILURE(ExpectSameHashes(session, hash, hashes));
    ASSERT_TRUE(session.SaveWorldStateTo(second_save));
    EXPECT_EQ(Tree(first_save), Tree(second_save));

    W::GameSession disabled;
    Configure(disabled, false);
    ASSERT_TRUE(disabled.CreateWorld("Disabled panel", "1337", "default"));
    ASSERT_TRUE(disabled.SaveWorldState());
    const auto disabled_files = Tree(root);
    const auto disabled_hash = disabled.ComputeChunkStateHash();
    const auto disabled_ledger = disabled.GetActiveRegionLedger().canonical_bytes();
    app.overlay.region_panel.open = true;
    Draw(disabled, app);
    EXPECT_NE(Draw(disabled, app).find("Active regions disabled"), std::string::npos);
    app.overlay.region_panel.open = false;
    Draw(disabled, app);
    EXPECT_EQ(disabled.ComputeChunkStateHash(), disabled_hash);
    EXPECT_EQ(disabled.GetActiveRegionLedger().canonical_bytes(), disabled_ledger);
    EXPECT_EQ(Tree(root), disabled_files);
}

TEST_F(ProductionRegionPanel, ProductionPanelReplayAndFrameGroupingIdentity) {
    W::GameSession initial;
    Configure(initial);
    ASSERT_TRUE(initial.CreateWorld("Panel replay", "1337", "default"));
    ASSERT_NO_FATAL_FAILURE(Populate(initial));
    initial.PinActiveRegion({4, 0}, true);
    ASSERT_TRUE(initial.SaveWorldState());
    const auto world_id = initial.GetMetadata().worldId;
    const auto baseline_directory = initial.GetWorldSaveDir();
    std::vector<RawAuthority> references;
    std::vector<std::string> canonical_states;
    std::vector<P::WorldStreamingStateSubHashes> named_hashes;
    std::map<std::string, std::string> reference_save;
    std::string reference_replay;
    std::optional<R::ReplayContents> recording;
    for (const std::uint32_t grouping : {1u, 2u, 3u}) {
        for (const bool panel_open : {false, true}) {
            SCOPED_TRACE(std::to_string(grouping) + (panel_open ? " open" : " closed"));
            W::GameSession session;
            Configure(session);
            ASSERT_TRUE(session.LoadWorld(world_id)) << session.GetWorldOpenError();
            session.SetReplicatedSimulationAnchors({});
            A::ClientAppContext app;
            app.overlay.region_panel.open = panel_open;
            const auto run_name =
                "replay-" + std::to_string(grouping) + (panel_open ? "-open" : "-closed");
            const auto replay_path = root / run_name;
            R::ReplayWriter writer;
            ASSERT_TRUE(writer.Open(Path(replay_path), R::ReplayHeader{}));
            std::size_t checkpoint_index = 0;
            for (std::uint64_t completed = 0; completed < 24; completed += grouping) {
                for (std::uint32_t offset = 1; offset <= grouping; ++offset) {
                    const auto tick = completed + offset;
                    std::vector<std::uint8_t> inputs;
                    if (recording) {
                        const auto* input = R::FindInput(*recording, tick);
                        ASSERT_NE(input, nullptr);
                        inputs = input->inputs;
                    } else if ((tick - 1) % 6 == 0) {
                        inputs.push_back(((tick - 1) / 6) % 2 == 0 ? std::uint8_t{0}
                                                                   : std::uint8_t{1});
                    }
                    if (!inputs.empty()) {
                        ASSERT_EQ(offset, 1u);
                        ASSERT_EQ(inputs.size(), 1u);
                        session.PinActiveRegion({4, 0}, inputs.front() != 0);
                    }
                    writer.RecordInput(tick, inputs);
                }
                ASSERT_EQ(session.TickSimulation(static_cast<double>(grouping) / 30.0 + 1e-12),
                          grouping);
                Draw(session, app);
                if ((completed + grouping) % 6 != 0)
                    continue;
                R::CheckpointRecord checkpoint;
                checkpoint.tick = session.GetSimulationTickCount();
                P::WorldStreamingStateSubHashes sub;
                ASSERT_TRUE(session.ComputeAuthorityWorldHashes(checkpoint.world_hash, sub));
                checkpoint.terrain = sub.terrain;
                checkpoint.water = sub.water;
                checkpoint.entities = sub.entities;
                writer.RecordCheckpoint(checkpoint);
                const auto capture = session.CaptureAuthorityState();
                ASSERT_TRUE(capture.state) << capture.error;
                const auto raw = RawAuthority::Capture(session);
                if (!recording) {
                    references.push_back(raw);
                    canonical_states.push_back(capture.state->authority_bytes());
                    named_hashes.push_back(sub);
                } else {
                    ASSERT_LT(checkpoint_index, references.size());
                    EXPECT_EQ(raw, references[checkpoint_index]);
                    EXPECT_EQ(capture.state->authority_bytes(), canonical_states[checkpoint_index]);
                    ASSERT_LT(checkpoint_index, recording->checkpoints.size());
                    EXPECT_EQ(checkpoint.world_hash,
                              recording->checkpoints[checkpoint_index].world_hash);
                    EXPECT_EQ(sub.terrain, recording->checkpoints[checkpoint_index].terrain);
                    EXPECT_EQ(sub.water, recording->checkpoints[checkpoint_index].water);
                    EXPECT_EQ(sub.entities, recording->checkpoints[checkpoint_index].entities);
                    ASSERT_NO_FATAL_FAILURE(ExpectSameHashes(
                        session, checkpoint.world_hash, named_hashes[checkpoint_index]));
                }
                ++checkpoint_index;
            }
            ASSERT_EQ(checkpoint_index, 4u);
            ASSERT_TRUE(writer.Finalize(24));
            const auto decoded = R::ReadReplay(Path(replay_path));
            ASSERT_TRUE(decoded);
            ASSERT_FALSE(decoded->truncated);
            ASSERT_TRUE(decoded->trailer_present);
            ASSERT_EQ(decoded->inputs.size(), 24u);
            ASSERT_EQ(decoded->checkpoints.size(), 4u);
            // Sibling save directories start from the identical committed index.
            const auto save_directory = root / (run_name + "-save");
            fs::copy(baseline_directory, save_directory, fs::copy_options::recursive);
            ASSERT_TRUE(session.SaveWorldStateTo(save_directory));
            if (!recording) {
                reference_save = Tree(save_directory);
                reference_replay = Read(replay_path);
                recording = decoded;
            } else {
                EXPECT_EQ(Tree(save_directory), reference_save);
                EXPECT_EQ(Read(replay_path), reference_replay);
            }
        }
    }
}

TEST_F(ProductionRegionPanel, TransitionRingContainsLast64ActualTransitions) {
    W::GameSession session;
    Configure(session, true, {0, 1, 1});
    ASSERT_TRUE(session.CreateWorld("Transition history", "1337", "default"));
    session.SetReplicatedSimulationAnchors({});
    const W::RegionKey key{100, 100};
    session.PinActiveRegion(key, true);
    std::vector<W::RegionTransitionObservation> expected;
    A::ClientAppContext app;
    app.overlay.region_panel.open = true;
    for (unsigned step = 0; step < 110; ++step) {
        app.overlay.region_panel.open = step >= 80;
        if (step % 3 == 0)
            session.PinActiveRegion(key, true);
        session.RecordActiveRegionWork({key, 100, 0, 0, 0});
        ASSERT_EQ(session.TickSimulation(1.0 / 30.0 + 1e-12), 1u);
        ASSERT_FALSE(session.GetRegionSchedulerFault());
        const auto& schedule = session.GetRegionSchedule();
        for (const auto& transition : schedule.transitions) {
            ASSERT_NE(transition.from, transition.to);
            expected.push_back({schedule.tick, transition});
        }
        const auto snapshot = W::BuildRegionPanelSnapshot(session);
        std::uint64_t pressure = 0;
        for (const auto& [owner, record] : session.GetActiveRegionLedger().records()) {
            (void)owner;
            pressure += record.pressure; // This fixture submits only 100 units per tick.
        }
        EXPECT_EQ(snapshot.pressure, pressure);
        EXPECT_EQ(snapshot.work, schedule.work);
        EXPECT_EQ(snapshot.work_limit, 0u);
        EXPECT_EQ(snapshot.active + snapshot.reduced + snapshot.frozen, snapshot.total_rows);
        for (const auto& row : snapshot.rows)
            EXPECT_EQ(row.due,
                      std::find(schedule.due.begin(), schedule.due.end(), row.key) !=
                          schedule.due.end());
        const auto count = std::min(expected.size(), std::size_t{64});
        ASSERT_EQ(snapshot.transition_count, count);
        for (std::size_t index = 0; index < count; ++index)
            EXPECT_EQ(snapshot.transitions[index], expected[expected.size() - count + index]);
        Draw(session, app);
        Draw(session, app);
        EXPECT_EQ(W::BuildRegionPanelSnapshot(session).transitions, snapshot.transitions);
    }
    ASSERT_GT(expected.size(), 64u);
    const auto ring_before_load = W::BuildRegionPanelSnapshot(session);
    ASSERT_EQ(ring_before_load.transition_count, 64u);
    ASSERT_TRUE(session.SaveWorldState());
    std::string hash;
    P::WorldStreamingStateSubHashes hashes;
    ASSERT_TRUE(session.ComputeAuthorityWorldHashes(hash, hashes));
    const auto cache_before_refusal = session.GetCachedRegionHashes();
    const auto raw_before_refusal = RawAuthority::Capture(session);
    EXPECT_FALSE(session.LoadWorld("absent-region-panel-world"));
    EXPECT_EQ(session.GetCachedRegionHashes(), cache_before_refusal);
    EXPECT_EQ(W::BuildRegionPanelSnapshot(session).transitions, ring_before_load.transitions);
    EXPECT_EQ(RawAuthority::Capture(session), raw_before_refusal);
    ASSERT_TRUE(session.LoadWorldStateFrom(session.GetWorldSaveDir()));
    EXPECT_EQ(W::BuildRegionPanelSnapshot(session).transition_count, 0u);
    EXPECT_FALSE(session.GetCachedRegionHashes().valid);

    W::RegionTransitionRing batch;
    W::RegionSchedule schedule;
    schedule.tick = 99;
    for (std::int32_t x = -80; x < 80; ++x)
        schedule.transitions.push_back({{x, 0}, W::RegionState::Frozen, W::RegionState::Active});
    batch.Append(schedule);
    ASSERT_EQ(batch.size(), 64u);
    for (std::size_t index = 0; index < batch.size(); ++index)
        EXPECT_EQ(batch.at(index).transition, schedule.transitions[96 + index]);
    EXPECT_EQ(batch.at(64), W::RegionTransitionObservation{});
    schedule.transitions = {{{0, 0}, W::RegionState::Active, W::RegionState::Active}};
    const auto newest = batch.at(63);
    batch.Append(schedule);
    EXPECT_EQ(batch.size(), 64u);
    EXPECT_EQ(batch.at(63), newest);
}

TEST_F(ProductionRegionPanel, PanelPaginationIsBoundedAndObservational) {
    W::GameSession session;
    Configure(session);
    ASSERT_TRUE(session.CreateWorld("Panel pages", "1337", "default"));
    for (std::int32_t x = -150; x <= 150; ++x)
        session.PinActiveRegion({x, -x}, true);
    ASSERT_TRUE(session.SaveWorldState());
    const auto first_save = root / "page-first-save", second_save = root / "page-second-save";
    fs::copy(session.GetWorldSaveDir(), first_save, fs::copy_options::recursive);
    fs::copy(session.GetWorldSaveDir(), second_save, fs::copy_options::recursive);
    ASSERT_TRUE(session.SaveWorldStateTo(first_save));
    const auto files = Tree(root);
    const auto raw = RawAuthority::Capture(session);
    const auto allocator = session.GetActiveRegionLedger().next_persistent_id();
    std::string hash;
    P::WorldStreamingStateSubHashes hashes;
    ASSERT_TRUE(session.ComputeAuthorityWorldHashes(hash, hashes));
    A::ClientAppContext app;
    AttachedPanelInput attached(app);
    ASSERT_NE(attached.input.window, nullptr);
    A::key_callback(attached.input.window, GLFW_KEY_F4, 0, GLFW_PRESS, 0);
    ASSERT_TRUE(app.overlay.region_panel.open);
    for (const auto size : {std::size_t{0},
                            std::size_t{1},
                            std::size_t{16},
                            std::size_t{64},
                            std::size_t{256},
                            std::numeric_limits<std::size_t>::max()}) {
        for (const bool descending : {false, true}) {
            std::vector<W::RegionKey> visited;
            const auto clamped = std::clamp(size, std::size_t{1}, std::size_t{256});
            const auto pages = 1 + (std::size_t{301} - 1) / clamped;
            for (std::size_t page = 0; page < pages; ++page) {
                const auto snapshot =
                    W::BuildRegionPanelSnapshot(session, {page, size, descending});
                EXPECT_EQ(snapshot.total_rows, 301u);
                EXPECT_EQ(snapshot.page_count, pages);
                EXPECT_EQ(snapshot.rows_per_page, clamped);
                ASSERT_LE(snapshot.rows.size(), 256u);
                ASSERT_LE(snapshot.rows.size(), clamped);
                for (const auto& row : snapshot.rows)
                    visited.push_back(row.key);
            }
            ASSERT_EQ(visited.size(), 301u);
            for (std::size_t index = 0; index < visited.size(); ++index) {
                const auto x = static_cast<std::int32_t>(index) - 150;
                EXPECT_EQ(visited[index], (descending ? W::RegionKey{-x, x} : W::RegionKey{x, -x}));
            }
            app.overlay.region_panel.page = std::numeric_limits<std::size_t>::max();
            app.overlay.region_panel.rows_per_page = size;
            app.overlay.region_panel.descending = descending;
            Draw(session, app);
            EXPECT_EQ(app.overlay.region_panel.page, pages - 1);
            EXPECT_EQ(app.overlay.region_panel.rows_per_page, clamped);
        }
    }
    app.overlay.region_panel = {true, 0, 64, false};
    Draw(session, app);
    auto* window = ImGui::FindWindowByName(A::kRegionPanelWindowTitle);
    ASSERT_NE(window, nullptr);
    ImGui::ActivateItemByID(window->GetID("Next page"));
    Draw(session, app);
    EXPECT_EQ(app.overlay.region_panel.page, 1u);
    ImGui::ActivateItemByID(window->GetID("Previous page"));
    Draw(session, app);
    EXPECT_EQ(app.overlay.region_panel.page, 0u);
    ImGui::ActivateItemByID(window->GetID("Descending"));
    Draw(session, app);
    EXPECT_TRUE(app.overlay.region_panel.descending);
    ImGui::ActivateItemByID(window->GetID("Rows per page"));
    Draw(session, app);
    auto* combo = ImGui::FindWindowByName("##Combo_00");
    ASSERT_NE(combo, nullptr);
    ImGui::ActivateItemByID(ImHashStr("256", 0, combo->GetID(2)));
    Draw(session, app);
    EXPECT_EQ(app.overlay.region_panel.rows_per_page, 256u);
    EXPECT_EQ(app.overlay.region_panel.page, 0u);
    ImGui::ActivateItemByID(window->GetID("#CLOSE"));
    Draw(session, app);
    EXPECT_FALSE(app.overlay.region_panel.open);
    A::CloseRegionPanelInput(attached.input.window);
    attached.ExpectRestored();
    EXPECT_EQ(RawAuthority::Capture(session), raw);
    EXPECT_EQ(session.GetActiveRegionLedger().next_persistent_id(), allocator);
    ASSERT_NO_FATAL_FAILURE(ExpectSameHashes(session, hash, hashes));
    EXPECT_EQ(Tree(root), files);
    ASSERT_TRUE(session.SaveWorldStateTo(second_save));
    EXPECT_EQ(Tree(first_save), Tree(second_save));

    W::GameSession empty;
    Configure(empty);
    const auto snapshot = W::BuildRegionPanelSnapshot(
        empty,
        {std::numeric_limits<std::size_t>::max(), std::numeric_limits<std::size_t>::max(), true});
    EXPECT_TRUE(snapshot.rows.empty());
    EXPECT_EQ(snapshot.page, 0u);
    EXPECT_EQ(snapshot.page_count, 1u);
    EXPECT_EQ(snapshot.rows_per_page, 256u);
}

TEST_F(ProductionRegionPanel, ProductionPanelPolledGameplayInputsAreBlocked) {
    W::GameSession session;
    Configure(session);
    ASSERT_TRUE(session.CreateWorld("Panel input", "1337", "default"));
    A::ClientAppContext app;
    AttachedPanelInput attached(app);
    ASSERT_NE(attached.input.window, nullptr);
    Luminumbra::Rendering::Camera camera;
    attached.player = std::make_unique<Luminumbra::Client::PlayerController>(
        attached.input.window, &camera, nullptr);
    attached.player->ProcessKeyInput(
        attached.player->key(Luminumbra::Client::InputAction::ToggleNoclip), GLFW_PRESS);
    ASSERT_EQ(attached.player->GetMovementMode(), Luminumbra::Client::MovementMode::Noclip);
    A::key_callback(attached.input.window, GLFW_KEY_F4, 0, GLFW_PRESS, 0);
    ASSERT_TRUE(app.overlay.region_panel.open);
    A::key_callback(attached.input.window, GLFW_KEY_SPACE, 0, GLFW_PRESS, 0);
    const auto position = attached.player->GetPosition();
    const auto player_frame = attached.player->CaptureReplaySnapshot().frame;
    const auto allocator = session.GetActiveRegionLedger().next_persistent_id();
    A::UpdateGameplayPlayer(app.overlay.region_panel, *attached.player, 1.0f / 30.0f);
    polled_keys.fill(GLFW_RELEASE);
    polled_keys[GLFW_KEY_R] = GLFW_PRESS;
    EXPECT_FALSE(
        A::PollPlayerVerbPresses(app, attached.input.window, *attached.player, ReadPolledKey)
            .terraformDig);
    EXPECT_TRUE(app.interactions.terraformDig);
    EXPECT_EQ(attached.player->CaptureReplaySnapshot().frame, player_frame + 1);
    EXPECT_EQ(attached.player->GetPosition(), position);
    EXPECT_EQ(session.GetActiveRegionLedger().next_persistent_id(), allocator);
    const auto tick = session.GetWorldClock().tick();
    ASSERT_EQ(session.TickSimulation(1.0 / 30.0 + 1e-12), 1u);
    EXPECT_EQ(session.GetWorldClock().tick(), tick + 1);
    A::key_callback(attached.input.window, GLFW_KEY_F4, 0, GLFW_PRESS, 0);
    ASSERT_FALSE(app.overlay.region_panel.open);
    A::UpdateGameplayPlayer(app.overlay.region_panel, *attached.player, 1.0f / 30.0f);
    EXPECT_FALSE(
        A::PollPlayerVerbPresses(app, attached.input.window, *attached.player, ReadPolledKey)
            .terraformDig);
    polled_keys[GLFW_KEY_R] = GLFW_RELEASE;
    EXPECT_FALSE(
        A::PollPlayerVerbPresses(app, attached.input.window, *attached.player, ReadPolledKey)
            .terraformDig);
    EXPECT_FALSE(app.interactions.terraformDig);
    EXPECT_EQ(attached.player->CaptureReplaySnapshot().frame, player_frame + 2);
    EXPECT_EQ(session.GetActiveRegionLedger().next_persistent_id(), allocator);
}

TEST_F(ProductionRegionPanel, ProductionPanelWalkingPhysicsContinuesDuringCapture) {
    A::ClientAppContext app;
    AttachedPanelInput attached(app);
    ASSERT_NE(attached.input.window, nullptr);
    Luminumbra::Systems::PhysicsSystem physics, reference_physics;
    physics.startup();
    reference_physics.startup();
    ASSERT_TRUE(physics.is_started());
    ASSERT_TRUE(reference_physics.is_started());
    auto floor = std::make_shared<Luminumbra::Chunk>(Luminumbra::IVec3{0, 0, 0});
    constexpr auto side = static_cast<std::size_t>(Luminumbra::CHUNK_SIZE_X) + 1;
    floor->heightmap_data.assign(side * side, 8.0f);
    physics.add_chunk_collision(*floor);
    reference_physics.add_chunk_collision(*floor);
    Luminumbra::Rendering::Camera camera, reference_camera;
    const glm::vec3 feet{8.0f, 20.0f, 8.0f};
    attached.player = std::make_unique<Luminumbra::Client::PlayerController>(
        attached.input.window, &camera, &physics, feet);
    Luminumbra::Client::PlayerController reference(
        attached.input.window, &reference_camera, &reference_physics, feet);
    ASSERT_EQ(attached.player->GetMovementMode(), Luminumbra::Client::MovementMode::Walking);
    ASSERT_TRUE(attached.player->CaptureReplaySnapshot().hasInitializedPhysicsPlayer);
    ASSERT_FALSE(physics.is_player_grounded());
    using IA = Luminumbra::Client::InputAction;
    // Both gameplay presses precede F4 in the same event batch, before any update.
    for (const auto action : {IA::Jump, IA::Crouch})
        A::key_callback(attached.input.window, attached.player->key(action), 0, GLFW_PRESS, 0);
    ASSERT_TRUE(attached.player->CaptureReplaySnapshot().wantsToJump);
    ASSERT_TRUE(attached.player->CaptureReplaySnapshot().wantsToCrouch);
    A::key_callback(attached.input.window, GLFW_KEY_F4, 0, GLFW_PRESS, 0);
    ASSERT_TRUE(app.overlay.region_panel.open);
    EXPECT_FALSE(attached.player->CaptureReplaySnapshot().wantsToJump);
    EXPECT_FALSE(attached.player->CaptureReplaySnapshot().wantsToCrouch);
    for (const auto action : {IA::Jump, IA::Crouch})
        A::key_callback(attached.input.window, attached.player->key(action), 0, GLFW_RELEASE, 0);
    for (const auto action : {IA::Jump, IA::Crouch, IA::ToggleNoclip})
        A::key_callback(attached.input.window, attached.player->key(action), 0, GLFW_PRESS, 0);
    constexpr float dt = 1.0f / 30.0f;
    for (std::uint64_t frame = 1; frame <= 120; ++frame) {
        A::UpdateGameplayPlayer(app.overlay.region_panel, *attached.player, dt);
        reference.ApplyReplayInput(dt, Luminumbra::Client::PlayerReplayInputFrame{});
        const auto actual = attached.player->CaptureReplaySnapshot();
        const auto expected = reference.CaptureReplaySnapshot();
        EXPECT_EQ(actual.frame, frame);
        EXPECT_EQ(actual.mode, Luminumbra::Client::MovementMode::Walking);
        EXPECT_EQ(actual.position, expected.position);
        EXPECT_EQ(actual.velocity, expected.velocity);
        EXPECT_EQ(camera.Position, reference_camera.Position);
        EXPECT_EQ(physics.is_player_grounded(), reference_physics.is_player_grounded());
        EXPECT_FALSE(actual.isCrouching);
        EXPECT_FALSE(actual.wantsToJump);
        EXPECT_FALSE(actual.wantsToCrouch);
        if (frame == 1) {
            EXPECT_LT(actual.position.y, feet.y);
            EXPECT_LT(actual.velocity.y, 0.0f);
        }
    }
    EXPECT_TRUE(physics.is_player_grounded());
    EXPECT_GT(attached.player->GetPosition().y, 7.9f);
    EXPECT_LT(attached.player->GetPosition().y, 9.0f);
    EXPECT_FLOAT_EQ(attached.player->GetPosition().x, feet.x);
    EXPECT_FLOAT_EQ(attached.player->GetPosition().z, feet.z);
    for (const auto action : {IA::Jump, IA::Crouch, IA::ToggleNoclip})
        A::key_callback(attached.input.window, attached.player->key(action), 0, GLFW_RELEASE, 0);
    A::key_callback(attached.input.window, GLFW_KEY_F4, 0, GLFW_PRESS, 0);
    ASSERT_FALSE(app.overlay.region_panel.open);
    A::UpdateGameplayPlayer(app.overlay.region_panel, *attached.player, dt);
    reference.ApplyReplayInput(dt, Luminumbra::Client::PlayerReplayInputFrame{});
    EXPECT_EQ(attached.player->GetPosition(), reference.GetPosition());
    EXPECT_EQ(attached.player->CaptureReplaySnapshot().velocity,
              reference.CaptureReplaySnapshot().velocity);
    EXPECT_FALSE(attached.player->CaptureReplaySnapshot().isCrouching);
    EXPECT_FALSE(attached.player->CaptureReplaySnapshot().wantsToJump);
    EXPECT_FALSE(attached.player->CaptureReplaySnapshot().wantsToCrouch);
    EXPECT_EQ(camera.Position, reference_camera.Position);
    EXPECT_EQ(attached.player->CaptureReplaySnapshot().frame, 121u);

    // Also cover open/release/close within one batch: no captured update can drain it.
    for (const auto action : {IA::Jump, IA::Crouch})
        A::key_callback(attached.input.window, attached.player->key(action), 0, GLFW_PRESS, 0);
    ASSERT_TRUE(attached.player->CaptureReplaySnapshot().wantsToJump);
    ASSERT_TRUE(attached.player->CaptureReplaySnapshot().wantsToCrouch);
    A::key_callback(attached.input.window, GLFW_KEY_F4, 0, GLFW_PRESS, 0);
    ASSERT_TRUE(app.overlay.region_panel.open);
    for (const auto action : {IA::Jump, IA::Crouch})
        A::key_callback(attached.input.window, attached.player->key(action), 0, GLFW_RELEASE, 0);
    A::key_callback(attached.input.window, GLFW_KEY_F4, 0, GLFW_PRESS, 0);
    ASSERT_FALSE(app.overlay.region_panel.open);
    A::UpdateGameplayPlayer(app.overlay.region_panel, *attached.player, dt);
    reference.ApplyReplayInput(dt, Luminumbra::Client::PlayerReplayInputFrame{});
    const auto after_batch = attached.player->CaptureReplaySnapshot();
    const auto expected_after_batch = reference.CaptureReplaySnapshot();
    EXPECT_EQ(after_batch.frame, 122u);
    EXPECT_EQ(after_batch.position, expected_after_batch.position);
    EXPECT_EQ(after_batch.velocity, expected_after_batch.velocity);
    EXPECT_EQ(camera.Position, reference_camera.Position);
    EXPECT_EQ(physics.is_player_grounded(), reference_physics.is_player_grounded());
    EXPECT_FALSE(after_batch.isCrouching);
    EXPECT_FALSE(after_batch.wantsToJump);
    EXPECT_FALSE(after_batch.wantsToCrouch);
}

TEST_F(ProductionRegionPanel, ProductionPanelCapturedActionHeldAcrossCloseRequiresRelease) {
    A::ClientAppContext app;
    AttachedPanelInput attached(app);
    ASSERT_NE(attached.input.window, nullptr);
    Luminumbra::Rendering::Camera camera;
    attached.player = std::make_unique<Luminumbra::Client::PlayerController>(
        attached.input.window, &camera, nullptr);
    using IA = Luminumbra::Client::InputAction;
    struct VerbCase {
        int key_code;
        bool A::InteractionEdgeState::*previous;
        bool A::PlayerVerbPresses::*fired;
    };
    const std::array<VerbCase, 7> cases{{
        {GLFW_KEY_R, &A::InteractionEdgeState::terraformDig, &A::PlayerVerbPresses::terraformDig},
        {GLFW_KEY_T, &A::InteractionEdgeState::terraformFill, &A::PlayerVerbPresses::terraformFill},
        {GLFW_KEY_V,
         &A::InteractionEdgeState::farmCycleSpecies,
         &A::PlayerVerbPresses::farmCycleSpecies},
        {attached.player->key(IA::FarmPlant),
         &A::InteractionEdgeState::farmPlant,
         &A::PlayerVerbPresses::farmPlant},
        {attached.player->key(IA::FarmWater),
         &A::InteractionEdgeState::farmWater,
         &A::PlayerVerbPresses::farmWater},
        {attached.player->key(IA::FarmFertilize),
         &A::InteractionEdgeState::farmFertilize,
         &A::PlayerVerbPresses::farmFertilize},
        {attached.player->key(IA::FarmHarvest),
         &A::InteractionEdgeState::farmHarvest,
         &A::PlayerVerbPresses::farmHarvest},
    }};
    const auto poll = [&] {
        return A::PollPlayerVerbPresses(
            app, attached.input.window, *attached.player, ReadPolledKey);
    };
    for (const auto& verb : cases) {
        SCOPED_TRACE(verb.key_code);
        // Exercise both an intervening frame and a press/close in one event batch.
        for (const bool poll_while_open : {true, false}) {
            SCOPED_TRACE(poll_while_open);
            polled_keys.fill(GLFW_RELEASE);
            poll();
            ASSERT_FALSE(app.interactions.*(verb.previous));
            A::key_callback(attached.input.window, GLFW_KEY_F4, 0, GLFW_PRESS, 0);
            ASSERT_TRUE(app.overlay.region_panel.open);
            auto& physical = polled_keys[static_cast<std::size_t>(verb.key_code)];
            physical = GLFW_PRESS;
            A::key_callback(attached.input.window, verb.key_code, 0, GLFW_PRESS, 0);
            if (poll_while_open) {
                EXPECT_FALSE(poll().*(verb.fired));
                EXPECT_TRUE(app.interactions.*(verb.previous));
                physical = GLFW_RELEASE;
                A::key_callback(attached.input.window, verb.key_code, 0, GLFW_RELEASE, 0);
                EXPECT_FALSE(poll().*(verb.fired));
                EXPECT_FALSE(app.interactions.*(verb.previous));
                physical = GLFW_PRESS;
                A::key_callback(attached.input.window, verb.key_code, 0, GLFW_PRESS, 0);
                EXPECT_FALSE(poll().*(verb.fired));
                EXPECT_TRUE(app.interactions.*(verb.previous));
            }
            A::key_callback(attached.input.window, GLFW_KEY_F4, 0, GLFW_PRESS, 0);
            ASSERT_FALSE(app.overlay.region_panel.open);
            const auto mode_before_repeat = attached.player->GetMovementMode();
            A::key_callback(attached.input.window, verb.key_code, 0, GLFW_REPEAT, 0);
            EXPECT_FALSE(poll().*(verb.fired));
            EXPECT_TRUE(app.interactions.*(verb.previous));
            EXPECT_FALSE(poll().*(verb.fired));
            EXPECT_EQ(attached.player->GetMovementMode(), mode_before_repeat);
            physical = GLFW_RELEASE;
            A::key_callback(attached.input.window, verb.key_code, 0, GLFW_RELEASE, 0);
            EXPECT_FALSE(poll().*(verb.fired));
            EXPECT_FALSE(app.interactions.*(verb.previous));
            physical = GLFW_PRESS;
            A::key_callback(attached.input.window, verb.key_code, 0, GLFW_PRESS, 0);
            EXPECT_TRUE(poll().*(verb.fired));
            EXPECT_TRUE(app.interactions.*(verb.previous));
            EXPECT_FALSE(poll().*(verb.fired));
            physical = GLFW_RELEASE;
            A::key_callback(attached.input.window, verb.key_code, 0, GLFW_RELEASE, 0);
            EXPECT_FALSE(poll().*(verb.fired));
        }
    }
    // Callback-driven actions that accept repeats must also wait for release.
    A::key_callback(
        attached.input.window, attached.player->key(IA::TogglePhotoMode), 0, GLFW_PRESS, 0);
    ASSERT_TRUE(attached.player->photo_mode_active());
    const int lens_key = attached.player->key(IA::LensFocusUp);
    A::key_callback(attached.input.window, GLFW_KEY_F4, 0, GLFW_PRESS, 0);
    ASSERT_TRUE(app.overlay.region_panel.open);
    A::key_callback(attached.input.window, GLFW_KEY_UNKNOWN, 0, GLFW_PRESS, 0);
    A::key_callback(attached.input.window, lens_key, 0, GLFW_PRESS, 0);
    EXPECT_FLOAT_EQ(attached.player->consume_focus_nudge(), 0.0f);
    A::key_callback(attached.input.window, GLFW_KEY_F4, 0, GLFW_PRESS, 0);
    ASSERT_FALSE(app.overlay.region_panel.open);
    A::key_callback(attached.input.window, lens_key, 0, GLFW_REPEAT, 0);
    EXPECT_FLOAT_EQ(attached.player->consume_focus_nudge(), 0.0f);
    A::key_callback(attached.input.window, lens_key, 0, GLFW_RELEASE, 0);
    A::key_callback(attached.input.window, lens_key, 0, GLFW_PRESS, 0);
    EXPECT_FLOAT_EQ(attached.player->consume_focus_nudge(), 0.25f);
    A::key_callback(attached.input.window, lens_key, 0, GLFW_RELEASE, 0);
}

TEST_F(ProductionRegionPanel, ProductionPanelEveryCloseReconcilesHeldAndQueuedInput) {
    W::GameSession session;
    Configure(session);
    ASSERT_TRUE(session.CreateWorld("Panel close", "1337", "default"));
    A::ClientAppContext app;
    AttachedPanelInput attached(app);
    ASSERT_NE(attached.input.window, nullptr);
    auto& io = ImGui::GetIO();
    io.ConfigInputTrickleEventQueue = false;
    for (int close_path = 0; close_path < 3; ++close_path) {
        SCOPED_TRACE(close_path);
        A::SetGameState(attached.input.window, attached.game_state, GameState::IN_GAME);
        attached.InstallCallbacks();
        app.input.firstMouse = false;
        A::key_callback(attached.input.window, GLFW_KEY_F4, 0, GLFW_PRESS, 0);
        ASSERT_TRUE(app.overlay.region_panel.open);
        EXPECT_EQ(glfwGetInputMode(attached.input.window, GLFW_CURSOR), GLFW_CURSOR_NORMAL);
        A::key_callback(attached.input.window, GLFW_KEY_RIGHT, 0, GLFW_PRESS, GLFW_MOD_SHIFT);
        const auto button = glfwSetMouseButtonCallback(attached.input.window, nullptr);
        ASSERT_NE(button, nullptr);
        glfwSetMouseButtonCallback(attached.input.window, button);
        button(attached.input.window, GLFW_MOUSE_BUTTON_LEFT, GLFW_PRESS, 0);
        Draw(session, app);
        ASSERT_TRUE(ImGui::IsKeyDown(ImGuiKey_RightArrow));
        ASSERT_TRUE(io.KeyShift);
        ASSERT_TRUE(io.MouseDown[ImGuiMouseButton_Left]);
        A::key_callback(attached.input.window, GLFW_KEY_DOWN, 0, GLFW_PRESS, 0);
        if (close_path == 0) {
            A::key_callback(attached.input.window, GLFW_KEY_F4, 0, GLFW_PRESS, 0);
            attached.ExpectRestored();
        } else if (close_path == 1) {
            auto* panel_window = ImGui::FindWindowByName(A::kRegionPanelWindowTitle);
            ASSERT_NE(panel_window, nullptr);
            ImGui::ActivateItemByID(panel_window->GetID("#CLOSE"));
            Draw(session, app);
            ASSERT_FALSE(app.overlay.region_panel.open);
            A::CloseRegionPanelInput(attached.input.window);
            attached.ExpectRestored();
        } else {
            A::SetGameState(attached.input.window, attached.game_state, GameState::MAIN_MENU);
            EXPECT_FALSE(app.overlay.region_panel_available);
            EXPECT_FALSE(app.overlay.region_panel_input_attached);
            EXPECT_EQ(glfwGetInputMode(attached.input.window, GLFW_CURSOR), GLFW_CURSOR_NORMAL);
            EXPECT_EQ(glfwSetMouseButtonCallback(attached.input.window, nullptr), nullptr);
            EXPECT_EQ(glfwSetCursorPosCallback(attached.input.window, nullptr), nullptr);
            EXPECT_EQ(glfwSetScrollCallback(attached.input.window, nullptr), nullptr);
        }
        EXPECT_FALSE(app.overlay.region_panel.open);
        EXPECT_FALSE(ImGui::IsKeyDown(ImGuiKey_RightArrow));
        EXPECT_FALSE(ImGui::IsKeyDown(ImGuiKey_DownArrow));
        EXPECT_FALSE(io.KeyShift);
        EXPECT_FALSE(io.MouseDown[ImGuiMouseButton_Left]);
        // Releases now follow the restored callbacks, so cleanup must already be complete.
        A::key_callback(attached.input.window, GLFW_KEY_RIGHT, 0, GLFW_RELEASE, 0);
        A::key_callback(attached.input.window, GLFW_KEY_DOWN, 0, GLFW_RELEASE, 0);
        Draw(session, app);
        EXPECT_FALSE(ImGui::IsKeyDown(ImGuiKey_RightArrow));
        EXPECT_FALSE(ImGui::IsKeyDown(ImGuiKey_DownArrow));
        EXPECT_FALSE(io.KeyShift);
        EXPECT_FALSE(io.MouseDown[ImGuiMouseButton_Left]);
        A::CloseRegionPanelInput(attached.input.window);
        if (close_path != 2)
            attached.ExpectRestored(); // Repeated cleanup preserves the restored callbacks.
    }
}

TEST_F(ProductionRegionPanel, PanelWaterCountersIncludeBordersAndPreparation) {
    W::GameSession session;
    Configure(session);
    ASSERT_TRUE(session.CreateWorld("Panel water", "1337", "default"));
    session.PinActiveRegion({0, 0}, true);
    session.PinActiveRegion({1, 0}, true);
    ASSERT_FALSE(session.GetRegionSchedulerFault());
    auto* water = session.GetWaterSystem();
    ASSERT_NE(water, nullptr);
    water->SetHydrology(true, 0, 0);
    auto& ledger = const_cast<W::ActiveRegionLedger&>(session.GetActiveRegionLedger());
    const int resolution = water->sim_resolution();
    ASSERT_TRUE(resolution == 4 || resolution == 8 || resolution == 16 || resolution == 32);
    // Legal resolutions bound each fixture grid to 1024 cells, with two flux entries per cell.
    const auto side = static_cast<std::size_t>(resolution);
    const auto cells = side * side;
    const auto grid = [resolution, cells](const Luminumbra::IVec3& coordinates,
                                          std::int32_t depth) {
        auto chunk = std::make_shared<Luminumbra::Chunk>(coordinates);
        chunk->set_state(Luminumbra::ChunkState::Idle);
        chunk->has_water_sim.store(true);
        chunk->current_water_resolution.store(resolution);
        chunk->water_depth_mm.assign(cells, depth);
        chunk->water_bed_mm.assign(cells, 1000);
        chunk->water_edge_flux.assign(2 * cells, 0);
        chunk->water_level_data.assign(cells, static_cast<float>(depth) / 1000.0f + 1.0f);
        chunk->water_flow_data.assign(cells, Luminumbra::Vec2(0));
        chunk->water_sim_terrain_height.assign(cells, 1.0f);
        return chunk;
    };
    std::vector<std::shared_ptr<Luminumbra::Chunk>> chunks{grid({31, 0, 0}, 1000),
                                                           grid({32, 0, 0}, 0)};
    W::RegionSchedule schedule;
    std::vector<W::RegionWork> completed;
    const std::uint64_t window_cells = 2 * static_cast<std::uint64_t>(cells) + 4 * side;
    water->UpdateRegional(
        session.GetRegistry(), chunks, ledger, schedule, completed, 4 * window_cells);
    const auto& border_work = water->regional_work();
    ASSERT_EQ(border_work.border_windows, 2u);
    ASSERT_EQ(border_work.allocated_windows, 0u);
    ASSERT_EQ(border_work.used_windows, 0u);
    ASSERT_GT(border_work.total_cells, 0u);
    const auto border_snapshot = W::BuildRegionPanelSnapshot(session);
    EXPECT_EQ(border_snapshot.water_reserved, 2u);
    EXPECT_EQ(border_snapshot.water_used, 2u);
    EXPECT_EQ(border_snapshot.water_unused, border_work.unused_windows);
    EXPECT_EQ(border_snapshot.water_unallocated, border_work.unallocated_windows);
    EXPECT_EQ(border_snapshot.water_cells, border_work.total_cells);
    A::ClientAppContext app;
    app.overlay.region_panel.open = true;
    EXPECT_NE(Draw(session, app).find("Water reserved/used/unused/unallocated: 2 / 2"),
              std::string::npos);

    auto fresh = std::make_shared<Luminumbra::Chunk>(Luminumbra::IVec3{0, 0, 0});
    fresh->set_state(Luminumbra::ChunkState::Idle);
    chunks = {fresh};
    // Advance the public clock/scheduler while reserving water work for the explicit fixture.
    water->SetBootPaused(true);
    ASSERT_EQ(session.TickSimulation(1.0 / 30.0 + 1e-12), 1u);
    water->SetBootPaused(false);
    ASSERT_FALSE(session.GetRegionSchedulerFault());
    ASSERT_EQ(session.GetWorldClock().tick(), 1u);
    schedule = session.GetRegionSchedule();
    schedule.due = {{0, 0}};
    water->UpdateRegional(session.GetRegistry(), chunks, ledger, schedule, completed, window_cells);
    const auto& preparation_work = water->regional_work();
    ASSERT_GT(preparation_work.preparation_cells, 0u);
    ASSERT_GT(preparation_work.total_cells, preparation_work.water_cells);
    const auto preparation_snapshot = W::BuildRegionPanelSnapshot(session);
    EXPECT_EQ(preparation_snapshot.water_reserved,
              preparation_work.border_windows + preparation_work.allocated_windows);
    EXPECT_EQ(preparation_snapshot.water_used,
              preparation_work.border_windows + preparation_work.used_windows);
    EXPECT_EQ(preparation_snapshot.water_cells,
              preparation_work.preparation_cells + preparation_work.water_cells);
}

TEST_F(ProductionRegionPanel, PanelShowsUnknownWaterBoundAndCachedHashAge) {
    W::GameSession session;
    Configure(session);
    ASSERT_TRUE(session.CreateWorld("Cached diagnostics", "1337", "default"));
    ASSERT_NO_FATAL_FAILURE(Populate(session));
    A::ClientAppContext app;
    app.overlay.region_panel.open = true;
    Draw(session, app);
    EXPECT_NE(Draw(session, app).find("Cached hashes: unavailable"), std::string::npos);
    std::string hash;
    P::WorldStreamingStateSubHashes hashes;
    ASSERT_TRUE(session.ComputeAuthorityWorldHashes(hash, hashes));
    const auto cached = session.GetCachedRegionHashes();
    ASSERT_TRUE(cached.valid);
    ASSERT_EQ(session.TickSimulation(3.0 / 30.0 + 1e-12), 3u);
    const auto snapshot = W::BuildRegionPanelSnapshot(session);
    EXPECT_FALSE(snapshot.water_bound_ticks);
    ASSERT_TRUE(snapshot.hash_age_ticks);
    EXPECT_EQ(*snapshot.hash_age_ticks, 3u);
    EXPECT_EQ(snapshot.hashes, cached);
    const auto raw = RawAuthority::Capture(session);
    const auto text = Draw(session, app);
    EXPECT_NE(text.find("Water service bound: unknown"), std::string::npos);
    EXPECT_NE(text.find("age 3 ticks"), std::string::npos);
    for (const auto* value :
         {&hash, &hashes.regions, &hashes.creatures, &hashes.items, &hashes.pages}) {
        ASSERT_FALSE(value->empty());
        EXPECT_NE(text.find(*value), std::string::npos);
    }
    EXPECT_EQ(RawAuthority::Capture(session), raw);
    EXPECT_EQ(session.GetCachedRegionHashes(), cached);
    EXPECT_EQ(snapshot.hashes, cached); // Already-built snapshots own their data.

    // An unreadable committed owner cannot make drawing capture or latch a fault.
    const auto corrupt = P::WorldSaveService::region_file_path(session.GetWorldSaveDir(), 40, 40);
    Write(corrupt, "invalid authority");
    const auto before_files = Tree(root);
    Draw(session, app);
    EXPECT_FALSE(session.GetRegionSchedulerFault());
    EXPECT_EQ(Tree(root), before_files);
    P::WorldStreamingStateSubHashes refused;
    std::string refused_hash;
    EXPECT_FALSE(session.ComputeAuthorityWorldHashes(refused_hash, refused));
    EXPECT_TRUE(refused_hash.empty());
    EXPECT_FALSE(session.GetCachedRegionHashes().valid);
    ASSERT_TRUE(session.GetRegionSchedulerFault());
    const auto fault = *session.GetRegionSchedulerFault();
    const auto fault_ledger = session.GetActiveRegionLedger().canonical_bytes();
    const auto fault_allocator = session.GetActiveRegionLedger().next_persistent_id();
    const auto fault_text = Draw(session, app);
    EXPECT_NE(fault_text.find("Cached hashes: unavailable"), std::string::npos);
    EXPECT_NE(fault_text.find("Fault at tick"), std::string::npos);
    EXPECT_LE(W::BuildRegionPanelSnapshot(session).fault.size(),
              W::RegionPanelSnapshot::kMaxFaultText);
    ASSERT_TRUE(session.GetRegionSchedulerFault());
    EXPECT_EQ(session.GetRegionSchedulerFault()->tick, fault.tick);
    EXPECT_EQ(session.GetRegionSchedulerFault()->message, fault.message);
    EXPECT_EQ(session.GetActiveRegionLedger().canonical_bytes(), fault_ledger);
    EXPECT_EQ(session.GetActiveRegionLedger().next_persistent_id(), fault_allocator);
    EXPECT_EQ(Tree(root), before_files);

    W::GameSession disabled;
    Configure(disabled, false);
    const auto disabled_snapshot = W::BuildRegionPanelSnapshot(disabled);
    EXPECT_FALSE(disabled_snapshot.enabled);
    EXPECT_TRUE(disabled_snapshot.rows.empty());
    EXPECT_EQ(disabled_snapshot.transition_count, 0u);
    EXPECT_FALSE(disabled_snapshot.hashes.valid);
    ImGui::SetCurrentContext(nullptr);
    EXPECT_FALSE(A::DrawRegionPanel(snapshot, app.overlay.region_panel));
    ImGui::SetCurrentContext(context);
}
} // namespace
