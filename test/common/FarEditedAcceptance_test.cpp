#include "../support/FarEditedCacheFixture.h"
#include "../support/SimulationContentFixture.h"
#include "luminumbra_common/ai/EcologyHash.h"
#include "luminumbra_common/core/FilesystemPath.h"
#include "luminumbra_common/core/JobSystem.h"
#include "luminumbra_common/persistence/WorldPersistenceRoundtrip.h"
#include "luminumbra_common/systems/AetherFieldSystem.h"
#include "luminumbra_common/systems/SHIELD_WorldSystem.h"
#include "luminumbra_common/systems/WeatherSystem.h"
#include "luminumbra_common/systems/WindFieldSystem.h"
#include "luminumbra_common/world/FarVolumeTile.h"
#include "luminumbra_common/world/GameSession.h"

#include <algorithm>
#include <array>
#include <fstream>
#include <gtest/gtest.h>

namespace {
using namespace Luminumbra;
using namespace Luminumbra::World;
using namespace Luminumbra::Persistence;
using namespace Luminumbra::TestSupport;
using Luminumbra::world::GameSession;
namespace fs = std::filesystem;

class FarEditedAcceptance : public testing::Test {
protected:
    void SetUp() override {
        root = CreateUniqueTempDirectory("far_acceptance_");
        policy = FarCachePolicy();
    }
    void TearDown() override {
        std::error_code error;
        fs::remove_all(root, error);
    }
    bool Save(WorldStreamingState& state) {
        FarEditedCacheSaveScope scope(root, policy);
        return WorldSaveService{}.save_dirty_chunks(state, root).saved;
    }
    FarCacheError Acquire(std::uint64_t generation, std::shared_ptr<const FarCommittedCache>& out) {
        return AcquireFarCommittedCache(
            {root, {policy->selection}}, generation, policy->limits, out);
    }
    fs::path root;
    std::shared_ptr<FarEditedCachePolicy> policy;
};

TEST_F(FarEditedAcceptance, SavedSamplingPhasesAllTiersNegativeBoundaryTallAndDeep) {
    WorldStreamingState state;
    // An independently specified affine field exposes wrong sample phases and
    // sign-only substitution. The negative boundary is shared by all spacings.
    auto tall = FarCacheEditedChunk(state, {-4, 32, -4});
    auto deep = FarCacheEditedChunk(state, {-4, -32, -4});
    for (std::size_t z = 0; z < 17; ++z)
        for (std::size_t y = 0; y < 17; ++y)
            for (std::size_t x = 0; x < 17; ++x) {
                const auto offset = x + 17 * (y + 17 * z);
                const float slope = float(x) / 8 + float(y) / 16 + float(z) / 32;
                tall->sdf_data[offset] = -3 - slope;
                deep->sdf_data[offset] = 3 + slope;
            }
    tall->material_data.assign(kCacheLatticeSamples, 7);
    deep->material_data.assign(kCacheLatticeSamples, 5);
    const auto original_capture = policy->limits.capture_source;
    policy->limits.capture_source = [original_capture](const auto& image) {
        auto source = original_capture(image);
        source.pristine.density =
            [](const FarVolumePosition& p, float, std::uint32_t, FarCaveMode) {
                return std::optional<FarVolumeSample>{{p.y < 0 ? -1.0f : 1.0f, 0}};
            };
        return source;
    };
    for (auto& selected : policy->selection) {
        selected.support.min.y = -768;
        selected.support.max.y = 768;
    }
    ASSERT_TRUE(Save(state));
    state.clear();
    std::shared_ptr<const FarCommittedCache> cache;
    ASSERT_EQ(Acquire(1, cache), FarCacheError::None);
    ASSERT_TRUE(cache);
    ASSERT_EQ(cache->source->Lattices().size(), 2u);
    for (std::uint8_t tier = 1; tier <= kFarTierCount; ++tier) {
        SCOPED_TRACE(tier);
        const auto spacing = kFarTierTable[tier - 1].sample_spacing_meters;
        for (std::int64_t phase = 0; phase <= 16; phase += spacing) {
            std::optional<FarVolumeSample> high, low;
            ASSERT_EQ(SampleFarOverlay(cache->tiles, tier, {-64 + phase, 512, -64}, 0, 128, high),
                      FarOverlaySampleError::None);
            ASSERT_EQ(SampleFarOverlay(cache->tiles, tier, {-64 + phase, -512, -64}, 0, 128, low),
                      FarOverlaySampleError::None);
            ASSERT_TRUE(high);
            ASSERT_TRUE(low);
            EXPECT_FLOAT_EQ(high->density, -3 - float(phase) / 8);
            EXPECT_FLOAT_EQ(low->density, 3 + float(phase) / 8);
            EXPECT_EQ(high->material, 7);
            EXPECT_EQ(low->material, 5);
        }
        const auto receipt =
            std::find_if(cache->catalog.tiles.begin(),
                         cache->catalog.tiles.end(),
                         [tier](const auto& tile) { return tile.key == FarTileKey{tier, -1, -1}; });
        ASSERT_NE(receipt, cache->catalog.tiles.end());
        EXPECT_LE(receipt->edited_span.min_y, -512);
        EXPECT_GE(receipt->edited_span.max_y, 528);
    }
}

TEST_F(FarEditedAcceptance, RepeatableBoundedLargeSaveWithDiskOnlyEditsAndCacheRemoval) {
    WorldStreamingState state;
    // A bounded analogue: many pristine headers and two edited records, then a
    // later save with only the neighbour resident. No thousand-chunk CI work.
    for (int x = 0; x < 32; ++x) {
        auto chunk = state.get_or_create_chunk({x, 0, 0});
        chunk->set_state(ChunkState::Idle);
        chunk->sdf_data.assign(kCacheLatticeSamples, 1);
        chunk->mark_voxel_data_dirty();
        chunk->mark_sdf_generated_current_params();
    }
    auto earlier = FarCacheEditedChunk(state, {-2, 0, -2});
    const auto before_cache = WorldSaveService{}.world_hash(state);
    ASSERT_TRUE(WorldSaveService{}.save_world(state, root));
    WorldStreamingState disk;
    std::vector<std::string> errors;
    ASSERT_TRUE(WorldSaveService{}.load_world(disk, root, errors));
    EXPECT_EQ(disk.size(), 33u);
    ASSERT_TRUE(Save(state));
    EXPECT_EQ(WorldSaveService{}.world_hash(state), before_cache);
    state.clear();
    earlier.reset();
    FarCacheEditedChunk(state, {-1, 0, -1});
    ASSERT_TRUE(Save(state));
    const auto hash = WorldSaveService{}.world_hash(state);
    const auto durable_hash =
        WorldSaveService::region_simulation_digest(root, {-1, -1}, state.snapshot_chunks());
    std::shared_ptr<const FarCommittedCache> first, second, reconstructed;
    ASSERT_EQ(Acquire(2, first), FarCacheError::None);
    ASSERT_TRUE(first);
    EXPECT_EQ(first->source->Inventory().size(), 2u);
    ASSERT_EQ(Acquire(2, second), FarCacheError::None);
    ASSERT_TRUE(second);
    ASSERT_EQ(first->tiles.size(), second->tiles.size());
    for (std::size_t i = 0; i < first->tiles.size(); ++i) {
        std::string a, b;
        ASSERT_EQ(EncodeFarOverlay(first->tiles[i], a), FarOverlayError::None);
        ASSERT_EQ(EncodeFarOverlay(second->tiles[i], b), FarOverlayError::None);
        EXPECT_EQ(a, b);
    }
    fs::remove_all(root / "far");
    EXPECT_EQ(WorldSaveService{}.world_hash(state), hash);
    EXPECT_EQ(WorldSaveService::region_simulation_digest(root, {-1, -1}, state.snapshot_chunks()),
              durable_hash);
    ASSERT_EQ(Acquire(2, reconstructed), FarCacheError::None);
    ASSERT_TRUE(reconstructed);
    EXPECT_TRUE(reconstructed->report.derived_due);
    EXPECT_EQ(reconstructed->source->Inventory().size(), 2u);
    ASSERT_EQ(first->tiles.size(), reconstructed->tiles.size());
    for (std::size_t i = 0; i < first->tiles.size(); ++i) {
        std::string a, b;
        ASSERT_EQ(EncodeFarOverlay(first->tiles[i], a), FarOverlayError::None);
        ASSERT_EQ(EncodeFarOverlay(reconstructed->tiles[i], b), FarOverlayError::None);
        EXPECT_EQ(a, b);
    }
    EXPECT_EQ(WorldSaveService{}.world_hash(state), hash);
    EXPECT_EQ(WorldSaveService::region_simulation_digest(root, {-1, -1}, state.snapshot_chunks()),
              durable_hash);
    EXPECT_FALSE(fs::exists(root / "far"));
}

// A real session with a fixed dry plane and no client assets. Temp naming is
// provided by the shared fixture; names never feed any observation or hash.
class AcceptanceSession {
public:
    explicit AcceptanceSession(std::size_t workers)
        : session(*owner) {
        root = CreateUniqueTempDirectory("far_session_acceptance_");
        fs::create_directories(root / "worlds/atlas/presets");
        std::ofstream(root / "worlds/atlas/presets/plane.json") << R"({
            "schema_rev":6,"generation_params":{
                "terrain":{"base_frequency":0.01,"base_amplitude":0.0,"octaves":1,
                    "persistence":0.5,"lacunarity":2.0,"height_offset":8.0,
                    "shaping":{"peaks_amplitude":0.0,"domain_warp_amplitude":0.0,
                        "continental_spline":[[-1.0,0.0],[1.0,0.0]]},"hydro":{"enabled":false}},
                "features":{"caves_enabled":false,"cave_frequency":0.02}}})";
        WriteSimulationContentFixture(root);
        jobs.startup(workers);
        session.SetJobSystem(&jobs);
        session.SetRootPath(Filesystem::GenericPathString(root) + "/");
        session.EnableFarWorldPersistenceSafety();
    }
    ~AcceptanceSession() {
        owner.reset();
        jobs.shutdown();
        std::error_code error;
        fs::remove_all(root, error);
    }
    bool Create() {
        return session.CreateWorld("Far acceptance", "12345", "plane");
    }
    std::shared_ptr<Chunk> Edit() {
        auto chunk = std::make_shared<Chunk>(IVec3{4, 0, 4});
        chunk->sdf_data.assign(kCacheLatticeSamples, -3);
        chunk->material_data.assign(kCacheLatticeSamples, 7);
        chunk->mark_sdf_loaded_or_edited();
        chunk->mark_voxel_data_dirty();
        chunk->set_state(ChunkState::Idle);
        return session.GetWorldSystem()->adopt_streamed_chunk(chunk) ? chunk : nullptr;
    }
    void Park() {
        auto* world = session.GetWorldSystem();
        world->debug_set_streaming_radius_cap(1);
        for (int i = 0; i < 12; ++i) {
            world->update(
                session.GetRegistry(), {Vec3(1024, 10, 1024)}, session.GetPhysicsSystem());
            world->wait_for_streaming_jobs();
        }
    }
    std::string Hash() {
        auto aether = session.GetAetherFieldSystem()->ComputeAetherSubHash();
        const auto energy = session.ComputeAetherStateSubHash();
        if (!energy.empty())
            aether = StableChecksum(aether + "|state:" + energy);
        return ComposeWorldHash(session.ComputeChunkStateHash(),
                                session.GetWindFieldSystem()->ComputeWindSubHash(),
                                session.GetWeatherSystem()->ComputeWeatherSubHash(),
                                aether,
                                session.ComputeScentSubHash(),
                                session.FoldClockIntoEcologyHash(
                                    luminumbra::ai::ComputeEcologySubHash(session.GetRegistry())),
                                session.ComputePlantSubHash());
    }
    fs::path root;
    JobSystem jobs;
    std::unique_ptr<GameSession> owner = std::make_unique<GameSession>();
    GameSession& session;
};

