#include "../fixtures/TerrainFixtureParams.h"
#include "../support/UniqueTempPath.h"
#include "core/JobSystem.h"
#include "luminumbra_client/rendering/Camera.h"
#include "luminumbra_client/rendering/FarVolumeGpuBackend.h"
#include "luminumbra_client/rendering/FarVolumeHandoff.h"
#include "luminumbra_client/rendering/FarVolumeRenderMesh.h"
#include "luminumbra_client/rendering/FarVolumeResidency.h"
#include "luminumbra_client/rendering/FarWorldDiagnostics.h"
#include "luminumbra_client/rendering/FarWorldHandoffRenderer.h"
#include "luminumbra_client/rendering/FarWorldOwnership.h"
#include "luminumbra_client/rendering/FarWorldRenderer.h"
#include "luminumbra_client/rendering/FarWorldSelection.h"
#include "luminumbra_client/rendering/FarWorldWaterMesh.h"
#include "luminumbra_common/core/SystemConfig.h"
#include "world/FarEditedBake.h"
#include "world/FarFrozenObservation.h"
#include "world/FarHorizon.h"
#include "world/FarTierTable.h"
#include "world/FarVolume.h"
#include "world/FarVolumeMemory.h"
#include "world/FarVolumeRuntime.h"
#include "world/FarVolumeTile.h"
#include "world/FarWantedSet.h"
#include "world/WorldStreamingState.h"

#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <glm/matrix.hpp>
#include <gtest/gtest.h>
#include <latch>
#include <limits>
#include <memory>
#include <memory_resource>
#include <nlohmann/json.hpp>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <system_error>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>

namespace {
using namespace Luminumbra;
using namespace Luminumbra::World;
using namespace Luminumbra::Rendering;

struct BuildGate {
    std::latch entered{1}, release{1}, exited{1};
    std::atomic<bool> first_sample{true};
};

struct DriverState {
    bool current = true, fail_upload = false, fail_draw = false;
    std::size_t draws = 0, deletes = 0, live = 0;
    bool ownership_bound = false;
    bool destroyed = false, released_with_context = true, fixture_released = false;
    std::size_t uploads = 0, held_build_drains = 0;
    BuildGate* release_on_drain = nullptr;
    std::weak_ptr<const FarFrozenObservation> session_image;
    bool session_image_alive_during_drain = false;
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
        if (name == 0 && state_.release_on_drain) {
            // The owner has entered policy-switch Drain() while the worker is
            // still held. Release it here so Drain can join it without a timer
            // or a helper thread calling owner-only renderer APIs.
            auto* gate = std::exchange(state_.release_on_drain, nullptr);
            EXPECT_TRUE(gate->entered.try_wait());
            EXPECT_FALSE(gate->exited.try_wait());
            state_.session_image_alive_during_drain = !state_.session_image.expired();
            ++state_.held_build_drains;
            gate->release.count_down();
        }
    }
    void BindBuffer(FarVolumeBufferKind kind, std::uint32_t name) noexcept override {
        if (kind == FarVolumeBufferKind::Vertex)
            vbo_ = name;
        else
            ebo_ = name;
    }
    void
    UploadBuffer(FarVolumeBufferKind kind, std::intptr_t bytes, const void*) noexcept override {
        ++state_.uploads;
        sizes_[kind == FarVolumeBufferKind::Vertex ? vbo_ : ebo_] = bytes;
        error_ = state_.fail_upload;
    }
    void GetBufferBytes(FarVolumeBufferKind kind, std::int64_t& bytes) noexcept override {
        bytes = sizes_[kind == FarVolumeBufferKind::Vertex ? vbo_ : ebo_];
    }
    void SetAttribute(FarVolumeVertexAttribute, std::int32_t, std::size_t) noexcept override {}
    void EnableAttribute(FarVolumeVertexAttribute) noexcept override {}
    bool BindLiveOwnership(std::span<const FarWorldLiveCell>) noexcept override {
        EXPECT_FALSE(state_.ownership_bound);
        state_.ownership_bound = true;
        return true;
    }
    void UnbindLiveOwnership() noexcept override {
        state_.ownership_bound = false;
    }
    void ReleaseLiveOwnership() noexcept override {
        UnbindLiveOwnership();
    }
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
        EXPECT_FALSE(state_.ownership_bound);
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
    explicit Fixture(DriverState& state,
                     FarCaveMode caves = FarCaveMode::BandLimited,
                     std::uint64_t identity = 42,
                     std::shared_ptr<BuildGate> gate = {},
                     FarWorldWaterCapability water = FarWorldWaterCapability::ExplicitlyDry,
                     float level = 0)
        : state_(state)
        , caves_(caves)
        , identity_(identity)
        , gate_(std::move(gate))
        , water_(water)
        , level_(level) {
        state_.fixture_released = false;
    }
    ~Fixture() override {
        EXPECT_EQ(state_.live, 0u);
        EXPECT_FALSE(state_.destroyed);
        if (gate_ && !gate_->first_sample.load()) {
            EXPECT_TRUE(gate_->exited.try_wait());
        }
        state_.fixture_released = true;
    }
    std::uint64_t content_identity() const noexcept override {
        return identity_;
    }
    FarWorldWaterCapability WaterCapability() const noexcept override {
        return water_;
    }
    std::optional<FarWorldWaterBound>
    WaterBound(const Luminumbra::World::FarTileKey&) const override {
        if (water_ == FarWorldWaterCapability::Unavailable)
            return std::nullopt;
        return FarWorldWaterBound{water_ == FarWorldWaterCapability::ExplicitlyDry, level_, level_};
    }
    std::optional<FarWorldWaterSample> WaterSample(std::int64_t, std::int64_t) const override {
        if (water_ == FarWorldWaterCapability::Unavailable)
            return std::nullopt;
        return FarWorldWaterSample{water_ == FarWorldWaterCapability::Wet, level_};
    }
    std::uint64_t owned_bytes() const noexcept override {
        return sizeof(*this);
    }
    FarVolumeNumericProfile numeric() const noexcept override {
        return FarVolumeNumericProfile::RawFloat;
    }
    FarCaveMode caves() const noexcept override {
        return caves_;
    }
    std::optional<float> Height(std::int64_t, std::int64_t) const override {
        return 0.0f;
    }
    std::optional<FarVolumeSample>
    Sample(const FarVolumePosition& p, float, std::uint32_t, FarCaveMode caves) const override {
        if (gate_ && gate_->first_sample.exchange(false)) {
            gate_->entered.count_down();
            gate_->release.wait();
            gate_->exited.count_down();
        }
        if (caves != caves_)
            return std::nullopt;
        const bool column = p.x >= 0 && p.x <= 256 && p.z >= 0 && p.z <= 256;
        return FarVolumeSample{column ? static_cast<float>(p.y) + .25f : 3.0f, 7};
    }

private:
    DriverState& state_;
    const FarCaveMode caves_;
    const std::uint64_t identity_;
    const std::shared_ptr<BuildGate> gate_;
    const FarWorldWaterCapability water_;
    const float level_;
};

FarVolumeWantedRequest Request(FarTileKey key = {5, 0, 0}) {
    FarVolumeTileKey canonical;
    EXPECT_TRUE(ToFarVolumeTileKey(key, canonical));
    return {{1, 42, 0, key, 1, 1}, {canonical, {}}, 0};
}
class Owner {
public:
    explicit Owner(const FarWorldExperimentalLimits& limits = FarWorldDevelopmentLimits(),
                   FarWorldWaterCapability water = FarWorldWaterCapability::ExplicitlyDry,
                   float level = 0) {
        jobs.startup(2);
        renderer.InstallBackend(std::make_unique<Driver>(state));
        EXPECT_EQ(
            renderer.BindFixture(jobs,
                                 std::make_shared<const Fixture>(
                                     state, FarCaveMode::BandLimited, 42, nullptr, water, level),
                                 1,
                                 1,
                                 limits),
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
    DriverState& State() noexcept {
        return state;
    }
    JobSystem& Jobs() noexcept {
        return jobs;
    }
    FarWorldRenderer& Renderer() noexcept {
        return renderer;
    }
    std::uint64_t& Frame() noexcept {
        return frame;
    }

private:
    DriverState state;
    JobSystem jobs;
    FarWorldRenderer renderer{FarWorldMode::Ladder};
    std::uint64_t frame = 0;
};

void ExpectManifestRefusedTransactionally(Owner& owner,
                                          std::span<const FarVolumeWantedRequest> requests,
                                          FarWorldRefusal expected) {
    // Finish prior worker activity so charge comparisons cannot race a build.
    // The refused manifest must neither dispatch work nor mutate residency.
    owner.Jobs().shutdown();
    const auto before = owner.Renderer().Residency()->Snapshot();
    const auto frame = owner.Renderer().LastFrame();
    const auto live = owner.State().live;
    const auto deletes = owner.State().deletes;
    EXPECT_FALSE(owner.Renderer().AdvanceFrame(++owner.Frame(), {0, 0, 0}, requests));
    const auto selection = owner.Renderer().Selection(FarWorldPurpose::World);
    EXPECT_EQ(selection.refusal, expected);
    EXPECT_EQ(selection.effective, FarWorldMode::Legacy);
    const auto after = owner.Renderer().Residency()->Snapshot();
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
    EXPECT_EQ(owner.Renderer().LastFrame().dispatched, frame.dispatched);
    EXPECT_EQ(owner.Renderer().LastFrame().uploaded, frame.uploaded);
    EXPECT_EQ(owner.Renderer().LastFrame().upload_bytes, frame.upload_bytes);
    EXPECT_EQ(owner.State().live, live);
    EXPECT_EQ(owner.State().deletes, deletes);
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
              FarWorldMode::Ladder);
    EXPECT_EQ(RequestedFarWorldMode(disabled.enabled(SysKey::RenderFarWorldLadder)),
              FarWorldMode::Legacy);
    EXPECT_EQ(RequestedFarWorldMode(enabled.enabled(SysKey::RenderFarWorldLadder)),
              FarWorldMode::Ladder);
    EXPECT_EQ(enabled.ComputeConfigSubHash(), defaults.ComputeConfigSubHash());
    EXPECT_EQ(disabled.ComputeConfigSubHash(), defaults.ComputeConfigSubHash());
    auto user = defaults;
    user.OverlayUserFromJsonString(R"({"render":{"far_world_ladder":{"enabled":false}}})");
    EXPECT_TRUE(user.enabled(SysKey::RenderFarWorldLadder));
    user = disabled;
    user.OverlayUserFromJsonString(R"({"render":{"far_world_ladder":{"enabled":true}}})");
    EXPECT_FALSE(user.enabled(SysKey::RenderFarWorldLadder));
}

TEST(FarWorldRenderer, DefaultExplicitRefusedAndPreviewSelection) {
    // Two owners with fixed diagnostic/ownership storage exceed the Windows stack budget.
    const auto legacy = std::make_unique<const FarWorldRenderer>(FarWorldMode::Legacy);
    EXPECT_EQ(legacy->Selection(FarWorldPurpose::World).requested, FarWorldMode::Legacy);
    EXPECT_EQ(legacy->Selection(FarWorldPurpose::World).effective, FarWorldMode::Legacy);
    EXPECT_EQ(legacy->Selection(FarWorldPurpose::World).refusal, FarWorldRefusal::None);
    const auto refused = std::make_unique<const FarWorldRenderer>();
    const auto normal = refused->Selection(FarWorldPurpose::World);
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
    const auto offscreen = refused->Selection(FarWorldPurpose::OffscreenPreview);
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
                                      FarWorldPurpose::World,
                                      FarWorldRefusal::ImmutableAuthorityUnavailable))
                  .draws,
              1u);
    EXPECT_EQ(dispatch(SelectFarWorld(FarWorldMode::Ladder,
                                      FarWorldPurpose::OffscreenPreview,
                                      FarWorldRefusal::None))
                  .draws,
              0u);
    EXPECT_EQ(legacy_schedules, 3u);
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
    ASSERT_TRUE(owner.Renderer().AdvanceFrame(10, {0, 0, 0}, wanted));
    const auto first = owner.Renderer().LastFrame();
    EXPECT_EQ(first.dispatched, 1u);
    const auto coarse = owner.Renderer().Residency()->Observe(wanted[1].identity.key);
    ASSERT_TRUE(coarse) << "Missing coarse tile observation";
    if (coarse) {
        EXPECT_EQ(coarse->eligibility, FarVolumeEligibility::Building);
    }
    const auto fine = owner.Renderer().Residency()->Observe(wanted[0].identity.key);
    ASSERT_TRUE(fine) << "Missing fine tile observation";
    if (fine) {
        EXPECT_EQ(fine->eligibility, FarVolumeEligibility::Wanted);
    }
    EXPECT_FALSE(owner.Renderer().AdvanceFrame(10, {0, 0, 0}, wanted));
    EXPECT_FALSE(owner.Renderer().AdvanceFrame(9, {0, 0, 0}, wanted));
    EXPECT_EQ(owner.Renderer().LastFrame().dispatched, first.dispatched);
    EXPECT_TRUE(owner.Renderer().AdvanceFrame(11, {0, 0, 0}, wanted));
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
        const auto observed = owner.Renderer().Residency()->Observe(valid.identity.key);
        ASSERT_TRUE(observed);
        if (observed) {
            EXPECT_EQ(observed->desired, valid.identity);
        }
        EXPECT_EQ(owner.Renderer().Selection(FarWorldPurpose::World).refusal,
                  FarWorldRefusal::InvalidRequests);
    }
}

TEST(FarWorldRenderer, UploadedDrawMetadataAndSynchronousBorrowing) {
    Owner owner;
    const auto request = Request();
    ASSERT_TRUE(owner.WaitSettled(request));
    owner.Renderer().BeginLiveOwnership(owner.Frame());
    ASSERT_TRUE(owner.Renderer().FinishLiveOwnership());
    const auto stats = owner.Renderer().Submit({.5, -128.25, -.25}, [&](const auto& command) {
        EXPECT_FLOAT_EQ(command.relative_origin[0], -.5f);
        EXPECT_FLOAT_EQ(command.relative_origin[1], -127.75f);
        EXPECT_FLOAT_EQ(command.relative_origin[2], .25f);
        EXPECT_EQ(command.horizontal_outer_clip_meters, 16384);
        EXPECT_EQ(command.view_ray_bias_meters, 32);
        EXPECT_GT(command.index_count, 0);
        EXPECT_THROW(owner.Renderer().PrepareWorldSwap(), std::logic_error);
        return true;
    });
    EXPECT_EQ(stats.draws, 1u);
    EXPECT_EQ(stats.refused, 0u);
    EXPECT_EQ(owner.State().draws, 1u);
}

TEST(FarWorldRenderer, SaveFencePreservesBindingOwnershipAndDraws) {
    Owner owner;
    ASSERT_TRUE(owner.WaitSettled(Request()));
    auto& renderer = owner.Renderer();
    renderer.BeginLiveOwnership(owner.Frame());
    ASSERT_TRUE(renderer.FinishLiveOwnership());
    const auto* residency = renderer.Residency();
    const auto epoch = renderer.world_epoch();
    const auto live = owner.State().live;
    const auto deletes = owner.State().deletes;
    owner.State().ownership_bound = true;
    renderer.WaitForAcceptedCacheWrites();
    EXPECT_TRUE(owner.State().ownership_bound);
    EXPECT_EQ(renderer.Residency(), residency);
    EXPECT_EQ(renderer.world_epoch(), epoch);
    EXPECT_EQ(owner.State().live, live);
    EXPECT_EQ(owner.State().deletes, deletes);
    owner.State().ownership_bound = false;
    EXPECT_EQ(renderer
                  .Submit({0, 0, 0},
                          [&](const auto&) {
                              EXPECT_THROW(renderer.WaitForAcceptedCacheWrites(), std::logic_error);
                              return true;
                          })
                  .draws,
              1u);
    std::thread foreign(
        [&] { EXPECT_THROW(renderer.WaitForAcceptedCacheWrites(), std::logic_error); });
    foreign.join();
    renderer.PrepareWorldSwap();
    EXPECT_NO_THROW(renderer.WaitForAcceptedCacheWrites()); // no binding or accepted writes
    EXPECT_TRUE(owner.State().fixture_released);
}

TEST(FarWorldRenderer, WorldSwapDrainsBeforeAuthorityMemoryDriverAndContextRelease) {
    Owner owner;
    ASSERT_TRUE(owner.WaitSettled(Request()));
    ASSERT_GT(owner.State().live, 0u);
    // Preview/offscreen selection retains the Ladder owner's resources. A swap
    // must drain them even though the current effective renderer is Legacy.
    const auto preview = owner.Renderer().Selection(FarWorldPurpose::Preview);
    EXPECT_EQ(preview.effective, FarWorldMode::Legacy);
    EXPECT_EQ(owner.Renderer().Selection(FarWorldPurpose::OffscreenPreview).effective,
              FarWorldMode::Legacy);
    EXPECT_GT(owner.State().live, 0u);
    owner.State().ownership_bound = true; // Drain must unbind before GPU destruction.
    owner.Renderer().PrepareWorldSwap();
    EXPECT_EQ(owner.State().live, 0u);
    EXPECT_TRUE(owner.State().fixture_released);
    EXPECT_TRUE(owner.State().released_with_context);
    EXPECT_FALSE(owner.State().destroyed);
    EXPECT_EQ(owner.Renderer().world_epoch(), 0u);
    EXPECT_EQ(owner.Renderer().Selection(FarWorldPurpose::World).effective, FarWorldMode::Legacy);
    owner.Renderer().Shutdown();
    EXPECT_TRUE(owner.State().destroyed);
    owner.State().current = false; // Client may now destroy the context.
}

TEST(FarWorldRenderer, SwapAndShutdownJoinHeldWorkerBeforeFixtureAndDriverRelease) {
    for (const bool shutdown : {false, true}) {
        const auto gate = std::make_shared<BuildGate>();
        Owner owner;
        owner.Renderer().PrepareWorldSwap();
        ASSERT_EQ(
            owner.Renderer().BindFixture(
                owner.Jobs(),
                std::make_shared<const Fixture>(owner.State(), FarCaveMode::BandLimited, 42, gate),
                1,
                1,
                FarWorldDevelopmentLimits()),
            FarWorldRefusal::None);
        owner.State().release_on_drain = gate.get();
        ASSERT_TRUE(owner.Advance(Request()));
        ASSERT_EQ(owner.Renderer().LastFrame().dispatched, 1u);
        gate->entered.wait();
        EXPECT_FALSE(gate->exited.try_wait());
        if (shutdown)
            owner.Renderer().Shutdown();
        else
            owner.Renderer().PrepareWorldSwap();
        EXPECT_TRUE(gate->exited.try_wait());
        EXPECT_EQ(owner.State().held_build_drains, 1u);
        EXPECT_TRUE(owner.State().fixture_released);
        EXPECT_EQ(owner.State().destroyed, shutdown);
        EXPECT_EQ(owner.State().uploads, 0u);
        EXPECT_EQ(owner.Renderer().Residency(), nullptr);
        EXPECT_EQ(owner.Renderer().Selection(FarWorldPurpose::World).effective,
                  FarWorldMode::Legacy);
        EXPECT_FALSE(owner.Advance(Request()));
        owner.Renderer().Shutdown();
        owner.State().current = false;
    }
}

TEST(FarWorldRenderer, MatchingImmutableCaveBindingPublishesRequestedBoundAndEffectivePolicy) {
    Owner owner;
    for (const auto caves : {FarCaveMode::BandLimited, FarCaveMode::BoxFiltered}) {
        owner.Renderer().SetRequestedCavePolicy(caves);
        // Drain even when rebinding the same policy: this is new immutable content.
        owner.Renderer().PrepareWorldSwap();
        const auto identity = caves == FarCaveMode::BandLimited ? 42u : 43u;
        ASSERT_EQ(owner.Renderer().BindFixture(
                      owner.Jobs(),
                      std::make_shared<const Fixture>(owner.State(), caves, identity),
                      1,
                      1,
                      FarWorldDevelopmentLimits()),
                  FarWorldRefusal::None);
        const auto selected = owner.Renderer().Selection(FarWorldPurpose::World);
        ASSERT_EQ(selected.effective, FarWorldMode::Ladder);
        const auto policy = owner.Renderer().CavePolicy(selected);
        EXPECT_EQ(policy.requested, caves);
        EXPECT_EQ(policy.bound, caves);
        EXPECT_EQ(policy.effective, caves);
        auto request = Request();
        request.identity.content_hash = identity;
        request.identity.cave_policy = static_cast<std::uint8_t>(caves);
        ASSERT_TRUE(owner.WaitSettled(request));
        const auto observed = owner.Renderer().Residency()->Observe(request.identity.key);
        ASSERT_TRUE(observed);
        if (observed) {
            EXPECT_EQ(observed->desired, request.identity);
        }
        for (const auto purpose : {FarWorldPurpose::Preview, FarWorldPurpose::OffscreenPreview}) {
            const auto preview_policy = owner.Renderer().CavePolicy(purpose);
            EXPECT_EQ(preview_policy.requested, caves);
            EXPECT_EQ(preview_policy.bound, caves);
            EXPECT_FALSE(preview_policy.effective);
        }
        auto pass_refused = selected;
        pass_refused.effective = FarWorldMode::Legacy;
        pass_refused.refusal = FarWorldRefusal::ShaderProgramUnavailable;
        EXPECT_FALSE(owner.Renderer().CavePolicy(pass_refused).effective);
    }
}

void ExpectPairedCaveDiagnosticPreserved(FarWorldRenderer& renderer) {
    const auto on = renderer.Diagnostic();
    const auto on_json = FarWorldDiagnosticJson(on);
    const auto off = FarWorldSubmissionOverride(on.selection, false);
    bool advanced = false, submitted = false;
    AdvanceSelectedFarWorld(off, [&] { advanced = true; }, [&] { advanced = true; });
    const auto result = SubmitSelectedFarWorld(
        off,
        [&] {
            submitted = true;
            return FarWorldSubmitStats{};
        },
        [&](const auto&) {
            submitted = true;
            return FarWorldSubmitStats{};
        },
        [](const auto&) { return true; });
    EXPECT_FALSE(advanced);
    EXPECT_FALSE(submitted);
    EXPECT_EQ(result.draws, 0u);
    EXPECT_EQ(renderer.Diagnostic().requested_cave_policy, on.requested_cave_policy);
    EXPECT_EQ(renderer.Diagnostic().bound_cave_policy, on.bound_cave_policy);
    EXPECT_EQ(FarWorldDiagnosticJson(renderer.Diagnostic()), on_json);
}

TEST(FarWorldRenderer, DiagnosticRecordsRequestedBoxFilteredWithoutFixture) {
    FarWorldRenderer renderer{FarWorldMode::Ladder};
    renderer.SetRequestedCavePolicy(FarCaveMode::BoxFiltered);
    const auto selection = renderer.Selection(FarWorldPurpose::World);
    renderer.CaptureDiagnostic(selection, 1, {0, 0, 0}, kLegacyFarPlaneMeters, false, 0, 0);
    const auto snapshot = renderer.Diagnostic();
    EXPECT_EQ(snapshot.requested_cave_policy, FarCaveMode::BoxFiltered);
    EXPECT_FALSE(snapshot.bound_cave_policy);
    const auto json = FarWorldDiagnosticJson(snapshot);
    EXPECT_EQ(json["requested_cave_policy"], 1);
    EXPECT_TRUE(json["bound_cave_policy"].is_null());
    ExpectPairedCaveDiagnosticPreserved(renderer);
    renderer.SetRequestedCavePolicy(std::nullopt);
    EXPECT_EQ(snapshot.requested_cave_policy, FarCaveMode::BoxFiltered);
    renderer.CaptureDiagnostic(renderer.Selection(FarWorldPurpose::World),
                               2,
                               {0, 0, 0},
                               kLegacyFarPlaneMeters,
                               false,
                               0,
                               0);
    EXPECT_TRUE(FarWorldDiagnosticJson(renderer.Diagnostic())["requested_cave_policy"].is_null());
    EXPECT_TRUE(FarWorldDiagnosticJson(renderer.Diagnostic())["bound_cave_policy"].is_null());
}

