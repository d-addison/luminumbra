#include "luminumbra_client/rendering/Camera.h"
#include "luminumbra_client/rendering/FarWorldRenderer.h"
#include "luminumbra_common/core/SystemConfig.h"

#include <chrono>
#include <gtest/gtest.h>
#include <limits>
#include <stdexcept>
#include <utility>

namespace {
using namespace Luminumbra;
using namespace Luminumbra::World;
using namespace Luminumbra::Rendering;

struct DriverState {
    bool current = true, fail_upload = false, fail_draw = false;
    std::size_t draws = 0, deletes = 0, live = 0;
    bool destroyed = false, released_with_context = true, fixture_released = false;
};
// Fixed storage: fake operations neither load GL nor allocate hidden buffers.
class Driver final : public FarVolumeGpuDriver {
public:
    explicit Driver(DriverState& state)
        : state_(state) {}
    ~Driver() override {
        EXPECT_EQ(state_.live, 0u);
        EXPECT_TRUE(state_.fixture_released);
        state_.destroyed = true;
    }
    bool ValidContext() const noexcept override {
        return state_.current;
    }
    bool HasError() noexcept override {
        return std::exchange(error_, false);
    }
    void GetVertexArrayBinding(std::uint32_t& name) noexcept override {
        name = vao_;
    }
    void GetVertexBufferBinding(std::uint32_t& name) noexcept override {
        name = vbo_;
    }
    void CreateVertexArray(std::uint32_t& name) noexcept override {
        Create(name);
    }
    void CreateBuffer(std::uint32_t& name) noexcept override {
        Create(name);
    }
    void DeleteVertexArray(std::uint32_t) noexcept override {
        Delete();
    }
    void DeleteBuffer(std::uint32_t) noexcept override {
        Delete();
    }
    void BindVertexArray(std::uint32_t name) noexcept override {
        vao_ = name;
    }
    void BindBuffer(FarVolumeBufferKind kind, std::uint32_t name) noexcept override {
        if (kind == FarVolumeBufferKind::Vertex)
            vbo_ = name;
        else
            ebo_ = name;
    }
    void
    UploadBuffer(FarVolumeBufferKind kind, std::intptr_t bytes, const void*) noexcept override {
        sizes_[kind == FarVolumeBufferKind::Vertex ? vbo_ : ebo_] = bytes;
        error_ = state_.fail_upload;
    }
    void GetBufferBytes(FarVolumeBufferKind kind, std::int64_t& bytes) noexcept override {
        bytes = sizes_[kind == FarVolumeBufferKind::Vertex ? vbo_ : ebo_];
    }
    void SetAttribute(FarVolumeVertexAttribute, std::int32_t, std::size_t) noexcept override {}
    void EnableAttribute(FarVolumeVertexAttribute) noexcept override {}
    bool DrawIndexed(std::int32_t count) noexcept override {
        EXPECT_GT(count, 0);
        EXPECT_NE(vao_, 0u);
        ++state_.draws;
        return !state_.fail_draw;
    }
    std::uint32_t vao() const {
        return vao_;
    }

private:
    void Create(std::uint32_t& name) noexcept {
        name = next_++;
        if (name >= sizes_.size()) {
            name = 0;
            error_ = true;
            return;
        }
        ++state_.live;
    }
    void Delete() noexcept {
        state_.released_with_context = state_.released_with_context && state_.current &&
                                       !state_.destroyed && !state_.fixture_released;
        ++state_.deletes;
        --state_.live;
    }
    DriverState& state_;
    std::uint32_t vao_ = 0, vbo_ = 0, ebo_ = 0, next_ = 1;
    std::array<std::int64_t, 64> sizes_{};
    bool error_ = false;
};

class Fixture final : public FarWorldPristineFixture {
public:
    explicit Fixture(DriverState& state)
        : state_(state) {}
    ~Fixture() override {
        EXPECT_EQ(state_.live, 0u);
        EXPECT_FALSE(state_.destroyed);
        state_.fixture_released = true;
    }
    std::uint64_t content_identity() const noexcept override {
        return 42;
    }
    std::uint64_t owned_bytes() const noexcept override {
        return sizeof(*this);
    }
    FarVolumeNumericProfile numeric() const noexcept override {
        return FarVolumeNumericProfile::RawFloat;
    }
    std::optional<float> Height(std::int64_t, std::int64_t) const override {
        return 0.0f;
    }
    std::optional<FarVolumeSample>
    Sample(const FarVolumePosition& p, float, std::uint32_t, FarCaveMode) const override {
        const bool column = p.x >= 0 && p.x <= 256 && p.z >= 0 && p.z <= 256;
        return FarVolumeSample{column ? static_cast<float>(p.y) + .25f : 3.0f, 7};
    }

private:
    DriverState& state_;
};

FarVolumeWantedRequest Request(FarTileKey key = {5, 0, 0}) {
    FarVolumeTileKey canonical;
    EXPECT_TRUE(ToFarVolumeTileKey(key, canonical));
    return {{1, 42, 0, key, 1, 1}, {canonical, {}}, 0};
}
struct Owner {
    DriverState state;
    JobSystem jobs;
    FarWorldRenderer renderer{FarWorldMode::Ladder};
    std::uint64_t frame = 0;
    explicit Owner(const FarWorldExperimentalLimits& limits = FarWorldDevelopmentLimits()) {
        jobs.startup(2);
        renderer.InstallBackend(std::make_unique<Driver>(state));
        EXPECT_EQ(renderer.BindFixture(jobs, std::make_shared<const Fixture>(state), 1, 1, limits),
                  FarWorldRefusal::None);
    }
    ~Owner() {
        renderer.Shutdown();
        jobs.shutdown();
    }
    bool Advance(const FarVolumeWantedRequest& request) {
        return renderer.AdvanceFrame(++frame, {0, 0, 0}, std::span(&request, 1));
    }
    bool WaitSettled(const FarVolumeWantedRequest& request) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
        do {
            if (!Advance(request))
                return false;
            const auto observed = renderer.Residency()->Observe(request.identity.key);
            if (observed && (observed->eligibility == FarVolumeEligibility::Settled ||
                             observed->eligibility == FarVolumeEligibility::RequestRejected))
                return observed->eligibility == FarVolumeEligibility::Settled;
            std::this_thread::yield();
        } while (std::chrono::steady_clock::now() < deadline);
        return false;
    }
};

