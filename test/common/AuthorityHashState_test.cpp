#include "../support/SimulationContentFixture.h"
#include "../support/UniqueTempPath.h"

#include "luminumbra_common/ai/CreaturePersistence.h"
#include "luminumbra_common/components/CreatureComponents.h"
#include "luminumbra_common/core/FilesystemPath.h"
#include "luminumbra_common/core/JobSystem.h"
#include "luminumbra_common/fields/EnergyFieldState.h"
#include "luminumbra_common/persistence/PlantPersistence.h"
#include "luminumbra_common/persistence/WorldPersistenceRoundtrip.h"
#include "luminumbra_common/systems/SHIELD_WorldSystem.h"
#include "luminumbra_common/world/AuthorityHashState.h"
#include "luminumbra_common/world/GameSession.h"

#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include <iterator>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {
namespace W = Luminumbra::world;
namespace P = Luminumbra::Persistence;
namespace C = Luminumbra::Components;
namespace A = luminumbra::ai;
namespace fs = std::filesystem;
using Luminumbra::Chunk;
using Luminumbra::ChunkState;
using Luminumbra::IVec3;
using Luminumbra::WorldStreamingState;

std::shared_ptr<Chunk> Edited(IVec3 coords, float value) {
    auto chunk = std::make_shared<Chunk>(coords);
    chunk->set_state(ChunkState::Idle);
    chunk->heightmap_data = {value};
    chunk->mark_voxel_data_dirty();
    return chunk;
}
std::string Read(const fs::path& path) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream)
        throw std::runtime_error("Cannot read fixture bytes");
    return {std::istreambuf_iterator<char>(stream), {}};
}
std::map<std::string, std::string> Tree(const fs::path& root) {
    std::map<std::string, std::string> result;
    for (const auto& entry : fs::recursive_directory_iterator(root))
        if (entry.is_regular_file())
            result.emplace(entry.path().lexically_relative(root).generic_string(),
                           Read(entry.path()));
    return result;
}

class CaptureFixture {
public:
    fs::path root = Luminumbra::TestSupport::CreateUniqueTempDirectory("authority_capture_");
    entt::registry registry;
    W::ActiveRegionLedger ledger;
    W::GroundObjectStore ground;
    W::FieldPageState fields;
    luminumbra::fields::EnergyFieldState energy;
    ~CaptureFixture() {
        std::error_code error;
        fs::remove_all(root, error);
    }
    W::AuthorityHashInputs Inputs(std::vector<std::shared_ptr<Chunk>> live = {},
                                  std::vector<std::shared_ptr<Chunk>> parked = {},
                                  std::vector<IVec3> expected = {},
                                  std::string identity = "fixture_identity",
                                  std::string avatars = {}) const {
        return {.directory = root,
                .chunks = {.live = std::move(live),
                           .parked = std::move(parked),
                           .durable = std::move(expected),
                           .ground_object_bytes = {},
                           .field_page_bytes = {},
                           .creature_bytes = {}},
                .registry = registry,
                .ledger = ledger,
                .ground = ground,
                .fields = fields,
                .energy = &energy,
                .clock_bytes = W::WorldClock{}.canonical_bytes(),
                .identity_bytes = std::move(identity),
                .avatar_bytes = std::move(avatars)};
    }
    W::AuthorityHashCapture Capture(std::vector<std::shared_ptr<Chunk>> live = {},
                                    std::vector<std::shared_ptr<Chunk>> parked = {},
                                    std::vector<IVec3> expected = {},
                                    std::string identity = "fixture_identity",
                                    std::string avatars = {}) const {
        return W::CaptureAuthorityHashState(Inputs(std::move(live),
                                                   std::move(parked),
                                                   std::move(expected),
                                                   std::move(identity),
                                                   std::move(avatars)));
    }
    bool Save(const std::vector<std::shared_ptr<Chunk>>& chunks) const {
        std::vector<std::string> errors;
        P::SaveTransaction transaction(root, 0, {}, &P::WorldSaveService::member_digest);
        if (!transaction.begin(&errors))
            return false;
        WorldStreamingState state;
        for (const auto& chunk : chunks)
            state.insert_chunk(chunk);
        return P::WorldSaveService{}.save_world(state, root, &errors) &&
               transaction.commit(&errors);
    }
};