TEST(FarWorldRenderer, LegacyDiagnosticPreservesDistinctRequestedAndBoundCavePolicies) {
    Owner owner;
    owner.Renderer().SetRequested(FarWorldMode::Legacy);
    owner.Renderer().SetRequestedCavePolicy(FarCaveMode::BoxFiltered);
    ASSERT_EQ(owner.Renderer().BindFixture(owner.Jobs(),
                                           std::make_shared<const Fixture>(owner.State()),
                                           1,
                                           1,
                                           FarWorldDevelopmentLimits()),
              FarWorldRefusal::None);
    const auto selection = owner.Renderer().Selection(FarWorldPurpose::World);
    EXPECT_EQ(selection.effective, FarWorldMode::Legacy);
    owner.Renderer().CaptureDiagnostic(selection, 1, {0, 0, 0}, kLegacyFarPlaneMeters, false, 0, 0);
    const auto snapshot = owner.Renderer().Diagnostic();
    EXPECT_EQ(snapshot.requested_cave_policy, FarCaveMode::BoxFiltered);
    EXPECT_EQ(snapshot.bound_cave_policy, FarCaveMode::BandLimited);
    const auto json = FarWorldDiagnosticJson(snapshot);
    EXPECT_EQ(json["requested_cave_policy"], 1);
    EXPECT_EQ(json["bound_cave_policy"], 0);
    ExpectPairedCaveDiagnosticPreserved(owner.Renderer());
}

TEST(FarWorldRenderer, MismatchedImmutableCaveBindingRefusesWithoutRelabeling) {
    for (const auto requested : {FarCaveMode::BandLimited, FarCaveMode::BoxFiltered}) {
        Owner owner;
        owner.Renderer().SetRequestedCavePolicy(requested);
        const auto other = requested == FarCaveMode::BandLimited ? FarCaveMode::BoxFiltered
                                                                 : FarCaveMode::BandLimited;
        ASSERT_EQ(
            owner.Renderer().BindFixture(owner.Jobs(),
                                         std::make_shared<const Fixture>(owner.State(), other, 43),
                                         1,
                                         1,
                                         FarWorldDevelopmentLimits()),
            FarWorldRefusal::CavePolicyMismatch);
        const auto selected = owner.Renderer().Selection(FarWorldPurpose::World);
        EXPECT_EQ(selected.requested, FarWorldMode::Ladder);
        EXPECT_EQ(selected.effective, FarWorldMode::Legacy);
        EXPECT_EQ(selected.refusal, FarWorldRefusal::CavePolicyMismatch);
        EXPECT_STREQ(FarWorldRefusalName(selected.refusal), "CavePolicyMismatch");
        EXPECT_EQ(owner.Renderer().CavePolicy(selected).requested, requested);
        EXPECT_FALSE(owner.Renderer().CavePolicy(selected).bound);
        EXPECT_FALSE(owner.Renderer().CavePolicy(selected).effective);
        EXPECT_EQ(owner.Renderer().Residency(), nullptr);
        EXPECT_EQ(owner.Renderer().world_epoch(), 0u);
        EXPECT_EQ(owner.State().live, 0u);
        owner.Renderer().CaptureDiagnostic(
            selected, 1, {0, 0, 0}, kLegacyFarPlaneMeters, false, 0, 0);
        EXPECT_EQ(owner.Renderer().Diagnostic().requested_cave_policy, requested);
        EXPECT_FALSE(owner.Renderer().Diagnostic().bound_cave_policy);
        const auto json = FarWorldDiagnosticJson(owner.Renderer().Diagnostic());
        EXPECT_EQ(json["requested_cave_policy"], static_cast<int>(requested));
        EXPECT_TRUE(json["bound_cave_policy"].is_null());
        ExpectPairedCaveDiagnosticPreserved(owner.Renderer());
    }
}

TEST(FarWorldRenderer, ChangedCavePolicyDrainsUploadsAndRequiresNewImmutableBinding) {
    Owner owner;
    const auto old = Request();
    ASSERT_TRUE(owner.WaitSettled(old));
    ASSERT_GT(owner.State().live, 0u);
    owner.Renderer().SetRequestedCavePolicy(FarCaveMode::BoxFiltered);
    EXPECT_EQ(owner.State().live, 0u);
    EXPECT_TRUE(owner.State().fixture_released);
    EXPECT_TRUE(owner.State().released_with_context);
    EXPECT_FALSE(owner.State().destroyed);
    EXPECT_EQ(owner.Renderer().Residency(), nullptr);
    EXPECT_EQ(owner.Renderer().world_epoch(), 0u);
    EXPECT_FALSE(owner.Advance(old));
    EXPECT_EQ(owner.Renderer().Selection(FarWorldPurpose::World).refusal,
              FarWorldRefusal::ImmutableAuthorityUnavailable);
    EXPECT_FALSE(owner.Renderer().CavePolicy(FarWorldPurpose::World).bound);

    ASSERT_EQ(owner.Renderer().BindFixture(
                  owner.Jobs(),
                  std::make_shared<const Fixture>(owner.State(), FarCaveMode::BoxFiltered, 43),
                  1,
                  1,
                  FarWorldDevelopmentLimits()),
              FarWorldRefusal::None);
    auto current = old;
    current.identity.content_hash = 43;
    current.identity.cave_policy = static_cast<std::uint8_t>(FarCaveMode::BoxFiltered);
    EXPECT_NE(old.identity, current.identity);
    ASSERT_TRUE(owner.WaitSettled(current));
    const auto before = owner.Renderer().Residency()->Observe(current.identity.key);
    ASSERT_TRUE(before);
    if (before) {
        EXPECT_EQ(before->desired, current.identity);
    }
    ExpectManifestRefusedTransactionally(
        owner, std::span(&old, 1), FarWorldRefusal::InvalidRequests);
    const auto after = owner.Renderer().Residency()->Observe(current.identity.key);
    ASSERT_TRUE(after);
    if (after) {
        EXPECT_EQ(after->desired, current.identity);
    }
}

TEST(FarWorldRenderer, InFlightCaveSwitchDrainsStaleBuildAndAcceptsNewPolicySubmission) {
    const auto gate = std::make_shared<BuildGate>();
    Owner owner;
    owner.Renderer().PrepareWorldSwap();
    ASSERT_EQ(owner.Renderer().BindFixture(owner.Jobs(),
                                           std::make_shared<const Fixture>(
                                               owner.State(), FarCaveMode::BandLimited, 42, gate),
                                           1,
                                           1,
                                           FarWorldDevelopmentLimits()),
              FarWorldRefusal::None);
    EXPECT_FALSE(owner.State().fixture_released);
    const auto old = Request();
    owner.State().release_on_drain = gate.get(); // Also releases on assertion-failure cleanup.
    ASSERT_TRUE(owner.Advance(old));
    ASSERT_EQ(owner.Renderer().LastFrame().dispatched, 1u);
    gate->entered.wait(); // Worker is inside the actual facade's density callback.
    EXPECT_FALSE(gate->exited.try_wait());
    const auto held = owner.Renderer().Residency()->Observe(old.identity.key);
    ASSERT_TRUE(held);
    if (held) {
        EXPECT_EQ(held->desired, old.identity);
        EXPECT_EQ(held->eligibility, FarVolumeEligibility::Building);
        EXPECT_FALSE(IsFarTileCovering(held->coverage));
    }

    // Drain's first driver unbind releases the held worker. This call must join
    // its stale result before dropping the immutable fixture and residency.
    owner.Renderer().SetRequestedCavePolicy(FarCaveMode::BoxFiltered);
    EXPECT_EQ(owner.State().held_build_drains, 1u);
    EXPECT_TRUE(gate->exited.try_wait());
    EXPECT_TRUE(owner.State().fixture_released);
    EXPECT_EQ(owner.State().uploads, 0u); // The old completion was never accepted for upload.
    EXPECT_EQ(owner.State().live, 0u);
    EXPECT_EQ(owner.Renderer().Residency(), nullptr);
    EXPECT_EQ(owner.Renderer().world_epoch(), 0u);
    EXPECT_EQ(owner.Renderer().Selection(FarWorldPurpose::World).refusal,
              FarWorldRefusal::ImmutableAuthorityUnavailable);
    EXPECT_FALSE(owner.Advance(old));

    ASSERT_EQ(owner.Renderer().BindFixture(
                  owner.Jobs(),
                  std::make_shared<const Fixture>(owner.State(), FarCaveMode::BoxFiltered, 43),
                  1,
                  1,
                  FarWorldDevelopmentLimits()),
              FarWorldRefusal::None);
    EXPECT_FALSE(owner.Renderer().Residency()->Observe(old.identity.key));
    for (const auto& tier : owner.Renderer().Residency()->Snapshot()) {
        EXPECT_EQ(tier.wanted, 0u);
        EXPECT_EQ(tier.covering, 0u);
        EXPECT_EQ(tier.uploaded_pairs, 0u);
        EXPECT_EQ(tier.empty_pairs, 0u);
    }
    auto current = old;
    current.identity.content_hash = 43;
    current.identity.cave_policy = static_cast<std::uint8_t>(FarCaveMode::BoxFiltered);
    EXPECT_NE(current.identity, old.identity);
    ASSERT_TRUE(owner.Advance(current)); // A fresh submission after the switch.
    ASSERT_EQ(owner.Renderer().LastFrame().dispatched, 1u);
    owner.Jobs().shutdown();             // Joins the new worker; no polling, sleep or deadline.
    ASSERT_TRUE(owner.Advance(current)); // Reaps and accepts only the new identity.
    EXPECT_EQ(owner.Renderer().LastFrame().uploaded, 1u);
    const auto accepted = owner.Renderer().Residency()->Observe(current.identity.key);
    ASSERT_TRUE(accepted);
    if (accepted) {
        EXPECT_EQ(accepted->desired, current.identity);
        EXPECT_NE(accepted->desired.cave_policy, old.identity.cave_policy);
        EXPECT_EQ(accepted->eligibility, FarVolumeEligibility::Settled);
        EXPECT_TRUE(IsFarTileCovering(accepted->coverage));
    }
    std::size_t covering = 0;
    for (const auto& tier : owner.Renderer().Residency()->Snapshot()) {
        covering += tier.covering;
        EXPECT_EQ(tier.building, 0u);
        EXPECT_EQ(tier.ready, 0u);
    }
    EXPECT_EQ(covering, 1u);
    EXPECT_EQ(owner.Renderer().CavePolicy(FarWorldPurpose::World).effective,
              FarCaveMode::BoxFiltered);
    owner.Renderer().BeginLiveOwnership(owner.Frame());
    ASSERT_TRUE(owner.Renderer().FinishLiveOwnership());
    EXPECT_EQ(owner.Renderer().Submit({0, 0, 0}, [](const auto&) { return true; }).draws, 1u);
    EXPECT_EQ(owner.State().draws, 1u);
    ExpectManifestRefusedTransactionally(
        owner, std::span(&old, 1), FarWorldRefusal::InvalidRequests);
}

TEST(FarWorldRenderer, SamePolicyPreservesBindingAndPressureOrCameraCannotSelectAnotherPolicy) {
    auto limits = FarWorldDevelopmentLimits();
    limits.frame.builds = limits.frame.uploads = 1;
    // Enough for bounded V1 control storage, less than the requested build headroom.
    limits.budgets.bytes[0] = 1024ULL * 1024;
    Owner owner(limits);
    auto pressured = Request({1, 0, 0});
    pressured.build_headroom_bytes = 2ULL * 1024 * 1024;
    ASSERT_TRUE(owner.Advance(pressured));
    EXPECT_GT(owner.Renderer().LastFrame().rejected, 0u);
    const auto observed = owner.Renderer().Residency()->Observe(pressured.identity.key);
    ASSERT_TRUE(observed);
    if (observed) {
        EXPECT_EQ(observed->eligibility, FarVolumeEligibility::RequestRejected);
    }
    const auto* residency = owner.Renderer().Residency();
    owner.Renderer().SetRequestedCavePolicy(FarCaveMode::BandLimited);
    EXPECT_EQ(owner.Renderer().Residency(), residency);
    for (const std::array<double, 3> camera :
         {std::array<double, 3>{0, -1024, 0}, std::array<double, 3>{16384, 1024, -16384}}) {
        ASSERT_TRUE(owner.Renderer().PrepareFrame(++owner.Frame(), camera));
        EXPECT_EQ(owner.Renderer().CavePolicy(FarWorldPurpose::World).requested,
                  FarCaveMode::BandLimited);
        EXPECT_EQ(owner.Renderer().CavePolicy(FarWorldPurpose::World).effective,
                  FarCaveMode::BandLimited);
    }
}

TEST(FarWorldRenderer, InvalidTypedPolicyDrainsAndRefusesUntilValidPolicyIsRebound) {
    Owner owner;
    ASSERT_TRUE(owner.WaitSettled(Request()));
    owner.Renderer().SetRequestedCavePolicy(std::bit_cast<FarCaveMode>(std::uint8_t{255}));
    EXPECT_EQ(owner.Renderer().Residency(), nullptr);
    EXPECT_EQ(owner.State().live, 0u);
    EXPECT_TRUE(owner.State().fixture_released);
    EXPECT_EQ(owner.Renderer().Selection(FarWorldPurpose::World).refusal,
              FarWorldRefusal::InvalidCavePolicy);
    EXPECT_STREQ(FarWorldRefusalName(FarWorldRefusal::InvalidCavePolicy), "InvalidCavePolicy");
    owner.Renderer().SetRequestedCavePolicy(FarCaveMode::BandLimited);
    EXPECT_EQ(owner.Renderer().Selection(FarWorldPurpose::World).refusal,
              FarWorldRefusal::ImmutableAuthorityUnavailable);
    ASSERT_EQ(owner.Renderer().BindFixture(owner.Jobs(),
                                           std::make_shared<const Fixture>(owner.State()),
                                           1,
                                           1,
                                           FarWorldDevelopmentLimits()),
              FarWorldRefusal::None);
    EXPECT_TRUE(owner.WaitSettled(Request()));
}

TEST(FarWorldRenderer, CaveRequestWithLegacyEffectiveLeavesSchedulingAndBindingInert) {
    Owner owner;
    owner.Renderer().SetRequested(FarWorldMode::Legacy);
    ASSERT_EQ(owner.Renderer().BindFixture(owner.Jobs(),
                                           std::make_shared<const Fixture>(owner.State()),
                                           1,
                                           1,
                                           FarWorldDevelopmentLimits()),
              FarWorldRefusal::None);
    const auto* residency = owner.Renderer().Residency();
    for (const auto requested :
         {std::optional(FarCaveMode::BoxFiltered), std::optional<FarCaveMode>{}}) {
        owner.Renderer().SetRequestedCavePolicy(requested);
        const auto selected = owner.Renderer().Selection(FarWorldPurpose::World);
        EXPECT_EQ(selected.effective, FarWorldMode::Legacy);
        EXPECT_EQ(selected.refusal, FarWorldRefusal::None);
        EXPECT_EQ(owner.Renderer().Residency(), residency);
        EXPECT_FALSE(owner.Advance(Request()));
        EXPECT_FALSE(owner.Renderer().CavePolicy(selected).effective);
        EXPECT_EQ(owner.Renderer().CavePolicy(selected).bound, FarCaveMode::BandLimited);
        EXPECT_EQ(owner.Renderer().LastFrame().dispatched, 0u);
        EXPECT_EQ(owner.State().live, 0u);
    }
    owner.Renderer().SetRequested(FarWorldMode::Ladder);
    EXPECT_EQ(owner.Renderer().Residency(), nullptr);
    EXPECT_EQ(owner.Renderer().Selection(FarWorldPurpose::World).refusal,
              FarWorldRefusal::InvalidCavePolicy);
}

TEST(FarWorldRenderer, FailedRebuildRetainsOldAllocationAndLogicalChargeButDoesNotDraw) {
    Owner owner;
    auto request = Request();
    ASSERT_TRUE(owner.WaitSettled(request));
    const auto* old = owner.Renderer().Residency()->Resource(request.identity.key);
    ASSERT_NE(old, nullptr);
    const auto bytes = old->bytes();
    const auto live = owner.State().live;
    request.intent.extra_span = FarVolumeCoverageSpan{-512, 512};
    ++request.identity.span_generation;
    owner.State().fail_upload = true;
    EXPECT_FALSE(owner.WaitSettled(request));
    EXPECT_EQ(owner.Renderer().Residency()->Resource(request.identity.key), old);
    const auto observed = owner.Renderer().Residency()->Observe(request.identity.key);
    ASSERT_TRUE(observed);
    if (observed) {
        EXPECT_EQ(observed->gpu_bytes, bytes);
        EXPECT_FALSE(IsFarTileCovering(observed->coverage));
    }
    EXPECT_EQ(owner.State().live, live);
    owner.Renderer().BeginLiveOwnership(owner.Frame());
    ASSERT_TRUE(owner.Renderer().FinishLiveOwnership());
    std::size_t prepares = 0;
    const auto submission = owner.Renderer().Submit({0, 0, 0}, [&](const auto&) {
        ++prepares;
        return true;
    });
    EXPECT_EQ(submission.draws, 0u);
    EXPECT_EQ(submission.refused, 0u); // complete mask; stale residency itself excludes the draw
    EXPECT_EQ(prepares, 0u);
    const auto attempts = owner.Renderer().LastFrame().upload_attempts;
    EXPECT_EQ(attempts, 1u);
    EXPECT_GT(owner.Renderer().LastFrame().upload_bytes, 0u);
    EXPECT_EQ(owner.Renderer().LastFrame().successful_upload_bytes, 0u);
    ASSERT_TRUE(owner.Advance(request));
    EXPECT_EQ(owner.Renderer().LastFrame().upload_attempts, 0u);
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
    Camera const camera;
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
    const auto observed = owner.Renderer().Residency()->Observe(Request().identity.key);
    ASSERT_TRUE(observed);
    if (observed) {
        EXPECT_EQ(observed->eligibility, FarVolumeEligibility::RequestRejected);
        EXPECT_EQ(observed->gpu_bytes, 0u);
    }
    EXPECT_EQ(owner.State().live, 0u);
    EXPECT_EQ(owner.State().deletes, 0u);
}

TEST(FarWorldRenderer, CameraXZWantedSetSpanGrowthAndRejectedExtensionAreTransactional) {
    Owner owner;
    std::vector<FarTileKey> wanted;
    ASSERT_EQ(ComputeFarWantedSet(-.5, -512.25, wanted), FarWantedError::None);
    ASSERT_TRUE(owner.Renderer().PrepareFrame(1, {-.5, -20, -512.25}));
    const auto first = owner.Renderer().Residency()->Snapshot();
    std::size_t count = 0;
    for (const auto& tier : first)
        count += tier.wanted;
    EXPECT_EQ(count, wanted.size());
    const auto before = owner.Renderer().Residency()->Observe(wanted.front());
    EXPECT_TRUE(before);
    ASSERT_TRUE(before);
    ASSERT_TRUE(owner.Renderer().PrepareFrame(2, {-.5, 100, -512.25}));
    const auto after = owner.Renderer().Residency()->Observe(wanted.front());
    ASSERT_TRUE(after);
    if (before && after) {
        EXPECT_GT(after->desired.span_generation, before->desired.span_generation);
    }
    ASSERT_TRUE(owner.Renderer().PrepareFrame(3, {-.5, 0, -512.25}));
    const auto unchanged = owner.Renderer().Residency()->Observe(wanted.front());
    ASSERT_TRUE(unchanged) << "Missing tile observation after unchanged span";
    if (unchanged && after) {
        EXPECT_EQ(unchanged->desired, after->desired);
    }
    const std::array invalid{FarVolumeCoverageSpan{-9000, 0}};
    EXPECT_FALSE(owner.Renderer().PrepareFrame(4, {-.5, 0, -512.25}, invalid));
    const auto rejected = owner.Renderer().Residency()->Observe(wanted.front());
    ASSERT_TRUE(rejected) << "Missing tile observation after rejected extension";
    if (rejected && after) {
        EXPECT_EQ(rejected->desired, after->desired);
    }
    EXPECT_EQ(owner.Renderer().Selection(FarWorldPurpose::World).refusal,
              FarWorldRefusal::SpanRejected);
}

TEST(FarWorldRenderer, LeaveAndReturnPreservesAcceptedExtensionsAndGeneration) {
    Owner owner;
    const FarTileKey key{1, -1, -2};
    const std::array extensions{FarVolumeCoverageSpan{-100, 1200}};
    ASSERT_TRUE(owner.Renderer().PrepareFrame(1, {-.5, 1000, -512.25}, extensions));
    const auto before = owner.Renderer().Residency()->Observe(key);
    ASSERT_TRUE(before);
    ASSERT_TRUE(owner.Renderer().PrepareFrame(2, {65536, 500, 65536}));
    EXPECT_FALSE(owner.Renderer().Residency()->Observe(key));
    ASSERT_TRUE(owner.Renderer().PrepareFrame(3, {-.5, 500, -512.25}));
    const auto returned = owner.Renderer().Residency()->Observe(key);
    ASSERT_TRUE(returned);
    if (!returned)
        return;
    if (before) {
        EXPECT_EQ(returned->desired, before->desired);
    }
    // Equal identity freezes the complete intent in ReplaceWanted. Accepting
    // this manifest proves that the returned request kept the prior coverage.
    auto expected = Request(key);
    expected.identity = returned->desired;
    expected.intent.extra_span = extensions.front();
    expected.build_headroom_bytes = FarWorldDevelopmentLimits().build_headroom_bytes;
    owner.Frame() = 3;
    EXPECT_TRUE(owner.Advance(expected));
}