TEST(FarEditedSessionAcceptance, SaveLoadOpenParkReenterEditRetryAndSaveAs) {
    AcceptanceSession fixture(2);
    ASSERT_TRUE(fixture.Create());
    auto& session = fixture.session;
    auto chunk = fixture.Edit();
    ASSERT_TRUE(chunk);
    fixture.Park();
    ASSERT_FALSE(session.GetWorldSystem()->find_streamed_chunk(chunk->get_coords()));
    ASSERT_EQ(session.GetWorldSystem()->find_authoritative_chunk(chunk->get_coords()), chunk);
    ASSERT_TRUE(session.GetWorldSystem()->unpark_chunk(chunk->get_id()));
    ASSERT_EQ(session.GetWorldSystem()->find_streamed_chunk(chunk->get_coords()), chunk);
    chunk->sdf_data[1 + 17 * (1 + 17)] = -9;
    chunk->mark_voxel_data_dirty();
    const auto occupied = fixture.root / "occupied";
    std::ofstream(occupied) << "occupied";
    EXPECT_FALSE(session.SaveWorldStateTo(occupied));
    EXPECT_TRUE(chunk->is_voxel_data_dirty());
    ASSERT_TRUE(session.SaveWorldState());
    EXPECT_FALSE(chunk->is_voxel_data_dirty());
    const auto original = session.GetWorldSaveDir();
    const auto destination = fixture.root / "copy";
    ASSERT_TRUE(session.SaveWorldStateTo(destination));
    ASSERT_TRUE(session.LoadWorldStateFrom(destination));
    auto loaded = session.GetWorldSystem()->find_authoritative_chunk({4, 0, 4});
    ASSERT_TRUE(loaded);
    EXPECT_EQ(loaded->sdf_data[1 + 17 * (1 + 17)], -9);
    EXPECT_FALSE(session.LoadWorld("missing_world"));
    EXPECT_EQ(session.GetWorldSystem()->find_authoritative_chunk({4, 0, 4}), loaded);
    ASSERT_TRUE(session.LoadWorldStateFrom(original));
    loaded = session.GetWorldSystem()->find_authoritative_chunk({4, 0, 4});
    ASSERT_TRUE(loaded);
    EXPECT_EQ(loaded->sdf_data[1 + 17 * (1 + 17)], -9);
    EXPECT_FALSE(session.ActiveRegionsEnabled());
}

