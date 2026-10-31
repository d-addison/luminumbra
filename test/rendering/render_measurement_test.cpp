#include "luminumbra_client/app/RenderMeasurement.h"
#include "luminumbra_client/app/RenderWorkload.h"
#include "luminumbra_client/core/PresentationClock.h"
#include "luminumbra_client/rendering/FoliageCountObservation.h"

#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <map>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {
using namespace Luminumbra::Client::Measurement;

void require(bool condition) {
    if (!condition)
        throw std::runtime_error("measurement assertion failed");
}

template<class Function>
void refuses(Function function) {
    try {
        function();
    } catch (const std::exception&) {
        return;
    }
    throw std::runtime_error("invalid measurement input was accepted");
}

std::string fixture(const std::string& name) {
    std::ifstream input(std::string(LUMINUMBRA_SOURCE_ROOT) + "/tools/perf/fixtures/" + name,
                        std::ios::binary);
    require(input.good());
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

struct FakeQueryState {
    std::uint32_t nextId = 0;
    std::uint64_t clock = 0;
    bool supported = true;
    bool ready = false;
    int reads = 0;
    std::map<std::uint32_t, std::uint64_t> timestamps;
};
struct FakeQueries {
    std::shared_ptr<FakeQueryState> state;
    bool supported() const {
        return state->supported;
    }
    void create(std::uint32_t* ids, std::size_t count) const {
        for (std::size_t i = 0; i < count; ++i)
            ids[i] = ++state->nextId;
    }
    void destroy(const std::uint32_t*, std::size_t) const {}
    void stamp(std::uint32_t id) const {
        state->timestamps[id] = (state->clock += 1000000);
    }
    bool available(std::uint32_t) const {
        return state->ready;
    }
    std::uint64_t result(std::uint32_t id) const {
        ++state->reads;
        return state->timestamps.at(id);
    }
};

void gpu_queries() {
    auto state = std::make_shared<FakeQueryState>();
    SampleRing samples(10);
    GpuQueryRing<FakeQueries> queries(samples, FakeQueries{state});
    queries.initialize();
    for (std::uint64_t frame = 400; frame < 410; ++frame) {
        samples.append(frame);
        queries.begin_frame(frame);
        queries.begin_pass(1);
        queries.end_pass(1);
        queries.end_frame();
    }
    // Eight late frames fill the ring. The ninth must drain the oldest before reuse.
    require(state->reads == 8);
    require(samples.at(400).gpu == 3.0 && samples.at(401).passSum == 1.0);
    require(!samples.at(402).gpu);
    queries.drain();
    require(state->reads == 40);
    for (std::uint64_t frame = 400; frame < 410; ++frame) {
        require(samples.at(frame).frame == frame);
        require(samples.at(frame).gpu == 3.0 && samples.at(frame).passSum == 1.0);
        require(!samples.at(frame).issued[0] && !samples.at(frame).passes[0]);
    }
    queries.shutdown();
    require(state->reads == 40); // already drained; never read a frame twice

    SampleRing incomplete(1);
    GpuQueryRing<FakeQueries> missingEnd(incomplete, FakeQueries{state});
    missingEnd.initialize();
    incomplete.append(0);
    missingEnd.begin_frame(0);
    missingEnd.begin_pass(1);
    missingEnd.end_frame();
    missingEnd.drain();
    require(incomplete.at(0).gpu.has_value());
    require(!incomplete.at(0).passSum && !incomplete.at(0).passes[1]);
    missingEnd.shutdown();

    SampleRing repeated(1);
    GpuQueryRing<FakeQueries> duplicate(repeated, FakeQueries{state});
    duplicate.initialize();
    repeated.append(0);
    duplicate.begin_frame(0);
    duplicate.begin_pass(1);
    duplicate.end_pass(1);
    duplicate.begin_pass(1);
    duplicate.end_pass(1);
    duplicate.end_frame();
    duplicate.drain();
    require(repeated.at(0).gpu.has_value() && !repeated.at(0).passSum);
    duplicate.shutdown();

    SampleRing segmented(1);
    GpuQueryRing<FakeQueries> segments(segmented, FakeQueries{state});
    segments.initialize();
    segmented.append(0);
    segments.begin_frame(0);
    for (int segment = 0; segment < 600; ++segment) {
        segments.begin_pass(1);
        segments.end_pass(1);
        segments.checkpoint_passes();
    }
    segments.end_frame();
    segments.drain();
    require(segmented.at(0).issued[1] && segmented.at(0).passes[1] == 600.0);
    require(segmented.at(0).passSum == 600.0);
    segments.shutdown();

    SampleRing failedSegment(1);
    GpuQueryRing<FakeQueries> failedSegments(failedSegment, FakeQueries{state});
    failedSegments.initialize();
    failedSegment.append(0);
    failedSegments.begin_frame(0);
    failedSegments.begin_pass(1); // A partial segment remains invalid after a valid one.
    failedSegments.checkpoint_passes();
    failedSegments.begin_pass(1);
    failedSegments.end_pass(1);
    failedSegments.checkpoint_passes();
    failedSegments.end_frame();
    failedSegments.drain();
    require(failedSegment.at(0).issued[1] && !failedSegment.at(0).passes[1]);
    require(!failedSegment.at(0).passSum);
    failedSegments.shutdown();

    for (int malformed = 0; malformed < 3; ++malformed) {
        SampleRing refusedSegment(1);
        const auto firstQuery = state->nextId + 1;
        GpuQueryRing<FakeQueries> refusedSegments(refusedSegment, FakeQueries{state});
        refusedSegments.initialize();
        refusedSegment.append(0);
        refusedSegments.begin_frame(0);
        if (malformed != 0)
            refusedSegments.begin_pass(1);
        refusedSegments.end_pass(1);
        if (malformed == 1) {
            refusedSegments.begin_pass(1);
            refusedSegments.end_pass(1);
        } else if (malformed == 2) {
            state->timestamps.at(firstQuery + 5) = state->timestamps.at(firstQuery + 4) - 1;
        }
        refusedSegments.checkpoint_passes();
        refusedSegments.end_frame();
        refusedSegments.drain();
        require(refusedSegment.at(0).issued[1] && !refusedSegment.at(0).passes[1]);
        require(!refusedSegment.at(0).passSum);
        refusedSegments.shutdown();
    }

    state->supported = false;
    SampleRing unsupported(1);
    GpuQueryRing<FakeQueries> unavailable(unsupported, FakeQueries{state});
    unavailable.initialize();
    unsupported.append(0);
    unavailable.begin_frame(0);
    unavailable.begin_pass(1);
    unavailable.end_pass(1);
    unavailable.checkpoint_passes();
    unavailable.end_frame();
    unavailable.drain();
    require(!unsupported.at(0).gpu && !unsupported.at(0).passSum);
    unavailable.shutdown();
}

void statistics() {
    std::istringstream input(fixture("distribution.txt"));
    std::size_t count = 0;
    input >> count;
    std::vector<double> values(count);
    for (double& value : values)
        input >> value;
    const auto actual = summarize(values);
    for (const double value : actual) {
        double expected = 0.0;
        input >> expected;
        require(std::abs(value - expected) < 1e-12);
    }
    require(summarize({7}) == (std::array<double, 5>{7, 7, 7, 7, 0}));
    require(percentile({0, 10, 20}, 0.95) == 19.0);
    refuses([] { summarize({}); });
    refuses([] { summarize({-1}); });
}

void traversal_deterministic() {
    std::istringstream first(fixture("surface-flight.traversal"));
    std::istringstream second(fixture("surface-flight.traversal"));
    const auto a = Traversal::read(first);
    const auto b = Traversal::read(second);
    require(a.expectedTicks == 1800 && a.duration == 60 && a.tickRate == 30);
    for (int tick = 0; tick <= a.expectedTicks; ++tick)
        require(a.sample(tick) == b.sample(tick));
    require(a.sample(0).position == (std::array<double, 3>{8, 80, 8}));
    require(a.sample(900).position == (std::array<double, 3>{248, 80, 8}));
    require(a.sample(1800).position == (std::array<double, 3>{488, 80, 8}));
    for (const int revision : {6, 7, 8}) {
        auto source = fixture("surface-flight.traversal");
        source.replace(source.find("preset_revision 8"),
                       std::string("preset_revision 8").size(),
                       "preset_revision " + std::to_string(revision));
        std::istringstream historical(source);
        require(Traversal::read(historical).presetRevision == revision);
    }
    refuses([&] { a.sample(-1); });
    refuses([&] { a.sample(1801); });
}

void traversal_refusal() {
    const auto source = fixture("surface-flight.traversal");
    for (const auto& replacement : std::vector<std::pair<std::string, std::string>>{
             {"traversal.v1", "traversal.v2"},
             {"tick_rate 30", "tick_rate 60"},
             {"expected_ticks 1800", "expected_ticks 1799"},
             {"seed 424242", "seed -1"},
             {"preset_revision 8", "preset_revision 9"},
             {"speed_mps 8", "speed_mps 80"},
             {"cold_cache true", "cold_cache false"},
             {"point 248 80 8", "point 8 80 8"}}) {
        auto text = source;
        text.replace(text.find(replacement.first), replacement.first.size(), replacement.second);
        refuses([&] {
            std::istringstream input(text);
            Traversal::read(input);
        });
    }
    refuses([&] {
        std::istringstream input(source + "unknown 1");
        Traversal::read(input);
    });
    refuses([] {
        std::istringstream input;
        Traversal::read(input);
    });
}

void workload_definitions() {
    const std::array<const char*, 5> names = {
        "cave", "edit", "region-crossing", "save-load", "menu-entry"};
    for (const char* name : names) {
        const auto source = fixture(std::string(name) + ".traversal");
        std::istringstream first(source);
        std::istringstream second(source);
        const auto a = Traversal::read(first);
        const auto b = Traversal::read(second);
        require(a.kind != "traversal" && a.expectedTicks == 1800 && a.tickRate == 30);
        require(a.steps.size() == 1801 && a.actions.size() == b.actions.size());
        for (int tick = 0; tick <= a.expectedTicks; ++tick)
            require(a.sample(tick) == b.sample(tick));
        for (std::size_t index = 0; index < a.actions.size(); ++index) {
            require(a.actions[index].tick == b.actions[index].tick);
            require(a.actions[index].name == b.actions[index].name);
            require(a.actions[index].position == b.actions[index].position);
            require(a.actions[index].radius == b.actions[index].radius);
        }
        if (a.kind == "edit")
            for (int tick = a.actions.back().tick; tick <= a.expectedTicks; ++tick)
                require(a.sample(tick) == a.sample(a.actions.back().tick));
        refuses([&] { a.sample(a.expectedTicks + 1); });
        refuses([&] {
            std::istringstream input(source + "unknown 1");
            Traversal::read(input);
        });
    }
    const auto source = fixture("edit.traversal");
    const std::vector<std::pair<std::string, std::string>> replacements = {
        {"workload edit", "workload unknown"},
        {"traversal.v2", "traversal.v3"},
        {"point 0", "point 1"},
        {"point 1800", "point 1799"},
        {"actions 2", "actions 65"},
        {"action 120 dig", "action 0 dig"},
        {"action 300 fill", "action 120 fill"},
        {"action 300 fill", "action 300 reload"},
        {"8 12 8 8", "8 12 8 17"},
        {"8 12 8 8", "8 12 8 -1"}};
    for (const auto& [before, after] : replacements) {
        auto text = source;
        const auto offset = text.find(before);
        require(offset != std::string::npos);
        text.replace(offset, before.size(), after);
        refuses([&] {
            std::istringstream input(text);
            Traversal::read(input);
        });
    }
}

void workload_dispatch() {
    using Event = std::pair<int, std::string>;
    const std::vector<std::pair<std::string, std::vector<Event>>> cases = {
        {"edit", {{120, "dig"}, {300, "fill"}}},
        {"save-load", {{240, "save"}, {300, "reload"}}},
        {"menu-entry",
         {{120, "menu_open"},
          {240, "settings_open"},
          {360, "settings_close"},
          {480, "menu_close"},
          {600, "main_menu"},
          {900, "enter_world"}}},
        {"region-crossing", {}}};
    for (const auto& [name, expected] : cases) {
        std::istringstream input(fixture(name + ".traversal"));
        const auto script = Traversal::read(input);
        std::size_t cursor = 0;
        std::vector<Event> executed;
        std::vector<Event> recorded;
        for (int tick = 0; tick <= script.expectedTicks; ++tick) {
            const auto event = [&](const char* operation) {
                executed.emplace_back(tick, operation);
                return ActionResult{true, 0};
            };
            require(dispatch_actions(
                script.actions,
                cursor,
                tick,
                [&](const Traversal::Action& action, bool fill) {
                    require(action.position == (std::array<double, 3>{8, 12, 8}));
                    require(action.radius == 8);
                    event(fill ? "fill" : "dig");
                    return ActionResult{true, 3};
                },
                [&](bool menu) { return event(menu ? "main_menu" : "save"); },
                [&](bool reload) { return event(reload ? "reload" : "enter_world"); },
                [&](bool open) { return event(open ? "menu_open" : "menu_close"); },
                [&](bool open) { return event(open ? "settings_open" : "settings_close"); },
                [&](const Traversal::Action& action, const ActionResult& result) {
                    require(result.succeeded);
                    require(result.modified_chunks ==
                            (action.name == "dig" || action.name == "fill" ? 3 : 0));
                    recorded.emplace_back(action.tick, action.name);
                }));
        }
        require(cursor == script.actions.size());
        require(executed == expected && recorded == expected);
    }
    // Cave preparation has multiple actions at tick zero. Preserve file order,
    // never repeat them on subsequent warmup frames at that same tick.
    std::istringstream input(fixture("cave.traversal"));
    const auto cave = Traversal::read(input);
    std::size_t cursor = 0;
    std::vector<std::array<double, 3>> positions;
    const auto edit = [&](const Traversal::Action& action, bool fill) {
        require(!fill && action.tick == 0);
        positions.push_back(action.position);
        return ActionResult{true, 1};
    };
    const auto unexpected = [](bool) -> ActionResult {
        throw std::runtime_error("unexpected cave action");
    };
    const auto record = [](const Traversal::Action&, const ActionResult&) {
    };
    require(dispatch_actions(
        cave.actions, cursor, 0, edit, unexpected, unexpected, unexpected, unexpected, record));
    require(dispatch_actions(
        cave.actions, cursor, 0, edit, unexpected, unexpected, unexpected, unexpected, record));
    require(positions.size() == cave.actions.size());
    for (std::size_t index = 0; index < positions.size(); ++index)
        require(positions[index] == cave.actions[index].position);

    cursor = 0;
    int failures = 0;
    const auto fail = [](const Traversal::Action&, bool) {
        return ActionResult{false, 0};
    };
    require(!dispatch_actions(cave.actions,
                              cursor,
                              0,
                              fail,
                              unexpected,
                              unexpected,
                              unexpected,
                              unexpected,
                              [&](const Traversal::Action&, const ActionResult& result) {
                                  require(!result.succeeded);
                                  ++failures;
                              }));
    require(cursor == 1 && failures == 1);
}

void reload_boundaries() {
    // Cover all frame policy branches through this registered native entry.
    // Invalid nonpositive/NaN scales cannot enable the variable-dt tick path.
    const std::array<float, 6> scales{
        -1.0f, 0.0f, 0.5f, 1.0f, 16.0f, std::numeric_limits<float>::quiet_NaN()};
    for (const bool measuring : {false, true}) {
        for (const bool paused : {false, true}) {
            for (const float scale : scales) {
                require(frame_simulation_enabled(true, true, measuring, paused, scale) ==
                        (measuring && !paused));
                require(frame_simulation_enabled(false, true, measuring, paused, scale) ==
                        measuring);
                require(frame_simulation_enabled(false, false, measuring, paused, scale) ==
                        (scale > 0.0f));
            }
        }
    }
    require(!reload_residency_observed(0, 4));
    require(!reload_residency_observed(3, 4));
    require(reload_residency_observed(4, 4));
    require(reload_residency_observed(5, 4));
    require(!reload_residency_observed(600, 0));
    require(!reload_residency_observed(600, -1));
    require(reload_residency_observed(std::numeric_limits<std::uint64_t>::max(), 4));
    struct LoadedWorld {
        bool paused = false;
        void SetWaterBootPaused(bool on) {
            paused = on;
        }
    } world;
    {
        const ReloadWaterPause water_pause(world);
        require(world.paused);
    }
    require(!world.paused);
    refuses([&] {
        const ReloadWaterPause water_pause(world);
        require(world.paused);
        throw std::runtime_error("refused residency preparation");
    });
    require(!world.paused);
    ReloadSettlingWatchdog progressing;
    // A restream may exceed the script's 300 ticks while still doing real work.
    for (std::uint64_t pass = 1; pass <= 600; ++pass)
        require(progressing.waiting(false, {pass, 0, 0, 0}));
    require(!progressing.waiting(true, {600, 0, 0, 0}));
    require(!progressing.failed());
    for (std::size_t lane = 0; lane < 4; ++lane) {
        ReloadSettlingWatchdog stalled;
        std::array<std::uint64_t, 4> counters{};
        counters[lane] = 1;
        require(stalled.waiting(false, counters));
        for (std::size_t pass = 1; pass < ReloadSettlingWatchdog::kNoProgressPasses; ++pass) {
            // Repeated/decreasing counters are not progress; oscillation cannot
            // keep a stuck lane alive. Empty progress also expires at the bound.
            counters[lane] = pass % 2;
            require(stalled.waiting(false, counters));
        }
        require(!stalled.waiting(false, counters));
        require(stalled.failed());
        require(!stalled.waiting(false, {100, 100, 100, 100}));
        require(stalled.failed());
    }
    ReloadSettlingWatchdog empty;
    for (std::size_t pass = 1; pass < ReloadSettlingWatchdog::kNoProgressPasses; ++pass)
        require(empty.waiting(false, {}));
    require(!empty.waiting(false, {}));
    require(empty.failed());
    ReloadSettlingWatchdog immediate;
    require(!immediate.waiting(true, {}));
    require(!immediate.failed());
    using namespace std::chrono;
    ReloadInterval interval;
    const ReloadInterval::Clock::time_point origin{};
    interval.observe(9, true, origin);
    require(!interval.result());
    interval.request(10, origin);
    interval.observe(9, true, origin + milliseconds(1));
    interval.observe(10, true, origin - milliseconds(1));
    interval.observe(10, false, origin + milliseconds(2));
    require(!interval.result());
    interval.observe(11, true, origin + milliseconds(7));
    const auto first = interval.result();
    ASSERT_TRUE(first.has_value());
    if (first.has_value()) {
        const auto& checked = first.value();
        require(checked.request_frame == 10);
        require(checked.first_settled_frame == 11);
        require(checked.elapsed_ms == 7);
    }
    interval.observe(12, true, origin + milliseconds(99));
    const auto latched = interval.result();
    ASSERT_TRUE(latched.has_value());
    if (latched.has_value()) {
        const auto& checked = latched.value();
        require(checked.first_settled_frame == 11 && checked.elapsed_ms == 7);
    }
    interval.request(20, origin + milliseconds(100));
    require(!interval.result());
    interval.observe(20, true, origin + milliseconds(103));
    const auto second = interval.result();
    ASSERT_TRUE(second.has_value());
    if (second.has_value()) {
        const auto& checked = second.value();
        require(checked.request_frame == 20 && checked.first_settled_frame == 20);
        require(checked.elapsed_ms == 3);
    }

    // Save must not open the reload interval. Drive the committed script with a
    // deterministic CPU clock, including request-frame and later presents.
    ReloadInterval scripted;
    std::istringstream input(fixture("save-load.traversal"));
    const auto script = Traversal::read(input);
    std::size_t cursor = 0;
    int saves = 0;
    const auto unexpected = [](bool) -> ActionResult {
        throw std::runtime_error("unexpected save/load action");
    };
    for (int tick = 0; tick <= 304; ++tick) {
        const auto frame = static_cast<std::uint64_t>(tick) + 1;
        const auto frame_start = origin + milliseconds(tick * 10);
        require(dispatch_actions(
            script.actions,
            cursor,
            tick,
            [](const Traversal::Action&, bool) -> ActionResult {
                throw std::runtime_error("unexpected save/load edit");
            },
            [&](bool menu) {
                require(!menu && tick == 240);
                ++saves;
                return ActionResult{};
            },
            [&](bool reload) {
                require(reload && tick == 300 && saves == 1);
                scripted.request(frame, frame_start + milliseconds(3));
                return ActionResult{};
            },
            unexpected,
            unexpected,
            [](const Traversal::Action&, const ActionResult&) {}));
        scripted.observe(frame, tick < 300 || tick >= 303, frame_start + milliseconds(9));
        if (tick < 303)
            require(!scripted.result());
        else {
            const auto result = scripted.result();
            ASSERT_TRUE(result.has_value());
            if (result.has_value()) {
                const auto& checked = result.value();
                require(checked.request_frame == 301);
                require(checked.first_settled_frame == 304);
                require(checked.elapsed_ms == 36);
            }
        }
    }
    require(cursor == 2 && saves == 1);
}

TEST(RenderReloadInterval, Boundaries) {
    reload_boundaries();
}

void menu_observations() {
    struct State {
        bool in_game;
        bool paused;
        const char* visible;
    };
    const std::array<State, 7> transitions = {{{true, false, "hud"},
                                               {true, true, "pause"},
                                               {true, true, "settings"},
                                               {true, true, "pause"},
                                               {true, false, "hud"},
                                               {false, false, "main_menu"},
                                               {true, false, "hud"}}};
    for (const auto& state : transitions) {
        const auto observation =
            observe_menu(state.in_game, state.paused, [&](const char* document) {
                return std::string_view(document) == state.visible;
            });
        require(observation.state == (state.in_game ? "gameplay" : "main_menu"));
        require(observation.document == state.visible && observation.paused == state.paused);
    }
    // A queued settings request cannot manufacture a visible settings document.
    require(observe_menu(true, true, [](const char*) { return false; }).document == "none");
    require(observe_menu(false, false, [](const char*) { return true; }).document == "main_menu");
}

void foliage_observations() {
    using Luminumbra::Rendering::observe_foliage_counts;
    int reads = 0;
    const auto gpu_count = [&]() -> std::size_t {
        ++reads;
        return 17;
    };
    for (const auto& [enabled, built] :
         std::array<std::pair<bool, bool>, 3>{{{false, false}, {false, true}, {true, false}}}) {
        const auto result = observe_foliage_counts(enabled, built, true, 65536, 100, 80, gpu_count);
        require(result.instances == 0 && result.candidates == 0 && result.candidate_slots == 0);
        require(!result.gpu && reads == 0);
    }
    const auto cpu = observe_foliage_counts(true, true, false, 12, 100, 80, gpu_count);
    require(cpu.instances == 12 && cpu.candidates == 80 && cpu.candidate_slots == 100);
    require(!cpu.gpu && reads == 0);
    const auto gpu = observe_foliage_counts(true, true, true, 65536, 100, 80, gpu_count);
    require(gpu.instances == 17 && gpu.candidates == 80 && gpu.candidate_slots == 100);
    require(gpu.gpu && reads == 1);
    const auto empty = observe_foliage_counts(true, true, true, 0, 100, 0, gpu_count);
    require(empty.instances == 0 && empty.candidates == 0 && empty.candidate_slots == 100);
    require(empty.gpu && reads == 1);
    const auto rejected =
        observe_foliage_counts(true, true, true, 65536, 100, 80, []() -> std::size_t { return 0; });
    require(rejected.instances == 0 && rejected.candidates == 80 && rejected.gpu);
}

void frame_ring() {
    SampleRing ring(3);
    ring.append(400);
    ring.append(401);
    ring.append(402);
    // Deliberately resolve in reverse order to prove attribution is by issue id.
    ring.at(402).gpu = 12.0;
    ring.at(400).gpu = 10.0;
    require(!ring.at(401).gpu.has_value());
    require(ring.at(400).gpu == 10.0 && ring.at(402).gpu == 12.0);
    refuses([&] { ring.append(403); });
    refuses([&] { ring.at(399); });
    refuses([&] { ring.at(403); });
    refuses([] { const SampleRing invalid(0); });
    refuses([] { const SampleRing invalid(kMaxFrames + 1); });
}

void presentation_time() {
    using Luminumbra::Client::PresentationClock;
    PresentationClock clock;
    double wall = 7.0;
    int reads = 0;
    const auto wall_clock = [&] {
        ++reads;
        return wall;
    };
    require(clock.now(wall_clock) == 7.0 && reads == 1);
    // Simulate arbitrarily long startup and reload delays, with repeated
    // preparations/UI queries at a tick. No elapsed wall time may enter capture.
    for (const auto tick : {0u, 0u, 1u, 60u, 61u, 300u, 300u, 301u}) {
        clock.set_capture_tick(tick, 30);
        const double expected = static_cast<double>(tick) / 30.0;
        for (const double delay : {0.0, 0.25, 10.0, 600.0}) {
            wall += delay;
            require(clock.now(wall_clock) == expected);
            require(clock.capture_seconds() == expected);
        }
    }
    require(reads == 1);
    refuses([&] { clock.set_capture_tick(1, 0); });
    clock.use_wall_clock();
    require(clock.now(wall_clock) == wall && reads == 2);

    // Exercise the same per-frame selection used by the client before RmlUi
    // update and after ImGui's GLFW NewFrame. Counts are completed frames.
    using Luminumbra::Client::select_frame_presentation;
    PresentationClock selected_clock;
    std::uint64_t previous_tick = 0;
    const auto check_frame =
        [&](bool ready, std::uint32_t warmup, std::uint32_t measured, std::uint64_t expected_tick) {
            for (const double delay : {0.0, 0.25, 10.0, 600.0}) {
                wall += delay; // Startup, reload or a delayed report/screenshot frame.
                const auto frame =
                    select_frame_presentation(true, ready, warmup, measured, 60, 1800, 30);
                require(frame.tick == expected_tick);
                require(frame.imgui_delta == 1.0f / 30.0f);
                require(*frame.tick >= previous_tick);
                previous_tick = *frame.tick;
                selected_clock.set_capture_tick(*frame.tick, 30);
                require(selected_clock.now(wall_clock) ==
                        static_cast<double>(expected_tick) / 30.0);
            }
        };
    check_frame(false, 0, 0, 0); // Startup holds zero for any wall-clock delay.
    for (std::uint32_t warmup = 0; warmup < 60; ++warmup)
        check_frame(true, warmup, 0, warmup + 1);
    for (std::uint32_t measured = 0; measured < 1800; ++measured)
        check_frame(true, 60, measured, 61 + measured); // Includes paused menus and reloads.
    check_frame(true, 60, 1800, 1860);                  // Report frame.
    for (int frame = 0; frame < 4; ++frame)
        check_frame(true, 60, 1801, 1860); // Screenshot/AOV frames after the report latch.
    require(reads == 2); // Neither presentation consumer consulted the wall clock.

    // Outside v4 captures, leave both the presentation clock and GLFW delta alone.
    for (const bool ready : {false, true}) {
        const auto normal = select_frame_presentation(false, ready, 60, 1801, 60, 1800, 30);
        require(!normal.tick && !normal.imgui_delta);
    }
    refuses([] { select_frame_presentation(true, true, 0, 0, 60, 1800, 0); });
    constexpr auto maximum = std::numeric_limits<std::uint32_t>::max();
    const auto largest =
        select_frame_presentation(true, true, maximum, maximum, maximum, maximum, 30);
    require(largest.tick == 2ull * maximum);
}

void workload_world_authority() {
    // CLI startup has no capture-created id, even when metadata/seed match.
    refuses([] { require_workload_world_load(true, "existing-world", {}); });
    refuses([] { require_workload_world_load(true, {}, {}); });
    // UI and CLI share the load guard. An unrelated save stays forbidden after creation.
    refuses([] { require_workload_world_load(true, "existing-world", "capture-world"); });
    refuses([] { require_workload_world_load(true, {}, "capture-world"); });
    // Save/load and menu-entry reload only this capture's newly created world.
    require_workload_world_load(true, "capture-world", "capture-world");
    // Legacy captures and ordinary interactive loads retain their existing authority.
    require_workload_world_load(false, "existing-world", {});
    require_workload_world_load(false, "existing-world", "capture-world");
}
} // namespace

int main(int argc, char** argv) {
    try {
        require(argc == 2);
        const std::string name = argv[1];
        if (name == "queries")
            gpu_queries();
        else if (name == "statistics")
            statistics();
        else if (name == "traversal")
            traversal_deterministic();
        else if (name == "refusal")
            traversal_refusal();
        else if (name == "workloads")
            workload_definitions();
        else if (name == "dispatch")
            workload_dispatch();
        else if (name == "reload") {
            ::testing::InitGoogleTest(&argc, argv);
            return RUN_ALL_TESTS();
        } else if (name == "menus")
            menu_observations();
        else if (name == "foliage")
            foliage_observations();
        else if (name == "ring")
            frame_ring();
        else if (name == "presentation")
            presentation_time();
        else if (name == "world-authority")
            workload_world_authority();
        else
            throw std::runtime_error("unknown test");
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