TEST(FarWorldRenderer, LeaveAndReturnSpanGrowthKeepsGenerationMonotone) {
    Owner owner;
    const FarTileKey key{1, 0, 0};
    ASSERT_TRUE(owner.Renderer().PrepareFrame(1, {0, 1000, 0}));
    const auto before = owner.Renderer().Residency()->Observe(key);
    ASSERT_TRUE(before);
    ASSERT_TRUE(owner.Renderer().PrepareFrame(2, {65536, 0, 65536}));
    EXPECT_FALSE(owner.Renderer().Residency()->Observe(key));
    ASSERT_TRUE(owner.Renderer().PrepareFrame(3, {0, 0, 0}));
    const auto returned = owner.Renderer().Residency()->Observe(key);
    ASSERT_TRUE(returned);
    auto expected = Request(key);
    expected.build_headroom_bytes = FarWorldDevelopmentLimits().build_headroom_bytes;
    if (returned) {
        if (before) {
            EXPECT_GT(returned->desired.span_generation, before->desired.span_generation);
        }
        expected.identity = returned->desired;
        expected.intent.extra_span = FarVolumeCoverageSpan{0, 1000};
        owner.Frame() = 3;
        ASSERT_TRUE(owner.Advance(expected));
    }
    ASSERT_TRUE(owner.Renderer().PrepareFrame(5, {65536, 0, 65536}));
    EXPECT_FALSE(owner.Renderer().Residency()->Observe(key));
    ASSERT_TRUE(owner.Renderer().PrepareFrame(6, {0, -100, 0}));
    const auto grown = owner.Renderer().Residency()->Observe(key);
    ASSERT_TRUE(grown);
    if (!grown)
        return;
    if (returned) {
        EXPECT_GT(grown->desired.span_generation, returned->desired.span_generation);
    }
    expected.identity = grown->desired;
    expected.intent.extra_span = FarVolumeCoverageSpan{-100, 1000};
    owner.Frame() = 6;
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
            ASSERT_TRUE(owner.Renderer().PrepareFrame(station + 1, {coordinate, 0, coordinate}));
            tracked += wanted.size();
            last_key = wanted.front();
            const auto observed = owner.Renderer().Residency()->Observe(last_key);
            ASSERT_TRUE(observed);
            if (observed) {
                last_identity = observed->desired;
            }
            last_snapshot = owner.Renderer().Residency()->Snapshot();
            last_frame = owner.Renderer().LastFrame();
            continue;
        }
        ASSERT_GT(tracked, 0u);
        EXPECT_FALSE(owner.Renderer().PrepareFrame(station + 1, {coordinate, 0, coordinate}));
        const auto selection = owner.Renderer().Selection(FarWorldPurpose::World);
        EXPECT_EQ(selection.refusal, FarWorldRefusal::SpanHistoryExhausted);
        EXPECT_EQ(selection.effective, FarWorldMode::Legacy);
        const auto observed = owner.Renderer().Residency()->Observe(last_key);
        ASSERT_TRUE(observed);
        if (observed) {
            EXPECT_EQ(observed->desired, last_identity);
        }
        EXPECT_FALSE(owner.Renderer().Residency()->Observe(wanted.front()));
        const auto snapshot = owner.Renderer().Residency()->Snapshot();
        for (std::size_t tier = 0; tier < kFarTierCount; ++tier)
            EXPECT_EQ(snapshot[tier].wanted, last_snapshot[tier].wanted);
        EXPECT_EQ(owner.Renderer().LastFrame().dispatched, last_frame.dispatched);
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
    ASSERT_TRUE(owner.Renderer().AdvanceFrame(++owner.Frame(), {0, 0, 0}, {}));
    ASSERT_FALSE(owner.Renderer().Residency()->Observe(accepted.identity.key));
    auto stale = accepted;
    stale.identity.span_generation = 1;
    stale.intent.extra_span = FarVolumeCoverageSpan{0, 0};
    ExpectManifestRefusedTransactionally(
        owner, std::span(&stale, 1), FarWorldRefusal::InvalidRequests);
    EXPECT_FALSE(owner.Renderer().Residency()->Observe(accepted.identity.key));
}

TEST(FarWorldRenderer, AdvanceFrameLeaveAndReturnRejectsGenerationReuseWithChangedIntent) {
    Owner owner;
    auto accepted = Request();
    accepted.identity.span_generation = 2;
    accepted.intent.extra_span = FarVolumeCoverageSpan{-100, 1200};
    ASSERT_TRUE(owner.Advance(accepted));
    ASSERT_TRUE(owner.Renderer().AdvanceFrame(++owner.Frame(), {0, 0, 0}, {}));
    ASSERT_FALSE(owner.Renderer().Residency()->Observe(accepted.identity.key));
    auto reused = accepted;
    reused.intent.extra_span = FarVolumeCoverageSpan{-200, 1200};
    // A valid prefix must not be admitted before rejecting the returning key.
    const std::array manifest{Request({1, 100, 100}), reused};
    ExpectManifestRefusedTransactionally(owner, manifest, FarWorldRefusal::InvalidRequests);
    EXPECT_FALSE(owner.Renderer().Residency()->Observe(manifest.front().identity.key));
    EXPECT_FALSE(owner.Renderer().Residency()->Observe(accepted.identity.key));
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
        ASSERT_TRUE(owner.Renderer().AdvanceFrame(++owner.Frame(), {0, 0, 0}, {}));
        ASSERT_FALSE(owner.Renderer().Residency()->Observe(accepted.identity.key));
        auto shrinking = accepted;
        ++shrinking.identity.span_generation;
        shrinking.intent.extra_span = extension;
        const std::array manifest{Request({1, 100, 100}), shrinking};
        ExpectManifestRefusedTransactionally(owner, manifest, FarWorldRefusal::SpanRejected);
        EXPECT_FALSE(owner.Renderer().Residency()->Observe(manifest.front().identity.key));
        EXPECT_FALSE(owner.Renderer().Residency()->Observe(accepted.identity.key));
    }
}

TEST(FarWorldRenderer, AdvanceFrameSpanHistoryCapacityExhaustionRefusesTransactionally) {
    Owner owner;
    constexpr std::size_t history_slots = 512;
    std::array<FarVolumeWantedRequest, 64> batch;
    for (std::size_t offset = 0; offset < history_slots; offset += batch.size()) {
        for (std::size_t i = 0; i < batch.size(); ++i)
            batch[i] = Request({1, static_cast<std::int64_t>(100 + offset + i), 100});
        ASSERT_TRUE(owner.Renderer().AdvanceFrame(++owner.Frame(), {0, 0, 0}, batch));
    }
    // Replaying existing keys at full capacity still succeeds.
    ASSERT_TRUE(owner.Renderer().AdvanceFrame(++owner.Frame(), {0, 0, 0}, batch));
    const auto retained = batch.back();
    auto growth = retained;
    ++growth.identity.span_generation;
    growth.intent.extra_span = FarVolumeCoverageSpan{0, 0};
    const std::array manifest{growth, Request({1, 612, 100})};
    ExpectManifestRefusedTransactionally(owner, manifest, FarWorldRefusal::SpanHistoryExhausted);
    const auto observed = owner.Renderer().Residency()->Observe(retained.identity.key);
    ASSERT_TRUE(observed);
    if (observed) {
        EXPECT_EQ(observed->desired, retained.identity);
    }
    EXPECT_FALSE(owner.Renderer().Residency()->Observe(manifest.back().identity.key));
}

TEST(FarWorldRenderer, MixedPrepareFrameAndAdvanceFramePreserveHistoryAndGenerations) {
    Owner owner;
    const FarTileKey key{1, 0, 0};
    const std::array extensions{FarVolumeCoverageSpan{-100, 1200}};
    ASSERT_TRUE(owner.Renderer().PrepareFrame(++owner.Frame(), {0, 1000, 0}, extensions));
    const auto prepared = owner.Renderer().Residency()->Observe(key);
    ASSERT_TRUE(owner.Renderer().AdvanceFrame(++owner.Frame(), {0, 0, 0}, {}));
    ASSERT_FALSE(owner.Renderer().Residency()->Observe(key));
    ASSERT_TRUE(prepared);
    if (!prepared)
        return;
    auto request = Request(key);
    request.identity = prepared->desired;
    request.intent.extra_span = extensions.front();
    request.build_headroom_bytes = FarWorldDevelopmentLimits().build_headroom_bytes;
    ASSERT_TRUE(owner.Advance(request));
    request.identity.span_generation += 7;
    request.intent.extra_span = FarVolumeCoverageSpan{-200, 1200};
    ASSERT_TRUE(owner.Advance(request));
    ASSERT_TRUE(owner.Renderer().AdvanceFrame(++owner.Frame(), {0, 0, 0}, {}));
    ASSERT_FALSE(owner.Renderer().Residency()->Observe(key));
    ASSERT_TRUE(owner.Renderer().PrepareFrame(++owner.Frame(), {0, 500, 0}));
    const auto returned = owner.Renderer().Residency()->Observe(key);
    ASSERT_TRUE(returned);
    if (returned) {
        EXPECT_EQ(returned->desired, request.identity);
    }
    ASSERT_TRUE(owner.Advance(request)); // Equal identity also checks the complete intent.
    ASSERT_TRUE(owner.Renderer().PrepareFrame(++owner.Frame(), {0, -300, 0}));
    const auto grown = owner.Renderer().Residency()->Observe(key);
    ASSERT_TRUE(grown);
    if (!grown)
        return;
    EXPECT_EQ(grown->desired.span_generation, request.identity.span_generation + 1);
    auto expected = request;
    expected.identity = grown->desired;
    expected.intent.extra_span = FarVolumeCoverageSpan{-300, 1200};
    ASSERT_TRUE(owner.Advance(expected));
    ASSERT_TRUE(owner.Renderer().AdvanceFrame(++owner.Frame(), {0, 0, 0}, {}));
    // AdvanceFrame must consult the later generation accepted by PrepareFrame.
    ExpectManifestRefusedTransactionally(
        owner, std::span(&request, 1), FarWorldRefusal::InvalidRequests);
}

TEST(FarWorldRenderer, PrepareFrameHistoryRejectsChangedIntentAndShrinkageThroughAdvanceFrame) {
    for (const bool shrink : {false, true}) {
        Owner owner;
        const FarTileKey key{1, 0, 0};
        const std::array extensions{FarVolumeCoverageSpan{-100, 1200}};
        ASSERT_TRUE(owner.Renderer().PrepareFrame(++owner.Frame(), {0, 1000, 0}, extensions));
        const auto prepared = owner.Renderer().Residency()->Observe(key);
        ASSERT_TRUE(prepared);
        if (prepared) {
            ASSERT_TRUE(owner.Renderer().AdvanceFrame(++owner.Frame(), {0, 0, 0}, {}));
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
            EXPECT_FALSE(owner.Renderer().Residency()->Observe(key));
        }
    }
}

TEST(FarWorldRenderer, CheckedDrawRejectsForeignBackendAndRestoresBindingsOnFailure) {
    struct Foreign final : FarVolumeGpuAllocation {
        std::uint64_t bytes() const noexcept override {
            return 96;
        }
    } const foreign;
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
    owner.Renderer().BeginLiveOwnership(owner.Frame(), 1);
    ASSERT_TRUE(owner.Renderer().AddLiveUpload({{0, 0, 0, 0}, 1, 1, 3, true}));
    EXPECT_FALSE(owner.Renderer().AddLiveUpload({{1, 0, 0, 0}, 1, 1, 3, true}));
    EXPECT_FALSE(owner.Renderer().FinishLiveOwnership());
    std::size_t prepares = 0;
    const auto prepare = [&](const auto&) {
        ++prepares;
        return true;
    };
    EXPECT_EQ(owner.Renderer().Submit({0, 0, 0}, prepare).refused, 1u);
    owner.Renderer().BeginLiveOwnership(owner.Frame() - 1);
    ASSERT_TRUE(owner.Renderer().FinishLiveOwnership());
    EXPECT_EQ(owner.Renderer().Submit({0, 0, 0}, prepare).refused, 1u);
    EXPECT_EQ(prepares, 0u);
    EXPECT_EQ(owner.State().draws, 0u);
    owner.Renderer().BeginLiveOwnership(owner.Frame());
    ASSERT_TRUE(owner.Renderer().FinishLiveOwnership());
    EXPECT_EQ(owner.Renderer().Submit({0, 0, 0}, prepare).draws, 1u);
}

TEST(FarWorldRenderer, FailedSubmissionAndPrepareExceptionUnbindBeforeNextSubmission) {
    Owner owner;
    ASSERT_TRUE(owner.WaitSettled(Request()));
    owner.Renderer().BeginLiveOwnership(owner.Frame());
    ASSERT_TRUE(owner.Renderer().FinishLiveOwnership());
    const FarWorldPrepareDraw prepare = [](const auto&) {
        return true;
    };
    owner.State().fail_draw = true;
    const auto failed = owner.Renderer().Submit({0, 0, 0}, prepare);
    EXPECT_EQ(failed.draws, 0u);
    EXPECT_EQ(failed.refused, 1u);
    EXPECT_FALSE(owner.State().ownership_bound);
    owner.State().fail_draw = false;
    const auto refused = owner.Renderer().Submit({0, 0, 0}, [](const auto&) { return false; });
    EXPECT_EQ(refused.draws, 0u);
    EXPECT_EQ(refused.refused, 1u);
    EXPECT_FALSE(owner.State().ownership_bound);
    EXPECT_THROW(owner.Renderer().Submit({0, 0, 0},
                                         [](const FarWorldDrawCommand&) -> bool {
                                             throw std::runtime_error("prepare failed");
                                         }),
                 std::runtime_error);
    EXPECT_FALSE(owner.State().ownership_bound);
    EXPECT_EQ(owner.Renderer().Submit({0, 0, 0}, prepare).draws, 1u);
    EXPECT_FALSE(owner.State().ownership_bound);
}

TEST(FarWorldRenderer, DiagnosticUsesSelectedFrameAndSuccessfulDrawsNotRetainedResidency) {
    Owner owner;
    const auto request = Request();
    ASSERT_TRUE(owner.WaitSettled(request));
    owner.Renderer().BeginLiveOwnership(owner.Frame());
    ASSERT_TRUE(owner.Renderer().FinishLiveOwnership());
    const auto selection = owner.Renderer().Selection(FarWorldPurpose::World);
    const auto stats = owner.Renderer().Submit({0, 0, 0}, [](const auto&) { return true; });
    owner.Renderer().CaptureDiagnostic(
        selection, owner.Frame(), {0, 0, 0}, kFarPlaneMeters, true, stats.draws, stats.indices);
    const auto on = owner.Renderer().Diagnostic();
    ASSERT_EQ(on.tile_count, 1u);
    EXPECT_EQ(on.requested_cave_policy, FarCaveMode::BandLimited);
    EXPECT_EQ(on.bound_cave_policy, FarCaveMode::BandLimited);
    EXPECT_EQ(on.tiles[0].built, request.identity);
    EXPECT_EQ(on.tiles[0].desired, request.identity);
    EXPECT_TRUE(on.tiles[0].has_pair);
    EXPECT_TRUE(on.tiles[0].coverage.current);
    EXPECT_TRUE(on.tiles[0].coverage.span_ok);
    EXPECT_EQ(on.tiers[4].resident.uploaded_pairs, 1u);
    EXPECT_EQ(on.tiers[4].resident.covering, 1u);
    EXPECT_EQ(on.tiers[4].draws, 1u);
    EXPECT_GT(on.tiers[4].resident.retained_gpu_bytes, 0u);
    EXPECT_TRUE(on.submission_occurred);
    EXPECT_EQ(on.upload.upload_attempts, owner.Renderer().LastFrame().upload_attempts);

    const auto off = FarWorldSubmissionOverride(selection, false);
    bool advanced = false;
    AdvanceSelectedFarWorld(off, [&] { advanced = true; }, [&] { advanced = true; });
    const auto paired = SubmitSelectedFarWorld(
        off,
        [] { return FarWorldSubmitStats{}; },
        [&](const auto& prepare) { return owner.Renderer().Submit({0, 0, 0}, prepare); },
        [](const auto&) { return true; });
    EXPECT_FALSE(advanced);
    EXPECT_EQ(paired.draws, 0u);
    // Paired dispatch intentionally performs no CaptureDiagnostic.
    EXPECT_EQ(owner.Renderer().Diagnostic().frame, on.frame);
    EXPECT_EQ(owner.Renderer().Diagnostic().camera, on.camera);
    EXPECT_EQ(owner.Renderer().Diagnostic().world_epoch, on.world_epoch);
    EXPECT_EQ(owner.Renderer().Diagnostic().far_plane, on.far_plane);
    EXPECT_EQ(owner.Renderer().Diagnostic().requested_cave_policy, on.requested_cave_policy);
    EXPECT_EQ(owner.Renderer().Diagnostic().bound_cave_policy, on.bound_cave_policy);
    EXPECT_EQ(owner.Renderer().Diagnostic().tiers[4].resident.covering,
              on.tiers[4].resident.covering);
    EXPECT_EQ(owner.Renderer().Diagnostic().draws, on.draws);
    EXPECT_EQ(owner.Renderer().Diagnostic().upload.upload_bytes, on.upload.upload_bytes);
    EXPECT_EQ(owner.State().draws, 1u);
    EXPECT_FALSE(owner.Renderer().AdvanceFrame(owner.Frame(), {0, 0, 0}, std::span(&request, 1)));

    owner.State().fail_draw = true;
    const auto failed = owner.Renderer().Submit({0, 0, 0}, [](const auto&) { return true; });
    owner.Renderer().CaptureDiagnostic(
        selection, owner.Frame(), {0, 0, 0}, kFarPlaneMeters, false, failed.draws, failed.indices);
    EXPECT_EQ(owner.Renderer().Diagnostic().draws, 0u);
    EXPECT_FALSE(owner.Renderer().Diagnostic().submission_occurred);
    EXPECT_EQ(owner.Renderer().Diagnostic().tiers[4].resident.uploaded_pairs, 1u);
    EXPECT_EQ(owner.Renderer().Diagnostic().tiers[4].draw_refusals, 1u);

    auto refused = selection;
    refused.effective = FarWorldMode::Legacy;
    refused.refusal = FarWorldRefusal::ShaderProgramUnavailable;
    owner.Renderer().CaptureDiagnostic(
        refused, owner.Frame() + 1, {0, 0, 0}, kLegacyFarPlaneMeters, false, 0, 0, 9);
    EXPECT_EQ(owner.Renderer().Diagnostic().selection.requested, FarWorldMode::Ladder);
    EXPECT_EQ(owner.Renderer().Diagnostic().selection.effective, FarWorldMode::Legacy);
    EXPECT_EQ(owner.Renderer().Diagnostic().selection.refusal,
              FarWorldRefusal::ShaderProgramUnavailable);
    EXPECT_EQ(owner.Renderer().Diagnostic().tiers[4].resident.covering, 1u);
    EXPECT_EQ(owner.Renderer().Diagnostic().tiers[4].draws, 0u);
    EXPECT_EQ(owner.Renderer().Diagnostic().world_epoch, 9u);
}

TEST(FarWorldRenderer, DiagnosticRangeAndStaleCountersAccumulateOncePerTransition) {
    Owner owner;
    auto request = Request();
    ASSERT_TRUE(owner.WaitSettled(request));
    ++request.identity.span_generation;
    request.intent.extra_span = FarVolumeCoverageSpan{-300, 100};
    ASSERT_TRUE(owner.Advance(request));
    auto selection = owner.Renderer().Selection(FarWorldPurpose::World);
    owner.Renderer().CaptureDiagnostic(
        selection, owner.Frame(), {0, 0, 0}, kFarPlaneMeters, true, 0, 0);
    EXPECT_EQ(owner.Renderer().Diagnostic().tiers[4].stale_rebuilds, 1u);
    EXPECT_NE(owner.Renderer().Diagnostic().tiles[0].built, request.identity);
    ASSERT_TRUE(owner.Renderer().AdvanceFrame(++owner.Frame(), {0, 0, 0}, {}));
    owner.Renderer().CaptureDiagnostic(
        selection, owner.Frame(), {0, 0, 0}, kFarPlaneMeters, true, 0, 0);
    EXPECT_EQ(owner.Renderer().Diagnostic().tiers[4].evicted_range, 1u);
    ASSERT_TRUE(owner.Renderer().AdvanceFrame(++owner.Frame(), {0, 0, 0}, {}));
    owner.Renderer().CaptureDiagnostic(
        selection, owner.Frame(), {0, 0, 0}, kFarPlaneMeters, true, 0, 0);
    EXPECT_EQ(owner.Renderer().Diagnostic().tiers[4].evicted_range, 1u);
}

TEST(FarWorldRenderer, DiagnosticEmptyPairCoversWithoutDrawingOrRetainedGpuStore) {
    Owner owner;
    const auto empty = Request({5, 1, 1}); // immutable fixture is air outside its central column
    ASSERT_TRUE(owner.WaitSettled(empty));
    owner.Renderer().BeginLiveOwnership(owner.Frame());
    ASSERT_TRUE(owner.Renderer().FinishLiveOwnership());
    const auto stats = owner.Renderer().Submit({0, 0, 0}, [](const auto&) { return true; });
    EXPECT_EQ(stats.draws, 0u);
    EXPECT_EQ(owner.State().draws, 0u);
    owner.Renderer().CaptureDiagnostic(owner.Renderer().Selection(FarWorldPurpose::World),
                                       owner.Frame(),
                                       {0, 0, 0},
                                       kFarPlaneMeters,
                                       true,
                                       stats.draws,
                                       stats.indices);
    const auto& snapshot = owner.Renderer().Diagnostic();
    ASSERT_EQ(snapshot.tile_count, 1u);
    EXPECT_EQ(snapshot.tiles[0].coverage.state, FarTileState::Empty);
    EXPECT_EQ(snapshot.tiles[0].built, empty.identity);
    EXPECT_EQ(snapshot.tiers[4].resident.empty_pairs, 1u);
    EXPECT_EQ(snapshot.tiers[4].resident.covering, 1u);
    EXPECT_EQ(snapshot.tiers[4].resident.retained_gpu_bytes, 0u);
    EXPECT_EQ(snapshot.tiers[4].draws, 0u);
    EXPECT_FALSE(snapshot.submission_occurred);
    EXPECT_TRUE(FarWorldStationReady(snapshot, std::span(&empty.identity.key, 1), 1));
}

TEST(FarWorldRenderer, WaterBoundGrowsSpanBeforeIdentityAndRefusesOutOfBudgetHeight) {
    std::uint64_t dry_generation = 0;
    {
        auto dry = std::make_unique<Owner>();
        ASSERT_TRUE(dry->Renderer().PrepareFrame(++dry->Frame(), {0, 0, 0}));
        const auto dry_observation = dry->Renderer().Residency()->Observe({5, 0, 0});
        ASSERT_TRUE(dry_observation);
        if (dry_observation) {
            dry_generation = dry_observation->desired.span_generation;
        }
    }
    {
        auto wet = std::make_unique<Owner>(
            FarWorldDevelopmentLimits(), FarWorldWaterCapability::Wet, 500.0f);
        ASSERT_TRUE(wet->Renderer().PrepareFrame(++wet->Frame(), {0, 0, 0}));
        const auto wet_observation = wet->Renderer().Residency()->Observe({5, 0, 0});
        ASSERT_TRUE(wet_observation);
        if (wet_observation) {
            EXPECT_GT(wet_observation->desired.span_generation, dry_generation);
        }
    }
    auto limits = FarWorldDevelopmentLimits();
    limits.spans.max_y = 400;
    auto refused = std::make_unique<Owner>(limits, FarWorldWaterCapability::Wet, 500.0f);
    EXPECT_FALSE(refused->Renderer().PrepareFrame(++refused->Frame(), {0, 0, 0}));
    EXPECT_EQ(refused->Renderer().Selection(FarWorldPurpose::World).refusal,
              FarWorldRefusal::WaterSpanRefused);
    EXPECT_EQ(refused->Renderer().LastFrame().dispatched, 0u);
}

TEST(FarWorldRenderer, ExplicitWetManifestRefusesMissingWaterSpanBeforeDispatch) {
    Owner owner(FarWorldDevelopmentLimits(), FarWorldWaterCapability::Wet, 500);
    auto request = Request();
    request.intent.extra_span = FarVolumeCoverageSpan{0, 0};
    EXPECT_FALSE(owner.Renderer().AdvanceFrame(++owner.Frame(), {0, 0, 0}, std::span(&request, 1)));
    EXPECT_EQ(owner.Renderer().Selection(FarWorldPurpose::World).refusal,
              FarWorldRefusal::WaterSpanRefused);
    EXPECT_EQ(owner.Renderer().LastFrame().dispatched, 0u);
}

TEST(FarWorldRenderer, UnavailableWaterRefusesActivationWithoutAssumingDry) {
    DriverState state;
    JobSystem jobs;
    FarWorldRenderer renderer(FarWorldMode::Ladder);
    renderer.InstallBackend(std::make_unique<Driver>(state));
    EXPECT_EQ(
        renderer.BindFixture(
            jobs,
            std::make_shared<const Fixture>(
                state, FarCaveMode::BandLimited, 42, nullptr, FarWorldWaterCapability::Unavailable),
            1,
            1,
            FarWorldDevelopmentLimits()),
        FarWorldRefusal::WaterUnavailable);
    EXPECT_EQ(renderer.Selection(FarWorldPurpose::World).effective, FarWorldMode::Legacy);
    EXPECT_EQ(state.live, 0u);
}