void ExpectManifestRefusedTransactionally(Owner& owner,
                                          std::span<const FarVolumeWantedRequest> requests,
                                          FarWorldRefusal expected) {
    // Finish prior worker activity so charge comparisons cannot race a build.
    // The refused manifest must neither dispatch work nor mutate residency.
    owner.jobs.shutdown();
    const auto before = owner.renderer.Residency()->Snapshot();
    const auto frame = owner.renderer.LastFrame();
    const auto live = owner.state.live;
    const auto deletes = owner.state.deletes;
    EXPECT_FALSE(owner.renderer.AdvanceFrame(++owner.frame, {0, 0, 0}, requests));
    const auto selection = owner.renderer.Selection(FarWorldPurpose::World);
    EXPECT_EQ(selection.refusal, expected);
    EXPECT_EQ(selection.effective, FarWorldMode::Legacy);
    const auto after = owner.renderer.Residency()->Snapshot();
    for (std::size_t tier = 0; tier < kFarTierCount; ++tier) {
        EXPECT_EQ(after[tier].wanted, before[tier].wanted);
        EXPECT_EQ(after[tier].pending, before[tier].pending);
        EXPECT_EQ(after[tier].building, before[tier].building);
        EXPECT_EQ(after[tier].ready, before[tier].ready);
        EXPECT_EQ(after[tier].settled, before[tier].settled);
        EXPECT_EQ(after[tier].retained_gpu_bytes, before[tier].retained_gpu_bytes);
        EXPECT_EQ(after[tier].memory.bytes_cpu, before[tier].memory.bytes_cpu);
        EXPECT_EQ(after[tier].memory.bytes_gpu, before[tier].memory.bytes_gpu);
        EXPECT_EQ(after[tier].memory.bytes_total, before[tier].memory.bytes_total);
        EXPECT_EQ(after[tier].memory.peak_total, before[tier].memory.peak_total);
        EXPECT_EQ(after[tier].memory.admission_refusals, before[tier].memory.admission_refusals);
    }
    EXPECT_EQ(owner.renderer.LastFrame().dispatched, frame.dispatched);
    EXPECT_EQ(owner.renderer.LastFrame().uploaded, frame.uploaded);
    EXPECT_EQ(owner.renderer.LastFrame().upload_bytes, frame.upload_bytes);
    EXPECT_EQ(owner.state.live, live);
    EXPECT_EQ(owner.state.deletes, deletes);
}
} // namespace

TEST(FarWorldRenderer, ConfigDefaultExplicitAndHashExcluded) {
    using luminumbra::core::SysKey;
    using luminumbra::core::SystemConfig;
    const auto defaults = SystemConfig::Defaults();
    const auto disabled =
        SystemConfig::FromJsonString(R"({"render":{"far_world_ladder":{"enabled":false}}})");
    const auto enabled =
        SystemConfig::FromJsonString(R"({"render":{"far_world_ladder":{"enabled":true}}})");
    EXPECT_EQ(RequestedFarWorldMode(defaults.enabled(SysKey::RenderFarWorldLadder)),
              FarWorldMode::Legacy);
    EXPECT_EQ(RequestedFarWorldMode(disabled.enabled(SysKey::RenderFarWorldLadder)),
              FarWorldMode::Legacy);
    EXPECT_EQ(RequestedFarWorldMode(enabled.enabled(SysKey::RenderFarWorldLadder)),
              FarWorldMode::Ladder);
    EXPECT_EQ(enabled.ComputeConfigSubHash(), defaults.ComputeConfigSubHash());
    auto user = defaults;
    user.OverlayUserFromJsonString(R"({"render":{"far_world_ladder":{"enabled":true}}})");
    EXPECT_FALSE(user.enabled(SysKey::RenderFarWorldLadder));
}

TEST(FarWorldRenderer, DefaultExplicitRefusedAndPreviewSelection) {
    const FarWorldRenderer legacy;
    EXPECT_EQ(legacy.Selection(FarWorldPurpose::World).effective, FarWorldMode::Legacy);
    const FarWorldRenderer refused(FarWorldMode::Ladder);
    const auto normal = refused.Selection(FarWorldPurpose::World);
    EXPECT_EQ(normal.requested, FarWorldMode::Ladder);
    EXPECT_EQ(normal.effective, FarWorldMode::Legacy);
    EXPECT_EQ(normal.refusal, FarWorldRefusal::ImmutableAuthorityUnavailable);
    const auto ladder =
        SelectFarWorld(FarWorldMode::Ladder, FarWorldPurpose::World, FarWorldRefusal::None);
    EXPECT_EQ(ladder.effective, FarWorldMode::Ladder);
    const auto suppressed =
        SelectFarWorld(FarWorldMode::Ladder, FarWorldPurpose::World, FarWorldRefusal::None, false);
    EXPECT_EQ(suppressed.effective, FarWorldMode::Ladder);
    EXPECT_FALSE(suppressed.advance);
    EXPECT_FALSE(suppressed.submit);
    const auto preview =
        SelectFarWorld(FarWorldMode::Ladder, FarWorldPurpose::Preview, FarWorldRefusal::None);
    EXPECT_EQ(preview.requested, FarWorldMode::Ladder);
    EXPECT_EQ(preview.effective, FarWorldMode::Legacy);
    EXPECT_TRUE(preview.advance);
    EXPECT_EQ(preview.refusal, FarWorldRefusal::PreviewOverride);
    const auto offscreen = refused.Selection(FarWorldPurpose::OffscreenPreview);
    EXPECT_FALSE(offscreen.advance);
    EXPECT_FALSE(offscreen.submit);
    EXPECT_EQ(offscreen.refusal, FarWorldRefusal::OffscreenSuppressed);
}