std::uint64_t Content(const std::vector<std::shared_ptr<Chunk>>& chunks) {
    WorldStreamingState state;
    for (const auto& chunk : chunks)
        state.insert_chunk(chunk);
    return std::stoull(P::ComputeRegionContentHash(state), nullptr, 16);
}

TEST(AuthorityHashState, AuthorityCaptureLiveParkedDurablePrecedence) {
    CaptureFixture fixture;
    const auto disk = Edited({0, 0, 0}, 1.0f);
    const auto retained = Edited({1, 0, 0}, 2.0f);
    const auto negative = Edited({-32, 0, 0}, 3.0f);
    ASSERT_TRUE(fixture.Save({disk, retained, negative}));
    const auto live = Edited({0, 0, 0}, 11.0f);
    const auto parked = Edited({0, 0, 0}, 22.0f);
    const auto capture = fixture.Capture({live}, {parked});
    ASSERT_TRUE(capture.state) << capture.error;
    EXPECT_EQ(capture.state->region_content().at({0, 0}), Content({live, retained}));
    EXPECT_EQ(capture.state->region_content().at({-1, 0}), Content({negative}));
    EXPECT_EQ(capture.state->region_content().begin()->first, (W::RegionKey{-1, 0}));
    const auto parked_capture = fixture.Capture({}, {parked});
    ASSERT_TRUE(parked_capture.state) << parked_capture.error;
    EXPECT_EQ(parked_capture.state->region_content().at({0, 0}), Content({parked, retained}));
    const auto disk_capture = fixture.Capture();
    ASSERT_TRUE(disk_capture.state) << disk_capture.error;
    EXPECT_EQ(disk_capture.state->region_content().at({0, 0}), Content({disk, retained}));
    EXPECT_NE(capture.state->chunk_hash(), parked_capture.state->chunk_hash());
    EXPECT_NE(parked_capture.state->chunk_hash(), disk_capture.state->chunk_hash());
    // Input order and lifecycle have no enabled authority meaning.
    live->set_state(ChunkState::Ready);
    const auto reordered = fixture.Capture({nullptr, live}, {parked});
    ASSERT_TRUE(reordered.state) << reordered.error;
    EXPECT_EQ(reordered.state->chunk_hash(), capture.state->chunk_hash());
    EXPECT_EQ(reordered.state->terrain_hash(), capture.state->terrain_hash());
    EXPECT_EQ(reordered.state->water_hash(), capture.state->water_hash());
}

TEST(AuthorityHashState, PristineAndLoadingCannotShadowAuthority) {
    CaptureFixture fixture;
    const auto disk = Edited({0, 0, 0}, 1.0f);
    ASSERT_TRUE(fixture.Save({disk}));
    const auto baseline = fixture.Capture();
    ASSERT_TRUE(baseline.state) << baseline.error;
    for (const bool loading : {false, true}) {
        auto candidate = Edited({0, 0, 0}, 99.0f);
        if (loading)
            candidate->set_state(ChunkState::Loading);
        else
            candidate->mark_sdf_generated_current_params();
        const auto both = fixture.Capture({candidate}, {candidate});
        ASSERT_TRUE(both.state) << both.error;
        EXPECT_EQ(both.state->chunk_hash(), baseline.state->chunk_hash());
        EXPECT_EQ(both.state->terrain_hash(), baseline.state->terrain_hash());
        const auto parked = Edited({0, 0, 0}, 55.0f);
        const auto fallthrough = fixture.Capture({candidate}, {parked});
        ASSERT_TRUE(fallthrough.state) << fallthrough.error;
        EXPECT_EQ(fallthrough.state->region_content().at({0, 0}), Content({parked}));
    }
    auto pristine = Edited({32, 0, 0}, 77.0f);
    pristine->mark_sdf_generated_current_params();
    const auto extra = fixture.Capture({pristine});
    ASSERT_TRUE(extra.state) << extra.error;
    EXPECT_EQ(extra.state->region_content().size(), 1u);
    EXPECT_EQ(extra.state->chunk_hash(), baseline.state->chunk_hash());
}