TEST(FarWorldRenderer, LegacyBindingDoesNotRequireWaterObservation) {
    DriverState state;
    JobSystem jobs;
    FarWorldRenderer renderer(FarWorldMode::Legacy);
    renderer.InstallBackend(std::make_unique<Driver>(state));
    EXPECT_EQ(
        renderer.BindFixture(
            jobs,
            std::make_shared<const Fixture>(
                state, FarCaveMode::BandLimited, 42, nullptr, FarWorldWaterCapability::Unavailable),
            1,
            1,
            FarWorldDevelopmentLimits()),
        FarWorldRefusal::None);
    EXPECT_EQ(renderer.Selection(FarWorldPurpose::World).effective, FarWorldMode::Legacy);
    EXPECT_EQ(state.live, 0u);
}

TEST(FarWorldRenderer, WetEmptyTerrainCountsRetainedAndActualSubmissionSeparately) {
    Owner owner(FarWorldDevelopmentLimits(), FarWorldWaterCapability::Wet);
    const auto request = Request({5, 1, 1});
    // Explicit wet manifests contain the conservative bound before dispatch.
    auto wet_request = request;
    wet_request.intent.extra_span = FarVolumeCoverageSpan{0, 0};
    ASSERT_TRUE(owner.WaitSettled(wet_request));
    const auto observation = owner.Renderer().Residency()->Observe(request.identity.key);
    ASSERT_TRUE(observation);
    if (observation) {
        EXPECT_EQ(observation->coverage.state, FarTileState::Uploaded);
        EXPECT_EQ(observation->terrain_indices, 0u);
        EXPECT_EQ(observation->water_indices, 98304u);
    }
    owner.Renderer().BeginLiveOwnership(owner.Frame());
    ASSERT_TRUE(owner.Renderer().FinishLiveOwnership());
    owner.State().fail_draw = true;
    EXPECT_EQ(owner.Renderer().Submit({0, 0, 0}, [](const auto&) { return true; }).water_indices,
              0u);
    owner.Renderer().CaptureDiagnostic(owner.Renderer().Selection(FarWorldPurpose::World),
                                       owner.Frame(),
                                       {0, 0, 0},
                                       17408,
                                       true,
                                       0,
                                       0);
    EXPECT_EQ(owner.Renderer().Diagnostic().tiers[4].resident.retained_water_indices, 98304u);
    EXPECT_EQ(owner.Renderer().Diagnostic().tiers[4].resident.submitted_water_indices, 0u);
    owner.State().fail_draw = false;
    const auto submitted = owner.Renderer().Submit({0, 0, 0}, [](const auto&) { return true; });
    EXPECT_EQ(submitted.water_indices, 98304u);
    owner.Renderer().CaptureDiagnostic(owner.Renderer().Selection(FarWorldPurpose::World),
                                       owner.Frame(),
                                       {0, 0, 0},
                                       17408,
                                       true,
                                       submitted.draws,
                                       submitted.indices);
    const auto on = owner.Renderer().Diagnostic();
    EXPECT_EQ(on.tiers[4].resident.submitted_water_indices, 98304u);
    EXPECT_EQ(on.tiers[4].resident.water_wet_tiles, 1u);
    EXPECT_TRUE(on.ladder_water_available);
    const auto off = FarWorldSubmissionOverride(on.selection, false);
    SubmitSelectedFarWorld(off, {}, {}, {});
    EXPECT_EQ(owner.Renderer().Diagnostic().tiers[4].resident.submitted_water_indices,
              on.tiers[4].resident.submitted_water_indices);
}

namespace {
FarVolumeResidencyLimits RendererHandoffLimits() {
    auto limits = FarWorldDevelopmentLimits().residency;
    limits.entry_slots.fill(6);
    for (auto& queue : limits.queues) {
        queue.descriptor_slots = 8;
        queue.result_slots = 2;
    }
    return limits;
}
FarTierBudgets RendererHandoffBudgets() {
    FarTierBudgets budgets;
    budgets.bytes.fill(64ULL * 1024 * 1024);
    return budgets;
}
FarHandoffTileInput
RendererHandoffInput(FarWorldHandoffRenderer& renderer,
                     FarTileKey key,
                     std::uint64_t generation,
                     bool mixed,
                     FarCaveMode caves = FarCaveMode::BandLimited,
                     FarVolumeNumericProfile numeric = FarVolumeNumericProfile::RawFloat,
                     FarFrozenUse use = FarFrozenUse::Committed) {
    FarVolumeWantedRequest wanted;
    EXPECT_TRUE(ToFarVolumeTileKey(key, wanted.intent.key));
    wanted.identity = {1, 100 + generation, static_cast<std::uint8_t>(caves), key, generation, 1};
    wanted.intent.extra_span = FarVolumeCoverageSpan{-256, 64};
    const auto edge = FarTileEdgeMeters(key.tier);
    FarFrozenCaptureSource source;
    source.pristine.caves = caves;
    source.pristine.numeric = numeric;
    source.content_identity = source.pristine.field_identity = wanted.identity.content_hash;
    source.durable = {generation, FarEditedImageState::Committed};
    source.use = use;
    source.committed = [](std::uint8_t, const FarVolumePosition&) {
        return FarFrozenOverride<FarVolumeSample>{FarFrozenPresence::Absent, {}};
    };
    source.pristine.height = [](std::int64_t, std::int64_t) {
        return 0.0f;
    };
    source.pristine.density =
        [mixed, key, edge](const FarVolumePosition& p, float, std::uint32_t, FarCaveMode) {
            const auto x = p.x - key.tx * edge, z = p.z - key.tz * edge;
            return FarVolumeSample{
                mixed && x >= 0 && x <= 256 && z >= 0 && z <= 256 ? float(p.y) + .25f : 3.0f, 7};
        };
    source.water = [](std::int64_t, std::int64_t) {
        return FarFrozenWaterSample{false, 0};
    };
    FarVolumeRequest request;
    EXPECT_TRUE(
        ResolveFarVolumeCoverage(wanted.intent,
                                 source.pristine,
                                 RendererHandoffLimits().queues[key.tier - 1].compilation.authority,
                                 request));
    std::shared_ptr<const FarFrozenObservation> image;
    EXPECT_EQ(CaptureFarVolumeObservation(request,
                                          source,
                                          {1, 100'000, 20'000'000, 100'000, 32ULL * 1024 * 1024},
                                          renderer.MemoryPool(key.tier),
                                          image),
              FarFrozenCaptureError::None);
    if (!image)
        return {};
    wanted.identity.content_hash = image->content_identity();
    const auto input = MakeFarHandoffTileInput(wanted, image);
    EXPECT_TRUE(input);
    return input.value_or(FarHandoffTileInput{});
}
FarHandoffManifest RendererHandoffManifest(std::uint64_t serial,
                                           std::span<const FarHandoffTileInput> tiles) {
    return {1, serial, serial, FarCaveMode::BandLimited, FarVolumeNumericProfile::RawFloat, tiles};
}
bool RendererHandoffWait(const std::function<bool()>& predicate) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    do {
        if (predicate())
            return true;
        std::this_thread::yield();
    } while (std::chrono::steady_clock::now() < deadline);
    return false;
}
class RendererHandoffGate final : public std::pmr::memory_resource {
public:
    explicit RendererHandoffGate(BuildGate& gate)
        : gate_(gate) {}
    void Arm() {
        armed_ = true;
    }

private:
    std::atomic<bool> armed_{false};
    BuildGate& gate_;
    const std::thread::id owner_ = std::this_thread::get_id();
    void* do_allocate(std::size_t bytes, std::size_t alignment) override {
        if (std::this_thread::get_id() != owner_ && armed_.exchange(false)) {
            gate_.entered.count_down();
            gate_.release.wait();
            gate_.exited.count_down();
        }
        return std::pmr::new_delete_resource()->allocate(bytes, alignment);
    }
    void do_deallocate(void* p, std::size_t bytes, std::size_t alignment) override {
        std::pmr::new_delete_resource()->deallocate(p, bytes, alignment);
    }
    bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override {
        return this == &other;
    }
};
} // namespace

TEST(FarWorldHandoffRenderer, EveryTierAndProfileCapturesInTheBoundMemoryDomain) {
    JobSystem jobs;
    for (std::uint8_t tier = 1; tier <= kFarTierCount; ++tier) {
        SCOPED_TRACE(tier);
        for (const auto caves : {FarCaveMode::BandLimited, FarCaveMode::BoxFiltered}) {
            SCOPED_TRACE(static_cast<unsigned>(caves));
            for (const auto numeric :
                 {FarVolumeNumericProfile::RawFloat, FarVolumeNumericProfile::LegacySigned16}) {
                SCOPED_TRACE(static_cast<unsigned>(numeric));
                DriverState state;
                state.fixture_released = true;
                FarWorldHandoffRenderer renderer;
                renderer.InstallBackend(std::make_unique<Driver>(state));
                ASSERT_TRUE(renderer.Bind(
                    jobs, 1, caves, numeric, RendererHandoffBudgets(), RendererHandoffLimits()));
                const std::array inputs{
                    RendererHandoffInput(renderer, {tier, -1, 0}, 1, false, caves, numeric),
                    RendererHandoffInput(renderer, {5, 0, 0}, 1, false, caves, numeric)};
                ASSERT_TRUE(inputs[0].observation);
                ASSERT_TRUE(inputs[1].observation);
                auto manifest = RendererHandoffManifest(1, inputs);
                manifest.caves = caves;
                manifest.numeric = numeric;
                ASSERT_TRUE(renderer.StageTarget(manifest));
                const auto observed = renderer.Handoff()->Observe({tier, -1, 0});
                ASSERT_TRUE(observed);
                if (!observed)
                    return;
                ASSERT_TRUE(observed->target);
                if (!observed->target)
                    return;
                EXPECT_EQ(observed->target->identity, inputs[0].wanted.identity);
                EXPECT_EQ(inputs[0].observation->memory_pool(), renderer.MemoryPool(tier).get());
            }
        }
    }
}

TEST(FarWorldHandoffRenderer, IdleRetirementMakesCompleteDiagnosticsSafeForHistoryReclamation) {
    JobSystem jobs;
    DriverState state;
    state.fixture_released = true;
    FarWorldHandoffRenderer renderer;
    EXPECT_EQ(renderer.RetireIdleHistory(), 0u);
    renderer.InstallBackend(std::make_unique<Driver>(state));
    ASSERT_TRUE(renderer.Bind(jobs,
                              1,
                              FarCaveMode::BandLimited,
                              FarVolumeNumericProfile::RawFloat,
                              RendererHandoffBudgets(),
                              RendererHandoffLimits()));
    EXPECT_EQ(renderer.RetireIdleHistory(), 0u);
    EXPECT_TRUE(renderer.Diagnostic().captured);
    EXPECT_TRUE(renderer.Diagnostic().complete);
    EXPECT_EQ(renderer.Diagnostic().tile_count, 0u);
    const std::array first{RendererHandoffInput(renderer, {5, -1, 0}, 1, false)};
    const std::array next{RendererHandoffInput(renderer, {5, 1, 0}, 2, false)};
    ASSERT_TRUE(renderer.StageTarget(RendererHandoffManifest(1, first)));
    ASSERT_TRUE(renderer.StageTarget(RendererHandoffManifest(2, next)));
    auto returned = std::array{RendererHandoffInput(renderer, {5, -1, 0}, 3, false)};
    returned[0].wanted.intent.extra_span = FarVolumeCoverageSpan{-128, 64};
    EXPECT_FALSE(renderer.StageTarget(RendererHandoffManifest(3, returned)));
    EXPECT_EQ(renderer.AdmissionRefusal(), FarWorldRefusal::SpanRejected);
    ASSERT_TRUE(renderer.Diagnostic().complete);
    ASSERT_EQ(renderer.Diagnostic().tile_count, 1u);
    EXPECT_EQ(renderer.Diagnostic().tiles[0].key, next[0].wanted.identity.key);
    EXPECT_EQ(renderer.Submit({0, 0, 0}, [](const auto&) { return true; }).refused, 1u);
    ASSERT_EQ(renderer.Diagnostic().submission.refused, 1u);
    EXPECT_EQ(renderer.RetireIdleHistory(), 1u);
    EXPECT_TRUE(renderer.Diagnostic().captured);
    EXPECT_TRUE(renderer.Diagnostic().complete);
    ASSERT_EQ(renderer.Diagnostic().tile_count, 1u);
    EXPECT_EQ(renderer.Diagnostic().tiles[0].key, next[0].wanted.identity.key);
    EXPECT_EQ(renderer.Diagnostic().submission.refused, 0u);
    EXPECT_EQ(renderer.AdmissionRefusal(), FarWorldRefusal::SpanRejected);
    EXPECT_EQ(renderer.RetireIdleHistory(), 0u);
    EXPECT_TRUE(renderer.StageTarget(RendererHandoffManifest(3, returned)));
    renderer.PrepareWorldSwap();
    EXPECT_EQ(renderer.RetireIdleHistory(), 0u);
}

TEST(FarWorldHandoffRenderer, SaveFencePreservesBindingOwnershipAndDraws) {
    JobSystem jobs;
    jobs.startup(1);
    DriverState state;
    FarWorldHandoffRenderer renderer;
    renderer.InstallBackend(std::make_unique<Driver>(state));
    EXPECT_NO_THROW(renderer.WaitForAcceptedCacheWrites()); // no binding or accepted writes
    ASSERT_TRUE(renderer.Bind(jobs,
                              1,
                              FarCaveMode::BandLimited,
                              FarVolumeNumericProfile::RawFloat,
                              RendererHandoffBudgets(),
                              RendererHandoffLimits()));
    const std::array inputs{RendererHandoffInput(renderer, {5, 0, 0}, 1, true)};
    ASSERT_TRUE(renderer.StageTarget(RendererHandoffManifest(1, inputs)));
    std::uint64_t frame = 0;
    ASSERT_TRUE(RendererHandoffWait([&] {
        renderer.AdvanceFrame(++frame, {0, 0, 0}, {1, 1, 16ULL * 1024 * 1024});
        return renderer.Handoff()->State().serving_serial == 1;
    }));
    renderer.BeginLiveOwnership(frame);
    ASSERT_TRUE(renderer.FinishLiveOwnership());
    const auto* handoff = renderer.Handoff();
    const auto live = state.live;
    const auto deletes = state.deletes;
    state.ownership_bound = true;
    renderer.WaitForAcceptedCacheWrites();
    EXPECT_TRUE(state.ownership_bound);
    ASSERT_EQ(renderer.Handoff(), handoff);
    EXPECT_EQ(renderer.Handoff()->State().serving_serial, 1u);
    EXPECT_EQ(state.live, live);
    EXPECT_EQ(state.deletes, deletes);
    state.ownership_bound = false;
    EXPECT_EQ(renderer
                  .Submit({0, 0, 0},
                          [&](const auto&) {
                              EXPECT_THROW(renderer.WaitForAcceptedCacheWrites(), std::logic_error);
                              return true;
                          })
                  .draws,
              1u);
    std::thread foreign(
        [&] { EXPECT_THROW(renderer.WaitForAcceptedCacheWrites(), std::logic_error); });
    foreign.join();
    renderer.PrepareWorldSwap();
    EXPECT_EQ(state.live, 0u);
    EXPECT_TRUE(state.released_with_context);
    state.fixture_released = true; // Handoff inputs do not install a Fixture owner.
    renderer.Shutdown();
    jobs.shutdown();
}

TEST(FarWorldHandoffRenderer, SameFrameOwnershipAndSubmitExceptionRestoreBindings) {
    JobSystem jobs;
    jobs.startup(1);
    DriverState state;
    FarWorldHandoffRenderer renderer;
    auto driver = std::make_unique<Driver>(state);
    auto* fake = driver.get();
    renderer.InstallBackend(std::move(driver));
    ASSERT_TRUE(renderer.Bind(jobs,
                              1,
                              FarCaveMode::BandLimited,
                              FarVolumeNumericProfile::RawFloat,
                              RendererHandoffBudgets(),
                              RendererHandoffLimits()));
    auto inputs = std::array{RendererHandoffInput(renderer, {5, 0, 0}, 1, true)};
    ASSERT_TRUE(renderer.StageTarget(RendererHandoffManifest(1, inputs)));
    std::uint64_t frame = 0;
    ASSERT_TRUE(RendererHandoffWait([&] {
        renderer.AdvanceFrame(++frame, {0, 0, 0}, {1, 1, 16ULL * 1024 * 1024});
        return renderer.Handoff()->State().serving_serial == 1;
    }));
    std::size_t prepares = 0;
    const auto prepare = [&](const auto&) {
        ++prepares;
        return true;
    };
    EXPECT_EQ(renderer.Submit({0, 0, 0}, prepare).refused, 1u);
    EXPECT_EQ(prepares, 0u);
    renderer.BeginLiveOwnership(frame - 1);
    ASSERT_TRUE(renderer.FinishLiveOwnership());
    EXPECT_EQ(renderer.Submit({0, 0, 0}, prepare).refused, 1u);
    renderer.BeginLiveOwnership(frame);
    ASSERT_TRUE(renderer.FinishLiveOwnership());
    fake->BindVertexArray(37);
    fake->BindBuffer(FarVolumeBufferKind::Vertex, 38);
    EXPECT_THROW(renderer.Submit({0, 0, 0},
                                 [&](const auto&) -> bool {
                                     EXPECT_THROW(renderer.BeginLiveOwnership(frame),
                                                  std::logic_error);
                                     fake->BindVertexArray(39);
                                     throw std::runtime_error("draw preparation failed");
                                 }),
                 std::runtime_error);
    EXPECT_FALSE(state.ownership_bound);
    EXPECT_EQ(fake->vao(), 37u);
    std::uint32_t vbo = 0;
    fake->GetVertexBufferBinding(vbo);
    EXPECT_EQ(vbo, 38u);
    EXPECT_EQ(renderer.Submit({0, 0, 0}, prepare).draws, 1u);
    EXPECT_FALSE(state.ownership_bound);
    EXPECT_EQ(fake->vao(), 37u);
    renderer.PrepareWorldSwap();
    EXPECT_EQ(state.live, 0u);
    EXPECT_FALSE(renderer.Diagnostic().captured);
    EXPECT_EQ(renderer.Diagnostic().tile_count, 0u);
    EXPECT_EQ(renderer.Diagnostic().draw_count, 0u);
    EXPECT_EQ(renderer.Diagnostic().state.target_serial, 0u);
    EXPECT_EQ(renderer.Diagnostic().submission.draws, 0u);
    state.fixture_released = true;
    renderer.Shutdown();
}

TEST(FarWorldHandoffRenderer, RepeatedBackendInstallAndEmptySwapUseBoundedStack) {
    DriverState state;
    state.fixture_released = true;
    FarWorldHandoffRenderer renderer;
    renderer.InstallBackend(std::make_unique<Driver>(state));
    renderer.InstallBackend(std::make_unique<Driver>(state));
    renderer.PrepareWorldSwap();
    EXPECT_FALSE(renderer.Diagnostic().captured);
    EXPECT_TRUE(renderer.Diagnostic().complete);
    EXPECT_EQ(renderer.Diagnostic().tile_count, 0u);
    EXPECT_EQ(renderer.Diagnostic().draw_count, 0u);
    renderer.Shutdown();
}

TEST(FarWorldHandoffRenderer, SubmissionDiagnosticsKeepOrderAcrossRefusedAndRepeatedDraws) {
    JobSystem jobs;
    jobs.startup(2);
    DriverState state;
    state.fixture_released = true;
    FarWorldHandoffRenderer renderer;
    renderer.InstallBackend(std::make_unique<Driver>(state));
    ASSERT_TRUE(renderer.Bind(jobs,
                              1,
                              FarCaveMode::BandLimited,
                              FarVolumeNumericProfile::RawFloat,
                              RendererHandoffBudgets(),
                              RendererHandoffLimits()));
    const auto inputs = std::array{RendererHandoffInput(renderer, {5, -1, 0}, 1, true),
                                   RendererHandoffInput(renderer, {5, 0, 0}, 1, true)};
    ASSERT_TRUE(renderer.StageTarget(RendererHandoffManifest(1, inputs)));
    std::uint64_t frame = 0;
    ASSERT_TRUE(RendererHandoffWait([&] {
        renderer.AdvanceFrame(++frame, {0, 0, 0}, {2, 2, 16ULL * 1024 * 1024});
        return renderer.Handoff()->State().serving_serial == 1;
    }));
    renderer.BeginLiveOwnership(frame);
    ASSERT_TRUE(renderer.FinishLiveOwnership());
    ASSERT_EQ(renderer.Diagnostic().draw_count, 2u);
    std::size_t prepares = 0;
    const auto partial = renderer.Submit({0, 0, 0}, [&](const auto&) { return ++prepares == 2; });
    EXPECT_EQ(partial.draws, 1u);
    EXPECT_EQ(partial.refused, 1u);
    EXPECT_FALSE(renderer.Diagnostic().draws[0].submitted);
    EXPECT_TRUE(renderer.Diagnostic().draws[1].submitted);
    const auto refused = renderer.Submit({0, 0, 0}, [](const auto&) { return false; });
    EXPECT_EQ(refused.draws, 0u);
    EXPECT_EQ(refused.refused, 2u);
    EXPECT_FALSE(renderer.Diagnostic().draws[0].submitted);
    EXPECT_FALSE(renderer.Diagnostic().draws[1].submitted);
    EXPECT_EQ(renderer.Submit({0, 0, 0}, [](const auto&) { return true; }).draws, 2u);
    EXPECT_TRUE(renderer.Diagnostic().draws[0].submitted);
    EXPECT_TRUE(renderer.Diagnostic().draws[1].submitted);
}

TEST(FarWorldHandoffRenderer, WorldSwapUnbindsBeforeJoiningAndReleasesBothGenerations) {
    JobSystem jobs;
    jobs.startup(2);
    DriverState state;
    BuildGate barrier;
    RendererHandoffGate gate(barrier);
    FarWorldHandoffRenderer renderer;
    // Fatal test exits still release the worker before renderer destruction.
    class Release {
    public:
        Release(BuildGate& barrier, DriverState& state)
            : barrier_(barrier)
            , state_(state) {}
        ~Release() {
            if (!barrier_.release.try_wait())
                barrier_.release.count_down();
            state_.release_on_drain = nullptr;
        }

    private:
        BuildGate& barrier_;
        DriverState& state_;
    } const release{barrier, state};
    renderer.InstallBackend(std::make_unique<Driver>(state));
    ASSERT_TRUE(renderer.Bind(jobs,
                              1,
                              FarCaveMode::BandLimited,
                              FarVolumeNumericProfile::RawFloat,
                              RendererHandoffBudgets(),
                              RendererHandoffLimits(),
                              &gate));
    auto a = std::array{RendererHandoffInput(renderer, {5, 0, 0}, 1, true),
                        RendererHandoffInput(renderer, {4, 0, 0}, 1, true)};
    ASSERT_TRUE(renderer.StageTarget(RendererHandoffManifest(1, a)));
    std::uint64_t frame = 0;
    ASSERT_TRUE(RendererHandoffWait([&] {
        renderer.AdvanceFrame(++frame, {0, 0, 0}, {2, 2, 16ULL * 1024 * 1024});
        const auto observed = renderer.Handoff()->Observe({4, 0, 0});
        if (!observed)
            return false;
        return observed->serving.has_value();
    }));
    auto b = std::array{RendererHandoffInput(renderer, {5, 0, 0}, 2, true),
                        RendererHandoffInput(renderer, {4, 0, 0}, 2, true)};
    ASSERT_TRUE(renderer.StageTarget(RendererHandoffManifest(2, b)));
    gate.Arm();
    renderer.AdvanceFrame(++frame, {0, 0, 0}, {1, 0, 0});
    ASSERT_TRUE(RendererHandoffWait([&] { return barrier.entered.try_wait(); }));
    ASSERT_TRUE(RendererHandoffWait([&] {
        renderer.AdvanceFrame(++frame, {0, 0, 0}, {1, 1, 16ULL * 1024 * 1024});
        const auto observed = renderer.Handoff()->Observe({4, 0, 0});
        if (!observed)
            return false;
        return IsFarTileCovering(observed->target_coverage);
    }));
    EXPECT_EQ(renderer.Handoff()->State().serving_serial, 1u);
    const auto staged = renderer.Handoff()->Observe({4, 0, 0});
    ASSERT_TRUE(staged);
    if (!staged)
        return;
    EXPECT_GT(staged->staged_gpu_bytes, 0u);
    std::weak_ptr<const FarFrozenObservation> const held = b[0].observation;
    a = {};
    b = {};
    const auto creates = state.live;
    EXPECT_GT(creates, 0u);
    state.release_on_drain = &barrier;
    renderer.PrepareWorldSwap();
    EXPECT_EQ(state.held_build_drains, 1u);
    EXPECT_EQ(state.live, 0u);
    EXPECT_EQ(state.deletes, creates);
    EXPECT_TRUE(held.expired());
    EXPECT_TRUE(barrier.exited.try_wait());
    EXPECT_EQ(renderer.Handoff(), nullptr);
    state.fixture_released = true;
    renderer.Shutdown();
}

