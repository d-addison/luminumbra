#include "../fixtures/TerrainFixtureParams.h"
#include "../support/UniqueTempPath.h"
#include "core/JobSystem.h"
#include "core/Log.h"
#include "core/LogDispatcherSink.h"
#include "luminumbra_client/rendering/FarWorldHandoffDiagnostics.h"
#include "luminumbra_client/rendering/FarWorldSessionController.h"
#include "world/FarWorldCapturePlan.h"
#include "world/WorldStreamingState.h"
#include <array>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include <limits>
#include <mutex>
#include <string_view>

namespace {
using namespace Luminumbra;
using namespace Luminumbra::World;
using namespace Luminumbra::Rendering;
using Controller = Rendering::FarWorldSessionController;
using Producer = FarWorldAuthorityProducer;
using Plan = World::FarWorldCapturePlan;
using Save = Persistence::WorldSaveService;

// Fixed fake storage for the real scheduler; no GL calls or native context.
class Context final : public FarVolumeGpuDriver {
public:
    bool ValidContext() const noexcept override {
        return true;
    }
    bool HasError() noexcept override {
        return false;
    }
    void GetVertexArrayBinding(std::uint32_t& name) noexcept override {
        name = vao_;
    }
    void GetVertexBufferBinding(std::uint32_t& name) noexcept override {
        name = vertex_;
    }
    void CreateVertexArray(std::uint32_t& name) noexcept override {
        name = next_++;
    }
    void CreateBuffer(std::uint32_t& name) noexcept override {
        name = next_++;
    }
    void DeleteVertexArray(std::uint32_t) noexcept override {}
    void DeleteBuffer(std::uint32_t) noexcept override {}
    void BindVertexArray(std::uint32_t name) noexcept override {
        vao_ = name;
    }
    void BindBuffer(FarVolumeBufferKind kind, std::uint32_t name) noexcept override {
        if (kind == FarVolumeBufferKind::Vertex)
            vertex_ = name;
        else
            index_ = name;
    }
    void
    UploadBuffer(FarVolumeBufferKind kind, std::intptr_t bytes, const void*) noexcept override {
        const auto name = kind == FarVolumeBufferKind::Vertex ? vertex_ : index_;
        ASSERT_LT(name, sizes_.size());
        sizes_[name] = bytes;
    }
    void GetBufferBytes(FarVolumeBufferKind kind, std::int64_t& bytes) noexcept override {
        const auto name = kind == FarVolumeBufferKind::Vertex ? vertex_ : index_;
        ASSERT_LT(name, sizes_.size());
        bytes = sizes_[name];
    }
    void SetAttribute(FarVolumeVertexAttribute, std::int32_t, std::size_t) noexcept override {}
    void EnableAttribute(FarVolumeVertexAttribute) noexcept override {}

private:
    std::uint32_t next_ = 1, vao_ = 0, vertex_ = 0, index_ = 0;
    std::array<std::int64_t, 64> sizes_{};
};
class FarWorldSessionController : public testing::Test {
protected:
    mutable std::uint64_t ticks = 0;
    std::uint64_t clock_step = 0, frame = 0;
    JobSystem jobs;
    FarWorldRenderer renderer;
    Controller controller{renderer};
    FarWorldSessionQualificationLimits limits;
    Producer::FrozenPolicy policy;
    std::shared_ptr<Producer> producer;
    std::shared_ptr<Systems::SHIELD_WorldSystem> world;
    std::filesystem::path root = TestSupport::CreateUniqueTempDirectory("session_controller_");
    // Handoff requires V5 fallback coverage: both halos need 18x18 source
    // member records, within the injected 512-member test read profile.
    const std::array<FarTileKey, 1> keys{{{5, 0, 0}}};
    void SetUp() override {
        jobs.startup(1);
        limits.rendering = FarWorldDevelopmentLimits();
        limits.source = {512, 4u * 1024u * 1024u, 300'000, {}};
        limits.capture = {{512, 512, 1024, 8192, 4194304, 16777216, 4194304, 16777216, 131072},
                          4,
                          {[](const void* context) noexcept {
                               const auto& fixture =
                                   *static_cast<const FarWorldSessionController*>(context);
                               const auto result = fixture.ticks;
                               fixture.ticks += fixture.clock_step;
                               return result;
                           },
                           this,
                           1'000'000}};
        limits.sampling = {1, 400'000, 20'000'000, 400'000, 64u * 1024u * 1024u};
        limits.transaction_samples = {2'000'000, 40'000'000, 2'000'000};
        limits.tiles = 8;
        limits.planning_operations = 4'000'000;
        limits.admission_operations = 20'000'000;
        limits.operations_per_pump = 32'768;
        limits.refresh_ticks = 2'000'000;
        limits.ticks_per_second = 1000;
        limits.refresh_frames = 4096;
        auto params = Luminumbra::Test::SyntheticTerrainParams();
        params.caves_enabled = false;
        params.base_amplitude = params.peaks_amplitude = 0;
        params.height_offset = -8;
        params.continental_spline = {{-1, 0}, {1, 0}};
        world = std::make_shared<Systems::SHIELD_WorldSystem>(nullptr, nullptr, params, 1337);
        policy = {WorldFarVolumeIdentity(
                      *world, FarCaveMode::BandLimited, FarVolumeNumericProfile::RawFloat, 4),
                  FarCaveMode::BandLimited,
                  FarVolumeNumericProfile::RawFloat,
                  4};
        renderer.InstallBackend(std::make_unique<Context>());
        WorldStreamingState disk;
        ASSERT_TRUE(Save{}.save_world(disk, root));
        ASSERT_NO_FATAL_FAILURE(ConfigureSource());
    }
    void ConfigureSource() {
        controller.Cancel();
        if (producer)
            producer->retire();
        producer.reset();
        ASSERT_EQ(controller.Configure(jobs, 1, policy, limits), FarWorldRefusal::None);
        const auto config = controller.SourceConfig();
        producer = std::allocate_shared<Producer>(
            FarVolumePoolAllocator<Producer>(config.source.memory), config.source);
        std::string error;
        ASSERT_TRUE(producer->admit_saved_source(root, config.capture.read, error)) << error;
        ASSERT_EQ(controller.Attach(producer, world), FarWorldRefusal::None);
    }
    void TearDown() override {
        controller.Cancel();
        if (producer)
            producer->retire();
        renderer.Shutdown();
        jobs.shutdown();
        std::error_code error;
        std::filesystem::remove_all(root, error);
    }
    void Admit() {
        ASSERT_TRUE(controller.Request(keys));
        for (unsigned pump = 0; pump < 2048; ++pump) {
            const auto phase = controller.Pump(++frame);
            ASSERT_NE(phase, Controller::Phase::Refused)
                << int(controller.failure().source) << ":" << int(controller.failure().capture)
                << ":" << int(renderer.Selection(FarWorldPurpose::World, true).refusal);
            if (phase == Controller::Phase::Waiting)
                return;
        }
        FAIL() << "bounded admission did not finish";
    }
    void FinishBuild() {
        // Drain the actual worker after dispatch, then consume its completion.
        // Bounded joins avoid sleeps and scheduler-dependent polling assertions.
        for (unsigned pass = 0; pass < 4; ++pass) {
            ASSERT_TRUE(renderer.PrepareFrame(++frame, {0, 0, 0}));
            jobs.shutdown();
            jobs.startup(1);
            const auto phase = controller.Pump(frame);
            ASSERT_NE(phase, Controller::Phase::Refused);
            if (phase == Controller::Phase::Complete)
                return;
        }
        FAIL() << "target did not become ready after draining its worker";
    }
    void CaptureDiagnostic() {
        renderer.CaptureDiagnostic(renderer.Selection(FarWorldPurpose::World),
                                   frame,
                                   {},
                                   kLegacyFarPlaneMeters,
                                   false,
                                   0,
                                   0);
    }
    void CaptureDiscoverySource(const Plan& plan) {
        ASSERT_TRUE(producer->request_capture(plan.source_windows()));
        for (std::uint64_t work = 0; work < limits.admission_operations;
             work += limits.operations_per_pump) {
            producer->pump_capture(limits.operations_per_pump);
            const auto status = producer->capture_status();
            ASSERT_EQ(status.error, Producer::Error::None);
            if (!status.active) {
                ASSERT_TRUE(status.latest_ready);
                return;
            }
        }
        FAIL() << "discovery source exceeded the operation bound";
    }
    Plan::Result FinishDiscovery(Plan& plan) const {
        for (std::uint64_t work = 0; work < limits.planning_operations;
             work += limits.operations_per_pump) {
            const auto result = plan.Pump(limits.operations_per_pump);
            EXPECT_LE(result.work, limits.operations_per_pump);
            if (!result.ok() || result.complete) {
                return result;
            }
        }
        ADD_FAILURE() << "discovery exceeded the operation bound";
        plan.Cancel();
        return {Plan::Error::WorkLimit};
    }
};

TEST_F(FarWorldSessionController, DiagnosticsDiscoveryUsesTransactionAcrossSixPristineTiles) {
    limits = FarWorldDiagnosticsSessionLimits(limits.capture.clock.now, this);
    ASSERT_NO_FATAL_FAILURE(ConfigureSource());
    Plan plan(controller.SourceConfig().source.memory);
    const std::array<FarTileKey, 6> wanted{
        {{1, 0, 0}, {1, 1, 0}, {1, 2, 0}, {1, 3, 0}, {1, 4, 0}, {1, 5, 0}}};
    ASSERT_EQ(plan.SetWanted(wanted), Plan::Error::None);
    ASSERT_NO_FATAL_FAILURE(CaptureDiscoverySource(plan));
    FarFrozenCaptureWork transaction(limits.transaction_samples);
    // Existing transaction work must survive Start and both completion and cancel.
    ASSERT_TRUE(transaction.ChargeHeight());
    ASSERT_TRUE(transaction.ChargeDensity());
    ASSERT_TRUE(transaction.ChargeWater());
    auto sampling = limits.sampling;
    sampling.work = &transaction;
    const Plan::Limits planning{limits.tiles, limits.planning_operations, limits.rendering.spans};
    ASSERT_TRUE(plan.Start(producer, world, policy, sampling, planning).ok());
    const auto result = FinishDiscovery(plan);
    ASSERT_TRUE(result.ok()) << static_cast<int>(result.source.capture);
    ASSERT_TRUE(result.complete);
    const auto accepted = plan.accepted();
    ASSERT_NE(accepted, nullptr);
    EXPECT_EQ(accepted->tiles.size(), wanted.size());
    constexpr std::uint64_t water_per_tile = 4u * 131u * 131u;
    constexpr std::uint64_t height_per_tile = 131u * 131u + 129u * 129u;
    const auto counts = transaction.counts();
    EXPECT_EQ(counts.water, 1u + wanted.size() * water_per_tile);
    EXPECT_EQ(counts.height, 1u + wanted.size() * height_per_tile);
    EXPECT_EQ(counts.density, 1u);
    EXPECT_GT(counts.water, sampling.max_water_samples);
    plan.Cancel();
    EXPECT_EQ(transaction.counts().water, counts.water);
    EXPECT_FALSE(producer->frozen_active());
}

TEST_F(FarWorldSessionController, DiagnosticsDiscoveryRetainsTileAndTransactionCaps) {
    limits = FarWorldDiagnosticsSessionLimits(limits.capture.clock.now, this);
    ASSERT_NO_FATAL_FAILURE(ConfigureSource());
    Plan plan(controller.SourceConfig().source.memory);
    const std::array<FarTileKey, 6> wanted{
        {{1, 0, 0}, {1, 1, 0}, {1, 2, 0}, {1, 3, 0}, {1, 4, 0}, {1, 5, 0}}};
    ASSERT_EQ(plan.SetWanted(wanted), Plan::Error::None);
    ASSERT_NO_FATAL_FAILURE(CaptureDiscoverySource(plan));
    constexpr std::uint64_t water_per_tile = 4u * 131u * 131u;
    constexpr std::uint64_t height_per_tile = 131u * 131u + 129u * 129u;
    const Plan::Limits planning{limits.tiles, limits.planning_operations, limits.rendering.spans};
    const FarFrozenCaptureWork::Counts exact{
        wanted.size() * height_per_tile, 0, wanted.size() * water_per_tile};
    FarFrozenCaptureWork full(exact);
    auto sampling = limits.sampling;
    sampling.max_height_samples = height_per_tile;
    sampling.max_water_samples = water_per_tile;
    sampling.work = &full;
    ASSERT_TRUE(plan.Start(producer, world, policy, sampling, planning).ok());
    const auto complete = FinishDiscovery(plan);
    ASSERT_TRUE(complete.ok());
    ASSERT_TRUE(complete.complete);
    EXPECT_EQ(full.counts().height, exact.height);
    EXPECT_EQ(full.counts().water, exact.water);
    const auto accepted = plan.accepted();
    ASSERT_NE(accepted, nullptr);
    // Both kinds must refuse one sample below either the tile or transaction
    // boundary. Discovery never charges density; its zero allowance is valid.
    for (const bool height : {false, true}) {
        for (const bool per_tile : {false, true}) {
            SCOPED_TRACE(testing::Message() << "height=" << height << " tile=" << per_tile);
            auto caps = exact;
            auto bounded = limits.sampling;
            if (per_tile) {
                if (height) {
                    bounded.max_height_samples = height_per_tile - 1;
                } else {
                    bounded.max_water_samples = water_per_tile - 1;
                }
            } else if (height) {
                --caps.height;
            } else {
                --caps.water;
            }
            FarFrozenCaptureWork transaction(caps);
            bounded.work = &transaction;
            ASSERT_TRUE(plan.Start(producer, world, policy, bounded, planning).ok());
            const auto result = FinishDiscovery(plan);
            EXPECT_EQ(result.error, Plan::Error::SourceRefused);
            EXPECT_EQ(result.source.capture, FarFrozenCaptureError::WorkLimit);
            EXPECT_EQ(plan.accepted(), accepted);
            EXPECT_FALSE(plan.active());
            EXPECT_FALSE(producer->frozen_active());
            const auto counts = transaction.counts();
            EXPECT_EQ(counts.density, 0u);
            if (height) {
                EXPECT_EQ(counts.height, per_tile ? height_per_tile - 1 : caps.height);
            } else {
                EXPECT_EQ(counts.water, per_tile ? water_per_tile - 1 : caps.water);
            }
        }
    }
}

TEST_F(FarWorldSessionController, DiagnosticsDiscoveryChecksParentDeadlineAndEmptyWanted) {
    limits = FarWorldDiagnosticsSessionLimits(limits.capture.clock.now, this);
    ASSERT_NO_FATAL_FAILURE(ConfigureSource());
    Plan plan(controller.SourceConfig().source.memory);
    ASSERT_EQ(plan.SetWanted(keys), Plan::Error::None);
    ASSERT_NO_FATAL_FAILURE(CaptureDiscoverySource(plan));
    const Plan::Limits planning{limits.tiles, limits.planning_operations, limits.rendering.spans};
    FarFrozenCaptureWork transaction(
        limits.transaction_samples, nullptr, {limits.capture.clock.now, this, ticks + 1});
    auto sampling = limits.sampling;
    sampling.work = &transaction;
    ASSERT_TRUE(plan.Start(producer, world, policy, sampling, planning).ok());
    ++ticks;
    const auto expired = FinishDiscovery(plan);
    EXPECT_EQ(expired.error, Plan::Error::SourceRefused);
    EXPECT_EQ(expired.source.capture, FarFrozenCaptureError::DeadlineExceeded);
    EXPECT_EQ(plan.accepted(), nullptr);
    EXPECT_EQ(transaction.counts().height, 0u);
    EXPECT_EQ(transaction.counts().water, 0u);
    ASSERT_EQ(plan.SetWanted(std::span<const FarTileKey>{}), Plan::Error::None);
    FarFrozenCaptureWork empty({0, 0, 0});
    sampling.work = &empty;
    ASSERT_TRUE(plan.Start(producer, world, policy, sampling, planning).ok());
    ASSERT_TRUE(FinishDiscovery(plan).complete);
    const auto accepted = plan.accepted();
    ASSERT_NE(accepted, nullptr);
    EXPECT_TRUE(accepted->tiles.empty());
    EXPECT_EQ(empty.counts().height, 0u);
    EXPECT_EQ(empty.counts().density, 0u);
    EXPECT_EQ(empty.counts().water, 0u);
}

TEST_F(FarWorldSessionController, DiagnosticsProfileIsCompleteAndFinite) {
    const auto profile = FarWorldDiagnosticsSessionLimits(limits.capture.clock.now, this);
    EXPECT_TRUE(profile.Complete());
    EXPECT_FALSE(FarWorldDiagnosticsSessionLimits(nullptr).Complete());
    const auto& read = profile.capture.read;
    for (const auto value : {std::uint64_t(profile.source.index_records),
                             profile.source.copy_bytes,
                             profile.source.query_columns,
                             read.members,
                             read.records,
                             read.headers,
                             read.probes,
                             read.compressed_member_bytes,
                             read.compressed_total_bytes,
                             read.decoded_member_bytes,
                             read.decoded_total_bytes,
                             read.control_bytes,
                             std::uint64_t(profile.capture.deadline_frames),
                             profile.capture.clock.max_ticks,
                             std::uint64_t(profile.sampling.max_regions),
                             profile.sampling.max_height_samples,
                             profile.sampling.max_density_samples,
                             profile.sampling.max_water_samples,
                             profile.sampling.max_bytes,
                             profile.transaction_samples.height,
                             profile.transaction_samples.density,
                             profile.transaction_samples.water,
                             std::uint64_t(profile.tiles),
                             profile.planning_operations,
                             profile.admission_operations,
                             profile.operations_per_pump,
                             profile.refresh_ticks,
                             profile.ticks_per_second,
                             std::uint64_t(profile.refresh_frames)}) {
        EXPECT_GT(value, 0u);
        EXPECT_LT(value, std::numeric_limits<std::uint64_t>::max());
    }
    EXPECT_EQ(profile.source.query_columns, 300'000u);
    EXPECT_EQ(profile.source.copy_bytes, 4u * 1024u * 1024u);
    EXPECT_EQ(profile.source.index_records, 512u);
    EXPECT_EQ(read.members, 512u);
    EXPECT_EQ(read.records, 512u);
    EXPECT_EQ(read.headers, 1024u);
    EXPECT_EQ(read.probes, 8192u);
    EXPECT_EQ(read.compressed_member_bytes, 4u * 1024u * 1024u);
    EXPECT_EQ(read.decoded_member_bytes, 4u * 1024u * 1024u);
    EXPECT_EQ(read.control_bytes, 128u * 1024u);
    EXPECT_EQ(profile.tiles, 512u);
    EXPECT_EQ(profile.operations_per_pump, 32'768u);
    EXPECT_EQ(profile.ticks_per_second, 1000u);
    EXPECT_GE(profile.transaction_samples.height, 4ull * 10'171'800);
    EXPECT_GE(profile.transaction_samples.density, 4ull * 261'945'504);
    EXPECT_GE(profile.transaction_samples.water, 4ull * 16'474'560);
    EXPECT_GE(profile.planning_operations, 4ull * 4'058'282);
    EXPECT_GE(profile.admission_operations, 4ull * 204'655'998);
    EXPECT_GE(read.compressed_total_bytes, 4ull * 90 * 1024 * 1024);
    EXPECT_GE(read.decoded_total_bytes, 4ull * 90 * 1024 * 1024);
    EXPECT_GE(profile.refresh_frames, 4u * 6371);
    EXPECT_GE(profile.capture.deadline_frames, 4u * 6371);
    EXPECT_GE(profile.capture.clock.max_ticks, 4ull * 2000);
    EXPECT_GE(profile.refresh_ticks, 4ull * 30'000);
    EXPECT_FALSE(profile.source.memory);
    EXPECT_EQ(profile.sampling.work, nullptr);
    EXPECT_EQ(profile.sampling.deadline.now, nullptr);
    const auto rendering = FarWorldDevelopmentLimits();
    EXPECT_TRUE(profile.rendering.Complete());
    EXPECT_EQ(profile.rendering.budgets.bytes, rendering.budgets.bytes);
    EXPECT_EQ(profile.rendering.residency.entry_slots, rendering.residency.entry_slots);
    EXPECT_EQ(profile.rendering.frame.builds, rendering.frame.builds);
    EXPECT_EQ(profile.rendering.frame.uploads, rendering.frame.uploads);
    EXPECT_EQ(profile.rendering.frame.upload_bytes, rendering.frame.upload_bytes);
    const auto finite = [](auto actual, auto expected) {
        EXPECT_EQ(actual, expected);
        EXPECT_GT(actual, 0);
        EXPECT_LT(actual, std::numeric_limits<decltype(actual)>::max());
    };
    finite(profile.rendering.max_fixture_bytes, rendering.max_fixture_bytes);
    finite(profile.rendering.build_headroom_bytes, rendering.build_headroom_bytes);
    for (std::size_t tier = 0; tier < kFarTierCount; ++tier) {
        const auto& actual = profile.rendering.residency.queues[tier];
        const auto& expected = rendering.residency.queues[tier];
        finite(profile.rendering.budgets.bytes[tier], rendering.budgets.bytes[tier]);
        finite(profile.rendering.residency.entry_slots[tier],
               rendering.residency.entry_slots[tier]);
        finite(actual.descriptor_slots, expected.descriptor_slots);
        finite(actual.result_slots, expected.result_slots);
        finite(actual.max_converted_bytes, expected.max_converted_bytes);
        finite(actual.compilation.authority.max_height_samples,
               expected.compilation.authority.max_height_samples);
        finite(actual.compilation.authority.max_density_samples,
               expected.compilation.authority.max_density_samples);
        finite(actual.compilation.generation.max_candidate_bricks,
               expected.compilation.generation.max_candidate_bricks);
        finite(actual.compilation.generation.max_surface_bricks,
               expected.compilation.generation.max_surface_bricks);
        finite(actual.compilation.mesh.max_cells, expected.compilation.mesh.max_cells);
        finite(actual.compilation.mesh.max_sample_evaluations,
               expected.compilation.mesh.max_sample_evaluations);
        finite(actual.compilation.mesh.max_vertices, expected.compilation.mesh.max_vertices);
        finite(actual.compilation.mesh.max_indices, expected.compilation.mesh.max_indices);
    }
    EXPECT_TRUE(std::isfinite(profile.rendering.spans.min_y));
    EXPECT_TRUE(std::isfinite(profile.rendering.spans.max_y));
    EXPECT_TRUE(std::isfinite(profile.rendering.spans.max_extent_meters));
    EXPECT_EQ(profile.rendering.spans.min_y, rendering.spans.min_y);
    EXPECT_EQ(profile.rendering.spans.max_y, rendering.spans.max_y);
    EXPECT_EQ(profile.rendering.spans.max_extent_meters, rendering.spans.max_extent_meters);
    controller.Cancel();
    EXPECT_EQ(controller.Configure(jobs, 1, policy, profile), FarWorldRefusal::None);
    EXPECT_TRUE(controller.SourceConfig().source.memory);
}

TEST_F(FarWorldSessionController, SavedBindingBecomesEffectiveLadder) {
    EXPECT_EQ(renderer.Selection(FarWorldPurpose::World).effective, FarWorldMode::Legacy);
    ASSERT_NO_FATAL_FAILURE(Admit());
    ASSERT_NO_FATAL_FAILURE(FinishBuild());
    EXPECT_EQ(controller.phase(), Controller::Phase::Complete);
    EXPECT_EQ(renderer.Selection(FarWorldPurpose::World).effective, FarWorldMode::Ladder);
    EXPECT_EQ(renderer.Selection(FarWorldPurpose::Preview).effective, FarWorldMode::Legacy);
    EXPECT_EQ(renderer.Selection(FarWorldPurpose::OffscreenPreview).effective,
              FarWorldMode::Legacy);
    renderer.SetRequested(FarWorldMode::Legacy);
    EXPECT_EQ(renderer.Selection(FarWorldPurpose::World).effective, FarWorldMode::Legacy);
}

TEST_F(FarWorldSessionController, FailedOpenResumesAndSuccessfulSwapCancels) {
    ASSERT_TRUE(controller.Request(keys));
    ASSERT_EQ(controller.Pump(++frame), Controller::Phase::Planning);
    const auto* source = producer->capture();
    const auto serial = producer->capture_status().serving_serial;
    producer->fence_source_reads(true);
    // The host suspends controller pumps during a fallible open. Refusal resumes
    // the same binding before the next render; it does not cancel accepted state.
    EXPECT_EQ(controller.phase(), Controller::Phase::Planning);
    producer->fence_source_reads(false);
    ASSERT_NO_FATAL_FAILURE(Admit());
    EXPECT_EQ(producer->capture(), source);
    EXPECT_EQ(producer->capture_status().serving_serial, serial);
    ASSERT_NO_FATAL_FAILURE(FinishBuild());
    ASSERT_TRUE(controller.Request(keys));
    ASSERT_EQ(controller.Pump(++frame), Controller::Phase::Planning);
    ASSERT_EQ(controller.Pump(++frame), Controller::Phase::Planning);
    ASSERT_TRUE(producer->frozen_active());
    producer->retire(); // successful session adoption retires the previous epoch
    controller.Cancel();
    renderer.PrepareWorldSwap();
    EXPECT_FALSE(producer->frozen_active());
    EXPECT_EQ(producer->capture(), nullptr);
    EXPECT_EQ(controller.phase(), Controller::Phase::Idle);
    EXPECT_EQ(renderer.Selection(FarWorldPurpose::World).effective, FarWorldMode::Legacy);
    EXPECT_EQ(renderer.SessionDiagnostic(), nullptr);
}

class RefreshReceipt final : public spdlog::sinks::base_sink<std::mutex> {
public:
    // A separate scoped attachment below owns the sink; this class only stores
    // a bounded terminal line and count, never the complete logging stream.
    std::string line;
    unsigned receipts = 0;

private:
    void sink_it_(const spdlog::details::log_msg& message) override {
        const std::string_view text(message.payload.data(), message.payload.size());
        if (text.starts_with("Far ladder diagnostics refresh:") && text.size() <= 2048) {
            line.assign(text);
            ++receipts;
        }
    }
    void flush_() override {}
};

TEST_F(FarWorldSessionController, DiagnosticsReceiptsCoverCompletionRefusalAndCancellation) {
    const auto sink = std::make_shared<RefreshReceipt>();
    Log::GetDispatcherSink()->Attach(sink);
    struct Detach {
        const std::shared_ptr<RefreshReceipt>& sink;
        ~Detach() {
            Log::GetDispatcherSink()->Detach(sink);
        }
    } detach{sink};
    ASSERT_NO_FATAL_FAILURE(ConfigureSource());
    ASSERT_NO_FATAL_FAILURE(Admit());
    EXPECT_EQ(sink->receipts, 0u);
    ASSERT_NO_FATAL_FAILURE(FinishBuild());
    EXPECT_EQ(sink->receipts, 1u);
    for (const auto* field : {"reads=1 ",
                              "columns=",
                              "members=324 ",
                              "compressed_bytes=",
                              "decoded_bytes=",
                              "source_ops=",
                              "planning_ops=",
                              "admission_ops=",
                              "height_samples=",
                              "density_samples=",
                              "water_samples=",
                              "source_frames=1 ",
                              "preparation_frames=",
                              "elapsed_ticks=",
                              "refusal=None ",
                              "source_error=0 capture_error=0"})
        EXPECT_NE(sink->line.find(field), std::string::npos) << field;
    controller.Pump(++frame);
    EXPECT_EQ(sink->receipts, 1u);
    limits.transaction_samples.water = 1;
    ASSERT_NO_FATAL_FAILURE(ConfigureSource());
    ASSERT_TRUE(controller.Request(keys));
    ASSERT_EQ(controller.Pump(++frame), Controller::Phase::Planning);
    ASSERT_EQ(controller.Pump(++frame), Controller::Phase::Refused);
    EXPECT_EQ(sink->receipts, 2u);
    EXPECT_NE(sink->line.find("refusal=AdmissionFailed"), std::string::npos);
    EXPECT_NE(sink->line.find("water_samples=1 "), std::string::npos);
    controller.Pump(++frame);
    EXPECT_EQ(sink->receipts, 2u);
    ASSERT_NO_FATAL_FAILURE(ConfigureSource());
    ASSERT_TRUE(controller.Request(keys));
    ASSERT_EQ(controller.Pump(++frame), Controller::Phase::Planning);
    controller.Cancel();
    EXPECT_EQ(sink->receipts, 3u);
    EXPECT_NE(sink->line.find("refusal=ImmutableAuthorityUnavailable"), std::string::npos);
    // A new transaction that refuses before creating its sampler must not
    // report the previous transaction's samples.
    limits.transaction_samples.water = 2'000'000;
    ASSERT_NO_FATAL_FAILURE(ConfigureSource());
    ASSERT_NO_FATAL_FAILURE(Admit());
    ASSERT_NO_FATAL_FAILURE(FinishBuild());
    ASSERT_EQ(sink->receipts, 4u);
    ticks = std::numeric_limits<std::uint64_t>::max() - limits.refresh_ticks + 1;
    ASSERT_TRUE(controller.Request(keys));
    EXPECT_EQ(controller.Pump(++frame), Controller::Phase::Refused);
    EXPECT_EQ(sink->receipts, 5u);
    EXPECT_NE(sink->line.find("source_ops=0 planning_ops=0 admission_ops=0 "), std::string::npos);
    EXPECT_NE(sink->line.find("height_samples=0 density_samples=0 water_samples=0 "),
              std::string::npos);
}

TEST_F(FarWorldSessionController, InvalidatedSavedSourceRefusesWithExactFailure) {
    producer->invalidate_source();
    ASSERT_TRUE(controller.Request(keys));
    EXPECT_EQ(controller.Pump(++frame), Controller::Phase::Refused);
    EXPECT_EQ(controller.failure().source, Producer::Error::AuthorityUnavailable);
    EXPECT_EQ(renderer.Selection(FarWorldPurpose::World).effective, FarWorldMode::Legacy);
    EXPECT_EQ(producer->capture_progress().reads, 0u);
    EXPECT_FALSE(producer->capture_status().active);
}

TEST_F(FarWorldSessionController, RefusedReadRetainsAttemptAndByteMeasurements) {
    const auto path = Save::region_file_path(root, 0, 0);
    std::filesystem::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary);
    output << "partial";
    output.close();
    ASSERT_TRUE(output);
    ASSERT_TRUE(controller.Request(keys));
    ASSERT_EQ(controller.Pump(++frame), Controller::Phase::Refused);
    EXPECT_EQ(controller.failure().source, Producer::Error::CommittedReadRefused);
    EXPECT_EQ(producer->capture_progress().reads, 1u);
    EXPECT_GT(controller.work().source_read_bytes, 0u);
    EXPECT_EQ(controller.work().source_read_bytes, producer->capture_progress().compressed_bytes);
    EXPECT_EQ(controller.work().source_decode_work, producer->capture_progress().decoded_bytes);
    EXPECT_EQ(producer->capture(), nullptr);
}

TEST_F(FarWorldSessionController, FixtureWindowsReadAndOversizedWindowKeepsExactFailure) {
    // Cover the initial window and both successor directions under the
    // 512-member test profile. Each read retains exactly 324 absent-member records.
    const std::array<FarTileKey, 3> readable{{keys[0], {5, -1, 0}, {5, 1, 0}}};
    for (const auto& key : readable) {
        FarVolumeTileKey volume;
        Save::AuthorityWindow window;
        ASSERT_TRUE(ToFarVolumeTileKey(key, volume));
        ASSERT_TRUE(Producer::source_window(volume, window));
        const auto result = producer->read_committed(window, limits.capture.read);
        ASSERT_EQ(result.membership, Save::AuthorityMembership::ProvenAbsent) << result.error;
        EXPECT_EQ(result.members.size(), 324u);
        EXPECT_TRUE(result.chunks.empty());
        EXPECT_TRUE(result.error.empty());
    }
    // Deliberately inject a smaller test-local read profile. V5's 18x18
    // records exceed 64 even for an empty save, causing committed-read refusal.
    limits.capture.read.members = 64;
    ASSERT_NO_FATAL_FAILURE(ConfigureSource());
    const std::array<FarTileKey, 1> oversized{{{5, 0, 0}}};
    ASSERT_TRUE(controller.Request(oversized));
    EXPECT_EQ(controller.Pump(++frame), Controller::Phase::Refused);
    EXPECT_EQ(controller.failure().source, Producer::Error::CommittedReadRefused);
    EXPECT_EQ(controller.failure().capture, FarFrozenCaptureError::None);
    EXPECT_EQ(producer->capture(), nullptr);
    EXPECT_EQ(renderer.SessionMemoryPool(kFarTierCount)->refusals(), 0u);
}

TEST_F(FarWorldSessionController, UnsortedDuplicateWindowsReadOncePerTileInCanonicalOrder) {
    limits.operations_per_pump = 325;
    ASSERT_NO_FATAL_FAILURE(ConfigureSource());
    const std::array<FarTileKey, 3> unsorted{{{5, 1, 0}, {5, 0, 0}, {5, 1, 0}}};
    ASSERT_TRUE(controller.Request(unsorted));
    std::uint64_t observed = 0;
    const std::array<FarVolumeTileKey, 2> canonical{{{5, 0, 0}, {5, 1, 0}}};
    for (unsigned pump = 0; pump < limits.capture.deadline_frames; ++pump) {
        const auto phase = controller.Pump(++frame);
        ASSERT_NE(phase, Controller::Phase::Refused) << int(controller.failure().source);
        const auto progress = producer->capture_progress();
        if (progress.reads != observed) {
            ASSERT_EQ(progress.reads, observed + 1);
            Save::AuthorityWindow expected;
            ASSERT_TRUE(Producer::source_window(canonical[observed], expected));
            EXPECT_EQ(progress.last_window.min_chunk_x, expected.min_chunk_x);
            EXPECT_EQ(progress.last_window.min_chunk_z, expected.min_chunk_z);
            EXPECT_LE(progress.last_columns, limits.source.query_columns);
            EXPECT_LE(progress.last_members, limits.capture.read.members);
            observed = progress.reads;
        }
        EXPECT_LE(progress.operations, (pump + 1u) * limits.operations_per_pump);
        controller.Pump(frame); // duplicate frames neither advance nor age capture
        EXPECT_EQ(producer->capture_progress().operations, progress.operations);
        if (phase == Controller::Phase::Planning) {
            break;
        }
        EXPECT_EQ(producer->capture(), nullptr);
    }
    EXPECT_EQ(controller.phase(), Controller::Phase::Planning);
    EXPECT_EQ(observed, 2u);
    ASSERT_NE(producer->capture(), nullptr);
    EXPECT_EQ(producer->capture()->window_count, 2u);
    EXPECT_EQ(producer->capture_progress().columns, 544'968u);
    EXPECT_EQ(producer->capture_progress().members, 648u);
    EXPECT_EQ(producer->capture()->committed.members.size(), 612u);
}

TEST_F(FarWorldSessionController, PublicationDuringIncrementalSourceQueuesSuccessor) {
    limits.operations_per_pump = 325;
    ASSERT_NO_FATAL_FAILURE(ConfigureSource());
    auto live = std::make_shared<Chunk>(IVec3{0, 0, 0});
    live->current_water_resolution.store(4);
    live->water_bed_mm.assign(16, 0);
    live->water_depth_mm.assign(16, 0);
    live->set_water_authority();
    live->set_state(ChunkState::Idle);
    producer->changes()->membership(live, false, true);
    ASSERT_TRUE(controller.Request(keys));
    ASSERT_EQ(controller.Pump(++frame), Controller::Phase::Source);
    live->water_depth_mm[0] = 1000;
    live->mark_water_state_dirty();
    for (unsigned pump = 0; pump < 3; ++pump) {
        const auto phase = controller.Pump(++frame);
        ASSERT_NE(phase, Controller::Phase::Refused);
        if (phase == Controller::Phase::Planning) {
            break;
        }
    }
    EXPECT_EQ(controller.phase(), Controller::Phase::Planning);
    EXPECT_TRUE(producer->capture_status().queued);
    ASSERT_NE(producer->capture(), nullptr);
    ASSERT_EQ(producer->capture()->published.size(), 1u);
    EXPECT_EQ(producer->capture()->published[0].depth[0], 0);
}

TEST_F(FarWorldSessionController, SingleFrameCaptureDeadlineCompletesInOneSourcePump) {
    // Completeness may accept one frame because the source phase now advances
    // and ages exactly once per controller frame, including snapshot and read.
    limits.capture.deadline_frames = 1;
    ASSERT_TRUE(limits.Complete());
    ASSERT_NO_FATAL_FAILURE(ConfigureSource());
    ASSERT_TRUE(controller.Request(keys));
    EXPECT_EQ(controller.Pump(++frame), Controller::Phase::Planning);
    EXPECT_EQ(producer->capture_status().error, Producer::Error::None);
    EXPECT_EQ(producer->capture_progress().reads, 1u);
    EXPECT_TRUE(producer->capture_status().latest_ready);
}

TEST_F(FarWorldSessionController, SourceOperationExhaustionAndCancellationReleasePartialCapture) {
    for (const bool cancel : {false, true}) {
        SCOPED_TRACE(cancel);
        limits.operations_per_pump = 2;
        limits.admission_operations = 2;
        ASSERT_NO_FATAL_FAILURE(ConfigureSource());
        const auto pool = renderer.SessionMemoryPool(kFarTierCount);
        ASSERT_NE(pool, nullptr);
        const auto baseline = pool->used();
        ASSERT_TRUE(controller.Request(keys));
        ASSERT_EQ(controller.Pump(++frame), Controller::Phase::Source);
        ASSERT_EQ(producer->capture_progress().reads, 1u);
        ASSERT_TRUE(producer->capture_status().active);
        if (cancel) {
            controller.Cancel();
            EXPECT_EQ(controller.phase(), Controller::Phase::Idle);
            EXPECT_LT(pool->used(), baseline); // controller controls were released too
        } else {
            EXPECT_EQ(controller.Pump(++frame), Controller::Phase::Refused);
            EXPECT_EQ(controller.failure().capture, FarFrozenCaptureError::WorkLimit);
            EXPECT_EQ(pool->used(), baseline);
        }
        EXPECT_FALSE(producer->capture_status().active);
        EXPECT_EQ(producer->capture(), nullptr);
    }
}

TEST_F(FarWorldSessionController, IncrementalSourcePauseAndRefreshExpiryUnwind) {
    limits.operations_per_pump = 2;
    ASSERT_NO_FATAL_FAILURE(ConfigureSource());
    const auto pool = renderer.SessionMemoryPool(kFarTierCount);
    ASSERT_NE(pool, nullptr);
    const auto baseline = pool->used();
    ASSERT_TRUE(controller.Request(keys));
    ASSERT_EQ(controller.Pump(++frame), Controller::Phase::Source);
    producer->fence_source_reads(true);
    EXPECT_EQ(controller.Pump(++frame), Controller::Phase::Refused);
    EXPECT_EQ(controller.failure().source, Producer::Error::AuthorityUnavailable);
    EXPECT_FALSE(producer->capture_status().active);
    EXPECT_EQ(pool->used(), baseline);
    producer->fence_source_reads(false);
    limits.refresh_ticks = 3;
    ASSERT_NO_FATAL_FAILURE(ConfigureSource());
    const auto refreshed_pool = renderer.SessionMemoryPool(kFarTierCount);
    ASSERT_NE(refreshed_pool, nullptr);
    const auto refreshed_baseline = refreshed_pool->used();
    ticks = 0;
    clock_step = 1;
    ASSERT_TRUE(controller.Request(keys));
    EXPECT_EQ(controller.Pump(++frame), Controller::Phase::Refused);
    EXPECT_EQ(controller.failure().source, Producer::Error::DeadlineExceeded);
    EXPECT_EQ(producer->capture_progress().reads, 1u);
    EXPECT_FALSE(producer->capture_status().active);
    EXPECT_EQ(producer->capture(), nullptr);
    EXPECT_EQ(refreshed_pool->used(), refreshed_baseline);
}

TEST_F(FarWorldSessionController, ContinuousEditsCompleteActiveTransaction) {
    ASSERT_TRUE(controller.Request(keys));
    ASSERT_EQ(controller.Pump(++frame), Controller::Phase::Planning);
    const auto serial = producer->capture_status().serving_serial;
    const auto* source = producer->capture();
    std::uint64_t previous_planning = 0, previous_admission = 0;
    for (unsigned pump = 0; pump < 2048; ++pump) {
        producer->changes()->notify({0, 0, 0}, FarWorldAuthorityChange::Water);
        ASSERT_TRUE(controller.Request(keys));
        const auto phase = controller.Pump(++frame);
        ASSERT_NE(phase, Controller::Phase::Refused)
            << int(controller.failure().source) << ":" << int(controller.failure().capture) << ":"
            << int(renderer.Selection(FarWorldPurpose::World, true).refusal);
        EXPECT_EQ(producer->capture(), source);
        EXPECT_EQ(producer->capture_status().serving_serial, serial);
        EXPECT_LE(controller.work().capture_work - previous_planning, limits.operations_per_pump);
        EXPECT_LE(controller.work().admission_work - previous_admission,
                  limits.operations_per_pump);
        previous_planning = controller.work().capture_work;
        previous_admission = controller.work().admission_work;
        if (phase == Controller::Phase::Waiting)
            break;
    }
    ASSERT_EQ(controller.phase(), Controller::Phase::Waiting);
    ASSERT_NE(renderer.SessionDiagnostic(), nullptr);
    EXPECT_EQ(renderer.SessionDiagnostic()->state.target_serial, serial);
    EXPECT_TRUE(controller.Retains(keys[0]));
    EXPECT_FALSE(producer->frozen_active());
    EXPECT_GT(controller.work().admission_height_observations, 0u);
    const auto work = controller.work().admission_work;
    controller.Pump(frame); // duplicate frame must do no work
    EXPECT_EQ(controller.work().admission_work, work);
    const std::array<FarTileKey, 1> last{{{5, -1, 0}}};
    ASSERT_TRUE(controller.Request(last));
    ASSERT_NO_FATAL_FAILURE(FinishBuild());
    EXPECT_EQ(controller.phase(), Controller::Phase::Complete);
    EXPECT_EQ(producer->capture_status().serving_serial, serial);
    ASSERT_EQ(controller.Pump(++frame), Controller::Phase::Planning);
    EXPECT_GT(producer->capture_status().serving_serial, serial);
    FarVolumeTileKey volume;
    Save::AuthorityWindow expected;
    ASSERT_TRUE(ToFarVolumeTileKey(last[0], volume));
    ASSERT_TRUE(Producer::source_window(volume, expected));
    EXPECT_EQ(producer->capture()->window.min_chunk_x, expected.min_chunk_x);
    EXPECT_EQ(producer->capture()->window.max_chunk_x, expected.max_chunk_x);
}

TEST_F(FarWorldSessionController, ExpirySelectsLegacyWithExactSourceFailure) {
    ASSERT_TRUE(controller.Request(keys));
    ASSERT_EQ(controller.Pump(++frame), Controller::Phase::Planning);
    ticks = limits.capture.clock.max_ticks;
    ASSERT_EQ(controller.Pump(++frame), Controller::Phase::Refused);
    EXPECT_EQ(controller.failure().source, Producer::Error::SourceReadExpired);
    const auto selection = renderer.Selection(FarWorldPurpose::World);
    EXPECT_EQ(selection.effective, FarWorldMode::Legacy);
    EXPECT_EQ(selection.refusal, FarWorldRefusal::AdmissionFailed);
    CaptureDiagnostic();
    EXPECT_EQ(renderer.Diagnostic().source_failure.source, Producer::Error::SourceReadExpired);
    const auto refresh = renderer.Diagnostic().refresh_work;
    if (!refresh.has_value()) {
        FAIL() << "missing refresh measurements";
    }
    EXPECT_EQ(refresh->capture_work, controller.work().capture_work);
    EXPECT_FALSE(producer->frozen_active());
    EXPECT_EQ(renderer.Selection(FarWorldPurpose::Preview).refusal,
              FarWorldRefusal::PreviewOverride);
    renderer.SetRequestedCavePolicy(std::nullopt);
    EXPECT_EQ(renderer.Selection(FarWorldPurpose::World).refusal,
              FarWorldRefusal::InvalidCavePolicy);
}

TEST_F(FarWorldSessionController, RetirementPrunesOnlyAbsentKeys) {
    ASSERT_NO_FATAL_FAILURE(Admit());
    ASSERT_TRUE(controller.Retains(keys[0]));
    EXPECT_EQ(controller.RetireIdleHistory(), 0u);
    EXPECT_TRUE(controller.Retains(keys[0])); // current target is pinned
    ASSERT_NO_FATAL_FAILURE(FinishBuild());
    const std::array<FarTileKey, 1> replacement{{{5, 1, 0}}};
    ASSERT_TRUE(controller.Request(replacement));
    for (unsigned pump = 0; pump < 2048; ++pump) {
        const auto phase = controller.Pump(++frame);
        ASSERT_NE(phase, Controller::Phase::Refused);
        if (phase == Controller::Phase::Waiting)
            break;
    }
    ASSERT_EQ(controller.phase(), Controller::Phase::Waiting);
    EXPECT_TRUE(controller.Retains(keys[0])); // serving is still required
    EXPECT_TRUE(controller.Retains(replacement[0]));
    EXPECT_EQ(controller.RetireIdleHistory(), 0u);
    ASSERT_NO_FATAL_FAILURE(FinishBuild());
    EXPECT_EQ(controller.RetireIdleHistory(), 1u);
    EXPECT_FALSE(controller.Retains(keys[0]));
    EXPECT_TRUE(controller.Retains(replacement[0]));
    ASSERT_NE(renderer.SessionDiagnostic(), nullptr);
    ASSERT_EQ(renderer.SessionDiagnostic()->tile_count, 1u);
    EXPECT_EQ(renderer.SessionDiagnostic()->tiles[0].key, replacement[0]);
}

TEST_F(FarWorldSessionController, MissingLimitsRefuseBeforeAllocation) {
    controller.Cancel();
    const auto pool = renderer.SessionMemoryPool(kFarTierCount);
    ASSERT_NE(pool, nullptr);
    const auto attempts = pool->attempts();
    for (unsigned point = 0; point < 14; ++point) {
        auto invalid = limits;
        switch (point) {
            case 0:
                invalid.source.copy_bytes = 0;
                break;
            case 1:
                invalid.capture.read.probes = 0;
                break;
            case 2:
                invalid.capture.clock.now = nullptr;
                break;
            case 3:
                invalid.sampling.max_bytes = 0;
                break;
            case 4:
                invalid.transaction_samples.water = 0;
                break;
            case 5:
                invalid.tiles = 513;
                break;
            case 6:
                invalid.planning_operations = 0;
                break;
            case 7:
                invalid.admission_operations = 0;
                break;
            case 8:
                invalid.operations_per_pump = 0;
                break;
            case 9:
                invalid.refresh_ticks = 0;
                break;
            case 10:
                invalid.refresh_frames = 0;
                break;
            case 11:
                invalid.rendering.residency.queues[0].result_slots = 0;
                break;
            case 12:
                invalid.rendering.budgets.bytes[0] = 0;
                break;
            case 13:
                invalid.capture.read.decoded_member_bytes =
                    invalid.capture.read.decoded_total_bytes + 1;
                break;
            default:
                FAIL() << "unexpected case";
        }
        EXPECT_EQ(controller.Configure(jobs, 1, policy, invalid), FarWorldRefusal::MissingLimits);
        EXPECT_EQ(pool->attempts(), attempts);
        EXPECT_EQ(renderer.SessionMemoryPool(kFarTierCount), pool);
    }
    EXPECT_EQ(controller.Attach(producer, world), FarWorldRefusal::MissingLimits);
    EXPECT_FALSE(controller.Request(keys));
    EXPECT_EQ(controller.Pump(++frame), Controller::Phase::Idle);
}

TEST_F(FarWorldSessionController, FailuresPreserveTargetAndCancelUnwindsSampling) {
    ASSERT_NO_FATAL_FAILURE(Admit());
    const auto serial = renderer.SessionDiagnostic()->state.target_serial;
    const std::array<FarTileKey, 1> invalid{{{0, 0, 0}}};
    EXPECT_FALSE(controller.Request(invalid));
    EXPECT_FALSE(controller.Request({}));
    ticks = limits.refresh_ticks;
    EXPECT_EQ(controller.Pump(++frame), Controller::Phase::Refused);
    EXPECT_EQ(controller.failure().source, Producer::Error::DeadlineExceeded);
    EXPECT_EQ(renderer.SessionDiagnostic()->state.target_serial, serial);
    CaptureDiagnostic();
    EXPECT_EQ(renderer.Diagnostic().source_failure.source, Producer::Error::DeadlineExceeded);
    ASSERT_TRUE(controller.Request(keys));
    ASSERT_EQ(controller.Pump(++frame), Controller::Phase::Planning);
    ASSERT_EQ(controller.Pump(++frame), Controller::Phase::Planning);
    EXPECT_TRUE(producer->frozen_active());
    const auto pool = renderer.SessionMemoryPool(kFarTierCount);
    const auto memory = pool->used();
    controller.Cancel();
    EXPECT_FALSE(producer->frozen_active());
    EXPECT_LT(pool->used(), memory);
    EXPECT_EQ(renderer.SessionDiagnostic()->state.target_serial, serial);
}

TEST_F(FarWorldSessionController, PartialWaterAndUnreadableSourceKeepExactFailure) {
    auto water = std::make_shared<Chunk>(IVec3{0, 0, 0});
    water->set_water_authority();
    water->current_water_resolution.store(4);
    water->water_bed_mm.assign(16, 0);
    water->water_depth_mm.assign(15, 1);
    water->set_state(ChunkState::Idle);
    producer->changes()->membership(water, false, true);
    ASSERT_TRUE(controller.Request(keys));
    EXPECT_EQ(controller.Pump(++frame), Controller::Phase::Refused);
    EXPECT_EQ(controller.failure().source, Producer::Error::WaterUnavailable);
    EXPECT_EQ(renderer.Selection(FarWorldPurpose::World).refusal,
              FarWorldRefusal::WaterUnavailable);
    producer->changes()->membership(water, false, false);
    std::error_code error;
    std::filesystem::remove_all(root, error);
    ASSERT_FALSE(error);
    ASSERT_TRUE(controller.Request(keys));
    EXPECT_EQ(controller.Pump(++frame), Controller::Phase::Refused);
    EXPECT_EQ(controller.failure().source, Producer::Error::CommittedReadRefused);
    CaptureDiagnostic();
    EXPECT_EQ(renderer.Diagnostic().source_failure.source, Producer::Error::CommittedReadRefused);
}

TEST_F(FarWorldSessionController, AllocationRefusalUnwindsAndFallbackStaysLatchedDuringRetry) {
    const auto pool = renderer.SessionMemoryPool(kFarTierCount);
    const auto memory = pool->used();
    pool->RefuseAttemptsFrom(pool->attempts());
    ASSERT_TRUE(controller.Request(keys));
    EXPECT_EQ(controller.Pump(++frame), Controller::Phase::Refused);
    EXPECT_EQ(controller.failure().source, Producer::Error::AllocationFailure);
    EXPECT_EQ(pool->used(), memory);
    pool->RefuseAttemptsFrom(std::numeric_limits<std::uint64_t>::max());
    ASSERT_TRUE(controller.Request(keys));
    EXPECT_EQ(controller.Pump(++frame), Controller::Phase::Planning);
    EXPECT_EQ(renderer.Selection(FarWorldPurpose::World).effective, FarWorldMode::Legacy);
    EXPECT_EQ(renderer.Selection(FarWorldPurpose::World).refusal, FarWorldRefusal::AdmissionFailed);
    ASSERT_NO_FATAL_FAILURE(Admit());
    EXPECT_EQ(renderer.Selection(FarWorldPurpose::World).effective, FarWorldMode::Ladder);
}

TEST_F(FarWorldSessionController, FrameExpiryAndClockOverflowRefuseWithoutLosingSource) {
    ASSERT_TRUE(controller.Request(keys));
    ASSERT_EQ(controller.Pump(++frame), Controller::Phase::Planning);
    const auto* source = producer->capture();
    frame += limits.refresh_frames;
    EXPECT_EQ(controller.Pump(frame), Controller::Phase::Refused);
    EXPECT_EQ(controller.failure().source, Producer::Error::DeadlineExceeded);
    EXPECT_EQ(producer->capture(), source);
    ticks = std::numeric_limits<std::uint64_t>::max() - limits.refresh_ticks + 1;
    ASSERT_TRUE(controller.Request(keys));
    EXPECT_EQ(controller.Pump(++frame), Controller::Phase::Refused);
    EXPECT_EQ(controller.failure().source, Producer::Error::InvalidLimits);
    EXPECT_EQ(producer->capture(), source);
}

TEST_F(FarWorldSessionController, DiscoveredWaterSpanReachesHandoffAndDoesNotShrink) {
    auto water = std::make_shared<Chunk>(IVec3{0, 0, 0});
    water->set_water_authority();
    water->current_water_resolution.store(4);
    water->water_bed_mm.assign(16, 500'000);
    water->water_depth_mm.assign(16, 1000);
    water->set_state(ChunkState::Idle);
    producer->changes()->membership(water, false, true);
    ASSERT_NO_FATAL_FAILURE(Admit());
    const auto* diagnostic = renderer.SessionDiagnostic();
    ASSERT_NE(diagnostic, nullptr);
    ASSERT_EQ(diagnostic->tile_count, 1u);
    const auto initial = diagnostic->tiles[0].observation.target;
    if (!initial.has_value()) {
        FAIL() << "missing initial target";
    }
    const auto initial_span = initial->intent.extra_span;
    if (!initial_span.has_value()) {
        FAIL() << "missing discovered span";
    }
    EXPECT_GE(initial_span->max_y, 501);
    const auto generation = initial->identity.span_generation;
    ASSERT_NO_FATAL_FAILURE(FinishBuild());
    producer->changes()->membership(water, false, false);
    ASSERT_NO_FATAL_FAILURE(Admit());
    diagnostic = renderer.SessionDiagnostic();
    ASSERT_NE(diagnostic, nullptr);
    ASSERT_EQ(diagnostic->tile_count, 1u);
    const auto target = diagnostic->tiles[0].observation.target;
    if (!target.has_value()) {
        FAIL() << "missing replacement target";
    }
    const auto target_span = target->intent.extra_span;
    if (!target_span.has_value()) {
        FAIL() << "missing retained span";
    }
    EXPECT_EQ(target_span->min_y, initial_span->min_y);
    EXPECT_EQ(target_span->max_y, initial_span->max_y);
    EXPECT_EQ(target->identity.span_generation, generation);
}

TEST_F(FarWorldSessionController, OperationAllowancesStopActualPlanningAndAdmissionWork) {
    limits.planning_operations = 1;
    ASSERT_NO_FATAL_FAILURE(ConfigureSource());
    ASSERT_TRUE(controller.Request(keys));
    ASSERT_EQ(controller.Pump(++frame), Controller::Phase::Planning);
    EXPECT_EQ(controller.Pump(++frame), Controller::Phase::Refused);
    EXPECT_EQ(controller.failure().capture, FarFrozenCaptureError::WorkLimit);
    EXPECT_EQ(controller.work().capture_work, 1u);
    EXPECT_FALSE(producer->frozen_active());
    limits.planning_operations = 4'000'000;
    // Leave exactly the original one admission unit after charging source work
    // to the same transaction allowance.
    limits.admission_operations = producer->capture_progress().operations + 1;
    ASSERT_NO_FATAL_FAILURE(ConfigureSource());
    ASSERT_TRUE(controller.Request(keys));
    for (unsigned pump = 0; pump < 1024; ++pump) {
        const auto phase = controller.Pump(++frame);
        ASSERT_NE(phase, Controller::Phase::Refused);
        if (phase == Controller::Phase::Admission)
            break;
    }
    ASSERT_EQ(controller.phase(), Controller::Phase::Admission);
    EXPECT_EQ(controller.Pump(++frame), Controller::Phase::Admission);
    EXPECT_EQ(controller.work().admission_work, 1u);
    EXPECT_EQ(controller.Pump(++frame), Controller::Phase::Refused);
    EXPECT_EQ(controller.failure().capture, FarFrozenCaptureError::WorkLimit);
    EXPECT_FALSE(producer->frozen_active());
    EXPECT_EQ(renderer.SessionDiagnostic()->state.target_serial, 0u);
}

TEST_F(FarWorldSessionController, SamplingAndUnknownWaterMetadataRefusePrecisely) {
    limits.transaction_samples.water = 1;
    ASSERT_NO_FATAL_FAILURE(ConfigureSource());
    ASSERT_TRUE(controller.Request(keys));
    ASSERT_EQ(controller.Pump(++frame), Controller::Phase::Planning);
    EXPECT_EQ(controller.Pump(++frame), Controller::Phase::Refused);
    EXPECT_EQ(controller.failure().capture, FarFrozenCaptureError::WorkLimit);
    limits.transaction_samples.water = 2'000'000;
    policy.water_resolution = 3;
    ASSERT_NO_FATAL_FAILURE(ConfigureSource());
    ASSERT_TRUE(controller.Request(keys));
    ASSERT_EQ(controller.Pump(++frame), Controller::Phase::Planning);
    EXPECT_EQ(controller.Pump(++frame), Controller::Phase::Refused);
    EXPECT_EQ(controller.failure().source, Producer::Error::WaterUnavailable);
    EXPECT_EQ(renderer.Selection(FarWorldPurpose::World).refusal,
              FarWorldRefusal::WaterUnavailable);
}

TEST_F(FarWorldSessionController, ChangedWorldAndWrongMemoryDomainAreRejected) {
    ASSERT_TRUE(controller.Request(keys));
    ASSERT_EQ(controller.Pump(++frame), Controller::Phase::Planning);
    auto params = world->get_params();
    params.height_offset += 1;
    world->set_params(params);
    EXPECT_EQ(controller.Pump(++frame), Controller::Phase::Refused);
    EXPECT_EQ(controller.failure().capture, FarFrozenCaptureError::IdentityMismatch);
    controller.Cancel();
    ASSERT_EQ(controller.Configure(jobs, 1, policy, limits), FarWorldRefusal::None);
    EXPECT_EQ(controller.Attach({}, world), FarWorldRefusal::ImmutableAuthorityUnavailable);
    EXPECT_EQ(controller.Attach(producer, {}), FarWorldRefusal::ImmutableAuthorityUnavailable);
    EXPECT_EQ(controller.Attach(producer, world), FarWorldRefusal::MissingLimits);
    producer->retire();
    EXPECT_EQ(controller.Attach(producer, world), FarWorldRefusal::ImmutableAuthorityUnavailable);
}

TEST_F(FarWorldSessionController, SynchronousSourceCostAndRetiredBindingFailureAreVisible) {
    // A deterministic stepping clock makes synchronous cost measurable without
    // depending on machine speed, worker scheduling or wall-clock timers.
    clock_step = 1;
    ASSERT_TRUE(controller.Request(keys));
    ASSERT_EQ(controller.Pump(++frame), Controller::Phase::Planning);
    EXPECT_GT(controller.work().source_ms, 0);
    CaptureDiagnostic();
    const auto refresh = renderer.Diagnostic().refresh_work;
    if (!refresh.has_value()) {
        FAIL() << "missing refresh measurements";
    }
    EXPECT_EQ(refresh->source_ms, controller.work().source_ms);
    producer->retire();
    EXPECT_EQ(controller.Pump(++frame), Controller::Phase::Refused);
    EXPECT_EQ(controller.failure().source, Producer::Error::AuthorityUnavailable);
}
} // namespace