TEST(FarWorldRenderer, ExclusiveSchedulerSubmissionAndOffscreenSuppression) {
    std::size_t legacy_schedules = 0, ladder_schedules = 0, legacy_draws = 0, ladder_draws = 0;
    auto dispatch = [&](FarWorldSelection selected) {
        AdvanceSelectedFarWorld(selected, [&] { ++legacy_schedules; }, [&] { ++ladder_schedules; });
        return SubmitSelectedFarWorld(
            selected,
            [&] {
                ++legacy_draws;
                return FarWorldSubmitStats{1, 3, 0};
            },
            [&](const auto&) {
                ++ladder_draws;
                return FarWorldSubmitStats{2, 6, 0};
            },
            {});
    };
    EXPECT_EQ(dispatch(SelectFarWorld(
                           FarWorldMode::Legacy, FarWorldPurpose::World, FarWorldRefusal::None))
                  .draws,
              1u);
    EXPECT_EQ(dispatch(SelectFarWorld(
                           FarWorldMode::Ladder, FarWorldPurpose::World, FarWorldRefusal::None))
                  .draws,
              2u);
    EXPECT_EQ(dispatch(SelectFarWorld(
                           FarWorldMode::Ladder, FarWorldPurpose::Preview, FarWorldRefusal::None))
                  .draws,
              1u);
    EXPECT_EQ(dispatch(SelectFarWorld(FarWorldMode::Ladder,
                                      FarWorldPurpose::OffscreenPreview,
                                      FarWorldRefusal::None))
                  .draws,
              0u);
    EXPECT_EQ(legacy_schedules, 2u);
    EXPECT_EQ(ladder_schedules, 1u);
    EXPECT_EQ(legacy_draws, legacy_schedules);
    EXPECT_EQ(ladder_draws, ladder_schedules);
}

TEST(FarWorldRenderer, LimitsRequireEveryBudgetQueueWorkAndFrameBound) {
    const auto profile = FarWorldDevelopmentLimits();
    ASSERT_TRUE(profile.Complete());
    auto missing = profile;
    missing.budgets.bytes[2] = 0;
    EXPECT_FALSE(missing.Complete());
    missing = profile;
    missing.residency.queues[4].compilation.authority.max_density_samples = 0;
    EXPECT_FALSE(missing.Complete());
    missing = profile;
    missing.residency.queues[0].max_converted_bytes = 0;
    EXPECT_FALSE(missing.Complete());
    missing = profile;
    missing.frame.upload_bytes = 0;
    EXPECT_FALSE(missing.Complete());
    JobSystem jobs;
    FarWorldRenderer renderer(FarWorldMode::Ladder);
    EXPECT_EQ(renderer.BindFixture(jobs, {}, 1, 1, missing), FarWorldRefusal::MissingLimits);
    EXPECT_EQ(renderer.BindFixture(jobs, {}, 1, 1, profile),
              FarWorldRefusal::ImmutableAuthorityUnavailable);
}

TEST(FarWorldRenderer, OneAdvancementPerLogicalFrameAndGlobalCoarseFirst) {
    auto limits = FarWorldDevelopmentLimits();
    limits.frame.builds = 1;
    Owner owner(limits);
    const std::array wanted{Request({1, 0, 0}), Request({5, 0, 0})};
    ASSERT_TRUE(owner.renderer.AdvanceFrame(10, {0, 0, 0}, wanted));
    const auto first = owner.renderer.LastFrame();
    EXPECT_EQ(first.dispatched, 1u);
    const auto coarse = owner.renderer.Residency()->Observe(wanted[1].identity.key);
    ASSERT_TRUE(coarse) << "Missing coarse tile observation";
    if (!coarse)
        return;
    EXPECT_EQ(coarse->eligibility, FarVolumeEligibility::Building);
    const auto fine = owner.renderer.Residency()->Observe(wanted[0].identity.key);
    ASSERT_TRUE(fine) << "Missing fine tile observation";
    if (!fine)
        return;
    EXPECT_EQ(fine->eligibility, FarVolumeEligibility::Wanted);
    EXPECT_FALSE(owner.renderer.AdvanceFrame(10, {0, 0, 0}, wanted));
    EXPECT_FALSE(owner.renderer.AdvanceFrame(9, {0, 0, 0}, wanted));
    EXPECT_EQ(owner.renderer.LastFrame().dispatched, first.dispatched);
    EXPECT_TRUE(owner.renderer.AdvanceFrame(11, {0, 0, 0}, wanted));
}

TEST(FarWorldRenderer, IncompatibleEntryDescriptorLimitsRefuseWithoutThrowing) {
    DriverState state;
    JobSystem jobs;
    FarWorldRenderer renderer(FarWorldMode::Ladder);
    renderer.InstallBackend(std::make_unique<Driver>(state));
    for (std::size_t tier = 0; tier < kFarTierCount; ++tier) {
        auto limits = FarWorldDevelopmentLimits();
        limits.residency.queues[tier].descriptor_slots = 63;
        ASSERT_EQ(limits.residency.entry_slots[tier], 64u);
        EXPECT_FALSE(limits.Complete());
        auto outcome = FarWorldRefusal::None;
        EXPECT_NO_THROW(outcome = renderer.BindFixture(
                            jobs, std::make_shared<const Fixture>(state), 1, 1, limits));
        EXPECT_EQ(outcome, FarWorldRefusal::MissingLimits);
        EXPECT_EQ(renderer.Residency(), nullptr);
        const auto selection = renderer.Selection(FarWorldPurpose::World);
        EXPECT_EQ(selection.effective, FarWorldMode::Legacy);
        EXPECT_EQ(selection.refusal, FarWorldRefusal::MissingLimits);
        EXPECT_EQ(state.live, 0u);
    }
}