TEST(FarWorldHandoffRenderer, ExternalCaptureAliasesKeepOneMemoryDomainAliveAfterSwap) {
    JobSystem jobs;
    DriverState state;
    FarWorldHandoffRenderer renderer;
    renderer.InstallBackend(std::make_unique<Driver>(state));
    ASSERT_TRUE(renderer.Bind(jobs,
                              1,
                              FarCaveMode::BandLimited,
                              FarVolumeNumericProfile::RawFloat,
                              RendererHandoffBudgets(),
                              RendererHandoffLimits()));
    auto pool = renderer.MemoryPool(5);
    std::weak_ptr<FarVolumeMemoryPool> const weak = pool;
    auto input = RendererHandoffInput(renderer, {5, 0, 0}, 1, false);
    const auto before_alias = pool->used();
    auto alias = input;
    EXPECT_EQ(pool->used(), before_alias);
    renderer.PrepareWorldSwap();
    EXPECT_FALSE(weak.expired());
    const auto retained = pool->used();
    input = {};
    EXPECT_EQ(pool->used(), retained);
    alias = {};
    EXPECT_LT(pool->used(), retained);
    EXPECT_GT(pool->used(), 0u); // The shared memory domain's own control charge.
    pool.reset();
    EXPECT_TRUE(weak.expired());
    state.fixture_released = true;
    renderer.Shutdown();
}

namespace {
class SessionRendererTest : public testing::Test {
protected:
    using Producer = FarWorldAuthorityProducer;
    using Save = Persistence::WorldSaveService;
    std::filesystem::path root = TestSupport::CreateUniqueTempDirectory("session_renderer_");
    DriverState state;
    JobSystem jobs;
    FarWorldRenderer renderer;
    Producer producer{{512,
                       4u * 1024u * 1024u,
                       300'000,
                       std::make_shared<FarVolumeMemoryPool>(64u * 1024u * 1024u)}};
    std::unique_ptr<Systems::SHIELD_WorldSystem> world;
    Producer::FrozenPolicy policy;
    FarWorldExperimentalLimits limits = FarWorldDevelopmentLimits();
    FarFrozenCaptureLimits capture_limits{1, 100'000, 20'000'000, 200'000, 64u * 1024u * 1024u};
    FarFrozenCaptureWork::Counts samples{400'000, 40'000'000, 480'000};
    std::uint64_t frame = 0;
    const std::array<Save::AuthorityWindow, 1> windows{{{-6, 518, -6, 518}}};
    void SetUp() override {
        jobs.startup(1);
        auto params = Luminumbra::Test::SyntheticTerrainParams();
        params.caves_enabled = false;
        params.base_amplitude = params.peaks_amplitude = 0;
        params.height_offset = -8;
        params.continental_spline = {{-1, 0}, {1, 0}};
        world = std::make_unique<Systems::SHIELD_WorldSystem>(nullptr, nullptr, params, 1337);
        policy = {WorldFarVolumeIdentity(
                      *world, FarCaveMode::BandLimited, FarVolumeNumericProfile::RawFloat, 4),
                  FarCaveMode::BandLimited,
                  FarVolumeNumericProfile::RawFloat,
                  4};
        WorldStreamingState disk;
        ASSERT_TRUE(Save{}.save_world(disk, root));
        const Save::AuthorityReadLimits reads{
            512, 512, 1024, 8192, 4194304, 16777216, 4194304, 16777216, 131072};
        std::string error;
        ASSERT_TRUE(producer.admit_saved_source(root, reads, error)) << error;
        const Producer::CaptureClock clock{
            [](const void*) noexcept -> std::uint64_t { return 0; }, nullptr, 1};
        ASSERT_TRUE(producer.configure_capture({reads, 4, clock}));
        ASSERT_TRUE(producer.request_capture(windows));
        producer.pump_capture();
        producer.pump_capture();
        ASSERT_TRUE(producer.capture_status().latest_ready);
        limits.residency = RendererHandoffLimits();
        renderer.InstallBackend(std::make_unique<Driver>(state));
        ASSERT_EQ(renderer.BindSession(jobs, 1, policy.caves, policy.numeric, limits),
                  FarWorldRefusal::None);
    }
    void TearDown() override {
        renderer.PrepareWorldSwap();
        state.fixture_released = true;
        renderer.Shutdown();
        jobs.shutdown();
        std::error_code error;
        std::filesystem::remove_all(root, error);
    }
    std::array<FarSessionHandoffTile, 2> Tiles() const {
        std::array<FarSessionHandoffTile, 2> result{};
        const std::array<FarTileKey, 2> keys{{{1, 0, 0}, {5, 0, 0}}};
        for (std::size_t i = 0; i < keys.size(); ++i) {
            auto& tile = result[i];
            const auto& key = keys[i];
            tile.wanted.identity = {1, policy.pristine_identity, 0, key, 0, 1};
            EXPECT_TRUE(ToFarVolumeTileKey(key, tile.wanted.intent.key));
            tile.wanted.intent.extra_span =
                key.tier == 1 ? FarVolumeCoverageSpan{-256, 0} : FarVolumeCoverageSpan{-256, 512};
            tile.wanted.build_headroom_bytes = limits.build_headroom_bytes;
            // Include the -8 m surface's 256 m interior depth at each brick alignment.
            tile.capture = {tile.wanted.intent.key,
                            key.tier == 1 ? FarVolumeSpan{-272, 16} : FarVolumeSpan{-512, 768},
                            policy.pristine_identity};
        }
        return result;
    }
    bool Stage() {
        const auto tiles = Tiles();
        return renderer.StageSessionTarget(
            producer, *world, tiles, policy, capture_limits, samples);
    }
    bool Settle() {
        return RendererHandoffWait([&] {
            if (!renderer.PrepareFrame(++frame, {0, 0, 0})) {
                return false;
            }
            const auto* diagnostic = renderer.SessionDiagnostic();
            return diagnostic &&
                   diagnostic->state.serving_serial == diagnostic->state.target_serial &&
                   FarWorldHandoffTargetReady(*diagnostic);
        });
    }
    void Capture(const FarWorldSubmitStats& stats = {}) {
        renderer.CaptureDiagnostic(renderer.Selection(FarWorldPurpose::World),
                                   frame,
                                   {0, 0, 0},
                                   kFarPlaneMeters,
                                   true,
                                   stats.draws,
                                   stats.indices);
    }
};
} // namespace

TEST(FarWorldRenderer, IdleRetirementWithoutSessionIsAnIdempotentNoOp) {
    for (const auto mode : {FarWorldMode::Ladder, FarWorldMode::Legacy}) {
        FarWorldRenderer renderer(mode);
        const auto before = renderer.Selection(FarWorldPurpose::World);
        EXPECT_EQ(renderer.RetireIdleHistory(), 0u);
        EXPECT_EQ(renderer.RetireIdleHistory(), 0u);
        EXPECT_EQ(renderer.SessionDiagnostic(), nullptr);
        EXPECT_EQ(renderer.Selection(FarWorldPurpose::World).refusal, before.refusal);
    }
}

TEST(FarWorldRenderer, IdleRetirementRequiresTheOwnerThreadEvenWhenUnbound) {
    FarWorldRenderer renderer;
    bool refused = false;
    std::thread worker([&] {
        try {
            renderer.RetireIdleHistory();
        } catch (const std::logic_error&) {
            refused = true;
        }
    });
    worker.join();
    EXPECT_TRUE(refused);
    EXPECT_EQ(renderer.RetireIdleHistory(), 0u);
}

TEST_F(SessionRendererTest, IdleRetirementMatchesHandoffCountAndAdmitsFreshReturns) {
    DriverState direct_state;
    direct_state.fixture_released = true;
    FarWorldHandoffRenderer direct;
    direct.InstallBackend(std::make_unique<Driver>(direct_state));
    ASSERT_TRUE(
        direct.Bind(jobs, 1, policy.caves, policy.numeric, limits.budgets, limits.residency));
    const auto stage = [&](std::span<const FarSessionHandoffTile> tiles) {
        const bool accepted =
            renderer.StageSessionTarget(producer, *world, tiles, policy, capture_limits, samples);
        EXPECT_EQ(
            accepted,
            direct.StageSessionTarget(producer, *world, tiles, policy, capture_limits, samples));
        EXPECT_EQ(renderer.Selection(FarWorldPurpose::World).refusal, direct.AdmissionRefusal());
        return accepted;
    };
    const auto tiles = Tiles();
    EXPECT_EQ(renderer.RetireIdleHistory(), 0u);
    ASSERT_TRUE(stage(tiles));
    EXPECT_EQ(renderer.RetireIdleHistory(), 0u); // Target descriptors remain protected.
    ASSERT_TRUE(producer.request_capture(windows));
    producer.pump_capture();
    producer.pump_capture();
    ASSERT_TRUE(producer.capture_status().latest_ready);
    ASSERT_TRUE(stage(std::span(tiles).subspan(1)));
    ASSERT_TRUE(producer.request_capture(windows));
    producer.pump_capture();
    producer.pump_capture();
    ASSERT_TRUE(producer.capture_status().latest_ready);
    auto returned = tiles;
    returned[0].wanted.intent.extra_span = FarVolumeCoverageSpan{-128, 0};
    EXPECT_FALSE(stage(returned));
    // A new capture serial with unchanged content still reuses the tile identity.
    // Changed metadata under that retained identity is an invalid request.
    EXPECT_EQ(renderer.Selection(FarWorldPurpose::World).refusal, FarWorldRefusal::InvalidRequests);
    const auto* diagnostic = renderer.SessionDiagnostic();
    ASSERT_NE(diagnostic, nullptr);
    ASSERT_TRUE(diagnostic->captured);
    ASSERT_TRUE(diagnostic->complete);
    ASSERT_EQ(diagnostic->tile_count, 1u);
    EXPECT_EQ(diagnostic->tiles[0].key, tiles[1].wanted.identity.key);
    const auto retired = direct.RetireIdleHistory();
    ASSERT_EQ(retired, 1u);
    EXPECT_EQ(renderer.RetireIdleHistory(), retired);
    EXPECT_EQ(renderer.Selection(FarWorldPurpose::World).refusal, FarWorldRefusal::InvalidRequests);
    EXPECT_EQ(renderer.RetireIdleHistory(), 0u);
    EXPECT_EQ(direct.RetireIdleHistory(), 0u);
    ASSERT_TRUE(stage(returned));
    EXPECT_EQ(renderer.RetireIdleHistory(), 0u);
    renderer.PrepareWorldSwap();
    EXPECT_EQ(renderer.RetireIdleHistory(), 0u);
}

TEST_F(SessionRendererTest, LegacyIdleRetirementPreservesBoundSessionHistory) {
    renderer.SetRequested(FarWorldMode::Legacy);
    ASSERT_EQ(renderer.BindSession(jobs, 1, policy.caves, policy.numeric, limits),
              FarWorldRefusal::None);
    const auto tiles = Tiles();
    ASSERT_TRUE(
        renderer.StageSessionTarget(producer, *world, tiles, policy, capture_limits, samples));
    ASSERT_TRUE(producer.request_capture(windows));
    producer.pump_capture();
    producer.pump_capture();
    ASSERT_TRUE(producer.capture_status().latest_ready);
    ASSERT_TRUE(renderer.StageSessionTarget(
        producer, *world, std::span(tiles).subspan(1), policy, capture_limits, samples));
    const auto* diagnostic = renderer.SessionDiagnostic();
    ASSERT_NE(diagnostic, nullptr);
    const auto target = diagnostic->state.target_serial;
    EXPECT_EQ(renderer.Selection(FarWorldPurpose::World).effective, FarWorldMode::Legacy);
    EXPECT_EQ(renderer.RetireIdleHistory(), 0u);
    EXPECT_EQ(renderer.RetireIdleHistory(), 0u);
    EXPECT_EQ(diagnostic->state.target_serial, target);
    ASSERT_TRUE(producer.request_capture(windows));
    producer.pump_capture();
    producer.pump_capture();
    ASSERT_TRUE(producer.capture_status().latest_ready);
    auto returned = tiles;
    returned[0].wanted.intent.extra_span = FarVolumeCoverageSpan{-128, 0};
    EXPECT_FALSE(
        renderer.StageSessionTarget(producer, *world, returned, policy, capture_limits, samples));
    EXPECT_EQ(renderer.Selection(FarWorldPurpose::World).refusal, FarWorldRefusal::InvalidRequests);
    EXPECT_EQ(renderer.RetireIdleHistory(), 0u);
    EXPECT_EQ(diagnostic->state.target_serial, target);
    // A different capture support changes the identity, exposing the retained
    // extra-span constraint rather than the equal-identity metadata check.
    returned[0].capture.span.max_y += 16;
    EXPECT_FALSE(
        renderer.StageSessionTarget(producer, *world, returned, policy, capture_limits, samples));
    EXPECT_EQ(renderer.Selection(FarWorldPurpose::World).refusal, FarWorldRefusal::SpanRejected);
    EXPECT_EQ(diagnostic->state.target_serial, target);
}

TEST_F(SessionRendererTest, SessionSelectionPreservesAdmissionRefusalsAndPurposeOverrides) {
    for (const auto mode : {FarWorldMode::Ladder, FarWorldMode::Legacy}) {
        SCOPED_TRACE(static_cast<int>(mode));
        renderer.SetRequested(mode);
        ASSERT_EQ(renderer.BindSession(jobs, 1, policy.caves, policy.numeric, limits),
                  FarWorldRefusal::None);
        const auto check = [&](FarWorldRefusal refusal) {
            for (const bool enabled : {false, true}) {
                const auto selection = renderer.Selection(FarWorldPurpose::World, enabled);
                EXPECT_EQ(selection.refusal, refusal);
                EXPECT_EQ(selection.effective,
                          refusal == FarWorldRefusal::None ? mode : FarWorldMode::Legacy);
                EXPECT_EQ(selection.advance, enabled);
                EXPECT_EQ(selection.submit, enabled);
                const auto preview = renderer.Selection(FarWorldPurpose::Preview, enabled);
                EXPECT_EQ(preview.refusal, FarWorldRefusal::PreviewOverride);
                EXPECT_EQ(preview.effective, FarWorldMode::Legacy);
                EXPECT_EQ(preview.advance, enabled);
                EXPECT_EQ(preview.submit, enabled);
                const auto offscreen =
                    renderer.Selection(FarWorldPurpose::OffscreenPreview, enabled);
                EXPECT_EQ(offscreen.refusal, FarWorldRefusal::OffscreenSuppressed);
                EXPECT_EQ(offscreen.effective, FarWorldMode::Legacy);
                EXPECT_FALSE(offscreen.advance);
                EXPECT_FALSE(offscreen.submit);
            }
        };
        check(FarWorldRefusal::ImmutableAuthorityUnavailable);
        ASSERT_TRUE(Stage());
        check(FarWorldRefusal::None);
        state.current = false;
        check(FarWorldRefusal::BackendUnavailable);
        state.current = true;
        check(FarWorldRefusal::None);
        const auto target = renderer.SessionDiagnostic()->state.target_serial;
        auto invalid = Tiles();
        invalid[0].capture.span.min_y += 1;
        EXPECT_FALSE(renderer.StageSessionTarget(
            producer, *world, invalid, policy, capture_limits, samples));
        check(FarWorldRefusal::SpanRejected);
        EXPECT_FALSE(
            renderer.StageSessionTarget(producer, *world, {}, policy, capture_limits, samples));
        check(FarWorldRefusal::InvalidRequests);
        auto unknown = policy;
        unknown.numeric = static_cast<FarVolumeNumericProfile>(255);
        EXPECT_FALSE(renderer.StageSessionTarget(
            producer, *world, Tiles(), unknown, capture_limits, samples));
        check(FarWorldRefusal::InvalidRequests);
        unknown = policy;
        unknown.caves = static_cast<FarCaveMode>(255);
        EXPECT_FALSE(renderer.StageSessionTarget(
            producer, *world, Tiles(), unknown, capture_limits, samples));
        check(FarWorldRefusal::CavePolicyMismatch);
        auto incomplete = capture_limits;
        incomplete.max_bytes = 0;
        EXPECT_FALSE(
            renderer.StageSessionTarget(producer, *world, Tiles(), policy, incomplete, samples));
        check(FarWorldRefusal::MissingLimits);
        auto partial = samples;
        partial.density = 1;
        EXPECT_FALSE(renderer.StageSessionTarget(
            producer, *world, Tiles(), policy, capture_limits, partial));
        check(FarWorldRefusal::AdmissionFailed);
        auto expired = capture_limits;
        expired.deadline = {[](const void*) noexcept -> std::uint64_t { return 1; }, nullptr, 1};
        EXPECT_FALSE(
            renderer.StageSessionTarget(producer, *world, Tiles(), policy, expired, samples));
        check(FarWorldRefusal::AdmissionFailed);
        producer.fence_source_reads(true);
        EXPECT_FALSE(Stage());
        check(FarWorldRefusal::AdmissionFailed);
        producer.fence_source_reads(false);
        EXPECT_EQ(renderer.SessionDiagnostic()->state.target_serial, target);
        EXPECT_EQ(renderer.SessionDiagnostic()->state.serving_serial, 0u);
        ASSERT_TRUE(Stage());
        check(FarWorldRefusal::None);
    }
}

TEST_F(SessionRendererTest, ProducerSessionInputsBindAndSubmitHeterogeneousTileFingerprints) {
    EXPECT_EQ(renderer.Residency(), nullptr);
    ASSERT_TRUE(Stage()) << FarWorldRefusalName(renderer.Selection(FarWorldPurpose::World).refusal);
    ASSERT_TRUE(Settle());
    renderer.BeginLiveOwnership(frame);
    ASSERT_TRUE(renderer.FinishLiveOwnership());
    const auto stats = renderer.Submit({0, 0, 0}, [](const auto&) { return true; });
    Capture(stats);
    const auto& snapshot = renderer.Diagnostic();
    ASSERT_EQ(snapshot.tile_count, 2u);
    EXPECT_GT(stats.water_indices, 0u);
    EXPECT_NE(snapshot.tiles[0].accepted_content_identity,
              snapshot.tiles[1].accepted_content_identity);
    EXPECT_EQ(snapshot.authority_kind, FarWorldAuthorityKind::Session);
    EXPECT_EQ(snapshot.bake_generation, 0u);
    const std::array<FarTileKey, 2> expected{{{1, 0, 0}, {5, 0, 0}}};
    EXPECT_EQ(CheckFarWorldStation(snapshot, expected, 1), FarWorldStationRefusal::None);
    EXPECT_EQ(FarWorldDiagnosticJson(snapshot)["authority_kind"], "session");
    EXPECT_EQ(renderer.Selection(FarWorldPurpose::Preview).effective, FarWorldMode::Legacy);
    EXPECT_FALSE(renderer.AdvanceFrame(++frame, {0, 0, 0}, {}));
}

TEST_F(SessionRendererTest, TargetSerialPendingSourceAndRefusalsCannotQualifyOldServing) {
    ASSERT_TRUE(Stage()) << FarWorldRefusalName(renderer.Selection(FarWorldPurpose::World).refusal);
    ASSERT_TRUE(Settle());
    const auto serving = renderer.SessionDiagnostic()->state.serving_serial;
    producer.fence_source_reads(true);
    EXPECT_FALSE(Stage());
    EXPECT_EQ(renderer.Selection(FarWorldPurpose::World).refusal, FarWorldRefusal::AdmissionFailed);
    EXPECT_EQ(renderer.SessionDiagnostic()->state.serving_serial, serving);
    producer.fence_source_reads(false);
    ASSERT_TRUE(Stage()) << FarWorldRefusalName(renderer.Selection(FarWorldPurpose::World).refusal);
    ASSERT_TRUE(producer.request_capture(windows));
    EXPECT_FALSE(Stage());
    EXPECT_EQ(renderer.Selection(FarWorldPurpose::World).refusal, FarWorldRefusal::AdmissionFailed);
    EXPECT_EQ(renderer.SessionDiagnostic()->state.serving_serial, serving);
    producer.pump_capture();
    producer.pump_capture();
    const auto target = renderer.SessionDiagnostic()->state.target_serial;
    const auto complete = Tiles();
    for (std::size_t i = 0; i < complete.size(); ++i) {
        auto truncated = complete;
        truncated[i].capture.span.min_y = -256;
        EXPECT_FALSE(renderer.StageSessionTarget(
            producer, *world, truncated, policy, capture_limits, samples))
            << i;
        EXPECT_EQ(renderer.Selection(FarWorldPurpose::World).refusal, FarWorldRefusal::SpanRejected)
            << i;
        EXPECT_EQ(renderer.SessionDiagnostic()->state.serving_serial, serving) << i;
        EXPECT_EQ(renderer.SessionDiagnostic()->state.target_serial, target) << i;
    }
    ASSERT_TRUE(Stage()) << FarWorldRefusalName(renderer.Selection(FarWorldPurpose::World).refusal);
    Capture();
    const std::array<FarTileKey, 2> expected{{{1, 0, 0}, {5, 0, 0}}};
    EXPECT_NE(CheckFarWorldStation(renderer.Diagnostic(), expected, 1),
              FarWorldStationRefusal::None);
    EXPECT_EQ(renderer.Diagnostic().serving_serial, serving);
    EXPECT_GT(renderer.Diagnostic().target_serial, serving);
    auto bad = Tiles();
    bad[0].capture.span.min_y = -255;
    EXPECT_FALSE(
        renderer.StageSessionTarget(producer, *world, bad, policy, capture_limits, samples));
    EXPECT_EQ(renderer.Selection(FarWorldPurpose::World).refusal, FarWorldRefusal::SpanRejected);
    EXPECT_EQ(renderer.SessionDiagnostic()->state.serving_serial, serving);
    auto no_work = samples;
    no_work.density = 1;
    EXPECT_FALSE(
        renderer.StageSessionTarget(producer, *world, Tiles(), policy, capture_limits, no_work));
    EXPECT_EQ(renderer.Selection(FarWorldPurpose::World).refusal, FarWorldRefusal::AdmissionFailed);
    Capture();
    EXPECT_EQ(renderer.Diagnostic().source_failure.capture, FarFrozenCaptureError::WorkLimit);
    auto expired = capture_limits;
    expired.deadline = {[](const void*) noexcept -> std::uint64_t { return 1; }, nullptr, 1};
    EXPECT_FALSE(renderer.StageSessionTarget(producer, *world, Tiles(), policy, expired, samples));
    EXPECT_EQ(renderer.Selection(FarWorldPurpose::World).refusal, FarWorldRefusal::AdmissionFailed);
    Capture();
    EXPECT_EQ(renderer.Diagnostic().source_failure.capture,
              FarFrozenCaptureError::DeadlineExceeded);
    ASSERT_TRUE(Stage()) << FarWorldRefusalName(renderer.Selection(FarWorldPurpose::World).refusal);
    ASSERT_TRUE(Settle());
}

