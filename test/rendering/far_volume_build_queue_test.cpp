#include "luminumbra_client/rendering/FarVolumeBuildQueue.h"
#include "luminumbra_common/systems/SHIELD_WorldSystem.h"
#include "luminumbra_common/world/FarLodStore.h"

#include <gtest/gtest.h>

#include <array>
#include <bit>
#include <chrono>
#include <condition_variable>
#include <future>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>

namespace Luminumbra::Rendering {
struct FarVolumeBuildQueueTestAccess {
    using Phase = FarVolumeBuildQueue::Phase;
    using Hook = FarVolumeBuildQueue::PhaseHook;
    static void hook(FarVolumeBuildQueue& queue, Hook function, void* context) {
        queue.set_test_hook(function, context);
    }
    static void sequence(FarVolumeBuildQueue& queue, std::uint64_t sequence) {
        queue.set_test_sequence(sequence);
    }
};
} // namespace Luminumbra::Rendering

namespace {
using namespace Luminumbra;
using namespace Luminumbra::Rendering;
using namespace Luminumbra::World;
using Luminumbra::Systems::SHIELD_WorldSystem;
using Luminumbra::Systems::TerrainGenParams;
using Access = FarVolumeBuildQueueTestAccess;
using Phase = Access::Phase;

enum class Fault {
    None,
    Allocation,
    Length,
    Invalid,
    Other,
    Unknown,
    CorruptTile,
    EmptyTile,
    BadIndex,
    WrongIdentity,
    TruncatedSpan,
    BrickCapacity,
    GeometryCapacity
};

struct WorkerGate {
    std::mutex mutex;
    std::condition_variable changed;
    Phase phase = Phase::BeforeSampling;
    Fault fault = Fault::None;
    bool hold = false, opened = false, on_owner = false;
    std::size_t entered = 0;
    std::array<std::size_t, 5> calls{};
    const std::thread::id owner = std::this_thread::get_id();