TEST(FarWorldRenderer, StaleBindingIdentityCannotReplaceWanted) {
    for (unsigned field = 0; field < 5; ++field) {
        Owner owner;
        const auto valid = Request();
        ASSERT_TRUE(owner.Advance(valid));
        auto stale = valid;
        if (field == 0)
            ++stale.identity.world_epoch;
        if (field == 1)
            ++stale.identity.content_hash;
        if (field == 2)
            ++stale.identity.cave_policy;
        if (field == 3)
            ++stale.identity.bake_generation;
        if (field == 4)
            --stale.identity.span_generation;
        EXPECT_FALSE(owner.Advance(stale));
        const auto observed = owner.renderer.Residency()->Observe(valid.identity.key);
        ASSERT_TRUE(observed);
        if (!observed)
            return;
        EXPECT_EQ(observed->desired, valid.identity);
        EXPECT_EQ(owner.renderer.Selection(FarWorldPurpose::World).refusal,
                  FarWorldRefusal::InvalidRequests);
    }
}

TEST(FarWorldRenderer, UploadedDrawMetadataAndSynchronousBorrowing) {
    Owner owner;
    const auto request = Request();
    ASSERT_TRUE(owner.WaitSettled(request));
    owner.renderer.BeginLiveOwnership(owner.frame);
    ASSERT_TRUE(owner.renderer.FinishLiveOwnership());
    const auto stats = owner.renderer.Submit({.5, -128.25, -.25}, [&](const auto& command) {
        EXPECT_FLOAT_EQ(command.relative_origin[0], -.5f);
        EXPECT_FLOAT_EQ(command.relative_origin[1], -127.75f);
        EXPECT_FLOAT_EQ(command.relative_origin[2], .25f);
        EXPECT_EQ(command.horizontal_outer_clip_meters, 16384);
        EXPECT_EQ(command.view_ray_bias_meters, 32);
        EXPECT_GT(command.index_count, 0);
        EXPECT_THROW(owner.renderer.PrepareWorldSwap(), std::logic_error);
        return true;
    });
    EXPECT_EQ(stats.draws, 1u);
    EXPECT_EQ(stats.refused, 0u);
    EXPECT_EQ(owner.state.draws, 1u);
}

TEST(FarWorldRenderer, WorldSwapDrainsBeforeAuthorityMemoryDriverAndContextRelease) {
    Owner owner;
    ASSERT_TRUE(owner.WaitSettled(Request()));
    ASSERT_GT(owner.state.live, 0u);
    owner.renderer.PrepareWorldSwap();
    EXPECT_EQ(owner.state.live, 0u);
    EXPECT_TRUE(owner.state.fixture_released);
    EXPECT_TRUE(owner.state.released_with_context);
    EXPECT_FALSE(owner.state.destroyed);
    EXPECT_EQ(owner.renderer.world_epoch(), 0u);
    EXPECT_EQ(owner.renderer.Selection(FarWorldPurpose::World).effective, FarWorldMode::Legacy);
    owner.renderer.Shutdown();
    EXPECT_TRUE(owner.state.destroyed);
    owner.state.current = false; // Client may now destroy the context.
}

TEST(FarWorldRenderer, FailedRebuildRetainsOldAllocationAndLogicalChargeButDoesNotDraw) {
    Owner owner;
    auto request = Request();
    ASSERT_TRUE(owner.WaitSettled(request));
    const auto* old = owner.renderer.Residency()->Resource(request.identity.key);
    ASSERT_NE(old, nullptr);
    const auto bytes = old->bytes();
    const auto live = owner.state.live;
    request.intent.extra_span = FarVolumeCoverageSpan{-512, 512};
    ++request.identity.span_generation;
    owner.state.fail_upload = true;
    EXPECT_FALSE(owner.WaitSettled(request));
    EXPECT_EQ(owner.renderer.Residency()->Resource(request.identity.key), old);
    const auto observed = owner.renderer.Residency()->Observe(request.identity.key);
    ASSERT_TRUE(observed);
    if (!observed)
        return;
    EXPECT_EQ(observed->gpu_bytes, bytes);
    EXPECT_FALSE(IsFarTileCovering(observed->coverage));
    EXPECT_EQ(owner.state.live, live);
    owner.renderer.BeginLiveOwnership(owner.frame);
    ASSERT_TRUE(owner.renderer.FinishLiveOwnership());
    std::size_t prepares = 0;
    const auto submission = owner.renderer.Submit({0, 0, 0}, [&](const auto&) {
        ++prepares;
        return true;
    });
    EXPECT_EQ(submission.draws, 0u);
    EXPECT_EQ(submission.refused, 0u); // complete mask; stale residency itself excludes the draw
    EXPECT_EQ(prepares, 0u);
    const auto attempts = owner.renderer.LastFrame().upload_attempts;
    EXPECT_EQ(attempts, 1u);
    EXPECT_GT(owner.renderer.LastFrame().upload_bytes, 0u);
    EXPECT_EQ(owner.renderer.LastFrame().successful_upload_bytes, 0u);
    ASSERT_TRUE(owner.Advance(request));
    EXPECT_EQ(owner.renderer.LastFrame().upload_attempts, 0u);
}

TEST(FarWorldRenderer, LogicalAdmissionIncludesIntegrationBeforeAnyBuildOrUpload) {
    DriverState state;
    JobSystem jobs;
    FarWorldRenderer renderer(FarWorldMode::Ladder);
    renderer.InstallBackend(std::make_unique<Driver>(state));
    auto limits = FarWorldDevelopmentLimits();
    limits.budgets.bytes[4] = 1;
    auto fixture = std::make_shared<const Fixture>(state);
    EXPECT_EQ(renderer.BindFixture(jobs, fixture, 1, 1, limits), FarWorldRefusal::AdmissionFailed);
    EXPECT_EQ(renderer.Residency(), nullptr);
    EXPECT_EQ(state.live, 0u);
    fixture.reset();
}

TEST(FarWorldRenderer, FramePlaneProjectionAndInverseFollowEffectiveSelection) {
    Camera camera;
    for (const auto mode : {FarWorldMode::Legacy, FarWorldMode::Ladder}) {
        camera.SelectFarWorldPlane(mode);
        EXPECT_EQ(camera.GetFarPlane(), FarWorldPlane(mode));
        EXPECT_EQ(camera.GetNearPlane(), .1f);
        const auto projection = camera.GetProjectionMatrix(640, 480);
        const auto point = glm::vec4(0, 0, -camera.GetFarPlane(), 1);
        const auto clip = projection * point;
        EXPECT_NEAR(clip.z / clip.w, 0, 1e-7);
        const auto restored = glm::inverse(projection) * clip;
        EXPECT_NEAR(restored.z / restored.w, point.z, .01);
    }
    camera.SelectFarWorldPlane(FarWorldMode::Legacy);
    EXPECT_EQ(camera.GetFarPlane(), 3200);
}