TEST_F(SessionRendererTest, FreshCompletedSerialStagesUnderQueuedAndActiveCapturesButNeverFences) {
    ASSERT_TRUE(Stage());
    for (const bool active : {false, true}) {
        SCOPED_TRACE(active);
        ASSERT_TRUE(producer.request_capture(windows));
        producer.pump_capture();
        producer.pump_capture();
        ASSERT_TRUE(producer.capture_status().latest_ready);
        const auto completed = producer.capture_status().serving_serial;
        const auto previous = renderer.SessionDiagnostic()->state.target_serial;
        ASSERT_GT(completed, previous);
        ASSERT_TRUE(producer.request_capture(windows));
        if (active) {
            producer.pump_capture();
            ASSERT_TRUE(producer.capture_status().active);
        } else {
            ASSERT_TRUE(producer.capture_status().queued);
        }
        producer.fence_source_reads(true);
        EXPECT_FALSE(Stage()); // Even a fresh completed serial cannot cross a source fence.
        EXPECT_EQ(renderer.Selection(FarWorldPurpose::World).refusal,
                  FarWorldRefusal::AdmissionFailed);
        EXPECT_EQ(renderer.SessionDiagnostic()->state.target_serial, previous);
        producer.fence_source_reads(false);
        ASSERT_TRUE(Stage());
        EXPECT_EQ(renderer.SessionDiagnostic()->state.target_serial, completed);
        EXPECT_FALSE(Stage()); // The same completed serial cannot qualify pending work again.
        EXPECT_EQ(renderer.Selection(FarWorldPurpose::World).refusal,
                  FarWorldRefusal::AdmissionFailed);
        EXPECT_EQ(renderer.SessionDiagnostic()->state.target_serial, completed);
        producer.pump_capture();
        producer.pump_capture();
        ASSERT_TRUE(producer.capture_status().latest_ready);
        ASSERT_TRUE(Stage());
        EXPECT_EQ(renderer.SessionDiagnostic()->state.target_serial,
                  producer.capture_status().serving_serial);
        EXPECT_TRUE(Stage()); // Latest-ready duplicate staging remains idempotent.
    }
}

TEST_F(SessionRendererTest,
       SessionStationChecksAcceptedIdentityPoliciesSpansKeysAndActualSubmission) {
    ASSERT_TRUE(Stage()) << FarWorldRefusalName(renderer.Selection(FarWorldPurpose::World).refusal);
    ASSERT_TRUE(Settle());
    renderer.BeginLiveOwnership(frame);
    ASSERT_TRUE(renderer.FinishLiveOwnership());
    const auto stats = renderer.Submit({0, 0, 0}, [](const auto&) { return true; });
    Capture(stats);
    const std::array<FarTileKey, 2> expected{{{1, 0, 0}, {5, 0, 0}}};
    const auto accepted = std::make_unique<FarWorldDiagnosticSnapshot>(renderer.Diagnostic());
    ASSERT_TRUE(FarWorldStationReady(*accepted, expected, 1));
    auto changed = std::make_unique<FarWorldDiagnosticSnapshot>();
    for (int fault = 0; fault < 16; ++fault) {
        *changed = *accepted;
        auto& tile = changed->tiles[0];
        switch (fault) {
            case 0:
                ++tile.accepted_content_identity;
                break;
            case 1:
                ++tile.desired.world_epoch;
                break;
            case 2:
                ++tile.desired.bake_generation;
                break;
            case 3:
                tile.desired.cave_policy = 1;
                break;
            case 4:
                tile.numeric = FarVolumeNumericProfile::LegacySigned16;
                break;
            case 5:
                ++tile.request.field_identity;
                break;
            case 6:
                tile.request.span = {0, 16};
                break;
            case 7:
                tile.water_complete = false;
                break;
            case 8:
                --changed->tile_count;
                break;
            case 9:
                changed->target_serial = 0;
                break;
            case 10:
                changed->latest_target_ready = false;
                break;
            case 11:
                changed->authority_kind = FarWorldAuthorityKind::Fixture;
                break;
            case 12:
                changed->numeric = static_cast<FarVolumeNumericProfile>(255);
                break;
            case 13:
                ++tile.request.span.max_y;
                break;
            case 14:
                tile.request.span.min_y = -16777216;
                break;
            case 15:
                changed->submitted_water_indices = 0;
                break;
        }
        EXPECT_FALSE(FarWorldStationReady(*changed, expected, 1)) << fault;
    }
    const std::array<FarTileKey, 2> duplicates{{{1, 0, 0}, {1, 0, 0}}};
    EXPECT_FALSE(FarWorldStationReady(*accepted, duplicates, 1));
    state.fail_draw = true;
    Capture(renderer.Submit({0, 0, 0}, [](const auto&) { return true; }));
    EXPECT_FALSE(FarWorldStationReady(renderer.Diagnostic(), expected, 1));
    EXPECT_EQ(renderer.Diagnostic().draws, 0u);
    EXPECT_GT(renderer.Diagnostic().tiers[4].resident.retained_gpu_bytes, 0u);
}

TEST(FarWorldRenderer, EveryProducerAndCaptureRefusalHasAnExactDiagnosticProjection) {
    using E = FarWorldAuthorityProducer::Error;
    using C = FarFrozenCaptureError;
    using R = FarWorldRefusal;
    const std::array source{std::pair{E::None, R::None},
                            std::pair{E::InvalidLimits, R::MissingLimits},
                            std::pair{E::InvalidWindow, R::SpanRejected},
                            std::pair{E::IndexCapacity, R::AdmissionFailed},
                            std::pair{E::CopyLimit, R::AdmissionFailed},
                            std::pair{E::AuthorityUnavailable, R::ImmutableAuthorityUnavailable},
                            std::pair{E::WaterUnavailable, R::WaterUnavailable},
                            std::pair{E::AllocationFailure, R::AdmissionFailed},
                            std::pair{E::DeadlineExceeded, R::AdmissionFailed},
                            std::pair{E::CommittedReadRefused, R::AdmissionFailed},
                            std::pair{E::SourceReadExpired, R::AdmissionFailed}};
    for (const auto& [error, expected] : source) {
        EXPECT_EQ(FarWorldProducerRefusal({error, C::None}), expected);
    }
    const std::array capture{std::pair{C::None, R::None},
                             std::pair{C::InvalidSource, R::ImmutableAuthorityUnavailable},
                             std::pair{C::InvalidRegion, R::SpanRejected},
                             std::pair{C::CoordinateRange, R::SpanRejected},
                             std::pair{C::WorkLimit, R::AdmissionFailed},
                             std::pair{C::ByteLimit, R::AdmissionFailed},
                             std::pair{C::Unavailable, R::ImmutableAuthorityUnavailable},
                             std::pair{C::NonFiniteSample, R::ImmutableAuthorityUnavailable},
                             std::pair{C::IdentityMismatch, R::ImmutableAuthorityUnavailable},
                             std::pair{C::AllocationFailure, R::AdmissionFailed},
                             std::pair{C::DeadlineExceeded, R::AdmissionFailed}};
    for (const auto& [error, expected] : capture) {
        EXPECT_EQ(FarWorldProducerRefusal({E::None, error}), expected);
        EXPECT_EQ(FarWorldProducerRefusal({E::WaterUnavailable, error}), R::WaterUnavailable);
    }
    EXPECT_EQ(FarWorldProducerRefusal({static_cast<E>(255), C::None}), R::InvalidRequests);
    EXPECT_EQ(FarWorldProducerRefusal({E::None, static_cast<C>(255)}), R::InvalidRequests);
}

TEST(FarWorldHandoffRenderer, SessionImageLivesThroughHeldWorkerBackendTransfer) {
    JobSystem jobs;
    jobs.startup(1);
    DriverState state;
    BuildGate barrier;
    RendererHandoffGate gate(barrier);
    FarWorldHandoffRenderer renderer;
    struct ReleaseHeldWorker {
        BuildGate& barrier;
        ~ReleaseHeldWorker() {
            barrier.release.count_down();
        }
    };
    renderer.InstallBackend(std::make_unique<Driver>(state));
    ASSERT_TRUE(renderer.Bind(jobs,
                              1,
                              FarCaveMode::BandLimited,
                              FarVolumeNumericProfile::RawFloat,
                              RendererHandoffBudgets(),
                              RendererHandoffLimits(),
                              &gate));
    auto input = RendererHandoffInput(renderer,
                                      {5, 0, 0},
                                      1,
                                      true,
                                      FarCaveMode::BandLimited,
                                      FarVolumeNumericProfile::RawFloat,
                                      FarFrozenUse::SessionRender);
    ASSERT_TRUE(input.observation);
    state.session_image = input.observation;
    ASSERT_TRUE(renderer.StageTarget(RendererHandoffManifest(1, {&input, 1})));
    input = {};
    gate.Arm();
    ReleaseHeldWorker release{barrier};
    ASSERT_TRUE(renderer.AdvanceFrame(1, {0, 0, 0}, {1, 1, 64ULL * 1024 * 1024}));
    ASSERT_TRUE(RendererHandoffWait([&] { return barrier.entered.try_wait(); }));
    state.release_on_drain = &barrier;
    auto backend = renderer.TakeBackend();
    EXPECT_TRUE(state.session_image_alive_during_drain);
    EXPECT_TRUE(barrier.exited.try_wait());
    EXPECT_TRUE(state.session_image.expired());
    EXPECT_EQ(state.held_build_drains, 1u);
    EXPECT_EQ(renderer.Handoff(), nullptr);
    EXPECT_EQ(state.live, 0u);
    state.fixture_released = true;
    renderer.InstallBackend(std::move(backend));
    renderer.Shutdown();
    jobs.shutdown();
}

TEST_F(SessionRendererTest, BackendAndFixtureTransitionsDrainSessionBeforeRebinding) {
    ASSERT_TRUE(Stage()) << FarWorldRefusalName(renderer.Selection(FarWorldPurpose::World).refusal);
    ASSERT_TRUE(Settle());
    renderer.PrepareWorldSwap();
    EXPECT_EQ(renderer.SessionDiagnostic(), nullptr);
    EXPECT_EQ(state.live, 0u);
    auto fixture = std::make_shared<Fixture>(state);
    ASSERT_EQ(renderer.BindFixture(jobs, fixture, 2, 1, limits), FarWorldRefusal::None);
    fixture.reset();
    ASSERT_EQ(renderer.BindSession(jobs, 3, policy.caves, policy.numeric, limits),
              FarWorldRefusal::None);
    EXPECT_TRUE(state.fixture_released);
    EXPECT_EQ(renderer.Residency(), nullptr);
    EXPECT_EQ(renderer.Selection(FarWorldPurpose::World).refusal,
              FarWorldRefusal::ImmutableAuthorityUnavailable);
    renderer.SetRequestedCavePolicy(FarCaveMode::BoxFiltered);
    EXPECT_EQ(renderer.SessionDiagnostic(), nullptr);
    EXPECT_EQ(renderer.BindSession(jobs, 4, policy.caves, policy.numeric, limits),
              FarWorldRefusal::CavePolicyMismatch);
}

TEST(FarWorldRenderer, StationRejectsUnknownNumericPolicyAndRepeatedExpectedKeys) {
    Owner owner;
    const auto empty = Request({5, 1, 1});
    ASSERT_TRUE(owner.WaitSettled(empty));
    owner.Renderer().CaptureDiagnostic(owner.Renderer().Selection(FarWorldPurpose::World),
                                       owner.Frame(),
                                       {0, 0, 0},
                                       kFarPlaneMeters,
                                       true,
                                       0,
                                       0);
    auto snapshot = std::make_unique<FarWorldDiagnosticSnapshot>(owner.Renderer().Diagnostic());
    ASSERT_TRUE(FarWorldStationReady(*snapshot, {&empty.identity.key, 1}, 1));
    snapshot->numeric = static_cast<FarVolumeNumericProfile>(255);
    EXPECT_EQ(CheckFarWorldStation(*snapshot, {&empty.identity.key, 1}, 1),
              FarWorldStationRefusal::InvalidBinding);
    snapshot->numeric = FarVolumeNumericProfile::LegacySigned16;
    snapshot->tiles[1] = snapshot->tiles[0];
    ++snapshot->tiles[1].desired.key.tx;
    ++snapshot->tiles[1].built.key.tx;
    snapshot->tile_count = 2;
    auto& counters = snapshot->tiers[4].resident;
    counters.wanted = counters.covering = counters.settled = counters.empty_pairs =
        counters.water_known_dry_tiles = 2;
    const std::array<FarTileKey, 2> duplicate{{empty.identity.key, empty.identity.key}};
    EXPECT_EQ(CheckFarWorldStation(*snapshot, duplicate, 1),
              FarWorldStationRefusal::ExpectedSetMismatch);
}

TEST_F(SessionRendererTest, SessionProjectionRefusesMalformedBoundsAndStaleFrame) {
    ASSERT_TRUE(Stage()) << FarWorldRefusalName(renderer.Selection(FarWorldPurpose::World).refusal);
    ASSERT_TRUE(Settle());
    Capture();
    auto input = std::make_unique<FarWorldHandoffDiagnosticSnapshot>(*renderer.SessionDiagnostic());
    auto output = std::make_unique<FarWorldDiagnosticSnapshot>(renderer.Diagnostic());
    const auto tiles = input->tile_count;
    input->tile_count = input->tiles.size() + 1;
    ProjectFarWorldSessionDiagnostic(*input, policy.numeric, *output);
    EXPECT_FALSE(output->complete);
    EXPECT_FALSE(output->latest_target_ready);
    input->tile_count = tiles;
    input->draw_count = input->draws.size() + 1;
    *output = renderer.Diagnostic();
    ProjectFarWorldSessionDiagnostic(*input, policy.numeric, *output);
    EXPECT_FALSE(output->complete);
    input->draw_count = 0;
    *output = renderer.Diagnostic();
    ++output->frame;
    ProjectFarWorldSessionDiagnostic(*input, policy.numeric, *output);
    EXPECT_FALSE(output->advanced);
    EXPECT_EQ(output->draws, 0u);
    *output = renderer.Diagnostic();
    ++output->world_epoch;
    ProjectFarWorldSessionDiagnostic(*input, policy.numeric, *output);
    EXPECT_FALSE(output->complete);
}

TEST_F(SessionRendererTest, SessionProjectionKeepsLegacyFallbackTotals) {
    ASSERT_TRUE(Stage()) << FarWorldRefusalName(renderer.Selection(FarWorldPurpose::World).refusal);
    ASSERT_TRUE(Settle());
    Capture();
    auto input = std::make_unique<FarWorldHandoffDiagnosticSnapshot>(*renderer.SessionDiagnostic());
    auto output = std::make_unique<FarWorldDiagnosticSnapshot>(renderer.Diagnostic());
    input->submission.draws = 3;
    input->submission.indices = 9;
    input->submission.water_indices = 4;
    output->selection.effective = FarWorldMode::Legacy;
    output->draws = 7;
    output->indices = 21;
    output->submitted_water_indices = 0;
    output->submission_occurred = true;
    ProjectFarWorldSessionDiagnostic(*input, policy.numeric, *output);
    EXPECT_EQ(output->draws, 7u);
    EXPECT_EQ(output->indices, 21u);
    EXPECT_EQ(output->submitted_water_indices, 0u);
    EXPECT_TRUE(output->submission_occurred);
    output->selection.effective = FarWorldMode::Ladder;
    output->selection.submit = true;
    output->frame = input->frame;
    ProjectFarWorldSessionDiagnostic(*input, policy.numeric, *output);
    EXPECT_EQ(output->draws, 3u);
    EXPECT_EQ(output->indices, 9u);
    EXPECT_EQ(output->submitted_water_indices, 4u);
    EXPECT_TRUE(output->submission_occurred);
}

TEST_F(SessionRendererTest, SessionAdmissionKeepsTypedBindingAndInputRefusals) {
    auto invalid = policy;
    invalid.caves = FarCaveMode::BoxFiltered;
    EXPECT_FALSE(
        renderer.StageSessionTarget(producer, *world, Tiles(), invalid, capture_limits, samples));
    EXPECT_EQ(renderer.Selection(FarWorldPurpose::World).refusal,
              FarWorldRefusal::CavePolicyMismatch);
    invalid = policy;
    invalid.numeric = FarVolumeNumericProfile::LegacySigned16;
    EXPECT_FALSE(
        renderer.StageSessionTarget(producer, *world, Tiles(), invalid, capture_limits, samples));
    EXPECT_EQ(renderer.Selection(FarWorldPurpose::World).refusal, FarWorldRefusal::InvalidRequests);
    EXPECT_FALSE(
        renderer.StageSessionTarget(producer, *world, {}, policy, capture_limits, samples));
    EXPECT_EQ(renderer.Selection(FarWorldPurpose::World).refusal, FarWorldRefusal::InvalidRequests);
    auto incomplete = capture_limits;
    incomplete.max_bytes = 0;
    EXPECT_FALSE(
        renderer.StageSessionTarget(producer, *world, Tiles(), policy, incomplete, samples));
    EXPECT_EQ(renderer.Selection(FarWorldPurpose::World).refusal, FarWorldRefusal::MissingLimits);
    ASSERT_TRUE(Stage()) << FarWorldRefusalName(renderer.Selection(FarWorldPurpose::World).refusal);
    EXPECT_EQ(renderer.BindSession(jobs, 0, policy.caves, policy.numeric, limits),
              FarWorldRefusal::InvalidRequests);
    EXPECT_EQ(renderer.BindSession(jobs, 1, static_cast<FarCaveMode>(255), policy.numeric, limits),
              FarWorldRefusal::InvalidCavePolicy);
    EXPECT_EQ(renderer.BindSession(
                  jobs, 1, policy.caves, static_cast<FarVolumeNumericProfile>(255), limits),
              FarWorldRefusal::InvalidRequests);
    auto no_limits = limits;
    no_limits.frame.builds = 0;
    EXPECT_EQ(renderer.BindSession(jobs, 1, policy.caves, policy.numeric, no_limits),
              FarWorldRefusal::MissingLimits);
    auto tiny = limits;
    tiny.budgets.bytes.fill(1);
    EXPECT_EQ(renderer.BindSession(jobs, 1, policy.caves, policy.numeric, tiny),
              FarWorldRefusal::AdmissionFailed);
    state.current = false;
    EXPECT_EQ(renderer.BindSession(jobs, 1, policy.caves, policy.numeric, limits),
              FarWorldRefusal::BackendUnavailable);
    state.current = true;
    renderer.SetRequestedCavePolicy(std::nullopt);
    EXPECT_EQ(renderer.BindSession(jobs, 1, policy.caves, policy.numeric, limits),
              FarWorldRefusal::InvalidCavePolicy);
    EXPECT_FALSE(Stage());
    EXPECT_EQ(renderer.Selection(FarWorldPurpose::World).refusal,
              FarWorldRefusal::InvalidCavePolicy);
}

namespace {
class HandoffAdmissionTest : public testing::Test {
protected:
    JobSystem jobs;
    DriverState state;
    FarWorldHandoffRenderer renderer;
    void SetUp() override {
        renderer.InstallBackend(std::make_unique<Driver>(state));
        auto residency = RendererHandoffLimits();
        // Exercise the maximum manifest without allocating frozen images for
        // invalid descriptors: admission must yield before a later refusal.
        residency.entry_slots.back() = FarWorldAuthorityProducer::kSlots;
        residency.queues.back().descriptor_slots = FarWorldAuthorityProducer::kSlots;
        ASSERT_TRUE(renderer.Bind(jobs,
                                  1,
                                  FarCaveMode::BandLimited,
                                  FarVolumeNumericProfile::RawFloat,
                                  RendererHandoffBudgets(),
                                  residency));
    }
    void TearDown() override {
        renderer.PrepareWorldSwap();
        state.fixture_released = true;
        renderer.Shutdown();
    }
    std::uint64_t Complete(std::uint64_t budget) {
        std::uint64_t columns = 0;
        // Two tiles need fewer than 34,000 coverage units at budget one.
        for (std::size_t pump = 0; pump < 34'000; ++pump) {
            const auto result = renderer.PumpAdmission(budget);
            EXPECT_LE(result.work, budget);
            EXPECT_LE(result.height_observations, result.work);
            columns += result.height_observations;
            if (result.status != FarHandoffAdmissionStatus::Pending) {
                EXPECT_EQ(result.status, FarHandoffAdmissionStatus::Complete);
                return columns;
            }
        }
        ADD_FAILURE() << "Admission failed to terminate within its column bound";
        return columns;
    }
};
} // namespace

TEST_F(HandoffAdmissionTest, ColumnBudgetsPreservePrivateInputsAndSynchronousTileBytes) {
    const std::array inputs{RendererHandoffInput(renderer, {5, 0, 0}, 1, false),
                            RendererHandoffInput(renderer, {5, 1, 0}, 1, false)};
    auto manifest = RendererHandoffManifest(1, inputs);
    ASSERT_TRUE(renderer.StageTarget(manifest));
    const auto expected = FarWorldHandoffDiagnosticJson(renderer.Diagnostic())["tiles"].dump();
    std::array<FarVolumeRequest, 2> resolved{};
    for (std::size_t i = 0; i < inputs.size(); ++i) {
        const auto request =
            renderer.Handoff()->TargetCoverageRequest(inputs[i].wanted.identity.key);
        ASSERT_TRUE(request);
        if (request) {
            resolved[i] = *request;
            EXPECT_EQ(request->span, (FarVolumeSpan{-256, 256}));
        }
    }
    for (const std::uint64_t budget : {1u, 7u, 64u}) {
        ++manifest.publication_serial;
        ASSERT_EQ(renderer.StartTarget(manifest), FarHandoffAdmissionStatus::Pending);
        EXPECT_EQ(renderer.PumpAdmission(0).work, 0u);
        const auto before = renderer.Handoff()->State().target_serial;
        const auto first = renderer.PumpAdmission(budget);
        EXPECT_LE(first.work, budget);
        EXPECT_LE(first.height_observations, first.work);
        EXPECT_EQ(first.status, FarHandoffAdmissionStatus::Pending);
        EXPECT_EQ(renderer.Handoff()->State().target_serial, before);
        EXPECT_EQ(renderer.Handoff()->State().serving_serial, 0u);
        EXPECT_EQ(FarWorldHandoffDiagnosticJson(renderer.Diagnostic())["tiles"].dump(), expected);
        EXPECT_EQ(first.height_observations + Complete(budget), 2u * 129u * 129u);
        EXPECT_EQ(renderer.Handoff()->State().target_serial, manifest.publication_serial);
        for (std::size_t i = 0; i < inputs.size(); ++i) {
            EXPECT_EQ(renderer.Handoff()->TargetCoverageRequest(inputs[i].wanted.identity.key),
                      std::optional{resolved[i]});
        }
        EXPECT_EQ(FarWorldHandoffDiagnosticJson(renderer.Diagnostic())["tiles"].dump(), expected);
    }
    // Inputs are retained independently of the caller's manifest storage.
    auto owned = inputs;
    ++manifest.publication_serial;
    manifest.tiles = owned;
    ASSERT_EQ(renderer.StartTarget(manifest), FarHandoffAdmissionStatus::Pending);
    owned = {};
    EXPECT_EQ(Complete(64), 2u * 129u * 129u);
}

