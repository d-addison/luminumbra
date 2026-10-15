#include "../support/SimulationContentFixture.h"
#include "../support/UniqueTempPath.h"
#include "core/JobSystem.h"
#include "nlohmann/json.hpp"
#include "persistence/EditedSourceImage.h"
#include "persistence/WorldPersistenceRoundtrip.h"
#include "persistence/WorldSaveService.h"
#include "systems/SHIELD_WorldSystem.h"
#include "world/Chunk.h"
#include "world/GameSession.h"
#include "world/WorldStreamingState.h"
#include "gtest/gtest.h"

#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace {
using namespace Luminumbra;
using namespace Luminumbra::Persistence;
using namespace Luminumbra::World;
namespace fs = std::filesystem;
constexpr std::size_t kSamples = (CHUNK_SIZE_X + 1) * (CHUNK_SIZE_Y + 1) * (CHUNK_SIZE_Z + 1);
constexpr std::size_t kInterior = 1 + 17 * (1 + 17);

struct SaveRoot {
    SaveRoot()
        : path(TestSupport::CreateUniqueTempDirectory("edited_source_")) {}
    ~SaveRoot() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
    fs::path path;
};
EditedSourceLimits Limits() {
    EditedSourceLimits limits;
    limits.max_tiles = 10;
    limits.max_members = 32;
    limits.max_records = 32;
    limits.max_directory_entries = 128;
    limits.max_file_bytes = 4 * 1024 * 1024;
    limits.max_decode_bytes = 4 * 1024 * 1024;
    limits.max_samples = 32 * kSamples;
    limits.max_boundary_probes = 1'000'000;
    limits.max_selection_probes = 10'000;
    limits.max_image_bytes = 4 * 1024 * 1024;
    limits.max_scratch_bytes = 256 * 1024 * 1024;
    limits.memory = std::make_shared<FarVolumeMemoryPool>(1024ull * 1024 * 1024);
    return limits;
}
EditedSourceTileSupport Tile(std::uint8_t tier, std::int64_t tx = 0, std::int64_t tz = 0) {
    const auto edge = static_cast<std::int64_t>(kFarTierTable[tier - 1].tile_edge_meters);
    return {{tier, tx, tz}, {{tx * edge, 0, tz * edge}, {(tx + 1) * edge, 64, (tz + 1) * edge}}};
}
std::shared_ptr<Chunk> EditedChunk(WorldStreamingState& state, IVec3 coords = {0, 0, 0}) {
    auto chunk = state.get_or_create_chunk(coords);
    chunk->set_state(ChunkState::Idle);
    chunk->sdf_data.assign(kSamples, 1.0f);
    chunk->material_data.assign(kSamples, 0);
    chunk->material_data[kInterior] = 7;
    chunk->sdf_data[kInterior] = -3.0f;
    chunk->mark_voxel_data_dirty();
    return chunk;
}
void Wet(Chunk& chunk) {
    chunk.has_water_sim.store(true);
    chunk.current_water_resolution.store(4);
    chunk.water_depth_mm.assign(16, 125);
    chunk.water_bed_mm.assign(16, 1000);
    chunk.water_edge_flux.assign(32, 0);
    chunk.water_level_data.assign(16, 1.125f);
    chunk.water_flow_data.assign(16, Vec2(0.0f));
    chunk.water_sim_terrain_height.assign(16, 1.0f);
    chunk.is_water_sleeping.store(true);
    chunk.ticks_below_threshold = 9;
    chunk.max_water_delta_last_tick = 0.125f;
    chunk.set_water_authority();
}
bool Save(const SaveRoot& root, const WorldStreamingState& state) {
    return WorldSaveService{}.save_world(state, root.path);
}
bool Index(const SaveRoot& root) {
    SaveTransaction transaction(root.path, 0, {}, &WorldSaveService::member_digest);
    return transaction.begin(nullptr) && transaction.commit(nullptr);
}
const FarEditedLattice* Find(const EditedSourceImage& image, std::uint64_t id) {
    const auto inventory = image.Inventory();
    const auto lattices = image.Lattices();
    for (std::size_t i = 0; i < inventory.size(); ++i) {
        if (inventory[i].chunk_id == id)
            return &lattices[i];
    }
    return nullptr;
}
std::string Read(const fs::path& path) {
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

TEST(EditedSourceImage, CopiedTerrainMaterialWaterSurviveEditEvictionAndClose) {
    SaveRoot root;
    WorldStreamingState state;
    auto chunk = EditedChunk(state);
    Wet(*chunk);
    ASSERT_TRUE(Save(root, state));
    ASSERT_TRUE(Index(root));
    auto limits = Limits();
    const std::array tiles{Tile(1)};
    std::shared_ptr<const EditedSourceImage> image;
    ASSERT_EQ(CaptureCommittedEditedSource(root.path, 1, {tiles}, limits, image),
              EditedSourceError::None);
    ASSERT_TRUE(image);
    ASSERT_EQ(image->Lattices().size(), 1u);
    ASSERT_EQ(image->Water().size(), 1u);
    chunk->sdf_data[kInterior] = 99;
    chunk->material_data[kInterior] = 99;
    chunk->water_depth_mm[0] = 999;
    state.clear();
    chunk.reset();
    fs::remove_all(root.path);
    EXPECT_EQ(image->Lattices()[0].density[kInterior], -3);
    EXPECT_EQ(image->Lattices()[0].material[kInterior], 7);
    EXPECT_EQ(image->Water()[0].depth_mm[0], 125);
    EXPECT_EQ(image->Water()[0].level[0], 1.125f);
    EXPECT_EQ(image->Water()[0].bed_mm[0], 1000);
    EXPECT_TRUE(image->Water()[0].sleeping);
    EXPECT_EQ(image->Water()[0].ticks_below_threshold, 9);
    EXPECT_TRUE(image->RendererEligible());
    EXPECT_FALSE(image->LegacyAuthority());
    EXPECT_GT(limits.memory->used(), 0u);
    auto alias = image;
    image.reset();
    EXPECT_GT(limits.memory->used(), 0u);
    alias.reset();
    EXPECT_EQ(limits.memory->used(), 0u);
}

TEST(EditedSourceImage, DiskOnlyInventoryIsCompleteAcrossAllTiersAndNegativeBoundaries) {
    SaveRoot root;
    WorldStreamingState state;
    auto a = EditedChunk(state, {-1, 0, 0});
    auto b = EditedChunk(state, {0, 0, 0});
    auto corner = EditedChunk(state, {-1, -1, -1});
    auto deep = EditedChunk(state, {0, -2, 0});
    auto diagonal = EditedChunk(state, {-2, -2, -2});
    const auto aid = a->get_id();
    const auto bid = b->get_id();
    const auto corner_id = corner->get_id();
    const auto deep_id = deep->get_id();
    const auto diagonal_id = diagonal->get_id();
    ASSERT_TRUE(Save(root, state));
    ASSERT_TRUE(Index(root));
    state.clear();
    a.reset();
    b.reset();
    corner.reset();
    deep.reset();
    diagonal.reset();
    auto limits = Limits();
    const std::array tiles{Tile(1), Tile(2), Tile(3), Tile(4), Tile(5)};
    std::shared_ptr<const EditedSourceImage> first, second;
    ASSERT_EQ(CaptureCommittedEditedSource(root.path, 1, {tiles}, limits, first),
              EditedSourceError::None);
    ASSERT_EQ(CaptureCommittedEditedSource(root.path, 1, {tiles}, limits, second),
              EditedSourceError::None);
    ASSERT_TRUE(first);
    ASSERT_TRUE(second);
    ASSERT_EQ(first->Inventory().size(), 4u);
    ASSERT_EQ(first->Selection().size(), 5u);
    const auto* negative = Find(*first, aid);
    const auto* positive = Find(*first, bid);
    ASSERT_NE(negative, nullptr);
    ASSERT_NE(positive, nullptr);
    EXPECT_EQ(negative->origin.x, -16);
    EXPECT_EQ(positive->origin.x, 0);
    EXPECT_EQ(negative->member_generation, 1u);
    EXPECT_EQ(negative->density[16], positive->density[0]);
    const auto* negative_corner = Find(*first, corner_id);
    const auto* normal_contributor = Find(*first, deep_id);
    ASSERT_NE(negative_corner, nullptr);
    ASSERT_NE(normal_contributor, nullptr);
    EXPECT_EQ(negative_corner->origin.y, -16);
    EXPECT_EQ(negative_corner->origin.z, -16);
    EXPECT_EQ(normal_contributor->origin.y, -32);
    EXPECT_EQ(Find(*first, diagonal_id), nullptr);
    for (std::size_t i = 0; i < first->Inventory().size(); ++i) {
        EXPECT_EQ(first->Inventory()[i].chunk_id, second->Inventory()[i].chunk_id);
        EXPECT_EQ(first->Inventory()[i].authoritative_digest,
                  second->Inventory()[i].authoritative_digest);
        EXPECT_EQ(first->Lattices()[i].density[kInterior],
                  second->Lattices()[i].density[kInterior]);
    }
}

TEST(EditedSourceImage, CommittedCaptureExcludesUnsavedLiveAndParkedChanges) {
    SaveRoot root;
    WorldStreamingState state;
    auto live = EditedChunk(state);
    auto parked = EditedChunk(state, {2, 0, 0});
    ASSERT_TRUE(Save(root, state));
    ASSERT_TRUE(Index(root));
    live->sdf_data[kInterior] = -11;
    parked->sdf_data[kInterior] = -22;
    live->mark_voxel_data_dirty();
    parked->mark_voxel_data_dirty();
    auto limits = Limits();
    const std::array tiles{Tile(1)};
    std::shared_ptr<const EditedSourceImage> committed;
    ASSERT_EQ(CaptureCommittedEditedSource(root.path, 1, {tiles}, limits, committed),
              EditedSourceError::None);
    ASSERT_TRUE(committed);
    ASSERT_EQ(committed->Lattices().size(), 2u);
    for (const auto& lattice : committed->Lattices())
        EXPECT_EQ(lattice.density[kInterior], -3);
    std::shared_ptr<const EditedSourcePreviewLayer> live_layer, parked_layer;
    ASSERT_EQ(CaptureEditedSourcePreview(
                  [&](std::size_t, const EditedSourcePreviewVisitor& visit) {
                      return visit(*live, false) && visit(*parked, true);
                  },
                  {tiles},
                  limits,
                  live_layer,
                  parked_layer),
              EditedSourceError::None);
    ASSERT_TRUE(live_layer);
    ASSERT_TRUE(parked_layer);
    ASSERT_EQ(live_layer->Lattices().size(), 1u);
    ASSERT_EQ(parked_layer->Lattices().size(), 1u);
    EXPECT_EQ(live_layer->Lattices()[0].density[kInterior], -11);
    EXPECT_EQ(parked_layer->Lattices()[0].density[kInterior], -22);
    live->sdf_data[kInterior] = 55;
    parked->sdf_data[kInterior] = 66;
    EXPECT_EQ(live_layer->Lattices()[0].density[kInterior], -11);
    EXPECT_EQ(parked_layer->Lattices()[0].density[kInterior], -22);
    EXPECT_EQ(committed->Identity().generation, 1u);
}

TEST(EditedSourceImage, PreparedIncludesStagedAndUnchangedDiskEditsWithCommitReceipt) {
    SaveRoot root;
    WorldStreamingState state;
    auto changed = EditedChunk(state);
    auto disk = EditedChunk(state, {40, 0, 0});
    const auto changed_id = changed->get_id();
    const auto disk_id = disk->get_id();
    ASSERT_TRUE(Save(root, state));
    ASSERT_TRUE(Index(root));
    ASSERT_TRUE(state.erase_chunk(disk_id));
    disk.reset();
    changed->sdf_data[kInterior] = -44;
    changed->mark_voxel_data_dirty();
    const std::array tiles{Tile(2)};
    auto limits = Limits();
    std::shared_ptr<const EditedSourceImage> prepared, receipt;
    {
        SaveTransaction transaction(root.path, 0, {}, &WorldSaveService::member_digest);
        ASSERT_TRUE(transaction.begin(nullptr));
        ASSERT_TRUE(WorldSaveService{}.save_dirty_chunks(state, root.path).saved);
        ASSERT_EQ(CapturePreparedEditedSource(transaction, {tiles}, limits, prepared),
                  EditedSourceError::None);
        ASSERT_TRUE(prepared);
        EXPECT_FALSE(prepared->RendererEligible());
        EXPECT_EQ(prepared->Identity().state, FarEditedImageState::Prepared);
        EXPECT_EQ(PublishCommittedEditedSource(prepared, receipt), EditedSourceError::Ineligible);
        EXPECT_FALSE(receipt);
        EXPECT_FALSE(prepared->PublicationReceipt());
        const auto* staged = Find(*prepared, changed_id);
        const auto* unchanged = Find(*prepared, disk_id);
        ASSERT_NE(staged, nullptr);
        ASSERT_NE(unchanged, nullptr);
        EXPECT_EQ(staged->density[kInterior], -44);
        EXPECT_EQ(staged->member_generation, 2u);
        EXPECT_EQ(unchanged->density[kInterior], -3);
        EXPECT_EQ(unchanged->member_generation, 1u);
        ASSERT_TRUE(transaction.commit(nullptr));
    }
    ASSERT_EQ(PublishCommittedEditedSource(prepared, receipt), EditedSourceError::None);
    ASSERT_TRUE(receipt);
    EXPECT_TRUE(receipt->RendererEligible());
    EXPECT_EQ(receipt->Identity().state, FarEditedImageState::Committed);
    EXPECT_EQ(receipt->Identity().generation, 2u);
    EXPECT_EQ(prepared->Identity().state, FarEditedImageState::Prepared);
    EXPECT_FALSE(prepared->RendererEligible());
    EXPECT_EQ(prepared->Lattices().data(), receipt->Lattices().data());
    const auto publication = receipt->PublicationReceipt();
    ASSERT_TRUE(publication);
    EXPECT_EQ(publication, prepared->PublicationReceipt());
    EXPECT_EQ(publication->generation, receipt->Identity().generation);
    const auto index_bytes = Read(snapshot_index_path(root.path));
    EXPECT_EQ(publication->index_fnv64, fnv1a64_bytes(index_bytes.data(), index_bytes.size()));
}

TEST(EditedSourceImage, AbortedPreparedPayloadRemainsReadableWithoutRendererAuthority) {
    SaveRoot root;
    WorldStreamingState state;
    auto chunk = EditedChunk(state);
    ASSERT_TRUE(Save(root, state));
    ASSERT_TRUE(Index(root));
    auto limits = Limits();
    const std::array tiles{Tile(1)};
    std::shared_ptr<const EditedSourceImage> prepared, output;
    {
        SaveTransaction transaction(root.path, 0, {}, &WorldSaveService::member_digest);
        ASSERT_TRUE(transaction.begin(nullptr));
        chunk->sdf_data[kInterior] = -55;
        chunk->mark_voxel_data_dirty();
        ASSERT_TRUE(WorldSaveService{}.save_dirty_chunks(state, root.path).saved);
        ASSERT_EQ(CapturePreparedEditedSource(transaction, {tiles}, limits, prepared),
                  EditedSourceError::None);
        transaction.abort(nullptr);
    }
    ASSERT_TRUE(prepared);
    EXPECT_EQ(prepared->Lattices()[0].density[kInterior], -55);
    EXPECT_FALSE(prepared->RendererEligible());
    EXPECT_EQ(PublishCommittedEditedSource(prepared, output), EditedSourceError::Ineligible);
    EXPECT_FALSE(prepared->PublicationReceipt());
    EXPECT_FALSE(output);
    ASSERT_EQ(CaptureCommittedEditedSource(root.path, 1, {tiles}, limits, output),
              EditedSourceError::None);
    ASSERT_TRUE(output);
    EXPECT_EQ(output->Lattices()[0].density[kInterior], -3);
}

TEST(EditedSourceImage, ChangedOrAddedDependenciesAfterCaptureCannotPublishOldPreparedImage) {
    for (const bool addition : {false, true}) {
        SaveRoot root;
        WorldStreamingState state;
        auto chunk = EditedChunk(state);
        ASSERT_TRUE(Save(root, state));
        ASSERT_TRUE(Index(root));
        auto limits = Limits();
        const std::array tiles{Tile(2)};
        std::shared_ptr<const EditedSourceImage> prepared, receipt;
        SaveTransaction transaction(root.path, 0, {}, &WorldSaveService::member_digest);
        ASSERT_TRUE(transaction.begin(nullptr));
        ASSERT_EQ(CapturePreparedEditedSource(transaction, {tiles}, limits, prepared),
                  EditedSourceError::None);
        if (addition)
            EditedChunk(state, {40, 0, 0});
        else {
            chunk->sdf_data[kInterior] = -66;
            chunk->mark_voxel_data_dirty();
        }
        ASSERT_TRUE(WorldSaveService{}.save_dirty_chunks(state, root.path).saved);
        ASSERT_TRUE(transaction.commit(nullptr));
        EXPECT_EQ(PublishCommittedEditedSource(prepared, receipt), EditedSourceError::Ineligible);
        EXPECT_FALSE(receipt);
    }
}

TEST(EditedSourceImage, GenerationMismatchAndFutureMemberGenerationPreserveOutput) {
    SaveRoot root;
    WorldStreamingState state;
    EditedChunk(state);
    ASSERT_TRUE(Save(root, state));
    ASSERT_TRUE(Index(root));
    auto limits = Limits();
    const std::array tiles{Tile(1)};
    std::shared_ptr<const EditedSourceImage> output;
    ASSERT_EQ(CaptureCommittedEditedSource(root.path, 1, {tiles}, limits, output),
              EditedSourceError::None);
    const auto previous = output;
    const auto used = limits.memory->used();
    EXPECT_EQ(CaptureCommittedEditedSource(root.path, 2, {tiles}, limits, output),
              EditedSourceError::GenerationMismatch);
    EXPECT_EQ(output, previous);
    SnapshotIndex index;
    ASSERT_TRUE(read_snapshot_index(root.path, index, nullptr));
    ASSERT_FALSE(index.members.empty());
    index.members.front().generation = 2;
    ASSERT_TRUE(WorldSaveService::durable_write_file(
        snapshot_index_path(root.path), encode_snapshot_index(index), nullptr));
    EXPECT_EQ(CaptureCommittedEditedSource(root.path, 1, {tiles}, limits, output),
              EditedSourceError::GenerationMismatch);
    EXPECT_EQ(output, previous);
    EXPECT_EQ(limits.memory->used(), used);
}

TEST(EditedSourceImage, InvalidLatticeNonfiniteDensityAndMaterialCountRefuse) {
    for (const int corruption : {0, 1, 2}) {
        SaveRoot root;
        WorldStreamingState state;
        auto chunk = EditedChunk(state);
        ASSERT_TRUE(Save(root, state));
        auto limits = Limits();
        const std::array tiles{Tile(1)};
        std::shared_ptr<const EditedSourceImage> output;
        ASSERT_EQ(CaptureCommittedEditedSource(root.path, 0, {tiles}, limits, output),
                  EditedSourceError::None);
        const auto previous = output;
        const auto used = limits.memory->used();
        auto payload = nlohmann::json::parse(SerializeWorldStreamingStateSnapshotJson(state));
        auto& record = payload["chunks"][0];
        if (corruption == 0)
            record["sdf_data"] = {1.0f};
        else if (corruption == 1)
            record["sdf_data"][kInterior] = 1e100;
        else
            record["material_data"] = {0};
        WorldSaveService::ContainerRecord bad;
        bad.id = chunk->get_id();
        bad.flags = 1;
        bad.payload = payload.dump();
        ASSERT_TRUE(WorldSaveService::upsert_container_records(
            WorldSaveService::region_file_path(root.path, 0, 0), {bad}));
        EXPECT_NE(CaptureCommittedEditedSource(root.path, 0, {tiles}, limits, output),
                  EditedSourceError::None);
        EXPECT_EQ(output, previous);
        EXPECT_EQ(limits.memory->used(), used);
    }
}

TEST(EditedSourceImage, SharedBoundaryDisagreementRefusesBeforeCoarseSampling) {
    for (const bool material : {false, true}) {
        SaveRoot root;
        WorldStreamingState state;
        EditedChunk(state, {-1, 0, 0});
        auto conflicting = EditedChunk(state, {0, 0, 0});
        if (material)
            conflicting->material_data[17 * (1 + 17)] = 2;
        else
            conflicting->sdf_data[17 * (1 + 17)] = 2;
        ASSERT_TRUE(Save(root, state));
        auto limits = Limits();
        const std::array tiles{Tile(5)};
        std::shared_ptr<const EditedSourceImage> output;
        EXPECT_EQ(CaptureCommittedEditedSource(root.path, 0, {tiles}, limits, output),
                  EditedSourceError::BoundaryConflict);
        EXPECT_FALSE(output);
        EXPECT_EQ(limits.memory->used(), 0u);
    }
}

TEST(EditedSourceImage, UnreadableExpectedMemberIsUnavailableRatherThanAbsent) {
    SaveRoot root;
    WorldStreamingState state;
    EditedChunk(state);
    ASSERT_TRUE(Save(root, state));
    ASSERT_TRUE(Index(root));
    auto limits = Limits();
    const std::array tiles{Tile(1)};
    std::shared_ptr<const EditedSourceImage> output;
    ASSERT_EQ(CaptureCommittedEditedSource(root.path, 1, {tiles}, limits, output),
              EditedSourceError::None);
    const auto previous = output;
    ASSERT_TRUE(fs::remove(WorldSaveService::region_file_path(root.path, 0, 0)));
    EXPECT_EQ(CaptureCommittedEditedSource(root.path, 1, {tiles}, limits, output),
              EditedSourceError::Unavailable);
    EXPECT_EQ(output, previous);
}

TEST(EditedSourceImage, EveryWorkAndByteCapPreservesOutputAndReleasesNewCharges) {
    SaveRoot root;
    WorldStreamingState state;
    EditedChunk(state);
    EditedChunk(state, {1, 0, 0});
    ASSERT_TRUE(Save(root, state));
    ASSERT_TRUE(Index(root));
    auto limits = Limits();
    const std::array tiles{Tile(1)};
    std::shared_ptr<const EditedSourceImage> output;
    ASSERT_EQ(CaptureCommittedEditedSource(root.path, 1, {tiles}, limits, output),
              EditedSourceError::None);
    const auto previous = output;
    const auto used = limits.memory->used();
    for (int cap = 0; cap < 11; ++cap) {
        auto refused = limits;
        switch (cap) {
            case 0:
                refused.max_tiles = 0;
                break;
            case 1:
                refused.max_members = 0;
                break;
            case 2:
                refused.max_records = 0;
                break;
            case 3:
                refused.max_directory_entries = 0;
                break;
            case 4:
                refused.max_file_bytes = 0;
                break;
            case 5:
                refused.max_decode_bytes = 0;
                break;
            case 6:
                refused.max_samples = 0;
                break;
            case 7:
                refused.max_boundary_probes = 0;
                break;
            case 8:
                refused.max_selection_probes = 0;
                break;
            case 9:
                refused.max_image_bytes = 0;
                break;
            case 10:
                refused.max_scratch_bytes = 0;
                break;
            default:
                FAIL();
                break;
        }
        EXPECT_NE(CaptureCommittedEditedSource(root.path, 1, {tiles}, refused, output),
                  EditedSourceError::None)
            << cap;
        EXPECT_EQ(output, previous) << cap;
        EXPECT_EQ(limits.memory->used(), used) << cap;
    }
    for (const auto fail_at : {0u, 1u}) {
        auto refused = limits;
        refused.memory = std::make_shared<FarVolumeMemoryPool>(limits.memory->limit());
        refused.memory->RefuseAttemptsFrom(fail_at);
        EXPECT_EQ(CaptureCommittedEditedSource(root.path, 1, {tiles}, refused, output),
                  EditedSourceError::ByteLimit);
        EXPECT_EQ(output, previous);
        EXPECT_EQ(refused.memory->used(), 0u);
        EXPECT_EQ(limits.memory->used(), used);
    }
}

TEST(EditedSourceImage, PreviewAdmissionFailuresPreserveBothLayersAndReleaseCharges) {
    WorldStreamingState state;
    const auto chunk = EditedChunk(state);
    auto limits = Limits();
    const std::array tiles{Tile(1)};
    const EditedSourcePreviewProducer producer = [&](std::size_t, const auto& visit) {
        return visit(*chunk, false);
    };
    std::shared_ptr<const EditedSourcePreviewLayer> live, parked;
    ASSERT_EQ(CaptureEditedSourcePreview(producer, {tiles}, limits, live, parked),
              EditedSourceError::None);
    const auto old_live = live;
    const auto old_parked = parked;
    for (const auto fail_at : {0u, 1u, 2u}) {
        auto refused = limits;
        refused.memory = std::make_shared<FarVolumeMemoryPool>(limits.memory->limit());
        refused.memory->RefuseAttemptsFrom(fail_at);
        EXPECT_EQ(CaptureEditedSourcePreview(producer, {tiles}, refused, live, parked),
                  EditedSourceError::ByteLimit);
        EXPECT_EQ(live, old_live);
        EXPECT_EQ(parked, old_parked);
        EXPECT_EQ(refused.memory->used(), 0u);
    }
}

TEST(EditedSourceImage, LegacyAbsenceAdoptsGenerationZeroWithoutFabricatingEditsOrWriting) {
    SaveRoot root;
    WorldStreamingState state;
    const auto chunk = EditedChunk(state);
    chunk->mark_sdf_generated_current_params();
    ASSERT_TRUE(Save(root, state));
    const auto region = WorldSaveService::region_file_path(root.path, 0, 0);
    const auto before = Read(region);
    const auto world_hash = WorldSaveService{}.world_hash(state);
    auto limits = Limits();
    const std::array tiles{Tile(1)};
    std::shared_ptr<const EditedSourceImage> image;
    ASSERT_EQ(CaptureCommittedEditedSource(root.path, 0, {tiles}, limits, image),
              EditedSourceError::None);
    ASSERT_TRUE(image);
    EXPECT_TRUE(image->LegacyAuthority());
    EXPECT_TRUE(image->RendererEligible());
    EXPECT_EQ(image->Identity().generation, 0u);
    EXPECT_TRUE(image->Lattices().empty());
    EXPECT_TRUE(image->Water().empty());
    EXPECT_FALSE(snapshot_index_exists(root.path));
    EXPECT_EQ(Read(region), before);
    EXPECT_EQ(WorldSaveService{}.world_hash(state), world_hash);
}

TEST(EditedSourceImage, LegacyEditedAuthorityUsesZeroUntilSuccessfulAdoptionCommit) {
    SaveRoot root;
    WorldStreamingState state;
    EditedChunk(state);
    ASSERT_TRUE(Save(root, state));
    auto limits = Limits();
    const std::array tiles{Tile(1)};
    std::shared_ptr<const EditedSourceImage> legacy, prepared, receipt;
    ASSERT_EQ(CaptureCommittedEditedSource(root.path, 0, {tiles}, limits, legacy),
              EditedSourceError::None);
    ASSERT_TRUE(legacy);
    ASSERT_EQ(legacy->Lattices().size(), 1u);
    EXPECT_EQ(legacy->Lattices()[0].member_generation, 0u);
    {
        SaveTransaction transaction(root.path, 0, {}, &WorldSaveService::member_digest);
        ASSERT_TRUE(transaction.begin(nullptr));
        ASSERT_EQ(CapturePreparedEditedSource(transaction, {tiles}, limits, prepared),
                  EditedSourceError::None);
        ASSERT_TRUE(transaction.commit(nullptr));
    }
    ASSERT_EQ(PublishCommittedEditedSource(prepared, receipt), EditedSourceError::None);
    ASSERT_TRUE(receipt);
    EXPECT_EQ(receipt->Lattices()[0].member_generation, 1u);
    EXPECT_EQ(legacy->Identity().generation, 0u);
    EXPECT_EQ(legacy->Lattices()[0].member_generation, 0u);
}

TEST(EditedSourceImage, RemovalProducesEmptyInventoryAndRetainsAnAbsentDependency) {
    SaveRoot root;
    WorldStreamingState state;
    EditedChunk(state);
    ASSERT_TRUE(Save(root, state));
    ASSERT_TRUE(Index(root));
    const std::array tiles{Tile(1)};
    auto limits = Limits();
    std::shared_ptr<const EditedSourceImage> before, prepared, receipt;
    ASSERT_EQ(CaptureCommittedEditedSource(root.path, 1, {tiles}, limits, before),
              EditedSourceError::None);
    {
        SaveTransaction transaction(root.path, 0, {}, &WorldSaveService::member_digest);
        ASSERT_TRUE(transaction.begin(nullptr));
        ASSERT_TRUE(WorldSaveService::remove_member(root.path, "chunks/region/r.0.0.lmr"));
        ASSERT_EQ(CapturePreparedEditedSource(transaction, {tiles}, limits, prepared),
                  EditedSourceError::None);
        ASSERT_TRUE(transaction.commit(nullptr));
    }
    ASSERT_EQ(PublishCommittedEditedSource(prepared, receipt), EditedSourceError::None);
    ASSERT_TRUE(before);
    ASSERT_TRUE(receipt);
    EXPECT_EQ(before->Inventory().size(), 1u);
    EXPECT_TRUE(receipt->Inventory().empty());
    const auto dependency =
        std::find_if(receipt->Dependencies().begin(),
                     receipt->Dependencies().end(),
                     [](const auto& member) { return member.path == "chunks/region/r.0.0.lmr"; });
    ASSERT_NE(dependency, receipt->Dependencies().end());
    EXPECT_FALSE(dependency->present);
    EXPECT_EQ(dependency->generation, 2u);
}

TEST(EditedSourceImage, ReceiptAdmissionRefusalPreservesOutputAndSharedPayloadCharge) {
    SaveRoot root;
    WorldStreamingState state;
    EditedChunk(state);
    ASSERT_TRUE(Save(root, state));
    ASSERT_TRUE(Index(root));
    auto limits = Limits();
    const std::array tiles{Tile(1)};
    std::shared_ptr<const EditedSourceImage> output, prepared;
    ASSERT_EQ(CaptureCommittedEditedSource(root.path, 1, {tiles}, limits, output),
              EditedSourceError::None);
    const auto previous = output;
    {
        SaveTransaction transaction(root.path, 0, {}, &WorldSaveService::member_digest);
        ASSERT_TRUE(transaction.begin(nullptr));
        ASSERT_EQ(CapturePreparedEditedSource(transaction, {tiles}, limits, prepared),
                  EditedSourceError::None);
        ASSERT_TRUE(transaction.commit(nullptr));
    }
    const auto used = limits.memory->used();
    limits.memory->RefuseAttemptsFrom(limits.memory->attempts());
    EXPECT_EQ(PublishCommittedEditedSource(prepared, output), EditedSourceError::ByteLimit);
    EXPECT_EQ(output, previous);
    EXPECT_EQ(limits.memory->used(), used);
}

TEST(EditedSourceImage, PresentMalformedOrFutureRecordsRefuseLegacyAdoption) {
    for (const std::string bad : {"{", "{\"schema\":\"luminumbra.snapshot.v2\"}"}) {
        SaveRoot root;
        auto limits = Limits();
        const std::array tiles{Tile(1)};
        std::shared_ptr<const EditedSourceImage> output;
        ASSERT_TRUE(
            WorldSaveService::durable_write_file(snapshot_index_path(root.path), bad, nullptr));
        EXPECT_EQ(CaptureCommittedEditedSource(root.path, 0, {tiles}, limits, output),
                  EditedSourceError::InvalidRecord);
        EXPECT_FALSE(output);
        EXPECT_EQ(limits.memory->used(), 0u);
    }
}

TEST(EditedSourceImage, DecodeLengthAdmissionRefusesBeforeAnyChunkVisitor) {
    SaveRoot root;
    WorldStreamingState state;
    EditedChunk(state);
    ASSERT_TRUE(Save(root, state));
    auto bytes = Read(WorldSaveService::region_file_path(root.path, 0, 0));
    ASSERT_GT(bytes.size(), 22u);
    for (std::size_t i = 18; i < 22; ++i)
        bytes[i] = static_cast<char>(0xff);
    ASSERT_TRUE(WorldSaveService::durable_write_file(
        WorldSaveService::region_file_path(root.path, 0, 0), bytes, nullptr));
    const auto limits = Limits();
    const WorldSaveService::EditedRegionReadLimits read_limits{limits.max_file_bytes,
                                                               limits.max_decode_bytes,
                                                               limits.max_scratch_bytes,
                                                               limits.max_records,
                                                               limits.max_samples};
    std::size_t visits = 0, records = 0;
    std::uint64_t decoded = 0;
    MemberDigest digest;
    EXPECT_FALSE(WorldSaveService::read_edited_source_region(
        WorldSaveService::region_file_path(root.path, 0, 0),
        read_limits,
        false,
        [&](const Chunk&, std::uint64_t) {
            ++visits;
            return true;
        },
        digest,
        decoded,
        records));
    EXPECT_EQ(visits, 0u);
}

TEST(EditedSourceImage, SessionCaptureIsOwnerThreadOnlyAndSimulationNeutralAfterClose) {
    SaveRoot runtime;
    fs::create_directories(runtime.path / "worlds/atlas/presets");
    fs::copy_file(fs::path(LUMINUMBRA_SOURCE_ROOT) / "worlds/atlas/presets/default.json",
                  runtime.path / "worlds/atlas/presets/default.json");
    TestSupport::WriteSimulationContentFixture(runtime.path);
    JobSystem jobs;
    jobs.startup();
    auto limits = Limits();
    const std::array tiles{Tile(1)};
    EditedSourceCapture captured;
    {
        world::GameSession session;
        session.SetJobSystem(&jobs);
        session.SetRootPath(runtime.path.string() +
                            static_cast<char>(fs::path::preferred_separator));
        ASSERT_TRUE(session.CreateWorld("Source capture", "12345", "default"));
        ASSERT_NE(session.GetWorldSystem(), nullptr);
        WorldStreamingState state;
        const auto chunk = EditedChunk(state);
        ASSERT_TRUE(WorldSaveService{}.save_world(state, session.GetWorldSaveDir()));
        ASSERT_TRUE(session.GetWorldSystem()->adopt_streamed_chunk(chunk));
        chunk->sdf_data[kInterior] = -99;
        chunk->mark_voxel_data_dirty();
        const auto region = WorldSaveService::region_file_path(session.GetWorldSaveDir(), 0, 0);
        const auto bytes = Read(region);
        const auto hash = session.ComputeChunkStateHash();
        ASSERT_EQ(session.CaptureFarEditedSources(0, {tiles}, limits, true, captured),
                  EditedSourceError::None);
        ASSERT_TRUE(captured.committed);
        ASSERT_TRUE(captured.live);
        ASSERT_EQ(captured.live->Lattices().size(), 1u);
        EXPECT_EQ(captured.live->Lattices()[0].density[kInterior], -99);
        EXPECT_EQ(captured.committed->Lattices()[0].density[kInterior], -3);
        EXPECT_EQ(Read(region), bytes);
        EXPECT_EQ(session.ComputeChunkStateHash(), hash);
        EXPECT_TRUE(chunk->is_voxel_data_dirty());
        const auto previous = captured.committed;
        EditedSourceError off_thread = EditedSourceError::None;
        std::thread worker([&] {
            off_thread = session.CaptureFarEditedSources(0, {tiles}, limits, true, captured);
        });
        worker.join();
        EXPECT_EQ(off_thread, EditedSourceError::Ineligible);
        EXPECT_EQ(captured.committed, previous);
    }
    jobs.shutdown();
    ASSERT_TRUE(captured.committed);
    ASSERT_TRUE(captured.live);
    EXPECT_EQ(captured.committed->Lattices()[0].density[kInterior], -3);
    EXPECT_EQ(captured.live->Lattices()[0].density[kInterior], -99);
}

TEST(EditedSourceImage, PreparedPublicationSlotAdmissionPreservesOutputAndExpiredSlotsAreReusable) {
    SaveRoot root;
    WorldStreamingState state;
    EditedChunk(state);
    ASSERT_TRUE(Save(root, state));
    ASSERT_TRUE(Index(root));
    auto limits = Limits();
    const std::array tiles{Tile(1)};
    SaveTransaction transaction(root.path, 0, {}, &WorldSaveService::member_digest);
    ASSERT_TRUE(transaction.begin(nullptr));
    std::vector<std::shared_ptr<const EditedSourceImage>> retained;
    retained.reserve(64);
    std::shared_ptr<const EditedSourceImage> output;
    for (std::size_t i = 0; i < 64; ++i) {
        ASSERT_EQ(CapturePreparedEditedSource(transaction, {tiles}, limits, output),
                  EditedSourceError::None);
        retained.push_back(output);
    }
    const auto previous = output;
    const auto used = limits.memory->used();
    EXPECT_EQ(CapturePreparedEditedSource(transaction, {tiles}, limits, output),
              EditedSourceError::WorkLimit);
    EXPECT_EQ(output, previous);
    EXPECT_EQ(limits.memory->used(), used);
    retained.erase(retained.begin());
    EXPECT_EQ(limits.memory->used(), used - limits.max_image_bytes);
    ASSERT_EQ(CapturePreparedEditedSource(transaction, {tiles}, limits, output),
              EditedSourceError::None);
    EXPECT_EQ(limits.memory->used(), used);
    transaction.abort(nullptr);
    ASSERT_TRUE(output);
    EXPECT_FALSE(output->RendererEligible());
}

TEST(EditedSourceImage, MetadataDependencyChangedAfterPreparedCaptureCannotPublish) {
    SaveRoot root;
    WorldStreamingState state;
    EditedChunk(state);
    ASSERT_TRUE(Save(root, state));
    ASSERT_TRUE(Index(root));
    auto limits = Limits();
    const std::array tiles{Tile(1)};
    std::shared_ptr<const EditedSourceImage> prepared, receipt;
    SaveTransaction transaction(root.path, 0, {}, &WorldSaveService::member_digest);
    ASSERT_TRUE(transaction.begin(nullptr));
    ASSERT_EQ(CapturePreparedEditedSource(transaction, {tiles}, limits, prepared),
              EditedSourceError::None);
    const auto manifest = Read(WorldSaveService::world_manifest_path(root.path));
    ASSERT_TRUE(WorldSaveService::write_member_bytes(
        root.path, "chunks/region/world-manifest.json", manifest + "\n"));
    ASSERT_TRUE(transaction.commit(nullptr));
    EXPECT_EQ(PublishCommittedEditedSource(prepared, receipt), EditedSourceError::Ineligible);
    EXPECT_FALSE(receipt);
}

TEST(EditedSourceImage, PreviewRetainsParkedAuthorityWhenLiveObservationIsAbsent) {
    WorldStreamingState live_state, parked_state;
    const auto pristine = EditedChunk(live_state);
    pristine->mark_sdf_generated_current_params();
    const auto edited = EditedChunk(parked_state);
    const std::array tiles{Tile(1)};
    const auto limits = Limits();
    std::shared_ptr<const EditedSourcePreviewLayer> live, parked;
    ASSERT_EQ(CaptureEditedSourcePreview(
                  [&](std::size_t, const auto& visitor) {
                      return visitor(*pristine, false) && visitor(*edited, true);
                  },
                  {tiles},
                  limits,
                  live,
                  parked),
              EditedSourceError::None);
    ASSERT_TRUE(live);
    ASSERT_TRUE(parked);
    EXPECT_TRUE(live->Lattices().empty());
    ASSERT_EQ(parked->Lattices().size(), 1u);
    EXPECT_EQ(parked->Lattices()[0].density[kInterior], -3);
}

TEST(EditedSourceImage, SourceSampleAdmissionRefusesBeforeAnyChunkVisitor) {
    SaveRoot root;
    WorldStreamingState state;
    EditedChunk(state);
    ASSERT_TRUE(Save(root, state));
    const auto limits = Limits();
    const WorldSaveService::EditedRegionReadLimits read_limits{limits.max_file_bytes,
                                                               limits.max_decode_bytes,
                                                               limits.max_scratch_bytes,
                                                               limits.max_records,
                                                               0};
    std::size_t visits = 0, records = 0;
    std::uint64_t decoded = 0;
    MemberDigest digest;
    EXPECT_FALSE(WorldSaveService::read_edited_source_region(
        WorldSaveService::region_file_path(root.path, 0, 0),
        read_limits,
        false,
        [&](const Chunk&, std::uint64_t) {
            ++visits;
            return true;
        },
        digest,
        decoded,
        records));
    EXPECT_EQ(visits, 0u);
}

TEST(EditedSourceImage, RecoveryAdmitsUndoAndWorkBeforeReadingOrReplacingPayloads) {
    SaveRoot root;
    WorldStreamingState state;
    EditedChunk(state);
    ASSERT_TRUE(Save(root, state));
    ASSERT_TRUE(Index(root));
    const std::array tiles{Tile(1)};
    auto limits = Limits();
    std::shared_ptr<const EditedSourceImage> output;
    ASSERT_EQ(CaptureCommittedEditedSource(root.path, 1, {tiles}, limits, output),
              EditedSourceError::None);
    const auto previous = output;
    const auto used = limits.memory->used();
    const auto destination = root.path / "world_info.json";
    const std::string installed = R"({"container_version":2})";
    const std::string undo =
        nlohmann::json{{"container_version", 2}, {"padding", std::string(64 * 1024, 'x')}}.dump();
    std::ostringstream digest;
    digest << std::hex << std::setw(16) << std::setfill('0')
           << fnv1a64_bytes(undo.data(), undo.size());
    const auto undo_path = journal_directory(root.path) / "undo/0";
    ASSERT_TRUE(WorldSaveService::durable_write_file(destination, installed, nullptr));
    ASSERT_TRUE(WorldSaveService::durable_write_file(undo_path, undo, nullptr));
    const auto journal = nlohmann::json{{"schema", kJournalSchema},
                                        {"txn_id", 42u},
                                        {"base_generation", 1u},
                                        {"new_generation", 2u},
                                        {"entries",
                                         {{{"path", "world_info.json"},
                                           {"action", "replace"},
                                           {"prev_bytes", undo.size()},
                                           {"prev_fnv64", digest.str()},
                                           {"undo", "undo/0"}}}}}
                             .dump();
    const auto txn_path = journal_directory(root.path) / "txn.json";
    ASSERT_TRUE(WorldSaveService::durable_write_file(txn_path, journal, nullptr));
    ASSERT_LT(journal.size(), 16 * 1024u);
    ASSERT_GT(undo.size(), 16 * 1024u);
    for (const int cap : {0, 1, 2, 3, 4}) {
        auto refused = limits;
        if (cap == 0)
            refused.max_file_bytes = 16 * 1024;
        else if (cap == 1)
            refused.max_decode_bytes = 16 * 1024;
        else if (cap == 2)
            refused.max_scratch_bytes = 8 * 1024 * 1024;
        else if (cap == 3)
            refused.max_records = 0;
        else
            refused.max_members = 0;
        EXPECT_EQ(CaptureCommittedEditedSource(root.path, 1, {tiles}, refused, output),
                  cap < 3 ? EditedSourceError::ByteLimit : EditedSourceError::WorkLimit);
        EXPECT_EQ(output, previous);
        EXPECT_EQ(limits.memory->used(), used);
        EXPECT_EQ(Read(destination), installed);
        EXPECT_EQ(Read(txn_path), journal);
        EXPECT_EQ(Read(undo_path), undo);
    }
    // The exact undo is valid: ordinary recovery can restore it without capture caps.
    EXPECT_EQ(recover_pending_journal(root.path, nullptr), RecoveryOutcome::RolledBack);
    EXPECT_EQ(Read(destination), undo);
}

TEST(EditedSourceImage, LegacyAndPreviewRejectInvalidWaterGridsAndContinuation) {
    for (const int corruption : {0, 1, 2, 3, 4, 5, 6, 7}) {
        SaveRoot root;
        WorldStreamingState state;
        auto chunk = EditedChunk(state);
        Wet(*chunk);
        ASSERT_TRUE(Save(root, state));
        const std::array tiles{Tile(1)};
        auto limits = Limits();
        std::shared_ptr<const EditedSourceImage> output;
        std::shared_ptr<const EditedSourcePreviewLayer> live, parked;
        const EditedSourcePreviewProducer producer = [&](std::size_t, const auto& visit) {
            return visit(*chunk, false);
        };
        ASSERT_EQ(CaptureCommittedEditedSource(root.path, 0, {tiles}, limits, output),
                  EditedSourceError::None);
        ASSERT_EQ(CaptureEditedSourcePreview(producer, {tiles}, limits, live, parked),
                  EditedSourceError::None);
        const auto previous = output;
        const auto old_live = live, old_parked = parked;
        const auto used = limits.memory->used();
        if (corruption == 0)
            chunk->current_water_resolution.store(0);
        else if (corruption == 1)
            chunk->water_depth_mm.pop_back();
        else if (corruption == 2)
            chunk->water_level_data.pop_back();
        else if (corruption == 3)
            chunk->water_edge_flux.pop_back();
        else if (corruption == 4)
            chunk->water_depth_mm[0] = -1;
        else if (corruption == 5)
            chunk->ticks_below_threshold = -1;
        else if (corruption == 6)
            chunk->water_mesh_dirty_ticks = 16;
        else
            chunk->max_water_delta_last_tick = -1;
        WorldSaveService::ContainerRecord bad;
        bad.id = chunk->get_id();
        bad.flags = 1;
        bad.payload = SerializeWorldStreamingStateSnapshotJson(state);
        ASSERT_TRUE(WorldSaveService::upsert_container_records(
            WorldSaveService::region_file_path(root.path, 0, 0), {bad}));
        EXPECT_EQ(CaptureCommittedEditedSource(root.path, 0, {tiles}, limits, output),
                  EditedSourceError::InvalidRecord);
        EXPECT_EQ(CaptureEditedSourcePreview(producer, {tiles}, limits, live, parked),
                  EditedSourceError::InvalidRecord);
        EXPECT_EQ(output, previous);
        EXPECT_EQ(live, old_live);
        EXPECT_EQ(parked, old_parked);
        EXPECT_EQ(limits.memory->used(), used);
    }
}

TEST(EditedSourceImage, SupportedLegacyWaterRepresentationsRemainReadable) {
    for (const bool float_only : {false, true}) {
        SaveRoot root;
        WorldStreamingState state;
        auto chunk = EditedChunk(state);
        Wet(*chunk);
        ASSERT_TRUE(Save(root, state));
        auto payload = nlohmann::json::parse(SerializeWorldStreamingStateSnapshotJson(state));
        auto& record = payload["chunks"][0];
        // The legacy decoder uses top-level continuation when water_state is absent,
        // accepts absent integer depth/bed, and restores absent flux as zero momentum.
        record.erase("water_state");
        record.erase("water_edge_flux");
        if (float_only) {
            record.erase("water_depth_mm");
            record.erase("water_bed_mm");
        }
        WorldSaveService::ContainerRecord legacy;
        legacy.id = chunk->get_id();
        legacy.flags = 1;
        legacy.payload = payload.dump();
        const auto path = WorldSaveService::region_file_path(root.path, 0, 0);
        ASSERT_TRUE(WorldSaveService::upsert_container_records(path, {legacy}));
        const auto before = Read(path);
        const std::array tiles{Tile(1)};
        auto limits = Limits();
        std::shared_ptr<const EditedSourceImage> image;
        ASSERT_EQ(CaptureCommittedEditedSource(root.path, 0, {tiles}, limits, image),
                  EditedSourceError::None);
        ASSERT_EQ(image->Water().size(), 1u);
        EXPECT_TRUE(image->Water()[0].has_sim);
        EXPECT_EQ(image->Water()[0].resolution, 4);
        EXPECT_EQ(image->Water()[0].level[0], 1.125f);
        EXPECT_EQ(image->Water()[0].depth_mm.size(), float_only ? 0u : 16u);
        EXPECT_EQ(image->Water()[0].edge_flux.size(), float_only ? 0u : 32u);
        std::vector<std::shared_ptr<Chunk>> decoded;
        ASSERT_TRUE(WorldSaveService::read_region_chunks(path, decoded));
        ASSERT_EQ(decoded.size(), 1u);
        const EditedSourcePreviewProducer producer = [&](std::size_t, const auto& visit) {
            return visit(*decoded.front(), false);
        };
        std::shared_ptr<const EditedSourcePreviewLayer> live, parked;
        ASSERT_EQ(CaptureEditedSourcePreview(producer, {tiles}, limits, live, parked),
                  EditedSourceError::None);
        ASSERT_EQ(live->Water().size(), 1u);
        EXPECT_EQ(live->Water()[0].depth_mm.size(), float_only ? 0u : 16u);
        EXPECT_EQ(live->Water()[0].level[0], 1.125f);
        EXPECT_EQ(Read(path), before);
        EXPECT_FALSE(snapshot_index_exists(root.path));
    }
}

TEST(EditedSourceImage, ValidUnindexedNewGroundRecordIsAdoptedWithoutChangingNormalReader) {
    SaveRoot root;
    WorldStreamingState state;
    EditedChunk(state);
    ASSERT_TRUE(Save(root, state));
    world::ActiveRegionLedger ledger;
    ledger.mark_edited({0, 0}, world::WorldClock{});
    world::GroundObject object;
    ASSERT_TRUE(ledger.allocate_persistent_id(object.id));
    object.type = 1;
    world::GroundObjectStore objects;
    ASSERT_TRUE(objects.insert(object));
    ASSERT_TRUE(WorldSaveService::durable_write_file(
        WorldSaveService::active_regions_path(root.path), ledger.encode(), nullptr));
    const auto bytes = objects.serialize();
    ASSERT_TRUE(WorldSaveService::durable_write_file(
        WorldSaveService::ground_objects_path(root.path), bytes, nullptr));
    world::GroundObjectStore normal;
    EXPECT_FALSE(
        WorldSaveService::load_ground_objects(normal, root.path, ledger.next_persistent_id()));
    const std::array tiles{Tile(1)};
    auto limits = Limits();
    std::shared_ptr<const EditedSourceImage> image;
    ASSERT_EQ(CaptureCommittedEditedSource(root.path, 0, {tiles}, limits, image),
              EditedSourceError::None);
    EXPECT_TRUE(image->LegacyAuthority());
    EXPECT_TRUE(image->RendererEligible());
    EXPECT_FALSE(snapshot_index_exists(root.path));
    EXPECT_EQ(Read(WorldSaveService::ground_objects_path(root.path)), bytes);
}

TEST(EditedSourceImage, DigestConsistentOrphanedFieldPageRefusesCommittedAndPreparedCapture) {
    SaveRoot root;
    WorldStreamingState state;
    EditedChunk(state);
    ASSERT_TRUE(Save(root, state));
    const std::array tiles{Tile(1)};
    auto limits = Limits();
    std::shared_ptr<const EditedSourceImage> output;
    ASSERT_EQ(CaptureCommittedEditedSource(root.path, 0, {tiles}, limits, output),
              EditedSourceError::None);
    const auto previous = output;
    const auto used = limits.memory->used();
    world::FieldPageState pages;
    ASSERT_TRUE(pages.activate_region({0, 0}, 7, 0));
    const auto bytes = pages.serialize();
    const auto path = WorldSaveService::field_pages_path(root.path);
    ASSERT_TRUE(WorldSaveService::durable_write_file(path, bytes, nullptr));
    ASSERT_TRUE(Index(root));
    MemberDigest digest;
    std::string error;
    ASSERT_TRUE(
        WorldSaveService::member_digest(root.path, "chunks/region/field-pages.fpg", digest, error));
    SnapshotIndex index;
    ASSERT_TRUE(read_snapshot_index(root.path, index, nullptr));
    const auto member = std::find_if(index.members.begin(), index.members.end(), [](const auto& m) {
        return m.path == "chunks/region/field-pages.fpg";
    });
    ASSERT_NE(member, index.members.end());
    EXPECT_EQ(member->bytes, digest.bytes);
    EXPECT_EQ(member->fnv64, digest.fnv64);
    WorldStreamingState loaded;
    std::vector<std::string> errors;
    EXPECT_FALSE(WorldSaveService{}.load_world(loaded, root.path, errors));
    ASSERT_FALSE(errors.empty());
    EXPECT_NE(errors.front().find("orphaned page"), std::string::npos);
    EXPECT_EQ(CaptureCommittedEditedSource(root.path, 1, {tiles}, limits, output),
              EditedSourceError::InvalidRecord);
    EXPECT_EQ(output, previous);
    EXPECT_EQ(limits.memory->used(), used);
    // A baseline validation cache cannot certify a newly staged candidate.
    SaveRoot prepared_root;
    ASSERT_TRUE(Save(prepared_root, state));
    ASSERT_TRUE(Index(prepared_root));
    SaveTransaction transaction(prepared_root.path, 0, {}, &WorldSaveService::member_digest);
    ASSERT_TRUE(transaction.begin(nullptr));
    ASSERT_TRUE(transaction.protect({"chunks/region/field-pages.fpg"}, nullptr));
    ASSERT_TRUE(WorldSaveService::durable_write_file(
        WorldSaveService::field_pages_path(prepared_root.path), bytes, nullptr));
    EXPECT_EQ(CapturePreparedEditedSource(transaction, {tiles}, limits, output),
              EditedSourceError::InvalidRecord);
    EXPECT_EQ(output, previous);
    EXPECT_EQ(limits.memory->used(), used);
}

TEST(EditedSourceImage, PreviewSelectionAndBoundaryCapsAreSharedAcrossLayers) {
    WorldStreamingState state;
    const auto first = EditedChunk(state, {0, 0, 0});
    const auto second = EditedChunk(state, {2, 0, 0});
    const std::array tiles{Tile(1)};
    auto limits = Limits();
    std::shared_ptr<const EditedSourcePreviewLayer> live, parked;
    const EditedSourcePreviewProducer one_each = [&](std::size_t, const auto& visit) {
        return visit(*first, false) && visit(*second, true);
    };
    ASSERT_EQ(CaptureEditedSourcePreview(one_each, {tiles}, limits, live, parked),
              EditedSourceError::None);
    const auto old_live = live, old_parked = parked;
    const auto used = limits.memory->used();
    limits.max_selection_probes = 7;
    EXPECT_EQ(CaptureEditedSourcePreview(one_each, {tiles}, limits, live, parked),
              EditedSourceError::WorkLimit);
    EXPECT_EQ(live, old_live);
    EXPECT_EQ(parked, old_parked);
    EXPECT_EQ(limits.memory->used(), used);
    limits = Limits();
    limits.max_boundary_probes = 1;
    const EditedSourcePreviewProducer two_each = [&](std::size_t, const auto& visit) {
        return visit(*first, false) && visit(*second, false) && visit(*first, true) &&
               visit(*second, true);
    };
    EXPECT_EQ(CaptureEditedSourcePreview(two_each, {tiles}, limits, live, parked),
              EditedSourceError::WorkLimit);
    EXPECT_EQ(live, old_live);
    EXPECT_EQ(parked, old_parked);
    EXPECT_EQ(limits.memory->used(), 0u);
}
TEST(EditedSourceImage, AuthoritativeChunksCommitAndPublishReceiptWithDerivedReconstructionDue) {
    SaveRoot root;
    WorldStreamingState state;
    auto chunk = EditedChunk(state);
    Wet(*chunk);
    ASSERT_TRUE(Save(root, state));
    ASSERT_TRUE(Index(root));
    chunk->sdf_data[kInterior] = -45;
    chunk->mark_voxel_data_dirty();
    const std::array tiles{Tile(1)};
    auto limits = Limits();
    std::shared_ptr<const EditedSourceImage> prepared, committed;
    {
        SaveTransaction transaction(root.path, 0, {}, &WorldSaveService::member_digest);
        ASSERT_TRUE(transaction.begin(nullptr));
        ASSERT_TRUE(WorldSaveService{}.save_dirty_chunks(state, root.path).saved);
        ASSERT_EQ(CapturePreparedEditedSource(transaction, {tiles}, limits, prepared),
                  EditedSourceError::None);
        const auto group = transaction.BeginDerivedGroup({"far/v1.0.0.lmr"});
        ASSERT_TRUE(group.has_value());
        if (!group.has_value())
            return;
        ASSERT_TRUE(transaction.WriteDerivedMember(*group, "far/v1.0.0.lmr", "malformed"));
        EXPECT_TRUE(chunk->is_voxel_data_dirty());
        EXPECT_EQ(PublishCommittedEditedSource(prepared, committed), EditedSourceError::Ineligible);
        ASSERT_TRUE(transaction.commit(nullptr));
        EXPECT_FALSE(chunk->is_voxel_data_dirty());
        EXPECT_FALSE(fs::exists(root.path / "far/v1.0.0.lmr"));
        ASSERT_EQ(PublishCommittedEditedSource(prepared, committed), EditedSourceError::None);
        ASSERT_TRUE(committed);
        EXPECT_EQ(committed->PublicationReceipt(), transaction.receipt());
    }
    ASSERT_TRUE(committed);
    ASSERT_EQ(committed->Lattices().size(), 1u);
    ASSERT_EQ(committed->Water().size(), 1u);
    EXPECT_EQ(committed->Lattices()[0].density[kInterior], -45);
    EXPECT_EQ(committed->Water()[0].depth_mm[0], 125);
    EXPECT_TRUE(committed->RendererEligible());
    std::shared_ptr<const EditedSourceImage> reopened;
    ASSERT_EQ(CaptureCommittedEditedSource(root.path, 2, {tiles}, limits, reopened),
              EditedSourceError::None);
    ASSERT_TRUE(reopened);
    ASSERT_EQ(reopened->Lattices().size(), 1u);
    EXPECT_EQ(reopened->Lattices()[0].density[kInterior], -45);
    EXPECT_EQ(reopened->Dependencies().size(), committed->Dependencies().size());
}
} // namespace