TEST(AuthorityHashState, DiskOnlyAuthorityIncludedAndMissingExpectedRecordRefuses) {
    CaptureFixture fixture;
    const auto disk = Edited({320, 0, -320}, 9.0f);
    ASSERT_TRUE(fixture.Save({disk}));
    // No residency hint is needed to discover committed edited content.
    const auto disk_only = fixture.Capture();
    ASSERT_TRUE(disk_only.state) << disk_only.error;
    EXPECT_EQ(disk_only.state->region_content().size(), 1u);
    EXPECT_EQ(disk_only.state->region_content().at({10, -10}), Content({disk}));
    const auto indexed = fixture.Capture({}, {}, {disk->get_coords()});
    ASSERT_TRUE(indexed.state) << indexed.error;
    EXPECT_EQ(indexed.state->chunk_hash(), disk_only.state->chunk_hash());
    // A ready live winner cannot excuse a missing expected committed record.
    const auto absent = Edited({321, 0, -320}, 15.0f);
    const auto missing = fixture.Capture({absent}, {}, {absent->get_coords()});
    EXPECT_FALSE(missing.state);
    EXPECT_FALSE(missing.error.empty());
    ASSERT_TRUE(fs::remove(P::WorldSaveService::region_file_path(fixture.root, 10, -10)));
    const auto missing_file = fixture.Capture({disk}, {}, {disk->get_coords()});
    EXPECT_FALSE(missing_file.state);
    EXPECT_FALSE(missing_file.error.empty());
}

TEST(AuthorityHashState, RegionalFreezeDigestMatchesFullCaptureAcrossChunkPrecedence) {
    CaptureFixture fixture;
    const auto disk = Edited({0, 0, 0}, 1.0f);
    const auto disk_only = Edited({1, 0, 0}, 2.0f);
    const auto unrelated = Edited({320, 0, -320}, 3.0f);
    ASSERT_TRUE(fixture.Save({disk, disk_only, unrelated}));
    const auto live = Edited({0, 0, 0}, 4.0f);
    const auto parked = Edited({0, 0, 0}, 5.0f);
    for (unsigned scenario = 0; scenario < 4; ++scenario) {
        SCOPED_TRACE(scenario);
        if (scenario == 1)
            live->mark_sdf_generated_current_params();
        if (scenario == 2)
            live->set_state(ChunkState::Loading);
        if (scenario == 3) {
            live->set_state(ChunkState::Idle);
            live->mark_water_state_dirty();
        }
        const auto inputs =
            fixture.Inputs({live},
                           {parked},
                           {disk->get_coords(), disk_only->get_coords(), unrelated->get_coords()});
        const auto full = W::CaptureAuthorityHashState(inputs);
        ASSERT_TRUE(full.state) << full.error;
        EXPECT_EQ(W::CaptureRegionAuthorityDigest(inputs, {0, 0}),
                  full.state->region_content().at({0, 0}));
    }
}

TEST(AuthorityHashState, AuthorityEnvelopeFramesIdentityAndAvatars) {
    CaptureFixture fixture;
    const auto first = fixture.Capture({}, {}, {}, "ab", "c");
    const auto second = fixture.Capture({}, {}, {}, "a", "bc");
    ASSERT_TRUE(first.state) << first.error;
    ASSERT_TRUE(second.state) << second.error;
    EXPECT_TRUE(first.state->region_content().empty());
    EXPECT_TRUE(second.state->region_content().empty());
    EXPECT_NE(first.state->chunk_hash(), second.state->chunk_hash());
    const auto baseline = first.state->FoldEcology("ecology");
    fixture.ledger = W::ActiveRegionLedger(
        W::RegionSchedulerConfig{.work_limit = 7, .hold_ticks = 12, .reduced_shift = 2});
    const auto configured = fixture.Capture({}, {}, {}, "ab", "c");
    ASSERT_TRUE(configured.state) << configured.error;
    EXPECT_TRUE(configured.state->region_content().empty());
    EXPECT_EQ(configured.state->chunk_hash(), first.state->chunk_hash());
    EXPECT_NE(configured.state->FoldEcology("ecology"), baseline);
    EXPECT_EQ(first.state->FoldEcology("ecology"), baseline);
}

