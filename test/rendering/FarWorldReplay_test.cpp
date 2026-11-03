#include "luminumbra_client/rendering/FarWorldDiagnostics.h"
#include "luminumbra_client/rendering/FarWorldReplay.h"
#include <gtest/gtest.h>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using namespace Luminumbra::Rendering;
struct FakeGl final : ReplayStateAccess {
    struct VertexBinding {
        unsigned buffer = 71;
        std::int64_t offset = 128;
        int stride = 32, divisor = 1;
        bool operator==(const VertexBinding&) const = default;
    };
    struct Vao {
        unsigned element_buffer = 73;
        std::array<VertexBinding, 8> bindings{};
        // enabled, format/type/integer/long, binding index and relative offset
        std::array<int, 64> attributes{1, 4, 7, 16, 32};
        bool operator==(const Vao&) const = default;
    };
    struct Attachment {
        unsigned texture = 81;
        int level = 2, layer = 3, layered = 0, face = 0, type = 1;
        bool operator==(const Attachment&) const = default;
    };
    struct Fbo {
        std::array<Attachment, 8> attachments{};
        std::array<int, 8> draw_buffers{1, 2, 3};
        int read_buffer = 2;
        bool operator==(const Fbo&) const = default;
    };
    struct Range {
        int buffer = 92, offset = 256, size = 512;
        bool operator==(const Range&) const = default;
    };
    struct Context {
        std::array<int, 5> framebuffer_program_vao_active{4, 7, 10, 13, 16};
        // array, element, indirect, dispatch, parameter, SSBO, UBO, copies, pack/unpack
        std::array<int, 11> buffers{19, 22, 25, 28, 31, 34, 37};
        std::array<Range, 8> ssbo{}, ubo{};
        std::array<int, 32> texture_sampler_images{41, 43, 47};
        std::array<int, 8> viewport_scissor{3, 7, 1024, 768};
        std::array<int, 16> enables{1, 0, 1};
        std::array<int, 64> blend_masks{1, 2, 4};
        std::array<int, 16> depth_raster_clear{2, 3, 5, 7};
        std::array<int, 8> pixel_unpack{4, 0, 0, 0};
        bool operator==(const Context&) const = default;
    };
    Context state{}, saved{};
    std::map<unsigned, Vao> vaos{{1, {}}, {2, {}}, {3, {}}, {4, {}}};
    std::map<unsigned, Fbo> fbos{{11, {}}, {12, {}}, {13, {}}, {14, {}}, {15, {}}, {16, {}}};
    std::map<unsigned, std::array<int, 16>> programs{{21, {1, 2, 3}}, {22, {4, 5, 6}}};
    ReplayObjectJournal<Vao> saved_vaos;
    ReplayObjectJournal<Fbo> saved_fbos;
    ReplayObjectJournal<std::array<int, 16>> saved_programs;
    int saves = 0, restores = 0;
    void Save() override {
        saved = state;
        ++saves;
    }
    void PreserveObject(ReplayObjectKind kind, std::uint32_t name) override {
        switch (kind) {
            case ReplayObjectKind::VertexArray:
                saved_vaos.Preserve(name, [this, name] { return vaos.at(name); });
                break;
            case ReplayObjectKind::Framebuffer:
            case ReplayObjectKind::LayerFramebuffer:
                saved_fbos.Preserve(name, [this, name] { return fbos.at(name); });
                break;
            case ReplayObjectKind::Program:
                saved_programs.Preserve(name, [this, name] { return programs.at(name); });
                break;
        }
    }
    void Restore() noexcept override {
        saved_vaos.Restore(
            [this](unsigned name, const Vao& value) noexcept { vaos.find(name)->second = value; });
        saved_fbos.Restore(
            [this](unsigned name, const Fbo& value) noexcept { fbos.find(name)->second = value; });
        saved_programs.Restore([this](unsigned name, const std::array<int, 16>& value) noexcept {
            programs.find(name)->second = value;
        });
        state = saved;
        ++restores;
    }
    void EarlyCascade(int cascade) {
        PreserveReplayObject(ReplayObjectKind::VertexArray, 1); // geometry pool
        vaos.at(1).bindings[1] = {901u, 64 * cascade, 16, 1};
        PreserveReplayObject(ReplayObjectKind::LayerFramebuffer, 11); // depth shadow
        fbos.at(11).attachments[0].layer = cascade;
        PreserveReplayObject(ReplayObjectKind::LayerFramebuffer, 12); // tint shadow
        fbos.at(12).attachments[0].layer = cascade;
        fbos.at(12).attachments[1].layer = cascade;
        PreserveReplayObject(ReplayObjectKind::Program, 21);
        programs.at(21).fill(999); // light-space matrix / material uniforms
        state.buffers.fill(999);
        state.ssbo.fill({999, 0, 64});
        state.ubo.fill({999, 0, 64});
        state.framebuffer_program_vao_active.fill(999);
        state.texture_sampler_images.fill(999);
        state.viewport_scissor.fill(999);
        state.enables.fill(999);
        state.blend_masks.fill(999);
        state.depth_raster_clear.fill(999);
        state.pixel_unpack.fill(999);
    }
    void LaterStages() {
        for (const unsigned name : {2u, 3u, 4u}) { // static, foliage, particles
            PreserveReplayObject(ReplayObjectKind::VertexArray, name);
            vaos.at(name).element_buffer = 999;
            vaos.at(name).bindings.fill({999, 64, 16, 0});
            vaos.at(name).attributes.fill(999);
        }
        for (const unsigned name : {13u, 14u, 15u, 16u}) { // G-buffer, lighting, OIT, TAAU
            PreserveReplayObject(ReplayObjectKind::Framebuffer, name);
            fbos.at(name).attachments.fill({999, 0, 0, 0, 0, 2});
            fbos.at(name).draw_buffers.fill(999);
            fbos.at(name).read_buffer = 999;
        }
        PreserveReplayObject(ReplayObjectKind::Program, 22);
        programs.at(22).fill(999);
    }
};
struct FakeFrame {
    std::vector<std::string> trace{"shadow_ON", "gbuffer_ON", "final_ON"};
    FarWorldSelection selection{
        FarWorldMode::Ladder, FarWorldMode::Ladder, FarWorldRefusal::None, true, true};
    std::size_t mdi_cursor = 5;
    unsigned jitter = 11, query_ring = 4, readback_ring = 2;
    float exposure = 1.25f;
    std::array<float, 16> previous_view_projection{8};
    float previous_time = 2.5f;
    std::array<float, 2> jitter_offset{.125f, -.25f};
    bool replay = false, suppress_timers = false;
    TaauHistoryState history{{41, 42}, 1, true}; // post-ON
    TaauHistoryState frozen{{41, 42}, 0, false}; // ON input, reset history
    std::array<unsigned char, 4> on_pixels{7, 12, 3, 255};
    int timers = 0, reads = 0, writes = 0;