TEST_F(HandoffAdmissionTest, StartDefersManifestScansAndSortToBudgetedPumps) {
    const std::array inputs{RendererHandoffInput(renderer, {5, 2, 0}, 1, false),
                            RendererHandoffInput(renderer, {5, 1, 0}, 1, false),
                            RendererHandoffInput(renderer, {5, 0, 0}, 1, false)};
    auto manifest = RendererHandoffManifest(1, inputs);
    ASSERT_EQ(renderer.StartTarget(manifest), FarHandoffAdmissionStatus::Pending);
    const auto* handoff = renderer.Handoff();
    ASSERT_NE(handoff, nullptr);
    EXPECT_EQ(handoff->AdmissionWork().descriptor_visits, 0u);
    EXPECT_EQ(handoff->AdmissionWork().key_comparisons, 0u);
    EXPECT_EQ(renderer.PumpAdmission(0).work, 0u);
    EXPECT_EQ(handoff->AdmissionWork().descriptor_visits, 0u);
    EXPECT_EQ(handoff->AdmissionWork().key_comparisons, 0u);
    // Input indexing and all comparisons must be charged before coverage begins.
    std::uint64_t columns = 0;
    for (std::size_t pump = 0; pump < 1024; ++pump) {
        const auto before = handoff->AdmissionWork();
        const auto result = renderer.PumpAdmission(1);
        EXPECT_EQ(result.work, 1u);
        EXPECT_EQ(result.status, FarHandoffAdmissionStatus::Pending);
        // Frame preparation must not overwrite the pending sort's scratch.
        renderer.AdvanceFrame(pump, {0, 0, 0}, {});
        const auto after = handoff->AdmissionWork();
        EXPECT_LE(after.descriptor_visits - before.descriptor_visits, result.work);
        EXPECT_LE(after.key_comparisons - before.key_comparisons, result.work);
        columns += result.height_observations;
        if (columns != 0) {
            break;
        }
    }
    ASSERT_EQ(columns, 1u);
    EXPECT_EQ(columns + Complete(64), 3u * 129u * 129u);
    const auto expected = FarWorldHandoffDiagnosticJson(renderer.Diagnostic())["tiles"].dump();
    auto sorted = inputs;
    std::reverse(sorted.begin(), sorted.end());
    // Reordering and republishing retain the observations' committed generation.
    manifest.publication_serial = 2;
    manifest.tiles = sorted;
    ASSERT_TRUE(renderer.StageTarget(manifest));
    EXPECT_EQ(FarWorldHandoffDiagnosticJson(renderer.Diagnostic())["tiles"].dump(), expected);
    // A published target makes frame preparation actively overwrite its own
    // ordering buffer between merge emissions and the eventual publication.
    manifest.publication_serial = 3;
    manifest.tiles = inputs;
    ASSERT_EQ(renderer.StartTarget(manifest), FarHandoffAdmissionStatus::Pending);
    columns = 0;
    for (std::size_t pump = 0; pump < 1024; ++pump) {
        const auto result = renderer.PumpAdmission(1);
        ASSERT_EQ(result.status, FarHandoffAdmissionStatus::Pending);
        EXPECT_EQ(result.work, 1u);
        renderer.AdvanceFrame(1024 + pump, {0, 0, 0}, {});
        columns += result.height_observations;
        if (columns != 0) {
            break;
        }
    }
    ASSERT_EQ(columns, 1u);
    EXPECT_EQ(columns + Complete(64), 3u * 129u * 129u);
    EXPECT_EQ(FarWorldHandoffDiagnosticJson(renderer.Diagnostic())["tiles"].dump(), expected);
    // Duplicate serials still validate the full set, without observing heights.
    ASSERT_EQ(renderer.StartTarget(manifest), FarHandoffAdmissionStatus::Pending);
    EXPECT_EQ(Complete(64), 0u);
    auto incomplete = manifest;
    incomplete.tiles = incomplete.tiles.first(2);
    ASSERT_EQ(renderer.StartTarget(incomplete), FarHandoffAdmissionStatus::Pending);
    FarHandoffAdmissionPump refused;
    for (std::size_t pump = 0; pump < 1024; ++pump) {
        refused = renderer.PumpAdmission(1);
        EXPECT_EQ(refused.height_observations, 0u);
        if (refused.status != FarHandoffAdmissionStatus::Pending) {
            break;
        }
    }
    EXPECT_EQ(refused.status, FarHandoffAdmissionStatus::Refused);
    EXPECT_EQ(renderer.AdmissionRefusal(), FarWorldRefusal::InvalidRequests);
    EXPECT_EQ(renderer.Handoff()->State().target_serial, 3u);
}

TEST_F(HandoffAdmissionTest, BudgetedPreparationRefusesInvalidSetsWithoutPublication) {
    const auto input = RendererHandoffInput(renderer, {5, 0, 0}, 1, false);
    for (int invalid = 0; invalid < 6; ++invalid) {
        SCOPED_TRACE(invalid);
        std::array tiles{input, input};
        auto manifest = RendererHandoffManifest(1, tiles);
        auto expected = FarWorldRefusal::InvalidRequests;
        if (invalid == 1) {
            tiles[1].observation.reset();
            expected = FarWorldRefusal::ImmutableAuthorityUnavailable;
        } else if (invalid == 2) {
            tiles[1].wanted.identity.key.tier = 0;
        } else if (invalid == 3) {
            tiles[1].wanted.identity.world_epoch = 0;
        } else if (invalid == 4) {
            tiles[1].wanted.identity.cave_policy = 255;
        } else if (invalid == 5) {
            tiles[0].wanted.identity.key.tier = 1;
            ASSERT_TRUE(
                ToFarVolumeTileKey(tiles[0].wanted.identity.key, tiles[0].wanted.intent.key));
            manifest.tiles = {tiles.data(), 1}; // No coarsest fallback.
        }
        ASSERT_EQ(renderer.StartTarget(manifest), FarHandoffAdmissionStatus::Pending);
        FarHandoffAdmissionPump result;
        for (std::size_t pump = 0; pump < 1024; ++pump) {
            result = renderer.PumpAdmission(1);
            EXPECT_LE(result.work, 1u);
            EXPECT_EQ(result.height_observations, 0u);
            if (result.status != FarHandoffAdmissionStatus::Pending) {
                break;
            }
        }
        EXPECT_EQ(result.status, FarHandoffAdmissionStatus::Refused);
        EXPECT_EQ(renderer.AdmissionRefusal(), expected);
        EXPECT_EQ(renderer.Handoff()->State().target_serial, 0u);
    }
}

TEST_F(HandoffAdmissionTest, PerTierCapacityRefusesBeforeSortingOrCoverage) {
    std::array<FarHandoffTileInput, FarWorldAuthorityProducer::kSlots + 1> inputs{};
    inputs.fill(RendererHandoffInput(renderer, {5, 0, 0}, 1, false));
    ASSERT_EQ(renderer.StartTarget(RendererHandoffManifest(1, inputs)),
              FarHandoffAdmissionStatus::Pending);
    FarHandoffAdmissionPump result;
    for (std::size_t index = 0; index < inputs.size(); ++index) {
        result = renderer.PumpAdmission(1);
        EXPECT_EQ(result.work, 1u);
        EXPECT_EQ(result.height_observations, 0u);
    }
    EXPECT_EQ(result.status, FarHandoffAdmissionStatus::Refused);
    EXPECT_EQ(renderer.AdmissionRefusal(), FarWorldRefusal::AdmissionFailed);
    EXPECT_EQ(renderer.Handoff()->State().target_serial, 0u);
}

TEST_F(HandoffAdmissionTest, MaximumManifestYieldsAtColumnsBeforeLaterInputRefusal) {
    std::array<FarHandoffTileInput, FarWorldAuthorityProducer::kSlots> inputs{};
    inputs.fill(RendererHandoffInput(renderer, {5, 0, 0}, 1, false));
    for (std::size_t index = 1; index < inputs.size(); ++index) {
        const FarTileKey key{5, static_cast<std::int64_t>(index), 0};
        inputs[index].wanted.identity.key = key;
        ASSERT_TRUE(ToFarVolumeTileKey(key, inputs[index].wanted.intent.key));
    }
    ASSERT_EQ(renderer.StartTarget(RendererHandoffManifest(1, inputs)),
              FarHandoffAdmissionStatus::Pending);
    EXPECT_EQ(renderer.Handoff()->AdmissionWork().descriptor_visits, 0u);
    EXPECT_EQ(renderer.Handoff()->AdmissionWork().key_comparisons, 0u);
    std::uint64_t columns = 0;
    for (std::size_t pump = 0; pump < 300; ++pump) {
        const auto result = renderer.PumpAdmission(64);
        EXPECT_LE(result.work, 64u);
        EXPECT_LE(result.height_observations, result.work);
        columns += result.height_observations;
        EXPECT_EQ(renderer.Handoff()->State().target_serial, 0u);
        EXPECT_EQ(renderer.Handoff()->State().serving_serial, 0u);
        if (result.status != FarHandoffAdmissionStatus::Pending) {
            EXPECT_EQ(result.status, FarHandoffAdmissionStatus::Refused);
            EXPECT_EQ(renderer.AdmissionRefusal(), FarWorldRefusal::WaterUnavailable);
            EXPECT_EQ(columns, 129u * 129u);
            EXPECT_EQ(renderer.Diagnostic().tile_count, 0u);
            return;
        }
    }
    FAIL() << "Maximum manifest did not refuse its invalid second image";
}

TEST_F(HandoffAdmissionTest, CancelReleasesChargesAndRetainsHistoryUntilRestartFinishes) {
    auto first = std::array{RendererHandoffInput(renderer, {5, -1, 0}, 1, false)};
    auto next = std::array{RendererHandoffInput(renderer, {5, 1, 0}, 2, false)};
    ASSERT_TRUE(renderer.StageTarget(RendererHandoffManifest(1, first)));
    ASSERT_TRUE(renderer.StageTarget(RendererHandoffManifest(2, next)));
    auto returned = std::array{RendererHandoffInput(renderer, {5, -1, 0}, 3, false)};
    returned[0].wanted.intent.extra_span = FarVolumeCoverageSpan{-128, 64};
    const auto pool = renderer.MemoryPool(kFarTierCount);
    const auto baseline = pool->used();
    ASSERT_EQ(renderer.StartTarget(RendererHandoffManifest(3, returned)),
              FarHandoffAdmissionStatus::Pending);
    EXPECT_GT(pool->used(), baseline);
    EXPECT_EQ(renderer.PumpAdmission(64).status, FarHandoffAdmissionStatus::Pending);
    EXPECT_EQ(renderer.RetireIdleHistory(), 0u);
    ASSERT_TRUE(renderer.Diagnostic().complete);
    ASSERT_EQ(renderer.Diagnostic().tile_count, 2u);
    // Complete diagnostics retain the idle key that the transaction references.
    EXPECT_EQ(renderer.Diagnostic().tiles[0].key, first[0].wanted.identity.key);
    EXPECT_EQ(renderer.CancelAdmission(), FarHandoffAdmissionStatus::Idle);
    EXPECT_EQ(pool->used(), baseline);
    EXPECT_EQ(renderer.Handoff()->State().target_serial, 2u);
    ASSERT_EQ(renderer.StartTarget(RendererHandoffManifest(3, returned)),
              FarHandoffAdmissionStatus::Pending);
    FarHandoffAdmissionPump refused;
    for (std::size_t pump = 0; pump < 300; ++pump) {
        refused = renderer.PumpAdmission(64);
        if (refused.status != FarHandoffAdmissionStatus::Pending) {
            break;
        }
    }
    EXPECT_EQ(refused.status, FarHandoffAdmissionStatus::Refused);
    EXPECT_EQ(renderer.AdmissionRefusal(), FarWorldRefusal::SpanRejected);
    EXPECT_EQ(pool->used(), baseline);
    EXPECT_EQ(renderer.RetireIdleHistory(), 1u);
    ASSERT_EQ(renderer.StartTarget(RendererHandoffManifest(3, returned)),
              FarHandoffAdmissionStatus::Pending);
    EXPECT_EQ(Complete(64), 129u * 129u);
    EXPECT_EQ(renderer.Handoff()->State().target_serial, 3u);
}

TEST_F(HandoffAdmissionTest, MetadataAndLateRefusalsRemainAtomicAndRestartable) {
    const std::array input{RendererHandoffInput(renderer, {5, 0, 0}, 1, false)};
    auto manifest = RendererHandoffManifest(1, input);
    for (int invalid = 0; invalid < 6; ++invalid) {
        auto bad = manifest;
        auto tiles = input;
        bad.tiles = tiles;
        switch (invalid) {
            case 0:
                bad.world_epoch = 0;
                break;
            case 1:
                bad.publication_serial = 0;
                break;
            case 2:
                bad.caves = static_cast<FarCaveMode>(255);
                break;
            case 3:
                bad.numeric = static_cast<FarVolumeNumericProfile>(255);
                break;
            case 4:
                bad.tiles = {};
                break;
            case 5:
                tiles[0].observation.reset();
                break;
        }
        if (invalid == 5) {
            ASSERT_EQ(renderer.StartTarget(bad), FarHandoffAdmissionStatus::Pending);
            EXPECT_EQ(renderer.PumpAdmission(1).status, FarHandoffAdmissionStatus::Refused);
        } else {
            EXPECT_EQ(renderer.StartTarget(bad), FarHandoffAdmissionStatus::Refused);
        }
        EXPECT_EQ(renderer.AdmissionStatus(), FarHandoffAdmissionStatus::Refused);
        EXPECT_EQ(renderer.PumpAdmission(1).work, 0u);
        EXPECT_EQ(renderer.Handoff()->State().target_serial, 0u);
    }
    ASSERT_EQ(renderer.StartTarget(manifest), FarHandoffAdmissionStatus::Pending);
    EXPECT_EQ(Complete(7), 129u * 129u);
    auto retired = manifest;
    retired.publication_serial = 2;
    retired.committed_generation = 0;
    EXPECT_EQ(renderer.StartTarget(retired), FarHandoffAdmissionStatus::Refused);
    EXPECT_EQ(renderer.AdmissionRefusal(), FarWorldRefusal::ImmutableAuthorityUnavailable);
    EXPECT_EQ(renderer.Handoff()->State().target_serial, 1u);
}

TEST(FarWorldRenderer, BoundedAdmissionIsTypedNotApplicableWithoutSessionOrInLegacy) {
    for (const auto mode : {FarWorldMode::Legacy, FarWorldMode::Ladder}) {
        FarWorldRenderer renderer(mode);
        EXPECT_EQ(renderer.StartSessionTarget({}, {}, {}, {}, {}, {}),
                  FarHandoffAdmissionStatus::NotApplicable);
        EXPECT_EQ(renderer.PumpAdmission(1).status, FarHandoffAdmissionStatus::NotApplicable);
        EXPECT_EQ(renderer.CancelAdmission(), FarHandoffAdmissionStatus::NotApplicable);
        EXPECT_EQ(renderer.AdmissionStatus(), FarHandoffAdmissionStatus::NotApplicable);
    }
    FarWorldHandoffRenderer renderer;
    EXPECT_EQ(renderer.StartTarget({}), FarHandoffAdmissionStatus::NotApplicable);
    EXPECT_EQ(renderer.StartSessionTarget({}, {}, {}, {}, {}, {}),
              FarHandoffAdmissionStatus::NotApplicable);
    EXPECT_EQ(renderer.PumpAdmission(1).status, FarHandoffAdmissionStatus::NotApplicable);
    EXPECT_EQ(renderer.CancelAdmission(), FarHandoffAdmissionStatus::NotApplicable);
    EXPECT_EQ(renderer.AdmissionStatus(), FarHandoffAdmissionStatus::NotApplicable);
}

namespace {
class SessionAdmissionTest : public SessionRendererTest {
protected:
    // Declared before the producer so every capture clock borrower dies first.
    std::uint64_t capture_now = 0;
    std::shared_ptr<Producer> source;
    std::shared_ptr<const Systems::SHIELD_WorldSystem> sampling_world;
    void SetUp() override {
        SessionRendererTest::SetUp();
        if (HasFatalFailure()) {
            return;
        }
        sampling_world = std::move(world);
        source = std::make_shared<Producer>(
            Producer::Limits{512,
                             4u * 1024u * 1024u,
                             300'000,
                             std::make_shared<FarVolumeMemoryPool>(64u * 1024u * 1024u)});
        const Save::AuthorityReadLimits reads{
            512, 512, 1024, 8192, 4194304, 16777216, 4194304, 16777216, 131072};
        std::string error;
        ASSERT_TRUE(source->admit_saved_source(root, reads, error)) << error;
        const Producer::CaptureClock clock{
            [](const void*) noexcept -> std::uint64_t { return 0; }, nullptr, 1};
        ASSERT_TRUE(source->configure_capture({reads, 4, clock}));
        ASSERT_TRUE(source->request_capture(windows));
        source->pump_capture();
        source->pump_capture();
        ASSERT_TRUE(source->capture_status().latest_ready);
    }
    FarHandoffAdmissionStatus Start(const FarFrozenCaptureLimits& capture) {
        return renderer.StartSessionTarget(
            source, sampling_world, Tiles(), policy, capture, samples);
    }
    bool ReachAdmission() {
        for (std::size_t pump = 0; pump < 10'000; ++pump) {
            const auto result = renderer.PumpAdmission(4096);
            EXPECT_LE(result.work, 4096u);
            if (result.height_observations != 0) {
                EXPECT_EQ(result.status, FarHandoffAdmissionStatus::Pending);
                return true;
            }
            if (result.status != FarHandoffAdmissionStatus::Pending) {
                return false;
            }
        }
        return false;
    }
    bool CompleteSession() {
        for (std::size_t pump = 0; pump < 10'000; ++pump) {
            const auto result = renderer.PumpAdmission(4096);
            EXPECT_LE(result.work, 4096u);
            EXPECT_LE(result.height_observations, result.work);
            if (result.status != FarHandoffAdmissionStatus::Pending) {
                return result.status == FarHandoffAdmissionStatus::Complete;
            }
        }
        return false;
    }
};
} // namespace

TEST_F(SessionAdmissionTest, PrivateAdmissionAndCancellationKeepPublishedSelection) {
    EXPECT_EQ(renderer.Selection(FarWorldPurpose::World).effective, FarWorldMode::Legacy);
    ASSERT_EQ(Start(capture_limits), FarHandoffAdmissionStatus::Pending);
    EXPECT_EQ(renderer.Selection(FarWorldPurpose::World).effective, FarWorldMode::Legacy);
    EXPECT_EQ(renderer.Selection(FarWorldPurpose::World).refusal,
              FarWorldRefusal::ImmutableAuthorityUnavailable);
    ASSERT_TRUE(ReachAdmission());
    EXPECT_EQ(renderer.Selection(FarWorldPurpose::World).effective, FarWorldMode::Legacy);
    EXPECT_EQ(renderer.CancelAdmission(), FarHandoffAdmissionStatus::Idle);
    EXPECT_EQ(renderer.Selection(FarWorldPurpose::World).effective, FarWorldMode::Legacy);
    EXPECT_EQ(renderer.PumpAdmission(0).status, FarHandoffAdmissionStatus::Idle);
    EXPECT_EQ(renderer.Selection(FarWorldPurpose::World).effective, FarWorldMode::Legacy);
    EXPECT_EQ(
        renderer.StartSessionTarget({}, sampling_world, Tiles(), policy, capture_limits, samples),
        FarHandoffAdmissionStatus::Refused);
    EXPECT_EQ(renderer.CancelAdmission(), FarHandoffAdmissionStatus::Idle);
    EXPECT_EQ(renderer.Selection(FarWorldPurpose::World).effective, FarWorldMode::Legacy);
    ASSERT_EQ(Start(capture_limits), FarHandoffAdmissionStatus::Pending);
    ASSERT_TRUE(CompleteSession());
    EXPECT_EQ(renderer.Selection(FarWorldPurpose::World).effective, FarWorldMode::Ladder);
    ASSERT_TRUE(source->request_capture(windows));
    source->pump_capture();
    source->pump_capture();
    ASSERT_EQ(Start(capture_limits), FarHandoffAdmissionStatus::Pending);
    EXPECT_EQ(renderer.Selection(FarWorldPurpose::World).effective, FarWorldMode::Ladder);
    EXPECT_EQ(renderer.CancelAdmission(), FarHandoffAdmissionStatus::Idle);
    EXPECT_EQ(renderer.Selection(FarWorldPurpose::World).effective, FarWorldMode::Ladder);
    // A refused replacement does not erase the published target on cancellation.
    EXPECT_EQ(
        renderer.StartSessionTarget({}, sampling_world, Tiles(), policy, capture_limits, samples),
        FarHandoffAdmissionStatus::Refused);
    EXPECT_EQ(renderer.CancelAdmission(), FarHandoffAdmissionStatus::Idle);
    EXPECT_EQ(renderer.Selection(FarWorldPurpose::World).effective, FarWorldMode::Ladder);
}

TEST_F(SessionAdmissionTest, BetweenPumpSourceFailuresRetainTypedDiagnostics) {
    const Save::AuthorityReadLimits reads{
        512, 512, 1024, 8192, 4194304, 16777216, 4194304, 16777216, 131072};
    std::string error;
    for (const bool deadline : {false, true}) {
        SCOPED_TRACE(deadline);
        ASSERT_TRUE(source->admit_saved_source(root, reads, error)) << error;
        capture_now = 0;
        const Producer::CaptureClock clock{[](const void* context) noexcept -> std::uint64_t {
                                               return *static_cast<const std::uint64_t*>(context);
                                           },
                                           &capture_now,
                                           1};
        ASSERT_TRUE(source->configure_capture({reads, 4, clock}));
        ASSERT_TRUE(source->request_capture(windows));
        source->pump_capture();
        source->pump_capture();
        ASSERT_EQ(Start(capture_limits), FarHandoffAdmissionStatus::Pending);
        ASSERT_TRUE(ReachAdmission());
        if (deadline) {
            capture_now = 1;
        } else {
            ASSERT_FALSE(source->admit_saved_source(root / "missing", reads, error));
            ASSERT_EQ(source->capture_status().error, Producer::Error::CommittedReadRefused);
        }
        const auto result = renderer.PumpAdmission(1);
        EXPECT_EQ(result.status, FarHandoffAdmissionStatus::Refused);
        EXPECT_EQ(result.work, 0u);
        EXPECT_EQ(renderer.Selection(FarWorldPurpose::World).refusal,
                  FarWorldRefusal::AdmissionFailed);
        renderer.CaptureDiagnostic(renderer.Selection(FarWorldPurpose::World),
                                   0,
                                   {0, 0, 0},
                                   kLegacyFarPlaneMeters,
                                   false,
                                   0,
                                   0);
        EXPECT_EQ(renderer.Diagnostic().source_failure.source,
                  deadline ? Producer::Error::SourceReadExpired
                           : Producer::Error::CommittedReadRefused);
        EXPECT_EQ(renderer.Diagnostic().source_failure.capture, FarFrozenCaptureError::None);
        EXPECT_EQ(renderer.SessionDiagnostic()->state.target_serial, 0u);
        EXPECT_FALSE(source->frozen_active());
    }
    // A fresh source capture permits restart after either refusal.
    const Producer::CaptureClock clock{
        [](const void*) noexcept -> std::uint64_t { return 0; }, nullptr, 1};
    ASSERT_TRUE(source->configure_capture({reads, 4, clock}));
    ASSERT_TRUE(source->request_capture(windows));
    source->pump_capture();
    source->pump_capture();
    ASSERT_EQ(Start(capture_limits), FarHandoffAdmissionStatus::Pending);
    ASSERT_TRUE(CompleteSession());
}