TEST(AuthorityHashState, CompleteConsumerOwnersUseUniformFrozenProjection) {
    CaptureFixture fixture;
    const W::WorldClock clock;
    for (int x = -3; x <= 3; ++x)
        fixture.ledger.mark_edited({x, 0}, clock);
    std::uint64_t item_id = 0, plant_id = 0, creature_id = 0;
    ASSERT_TRUE(fixture.ledger.allocate_persistent_id(item_id));
    ASSERT_TRUE(fixture.ledger.allocate_persistent_id(plant_id));
    ASSERT_TRUE(fixture.ledger.allocate_persistent_id(creature_id));
    W::GroundObject object;
    object.id = item_id;
    object.type = 1;
    object.x_mm = -1'528'000;
    object.z_mm = 8'000;
    ASSERT_TRUE(fixture.ground.insert(object));
    const auto plant = fixture.registry.create();
    fixture.registry.emplace<C::TransformComponent>(plant).position = {-1016.0f, 0.0f, 8.0f};
    fixture.registry.emplace<C::PlantTag>(plant);
    fixture.registry.emplace<C::PlantGrowthComponent>(plant);
    fixture.registry.emplace<C::PlantGenomeComponent>(plant);
    auto& plant_owner = fixture.registry.emplace<C::PlantRegionComponent>(plant);
    plant_owner.persistent_id = plant_id;
    plant_owner.owner_x = -2;
    const auto creature = fixture.registry.create();
    fixture.registry.emplace<C::TransformComponent>(creature).position = {-504.0f, 0.0f, 8.0f};
    fixture.registry.emplace<C::CreatureComponent>(creature).species_id = 1;
    fixture.registry.emplace<C::CreatureCoarseComponent>(creature);
    auto& creature_owner = fixture.registry.emplace<C::CreatureRegionComponent>(creature);
    creature_owner.persistent_id = creature_id;
    creature_owner.owner_x = -1;
    ASSERT_TRUE(fixture.fields.activate_region({0, 0}, 1337, 0));
    ASSERT_TRUE(fixture.fields.set_soil({520, 8}, 1234));
    ASSERT_TRUE(fixture.energy.QueueRegionalDeposit(1, 43, 0, 0, 1000));
    luminumbra::fields::EnergyFieldState::RegionalResult energy_result;
    const std::array<W::RegionKey, 1> due{{{2, 0}}};
    ASSERT_TRUE(fixture.energy.TickRegional(1, fixture.ledger.records(), due, {}, energy_result));
    ASSERT_TRUE(fixture.ledger.set_energy_ages(energy_result.ages));
    auto pristine = Edited({128, 0, 0}, 4.0f);
    pristine->mark_sdf_generated_current_params();
    const auto capture = fixture.Capture({pristine});
    ASSERT_TRUE(capture.state) << capture.error;
    EXPECT_EQ(capture.state->region_content().size(), 6u);
    EXPECT_FALSE(capture.state->region_content().contains({3, 0}));
    EXPECT_FALSE(capture.state->region_content().contains({4, 0}));
    for (int x = -3; x <= 2; ++x)
        EXPECT_TRUE(capture.state->region_content().contains({x, 0}));
    for (const auto& [key, digest] : capture.state->region_content())
        EXPECT_EQ(W::CaptureRegionAuthorityDigest(fixture.Inputs({pristine}), key), digest);
    const auto fields = fixture.fields.region_projections();
    for (const auto& [key, bytes] : fields)
        EXPECT_EQ(bytes, fixture.fields.region_bytes(key));
    const auto energy = fixture.energy.RegionalProjections();
    ASSERT_TRUE(energy.contains({2, 0}));
    for (const auto& [key, bytes] : energy)
        EXPECT_EQ(bytes, fixture.energy.RegionBytes(key));
    const auto creatures = A::BuildCreatureSnapshot(fixture.registry);
    EXPECT_EQ(A::CreatureRegionProjections(creatures).at({-1, 0}),
              A::CreatureRegionProjection(fixture.registry, {-1, 0}));
    const std::string empty = P::ComputeRegionContentHash(WorldStreamingState{});
    const auto expected = [&empty](const std::string& suffix) {
        return std::stoull(P::StableChecksum(empty + suffix), nullptr, 16);
    };
    EXPECT_EQ(capture.state->region_content().at({-3, 0}),
              expected("|ground:" + fixture.ground.region_bytes({-3, 0})));
    EXPECT_EQ(capture.state->region_content().at({-2, 0}),
              expected("|ground:|plants:" +
                       Luminumbra::Ecs::SerializeEntityRegistrySnapshotJson(
                           luminumbra::foliage::BuildPlantEntitySnapshot(fixture.registry, true))));
    EXPECT_EQ(capture.state->region_content().at({-1, 0}),
              expected("|creatures:" + A::SerializeCreatureSnapshot(creatures)));
    for (const W::RegionKey key : {W::RegionKey{0, 0}, W::RegionKey{1, 0}})
        EXPECT_EQ(capture.state->region_content().at(key),
                  expected("|pages:" + fixture.fields.region_bytes(key)));
    std::string energy_bytes = "|pages:" + fixture.energy.RegionBytes({2, 0}) + "|energy_age:";
    const auto age = energy_result.ages.at({2, 0});
    for (unsigned i = 0; i < 8; ++i)
        energy_bytes.push_back(static_cast<char>((age >> (8 * i)) & 255));
    EXPECT_EQ(capture.state->region_content().at({2, 0}), expected(energy_bytes));
    auto& records =
        const_cast<std::map<W::RegionKey, W::ActiveRegionRecord>&>(fixture.ledger.records());
    for (const auto& [key, digest] : capture.state->region_content()) {
        auto& record = records.at(key);
        record.state = W::RegionState::Frozen;
        record.frozen_digest = digest;
    }
    const auto frozen = fixture.Capture({pristine});
    ASSERT_TRUE(frozen.state) << frozen.error;
    EXPECT_EQ(frozen.state->chunk_hash(), capture.state->chunk_hash());
    // The stamp is selected only for Frozen owners; the underlying complete content is retained.
    fixture.registry.get<C::CreatureComponent>(creature).hunger = 0.75f;
    const auto changed = fixture.Capture({pristine});
    ASSERT_TRUE(changed.state) << changed.error;
    EXPECT_NE(changed.state->region_content().at({-1, 0}),
              frozen.state->region_content().at({-1, 0}));
    EXPECT_EQ(changed.state->chunk_hash(), frozen.state->chunk_hash());
    records.at({-1, 0}).state = W::RegionState::Active;
    const auto thawed = fixture.Capture({pristine});
    ASSERT_TRUE(thawed.state) << thawed.error;
    EXPECT_NE(thawed.state->chunk_hash(), frozen.state->chunk_hash());
}