TEST(FarWorldRenderer, UploadByteAdmissionRefusesBeforeAnyNativeAllocation) {
    auto limits = FarWorldDevelopmentLimits();
    limits.frame.upload_bytes = 1;
    Owner owner(limits);
    EXPECT_FALSE(owner.WaitSettled(Request()));
    const auto observed = owner.renderer.Residency()->Observe(Request().identity.key);
    ASSERT_TRUE(observed);
    if (!observed)
        return;
    EXPECT_EQ(observed->eligibility, FarVolumeEligibility::RequestRejected);
    EXPECT_EQ(observed->gpu_bytes, 0u);
    EXPECT_EQ(owner.state.live, 0u);
    EXPECT_EQ(owner.state.deletes, 0u);
}

TEST(FarWorldRenderer, CameraXZWantedSetSpanGrowthAndRejectedExtensionAreTransactional) {
    Owner owner;
    std::vector<FarTileKey> wanted;
    ASSERT_EQ(ComputeFarWantedSet(-.5, -512.25, wanted), FarWantedError::None);
    ASSERT_TRUE(owner.renderer.PrepareFrame(1, {-.5, -20, -512.25}));
    const auto first = owner.renderer.Residency()->Snapshot();
    std::size_t count = 0;
    for (const auto& tier : first)
        count += tier.wanted;
    EXPECT_EQ(count, wanted.size());
    const auto before = owner.renderer.Residency()->Observe(wanted.front());
    ASSERT_TRUE(before);
    if (!before)
        return;
    ASSERT_TRUE(owner.renderer.PrepareFrame(2, {-.5, 100, -512.25}));
    const auto after = owner.renderer.Residency()->Observe(wanted.front());
    ASSERT_TRUE(after);
    if (!after)
        return;
    EXPECT_GT(after->desired.span_generation, before->desired.span_generation);
    ASSERT_TRUE(owner.renderer.PrepareFrame(3, {-.5, 0, -512.25}));
    const auto unchanged = owner.renderer.Residency()->Observe(wanted.front());
    ASSERT_TRUE(unchanged) << "Missing tile observation after unchanged span";
    if (!unchanged)
        return;
    EXPECT_EQ(unchanged->desired, after->desired);
    const std::array invalid{FarVolumeCoverageSpan{-9000, 0}};
    EXPECT_FALSE(owner.renderer.PrepareFrame(4, {-.5, 0, -512.25}, invalid));
    const auto rejected = owner.renderer.Residency()->Observe(wanted.front());
    ASSERT_TRUE(rejected) << "Missing tile observation after rejected extension";
    if (!rejected)
        return;
    EXPECT_EQ(rejected->desired, after->desired);
    EXPECT_EQ(owner.renderer.Selection(FarWorldPurpose::World).refusal,
              FarWorldRefusal::SpanRejected);
}

TEST(FarWorldRenderer, LeaveAndReturnPreservesAcceptedExtensionsAndGeneration) {
    Owner owner;
    const FarTileKey key{1, -1, -2};
    const std::array extensions{FarVolumeCoverageSpan{-100, 1200}};
    ASSERT_TRUE(owner.renderer.PrepareFrame(1, {-.5, 1000, -512.25}, extensions));
    const auto before = owner.renderer.Residency()->Observe(key);
    ASSERT_TRUE(before);
    if (!before)
        return;
    ASSERT_TRUE(owner.renderer.PrepareFrame(2, {65536, 500, 65536}));
    EXPECT_FALSE(owner.renderer.Residency()->Observe(key));
    ASSERT_TRUE(owner.renderer.PrepareFrame(3, {-.5, 500, -512.25}));
    const auto returned = owner.renderer.Residency()->Observe(key);
    ASSERT_TRUE(returned);
    if (!returned)
        return;
    EXPECT_EQ(returned->desired, before->desired);
    // Equal identity freezes the complete intent in ReplaceWanted. Accepting
    // this manifest proves that the returned request kept the prior coverage.
    auto expected = Request(key);
    expected.identity = returned->desired;
    expected.intent.extra_span = extensions.front();
    expected.build_headroom_bytes = FarWorldDevelopmentLimits().build_headroom_bytes;
    owner.frame = 3;
    EXPECT_TRUE(owner.Advance(expected));
}

TEST(FarWorldRenderer, LeaveAndReturnSpanGrowthKeepsGenerationMonotone) {
    Owner owner;
    const FarTileKey key{1, 0, 0};
    ASSERT_TRUE(owner.renderer.PrepareFrame(1, {0, 1000, 0}));
    const auto before = owner.renderer.Residency()->Observe(key);
    ASSERT_TRUE(before);
    if (!before)
        return;
    ASSERT_TRUE(owner.renderer.PrepareFrame(2, {65536, 0, 65536}));
    EXPECT_FALSE(owner.renderer.Residency()->Observe(key));
    ASSERT_TRUE(owner.renderer.PrepareFrame(3, {0, 0, 0}));
    const auto returned = owner.renderer.Residency()->Observe(key);
    ASSERT_TRUE(returned);
    if (!returned)
        return;
    EXPECT_GT(returned->desired.span_generation, before->desired.span_generation);
    auto expected = Request(key);
    expected.identity = returned->desired;
    expected.intent.extra_span = FarVolumeCoverageSpan{0, 1000};
    expected.build_headroom_bytes = FarWorldDevelopmentLimits().build_headroom_bytes;
    owner.frame = 3;
    ASSERT_TRUE(owner.Advance(expected));
    ASSERT_TRUE(owner.renderer.PrepareFrame(5, {65536, 0, 65536}));
    EXPECT_FALSE(owner.renderer.Residency()->Observe(key));
    ASSERT_TRUE(owner.renderer.PrepareFrame(6, {0, -100, 0}));
    const auto grown = owner.renderer.Residency()->Observe(key);
    ASSERT_TRUE(grown);
    if (!grown)
        return;
    EXPECT_GT(grown->desired.span_generation, returned->desired.span_generation);
    expected.identity = grown->desired;
    expected.intent.extra_span = FarVolumeCoverageSpan{-100, 1000};
    owner.frame = 6;
    EXPECT_TRUE(owner.Advance(expected));
}