    void Dispatch(FakeGl& gl, bool fail, bool fail_late) {
        EXPECT_EQ(previous_view_projection[0], 3.0f); // frozen ON temporal input
        EXPECT_EQ(previous_time, 1.5f);
        trace.clear();
        trace.push_back("shadow_OFF");
        ++mdi_cursor;
        gl.EarlyCascade(0);
        const FarReplayEffects effects{replay};
        if (effects.Timers() && !suppress_timers) {
            ++query_ring;
            ++timers;
        }
        if (effects.ExposureReadback()) {
            ++readback_ring;
            ++reads;
            exposure = 2;
        }
        const auto plan = PlanTaauResolve(replay ? frozen : history, 99, replay);
        EXPECT_EQ(plan.read_texture, 42u);
        EXPECT_EQ(plan.output_texture, 99u);
        EXPECT_FALSE(plan.history_valid); // same reset flag ON read
        if (plan.commit_history) {
            ++writes;
            history.write_index = 1 - history.write_index;
            history.valid = true;
            ++jitter;
            on_pixels.fill(0);
        }
        EXPECT_FALSE(selection.submit);
        EXPECT_FALSE(selection.advance);
        if (fail)
            throw std::runtime_error("stage failed");
        gl.EarlyCascade(1); // tests first-mutation snapshot deduplication
        gl.LaterStages();
        if (fail_late)
            throw std::runtime_error("late stage failed");
        trace.push_back("final_OFF");
    }
    void Replay(FakeGl& gl, bool fail = false, bool fail_begin = false, bool fail_late = false) {
        RunFarWorldReplay(
            gl,
            [this] {
                return [this, saved = *this]() mutable noexcept {
                    trace.swap(saved.trace);
                    selection = saved.selection;
                    mdi_cursor = saved.mdi_cursor;
                    replay = saved.replay;
                    suppress_timers = saved.suppress_timers;
                    exposure = saved.exposure;
                    query_ring = saved.query_ring;
                    readback_ring = saved.readback_ring;
                    jitter = saved.jitter;
                    history = saved.history;
                    previous_view_projection = saved.previous_view_projection;
                    previous_time = saved.previous_time;
                    jitter_offset = saved.jitter_offset;
                };
            },
            [this, &gl, fail_begin] {
                previous_view_projection[0] = 3.0f;
                previous_time = 1.5f;
                replay = true;
                suppress_timers = true;
                selection = FarWorldSubmissionOverride(selection, false);
                if (fail_begin) {
                    gl.EarlyCascade(0);
                    gl.LaterStages();
                    throw std::runtime_error("begin failed");
                }
            },
            [this, &gl, fail, fail_late] { Dispatch(gl, fail, fail_late); });
    }
};
void ExpectRestored(const FakeFrame& actual, const FakeFrame& on, const FakeGl& gl) {
    EXPECT_EQ(actual.trace, on.trace);
    EXPECT_EQ(actual.mdi_cursor, on.mdi_cursor);
    EXPECT_EQ(actual.jitter, on.jitter);
    EXPECT_EQ(actual.history, on.history);
    EXPECT_EQ(actual.exposure, on.exposure);
    EXPECT_EQ(actual.previous_view_projection, on.previous_view_projection);
    EXPECT_EQ(actual.previous_time, on.previous_time);
    EXPECT_EQ(actual.jitter_offset, on.jitter_offset);
    EXPECT_EQ(actual.query_ring, on.query_ring);
    EXPECT_EQ(actual.readback_ring, on.readback_ring);
    EXPECT_EQ(actual.selection.submit, on.selection.submit);
    EXPECT_EQ(actual.selection.advance, on.selection.advance);
    EXPECT_EQ(actual.selection.effective, on.selection.effective);
    EXPECT_EQ(actual.replay, on.replay);
    EXPECT_EQ(actual.suppress_timers, on.suppress_timers);
    EXPECT_EQ(gl.state, gl.saved);
    EXPECT_EQ(gl.saves, 1);
    EXPECT_EQ(gl.restores, 1);
}
TEST(FarWorldReplay, SuccessRestoresStateTraceCursorsAndEntryGl) {
    FakeFrame frame;
    const auto on = frame;
    FakeGl gl;
    frame.Replay(gl);
    ExpectRestored(frame, on, gl);
}
TEST(FarWorldReplay, StageExceptionRollsBackStateTraceCursorsAndGlGuard) {
    FakeFrame frame;
    const auto on = frame;
    FakeGl gl;
    EXPECT_THROW(frame.Replay(gl, true), std::runtime_error);
    ExpectRestored(frame, on, gl);
}
TEST(FarWorldReplay, BeginExceptionAlsoRollsBack) {
    FakeFrame frame;
    const auto on = frame;
    FakeGl gl;
    EXPECT_THROW(frame.Replay(gl, false, true), std::runtime_error);
    ExpectRestored(frame, on, gl);
}
TEST(FarWorldReplay, EveryAuditedObjectConfigurationRestoresOnAllExits) {
    for (int path = 0; path < 4; ++path) {
        SCOPED_TRACE(path); // success, early cascade, late stage, begin exception
        FakeFrame frame;
        const auto on = frame;
        FakeGl gl;
        const auto vaos = gl.vaos;
        const auto fbos = gl.fbos;
        const auto programs = gl.programs;
        if (path == 0)
            frame.Replay(gl);
        else
            EXPECT_THROW(frame.Replay(gl, path == 1, path == 3, path == 2), std::runtime_error);
        ExpectRestored(frame, on, gl);
        EXPECT_EQ(gl.vaos.at(1).bindings[1].buffer, vaos.at(1).bindings[1].buffer);
        EXPECT_EQ(gl.vaos.at(1).bindings[1].offset, vaos.at(1).bindings[1].offset);
        EXPECT_EQ(gl.vaos.at(1).bindings[1].stride, vaos.at(1).bindings[1].stride);
        EXPECT_EQ(gl.fbos.at(11).attachments[0].layer, fbos.at(11).attachments[0].layer);
        EXPECT_EQ(gl.vaos, vaos); // all buffers, formats, divisors and enables
        EXPECT_EQ(gl.fbos, fbos); // every attachment and draw/read selector
        EXPECT_EQ(gl.programs, programs);
        EXPECT_EQ(active_replay_state, nullptr);
    }
}
TEST(FarWorldReplay, ExposureTimersAndEvidenceAreSuppressed) {
    FakeFrame frame;
    FakeGl gl;
    frame.Replay(gl);
    EXPECT_EQ(frame.reads, 0);
    EXPECT_EQ(frame.timers, 0);
    EXPECT_FALSE(FarReplayEffects{true}.Evidence());
    EXPECT_TRUE(FarReplayEffects{false}.Evidence());
    EXPECT_TRUE(FarReplayEffects{false}.Timers());
    EXPECT_TRUE(FarReplayEffects{false}.ExposureReadback());
}
TEST(FarWorldReplay, TaauFrozenHistoryDoesNotWriteSwapOrAdvanceJitter) {
    FakeFrame frame;
    const auto on = frame;
    FakeGl gl;
    frame.Replay(gl);
    EXPECT_EQ(frame.writes, 0);
    EXPECT_EQ(frame.history, on.history);
    EXPECT_EQ(frame.jitter, on.jitter);
    EXPECT_EQ(frame.on_pixels, on.on_pixels);
    for (const bool valid : {false, true})
        for (const int index : {0, 1}) {
            const TaauHistoryState input{{71, 72}, index, valid};
            const auto normal = PlanTaauResolve(input, 99, false);
            const auto replay = PlanTaauResolve(input, 99, true);
            EXPECT_EQ(normal.read_texture, replay.read_texture);
            EXPECT_EQ(normal.history_valid, replay.history_valid);
            EXPECT_EQ(replay.output_texture, 99u);
            EXPECT_NE(replay.output_texture, input.textures[0]);
            EXPECT_NE(replay.output_texture, input.textures[1]);
            EXPECT_FALSE(replay.commit_history);
            EXPECT_TRUE(normal.commit_history);
        }
}
TEST(FarWorldReplay, EveryRefusalIncludesProjectionAndTargetDimensions) {
    using R = FarReplayRefusal;
    FarReplayCamera on;
    on.zoom = 60;
    on.near_plane = .1f;
    on.far_plane = 17408;
    on.width = on.internal_width = 1280;
    on.height = on.internal_height = 720;
    EXPECT_EQ(CheckFarReplay(true, true, false, on, on), R::None);
    EXPECT_EQ(CheckFarReplay(false, true, false, on, on), R::NotStarted);
    EXPECT_EQ(CheckFarReplay(true, false, false, on, on), R::Unprepared);
    EXPECT_EQ(CheckFarReplay(true, true, true, on, on), R::AlreadyReplaying);
    EXPECT_EQ(CheckFarReplay(true, true, false, on, on, false), R::TaauHistoryChanged);
    for (int fault = 0; fault < 12; ++fault) {
        auto now = on;
        R expected = R::DimensionsChanged;
        switch (fault) {
            case 0:
                now.view[3] = 1;
                expected = R::ViewChanged;
                break;
            case 1:
                now.projection[0] = 1;
                expected = R::ProjectionChanged;
                break;
            case 2:
                now.zoom = 45;
                expected = R::ProjectionChanged;
                break;
            case 3:
                now.near_plane = .2f;
                expected = R::ProjectionChanged;
                break;
            case 4:
                now.far_plane = 3200;
                expected = R::ProjectionChanged;
                break;
            case 5:
                ++now.width;
                break;
            case 6:
                ++now.height;
                break;
            case 7:
                ++now.internal_width;
                break;
            case 8:
                ++now.internal_height;
                break;
            case 9:
                now.target = 99;
                break;
            case 10:
                ++now.target_width;
                break;
            case 11:
                ++now.target_height;
                break;
        }
        EXPECT_EQ(CheckFarReplay(true, true, false, on, now), expected) << fault;
    }
    auto now = on;
    now.offscreen = true;
    EXPECT_EQ(CheckFarReplay(true, true, false, on, now), R::DimensionsChanged);
}
} // namespace