class CaptureWorld {
public:
    fs::path root = Luminumbra::TestSupport::CreateUniqueTempDirectory("authority_session_");
    Luminumbra::JobSystem jobs;
    W::GameSession session;
    CaptureWorld() {
        fs::create_directories(root / "worlds/atlas/presets");
        fs::copy_file(fs::path(LUMINUMBRA_SOURCE_ROOT) / "worlds/atlas/presets/default.json",
                      root / "worlds/atlas/presets/default.json");
        Luminumbra::TestSupport::WriteSimulationContentFixture(root);
        jobs.startup(1);
        session.SetRootPath(Luminumbra::Filesystem::GenericPathString(root) + "/");
        session.SetJobSystem(&jobs);
        session.SetActiveRegionsEnabled(true);
        if (!session.CreateWorld("Authority capture", "1337", "default"))
            throw std::runtime_error(session.GetWorldOpenError());
    }
    ~CaptureWorld() {
        session.GetRegistry().clear();
        std::error_code error;
        fs::remove_all(root, error);
    }
};

thread_local std::vector<fs::path> region_reads;
class ObserveRegionReads {
public:
    ObserveRegionReads() {
        region_reads.clear();
        P::WorldSaveService::set_region_read_hook_for_testing(
            [](const fs::path& path) { region_reads.push_back(path); });
    }
    ~ObserveRegionReads() {
        P::WorldSaveService::set_region_read_hook_for_testing(nullptr);
    }
};

