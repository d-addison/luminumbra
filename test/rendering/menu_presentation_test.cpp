#include "luminumbra_client/app/FrameAttribution.h"
#include "luminumbra_client/app/MenuSceneSchedule.h"
#include "luminumbra_client/rendering/AttributedGl.h"
#include "luminumbra_client/ui/gl3/UiFilterCache.h"

#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <cstddef>
#include <limits>
#include <sstream>
#include <string>

namespace {
using std::chrono::milliseconds;
using std::chrono::nanoseconds;
using std::chrono::seconds;
using namespace Luminumbra::Client;
using namespace Luminumbra::Client::App;
using namespace Luminumbra::Client::Attribution;

constexpr MenuSceneKey kScenery{.width = 1920,
                                .height = 1080,
                                .document = 1,
                                .preview = 0,
                                .preview_world = 0,
                                .renderer_target = 0,
                                .camera = 0,
                                .settings = {},
                                .preview_active = false,
                                .wireframe = false};

TEST(MenuPresentation, ThirtyHertzUsesAbsoluteElapsedSlotsWithoutDrift) {
    MenuSceneSchedule schedule;
    ASSERT_TRUE(schedule.update(seconds{1}, kScenery));
    schedule.rendered();
    EXPECT_FALSE(schedule.update(seconds{1} + nanoseconds{33'333'333}, kScenery));
    EXPECT_TRUE(schedule.update(seconds{1} + nanoseconds{33'333'334}, kScenery));
    schedule.rendered();
    EXPECT_FALSE(schedule.update(seconds{1} + nanoseconds{66'666'666}, kScenery));
    EXPECT_TRUE(schedule.update(seconds{1} + nanoseconds{66'666'667}, kScenery));
    schedule.rendered();
    EXPECT_FALSE(schedule.update(seconds{1} + nanoseconds{99'999'999}, kScenery));
    EXPECT_TRUE(schedule.update(milliseconds{1100}, kScenery));
    EXPECT_DOUBLE_EQ(schedule.elapsed_seconds(), 0.1);
    schedule.rendered();
    // Exact second boundary remains aligned after thousands of skipped slots.
    EXPECT_TRUE(schedule.update(seconds{101}, kScenery));
    EXPECT_DOUBLE_EQ(schedule.elapsed_seconds(), 100);
    EXPECT_FLOAT_EQ(schedule.render_delta_seconds(), 99.9f);
    schedule.rendered();
    EXPECT_FALSE(schedule.update(seconds{101} + nanoseconds{33'333'333}, kScenery));
    EXPECT_TRUE(schedule.update(seconds{101} + nanoseconds{33'333'334}, kScenery));
}

TEST(MenuPresentation, LongFramesSkipObsoleteSamplesAndKeepTheNextDeadline) {
    MenuSceneSchedule schedule;
    ASSERT_TRUE(schedule.update(nanoseconds{0}, kScenery));
    schedule.rendered();
    EXPECT_TRUE(schedule.update(milliseconds{730}, kScenery));
    EXPECT_FLOAT_EQ(schedule.render_delta_seconds(), 0.730f);
    schedule.rendered();
    EXPECT_FALSE(schedule.update(milliseconds{731}, kScenery));
    EXPECT_TRUE(schedule.update(nanoseconds{733'333'334}, kScenery));
    schedule.rendered();
    EXPECT_FALSE(schedule.update(milliseconds{734}, kScenery));
}

TEST(MenuPresentation, ImageCannotCrossResizePreviewOrDocumentTransitions) {
    for (int change = 0; change < 7; ++change) {
        MenuSceneSchedule schedule;
        ASSERT_TRUE(schedule.update(nanoseconds{0}, kScenery));
        schedule.rendered();
        ASSERT_TRUE(schedule.valid());
        EXPECT_FALSE(schedule.update(milliseconds{1}, kScenery));
        auto changed = kScenery;
        switch (change) {
            case 0:
                ++changed.width;
                break;
            case 1:
                ++changed.height;
                break;
            case 2:
                ++changed.document;
                break;
            case 3:
                ++changed.preview;
                break;
            case 4:
                changed.preview_active = true;
                break;
            case 5:
                changed.wireframe = true;
                break;
            case 6:
                ++changed.preview_world;
                break;
            default:
                FAIL() << "Unknown menu image input";
        }
        EXPECT_TRUE(schedule.update(milliseconds{2}, changed));
        EXPECT_FALSE(schedule.valid());
        schedule.rendered();
        EXPECT_FALSE(schedule.update(milliseconds{3}, changed));
        // Returning to the old menu is a transition too.
        EXPECT_TRUE(schedule.update(milliseconds{4}, kScenery));
        EXPECT_FALSE(schedule.valid());
    }
}

TEST(MenuPresentation, InteractionRefreshesImmediatelyWithoutMovingTheClock) {
    MenuSceneSchedule schedule;
    ASSERT_TRUE(schedule.update(nanoseconds{0}, kScenery));
    schedule.rendered();
    EXPECT_TRUE(schedule.update(milliseconds{5}, kScenery, true));
    schedule.rendered();
    EXPECT_FALSE(schedule.update(milliseconds{10}, kScenery));
    EXPECT_TRUE(schedule.update(nanoseconds{33'333'334}, kScenery));
    schedule.rendered();
    schedule.invalidate();
    EXPECT_TRUE(schedule.update(milliseconds{34}, kScenery));
    EXPECT_FALSE(schedule.valid());
    auto minimized = kScenery;
    minimized.width = 0;
    EXPECT_FALSE(schedule.update(milliseconds{35}, minimized));
    EXPECT_FALSE(schedule.valid());
    EXPECT_TRUE(schedule.update(milliseconds{36}, kScenery));
}

TEST(MenuPresentation, ResizeCommitInvalidatesAnImageCapturedDuringDebounce) {
    MenuSceneSchedule schedule;
    auto key = kScenery;
    ASSERT_TRUE(schedule.update(nanoseconds{0}, key));
    schedule.rendered();
    // The framebuffer changes first; targets still have the old generation.
    key.width = 2560;
    key.height = 1440;
    ASSERT_TRUE(schedule.update(milliseconds{1}, key));
    schedule.rendered();
    ASSERT_FALSE(schedule.update(milliseconds{2}, key));
    // on_resize commits without another framebuffer-size change, in the same slot.
    ++key.renderer_target;
    EXPECT_TRUE(schedule.update(milliseconds{3}, key));
    EXPECT_FALSE(schedule.valid());
    schedule.rendered();
    EXPECT_FALSE(schedule.update(milliseconds{4}, key));
}

TEST(MenuPresentation, RenderScaleAndEveryPresetInputInvalidateWithinTheSameSlot) {
    for (int change = 0; change < 9; ++change) {
        MenuSceneSchedule schedule;
        ASSERT_TRUE(schedule.update(nanoseconds{0}, kScenery));
        schedule.rendered();
        auto key = kScenery;
        switch (change) {
            case 0:
                ++key.renderer_target; // Render-scale target recreation.
                break;
            case 1:
                key.settings.render_scale = 0.75f;
                break;
            case 2:
                key.settings.exposure_override = 2;
                break;
            case 3:
                ++key.settings.cloud_quality;
                break;
            case 4:
                ++key.settings.ssao_quality;
                break;
            case 5:
                ++key.settings.volumetric_quality;
                break;
            case 6:
                key.settings.taau = true;
                break;
            case 7:
                key.settings.sky_lut_gpu = true;
                break;
            case 8:
                key.settings.metered_exposure = true;
                break;
            default:
                FAIL() << "Unknown graphics setting";
        }
        EXPECT_TRUE(schedule.update(milliseconds{1}, key));
        EXPECT_FALSE(schedule.valid());
        schedule.rendered();
        EXPECT_FALSE(schedule.update(milliseconds{2}, key));
    }
}

TEST(MenuPresentation, CameraEditsInvalidateWhileTheScenicPanKeepsItsCadence) {
    for (std::size_t component = 0; component < MenuCameraRevision::State{}.size(); ++component) {
        MenuCameraRevision camera;
        MenuCameraRevision::State pose{};
        MenuSceneSchedule schedule;
        auto key = kScenery;
        key.camera = camera.observe(pose);
        ASSERT_TRUE(schedule.update(nanoseconds{0}, key));
        schedule.rendered();
        // Save the expected, animated pose, as the scenic pan does every frame.
        pose[12] = 96;
        camera.animated(pose);
        key.camera = camera.observe(pose);
        EXPECT_FALSE(schedule.update(milliseconds{1}, key));
        // Position, orientation and FOV edits all invalidate before the next slot.
        pose[component] += 1;
        key.camera = camera.observe(pose);
        EXPECT_TRUE(schedule.update(milliseconds{2}, key));
        EXPECT_FALSE(schedule.valid());
        schedule.rendered();
        EXPECT_FALSE(schedule.update(milliseconds{3}, key));
    }
}

TEST(MenuPresentation, RuntimePhaseNestingRestoresParentsAndRecordsEveryScopeOnce) {
    const PhaseFrameScope frame(true);
    {
        const PhaseScope scene(Phase::MenuScene);
        ASSERT_TRUE(scene.is_current());
        {
            const PhaseScope ui(Phase::UiRender);
            EXPECT_TRUE(ui.is_current());
            EXPECT_FALSE(scene.is_current());
            {
                const PhaseScope filters(Phase::UiFilters);
                EXPECT_TRUE(filters.is_current());
                EXPECT_FALSE(ui.is_current());
            }
            EXPECT_TRUE(ui.is_current());
            const PhaseScope texture(Phase::TextureLoad);
            EXPECT_TRUE(texture.is_current());
        }
        EXPECT_TRUE(scene.is_current());
        const PhaseScope adoption(Phase::PreviewAdoption);
        EXPECT_TRUE(adoption.is_current());
    }
    for (std::size_t index = 0; index < kPhaseCount; ++index) {
        const auto phase = static_cast<Phase>(index);
        const bool visited = phase == Phase::MenuScene || phase == Phase::UiRender ||
                             phase == Phase::UiFilters || phase == Phase::TextureLoad ||
                             phase == Phase::PreviewAdoption;
        EXPECT_EQ(frame.snapshot().completed_scopes(phase), visited ? 1u : 0u);
    }
    EXPECT_DOUBLE_EQ(phase_total_ms(), frame.snapshot().sum());
    // Use the scopes' own observations to check conservation, with no elapsed
    // time threshold or clock-resolution assumption. Removing runtime child
    // subtraction would count descendants twice and break these identities.
    const auto& totals = frame.snapshot();
    EXPECT_DOUBLE_EQ(totals.sum(), totals.inclusive_milliseconds(Phase::MenuScene));
    EXPECT_DOUBLE_EQ(totals.milliseconds()[static_cast<std::size_t>(Phase::UiRender)],
                     totals.inclusive_milliseconds(Phase::UiRender) -
                         (totals.inclusive_milliseconds(Phase::UiFilters) +
                          totals.inclusive_milliseconds(Phase::TextureLoad)));
}

TEST(MenuPresentation, RuntimeCollectorsRestoreTheirParentAndSuppressDisabledCollection) {
    EXPECT_DOUBLE_EQ(phase_total_ms(), 0);
    const PhaseFrameScope frame(true);
    const PhaseScope scene(Phase::MenuScene);
    {
        const PhaseScope disabled(Phase::UiUpdate, false);
        EXPECT_FALSE(disabled.is_current());
        EXPECT_TRUE(scene.is_current());
    }
    EXPECT_EQ(frame.snapshot().completed_scopes(Phase::UiUpdate), 0u);
    {
        const PhaseFrameScope disabled(false);
        EXPECT_FALSE(disabled.is_current());
        EXPECT_FALSE(frame.is_current());
        const PhaseScope ignored(Phase::UiUpdate);
        EXPECT_FALSE(ignored.is_current());
        EXPECT_DOUBLE_EQ(phase_total_ms(), 0);
    }
    EXPECT_TRUE(frame.is_current());
    EXPECT_TRUE(scene.is_current());
    {
        const PhaseFrameScope nested(true);
        EXPECT_TRUE(nested.is_current());
        EXPECT_FALSE(frame.is_current());
        EXPECT_FALSE(scene.is_current());
        {
            const PhaseScope ui(Phase::UiUpdate);
            EXPECT_TRUE(ui.is_current());
        }
        EXPECT_EQ(nested.snapshot().completed_scopes(Phase::UiUpdate), 1u);
        EXPECT_EQ(frame.snapshot().completed_scopes(Phase::UiUpdate), 0u);
    }
    EXPECT_TRUE(frame.is_current());
    EXPECT_TRUE(scene.is_current());
    {
        const PhaseScope ui(Phase::UiUpdate);
        EXPECT_TRUE(ui.is_current());
    }
    EXPECT_TRUE(scene.is_current());
    EXPECT_EQ(frame.snapshot().completed_scopes(Phase::UiUpdate), 1u);
}

TEST(MenuPresentation, DisabledRuntimeCollectorLeavesNoPhasesForTheNextFrame) {
    {
        const PhaseFrameScope disabled;
        {
            const PhaseScope scene(Phase::MenuScene);
            const PhaseScope shader(Phase::ShaderFirstUse);
            EXPECT_FALSE(scene.is_current());
            EXPECT_FALSE(shader.is_current());
        }
        EXPECT_EQ(disabled.snapshot().completed_scopes(Phase::MenuScene), 0u);
        EXPECT_EQ(disabled.snapshot().completed_scopes(Phase::ShaderFirstUse), 0u);
        EXPECT_DOUBLE_EQ(disabled.snapshot().sum(), 0);
    }
    const PhaseFrameScope enabled(true);
    EXPECT_TRUE(enabled.is_current());
    EXPECT_DOUBLE_EQ(enabled.snapshot().sum(), 0);
    EXPECT_EQ(enabled.snapshot().completed_scopes(Phase::MenuScene), 0u);
}

TEST(MenuPresentation, ProgramFirstUseIncludesBindingAndTheFirstSubmissionOnly) {
    namespace Gl = Luminumbra::Rendering::AttributedGl;
    const PhaseFrameScope frame(true);
    const PhaseScope parent(Phase::MenuScene);
    const auto program = Gl::CreateProgram([] { return std::uint32_t{71}; });
    int binds = 0;
    const auto bind = [&](std::uint32_t name) {
        EXPECT_EQ(name, program);
        EXPECT_FALSE(parent.is_current());
        ++binds;
    };
    Gl::UseProgram(bind, program);
    Gl::UseProgram(bind, program); // Uniform setup can bind without submitting.
    EXPECT_EQ(binds, 2);
    EXPECT_TRUE(parent.is_current());
    EXPECT_EQ(frame.snapshot().completed_scopes(Phase::ShaderFirstUse), 2u);
    int submissions = 0;
    Gl::Submit(
        [&](int groups) {
            EXPECT_EQ(groups, 3);
            EXPECT_FALSE(parent.is_current()); // Scope is still open during dispatch/draw.
            EXPECT_EQ(frame.snapshot().completed_scopes(Phase::ShaderFirstUse), 2u);
            ++submissions;
        },
        3);
    EXPECT_TRUE(parent.is_current());
    EXPECT_EQ(frame.snapshot().completed_scopes(Phase::ShaderFirstUse), 3u);
    Gl::UseProgram([&](std::uint32_t) { EXPECT_TRUE(parent.is_current()); }, program);
    Gl::Submit([&] {
        EXPECT_TRUE(parent.is_current());
        ++submissions;
    });
    EXPECT_EQ(submissions, 2);
    EXPECT_EQ(frame.snapshot().completed_scopes(Phase::ShaderFirstUse), 3u);
    Gl::DeleteProgram([](std::uint32_t) {}, program);
}

TEST(MenuPresentation, ProgramDeletionAndHandleReuseStartANewFirstUse) {
    ProgramFirstUse programs;
    programs.created(7);
    programs.bind(7);
    EXPECT_TRUE(programs.pending());
    programs.bind(0);
    EXPECT_FALSE(programs.pending());
    programs.bind(7); // A state restore must retain a pending first submission.
    EXPECT_TRUE(programs.pending());
    programs.created(8);
    programs.bind(8);
    EXPECT_TRUE(programs.pending());
    programs.submit([] {});
    programs.bind(7);
    EXPECT_TRUE(programs.pending()); // Another program's draw does not consume this one.
    programs.deleted(8);
    programs.deleted(7);
    EXPECT_FALSE(programs.pending());
    programs.bind(7);
    EXPECT_FALSE(programs.pending());
    programs.created(7); // A relink/recreation can reuse the numeric GL name.
    programs.bind(7);
    EXPECT_TRUE(programs.pending());
    programs.submit([] {});
    EXPECT_FALSE(programs.pending());
    programs.bind(7);
    EXPECT_FALSE(programs.pending());
    programs.deleted(7);
    programs.created(0);
    programs.bind(0);
    EXPECT_FALSE(programs.pending());
}

TEST(MenuPresentation, SeenProgramsBypassFirstUseAttributionOnBindsAndSubmissions) {
    namespace Gl = Luminumbra::Rendering::AttributedGl;
    const PhaseFrameScope frame(true);
    const auto program = Gl::CreateProgram([] { return std::uint32_t{72}; });
    const auto before = Gl::programs.attributed_calls();
    const auto lookups = Gl::programs.registry_lookups();
    Gl::UseProgram([](std::uint32_t) {}, program);
    Gl::Submit([] {});
    ASSERT_EQ(Gl::programs.attributed_calls(), before + 2);
    int calls = 0;
    for (int index = 0; index < 4; ++index) {
        Gl::UseProgram(
            [&](std::uint32_t name) {
                EXPECT_EQ(name, program);
                ++calls;
            },
            program);
        Gl::Submit([&](int value) { calls += value; }, 2);
    }
    EXPECT_EQ(calls, 12);
    EXPECT_EQ(Gl::programs.attributed_calls(), before + 2);
    EXPECT_EQ(Gl::programs.registry_lookups(), lookups);
    EXPECT_EQ(frame.snapshot().completed_scopes(Phase::ShaderFirstUse), 2u);
    Gl::DeleteProgram([](std::uint32_t) {}, program);
}

TEST(MenuPresentation, DisabledCollectorsBypassAttributionAndConsumeFirstSubmission) {
    namespace Gl = Luminumbra::Rendering::AttributedGl;
    const PhaseFrameScope frame(true);
    const auto program = Gl::CreateProgram([] { return std::uint32_t{73}; });
    const auto before = Gl::programs.attributed_calls();
    const auto lookups = Gl::programs.registry_lookups();
    int calls = 0;
    {
        const PhaseFrameScope disabled(false);
        EXPECT_FALSE(shader_first_use_collection_enabled);
        Gl::UseProgram(
            [&](std::uint32_t name) {
                EXPECT_EQ(name, program);
                ++calls;
            },
            program);
        Gl::Submit([&] { ++calls; });
        EXPECT_FALSE(Gl::programs.pending());
        Gl::UseProgram([&](std::uint32_t) { ++calls; }, program);
        Gl::Submit([&] { ++calls; });
        EXPECT_EQ(Gl::programs.attributed_calls(), before);
        EXPECT_EQ(disabled.snapshot().completed_scopes(Phase::ShaderFirstUse), 0u);
    }
    EXPECT_TRUE(shader_first_use_collection_enabled);
    Gl::UseProgram([&](std::uint32_t) { ++calls; }, program);
    Gl::Submit([&] { ++calls; });
    EXPECT_EQ(calls, 6);
    EXPECT_EQ(Gl::programs.attributed_calls(), before);
    EXPECT_EQ(Gl::programs.registry_lookups(), lookups);
    EXPECT_EQ(frame.snapshot().completed_scopes(Phase::ShaderFirstUse), 0u);
    Gl::DeleteProgram([](std::uint32_t) {}, program);
}

TEST(MenuPresentation, DisabledBindingAlonePreservesFirstSubmissionAttribution) {
    namespace Gl = Luminumbra::Rendering::AttributedGl;
    const auto program = Gl::CreateProgram([] { return std::uint32_t{74}; });
    const auto before = Gl::programs.attributed_calls();
    {
        const PhaseFrameScope disabled(false);
        Gl::UseProgram([](std::uint32_t) {}, program);
        EXPECT_TRUE(Gl::programs.pending());
        EXPECT_EQ(Gl::programs.attributed_calls(), before);
    }
    {
        const PhaseFrameScope frame(true);
        Gl::UseProgram([](std::uint32_t) {}, program);
        Gl::Submit([] {});
        EXPECT_EQ(Gl::programs.attributed_calls(), before + 2);
        EXPECT_EQ(frame.snapshot().completed_scopes(Phase::ShaderFirstUse), 2u);
    }
    Gl::DeleteProgram([](std::uint32_t) {}, program);
}

TEST(MenuPresentation, ProgramHandlesShareFirstUseAcrossCopiesGrowthAndNumericRestores) {
    const PhaseFrameScope frame(true);
    ProgramFirstUse programs;
    const auto program = programs.created(1);
    const auto copy = program;
    // Grow the registry after retaining the handle; State addresses stay stable.
    for (std::uint32_t name = 2; name <= 128; ++name)
        programs.created(name);
    programs.use([](std::uint32_t name) { EXPECT_EQ(name, 1u); }, program);
    programs.submit([] {});
    EXPECT_FALSE(programs.pending());
    EXPECT_EQ(programs.registry_lookups(), 0u);
    programs.use([](std::uint32_t) {}, copy);
    programs.submit([] {});
    EXPECT_EQ(programs.attributed_calls(), 2u);
    // A numeric state restore must share the same first-use bit as owned handles.
    programs.bind(std::uint32_t{2});
    programs.submit([] {});
    const auto calls = programs.attributed_calls();
    programs.bind(std::uint32_t{2});
    EXPECT_FALSE(programs.pending());
    programs.submit([] {});
    EXPECT_EQ(programs.attributed_calls(), calls);
    programs.deleted(2);
    EXPECT_FALSE(programs.pending());
    const auto reused = programs.created(2);
    programs.bind(reused);
    EXPECT_TRUE(programs.pending());
    programs.submit([] {});
    EXPECT_FALSE(programs.pending());
    EXPECT_EQ(programs.attributed_calls(), calls + 1);
    programs.bind(std::uint32_t{999}); // An external/untracked program remains unscoped.
    programs.submit([] {});
    EXPECT_EQ(programs.attributed_calls(), calls + 1);
    EXPECT_EQ(frame.snapshot().completed_scopes(Phase::ShaderFirstUse), calls + 1);
}

TEST(MenuPresentation, ImageAllocationChecksExactSizeBeforeAllocation) {
    EXPECT_EQ(menu_image_bytes(1920, 1080), 8'294'400u);
    EXPECT_FALSE(menu_image_bytes(0, 1080));
    EXPECT_FALSE(menu_image_bytes(1920, -1));
    const auto maximum = std::numeric_limits<int>::max();
    const auto bytes = menu_image_bytes(maximum, maximum);
    if (sizeof(std::size_t) == 4) {
        EXPECT_FALSE(bytes);
    } else {
        ASSERT_TRUE(bytes);
        EXPECT_EQ(bytes.value_or(0),
                  static_cast<std::size_t>(maximum) * static_cast<std::size_t>(maximum) * 4);
    }
}

TEST(MenuPresentation, CoveredMenuPhasesAndPollCallbacksNeverBecomeOther) {
    FrameBuckets frame{};
    PhaseTotals phases;
    // Inject durations instead of measuring clocks. Scene includes a nested
    // preview adoption, shader load, and existing foliage rebuild accounting.
    phases.add(Phase::PreviewAdoption, 9, 0, 0);
    phases.add(Phase::ShaderLoad, 4, 0, 0);
    phases.add(Phase::MenuScene, 40, 13, 6);
    frame.foliage = 6;
    phases.add(Phase::TextureLoad, 3, 0, 0);
    phases.add(Phase::DocumentLoad, 8, 3, 0);
    phases.add(Phase::UiUpdate, 12, 8, 0);
    phases.add(Phase::ShaderFirstUse, 1, 0, 0);
    phases.add(Phase::UiFilters, 5, 1, 0);
    phases.add(Phase::UiRender, 9, 5, 0);
    phases.add(Phase::WorldCreateCallback, 730, 0, 0);
    phases.add(Phase::WorldLoadCallback, 100, 0, 0);
    frame.poll = exclusive_parent_ms(832, 0, 830);
    EXPECT_DOUBLE_EQ(frame.sum() + phases.sum(), 893);
    EXPECT_DOUBLE_EQ(frame.other(893, phases), 0);
    EXPECT_DOUBLE_EQ(phases.milliseconds()[static_cast<std::size_t>(Phase::MenuScene)], 21);
    EXPECT_DOUBLE_EQ(phases.milliseconds()[static_cast<std::size_t>(Phase::UiRender)], 4);
    std::ostringstream header, row;
    write_csv_header(header);
    ASSERT_TRUE(write_csv_row(row, 1, 893, frame, {}, 0, false, 0, 0, phases));
    std::istringstream columns(row.str());
    std::string field;
    for (int index = 0; index <= 12; ++index)
        ASSERT_TRUE(std::getline(columns, field, ','));
    EXPECT_EQ(field, "0.000000"); // Serialized other excludes every covered phase.
    for (std::size_t index = 0; index < kPhaseCount; ++index)
        EXPECT_NE(header.str().find(std::string(phase_name(static_cast<Phase>(index))) + "_ms"),
                  std::string::npos);
}

TEST(MenuPresentation, BlurKernelCacheRequiresTheExactSigma) {
    UiFilterCache<BlurKernelKey, std::array<float, 4>> cache;
    const BlurKernelKey key{.sigma = 2.75f};
    const std::array<float, 4> weights{0.5f, 0.25f, 0.125f, 0.125f};
    EXPECT_EQ(cache.find(key), nullptr);
    cache.store(key, weights);
    ASSERT_NE(cache.find(key), nullptr);
    EXPECT_EQ(*cache.find(key), weights);
    EXPECT_EQ(cache.find({.sigma = 2.75001f}), nullptr);
    EXPECT_EQ(cache.find({.sigma = 3.0f}), nullptr);
    // Bounded replacement must also invalidate the evicted kernel.
    for (int index = 0; index < 8; ++index)
        cache.store({.sigma = static_cast<float>(index)}, weights);
    EXPECT_EQ(cache.find(key), nullptr);
}

TEST(MenuPresentation, BlurQuadCacheRequiresEveryUvInput) {
    UiFilterCache<FilterQuadKey, unsigned int> cache;
    const FilterQuadKey key{.offset_x = 0, .offset_y = 0, .scale_x = 1, .scale_y = 1};
    EXPECT_EQ(cache.store(key, 41), 0u);
    ASSERT_NE(cache.find(key), nullptr);
    EXPECT_EQ(*cache.find(key), 41u);
    for (int change = 0; change < 4; ++change) {
        auto changed = key;
        switch (change) {
            case 0:
                changed.offset_x = 0.01f;
                break;
            case 1:
                changed.offset_y = 0.01f;
                break;
            case 2:
                changed.scale_x = 0.999f;
                break;
            case 3:
                changed.scale_y = 0.999f;
                break;
            default:
                FAIL() << "Unknown blur quad input";
        }
        EXPECT_EQ(cache.find(changed), nullptr);
    }
    for (int index = 1; index <= 8; ++index)
        cache.store({.offset_x = static_cast<float>(index)}, 42);
    EXPECT_EQ(cache.find(key), nullptr);
}
} // namespace