TEST(FarWorldRenderer, SpanHistoryCapacityExhaustionRefusesTransactionally) {
    Owner owner;
    constexpr std::size_t history_slots = 512; // Explicit development profile bound.
    std::size_t tracked = 0;
    FarTileKey last_key;
    FarBuildIdentity last_identity;
    std::array<FarVolumeTierObservation, kFarTierCount> last_snapshot{};
    FarVolumeFrameResult last_frame;
    // Stations are farther apart than any two wanted footprints; each adds a
    // disjoint key set even though residency evicts the preceding wanted set.
    for (std::uint64_t station = 0; station < 8; ++station) {
        const double coordinate = static_cast<double>(station) * 65536;
        std::vector<FarTileKey> wanted;
        ASSERT_EQ(ComputeFarWantedSet(coordinate, coordinate, wanted), FarWantedError::None);
        if (tracked + wanted.size() <= history_slots) {
            ASSERT_TRUE(owner.renderer.PrepareFrame(station + 1, {coordinate, 0, coordinate}));
            tracked += wanted.size();
            last_key = wanted.front();
            const auto observed = owner.renderer.Residency()->Observe(last_key);
            ASSERT_TRUE(observed);
            if (!observed)
                return;
            last_identity = observed->desired;
            last_snapshot = owner.renderer.Residency()->Snapshot();
            last_frame = owner.renderer.LastFrame();
            continue;
        }
        ASSERT_GT(tracked, 0u);
        EXPECT_FALSE(owner.renderer.PrepareFrame(station + 1, {coordinate, 0, coordinate}));
        const auto selection = owner.renderer.Selection(FarWorldPurpose::World);
        EXPECT_EQ(selection.refusal, FarWorldRefusal::SpanHistoryExhausted);
        EXPECT_EQ(selection.effective, FarWorldMode::Legacy);
        const auto observed = owner.renderer.Residency()->Observe(last_key);
        ASSERT_TRUE(observed);
        if (!observed)
            return;
        EXPECT_EQ(observed->desired, last_identity);
        EXPECT_FALSE(owner.renderer.Residency()->Observe(wanted.front()));
        const auto snapshot = owner.renderer.Residency()->Snapshot();
        for (std::size_t tier = 0; tier < kFarTierCount; ++tier)
            EXPECT_EQ(snapshot[tier].wanted, last_snapshot[tier].wanted);
        EXPECT_EQ(owner.renderer.LastFrame().dispatched, last_frame.dispatched);
        return;
    }
    FAIL() << "Disjoint wanted sets must exhaust the bounded retained history";
}

TEST(FarWorldRenderer, AdvanceFrameLeaveAndReturnRejectsGenerationRegression) {
    Owner owner;
    auto accepted = Request();
    accepted.identity.span_generation = 2;
    accepted.intent.extra_span = FarVolumeCoverageSpan{-100, 1200};
    ASSERT_TRUE(owner.Advance(accepted));
    ASSERT_TRUE(owner.renderer.AdvanceFrame(++owner.frame, {0, 0, 0}, {}));
    ASSERT_FALSE(owner.renderer.Residency()->Observe(accepted.identity.key));
    auto stale = accepted;
    stale.identity.span_generation = 1;
    stale.intent.extra_span = FarVolumeCoverageSpan{0, 0};
    ExpectManifestRefusedTransactionally(
        owner, std::span(&stale, 1), FarWorldRefusal::InvalidRequests);
    EXPECT_FALSE(owner.renderer.Residency()->Observe(accepted.identity.key));
}

TEST(FarWorldRenderer, AdvanceFrameLeaveAndReturnRejectsGenerationReuseWithChangedIntent) {
    Owner owner;
    auto accepted = Request();
    accepted.identity.span_generation = 2;
    accepted.intent.extra_span = FarVolumeCoverageSpan{-100, 1200};
    ASSERT_TRUE(owner.Advance(accepted));
    ASSERT_TRUE(owner.renderer.AdvanceFrame(++owner.frame, {0, 0, 0}, {}));
    ASSERT_FALSE(owner.renderer.Residency()->Observe(accepted.identity.key));
    auto reused = accepted;
    reused.intent.extra_span = FarVolumeCoverageSpan{-200, 1200};
    // A valid prefix must not be admitted before rejecting the returning key.
    const std::array manifest{Request({1, 100, 100}), reused};
    ExpectManifestRefusedTransactionally(owner, manifest, FarWorldRefusal::InvalidRequests);
    EXPECT_FALSE(owner.renderer.Residency()->Observe(manifest.front().identity.key));
    EXPECT_FALSE(owner.renderer.Residency()->Observe(accepted.identity.key));
}

TEST(FarWorldRenderer, AdvanceFrameLeaveAndReturnRejectsCoverageShrinkage) {
    const std::array<std::optional<FarVolumeCoverageSpan>, 3> shrunken{
        FarVolumeCoverageSpan{0, 1200}, FarVolumeCoverageSpan{-100, 1100}, std::nullopt};
    for (const auto& extension : shrunken) {
        Owner owner;
        auto accepted = Request();
        accepted.identity.span_generation = 2;
        accepted.intent.extra_span = FarVolumeCoverageSpan{-100, 1200};
        ASSERT_TRUE(owner.Advance(accepted));
        ASSERT_TRUE(owner.renderer.AdvanceFrame(++owner.frame, {0, 0, 0}, {}));
        ASSERT_FALSE(owner.renderer.Residency()->Observe(accepted.identity.key));
        auto shrinking = accepted;
        ++shrinking.identity.span_generation;
        shrinking.intent.extra_span = extension;
        const std::array manifest{Request({1, 100, 100}), shrinking};
        ExpectManifestRefusedTransactionally(owner, manifest, FarWorldRefusal::SpanRejected);
        EXPECT_FALSE(owner.renderer.Residency()->Observe(manifest.front().identity.key));
        EXPECT_FALSE(owner.renderer.Residency()->Observe(accepted.identity.key));
    }
}