TEST(AuthorityHashState, SchedulerFreezeReadsOnlyRequestedOwnerAndMatchesFullCapture) {
    CaptureWorld world;
    auto& session = world.session;
    const W::RegionKey target{10, 0}, unrelated{-10, 0};
    session.SetRegionSchedulerConfig({1, 1, 1});
    session.SetReplicatedSimulationAnchors({});
    session.PinActiveRegion(target, true);
    session.PinActiveRegion(unrelated, true);
    ASSERT_TRUE(session.GetWorldSystem()->adopt_streamed_chunk(Edited({320, 0, 0}, 8.0f)));
    ASSERT_TRUE(session.GetWorldSystem()->adopt_streamed_chunk(Edited({-320, 0, 0}, 9.0f)));
    ASSERT_TRUE(session.SaveWorld());
    ASSERT_EQ(session.TickSimulation(session.GetSimulationClock().fixed_dt()), 1u);
    ASSERT_FALSE(session.GetRegionSchedulerFault());
    auto& ledger = const_cast<W::ActiveRegionLedger&>(session.GetActiveRegionLedger());
    const std::array<W::RegionWork, 1> pressure{{{target, 5, 0, 0, 0}}};
    ledger.publish_completed_work(pressure);
    session.PinActiveRegion(unrelated, true);
    ASSERT_EQ(session.TickSimulation(session.GetSimulationClock().fixed_dt()), 1u);
    ASSERT_FALSE(session.GetRegionSchedulerFault());
    ASSERT_EQ(ledger.records().at(target).state, W::RegionState::Reduced);
    ledger.publish_completed_work(pressure);
    session.PinActiveRegion(unrelated, true);
    const auto target_file =
        P::WorldSaveService::region_file_path(session.GetWorldSaveDir(), 10, 0);
    const auto unrelated_file =
        P::WorldSaveService::region_file_path(session.GetWorldSaveDir(), -10, 0);
    const ObserveRegionReads observe;
    const auto full = session.CaptureAuthorityState();
    ASSERT_TRUE(full.state) << full.error;
    EXPECT_NE(std::find(region_reads.begin(), region_reads.end(), target_file), region_reads.end());
    EXPECT_NE(std::find(region_reads.begin(), region_reads.end(), unrelated_file),
              region_reads.end());
    region_reads.clear();
    ASSERT_EQ(session.TickSimulation(session.GetSimulationClock().fixed_dt()), 1u);
    ASSERT_FALSE(session.GetRegionSchedulerFault());
    ASSERT_EQ(ledger.records().at(target).state, W::RegionState::Frozen);
    EXPECT_EQ(ledger.records().at(target).frozen_digest, full.state->region_content().at(target));
    ASSERT_FALSE(region_reads.empty());
    for (const auto& path : region_reads)
        EXPECT_EQ(path, target_file);
    EXPECT_EQ(ledger.records().at(unrelated).state, W::RegionState::Active);
}