    static void invoke(void* context,
                       Phase stage,
                       const FarVolumeBuildIdentity&,
                       FarVolumeTile* tile,
                       FarVolumeMesh* mesh) {
        auto& self = *static_cast<WorkerGate*>(context);
        Fault fault = Fault::None;
        {
            std::unique_lock lock(self.mutex);
            ++self.calls[static_cast<std::size_t>(stage)];
            self.on_owner = self.on_owner || std::this_thread::get_id() == self.owner;
            if (stage != self.phase)
                return;
            ++self.entered;
            self.changed.notify_all();
            self.changed.wait(lock, [&] { return !self.hold || self.opened; });
            fault = self.fault;
        }
        switch (fault) {
            case Fault::None:
                break;
            case Fault::Allocation:
                throw std::bad_alloc();
            case Fault::Length:
                throw std::length_error("fixture work budget");
            case Fault::Invalid:
                throw std::invalid_argument("fixture invalid stream");
            case Fault::Other:
                throw std::runtime_error(std::string(512, 'x'));
            case Fault::Unknown:
                throw 7;
            case Fault::CorruptTile:
                tile->crc32 ^= 1u;
                break;
            case Fault::EmptyTile:
                // Synthetic completed empty stream, explicitly not pristine-world
                // evidence. It exercises the queue's valid-empty result boundary.
                tile->bricks.clear();
                tile->crc32 = FarVolumeTileCrc(*tile);
                break;
            case Fault::BadIndex:
                mesh->indices.front() = std::numeric_limits<std::uint32_t>::max();
                break;
            case Fault::WrongIdentity:
                tile->caves = FarCaveMode::BoxFiltered;
                tile->crc32 = FarVolumeTileCrc(*tile);
                break;
            case Fault::BrickCapacity:
                tile->bricks.reserve(tile->bricks.capacity() * 4);
                break;
            case Fault::GeometryCapacity:
                mesh->vertices.reserve(mesh->vertices.capacity() * 2);
                break;
            case Fault::TruncatedSpan:
                tile->first_brick_y = 0;
                tile->sampled_bricks = 1024ull * static_cast<std::uint64_t>(tile->last_brick_y);
                tile->crc32 = FarVolumeTileCrc(*tile);
                break;
        }
    }
    bool wait(std::size_t count) {
        std::unique_lock lock(mutex);
        return changed.wait_for(lock, std::chrono::seconds(30), [&] { return entered >= count; });
    }
    void open() {
        std::lock_guard lock(mutex);
        opened = true;
        changed.notify_all();
    }
};

TerrainGenParams FlatParams() {
    TerrainGenParams params;
    params.base_amplitude = 0;
    params.height_offset = 12.25f;
    params.caves_enabled = false;
    params.island_mask_enabled = false;
    return params;
}

struct Rig {
    WorkerGate gate;
    JobSystem jobs;
    SHIELD_WorldSystem world{nullptr, nullptr, FlatParams(), 1337};
    std::unique_ptr<FarVolumeBuildQueue> queue;
    explicit Rig(FarVolumeBuildLimits limits = {}) {
        jobs.startup(2);
        queue = std::make_unique<FarVolumeBuildQueue>(jobs, limits);
        if (!queue->bind_fixture_world(world))
            throw std::runtime_error("Fresh fixture rejected");
    }
    ~Rig() {
        gate.open();
        queue.reset();
        jobs.shutdown();
    }
    void
    observe(Phase phase = Phase::BeforeSampling, bool hold = false, Fault fault = Fault::None) {
        gate.phase = phase;
        gate.hold = hold;
        gate.fault = fault;
        Access::hook(*queue, WorkerGate::invoke, &gate);
    }
};

std::uint64_t MeshHash(const FarVolumeRenderMesh& mesh) {
    std::uint64_t value = 14695981039346656037ull;
    const auto mix = [&](std::uint32_t word) {
        for (unsigned byte = 0; byte < 4; ++byte) {
            value ^= (word >> (byte * 8)) & 255u;
            value *= 1099511628211ull;
        }
    };
    for (const auto& vertex : mesh.vertices) {
        for (const auto scalar : {vertex.position.x,
                                  vertex.position.y,
                                  vertex.position.z,
                                  vertex.normal.x,
                                  vertex.normal.y,
                                  vertex.normal.z})
            mix(std::bit_cast<std::uint32_t>(scalar));
        mix(vertex.material_id);
    }
    for (const auto index : mesh.indices)
        mix(index);
    return value;
}

std::optional<FarVolumeResultLease> Complete(Rig& rig, const FarVolumeRequest& request) {
    EXPECT_EQ(rig.queue->submit(request).status, FarVolumeSubmitStatus::Queued);
    EXPECT_TRUE(rig.queue->pump());
    rig.queue->drain();
    auto result = rig.queue->take_completed();
    EXPECT_TRUE(result.has_value());
    return result;
}

void CheckRealTierResult(const FarVolumeBuildResult& result,
                         const SHIELD_WorldSystem& world,
                         std::uint32_t tier,
                         FarCaveMode mode,
                         std::uint64_t hash) {
    ASSERT_EQ(result.status, FarVolumeBuildStatus::Ready);
    ASSERT_TRUE(result.span_complete);
    if (!result.mesh.bounds.has_value()) {
        FAIL() << "Expected result.mesh.bounds to contain a value";
    }
    const auto dimensions = FarTierAt(tier);
    if (!dimensions.has_value()) {
        FAIL() << "Expected dimensions to contain a value";
    }
    EXPECT_LE(result.first_brick_y * static_cast<int>(dimensions.value().brick_edge_meters), -400);
    EXPECT_GT(result.last_brick_y * static_cast<int>(dimensions.value().brick_edge_meters), 70);
    EXPECT_EQ(result.identity.effective_request.tier, tier);
    EXPECT_EQ(result.identity.effective_request.caves, mode);
    EXPECT_EQ(result.identity.seed, 1337);
    EXPECT_EQ(result.identity.params_hash, ComputeTerrainParamsHash(world.get_params(), 1337));
    EXPECT_EQ(result.identity.authority_revision, 0u);
    EXPECT_GT(result.sampled_bricks, 0u);
    EXPECT_LE(result.density_samples, 8'000'000u);
    EXPECT_LE(result.mesh.bounds.value().max.x, 0.0f);
    EXPECT_LT(result.mesh.bounds.value().max.z, 0.0f);
    const auto prefix =
        "tier" + std::to_string(tier) + "_mode" + std::to_string(static_cast<int>(mode)) + "_";
    const auto record = [&](const char* name, auto value) {
        ::testing::Test::RecordProperty(prefix + name, std::to_string(value));
    };
    record("mesh_hash", hash);
    record("tile_crc32", result.tile_crc32);
    record("first_brick_y", result.first_brick_y);
    record("last_brick_y", result.last_brick_y);
    record("sampled_bricks", result.sampled_bricks);
    record("density_samples", result.density_samples);
    record("tile_capacity_bytes", result.tile_capacity_bytes);
    record("geometry_capacity_bytes", result.geometry_capacity_bytes);
    record("converted_capacity_bytes", result.mesh.owned_bytes());
    record("vertices", result.mesh.vertices.size());
    record("indices", result.mesh.indices.size());
    record("bounds_min_y", result.mesh.bounds.value().min.y);
    record("bounds_max_y", result.mesh.bounds.value().max.y);
    EXPECT_LE(result.tile_capacity_bytes, 128u * 1024u * 1024u);
    EXPECT_LE(result.geometry_capacity_bytes, 64u * 1024u * 1024u);
    EXPECT_LE(result.mesh.owned_bytes(), 64u * 1024u * 1024u);
}

TEST(FarVolumeBuildQueue, RealFiveTiersBothModesAndExtendedSpansAreRepeatable) {
    Rig rig;
    rig.observe();
    for (std::uint32_t tier = 1; tier <= 5; ++tier) {
        for (const auto mode : {FarCaveMode::BandLimited, FarCaveMode::BoxFiltered}) {
            SCOPED_TRACE(::testing::Message()
                         << "tier=" << tier << " mode=" << static_cast<int>(mode));
            const FarVolumeRequest request{tier, -1, -2, FarVolumeSpan{-400, 70}, mode};
            std::uint64_t previous_hash = 0;
            std::uint32_t previous_crc = 0;
            for (int repeat = 0; repeat < 2; ++repeat) {
                auto lease = Complete(rig, request);
                if (!lease.has_value()) {
                    FAIL() << "Expected lease to contain a value";
                }
                const auto& result = lease.value().result();
                const auto hash = MeshHash(result.mesh);
                if (repeat != 0) {
                    EXPECT_EQ(hash, previous_hash);
                    EXPECT_EQ(result.tile_crc32, previous_crc);
                }
                previous_hash = hash;
                previous_crc = result.tile_crc32;
                CheckRealTierResult(result, rig.world, tier, mode, hash);
                EXPECT_TRUE(rig.queue->is_current(result.identity));
                const auto generation = result.identity.request_generation;
                lease.reset();
                EXPECT_TRUE(rig.queue->forget(generation));
                EXPECT_EQ(rig.queue->stats().payload_reserved_bytes, 0u);
            }
        }
    }
    EXPECT_FALSE(rig.gate.on_owner);
    EXPECT_EQ(rig.gate.calls[0], 20u);
    const auto stats = rig.queue->stats();
    RecordProperty("control_storage_bytes", std::to_string(stats.control_storage_bytes));
    RecordProperty("descriptor_stride", std::to_string(stats.descriptor_stride));
    RecordProperty("result_slot_stride", std::to_string(stats.result_slot_stride));
    RecordProperty("descriptor_capacity", std::to_string(kFarVolumeRequestCapacity));
    RecordProperty("result_capacity", std::to_string(kFarVolumeResultCapacity));
    RecordProperty("vertex_stride", std::to_string(stats.vertex_stride));
    RecordProperty("index_stride", std::to_string(stats.index_stride));
    RecordProperty("brick_stride", std::to_string(stats.brick_stride));
}

TEST(FarVolumeBuildQueue, DescriptorsAndTwoHeldWorkersBackpressureWithoutOwnerSampling) {
    Rig rig;
    rig.observe(Phase::BeforeSampling, true);
    for (int tile = 0; tile < 64; ++tile)
        EXPECT_EQ(rig.queue->submit({1, tile, 0, std::nullopt}).status,
                  FarVolumeSubmitStatus::Queued);
    EXPECT_EQ(rig.queue->submit({1, 65, 0, std::nullopt}).status, FarVolumeSubmitStatus::Full);
    EXPECT_EQ(rig.gate.calls[0], 0u);
    EXPECT_TRUE(rig.queue->pump());
    ASSERT_TRUE(rig.gate.wait(1));
    EXPECT_EQ(rig.queue->stats().running, 1u);
    EXPECT_EQ(rig.queue->stats().dispatches, 1u);
    EXPECT_TRUE(rig.queue->pump());
    ASSERT_TRUE(rig.gate.wait(2));
    EXPECT_FALSE(rig.queue->pump());
    EXPECT_EQ(rig.queue->stats().running, 2u);
    EXPECT_EQ(rig.queue->stats().pending, 62u);
    EXPECT_EQ(rig.queue->stats().payload_reserved_bytes, 2 * kFarVolumeSlotReservation);
    rig.gate.open();
    rig.queue->drain();
    EXPECT_FALSE(rig.queue->pump()); // completed results still hold both slots
    auto first = rig.queue->take_completed();
    auto second = rig.queue->take_completed();
    if (!first.has_value()) {
        FAIL() << "Expected first to contain a value";
    }
    if (!second.has_value()) {
        FAIL() << "Expected second to contain a value";
    }
    ASSERT_EQ(first.value().result().status, FarVolumeBuildStatus::Ready);
    ASSERT_EQ(second.value().result().status, FarVolumeBuildStatus::Ready);
    EXPECT_FALSE(rig.queue->pump()); // transferred leases still hold both slots
    EXPECT_EQ(rig.queue->stats().leased, 2u);
    first.reset();
    EXPECT_EQ(rig.queue->stats().payload_reserved_bytes, kFarVolumeSlotReservation);
    EXPECT_TRUE(rig.queue->pump());
    EXPECT_EQ(rig.queue->stats().dispatches, 3u);
    rig.queue->drain();
    second.reset();
}

TEST(FarVolumeBuildQueue, RealCoarseCaveModesRetainTheirProductionSamplingIdentity) {
    Rig rig;
    auto params = FlatParams();
    params.caves_enabled = true;
    rig.world.set_params(params);
    ASSERT_TRUE(rig.queue->bind_fixture_world(rig.world));
    for (const auto mode : {FarCaveMode::BandLimited, FarCaveMode::BoxFiltered}) {
        auto result = Complete(rig, {4, -1, -1, std::nullopt, mode});
        if (!result.has_value()) {
            FAIL() << "Expected result to contain a value";
        }
        ASSERT_EQ(result.value().result().status, FarVolumeBuildStatus::Ready);
        EXPECT_EQ(result.value().result().identity.effective_request.caves, mode);
        EXPECT_TRUE(result.value().result().span_complete);
        RecordProperty("coarse_caves_mode" + std::to_string(static_cast<int>(mode)) + "_mesh_hash",
                       std::to_string(MeshHash(result.value().result().mesh)));
        const auto generation = result.value().result().identity.request_generation;
        result.reset();
        EXPECT_TRUE(rig.queue->forget(generation));
    }
}

TEST(FarVolumeBuildQueue, SpanExtensionInvalidatesOldArrivalAndKeepsOriginalAndUnion) {
    Rig rig;
    rig.observe(Phase::AfterMeshing, true);
    const FarVolumeRequest original{1, -1, 0, FarVolumeSpan{-300, 20}};
    const auto old = rig.queue->submit(original);
    ASSERT_TRUE(rig.queue->pump());
    ASSERT_TRUE(rig.gate.wait(1));
    const FarVolumeRequest extension{1, -1, 0, FarVolumeSpan{-400, 80}};
    const auto newer = rig.queue->submit(extension);
    ASSERT_EQ(newer.status, FarVolumeSubmitStatus::Queued);
    EXPECT_GT(newer.request_generation, old.request_generation);
    rig.gate.open();
    rig.queue->drain();
    auto obsolete = rig.queue->take_completed();
    if (!obsolete.has_value()) {
        FAIL() << "Expected obsolete to contain a value";
    }
    EXPECT_EQ(obsolete.value().result().status, FarVolumeBuildStatus::Stale);
    EXPECT_FALSE(obsolete.value().result().span_complete);
    EXPECT_TRUE(obsolete.value().result().mesh.vertices.empty());
    EXPECT_EQ(rig.queue->stats().pending, 1u);
    obsolete.reset();
    ASSERT_TRUE(rig.queue->pump());
    rig.queue->drain();
    auto current = rig.queue->take_completed();
    if (!current.has_value()) {
        FAIL() << "Expected current to contain a value";
    }
    ASSERT_EQ(current.value().result().status, FarVolumeBuildStatus::Ready);
    const auto identity = current.value().result().identity;
    if (!identity.submitted_request.extra_span.has_value()) {
        FAIL() << "Expected identity.submitted_request.extra_span to contain a value";
    }
    if (!identity.effective_request.extra_span.has_value()) {
        FAIL() << "Expected identity.effective_request.extra_span to contain a value";
    }
    EXPECT_EQ(identity.submitted_request.extra_span.value().min_y, -400);
    EXPECT_EQ(identity.effective_request.extra_span.value().min_y, -400);
    EXPECT_EQ(identity.effective_request.extra_span.value().max_y, 80);
    EXPECT_EQ(rig.queue->submit(original).status, FarVolumeSubmitStatus::Duplicate);
}

TEST(FarVolumeBuildQueue, ChangedModeAndForgedRequestIdentityCannotReuseALease) {
    Rig rig;
    auto old = Complete(rig, {1, 0, 0, FarVolumeSpan{-300, 50}});
    if (!old.has_value()) {
        FAIL() << "Expected old to contain a value";
    }
    const auto identity = old.value().result().identity;
    EXPECT_TRUE(rig.queue->is_current(identity));
    for (int field = 0; field < 8; ++field) {
        auto bad = identity;
        switch (field) {
            case 0:
                ++bad.request_generation;
                break;
            case 1:
                ++bad.binding_epoch;
                break;
            case 2:
                ++bad.params_hash;
                break;
            case 3:
                ++bad.seed;
                break;
            case 4:
                ++bad.authority_revision;
                break;
            case 5:
                ++bad.effective_request.tile_x;
                break;
            case 6:
                bad.effective_request.caves = FarCaveMode::BoxFiltered;
                break;
            case 7:
                bad.submitted_request.extra_span = FarVolumeSpan{-301, 50};
                break;
        }
        EXPECT_FALSE(rig.queue->is_current(bad)) << "field=" << field;
    }
    const auto changed =
        rig.queue->submit({1, 0, 0, FarVolumeSpan{-200, 30}, FarCaveMode::BoxFiltered});
    ASSERT_EQ(changed.status, FarVolumeSubmitStatus::Queued);
    EXPECT_FALSE(rig.queue->is_current(identity));
    old.reset();
    ASSERT_TRUE(rig.queue->pump());
    rig.queue->drain();
    auto newer = rig.queue->take_completed();
    if (!newer.has_value()) {
        FAIL() << "Expected newer to contain a value";
    }
    const auto result = newer.value().result().identity;
    if (!result.effective_request.extra_span.has_value()) {
        FAIL() << "Expected result.effective_request.extra_span to contain a value";
    }
    if (!result.submitted_request.extra_span.has_value()) {
        FAIL() << "Expected result.submitted_request.extra_span to contain a value";
    }
    EXPECT_EQ(result.effective_request.extra_span.value().min_y, -300);
    EXPECT_EQ(result.submitted_request.extra_span.value().min_y, -200);
    EXPECT_EQ(result.effective_request.extra_span.value().max_y, 50);
}

TEST(FarVolumeBuildQueue, SeedAndParamsChangesDuringQueuedWorkRefuseOldIdentity) {
    for (bool change_seed : {false, true}) {
        Rig rig;
        rig.observe(Phase::BeforeSampling, true);
        const auto submitted = rig.queue->submit({1, 0, 0, std::nullopt});
        ASSERT_EQ(submitted.status, FarVolumeSubmitStatus::Queued);
        ASSERT_TRUE(rig.queue->pump());
        ASSERT_TRUE(rig.gate.wait(1));
        if (change_seed)
            rig.world.set_seed(2026);
        else {
            auto params = FlatParams();
            params.height_offset += 20;
            rig.world.set_params(params);
        }
        rig.gate.open();
        rig.queue->drain();
        auto result = rig.queue->take_completed();
        if (!result.has_value()) {
            FAIL() << "Expected result to contain a value";
        }
        EXPECT_EQ(result.value().result().status, FarVolumeBuildStatus::Stale);
        EXPECT_EQ(rig.gate.calls[static_cast<std::size_t>(Phase::AfterSampling)], 0u);
        EXPECT_EQ(rig.queue->submit({1, 0, 0, std::nullopt}).status,
                  FarVolumeSubmitStatus::IdentityChanged);
        result.reset();
        ASSERT_TRUE(rig.queue->bind_fixture_world(rig.world));
        auto changed = Complete(rig, {1, 0, 0, std::nullopt});
        if (!changed.has_value()) {
            FAIL() << "Expected changed to contain a value";
        }
        EXPECT_EQ(changed.value().result().status, FarVolumeBuildStatus::Ready);
    }
}

TEST(FarVolumeBuildQueue, AuthorityChangeAfterBuildRefusesFurtherPristineWork) {
    Rig rig;
    ASSERT_EQ(rig.queue->submit({1, 0, 0, std::nullopt}).status, FarVolumeSubmitStatus::Queued);
    ASSERT_TRUE(rig.queue->pump());
    rig.queue->drain();
    rig.world.notify_far_lod_authority_durable({0, 0, 0});
    auto result = rig.queue->take_completed();
    if (!result.has_value()) {
        FAIL() << "Expected result to contain a value";
    }
    EXPECT_EQ(result.value().result().status, FarVolumeBuildStatus::Stale);
    EXPECT_FALSE(result.value().result().span_complete);
    EXPECT_EQ(rig.queue->stats().payload_reserved_bytes, 0u);
    EXPECT_EQ(rig.queue->submit({1, 0, 0, std::nullopt}).status,
              FarVolumeSubmitStatus::IdentityChanged);
    result.reset();
    EXPECT_FALSE(rig.queue->bind_fixture_world(rig.world));
    EXPECT_EQ(rig.queue->submit({1, 0, 0, std::nullopt}).status, FarVolumeSubmitStatus::Unbound);
}

TEST(FarVolumeBuildQueue, CancellationAndExplicitRetryKeepBoundedDescriptors) {
    Rig rig;
    rig.observe(Phase::BeforeSampling, true);
    const auto submitted = rig.queue->submit({1, 0, 0, std::nullopt});
    ASSERT_TRUE(rig.queue->pump());
    ASSERT_TRUE(rig.gate.wait(1));
    EXPECT_TRUE(rig.queue->cancel(submitted.request_generation));
    rig.gate.open();
    rig.queue->drain();
    auto result = rig.queue->take_completed();
    if (!result.has_value()) {
        FAIL() << "Expected result to contain a value";
    }
    EXPECT_EQ(result.value().result().status, FarVolumeBuildStatus::Cancelled);
    EXPECT_EQ(rig.gate.calls[static_cast<std::size_t>(Phase::AfterSampling)], 0u);
    EXPECT_EQ(rig.queue->submit({1, 0, 0, std::nullopt}).status, FarVolumeSubmitStatus::Duplicate);
    EXPECT_EQ(rig.queue->stats().payload_reserved_bytes, 0u);
    result.reset();
    EXPECT_TRUE(rig.queue->forget(submitted.request_generation));
    auto retried = Complete(rig, {1, 0, 0, std::nullopt});
    if (!retried.has_value()) {
        FAIL() << "Expected retried to contain a value";
    }
    EXPECT_EQ(retried.value().result().status, FarVolumeBuildStatus::Ready);
    EXPECT_GT(retried.value().result().identity.request_generation, submitted.request_generation);
}

TEST(FarVolumeBuildQueue, SameAddressReentryAndWorldDestructionInvalidateOldLeases) {
    std::optional<FarVolumeResultLease> surviving;
    std::uint64_t hash = 0;
    {
        Rig rig;
        surviving = Complete(rig, {1, -1, 0, std::nullopt});
        if (!surviving.has_value()) {
            FAIL() << "Expected surviving to contain a value";
        }
        const auto old = surviving.value().result().identity;
        hash = MeshHash(surviving.value().result().mesh);
        rig.queue->prepare_world_swap();
        EXPECT_FALSE(rig.queue->is_current(old));
        std::destroy_at(&rig.world);
        std::construct_at(&rig.world, nullptr, nullptr, FlatParams(), 1337);
        ASSERT_TRUE(rig.queue->bind_fixture_world(rig.world));
        auto current = Complete(rig, {1, -1, 0, std::nullopt});
        if (!current.has_value()) {
            FAIL() << "Expected current to contain a value";
        }
        EXPECT_GT(current.value().result().identity.binding_epoch, old.binding_epoch);
        EXPECT_FALSE(rig.queue->is_current(old));
        EXPECT_EQ(rig.queue->stats().leased, 2u);
    }
    if (!surviving.has_value()) {
        FAIL() << "Expected result to survive world destruction";
    }
    EXPECT_EQ(MeshHash(surviving.value().result().mesh), hash);
    auto moved = std::move(surviving.value());
    surviving.reset();
    EXPECT_EQ(MeshHash(moved.result().mesh), hash);
    moved.release();
    EXPECT_THROW(moved.result(), std::logic_error);
}

TEST(FarVolumeBuildQueue, ReleasedResultCannotRecycleAnUnfinishedCompletionHandle) {
    Rig rig;
    rig.observe(Phase::AfterPublish, true);
    for (int tile = 0; tile < 3; ++tile)
        ASSERT_EQ(rig.queue->submit({1, tile, 0, std::nullopt}).status,
                  FarVolumeSubmitStatus::Queued);
    ASSERT_TRUE(rig.queue->pump());
    ASSERT_TRUE(rig.queue->pump());
    ASSERT_TRUE(rig.gate.wait(2));
    auto first = rig.queue->take_completed();
    auto second = rig.queue->take_completed();
    if (!first.has_value()) {
        FAIL() << "Expected first published result before JobSystem completion";
    }
    if (!second.has_value()) {
        FAIL() << "Expected second published result before JobSystem completion";
    }
    EXPECT_EQ(first.value().result().status, FarVolumeBuildStatus::Ready);
    EXPECT_EQ(second.value().result().status, FarVolumeBuildStatus::Ready);
    first.reset();
    EXPECT_EQ(rig.queue->stats().leased, 1u);
    EXPECT_EQ(rig.queue->stats().occupied_slots, 2u);
    EXPECT_EQ(rig.queue->stats().payload_reserved_bytes, kFarVolumeSlotReservation);
    EXPECT_FALSE(rig.queue->pump());
    EXPECT_EQ(rig.queue->stats().dispatches, 2u);
    rig.gate.open();
    rig.queue->drain();
    EXPECT_TRUE(rig.queue->pump());
    rig.queue->drain();
    auto third = rig.queue->take_completed();
    if (!third.has_value()) {
        FAIL() << "Expected progress after the held completion returned";
    }
    EXPECT_EQ(third.value().result().identity.effective_request.tile_x, 2);
    EXPECT_EQ(third.value().result().status, FarVolumeBuildStatus::Ready);
}

TEST(FarVolumeBuildQueue, SwapDrainsAHeldRealWorkerBeforeWorldCanBeDestroyed) {
    WorkerGate gate;
    gate.phase = Phase::AfterPublish;
    gate.hold = true;
    std::promise<void> held, swapped;
    auto ready = held.get_future();
    auto swap_finished = swapped.get_future();
    auto owner = std::async(std::launch::async, [&] {
        Rig rig;
        Access::hook(*rig.queue, WorkerGate::invoke, &gate);
        ASSERT_EQ(rig.queue->submit({1, 0, 0, std::nullopt}).status, FarVolumeSubmitStatus::Queued);
        ASSERT_TRUE(rig.queue->pump());
        ASSERT_TRUE(gate.wait(1));
        held.set_value();
        rig.queue->prepare_world_swap();
        swapped.set_value(); // Observe this method, before Rig's later destructor drain.
        EXPECT_EQ(rig.queue->stats().running, 0u);
        EXPECT_EQ(rig.queue->stats().payload_reserved_bytes, 0u);
        EXPECT_FALSE(rig.queue->pump());
        EXPECT_FALSE(rig.queue->take_completed().has_value());
    });
    // Declared after the future: even a fatal main-thread assertion opens the
    // externally owned gate before the future destructor joins its owner.
    struct OpenGateOnExit {
        WorkerGate& gate;
        ~OpenGateOnExit() {
            gate.open();
        }
    } cleanup{gate};
    ASSERT_EQ(ready.wait_for(std::chrono::seconds(30)), std::future_status::ready);
    // The worker is held, not merely slow. This probes required synchronization,
    // not a performance threshold or a general wall-clock shutdown guarantee.
    EXPECT_EQ(swap_finished.wait_for(std::chrono::milliseconds(100)), std::future_status::timeout);
    gate.open();
    owner.get();
}

TEST(FarVolumeBuildQueue, DeepAndConvertedBudgetRefusalsNeverPublishPartialSuccess) {
    for (bool converted : {false, true}) {
        FarVolumeBuildLimits limits;
        if (converted)
            limits.converted_bytes = 1;
        Rig rig(limits);
        const FarVolumeRequest request =
            converted ? FarVolumeRequest{1, 0, 0, std::nullopt}
                      : FarVolumeRequest{1, 0, 0, FarVolumeSpan{-100'000, 10}};
        auto failed = Complete(rig, request);
        if (!failed.has_value()) {
            FAIL() << "Expected failed to contain a value";
        }
        EXPECT_EQ(failed.value().result().status, FarVolumeBuildStatus::BudgetRefused);
        EXPECT_FALSE(failed.value().result().span_complete);
        EXPECT_TRUE(failed.value().result().mesh.vertices.empty());
        EXPECT_EQ(rig.queue->stats().payload_reserved_bytes, 0u);
        EXPECT_EQ(rig.queue->submit(request).status, FarVolumeSubmitStatus::Duplicate);
        EXPECT_FALSE(rig.queue->pump());
        failed.reset();
        if (!converted) {
            auto independent = Complete(rig, {1, -1, 0, std::nullopt});
            if (!independent.has_value()) {
                FAIL() << "Expected independent to contain a value";
            }
            EXPECT_EQ(independent.value().result().status, FarVolumeBuildStatus::Ready);
        }
    }
}

TEST(FarVolumeBuildQueue, CapacityGrowthCannotEscapeTheRetainedBufferLimits) {
    for (const bool geometry : {false, true}) {
        FarVolumeBuildLimits limits;
        if (geometry)
            limits.geometry.max_buffer_bytes = 18u * 1024u * 1024u;
        else
            limits.generation.max_buffer_bytes = 32u * 1024u * 1024u;
        Rig rig(limits);
        rig.observe(geometry ? Phase::AfterMeshing : Phase::AfterSampling,
                    false,
                    geometry ? Fault::GeometryCapacity : Fault::BrickCapacity);
        auto result = Complete(rig, {1, 0, 0, std::nullopt});
        if (!result.has_value()) {
            FAIL() << "Expected capacity refusal receipt";
        }
        EXPECT_EQ(result.value().result().status, FarVolumeBuildStatus::BudgetRefused);
        EXPECT_FALSE(result.value().result().span_complete);
        EXPECT_TRUE(result.value().result().mesh.vertices.empty());
        EXPECT_EQ(rig.gate.calls[static_cast<std::size_t>(Phase::BeforePublish)], 0u);
        EXPECT_EQ(rig.queue->stats().payload_reserved_bytes, 0u);
    }
}

TEST(FarVolumeBuildQueue, WorkerFaultsCorruptionAndAllocationReleasePayloadReservations) {
    const std::array<std::pair<Fault, FarVolumeBuildStatus>, 7> faults{
        {{Fault::Allocation, FarVolumeBuildStatus::BudgetRefused},
         {Fault::Length, FarVolumeBuildStatus::BudgetRefused},
         {Fault::Invalid, FarVolumeBuildStatus::Invalid},
         {Fault::Other, FarVolumeBuildStatus::Failed},
         {Fault::Unknown, FarVolumeBuildStatus::Failed},
         {Fault::CorruptTile, FarVolumeBuildStatus::Invalid},
         {Fault::BadIndex, FarVolumeBuildStatus::Invalid}}};
    for (const auto& [fault, expected] : faults) {
        SCOPED_TRACE(static_cast<int>(fault));
        Rig rig;
        rig.observe(
            fault == Fault::CorruptTile ? Phase::AfterSampling : Phase::AfterMeshing, false, fault);
        auto result = Complete(rig, {1, 0, 0, std::nullopt});
        if (!result.has_value()) {
            FAIL() << "Expected result to contain a value";
        }
        EXPECT_EQ(result.value().result().status, expected);
        EXPECT_FALSE(result.value().result().span_complete);
        EXPECT_TRUE(result.value().result().mesh.indices.empty());
        EXPECT_EQ(result.value().result().error.back(), '\0');
        EXPECT_NE(result.value().result().error.front(), '\0');
        EXPECT_EQ(rig.queue->stats().payload_reserved_bytes, 0u);
        EXPECT_EQ(rig.queue->stats().retained_mesh_bytes, 0u);
    }
}

TEST(FarVolumeBuildQueue, SyntheticCompleteEmptySpanIsSuccessfulAndAccounted) {
    Rig rig;
    rig.observe(Phase::AfterSampling, false, Fault::EmptyTile);
    auto result = Complete(rig, {1, 0, 0, FarVolumeSpan{-400, 60}});
    if (!result.has_value()) {
        FAIL() << "Expected result to contain a value";
    }
    EXPECT_EQ(result.value().result().status, FarVolumeBuildStatus::ReadyEmpty);
    EXPECT_TRUE(result.value().result().span_complete);
    EXPECT_FALSE(result.value().result().mesh.bounds.has_value());
    EXPECT_LE(result.value().result().first_brick_y * 16, -400);
    EXPECT_GT(result.value().result().last_brick_y * 16, 60);
    EXPECT_EQ(rig.queue->stats().payload_reserved_bytes, kFarVolumeSlotReservation);
    result.reset();
    EXPECT_EQ(rig.queue->stats().payload_reserved_bytes, 0u);
}

TEST(FarVolumeBuildQueue, ValidCrcCannotHideWrongIdentityOrTruncatedRequestedSpan) {
    for (const auto fault : {Fault::WrongIdentity, Fault::TruncatedSpan}) {
        Rig rig;
        rig.observe(Phase::AfterSampling, false, fault);
        auto result = Complete(rig, {1, 0, 0, FarVolumeSpan{-400, 60}});
        if (!result.has_value()) {
            FAIL() << "Expected result to contain a value";
        }
        EXPECT_EQ(result.value().result().status, FarVolumeBuildStatus::Invalid);
        EXPECT_FALSE(result.value().result().span_complete);
        EXPECT_TRUE(result.value().result().mesh.indices.empty());
        EXPECT_EQ(rig.queue->stats().payload_reserved_bytes, 0u);
    }
}

TEST(FarVolumeBuildQueue, InvalidRequestsOverflowAndWrongOwnerCannotAllocateWork) {
    Rig rig;
    const std::array<FarVolumeRequest, 6> invalid{
        {{0, 0, 0, std::nullopt},
         {6, 0, 0, std::nullopt},
         {1, std::numeric_limits<std::int32_t>::max(), 0, std::nullopt},
         {1, 0, 0, FarVolumeSpan{1, -1}},
         {1, 0, 0, FarVolumeSpan{0, std::numeric_limits<float>::quiet_NaN()}},
         {1, 0, 0, std::nullopt, static_cast<FarCaveMode>(99)}}};
    for (const auto& request : invalid)
        EXPECT_EQ(rig.queue->submit(request).status, FarVolumeSubmitStatus::Invalid);
    EXPECT_EQ(rig.queue->stats().descriptors, 0u);
    EXPECT_EQ(rig.queue->stats().payload_reserved_bytes, 0u);
    Access::sequence(*rig.queue, std::numeric_limits<std::uint64_t>::max());
    EXPECT_EQ(rig.queue->submit({1, 0, 0, std::nullopt}).status,
              FarVolumeSubmitStatus::SequenceExhausted);
    auto wrong_owner = std::async(std::launch::async, [&] {
        EXPECT_THROW(rig.queue->submit({1, 0, 0, std::nullopt}), std::logic_error);
    });
    wrong_owner.get();
    FarVolumeBuildLimits too_large;
    ++too_large.converted_bytes;
    EXPECT_THROW(FarVolumeBuildQueue(rig.jobs, too_large), std::invalid_argument);
}

TEST(FarVolumeBuildQueue, RejectedJobSystemDispatchIsATerminalFailure) {
    Rig rig;
    rig.jobs.shutdown();
    auto result = Complete(rig, {1, 0, 0, std::nullopt});
    if (!result.has_value()) {
        FAIL() << "Expected result to contain a value";
    }
    EXPECT_EQ(result.value().result().status, FarVolumeBuildStatus::Failed);
    EXPECT_FALSE(result.value().result().span_complete);
    EXPECT_EQ(rig.queue->stats().running, 0u);
    EXPECT_EQ(rig.queue->stats().payload_reserved_bytes, 0u);
    EXPECT_EQ(rig.queue->submit({1, 0, 0, std::nullopt}).status, FarVolumeSubmitStatus::Duplicate);
}

} // namespace