TEST_F(SessionAdmissionTest, CaptureAndAdmissionComposeWithOwnedInputsAndIdenticalBytes) {
    ASSERT_TRUE(renderer.StageSessionTarget(
        *source, *sampling_world, Tiles(), policy, capture_limits, samples));
    const auto expected =
        FarWorldHandoffDiagnosticJson(*renderer.SessionDiagnostic())["tiles"].dump();
    // A fresh serial forces coverage discovery after the capture cursors finish.
    ASSERT_TRUE(source->request_capture(windows));
    source->pump_capture();
    source->pump_capture();
    ASSERT_EQ(Start(capture_limits), FarHandoffAdmissionStatus::Pending);
    ASSERT_TRUE(ReachAdmission());
    EXPECT_EQ(renderer.SessionDiagnostic()->state.target_serial, 1u);
    EXPECT_EQ(renderer.SessionDiagnostic()->state.serving_serial, 0u);
    const std::weak_ptr<Producer> source_owner = source;
    const std::weak_ptr<const Systems::SHIELD_WorldSystem> world_owner = sampling_world;
    source.reset();
    sampling_world.reset();
    EXPECT_FALSE(source_owner.expired());
    EXPECT_FALSE(world_owner.expired());
    ASSERT_TRUE(CompleteSession());
    EXPECT_TRUE(source_owner.expired());
    EXPECT_TRUE(world_owner.expired());
    EXPECT_EQ(renderer.SessionDiagnostic()->state.target_serial, 2u);
    EXPECT_EQ(FarWorldHandoffDiagnosticJson(*renderer.SessionDiagnostic())["tiles"].dump(),
              expected);
}

TEST_F(SessionAdmissionTest, SourceReplacementFenceAndGenerationRetirementRefuseAfterCapture) {
    for (int change = 0; change < 4; ++change) {
        SCOPED_TRACE(change);
        ASSERT_EQ(Start(capture_limits), FarHandoffAdmissionStatus::Pending);
        ASSERT_TRUE(ReachAdmission());
        EXPECT_EQ(renderer.SessionDiagnostic()->state.target_serial, 0u);
        if (change == 0) {
            source->invalidate_source();
        } else if (change == 1) {
            ASSERT_TRUE(source->request_capture(windows));
            source->pump_capture();
            source->pump_capture();
        } else if (change == 2) {
            source->fence_source_reads(true);
        } else {
            // The retained world owner remains alive, but its generation retires.
            auto mutable_world =
                std::const_pointer_cast<Systems::SHIELD_WorldSystem>(sampling_world);
            mutable_world->set_seed(2);
        }
        const auto result = renderer.PumpAdmission(1);
        EXPECT_EQ(result.status, FarHandoffAdmissionStatus::Refused);
        EXPECT_EQ(result.work, 0u);
        EXPECT_EQ(result.height_observations, 0u);
        EXPECT_EQ(renderer.SessionDiagnostic()->state.target_serial, 0u);
        EXPECT_EQ(renderer.SessionDiagnostic()->state.serving_serial, 0u);
        EXPECT_FALSE(source->frozen_active());
        source->fence_source_reads(false);
        auto mutable_world = std::const_pointer_cast<Systems::SHIELD_WorldSystem>(sampling_world);
        mutable_world->set_seed(1337);
        if (change == 0) {
            EXPECT_FALSE(source->request_capture(windows));
            const Save::AuthorityReadLimits reads{
                512, 512, 1024, 8192, 4194304, 16777216, 4194304, 16777216, 131072};
            std::string error;
            ASSERT_TRUE(source->admit_saved_source(root, reads, error)) << error;
        }
        ASSERT_TRUE(source->request_capture(windows));
        source->pump_capture();
        source->pump_capture();
    }
    ASSERT_EQ(Start(capture_limits), FarHandoffAdmissionStatus::Pending);
    ASSERT_TRUE(CompleteSession());
}

TEST_F(SessionAdmissionTest, CancelCaptureAndAdmissionReleaseOwnershipAndRestart) {
    const auto baseline = source->counters();
    ASSERT_EQ(Start(capture_limits), FarHandoffAdmissionStatus::Pending);
    EXPECT_EQ(renderer.PumpAdmission(64).status, FarHandoffAdmissionStatus::Pending);
    EXPECT_EQ(renderer.CancelAdmission(), FarHandoffAdmissionStatus::Idle);
    EXPECT_FALSE(source->frozen_active());
    EXPECT_EQ(source->counters().copied_records, baseline.copied_records);
    ASSERT_EQ(Start(capture_limits), FarHandoffAdmissionStatus::Pending);
    ASSERT_TRUE(ReachAdmission());
    EXPECT_EQ(renderer.CancelAdmission(), FarHandoffAdmissionStatus::Idle);
    EXPECT_EQ(renderer.SessionDiagnostic()->state.target_serial, 0u);
    ASSERT_EQ(Start(capture_limits), FarHandoffAdmissionStatus::Pending);
    ASSERT_TRUE(CompleteSession());
}

TEST_F(SessionAdmissionTest, PromotionRefusesStaleAdmissionStateAndAllowsRestart) {
    ASSERT_TRUE(renderer.StageSessionTarget(
        *source, *sampling_world, Tiles(), policy, capture_limits, samples));
    ASSERT_TRUE(source->request_capture(windows));
    source->pump_capture();
    source->pump_capture();
    ASSERT_EQ(Start(capture_limits), FarHandoffAdmissionStatus::Pending);
    EXPECT_EQ(renderer.PumpAdmission(64).status, FarHandoffAdmissionStatus::Pending);
    ASSERT_TRUE(Settle());
    const auto result = renderer.PumpAdmission(1);
    EXPECT_EQ(result.status, FarHandoffAdmissionStatus::Refused);
    EXPECT_EQ(result.work, 0u);
    EXPECT_EQ(renderer.SessionDiagnostic()->state.serving_serial, 1u);
    EXPECT_EQ(renderer.SessionDiagnostic()->state.target_serial, 1u);
    EXPECT_FALSE(source->frozen_active());
    ASSERT_EQ(Start(capture_limits), FarHandoffAdmissionStatus::Pending);
    ASSERT_TRUE(CompleteSession());
    EXPECT_EQ(renderer.SessionDiagnostic()->state.target_serial, 2u);
}

TEST_F(SessionAdmissionTest, MissingOwnersInvalidLimitsAndExpiredDeadlinePermitRestart) {
    EXPECT_EQ(
        renderer.StartSessionTarget({}, sampling_world, Tiles(), policy, capture_limits, samples),
        FarHandoffAdmissionStatus::Refused);
    EXPECT_EQ(renderer.StartSessionTarget(source, {}, Tiles(), policy, capture_limits, samples),
              FarHandoffAdmissionStatus::Refused);
    EXPECT_EQ(
        renderer.StartSessionTarget(source, sampling_world, {}, policy, capture_limits, samples),
        FarHandoffAdmissionStatus::Refused);
    const std::array<FarSessionHandoffTile, Producer::kSlots + 1> oversized{};
    EXPECT_EQ(renderer.StartSessionTarget(
                  source, sampling_world, oversized, policy, capture_limits, samples),
              FarHandoffAdmissionStatus::Refused);
    auto invalid = capture_limits;
    invalid.max_height_samples = 0;
    EXPECT_EQ(Start(invalid), FarHandoffAdmissionStatus::Refused);
    EXPECT_EQ(
        renderer.StartSessionTarget(source, sampling_world, Tiles(), policy, capture_limits, {}),
        FarHandoffAdmissionStatus::Refused);
    auto mismatched = Tiles();
    mismatched[0].capture.key.x = 1;
    ASSERT_EQ(renderer.StartSessionTarget(
                  source, sampling_world, mismatched, policy, capture_limits, samples),
              FarHandoffAdmissionStatus::Pending);
    EXPECT_EQ(renderer.PumpAdmission(1).status, FarHandoffAdmissionStatus::Refused);
    std::uint64_t now = 0;
    auto expiring = capture_limits;
    expiring.deadline = {[](const void* context) noexcept -> std::uint64_t {
                             return *static_cast<const std::uint64_t*>(context);
                         },
                         &now,
                         1};
    ASSERT_EQ(Start(expiring), FarHandoffAdmissionStatus::Pending);
    ASSERT_TRUE(ReachAdmission());
    now = 1;
    const auto refused = renderer.PumpAdmission(1);
    EXPECT_EQ(refused.status, FarHandoffAdmissionStatus::Refused);
    EXPECT_EQ(refused.work, 0u);
    EXPECT_EQ(renderer.Selection(FarWorldPurpose::World).refusal, FarWorldRefusal::AdmissionFailed);
    renderer.CaptureDiagnostic(renderer.Selection(FarWorldPurpose::World),
                               0,
                               {0, 0, 0},
                               kLegacyFarPlaneMeters,
                               false,
                               0,
                               0);
    EXPECT_EQ(renderer.Diagnostic().source_failure.capture,
              FarFrozenCaptureError::DeadlineExceeded);
    EXPECT_EQ(renderer.SessionDiagnostic()->state.target_serial, 0u);
    ASSERT_EQ(Start(capture_limits), FarHandoffAdmissionStatus::Pending);
    ASSERT_TRUE(CompleteSession());
}

TEST_F(HandoffAdmissionTest, AllocationRefusalOversizeAndReplacementReleasePendingState) {
    const std::array input{RendererHandoffInput(renderer, {5, 0, 0}, 1, false)};
    const auto manifest = RendererHandoffManifest(1, input);
    const auto pool = renderer.MemoryPool(kFarTierCount);
    const auto used = pool->used();
    {
        FarVolumeMemoryLease exhausted(*pool);
        ASSERT_TRUE(exhausted.TryCharge(pool->limit() - used));
        EXPECT_EQ(renderer.StartTarget(manifest), FarHandoffAdmissionStatus::Refused);
        EXPECT_EQ(renderer.AdmissionRefusal(), FarWorldRefusal::AdmissionFailed);
    }
    EXPECT_EQ(pool->used(), used);
    for (const std::uint64_t allocation : {0u, 1u, 2u, 3u}) {
        pool->RefuseAttemptsFrom(pool->attempts() + allocation);
        EXPECT_EQ(renderer.StartTarget(manifest), FarHandoffAdmissionStatus::Refused);
        EXPECT_EQ(renderer.Handoff()->State().target_serial, 0u);
        EXPECT_EQ(pool->used(), used);
        pool->RefuseAttemptsFrom(std::numeric_limits<std::uint64_t>::max());
    }
    ASSERT_EQ(renderer.StartTarget(manifest), FarHandoffAdmissionStatus::Pending);
    EXPECT_EQ(renderer.PumpAdmission(64).status, FarHandoffAdmissionStatus::Pending);
    const auto residency = RendererHandoffLimits();
    auto capacity = FarWorldAuthorityProducer::kSlots;
    for (std::size_t tier = 0; tier + 1 < residency.entry_slots.size(); ++tier) {
        capacity += residency.entry_slots[tier];
    }
    std::vector<FarHandoffTileInput> oversized(capacity + 1);
    EXPECT_EQ(renderer.StartTarget(RendererHandoffManifest(1, oversized)),
              FarHandoffAdmissionStatus::Refused);
    EXPECT_EQ(pool->used(), used);
    ASSERT_EQ(renderer.StartTarget(manifest), FarHandoffAdmissionStatus::Pending);
    EXPECT_TRUE(renderer.StageTarget(manifest)); // Synchronous replacement drains its cursor.
    EXPECT_EQ(renderer.AdmissionStatus(), FarHandoffAdmissionStatus::Complete);
    EXPECT_EQ(pool->used(), used);
    EXPECT_EQ(renderer.PumpAdmission(1).work, 0u);
}

TEST(FarWorldRenderer, BoundedAdmissionOperationsRequireOwnerThreadWhenUnbound) {
    FarWorldRenderer renderer;
    FarWorldHandoffRenderer direct;
    std::size_t refusals = 0;
    std::thread worker([&] {
        const auto check = [&refusals](const auto& operation) {
            try {
                operation();
            } catch (const std::logic_error&) {
                ++refusals;
            }
        };
        check([&] { renderer.StartSessionTarget({}, {}, {}, {}, {}, {}); });
        check([&] { renderer.PumpAdmission(1); });
        check([&] { renderer.CancelAdmission(); });
        check([&] { renderer.AdmissionStatus(); });
        check([&] { direct.StartTarget({}); });
        check([&] { direct.StartSessionTarget({}, {}, {}, {}, {}, {}); });
        check([&] { direct.PumpAdmission(1); });
        check([&] { direct.CancelAdmission(); });
        check([&] { direct.AdmissionStatus(); });
    });
    worker.join();
    EXPECT_EQ(refusals, 9u);
}

TEST_F(HandoffAdmissionTest, RegionSearchIsChargedAndIndexedColumnsPreserveCoverage) {
    const FarTileKey key{5, 0, 0};
    const auto edge = FarTileEdgeMeters(key.tier);
    FarFrozenCaptureSource source;
    source.pristine.field_identity = source.content_identity = 123;
    source.durable = {1, FarEditedImageState::Committed};
    source.pristine.height = [](std::int64_t x, std::int64_t z) {
        return float((x + z) / 64 % 7);
    };
    source.pristine.density = [](const FarVolumePosition&, float, std::uint32_t, FarCaveMode) {
        return FarVolumeSample{3, 7};
    };
    source.committed = [](std::uint8_t, const FarVolumePosition&) {
        return FarFrozenOverride<FarVolumeSample>{FarFrozenPresence::Absent, {}};
    };
    source.water = [](std::int64_t, std::int64_t) {
        return FarFrozenWaterSample{false, 0};
    };
    const std::array<FarFrozenRegion, 2> regions{
        {{5, {{-2 * edge, -512, 0}, {-edge, 512, edge}}}, {5, {{0, -512, 0}, {edge, 512, edge}}}}};
    std::shared_ptr<const FarFrozenObservation> image;
    ASSERT_EQ(CaptureFarFrozenObservation(regions,
                                          source,
                                          {2, 100'000, 20'000'000, 100'000, 32ULL * 1024 * 1024},
                                          renderer.MemoryPool(key.tier),
                                          image),
              FarFrozenCaptureError::None);
    ASSERT_TRUE(image);
    EXPECT_FALSE(image->RegionHeight(2, 0, 0));
    EXPECT_FALSE(image->RegionHeight(0, std::numeric_limits<std::size_t>::max(), 0));
    EXPECT_FALSE(image->RegionHeight(0, 0, std::numeric_limits<std::size_t>::max()));
    FarVolumeWantedRequest wanted;
    wanted.identity = {1, image->content_identity(), 0, key, 1, 1};
    ASSERT_TRUE(ToFarVolumeTileKey(key, wanted.intent.key));
    FarVolumeRequest expected;
    ASSERT_TRUE(ResolveFarVolumeCoverage(wanted.intent, source.pristine, {100'000, 0}, expected));
    expected.field_identity = image->content_identity();
    const auto input = MakeFarHandoffTileInput(wanted, image);
    ASSERT_TRUE(input);
    if (input) {
        ASSERT_EQ(renderer.StartTarget(RendererHandoffManifest(1, {&*input, 1})),
                  FarHandoffAdmissionStatus::Pending);
    }
    std::uint64_t columns = 0, units = 0;
    for (std::size_t pump = 0; pump < 5000; ++pump) {
        const auto result = renderer.PumpAdmission(7);
        EXPECT_LE(result.work, 7u);
        EXPECT_LE(result.height_observations, result.work);
        columns += result.height_observations;
        units += result.work;
        if (result.status != FarHandoffAdmissionStatus::Pending) {
            EXPECT_EQ(result.status, FarHandoffAdmissionStatus::Complete);
            EXPECT_EQ(columns, 129u * 129u);
            EXPECT_GT(units, columns);
            EXPECT_EQ(renderer.Handoff()->TargetCoverageRequest(key), std::optional{expected});
            return;
        }
    }
    FAIL() << "Indexed multi-region coverage failed to terminate";
}

TEST_F(HandoffAdmissionTest, EveryYieldKeepsTheWholeCandidateInvisible) {
    const std::array inputs{RendererHandoffInput(renderer, {5, 0, 0}, 1, false),
                            RendererHandoffInput(renderer, {5, 1, 0}, 1, false)};
    ASSERT_EQ(renderer.StartTarget(RendererHandoffManifest(1, inputs)),
              FarHandoffAdmissionStatus::Pending);
    std::uint64_t columns = 0;
    for (std::size_t pump = 0; pump < 600; ++pump) {
        const auto result = renderer.PumpAdmission(64);
        EXPECT_LE(result.work, 64u);
        columns += result.height_observations;
        if (result.status != FarHandoffAdmissionStatus::Pending) {
            ASSERT_EQ(result.status, FarHandoffAdmissionStatus::Complete);
            EXPECT_EQ(columns, 2u * 129u * 129u);
            EXPECT_EQ(renderer.Handoff()->State().target_serial, 1u);
            for (const auto& input : inputs) {
                EXPECT_TRUE(renderer.Handoff()->Observe(input.wanted.identity.key));
            }
            return;
        }
        EXPECT_EQ(renderer.Handoff()->State().target_serial, 0u);
        EXPECT_EQ(renderer.Handoff()->State().serving_serial, 0u);
        for (const auto& input : inputs) {
            EXPECT_FALSE(renderer.Handoff()->Observe(input.wanted.identity.key));
            EXPECT_FALSE(renderer.Handoff()->TargetCoverageRequest(input.wanted.identity.key));
        }
    }
    FAIL() << "Candidate did not publish within the column bound";
}

TEST(FarWorldRenderer, CoverageAccumulatorRefusalsPreserveOutputAndPermitRestart) {
    FarVolumeCoverageCursor cursor;
    FarVolumeRequest held{{5, 1, 1}, {-256, 256}, 7};
    const auto unchanged = held;
    EXPECT_EQ(cursor.Finish(held), FarVolumeFacadeError::InvalidIntent);
    EXPECT_EQ(cursor.Observe(0), FarVolumeFacadeError::InvalidIntent);
    EXPECT_EQ(held, unchanged);
    EXPECT_EQ(cursor.Start({{0, 0, 0}, {}}, 1), FarVolumeFacadeError::InvalidIntent);
    EXPECT_EQ(cursor.Start({{5, std::numeric_limits<std::int64_t>::max(), 0}, {}}, 1),
              FarVolumeFacadeError::CoordinateRange);
    EXPECT_EQ(cursor.Start({{5, 0, 0}, FarVolumeCoverageSpan{1, 0}}, 1),
              FarVolumeFacadeError::InvalidIntent);
    EXPECT_EQ(cursor.Start({{5, 0, 0}, {}}, 1), FarVolumeFacadeError::None);
    EXPECT_EQ(cursor.Observe({}), FarVolumeFacadeError::SamplerFailure);
    EXPECT_EQ(cursor.Observe(std::numeric_limits<float>::quiet_NaN()),
              FarVolumeFacadeError::NonFiniteSample);
    EXPECT_EQ(cursor.Observe(float(kFarVolumeExactCoordinateLimitMeters)),
              FarVolumeFacadeError::CoordinateRange);
    EXPECT_EQ(cursor.Finish(held), FarVolumeFacadeError::InvalidIntent);
    EXPECT_EQ(held, unchanged);
    for (std::uint8_t tier = 1; tier <= kFarTierCount; ++tier) {
        const FarVolumeCoverageIntent intent{{tier, -1, -1}, FarVolumeCoverageSpan{-1000, 1000}};
        ASSERT_EQ(cursor.Start(intent, 42), FarVolumeFacadeError::None);
        FarVolumeAuthority authority;
        authority.field_identity = 42;
        authority.height = [](std::int64_t x, std::int64_t z) {
            return float((x + z) % 9);
        };
        authority.density = [](const FarVolumePosition&, float, std::uint32_t, FarCaveMode) {
            return FarVolumeSample{};
        };
        for (std::size_t column = 0; column < 129u * 129u; ++column) {
            const auto [x, z] = cursor.Column();
            ASSERT_EQ(cursor.Observe(authority.height(x, z)), FarVolumeFacadeError::None);
        }
        EXPECT_TRUE(cursor.Complete());
        EXPECT_EQ(cursor.Observe(0), FarVolumeFacadeError::InvalidIntent);
        ASSERT_EQ(cursor.Finish(held), FarVolumeFacadeError::None);
        FarVolumeRequest synchronous;
        ASSERT_TRUE(ResolveFarVolumeCoverage(intent, authority, {129u * 129u, 0}, synchronous));
        EXPECT_EQ(held, synchronous);
    }
    ASSERT_EQ(cursor.Start({{5, 0, 0}, FarVolumeCoverageSpan{-8'388'000, 0}}, 42),
              FarVolumeFacadeError::None);
    for (std::size_t column = 0; column < 129u * 129u; ++column) {
        ASSERT_EQ(cursor.Observe(0), FarVolumeFacadeError::None);
    }
    held = unchanged;
    EXPECT_EQ(cursor.Finish(held), FarVolumeFacadeError::CoordinateRange);
    EXPECT_EQ(held, unchanged);
}

TEST_F(SessionAdmissionTest, BusyProducerCursorIsRefusedWithoutCancellingItsOwner) {
    const auto pool = std::make_shared<FarVolumeMemoryPool>(64u * 1024u * 1024u);
    const auto request = Tiles()[1].capture;
    ASSERT_TRUE(source->start_frozen(sampling_world, request, policy, capture_limits, pool).ok());
    EXPECT_EQ(Start(capture_limits), FarHandoffAdmissionStatus::Refused);
    EXPECT_TRUE(source->frozen_active());
    source->cancel_frozen();
    ASSERT_EQ(Start(capture_limits), FarHandoffAdmissionStatus::Pending);
    ASSERT_TRUE(source->start_frozen(sampling_world, request, policy, capture_limits, pool).ok());
    EXPECT_EQ(renderer.PumpAdmission(1).status, FarHandoffAdmissionStatus::Refused);
    EXPECT_TRUE(source->frozen_active());
    EXPECT_EQ(renderer.SessionDiagnostic()->state.target_serial, 0u);
    source->cancel_frozen();
    ASSERT_EQ(Start(capture_limits), FarHandoffAdmissionStatus::Pending);
    EXPECT_EQ(renderer.CancelAdmission(), FarHandoffAdmissionStatus::Idle);
}

TEST_F(SessionAdmissionTest, AdmissionHeightLimitSharesCaptureChargesAndRestartsCleanly) {
    auto bounded = samples;
    // Terrain and base water in both images consume 4 * 131 * 131 height
    // observations. The remainder cannot admit the first 129 * 129 coverage
    // pass; per-image limits still fit.
    bounded.height = 80'000;
    ASSERT_EQ(renderer.StartSessionTarget(
                  source, sampling_world, Tiles(), policy, capture_limits, bounded),
              FarHandoffAdmissionStatus::Pending);
    ASSERT_TRUE(ReachAdmission());
    EXPECT_FALSE(CompleteSession());
    EXPECT_EQ(renderer.AdmissionStatus(), FarHandoffAdmissionStatus::Refused);
    EXPECT_EQ(renderer.SessionDiagnostic()->state.target_serial, 0u);
    EXPECT_EQ(renderer.SessionDiagnostic()->state.serving_serial, 0u);
    Capture();
    EXPECT_EQ(renderer.Diagnostic().source_failure.capture, FarFrozenCaptureError::WorkLimit);
    ASSERT_EQ(Start(capture_limits), FarHandoffAdmissionStatus::Pending);
    ASSERT_TRUE(CompleteSession());
}