TEST(AuthorityHashState, HashCaptureDoesNotMutateAuthority) {
    CaptureWorld world;
    auto& session = world.session;
    auto& registry = session.GetRegistry();
    const auto plant = registry.create();
    registry.emplace<C::TransformComponent>(plant).position = session.GetMetadata().spawnPoint;
    registry.emplace<C::PlantTag>(plant);
    registry.emplace<C::PlantGrowthComponent>(plant);
    registry.emplace<C::PlantGenomeComponent>(plant);
    ASSERT_TRUE(session.AdmitPlant(plant));
    const auto creature = registry.create();
    registry.emplace<C::TransformComponent>(creature).position = session.GetMetadata().spawnPoint;
    registry.emplace<C::CreatureComponent>(creature).species_id =
        C::CreatureSpeciesId16("fixture_creature");
    ASSERT_TRUE(session.AdmitCreature(creature));
    registry.get_or_emplace<C::CreatureLocomotionStateComponent>(creature).velocity_y = -1.25f;
    const auto creature_bytes = A::SerializeCreatureSnapshot(A::BuildCreatureSnapshot(registry));
    const auto plant_bytes = Luminumbra::Ecs::SerializeEntityRegistrySnapshotJson(
        luminumbra::foliage::BuildPlantEntitySnapshot(registry, true));
    auto chunk = Edited({0, 0, 0}, 8.0f);
    chunk->mark_sdf_generated_current_params();
    chunk->mark_water_state_dirty();
    ASSERT_TRUE(session.GetWorldSystem()->adopt_streamed_chunk(chunk));
    const auto ledger = session.GetActiveRegionLedger().encode();
    const auto pages = session.GetFieldPages().canonical_bytes();
    const auto ground = session.GetGroundObjects().canonical_bytes();
    const auto files = Tree(session.GetWorldSaveDir());
    const auto revision = chunk->water_revision();
    const auto voxel_revision = chunk->voxel_revision();
    const auto dirty = chunk->is_voxel_data_dirty();
    const auto far_revision = session.GetWorldSystem()->far_lod_authority_revision();
    const auto first = session.CaptureAuthorityState();
    ASSERT_TRUE(first.state) << first.error;
    const auto frozen_hash = first.state->chunk_hash();
    EXPECT_FALSE(frozen_hash.empty());
    EXPECT_EQ(session.ComputeChunkStateHash(), frozen_hash);
    EXPECT_FALSE(session.FoldClockIntoEcologyHash({}).empty());
    EXPECT_EQ(session.ComputePlantSubHash(), P::StableChecksum(plant_bytes));
    EXPECT_EQ(session.ComputeScentSubHash(), "");
    EXPECT_EQ(session.GetActiveRegionLedger().encode(), ledger);
    EXPECT_EQ(session.GetFieldPages().canonical_bytes(), pages);
    EXPECT_EQ(session.GetGroundObjects().canonical_bytes(), ground);
    EXPECT_EQ(A::SerializeCreatureSnapshot(A::BuildCreatureSnapshot(registry)), creature_bytes);
    EXPECT_EQ(Luminumbra::Ecs::SerializeEntityRegistrySnapshotJson(
                  luminumbra::foliage::BuildPlantEntitySnapshot(registry, true)),
              plant_bytes);
    EXPECT_EQ(chunk->sdf_provenance(), Luminumbra::ChunkSdfProvenance::GeneratedCurrentParams);
    EXPECT_TRUE(chunk->is_water_state_dirty());
    EXPECT_FALSE(chunk->has_water_authority());
    EXPECT_EQ(chunk->is_voxel_data_dirty(), dirty);
    EXPECT_EQ(chunk->water_revision(), revision);
    EXPECT_EQ(chunk->voxel_revision(), voxel_revision);
    EXPECT_EQ(session.GetWorldSystem()->far_lod_authority_revision(), far_revision);
    EXPECT_EQ(Tree(session.GetWorldSaveDir()), files);
    chunk->heightmap_data[0] += 1.0f;
    EXPECT_NE(session.ComputeChunkStateHash(), frozen_hash);
    EXPECT_EQ(first.state->chunk_hash(), frozen_hash);
    EXPECT_FALSE(session.GetRegionSchedulerFault());
}

TEST(AuthorityHashState, CaptureFailureProducesNoDigest) {
    CaptureWorld world;
    auto& session = world.session;
    const auto good = Edited({-32, 0, 0}, 8.0f);
    const auto damaged = Edited({320, 0, 0}, 16.0f);
    ASSERT_TRUE(session.GetWorldSystem()->adopt_streamed_chunk(good));
    ASSERT_TRUE(session.GetWorldSystem()->adopt_streamed_chunk(damaged));
    ASSERT_TRUE(session.SaveWorldState());
    const auto before = session.CaptureAuthorityState();
    ASSERT_TRUE(before.state) << before.error;
    const auto path = P::WorldSaveService::region_file_path(session.GetWorldSaveDir(), 10, 0);
    std::ofstream(path, std::ios::binary | std::ios::trunc) << "corrupt";
    const auto failed = session.CaptureAuthorityState();
    EXPECT_FALSE(failed.state);
    EXPECT_FALSE(failed.error.empty());
    EXPECT_FALSE(session.GetRegionSchedulerFault()); // checked capture itself is pure
    EXPECT_EQ(session.ComputeChunkStateHash(), "");
    ASSERT_TRUE(session.GetRegionSchedulerFault());
    EXPECT_EQ(session.GetRegionSchedulerFault()->tick, session.GetSimulationTickCount());
    EXPECT_THROW((void)session.FoldClockIntoEcologyHash("ecology"), std::runtime_error);
    EXPECT_THROW((void)session.ComputePlantSubHash(), std::runtime_error);
    EXPECT_THROW((void)session.ComputeScentSubHash(), std::runtime_error);
    EXPECT_THROW((void)session.ComputeAetherStateSubHash(), std::runtime_error);
    EXPECT_EQ(Read(path), "corrupt");
}
} // namespace