TEST(FarWorldRenderer, AdvanceFrameSpanHistoryCapacityExhaustionRefusesTransactionally) {
    Owner owner;
    constexpr std::size_t history_slots = 512;
    std::array<FarVolumeWantedRequest, 64> batch;
    for (std::size_t offset = 0; offset < history_slots; offset += batch.size()) {
        for (std::size_t i = 0; i < batch.size(); ++i)
            batch[i] = Request({1, static_cast<std::int64_t>(100 + offset + i), 100});
        ASSERT_TRUE(owner.renderer.AdvanceFrame(++owner.frame, {0, 0, 0}, batch));
    }
    // Replaying existing keys at full capacity still succeeds.
    ASSERT_TRUE(owner.renderer.AdvanceFrame(++owner.frame, {0, 0, 0}, batch));
    const auto retained = batch.back();
    auto growth = retained;
    ++growth.identity.span_generation;
    growth.intent.extra_span = FarVolumeCoverageSpan{0, 0};
    const std::array manifest{growth, Request({1, 612, 100})};
    ExpectManifestRefusedTransactionally(owner, manifest, FarWorldRefusal::SpanHistoryExhausted);
    const auto observed = owner.renderer.Residency()->Observe(retained.identity.key);
    ASSERT_TRUE(observed);
    if (!observed)
        return;
    EXPECT_EQ(observed->desired, retained.identity);
    EXPECT_FALSE(owner.renderer.Residency()->Observe(manifest.back().identity.key));
}

TEST(FarWorldRenderer, MixedPrepareFrameAndAdvanceFramePreserveHistoryAndGenerations) {
    Owner owner;
    const FarTileKey key{1, 0, 0};
    const std::array extensions{FarVolumeCoverageSpan{-100, 1200}};
    ASSERT_TRUE(owner.renderer.PrepareFrame(++owner.frame, {0, 1000, 0}, extensions));
    const auto prepared = owner.renderer.Residency()->Observe(key);
    ASSERT_TRUE(prepared);
    if (!prepared)
        return;
    ASSERT_TRUE(owner.renderer.AdvanceFrame(++owner.frame, {0, 0, 0}, {}));
    ASSERT_FALSE(owner.renderer.Residency()->Observe(key));
    auto request = Request(key);
    request.identity = prepared->desired;
    request.intent.extra_span = extensions.front();
    request.build_headroom_bytes = FarWorldDevelopmentLimits().build_headroom_bytes;
    ASSERT_TRUE(owner.Advance(request));
    request.identity.span_generation += 7;
    request.intent.extra_span = FarVolumeCoverageSpan{-200, 1200};
    ASSERT_TRUE(owner.Advance(request));
    ASSERT_TRUE(owner.renderer.AdvanceFrame(++owner.frame, {0, 0, 0}, {}));
    ASSERT_FALSE(owner.renderer.Residency()->Observe(key));
    ASSERT_TRUE(owner.renderer.PrepareFrame(++owner.frame, {0, 500, 0}));
    const auto returned = owner.renderer.Residency()->Observe(key);
    ASSERT_TRUE(returned);
    if (!returned)
        return;
    EXPECT_EQ(returned->desired, request.identity);
    ASSERT_TRUE(owner.Advance(request)); // Equal identity also checks the complete intent.
    ASSERT_TRUE(owner.renderer.PrepareFrame(++owner.frame, {0, -300, 0}));
    const auto grown = owner.renderer.Residency()->Observe(key);
    ASSERT_TRUE(grown);
    if (!grown)
        return;
    EXPECT_EQ(grown->desired.span_generation, request.identity.span_generation + 1);
    auto expected = request;
    expected.identity = grown->desired;
    expected.intent.extra_span = FarVolumeCoverageSpan{-300, 1200};
    ASSERT_TRUE(owner.Advance(expected));
    ASSERT_TRUE(owner.renderer.AdvanceFrame(++owner.frame, {0, 0, 0}, {}));
    // AdvanceFrame must consult the later generation accepted by PrepareFrame.
    ExpectManifestRefusedTransactionally(
        owner, std::span(&request, 1), FarWorldRefusal::InvalidRequests);
}

TEST(FarWorldRenderer, PrepareFrameHistoryRejectsChangedIntentAndShrinkageThroughAdvanceFrame) {
    for (const bool shrink : {false, true}) {
        Owner owner;
        const FarTileKey key{1, 0, 0};
        const std::array extensions{FarVolumeCoverageSpan{-100, 1200}};
        ASSERT_TRUE(owner.renderer.PrepareFrame(++owner.frame, {0, 1000, 0}, extensions));
        const auto prepared = owner.renderer.Residency()->Observe(key);
        ASSERT_TRUE(prepared);
        if (!prepared)
            return;
        ASSERT_TRUE(owner.renderer.AdvanceFrame(++owner.frame, {0, 0, 0}, {}));
        auto request = Request(key);
        request.identity = prepared->desired;
        request.build_headroom_bytes = FarWorldDevelopmentLimits().build_headroom_bytes;
        if (shrink) {
            ++request.identity.span_generation;
            request.intent.extra_span = FarVolumeCoverageSpan{0, 0};
        } else {
            request.intent.extra_span = FarVolumeCoverageSpan{-200, 1200};
        }
        ExpectManifestRefusedTransactionally(owner,
                                             std::span(&request, 1),
                                             shrink ? FarWorldRefusal::SpanRejected
                                                    : FarWorldRefusal::InvalidRequests);
        EXPECT_FALSE(owner.renderer.Residency()->Observe(key));
    }
}