TEST(FarEditedSessionAcceptance, RealRecaptureExpandsSupportBeforeCommonSupportSweep) {
    AcceptanceSession fixture(1);
    ASSERT_TRUE(fixture.Create());
    auto& session = fixture.session;
    // Tier 1 makes the real elevated band cross a brick boundary. One key keeps
    // the production capture bounded; no rendering or GL allocation is needed.
    const std::array keys{FarTileKey{1, 0, 0}};
    Luminumbra::world::FarWorldPublicationRequest request;
    request.world_epoch = 1;
    request.wanted = keys;
    request.limits = GameSession::FarWorldPublicationLimits();
    request.source_memory = std::make_shared<FarVolumeMemoryPool>(2ull * 1024 * 1024 * 1024);
    for (auto& pool : request.memory)
        pool = std::make_shared<FarVolumeMemoryPool>(256ull * 1024 * 1024);
    request.camera_y = 16;
    std::shared_ptr<const Luminumbra::world::FarWorldPublication> low, high, repeated;
    std::string diagnostic;
    ASSERT_TRUE(session.CaptureFarWorldPublication(request, low, diagnostic)) << diagnostic;
    ASSERT_TRUE(low);
    ASSERT_EQ(low->tiles.size(), 1u);
    const auto& low_tile = low->tiles.front();
    ASSERT_TRUE(low_tile.observation);
    const auto low_region = low_tile.observation->Region(0);
    ASSERT_TRUE(low_region);
    request.camera_y = 96;
    ASSERT_TRUE(session.CaptureFarWorldPublication(request, high, diagnostic)) << diagnostic;
    ASSERT_TRUE(high);
    ASSERT_EQ(high->tiles.size(), 1u);
    const auto& high_tile = high->tiles.front();
    ASSERT_TRUE(high_tile.observation);
    const auto high_region = high_tile.observation->Region(0);
    ASSERT_TRUE(high_region);
    EXPECT_EQ(high->world_epoch, low->world_epoch);
    EXPECT_EQ(high->committed_generation, low->committed_generation);
    EXPECT_EQ(high->caves, low->caves);
    EXPECT_EQ(high->source_token, low->source_token);
    EXPECT_EQ(high_tile.observation->durable_identity().generation,
              low_tile.observation->durable_identity().generation);
    EXPECT_EQ(high_tile.observation->durable_identity().state,
              low_tile.observation->durable_identity().state);
    EXPECT_GT(high_tile.span.generation(), low_tile.span.generation());
    EXPECT_EQ(high_region->support.min, low_region->support.min);
    EXPECT_GT(high_region->support.max.y, low_region->support.max.y);
    EXPECT_EQ(high_region->support.max.x, low_region->support.max.x);
    EXPECT_EQ(high_region->support.max.z, low_region->support.max.z);
    EXPECT_NE(high_tile.observation->content_identity(), low_tile.observation->content_identity());
    // A recapture at either pose now has identical support and render currency.
    for (const float camera_y : {16.0f, 96.0f}) {
        request.camera_y = camera_y;
        ASSERT_TRUE(session.CaptureFarWorldPublication(request, repeated, diagnostic))
            << diagnostic;
        ASSERT_TRUE(repeated);
        ASSERT_EQ(repeated->tiles.size(), 1u);
        ASSERT_TRUE(repeated->tiles.front().observation);
        EXPECT_EQ(repeated->tiles.front().span.Intent(), high_tile.span.Intent());
        EXPECT_EQ(repeated->tiles.front().observation->content_identity(),
                  high_tile.observation->content_identity());
    }
    // A relevant material edit changes authority under exactly the same support.
    auto chunk = fixture.Edit();
    ASSERT_TRUE(chunk);
    ASSERT_TRUE(session.CaptureFarWorldPublication(request, repeated, diagnostic)) << diagnostic;
    ASSERT_TRUE(repeated);
    ASSERT_EQ(repeated->tiles.size(), 1u);
    ASSERT_TRUE(repeated->tiles.front().observation);
    const auto edited_region = repeated->tiles.front().observation->Region(0);
    ASSERT_TRUE(edited_region);
    EXPECT_EQ(edited_region->support.min, high_region->support.min);
    EXPECT_EQ(edited_region->support.max, high_region->support.max);
    EXPECT_NE(repeated->tiles.front().observation->content_identity(),
              high_tile.observation->content_identity());
}

