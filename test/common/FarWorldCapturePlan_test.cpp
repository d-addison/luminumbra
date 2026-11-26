#include "../fixtures/TerrainFixtureParams.h"
#include "../support/UniqueTempPath.h"
#include "luminumbra_common/persistence/WorldSaveService.h"
#include "luminumbra_common/world/FarWorldCapturePlan.h"
#include "luminumbra_common/world/WorldStreamingState.h"
#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <limits>
#include <memory>
#include <string>
#include <vector>

namespace {
using namespace Luminumbra;
using namespace Luminumbra::World;
using Producer = FarWorldAuthorityProducer;
using Plan = World::FarWorldCapturePlan;
using Save = Persistence::WorldSaveService;

class FarWorldCapturePlan : public testing::Test {
protected:
    std::uint64_t ticks = 0;
    std::filesystem::path root = TestSupport::CreateUniqueTempDirectory("capture_plan_");
    std::shared_ptr<FarVolumeMemoryPool> pool =
        std::make_shared<FarVolumeMemoryPool>(64u * 1024u * 1024u);
    std::shared_ptr<Producer> producer =
        std::make_shared<Producer>(Producer::Limits{512, 16u * 1024u * 1024u, 8192, pool});
    Plan plan{pool};
    Save::AuthorityReadLimits reads{
        64, 512, 1024, 8192, 4194304, 16777216, 4194304, 16777216, 131072};
    std::shared_ptr<Systems::SHIELD_WorldSystem> world;
    Producer::FrozenPolicy policy;
    FarFrozenCaptureLimits sampling{1, 400'000, 1, 1'000'000, 1};
    Plan::Limits limits{512, 2'000'000, {-100'000, 100'000, 200'000}};
    void SetUp() override {
        auto params = Test::SyntheticTerrainParams();
        params.caves_enabled = false;
        params.base_amplitude = params.peaks_amplitude = params.height_offset = 0;
        params.continental_spline = {{-1, 0}, {1, 0}};
        world = std::make_shared<Systems::SHIELD_WorldSystem>(nullptr, nullptr, params, 1337);
        policy = {WorldFarVolumeIdentity(
                      *world, FarCaveMode::BandLimited, FarVolumeNumericProfile::RawFloat, 4),
                  FarCaveMode::BandLimited,
                  FarVolumeNumericProfile::RawFloat,
                  4};
        ASSERT_NE(policy.pristine_identity, 0u);
        const std::array<FarTileKey, 1> keys{{{1, 0, 0}}};
        ASSERT_EQ(plan.SetWanted(keys), Plan::Error::None);
        WorldStreamingState disk;
        ASSERT_TRUE(Save{}.save_world(disk, root));
        std::string error;
        ASSERT_TRUE(producer->admit_saved_source(root, reads, error)) << error;
        const Producer::CaptureClock clock{[](const void* context) noexcept {
                                               return *static_cast<const std::uint64_t*>(context);
                                           },
                                           &ticks,
                                           1000};
        ASSERT_TRUE(producer->configure_capture({reads, 4, clock}));
    }
    void TearDown() override {
        plan.Cancel();
        producer->retire();
        std::error_code error;
        std::filesystem::remove_all(root, error);
    }
    void Capture() {
        ASSERT_TRUE(producer->request_capture(plan.source_windows()));
        producer->pump_capture();
        producer->pump_capture();
        ASSERT_TRUE(producer->capture_status().latest_ready);
    }
    Plan::Result Finish(std::uint64_t allowance = 4096) {
        Plan::Result result;
        for (std::uint64_t work = 0; work < limits.operations + 1; work += allowance) {
            result = plan.Pump(allowance);
            EXPECT_LE(result.work, allowance);
            if (!result.ok() || result.complete) {
                return result;
            }
        }
        ADD_FAILURE() << "planning did not terminate within its operation bound";
        plan.Cancel();
        return {Plan::Error::WorkLimit};
    }
    void Build() {
        ASSERT_TRUE(plan.Start(producer, world, policy, sampling, limits).ok());
        const auto result = Finish();
        ASSERT_TRUE(result.ok()) << static_cast<int>(result.error) << ':'
                                 << static_cast<int>(result.source.source) << ':'
                                 << static_cast<int>(result.source.capture);
        ASSERT_TRUE(result.complete);
        ASSERT_NE(plan.accepted(), nullptr);
    }
    void StartSampling() {
        for (std::size_t index = 0; index < Producer::kSlots + 3; ++index) {
            ASSERT_TRUE(plan.Pump(1).ok());
            if (producer->frozen_active()) {
                return;
            }
        }
        FAIL() << "sampling did not start within the descriptor bound";
    }
    static std::shared_ptr<Chunk> Terrain(const IVec3& coords, float value = 1) {
        auto chunk = std::make_shared<Chunk>(coords);
        constexpr std::size_t samples =
            std::size_t{CHUNK_SIZE_X + 1} * (CHUNK_SIZE_Y + 1) * (CHUNK_SIZE_Z + 1);
        chunk->sdf_data.assign(samples, value);
        chunk->mark_voxel_data_dirty();
        chunk->set_state(ChunkState::Idle);
        return chunk;
    }
    static std::shared_ptr<Chunk>
    Water(const IVec3& coords, int resolution, std::int32_t height, std::int32_t depth) {
        auto chunk = std::make_shared<Chunk>(coords);
        chunk->set_water_authority();
        chunk->current_water_resolution.store(resolution);
        const auto cells =
            static_cast<std::size_t>(resolution) * static_cast<std::size_t>(resolution);
        chunk->water_bed_mm.assign(cells, height);
        chunk->water_depth_mm.assign(cells, depth);
        chunk->set_state(ChunkState::Idle);
        return chunk;
    }
};

TEST_F(FarWorldCapturePlan, NegativeBoundaryIncludesEveryHaloContributor) {
    const std::array<FarTileKey, 4> keys{{{2, 0, 0}, {1, -1, -1}, {1, -1, -1}, {1, 0, 0}}};
    ASSERT_EQ(plan.SetWanted(keys), Plan::Error::None);
    const auto wanted = plan.wanted_keys();
    ASSERT_EQ(wanted.size(), 3u);
    EXPECT_TRUE(std::is_sorted(wanted.begin(), wanted.end()));
    for (std::size_t index = 0; index < wanted.size(); ++index) {
        const auto tier = FarTierAt(wanted[index].tier);
        ASSERT_TRUE(tier);
        const auto& window = plan.source_windows()[index];
        const auto step = std::int64_t(tier->sample_spacing_meters);
        const auto edge = std::int64_t(tier->tile_edge_meters);
        // Enumerate every integer node, including every shared contributor in
        // the complete rectangular halo, independently of the window helper.
        for (auto z = wanted[index].tz * edge - step; z <= (wanted[index].tz + 1) * edge + step;
             ++z) {
            const auto low_z = static_cast<int>(std::floor(double(z - 1) / CHUNK_SIZE_Z));
            const auto high_z = static_cast<int>(std::floor(double(z) / CHUNK_SIZE_Z));
            EXPECT_LE(window.min_chunk_z, low_z);
            EXPECT_GE(window.max_chunk_z, high_z);
        }
        for (auto x = wanted[index].tx * edge - step; x <= (wanted[index].tx + 1) * edge + step;
             ++x) {
            const auto low_x = static_cast<int>(std::floor(double(x - 1) / CHUNK_SIZE_X));
            const auto high_x = static_cast<int>(std::floor(double(x) / CHUNK_SIZE_X));
            EXPECT_LE(window.min_chunk_x, low_x);
            EXPECT_GE(window.max_chunk_x, high_x);
        }
    }
    const auto memory = pool->used();
    ASSERT_EQ(plan.SetWanted(-0.25, -512.0), Plan::Error::None);
    std::vector<FarTileKey> expected;
    ASSERT_EQ(ComputeFarWantedSet(-0.25, -512.0, expected), FarWantedError::None);
    EXPECT_TRUE(std::equal(
        expected.begin(), expected.end(), plan.wanted_keys().begin(), plan.wanted_keys().end()));
    EXPECT_EQ(pool->used(), memory);
    for (const auto camera : {std::numeric_limits<double>::infinity(),
                              std::numeric_limits<double>::quiet_NaN(),
                              1e38}) {
        EXPECT_EQ(plan.SetWanted(camera, 0), Plan::Error::InvalidWanted);
    }
    EXPECT_EQ(plan.wanted_keys().size(), expected.size());
}

TEST_F(FarWorldCapturePlan, DeepEditAndWetBoundExtendCoverage) {
    ASSERT_NO_FATAL_FAILURE(Capture());
    ASSERT_NO_FATAL_FAILURE(Build());
    const auto initial = plan.accepted();
    ASSERT_EQ(initial->tiles.size(), 1u);
    const auto initial_span = initial->tiles[0].request.span;
    EXPECT_EQ(initial_span, (FarVolumeSpan{-256, 16}));
    const auto deep = Terrain({0, -40, 0});
    producer->changes()->membership(deep, false, true);
    // A sub-metre cell missed by the tier lattice must still extend coverage.
    const auto wet = Water({0, 0, 0}, 32, 200'000, 0);
    wet->water_depth_mm[33] = 1000;
    producer->changes()->membership(wet, true, true);
    ASSERT_NO_FATAL_FAILURE(Capture());
    ASSERT_NO_FATAL_FAILURE(Build());
    const auto extended = plan.accepted();
    ASSERT_EQ(extended->tiles.size(), 1u);
    EXPECT_LE(extended->tiles[0].request.span.min_y, -40 * CHUNK_SIZE_Y);
    EXPECT_GT(extended->tiles[0].request.span.max_y, 201);
    EXPECT_EQ(extended->tiles[0].span_generation, initial->tiles[0].span_generation + 1);
    EXPECT_EQ(initial->tiles[0].request.span, initial_span);
    producer->changes()->membership(deep, false, false);
    producer->changes()->membership(wet, true, false);
    ASSERT_NO_FATAL_FAILURE(Capture());
    ASSERT_NO_FATAL_FAILURE(Build());
    EXPECT_EQ(plan.accepted()->tiles[0].request.span, extended->tiles[0].request.span);
    EXPECT_EQ(plan.accepted()->tiles[0].span_generation, extended->tiles[0].span_generation);
    ASSERT_EQ(plan.SetWanted(std::span<const FarTileKey>{}), Plan::Error::None);
    ASSERT_NO_FATAL_FAILURE(Build());
    EXPECT_TRUE(plan.accepted()->tiles.empty());
    const std::array<FarTileKey, 1> keys{{{1, 0, 0}}};
    ASSERT_EQ(plan.SetWanted(keys), Plan::Error::None);
    ASSERT_NO_FATAL_FAILURE(Build());
    EXPECT_EQ(plan.accepted()->tiles[0].request.span, extended->tiles[0].request.span);
    EXPECT_EQ(plan.accepted()->tiles[0].span_generation, extended->tiles[0].span_generation);
}

TEST_F(FarWorldCapturePlan, RefusalPreservesAcceptedPlan) {
    ASSERT_NO_FATAL_FAILURE(Capture());
    ASSERT_NO_FATAL_FAILURE(Build());
    const auto accepted = plan.accepted();
    const auto retained = pool->used();
    auto constrained = limits;
    constrained.tiles = 0;
    EXPECT_EQ(plan.Start(producer, world, policy, sampling, constrained).error,
              Plan::Error::InvalidLimits);
    constrained = limits;
    constrained.spans.max_extent_meters = 1;
    ASSERT_TRUE(plan.Start(producer, world, policy, sampling, constrained).ok());
    EXPECT_EQ(Finish().error, Plan::Error::InvalidSpan);
    EXPECT_EQ(plan.accepted(), accepted);
    pool->RefuseAttemptsFrom(pool->attempts());
    EXPECT_EQ(plan.Start(producer, world, policy, sampling, limits).error,
              Plan::Error::AllocationFailure);
    pool->RefuseAttemptsFrom(std::numeric_limits<std::uint64_t>::max());
    EXPECT_EQ(pool->used(), retained);
    ASSERT_TRUE(plan.Start(producer, world, policy, sampling, limits).ok());
    pool->RefuseAttemptsFrom(pool->attempts());
    const auto allocation = Finish();
    EXPECT_EQ(allocation.error, Plan::Error::SourceRefused);
    EXPECT_EQ(allocation.source.source, Producer::Error::AllocationFailure);
    pool->RefuseAttemptsFrom(std::numeric_limits<std::uint64_t>::max());
    EXPECT_EQ(plan.accepted(), accepted);
    EXPECT_EQ(pool->used(), retained);
    std::array<FarTileKey, Producer::kSlots + 1> excessive{};
    EXPECT_EQ(plan.SetWanted(excessive), Plan::Error::Capacity);
    const std::array<FarTileKey, 1> invalid{{{0, 0, 0}}};
    EXPECT_EQ(plan.SetWanted(invalid), Plan::Error::InvalidWanted);
    EXPECT_EQ(plan.wanted_keys().size(), 1u);
    // Distinct history and wanted keys would exceed the fixed history envelope.
    std::array<FarTileKey, Producer::kSlots> distinct{};
    for (std::size_t index = 0; index < distinct.size(); ++index) {
        distinct[index] = {1, static_cast<std::int64_t>(index) + 1, 0};
    }
    ASSERT_EQ(plan.SetWanted(distinct), Plan::Error::None);
    ASSERT_TRUE(plan.Start(producer, world, policy, sampling, limits).ok());
    EXPECT_EQ(Finish().error, Plan::Error::Capacity);
    EXPECT_EQ(plan.accepted(), accepted);
    const std::array<FarTileKey, 1> original{{{1, 0, 0}}};
    ASSERT_EQ(plan.SetWanted(original), Plan::Error::None);
    // Source copy refuses partial water without replacing accepted source/plan.
    const auto partial = Water({0, 0, 0}, 4, 0, 1);
    partial->water_depth_mm.pop_back();
    producer->changes()->membership(partial, false, true);
    producer->pump_capture();
    EXPECT_EQ(producer->capture_status().error, Producer::Error::WaterUnavailable);
    EXPECT_EQ(plan.Start(producer, world, policy, sampling, limits).error,
              Plan::Error::SourceRefused);
    EXPECT_EQ(plan.accepted(), accepted);
}

TEST_F(FarWorldCapturePlan, PumpsRespectOperationAllowance) {
    ASSERT_NO_FATAL_FAILURE(Capture());
    ASSERT_TRUE(plan.Start(producer, world, policy, sampling, limits).ok());
    EXPECT_EQ(plan.Pump(0).work, 0u);
    EXPECT_EQ(plan.accepted(), nullptr);
    for (unsigned index = 0; index < 20; ++index) {
        const auto result = plan.Pump(1);
        ASSERT_TRUE(result.ok());
        EXPECT_EQ(result.work, 1u);
        EXPECT_FALSE(result.complete);
    }
    const auto result = Finish(7);
    ASSERT_TRUE(result.ok());
    ASSERT_TRUE(result.complete);
    const auto accepted = plan.accepted();
    auto short_limit = limits;
    short_limit.operations = 2;
    ASSERT_TRUE(plan.Start(producer, world, policy, sampling, short_limit).ok());
    const auto exhausted = plan.Pump(100);
    EXPECT_EQ(exhausted.error, Plan::Error::WorkLimit);
    EXPECT_EQ(exhausted.work, 2u);
    EXPECT_FALSE(producer->frozen_active());
    EXPECT_EQ(plan.accepted(), accepted);
    auto short_sampling = sampling;
    short_sampling.max_water_samples = 1;
    ASSERT_TRUE(plan.Start(producer, world, policy, short_sampling, limits).ok());
    const auto sampled = Finish();
    EXPECT_EQ(sampled.source.capture, FarFrozenCaptureError::WorkLimit);
    EXPECT_EQ(plan.accepted(), accepted);
    const std::array<FarTileKey, 2> keys{{{1, 0, 0}, {1, 1, 0}}};
    ASSERT_EQ(plan.SetWanted(keys), Plan::Error::None);
    ASSERT_NO_FATAL_FAILURE(Capture());
    auto aggregate_sampling = sampling;
    // Exactly one pristine tile's halo-water and surface height calls. Each
    // tile fits alone; the second must refuse the shared transaction allowance.
    aggregate_sampling.max_height_samples = 131u * 131u + 129u * 129u;
    ASSERT_TRUE(plan.Start(producer, world, policy, aggregate_sampling, limits).ok());
    EXPECT_EQ(Finish().source.capture, FarFrozenCaptureError::WorkLimit);
    EXPECT_EQ(plan.accepted(), accepted);
}

TEST_F(FarWorldCapturePlan, EveryPlanReservationUnwindsAndInvalidInputsPreservePublication) {
    EXPECT_THROW(Plan{std::shared_ptr<FarVolumeMemoryPool>{}}, FarVolumeMemoryRefused);
    const auto controls = pool->used();
    for (std::uint64_t point = 0; point < 2; ++point) {
        pool->RefuseAttemptsFrom(pool->attempts() + point);
        EXPECT_THROW(Plan{pool}, FarVolumeMemoryRefused);
        pool->RefuseAttemptsFrom(std::numeric_limits<std::uint64_t>::max());
        EXPECT_EQ(pool->used(), controls);
    }
    ASSERT_NO_FATAL_FAILURE(Capture());
    ASSERT_NO_FATAL_FAILURE(Build());
    const auto accepted = plan.accepted();
    const auto retained = pool->used();
    const auto before = pool->attempts();
    ASSERT_TRUE(plan.Start(producer, world, policy, sampling, limits).ok());
    const auto reservations = pool->attempts() - before;
    ASSERT_LT(reservations, 8u);
    plan.Cancel();
    for (std::uint64_t point = 0; point < reservations; ++point) {
        pool->RefuseAttemptsFrom(pool->attempts() + point);
        EXPECT_EQ(plan.Start(producer, world, policy, sampling, limits).error,
                  Plan::Error::AllocationFailure);
        pool->RefuseAttemptsFrom(std::numeric_limits<std::uint64_t>::max());
        EXPECT_FALSE(plan.active());
        EXPECT_EQ(plan.accepted(), accepted);
        EXPECT_EQ(pool->used(), retained);
    }
    EXPECT_EQ(plan.Start({}, world, policy, sampling, limits).error, Plan::Error::SourceRefused);
    EXPECT_EQ(plan.Start(producer, {}, policy, sampling, limits).error, Plan::Error::SourceRefused);
    for (unsigned point = 0; point < 9; ++point) {
        auto invalid_limits = limits;
        switch (point) {
            case 0:
                invalid_limits.tiles = Producer::kSlots + 1;
                break;
            case 1:
                invalid_limits.operations = 0;
                break;
            case 2:
                invalid_limits.spans.min_y = std::numeric_limits<float>::quiet_NaN();
                break;
            case 3:
                invalid_limits.spans.max_y = std::numeric_limits<float>::infinity();
                break;
            case 4:
                invalid_limits.spans.min_y = invalid_limits.spans.max_y + 1;
                break;
            case 5:
                invalid_limits.spans.min_y = -kFarVolumeExactCoordinateLimitMeters;
                break;
            case 6:
                invalid_limits.spans.max_y = kFarVolumeExactCoordinateLimitMeters;
                break;
            case 7:
                invalid_limits.spans.max_extent_meters = 0;
                break;
            case 8:
                invalid_limits.spans.max_extent_meters = std::numeric_limits<float>::quiet_NaN();
                break;
            default:
                FAIL() << "unexpected case";
        }
        EXPECT_EQ(plan.Start(producer, world, policy, sampling, invalid_limits).error,
                  Plan::Error::InvalidLimits);
        EXPECT_EQ(plan.accepted(), accepted);
    }
    auto invalid_policy = policy;
    invalid_policy.water_resolution = 3;
    ASSERT_TRUE(plan.Start(producer, world, invalid_policy, sampling, limits).ok());
    EXPECT_EQ(Finish().source.source, Producer::Error::WaterUnavailable);
    invalid_policy = policy;
    invalid_policy.pristine_identity ^= 1;
    ASSERT_TRUE(plan.Start(producer, world, invalid_policy, sampling, limits).ok());
    EXPECT_EQ(Finish().source.capture, FarFrozenCaptureError::IdentityMismatch);
    // Coverage cannot use an authority window containing only the tile interior.
    const std::array<Save::AuthorityWindow, 1> incomplete{{{0, 31, 0, 31}}};
    ASSERT_TRUE(producer->request_capture(incomplete));
    producer->pump_capture();
    producer->pump_capture();
    ASSERT_TRUE(plan.Start(producer, world, policy, sampling, limits).ok());
    EXPECT_EQ(Finish().source.source, Producer::Error::InvalidWindow);
    EXPECT_EQ(plan.accepted(), accepted);
}

TEST_F(FarWorldCapturePlan, DiskOnlyTerrainInNormalHaloExtendsCoverage) {
    WorldStreamingState disk;
    ASSERT_TRUE(disk.insert_chunk(Terrain({-1, -60, -1})));
    ASSERT_TRUE(Save{}.save_world(disk, root));
    std::string error;
    ASSERT_TRUE(producer->admit_saved_source(root, reads, error)) << error;
    ASSERT_NO_FATAL_FAILURE(Capture());
    ASSERT_TRUE(producer->capture()->published.empty());
    ASSERT_EQ(producer->capture()->committed.chunks.size(), 1u);
    ASSERT_NO_FATAL_FAILURE(Build());
    EXPECT_LE(plan.accepted()->tiles[0].request.span.min_y, -60 * CHUNK_SIZE_Y);
}

TEST_F(FarWorldCapturePlan, WaterResolutionPrecedenceAndAmbiguityUseImmutableImages) {
    WorldStreamingState disk;
    const auto committed = Water({0, 0, 0}, 32, 900'000, 1000);
    ASSERT_TRUE(disk.insert_chunk(committed));
    ASSERT_TRUE(Save{}.save_world(disk, root));
    std::string error;
    ASSERT_TRUE(producer->admit_saved_source(root, reads, error)) << error;
    producer->changes()->membership(Water({0, 0, 0}, 8, 500'000, 1000), true, true);
    const auto dry = Water({0, 0, 0}, 4, 700'000, 0);
    producer->changes()->membership(dry, false, true);
    ASSERT_NO_FATAL_FAILURE(Capture());
    // Mutation after source publication cannot change the accepted image.
    dry->water_depth_mm.assign(16, 1000);
    ASSERT_NO_FATAL_FAILURE(Build());
    EXPECT_EQ(plan.accepted()->tiles[0].request.span.max_y, 16);
    const auto accepted = plan.accepted();
    producer->changes()->membership(Water({0, 1, 0}, 4, 0, 1), false, true);
    ASSERT_NO_FATAL_FAILURE(Capture());
    ASSERT_TRUE(plan.Start(producer, world, policy, sampling, limits).ok());
    const auto result = Finish();
    EXPECT_EQ(result.source.source, Producer::Error::WaterUnavailable);
    EXPECT_EQ(plan.accepted(), accepted);
}

TEST_F(FarWorldCapturePlan, SourceReplacementGeneratorChangeAndExpiryRefuse) {
    ASSERT_NO_FATAL_FAILURE(Capture());
    ASSERT_NO_FATAL_FAILURE(Build());
    const auto accepted = plan.accepted();
    ASSERT_TRUE(plan.Start(producer, world, policy, sampling, limits).ok());
    ASSERT_TRUE(plan.Pump(1).ok());
    ASSERT_NO_FATAL_FAILURE(Capture());
    EXPECT_EQ(plan.Pump(1).error, Plan::Error::SourceChanged);
    EXPECT_EQ(plan.accepted(), accepted);
    ASSERT_TRUE(plan.Start(producer, world, policy, sampling, limits).ok());
    auto params = world->get_params();
    params.height_offset += 1;
    world->set_params(params);
    EXPECT_EQ(plan.Pump(1).error, Plan::Error::SourceChanged);
    EXPECT_EQ(plan.accepted(), accepted);
    ASSERT_TRUE(plan.Start(producer, world, policy, sampling, limits).ok());
    ticks = 1000;
    const auto expired = plan.Pump(1);
    EXPECT_EQ(expired.source.capture, FarFrozenCaptureError::DeadlineExceeded);
    EXPECT_EQ(plan.accepted(), accepted);
}

TEST_F(FarWorldCapturePlan, EmptyWantedPublishesAndCancellationReleasesRetainedOwners) {
    ASSERT_NO_FATAL_FAILURE(Capture());
    ASSERT_NO_FATAL_FAILURE(Build());
    const auto accepted = plan.accepted();
    ASSERT_EQ(plan.SetWanted(std::span<const FarTileKey>{}), Plan::Error::None);
    ASSERT_TRUE(plan.Start(producer, world, policy, sampling, limits).ok());
    EXPECT_EQ(plan.SetWanted(0, 0), Plan::Error::SourceRefused);
    EXPECT_TRUE(Finish().complete);
    EXPECT_TRUE(plan.accepted()->tiles.empty());
    EXPECT_EQ(accepted->tiles.size(), 1u);
    const std::array<FarTileKey, 1> keys{{{1, 0, 0}}};
    ASSERT_EQ(plan.SetWanted(keys), Plan::Error::None);
    ASSERT_TRUE(plan.Start(producer, world, policy, sampling, limits).ok());
    ASSERT_NO_FATAL_FAILURE(StartSampling());
    EXPECT_TRUE(producer->frozen_active());
    plan.Cancel();
    EXPECT_FALSE(producer->frozen_active());
    EXPECT_FALSE(plan.active());
    EXPECT_EQ(plan.Pump(1).error, Plan::Error::Inactive);
    const auto other =
        std::make_shared<Systems::SHIELD_WorldSystem>(nullptr, nullptr, world->get_params(), 1337);
    EXPECT_EQ(plan.Start(producer, other, policy, sampling, limits).error,
              Plan::Error::SourceChanged);
    plan.Reset();
    EXPECT_EQ(plan.accepted(), nullptr);
    EXPECT_TRUE(plan.wanted_keys().empty());
    ASSERT_EQ(plan.SetWanted(keys), Plan::Error::None);
    ASSERT_TRUE(plan.Start(producer, other, policy, sampling, limits).ok());
    EXPECT_TRUE(Finish().ok());
}
} // namespace