TEST(FarWorldRenderer, CheckedDrawRejectsForeignBackendAndRestoresBindingsOnFailure) {
    struct Foreign final : FarVolumeGpuAllocation {
        std::uint64_t bytes() const noexcept override {
            return 96;
        }
    } foreign;
    DriverState state, other_state;
    Driver driver(state), other(other_state);
    FarVolumeRenderMesh mesh;
    mesh.content = FarVolumeContent::Mixed;
    mesh.vertices = {
        {{0, 0, 0}, {0, 1, 0}, 7}, {{1, 0, 0}, {0, 1, 0}, 7}, {{0, 0, 1}, {0, 1, 0}, 7}};
    mesh.indices = {0, 1, 2};
    auto allocation = UploadFarVolumeGpu(mesh, driver);
    ASSERT_NE(allocation, nullptr);
    EXPECT_FALSE(DrawFarVolumeGpu(foreign, driver));
    EXPECT_FALSE(DrawFarVolumeGpu(*allocation, other));
    EXPECT_EQ(state.draws, 0u);
    state.current = false;
    EXPECT_FALSE(DrawFarVolumeGpu(*allocation, driver));
    state.current = true;
    state.fail_draw = true;
    EXPECT_FALSE(DrawFarVolumeGpu(*allocation, driver));
    EXPECT_EQ(driver.vao(), 0u);
    state.fail_draw = false;
    EXPECT_TRUE(DrawFarVolumeGpu(*allocation, driver));
    EXPECT_EQ(driver.vao(), 0u);
    allocation.reset();
    EXPECT_EQ(state.live, 0u);
    state.fixture_released = other_state.fixture_released = true;
}

TEST(FarWorldRenderer, DrawOriginSubtractsBeforeNarrowingAndIncludesSpanMinimum) {
    struct Allocation final : FarVolumeGpuAllocation {
        std::uint64_t bytes() const noexcept override {
            return 96;
        }
    } allocation;
    FarVolumeDrawRecord record;
    record.request.span = {-4096, -2048};
    record.origin_meters = {8'000'000, -4096, -8'000'000};
    record.resource = &allocation;
    record.horizontal_outer_clip_meters = 16384;
    record.view_ray_bias_meters = 32;
    FarWorldDrawCommand command;
    ASSERT_TRUE(
        MakeFarWorldDrawCommand(record, {7'999'999.875, -4095.75, -7'999'999.625}, 3, command));
    EXPECT_FLOAT_EQ(command.relative_origin[0], .125f);
    EXPECT_FLOAT_EQ(command.relative_origin[1], -.25f);
    EXPECT_FLOAT_EQ(command.relative_origin[2], -.375f);
    const auto previous = command;
    EXPECT_FALSE(MakeFarWorldDrawCommand(record, {0, 0, 0}, -1, command));
    EXPECT_EQ(command.relative_origin, previous.relative_origin);
    EXPECT_FALSE(MakeFarWorldDrawCommand(
        record, {std::numeric_limits<double>::quiet_NaN(), 0, 0}, 3, command));
    record.origin_meters.y = 0;
    EXPECT_FALSE(MakeFarWorldDrawCommand(record, {0, 0, 0}, 3, command));
}

TEST(FarWorldRenderer, DrawOwnershipKeyAndArrivalSpansNeverDependOnCamera) {
    struct Allocation final : FarVolumeGpuAllocation {
        std::uint64_t bytes() const noexcept override {
            return 96;
        }
    } allocation;
    for (const std::int64_t translation : {0, 4194304, -4194304}) {
        FarVolumeDrawRecord record;
        record.identity.key.tier = 2;
        record.request.span = {translation - 256, translation + 256};
        record.origin_meters = {translation - 1024, translation - 256, translation - 1024};
        record.resource = &allocation;
        record.arrival.eligible = true;
        record.arrival.children.fill({translation, translation + 256});
        const auto shift = static_cast<std::int32_t>(translation / 16);
        for (const float camera_y : {83.32f, -67.13f}) {
            FarWorldDrawCommand command;
            ASSERT_TRUE(MakeFarWorldDrawCommand(record,
                                                {static_cast<double>(translation) + 37.1f,
                                                 static_cast<double>(translation) + camera_y,
                                                 static_cast<double>(translation) + 219.77f},
                                                3,
                                                command));
            EXPECT_EQ(command.origin_chunk,
                      (std::array<std::int32_t, 3>{shift - 64, shift - 16, shift - 64}));
            EXPECT_EQ(command.origin_remainder, (std::array<float, 3>{0, 0, 0}));
            for (const auto& span : command.child_local_y_spans)
                EXPECT_EQ(span, (std::array<float, 2>{256, 512}));
        }
    }
}

TEST(FarWorldRenderer, IncompleteAndWrongFrameOwnershipRefuseFarSubmission) {
    Owner owner;
    ASSERT_TRUE(owner.WaitSettled(Request()));
    owner.renderer.BeginLiveOwnership(owner.frame, 1);
    ASSERT_TRUE(owner.renderer.AddLiveUpload({{0, 0, 0, 0}, 1, 1, 3, true}));
    EXPECT_FALSE(owner.renderer.AddLiveUpload({{1, 0, 0, 0}, 1, 1, 3, true}));
    EXPECT_FALSE(owner.renderer.FinishLiveOwnership());
    std::size_t prepares = 0;
    const auto prepare = [&](const auto&) {
        ++prepares;
        return true;
    };
    EXPECT_EQ(owner.renderer.Submit({0, 0, 0}, prepare).refused, 1u);
    owner.renderer.BeginLiveOwnership(owner.frame - 1);
    ASSERT_TRUE(owner.renderer.FinishLiveOwnership());
    EXPECT_EQ(owner.renderer.Submit({0, 0, 0}, prepare).refused, 1u);
    EXPECT_EQ(prepares, 0u);
    EXPECT_EQ(owner.state.draws, 0u);
    owner.renderer.BeginLiveOwnership(owner.frame);
    ASSERT_TRUE(owner.renderer.FinishLiveOwnership());
    EXPECT_EQ(owner.renderer.Submit({0, 0, 0}, prepare).draws, 1u);
}