void RunHashSequence(bool render,
                     std::size_t workers,
                     bool reverse,
                     std::vector<std::string>& hashes) {
    AcceptanceSession fixture(workers);
    ASSERT_TRUE(fixture.Create());
    ASSERT_TRUE(fixture.Edit());
    auto& session = fixture.session;
    const auto initial = fixture.Hash();
    const auto schedule = session.GetRegionSchedule();
    if (render) {
        const std::array keys{FarTileKey{5, 0, 0}};
        Luminumbra::world::FarWorldPublicationRequest request;
        request.world_epoch = 1;
        request.wanted = keys;
        request.limits = GameSession::FarWorldPublicationLimits();
        request.source_memory = std::make_shared<FarVolumeMemoryPool>(2ull * 1024 * 1024 * 1024);
        for (auto& pool : request.memory)
            pool = std::make_shared<FarVolumeMemoryPool>(256ull * 1024 * 1024);
        std::shared_ptr<const Luminumbra::world::FarWorldPublication> publication;
        std::string diagnostic;
        ASSERT_TRUE(session.CaptureFarWorldPublication(request, publication, diagnostic))
            << diagnostic;
        ASSERT_TRUE(publication);
        ASSERT_EQ(publication->tiles.size(), 1u);
        const auto image = publication->tiles[0].observation;
        ASSERT_TRUE(image);
        std::array<FarVolumeTile, 2> tiles;
        std::array<bool, 2> built{};
        std::vector<Job> work;
        for (std::size_t i = 0; i < tiles.size(); ++i) {
            const auto slot = reverse ? tiles.size() - i - 1 : i;
            work.push_back([&, image, slot] {
                const FarVolumeRequest tile_request{{5, 0, 0}, {-256, 256}, 1};
                built[slot] = BuildFarVolumeTile(
                    tile_request,
                    {4096, 2048},
                    [image](const FarVolumePosition& p) { return image->Sample(5, p); },
                    tiles[slot]);
            });
        }
        fixture.jobs.wait(
            fixture.jobs.dispatch_batch(work, reverse ? JobPriority::High : JobPriority::Normal));
        ASSERT_TRUE(built[0]);
        ASSERT_TRUE(built[1]);
        EXPECT_EQ(tiles[0], tiles[1]);
    }
    EXPECT_EQ(fixture.Hash(), initial);
    EXPECT_EQ(session.GetRegionSchedule(), schedule);
    EXPECT_FALSE(session.ActiveRegionsEnabled());
    for (int tick = 0; tick < 8; ++tick) {
        ASSERT_EQ(session.TickSimulation(session.GetSimulationClock().fixed_dt()), 1u);
        hashes.push_back(fixture.Hash());
    }
    EXPECT_EQ(session.GetSimulationTickCount(), 8u);
}

TEST(FarEditedSessionAcceptance, RenderingAndWorkerSchedulesPreserveSimulationHashSequence) {
    std::vector<std::string> off_one, on_one, off_three, on_three;
    RunHashSequence(false, 1, false, off_one);
    RunHashSequence(true, 1, false, on_one);
    RunHashSequence(false, 3, true, off_three);
    RunHashSequence(true, 3, true, on_three);
    ASSERT_EQ(off_one.size(), 8u);
    EXPECT_EQ(off_one, on_one);
    EXPECT_EQ(off_one, off_three);
    EXPECT_EQ(off_one, on_three);
}
} // namespace
