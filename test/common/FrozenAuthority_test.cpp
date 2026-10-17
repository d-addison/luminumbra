#include "../support/SimulationContentFixture.h"
#include "../support/UniqueTempPath.h"

#include "luminumbra_common/ai/CreaturePersistence.h"
#include "luminumbra_common/components/CreatureComponents.h"
#include "luminumbra_common/core/FilesystemPath.h"
#include "luminumbra_common/core/JobSystem.h"
#include "luminumbra_common/fields/EnergyFieldState.h"
#include "luminumbra_common/persistence/PlantPersistence.h"
#include "luminumbra_common/persistence/SaveTransaction.h"
#include "luminumbra_common/persistence/WorldSaveService.h"
#include "luminumbra_common/systems/PhysicsSystem.h"
#include "luminumbra_common/systems/SHIELD_WorldSystem.h"
#include "luminumbra_common/systems/WaterSystem.h"
#include "luminumbra_common/world/AuthorityHashState.h"
#include "luminumbra_common/world/GameSession.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include <iterator>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {
namespace W = Luminumbra::world;
namespace P = Luminumbra::Persistence;
namespace C = Luminumbra::Components;
namespace A = luminumbra::ai;
namespace fs = std::filesystem;
constexpr W::RegionKey kOwner{10, 0};
const Luminumbra::Vec3 kPosition{5200.0f, 100.0f, 8.0f};

std::string Read(const fs::path& path) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream)
        throw std::runtime_error("Cannot read fixture bytes");
    return {std::istreambuf_iterator<char>(stream), {}};
}
void Write(const fs::path& path, std::string_view bytes) {
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    stream.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    stream.close();
    if (!stream.good())
        throw std::runtime_error("Cannot write fixture bytes");
}
std::map<std::string, std::string> Tree(const fs::path& root) {
    std::map<std::string, std::string> result;
    for (const auto& entry : fs::recursive_directory_iterator(root))
        if (entry.is_regular_file())
            result.emplace(
                Luminumbra::Filesystem::GenericPathString(entry.path().lexically_relative(root)),
                Read(entry.path()));
    return result;
}
class Root {
public:
    fs::path path = Luminumbra::TestSupport::CreateUniqueTempDirectory("frozen_authority_");
    Root() {
        fs::create_directories(path / "worlds/atlas/presets");
        // A dry plane bounds the small residency exercise without erosion or cave work.
        Write(path / "worlds/atlas/presets/default.json", R"({
            "schema_rev":6,
            "generation_params":{
                "terrain":{"base_frequency":0.01,"base_amplitude":0.0,"octaves":1,
                    "persistence":0.5,"lacunarity":2.0,"height_offset":8.0,
                    "shaping":{"peaks_amplitude":0.0,"domain_warp_amplitude":0.0,
                        "continental_spline":[[-1.0,0.0],[1.0,0.0]]},
                    "hydro":{"enabled":false}},
                "features":{"caves_enabled":false,"cave_frequency":0.02}}})");
        Luminumbra::TestSupport::WriteSimulationContentFixture(path);
    }
    ~Root() {
        std::error_code error;
        fs::remove_all(path, error);
    }
};
class FrozenWorld {
public:
    Root root;
    Luminumbra::JobSystem jobs;
    W::GameSession session;
    FrozenWorld() {
        jobs.startup(1);
        Configure(session);
        if (!session.CreateWorld("Frozen authority", "1337", "default"))
            throw std::runtime_error(session.GetWorldOpenError());
        StartAt(100);
    }
    void Configure(W::GameSession& target) {
        target.SetRootPath(Luminumbra::Filesystem::GenericPathString(root.path) + "/");
        target.SetJobSystem(&jobs);
        target.SetActiveRegionsEnabled(true);
        target.SetAetherStateEnabled(true);
        target.SetRegionSchedulerConfig({0, 1, 2});
    }
    void SetClock(W::GameSession& target, std::uint64_t tick) {
        const_cast<W::WorldClock&>(target.GetWorldClock()).set_tick(tick);
        const_cast<luminumbra::core::SimulationClock&>(target.GetSimulationClock()).reset(tick);
        const_cast<W::ActiveRegionLedger&>(target.GetActiveRegionLedger())
            .acknowledge_world_clock(target.GetWorldClock());
    }
    void StartAt(std::uint64_t tick) {
        const auto identity = session.GetActiveRegionLedger().content_identity();
        if (!identity)
            throw std::runtime_error("Missing fixture identity");
        const W::WorldClock clock(tick);
        SetClock(session, tick);
        auto& ledger = const_cast<W::ActiveRegionLedger&>(session.GetActiveRegionLedger());
        ledger = W::ActiveRegionLedger({0, 1, 2}, clock);
        if (!ledger.adopt_content_identity(*identity))
            throw std::runtime_error("Fixture identity adoption failed");
        ledger.set_local_anchor({8.0f, 100.0f, 8.0f});
        session.SetReplicatedSimulationAnchors({});
    }
};
W::ActiveRegionLedger& Ledger(W::GameSession& session) {
    return const_cast<W::ActiveRegionLedger&>(session.GetActiveRegionLedger());
}
W::ActiveRegionRecord& Record(W::GameSession& session, W::RegionKey key = kOwner) {
    return const_cast<std::map<W::RegionKey, W::ActiveRegionRecord>&>(Ledger(session).records())
        .at(key);
}
void Step(W::GameSession& session) {
    // Sustain budget pressure through the public host input, including while Frozen.
    session.RecordActiveRegionWork({kOwner, 1, 0, 0, 0});
    ASSERT_EQ(session.TickSimulation(session.GetSimulationClock().fixed_dt()), 1u);
    ASSERT_FALSE(session.GetRegionSchedulerFault());
}
entt::entity Creature(W::GameSession& session, bool physics = false) {
    session.PinActiveRegion(kOwner, true);
    const auto& records = session.GetActiveRegionLedger().records();
    EXPECT_FALSE(session.GetRegionSchedulerFault());
    EXPECT_TRUE(records.contains(kOwner));
    if (session.GetRegionSchedulerFault() || !records.contains(kOwner))
        throw std::runtime_error("Creature owner admission failed");
    EXPECT_TRUE(records.at(kOwner).pinned);
    auto& registry = session.GetRegistry();
    const auto e = registry.create();
    registry.emplace<C::TransformComponent>(e).position = kPosition;
    registry.emplace<C::CreatureComponent>(e).species_id =
        C::CreatureSpeciesId16("fixture_creature");
    if (!session.AdmitCreature(e, physics))
        throw std::runtime_error("Creature admission failed");
    auto& coarse = registry.get<C::CreatureCoarseComponent>(e);
    coarse.age_cadences = 7;
    coarse.reproduce_cooldown_cadences = 9;
    coarse.travel_pending = true;
    coarse.destination_x = 11;
    registry.get_or_emplace<C::CreatureLocomotionStateComponent>(e) = {-0.0f, -3.0f, 2.0f, true};
    return e;
}
entt::entity FindCreature(W::GameSession& session) {
    const auto view = session.GetRegistry().view<C::CreatureRegionComponent>();
    if (view.size() != 1)
        throw std::runtime_error("Expected one fixture creature");
    return view.front();
}
std::uint64_t RawDigest(W::GameSession& session) {
    const auto capture = session.CaptureAuthorityState();
    if (!capture.state)
        throw std::runtime_error(capture.error);
    return capture.state->region_content().at(kOwner);
}
void Reduce(W::GameSession& session) {
    ASSERT_EQ(Record(session).state, W::RegionState::Active);
    session.PinActiveRegion(kOwner, false);
    // Admissions request one wake; the next over-budget tick selects Reduced.
    ASSERT_NO_FATAL_FAILURE(Step(session));
    ASSERT_EQ(Record(session).state, W::RegionState::Active);
    ASSERT_NO_FATAL_FAILURE(Step(session));
    ASSERT_EQ(Record(session).state, W::RegionState::Reduced);
}
void Freeze(W::GameSession& session) {
    if (Record(session).state == W::RegionState::Active) {
        ASSERT_NO_FATAL_FAILURE(Reduce(session));
    }
    ASSERT_EQ(Record(session).state, W::RegionState::Reduced);
    const auto before = RawDigest(session);
    const auto last_ticked = Record(session).last_ticked;
    ASSERT_NO_FATAL_FAILURE(Step(session));
    const auto& record = Record(session);
    ASSERT_EQ(record.state, W::RegionState::Frozen);
    EXPECT_EQ(record.frozen_at, session.GetWorldClock().tick());
    EXPECT_EQ(record.last_ticked, last_ticked);
    EXPECT_EQ(record.frozen_digest, before);
    EXPECT_EQ(RawDigest(session), before);
    const auto& schedule = session.GetRegionSchedule();
    ASSERT_EQ(schedule.transitions.size(), 1u);
    EXPECT_EQ(schedule.transitions.front(),
              (W::RegionTransition{kOwner, W::RegionState::Reduced, W::RegionState::Frozen}));
    EXPECT_EQ(std::find(schedule.due.begin(), schedule.due.end(), kOwner), schedule.due.end());
    W::ActiveRegionLedger validated;
    std::string error;
    ASSERT_TRUE(W::ActiveRegionLedger::decode(Ledger(session).encode(), validated, error)) << error;
}
void SeedEnergy(W::GameSession& session) {
    auto* energy = session.GetEnergyFieldState();
    ASSERT_NE(energy, nullptr);
    ASSERT_TRUE(energy->QueueRegionalDeposit(1, 216, 0, 0, 60000));
    const std::array<W::RegionKey, 1> due{{kOwner}};
    luminumbra::fields::EnergyFieldState::RegionalResult result;
    ASSERT_TRUE(energy->TickRegional(
        session.GetWorldClock().tick(), Ledger(session).records(), due, {}, result));
    ASSERT_TRUE(Ledger(session).set_energy_ages(result.ages));
}
struct Roster {
    std::shared_ptr<Luminumbra::Chunk> chunk;
    entt::entity plant = entt::null, creature = entt::null;
    std::uint64_t item = 0;
};
void SeedWaterGrid(Luminumbra::Chunk& chunk) {
    chunk.has_water_sim.store(true);
    chunk.current_water_resolution.store(4);
    chunk.is_water_sleeping.store(true);
    chunk.water_depth_mm.assign(16, 200);
    chunk.water_bed_mm.assign(16, 8000);
    chunk.water_edge_flux.assign(32, 0);
    chunk.water_level_data.assign(16, 8.2f);
    chunk.water_flow_data.assign(16, Luminumbra::Vec2{0.0f});
    chunk.water_sim_terrain_height.assign(16, 8.0f);
    chunk.water_src_mm.clear();
    chunk.water_rest_level.clear();
}
Roster Populate(W::GameSession& session) {
    Roster roster;
    roster.chunk = std::make_shared<Luminumbra::Chunk>(Luminumbra::IVec3{320, 0, 0});
    roster.chunk->set_state(Luminumbra::ChunkState::Idle);
    roster.chunk->heightmap_data = {8.0f};
    SeedWaterGrid(*roster.chunk);
    roster.chunk->mark_voxel_data_dirty();
    if (!session.GetWorldSystem()->adopt_streamed_chunk(roster.chunk))
        throw std::runtime_error("Chunk admission failed");
    auto& registry = session.GetRegistry();
    roster.plant = registry.create();
    registry.emplace<C::TransformComponent>(roster.plant).position = kPosition;
    registry.emplace<C::PlantTag>(roster.plant);
    registry.emplace<C::PlantGenomeComponent>(roster.plant);
    registry.emplace<C::PlantGrowthComponent>(roster.plant).species_id =
        W::SimulationSpeciesId("fixture_plant");
    if (!session.AdmitPlant(roster.plant))
        throw std::runtime_error("Plant admission failed");
    roster.creature = Creature(session);
    roster.item = session.SpawnGroundObject(7, 5'200'000, 100'000, 8'000, 1000, 17);
    if (!roster.item)
        throw std::runtime_error("Ground admission failed");
    auto& fields = const_cast<W::FieldPageState&>(session.GetFieldPages());
    if (!fields.activate_region(kOwner, 1337, session.GetWorldClock().tick()) ||
        !fields.set_soil({5200, 8}, 123456) || !fields.set_irrigation({5200, 8}, 654321) ||
        !fields.set_scent({5200, 8}, {std::bit_cast<std::uint64_t>(0.5), 0, 0, 0}))
        throw std::runtime_error("Field admission failed");
    W::RegionalStorm storm;
    storm.owner = kOwner;
    storm.center_x = std::bit_cast<std::uint32_t>(kPosition.x);
    storm.center_z = std::bit_cast<std::uint32_t>(kPosition.z);
    storm.intensity = std::bit_cast<std::uint32_t>(0.75f);
    storm.spawn_tick = 10;
    storm.lifetime_ticks = 1000;
    storm.seed_salt = 7;
    storm.active_age = 3;
    W::RegionalStrike strike;
    strike.owner = kOwner;
    strike.strike_tick = 200;
    strike.world_x = storm.center_x;
    strike.world_z = storm.center_z;
    strike.magnitude = std::bit_cast<std::uint32_t>(1.0f);
    strike.storm_salt = 7;
    if (!fields.replace_weather({storm}, {strike}) || !fields.set_weather_age(kOwner, 3))
        throw std::runtime_error("Weather admission failed");
    SeedEnergy(session);
    return roster;
}
std::map<std::string, std::string> ConsumerBytes(W::GameSession& session) {
    return {{"creatures",
             A::SerializeCreatureSnapshot(A::BuildCreatureSnapshot(session.GetRegistry()))},
            {"plants",
             Luminumbra::Ecs::SerializeEntityRegistrySnapshotJson(
                 luminumbra::foliage::BuildPlantEntitySnapshot(session.GetRegistry(), true))},
            {"items", session.GetGroundObjects().canonical_bytes()},
            {"fields", session.GetFieldPages().region_bytes(kOwner)},
            {"energy", session.GetEnergyFieldState()->RegionBytes(kOwner)}};
}
std::uint64_t Integer(std::string_view bytes, std::size_t offset, unsigned width) {
    std::uint64_t value = 0;
    for (unsigned i = 0; i < width; ++i)
        value |= std::uint64_t{static_cast<unsigned char>(bytes.at(offset + i))} << (8 * i);
    return value;
}
void Put(std::string& bytes, std::size_t offset, std::uint64_t value, unsigned width) {
    for (unsigned i = 0; i < width; ++i)
        bytes.at(offset + i) = static_cast<char>((value >> (8 * i)) & 255);
}
void Seal(std::string& bytes) {
    Put(bytes, bytes.size() - 8, P::fnv1a64_bytes(bytes.data(), bytes.size() - 8), 8);
}
std::size_t ExtensionOffset(std::string_view bytes) {
    return W::ActiveRegionLedger::kHeaderBytes +
           W::ActiveRegionLedger::kRecordBytes * Integer(bytes, 52, 4);
}
std::map<unsigned, std::string> Extensions(const std::string& bytes) {
    std::map<unsigned, std::string> result;
    if (Integer(bytes, 4, 2) == 1)
        return result;
    for (auto at = ExtensionOffset(bytes) + 4; at < bytes.size() - 8;) {
        const auto type = static_cast<unsigned>(Integer(bytes, at, 2));
        const auto length = static_cast<std::size_t>(Integer(bytes, at + 2, 4));
        result.emplace(type, bytes.substr(at, 6 + length));
        at += 6 + length;
    }
    return result;
}
std::string Envelope(const std::string& original,
                     const std::map<unsigned, std::string>& extensions) {
    auto bytes = original.substr(0, ExtensionOffset(original));
    Put(bytes, 4, extensions.empty() ? 1 : 2, 2);
    if (!extensions.empty()) {
        std::string tlvs;
        for (const auto& [type, encoded] : extensions) {
            (void)type;
            tlvs += encoded;
        }
        bytes.resize(bytes.size() + 4);
        Put(bytes, bytes.size() - 4, tlvs.size(), 4);
        bytes += tlvs;
    }
    bytes.resize(bytes.size() + 8);
    Seal(bytes);
    return bytes;
}
// Install historical or deliberately malformed bytes with a matching membership fingerprint.
// This isolates semantic refusal from an unrelated index digest mismatch.
void ReplaceLedgerFixture(const fs::path& save, const std::string& bytes) {
    P::SnapshotIndex index;
    std::vector<std::string> errors;
    ASSERT_TRUE(P::read_snapshot_index(save, index, &errors));
    ASSERT_NE(index.generation, 0u);
    Write(P::WorldSaveService::active_regions_path(save), bytes);
    bool found = false;
    for (auto& member : index.members)
        if (member.path == "chunks/region/active-regions.arl") {
            member.bytes = bytes.size();
            member.fnv64 = P::fnv1a64_bytes(bytes.data(), bytes.size());
            found = true;
        }
    ASSERT_TRUE(found);
    Write(P::snapshot_index_path(save), P::encode_snapshot_index(index));
}

TEST(FrozenAuthority, V030MissingIdentityAndProjectionAdoptThenStamp) {
    FrozenWorld fixture;
    auto& original = fixture.session;
    const auto roster = Populate(original);
    ASSERT_NO_FATAL_FAILURE(Freeze(original));
    const auto digest = Record(original).frozen_digest;
    ASSERT_EQ(digest, RawDigest(original));
    const auto content = ConsumerBytes(original);
    const auto next = Ledger(original).next_persistent_id();
    ASSERT_TRUE(original.SaveWorldState());
    const auto save = original.GetWorldSaveDir();
    const auto path = P::WorldSaveService::active_regions_path(save);
    auto extensions = Extensions(Read(path));
    ASSERT_EQ(extensions.erase(2), 1u);
    ASSERT_EQ(extensions.erase(5), 1u);
    // Only the historical fixture has an opaque recipe; retain the live scheduler stamp.
    auto historical_ledger = Ledger(original);
    const_cast<std::map<W::RegionKey, W::ActiveRegionRecord>&>(historical_ledger.records())
        .at(kOwner)
        .frozen_digest = digest ^ 0x1234u;
    const auto historical = Envelope(historical_ledger.encode(), extensions);
    ASSERT_NO_FATAL_FAILURE(ReplaceLedgerFixture(save, historical));
    W::ActiveRegionLedger decoded;
    std::string error;
    ASSERT_TRUE(W::ActiveRegionLedger::decode(historical, decoded, error)) << error;
    ASSERT_FALSE(decoded.content_identity());
    ASSERT_TRUE(decoded.needs_frozen_projection_adoption());
    ASSERT_NE(decoded.records().at(kOwner).frozen_digest, digest);
    const auto disk_before = Tree(save);
    W::GameSession loaded;
    fixture.Configure(loaded);
    ASSERT_TRUE(loaded.LoadWorld(original.GetMetadata().worldId)) << loaded.GetWorldOpenError();
    EXPECT_EQ(Tree(save), disk_before);
    EXPECT_EQ(loaded.GetWorldClock().tick(), original.GetWorldClock().tick());
    EXPECT_EQ(Ledger(loaded).next_persistent_id(), next);
    EXPECT_EQ(ConsumerBytes(loaded), content);
    EXPECT_EQ(RawDigest(loaded), digest);
    EXPECT_EQ(Record(loaded).frozen_digest, digest);
    EXPECT_EQ(Record(loaded).frozen_at, Record(original).frozen_at);
    EXPECT_FALSE(Ledger(loaded).needs_frozen_projection_adoption());
    EXPECT_EQ(Ledger(loaded).content_identity(), Ledger(original).content_identity());
    ASSERT_NE(loaded.GetGroundObjects().find(roster.item), nullptr);

    W::GameSession explicit_load;
    fixture.Configure(explicit_load);
    ASSERT_TRUE(explicit_load.CreateWorld("Explicit snapshot", "1337", "default"));
    ASSERT_TRUE(explicit_load.LoadWorldStateFrom(save)) << explicit_load.GetWorldOpenError();
    EXPECT_EQ(Record(explicit_load).frozen_digest, digest);
    EXPECT_EQ(Record(explicit_load).frozen_digest, Record(loaded).frozen_digest);
    EXPECT_EQ(RawDigest(explicit_load), digest);
    EXPECT_EQ(ConsumerBytes(explicit_load), content);
    EXPECT_EQ(Ledger(explicit_load).canonical_bytes(), Ledger(loaded).canonical_bytes());
    EXPECT_EQ(explicit_load.GetWorldClock().tick(), original.GetWorldClock().tick());
    EXPECT_EQ(Tree(save), disk_before);

    P::WorldSaveService::set_before_active_regions_replace_for_testing([] { return false; });
    const bool failed_save = loaded.SaveWorldState();
    P::WorldSaveService::set_before_active_regions_replace_for_testing(nullptr);
    EXPECT_FALSE(failed_save);
    EXPECT_EQ(Tree(save), disk_before);
    EXPECT_EQ(Record(loaded).frozen_digest, digest);
    ASSERT_TRUE(loaded.SaveWorldState());
    const auto stamped = Extensions(Read(path));
    ASSERT_TRUE(stamped.contains(2));
    EXPECT_EQ(stamped.at(2).substr(6), std::string(1, '\x02'));
    EXPECT_TRUE(stamped.contains(5));
    W::GameSession reopened;
    fixture.Configure(reopened);
    ASSERT_TRUE(reopened.LoadWorld(original.GetMetadata().worldId)) << reopened.GetWorldOpenError();
    EXPECT_EQ(ConsumerBytes(reopened), content);
    EXPECT_EQ(Record(reopened).frozen_digest, digest);
    EXPECT_EQ(Ledger(reopened).canonical_bytes(), Ledger(loaded).canonical_bytes());
}

TEST(FrozenAuthority, PresentMalformedProjectionRefusesAtomically) {
    FrozenWorld fixture;
    Creature(fixture.session);
    ASSERT_NO_FATAL_FAILURE(Freeze(fixture.session));
    ASSERT_TRUE(fixture.session.SaveWorldState());
    const auto save = fixture.session.GetWorldSaveDir();
    const auto good = Read(P::WorldSaveService::active_regions_path(save));
    W::GameSession current;
    fixture.Configure(current);
    ASSERT_TRUE(current.CreateWorld("Current authority", "1337", "default"));
    Creature(current);
    const auto* world = current.GetWorldSystem();
    const auto id = current.GetMetadata().worldId;
    const auto ledger = Ledger(current).encode();
    const auto creatures = ConsumerBytes(current);
    const auto capture = current.CaptureAuthorityState();
    ASSERT_TRUE(capture.state) << capture.error;
    const auto current_disk = Tree(current.GetWorldSaveDir());
    for (unsigned scenario = 0; scenario < 6; ++scenario) {
        SCOPED_TRACE(scenario);
        auto tlvs = Extensions(good);
        if (scenario < 3)
            tlvs.at(2).at(6) = static_cast<char>(scenario == 2 ? 3 : scenario);
        else if (scenario == 3)
            tlvs.at(2) += tlvs.at(2); // duplicate projection
        else if (scenario == 4)
            Put(tlvs.at(2), 2, 0, 4); // present with invalid length
        else
            tlvs.at(5).at(6) = '\0'; // present invalid identity, never absence
        const auto malformed = Envelope(good, tlvs);
        auto destination = Ledger(current);
        std::string error;
        EXPECT_FALSE(W::ActiveRegionLedger::decode(malformed, destination, error));
        EXPECT_EQ(destination.encode(), ledger);
        ASSERT_NO_FATAL_FAILURE(ReplaceLedgerFixture(save, malformed));
        const auto target_disk = Tree(save);
        EXPECT_FALSE(current.LoadWorld(fixture.session.GetMetadata().worldId));
        EXPECT_FALSE(current.GetWorldOpenError().empty());
        EXPECT_EQ(current.GetWorldSystem(), world);
        EXPECT_EQ(current.GetMetadata().worldId, id);
        EXPECT_EQ(Ledger(current).encode(), ledger);
        EXPECT_EQ(ConsumerBytes(current), creatures);
        const auto after = current.CaptureAuthorityState();
        ASSERT_TRUE(after.state) << after.error;
        EXPECT_EQ(after.state->authority_bytes(), capture.state->authority_bytes());
        EXPECT_EQ(Tree(save), target_disk);
        EXPECT_EQ(Tree(current.GetWorldSaveDir()), current_disk);
    }
    // The direct loader keeps its integrity refusal latch and cannot save over the source.
    EXPECT_FALSE(current.LoadWorldStateFrom(save));
    EXPECT_FALSE(current.SaveWorldState());
    EXPECT_EQ(current.GetWorldSystem(), world);
    EXPECT_EQ(Ledger(current).encode(), ledger);
    EXPECT_EQ(ConsumerBytes(current), creatures);
}

TEST(FrozenAuthority, FrozenEveryAuthorityDomainStableAcrossResidencySaveLoad) {
    FrozenWorld fixture;
    auto& session = fixture.session;
    const auto roster = Populate(session);
    ASSERT_NO_FATAL_FAILURE(Freeze(session));
    const auto baseline = RawDigest(session);
    const auto content = ConsumerBytes(session);
    const auto heights = roster.chunk->heightmap_data;
    const auto depths = roster.chunk->water_depth_mm;
    const auto ages = Ledger(session).energy_ages();
    // Each domain has an independent positive control; restore it before the stability run.
    const auto expect_edit = [&] {
        EXPECT_NE(RawDigest(session), baseline);
        const auto capture = session.CaptureAuthorityState();
        ASSERT_TRUE(capture.state) << capture.error;
        EXPECT_EQ(capture.state->region_contributions().at(kOwner), baseline);
    };
    roster.chunk->heightmap_data[0] += 1.0f;
    expect_edit();
    roster.chunk->heightmap_data = heights;
    ++roster.chunk->water_depth_mm[0];
    expect_edit();
    roster.chunk->water_depth_mm = depths;
    auto& registry = session.GetRegistry();
    auto& growth = registry.get<C::PlantGrowthComponent>(roster.plant);
    ++growth.growth_points;
    expect_edit();
    --growth.growth_points;
    auto& coarse = registry.get<C::CreatureCoarseComponent>(roster.creature);
    ++coarse.age_cadences;
    expect_edit();
    --coarse.age_cadences;
    auto& ground = const_cast<W::GroundObjectStore&>(session.GetGroundObjects());
    ASSERT_NE(ground.find(roster.item), nullptr);
    const auto item = *ground.find(roster.item);
    auto changed_item = item;
    ++changed_item.payload;
    ASSERT_TRUE(ground.erase(item.id));
    ASSERT_TRUE(ground.insert(changed_item));
    expect_edit();
    ASSERT_TRUE(ground.erase(item.id));
    ASSERT_TRUE(ground.insert(item));
    auto& fields = const_cast<W::FieldPageState&>(session.GetFieldPages());
    const auto saved_fields = fields;
    ASSERT_TRUE(fields.set_soil({5200, 8}, 123457));
    expect_edit();
    fields = saved_fields;
    auto& energy = *session.GetEnergyFieldState();
    const auto saved_energy = energy;
    auto active = Ledger(session).records();
    active.at(kOwner).state = W::RegionState::Active;
    active.at(kOwner).cadence_shift = 0;
    active.at(kOwner).cadence_phase = 0;
    const std::array<W::RegionKey, 1> due{{kOwner}};
    luminumbra::fields::EnergyFieldState::RegionalResult result;
    ASSERT_TRUE(energy.QueueRegionalDeposit(2, 216, 0, 0, 1));
    ASSERT_TRUE(energy.TickRegional(session.GetWorldClock().tick() + 1, active, due, ages, result));
    expect_edit();
    energy = saved_energy;
    EXPECT_EQ(RawDigest(session), baseline);
    for (int tick = 0; tick < 3; ++tick) {
        ASSERT_NO_FATAL_FAILURE(Step(session));
        EXPECT_EQ(Record(session).state, W::RegionState::Frozen);
        EXPECT_EQ(RawDigest(session), baseline);
        EXPECT_EQ(ConsumerBytes(session), content);
        EXPECT_EQ(Ledger(session).energy_ages(), ages);
    }
    auto* world = session.GetWorldSystem();
    world->debug_set_streaming_radius_cap(1);
    for (int frame = 0; frame < 4; ++frame) {
        world->update(registry, Luminumbra::Vec3{8.0f, 100.0f, 8.0f}, session.GetPhysicsSystem());
        world->wait_for_streaming_jobs();
    }
    ASSERT_FALSE(world->find_streamed_chunk(roster.chunk->get_coords()));
    ASSERT_EQ(world->find_authoritative_chunk(roster.chunk->get_coords()), roster.chunk);
    ASSERT_FALSE(world->snapshot_parked_chunks().empty());
    EXPECT_EQ(RawDigest(session), baseline);
    EXPECT_EQ(ConsumerBytes(session), content);
    ASSERT_TRUE(session.SaveWorldState());
    EXPECT_EQ(RawDigest(session), baseline);
    EXPECT_EQ(ConsumerBytes(session), content);
    EXPECT_FALSE(world->find_authoritative_chunk(roster.chunk->get_coords()));
    W::GameSession loaded;
    fixture.Configure(loaded);
    ASSERT_TRUE(loaded.LoadWorld(session.GetMetadata().worldId)) << loaded.GetWorldOpenError();
    loaded.SetReplicatedSimulationAnchors({});
    EXPECT_EQ(ConsumerBytes(loaded), content);
    EXPECT_EQ(RawDigest(loaded), baseline);
    EXPECT_EQ(Record(loaded).frozen_digest, baseline);
    EXPECT_EQ(Ledger(loaded).energy_ages(), ages);
    ASSERT_TRUE(loaded.GetWorldSystem()->restore_authoritative_chunk(roster.chunk->get_coords()));
    ASSERT_FALSE(loaded.GetRegionSchedulerFault());
    const auto restored = loaded.GetWorldSystem()->find_streamed_chunk(roster.chunk->get_coords());
    ASSERT_TRUE(restored);
    EXPECT_EQ(restored->heightmap_data, heights);
    EXPECT_EQ(restored->water_depth_mm, depths);
    EXPECT_EQ(RawDigest(loaded), baseline);
    // Thaw is allowed to evolve content, and continuation must agree across restart.
    ASSERT_TRUE(world->restore_authoritative_chunk(roster.chunk->get_coords()));
    ASSERT_FALSE(session.GetRegionSchedulerFault());
    session.PinActiveRegion(kOwner, true);
    loaded.PinActiveRegion(kOwner, true);
    ASSERT_NO_FATAL_FAILURE(Step(session));
    ASSERT_NO_FATAL_FAILURE(Step(loaded));
    EXPECT_EQ(Record(loaded).state, W::RegionState::Active);
    EXPECT_EQ(ConsumerBytes(loaded), ConsumerBytes(session));
    EXPECT_EQ(RawDigest(loaded), RawDigest(session));
    EXPECT_NE(RawDigest(loaded), baseline);
}

TEST(FrozenAuthority, EnabledDiskOnlyRestorePreservesFrozenWater) {
    FrozenWorld fixture;
    auto& session = fixture.session;
    const auto roster = Populate(session);
    ASSERT_NO_FATAL_FAILURE(Reduce(session));
    // Install a valid mismatched grid before the production freeze boundary.
    SeedWaterGrid(*roster.chunk);
    roster.chunk->water_edge_flux[0] = 7;
    roster.chunk->ticks_below_threshold = 19;
    roster.chunk->max_water_delta_last_tick = 0.125f;
    roster.chunk->mark_water_state_dirty();
    ASSERT_NO_FATAL_FAILURE(Freeze(session));
    const auto baseline = RawDigest(session);
    ASSERT_TRUE(session.SaveWorldState());
    W::GameSession loaded;
    fixture.Configure(loaded);
    ASSERT_TRUE(loaded.LoadWorld(session.GetMetadata().worldId)) << loaded.GetWorldOpenError();
    loaded.SetReplicatedSimulationAnchors({});
    auto* world = loaded.GetWorldSystem();
    const auto coords = roster.chunk->get_coords();
    ASSERT_FALSE(world->find_streamed_chunk(coords));
    ASSERT_TRUE(world->snapshot_parked_chunks().empty());
    const auto inventory = world->durable_chunk_inventory();
    ASSERT_NE(std::find(inventory.begin(), inventory.end(), coords), inventory.end());
    ASSERT_FALSE(loaded.GetRegionSchedulerFault());
    ASSERT_EQ(Record(loaded).state, W::RegionState::Frozen);
    const auto stamp = Record(loaded).frozen_digest;
    const auto frozen_at = Record(loaded).frozen_at;
    const auto ledger = Ledger(loaded).encode();
    const auto work = loaded.GetWaterSystem()->regional_work();
    const auto disk = Tree(session.GetWorldSaveDir());
    ASSERT_EQ(stamp, baseline);
    // A saved grid need not match the target: restoring it must not perform a resize.
    ASSERT_NE(roster.chunk->current_water_resolution.load(), world->debug_water_sim_resolution());
    ASSERT_TRUE(world->restore_authoritative_chunk(coords));
    ASSERT_FALSE(loaded.GetRegionSchedulerFault());
    ASSERT_FALSE(world->durable_read_failed());
    const auto restored = world->find_streamed_chunk(coords);
    ASSERT_TRUE(restored);
    EXPECT_EQ(restored->has_water_sim.load(), roster.chunk->has_water_sim.load());
    EXPECT_EQ(restored->current_water_resolution.load(),
              roster.chunk->current_water_resolution.load());
    EXPECT_EQ(restored->water_depth_mm, roster.chunk->water_depth_mm);
    EXPECT_EQ(restored->water_bed_mm, roster.chunk->water_bed_mm);
    EXPECT_EQ(restored->water_edge_flux, roster.chunk->water_edge_flux);
    EXPECT_EQ(restored->is_water_sleeping.load(), roster.chunk->is_water_sleeping.load());
    EXPECT_EQ(restored->ticks_below_threshold, roster.chunk->ticks_below_threshold);
    EXPECT_EQ(restored->max_water_delta_last_tick, roster.chunk->max_water_delta_last_tick);
    EXPECT_EQ(restored->water_level_data, roster.chunk->water_level_data);
    EXPECT_EQ(restored->water_flow_data, roster.chunk->water_flow_data);
    EXPECT_EQ(restored->water_sim_terrain_height, roster.chunk->water_sim_terrain_height);
    EXPECT_EQ(RawDigest(loaded), baseline);
    EXPECT_EQ(Record(loaded).state, W::RegionState::Frozen);
    EXPECT_EQ(Record(loaded).frozen_digest, stamp);
    EXPECT_EQ(Record(loaded).frozen_at, frozen_at);
    EXPECT_EQ(Ledger(loaded).encode(), ledger);
    EXPECT_EQ(loaded.GetWaterSystem()->regional_work(), work);
    EXPECT_EQ(Tree(session.GetWorldSaveDir()), disk);
    ASSERT_NO_FATAL_FAILURE(Step(loaded));
    EXPECT_EQ(Record(loaded).state, W::RegionState::Frozen);
    EXPECT_EQ(Record(loaded).frozen_digest, stamp);
    EXPECT_EQ(RawDigest(loaded), baseline);
    EXPECT_EQ(restored->current_water_resolution.load(),
              roster.chunk->current_water_resolution.load());
}

TEST(FrozenAuthority, FlagOffDiskOnlyRestoreStillMigratesWater) {
    FrozenWorld fixture;
    const auto roster = Populate(fixture.session);
    ASSERT_NO_FATAL_FAILURE(Reduce(fixture.session));
    SeedWaterGrid(*roster.chunk);
    roster.chunk->mark_water_state_dirty();
    ASSERT_NO_FATAL_FAILURE(Freeze(fixture.session));
    ASSERT_TRUE(fixture.session.SaveWorldState());
    const auto directory = fixture.session.GetWorldSaveDir();
    const auto disk = Tree(directory);
    W::GameSession legacy;
    fixture.Configure(legacy);
    legacy.SetActiveRegionsEnabled(false);
    ASSERT_TRUE(legacy.CreateWorld("Legacy water restoration", "1337", "default"));
    auto* world = legacy.GetWorldSystem();
    const auto coords = roster.chunk->get_coords();
    world->set_durable_residency(true);
    world->remember_durable_chunks({coords});
    int reads = 0;
    std::vector<std::string> faults;
    world->set_durable_chunk_loader(
        [&](const Luminumbra::IVec3& requested) {
            ++reads;
            const auto result = P::WorldSaveService::read_chunk(directory, requested, true);
            if (result.status != P::WorldSaveService::ChunkReadStatus::Found)
                throw std::runtime_error(result.error);
            return result.chunk;
        },
        [&](std::string error) { faults.push_back(error); });
    ASSERT_FALSE(world->find_streamed_chunk(coords));
    ASSERT_NE(roster.chunk->current_water_resolution.load(), world->debug_water_sim_resolution());
    ASSERT_TRUE(world->restore_authoritative_chunk(coords));
    ASSERT_FALSE(legacy.GetRegionSchedulerFault());
    ASSERT_FALSE(world->durable_read_failed());
    EXPECT_TRUE(faults.empty());
    EXPECT_EQ(reads, 1);
    const auto restored = world->find_streamed_chunk(coords);
    ASSERT_TRUE(restored);
    const auto resolution = world->debug_water_sim_resolution();
    EXPECT_EQ(restored->current_water_resolution.load(), resolution);
    EXPECT_EQ(restored->water_depth_mm.size(), static_cast<std::size_t>(resolution * resolution));
    EXPECT_TRUE(restored->is_water_state_dirty());
    EXPECT_EQ(world->debug_water_chunks_off_resolution(), 0u);
    EXPECT_EQ(Tree(directory), disk);
}

TEST(FrozenAuthority, EnabledDiskOnlyRestoreMissingAuthorityStillFaults) {
    FrozenWorld fixture;
    Populate(fixture.session);
    ASSERT_NO_FATAL_FAILURE(Freeze(fixture.session));
    ASSERT_TRUE(fixture.session.SaveWorldState());
    W::GameSession loaded;
    fixture.Configure(loaded);
    ASSERT_TRUE(loaded.LoadWorld(fixture.session.GetMetadata().worldId))
        << loaded.GetWorldOpenError();
    auto* world = loaded.GetWorldSystem();
    const Luminumbra::IVec3 missing{321, 0, 0};
    world->remember_durable_chunks({missing});
    const auto disk = Tree(fixture.session.GetWorldSaveDir());
    const auto tick = loaded.GetWorldClock().tick();
    ASSERT_FALSE(loaded.GetRegionSchedulerFault());
    // True suppresses generation of expected authority even when restoration fails.
    ASSERT_TRUE(world->restore_authoritative_chunk(missing));
    ASSERT_TRUE(loaded.GetRegionSchedulerFault());
    EXPECT_TRUE(world->durable_read_failed());
    EXPECT_FALSE(world->find_streamed_chunk(missing));
    EXPECT_EQ(loaded.TickSimulation(loaded.GetSimulationClock().fixed_dt()), 0u);
    EXPECT_EQ(loaded.GetWorldClock().tick(), tick);
    EXPECT_TRUE(world->restore_authoritative_chunk(missing));
    EXPECT_FALSE(world->find_streamed_chunk(missing));
    EXPECT_EQ(Tree(fixture.session.GetWorldSaveDir()), disk);
}

TEST(FrozenAuthority, FrozenControllerCleanupPreservesContinuation) {
    FrozenWorld fixture;
    auto& session = fixture.session;
    const auto creature = Creature(session, true);
    auto& registry = session.GetRegistry();
    ASSERT_NO_FATAL_FAILURE(Reduce(session));
    const auto before = ConsumerBytes(session);
    // Deliberately disagree with durable continuation at the production freeze boundary.
    const auto handle =
        session.GetPhysicsSystem()->create_avatar_handle(kPosition, {9, -8, 7}, false);
    registry.emplace<C::CreaturePhysicsComponent>(creature, handle);
    ASSERT_NO_FATAL_FAILURE(Freeze(session));
    const auto content = ConsumerBytes(session);
    const auto digest = RawDigest(session);
    EXPECT_EQ(content, before);
    EXPECT_FALSE(registry.all_of<C::CreaturePhysicsComponent>(creature));
    EXPECT_FALSE(session.GetPhysicsSystem()->valid_avatar_handle(handle));
    // A deferred runtime attachment must also survive save without being sampled.
    const auto deferred =
        session.GetPhysicsSystem()->create_avatar_handle(kPosition, {9, -8, 7}, false);
    registry.emplace<C::CreaturePhysicsComponent>(creature, deferred);
    ASSERT_TRUE(session.SaveWorldState());
    EXPECT_EQ(ConsumerBytes(session), content);
    EXPECT_TRUE(session.GetPhysicsSystem()->valid_avatar_handle(deferred));
    ASSERT_NO_FATAL_FAILURE(Step(session));
    EXPECT_FALSE(registry.all_of<C::CreaturePhysicsComponent>(creature));
    EXPECT_FALSE(session.GetPhysicsSystem()->valid_avatar_handle(deferred));
    EXPECT_TRUE(registry.get<C::CreatureControllerComponent>(creature).physics_controlled);
    EXPECT_EQ(ConsumerBytes(session), content);
    EXPECT_EQ(RawDigest(session), digest);
    ASSERT_TRUE(session.SaveWorldState());
    W::GameSession loaded;
    fixture.Configure(loaded);
    ASSERT_TRUE(loaded.LoadWorld(session.GetMetadata().worldId)) << loaded.GetWorldOpenError();
    EXPECT_EQ(ConsumerBytes(loaded), content);
    loaded.SetReplicatedSimulationAnchors({kPosition});
    session.SetReplicatedSimulationAnchors({kPosition});
    session.PinActiveRegion(kOwner, true);
    loaded.PinActiveRegion(kOwner, true);
    ASSERT_NO_FATAL_FAILURE(Step(session));
    ASSERT_NO_FATAL_FAILURE(Step(loaded));
    const auto restored = FindCreature(loaded);
    ASSERT_TRUE(loaded.GetRegistry().all_of<C::CreaturePhysicsComponent>(restored));
    ASSERT_TRUE(registry.all_of<C::CreaturePhysicsComponent>(creature));
    EXPECT_EQ(ConsumerBytes(loaded), ConsumerBytes(session));
    EXPECT_EQ(loaded.GetPhysicsSystem()->get_avatar_velocity(
                  loaded.GetRegistry().get<C::CreaturePhysicsComponent>(restored).avatar_index),
              session.GetPhysicsSystem()->get_avatar_velocity(
                  registry.get<C::CreaturePhysicsComponent>(creature).avatar_index));
}

TEST(FrozenAuthority, ThawDoesNotApplyCreatureOrEnergyCatchup) {
    FrozenWorld fixture;
    fixture.StartAt(1000);
    auto& session = fixture.session;
    const auto creature = Creature(session);
    session.GetRegistry().get<C::CreatureCoarseComponent>(creature).tier = C::CreatureTier::Coarse;
    ASSERT_NO_FATAL_FAILURE(SeedEnergy(session));
    // One ordinary due tick before freezing leaves the next fire at age eight.
    ASSERT_TRUE(Ledger(session).set_energy_age(kOwner, 6));
    ASSERT_NO_FATAL_FAILURE(Reduce(session));
    ASSERT_TRUE(session.SaveWorldState());
    W::GameSession recent;
    fixture.Configure(recent);
    ASSERT_TRUE(recent.LoadWorld(session.GetMetadata().worldId)) << recent.GetWorldOpenError();
    recent.SetReplicatedSimulationAnchors({});
    ASSERT_NO_FATAL_FAILURE(Freeze(session));
    const auto before = ConsumerBytes(session);
    const auto digest = Record(session).frozen_digest;
    ASSERT_EQ(session.GetRegistry().get<C::CreatureCoarseComponent>(creature).age_cadences, 8u);
    ASSERT_EQ(
        session.GetRegistry().get<C::CreatureCoarseComponent>(creature).reproduce_cooldown_cadences,
        8u);
    // Restore a clock gap without stepping consumers or changing either owner's history.
    fixture.SetClock(session, 2002);
    fixture.SetClock(recent, 2002);
    ASSERT_NO_FATAL_FAILURE(Step(session));
    ASSERT_NO_FATAL_FAILURE(Freeze(recent));
    ASSERT_EQ(Record(session).state, W::RegionState::Frozen);
    ASSERT_EQ(Record(recent).frozen_at - Record(session).frozen_at, 1000u);
    EXPECT_EQ(Record(recent).frozen_digest, digest);
    EXPECT_EQ(ConsumerBytes(session), before);
    EXPECT_EQ(ConsumerBytes(recent), before);
    ASSERT_EQ(Ledger(session).energy_ages().at(kOwner), 7u);
    ASSERT_EQ(Ledger(recent).energy_ages().at(kOwner), 7u);
    ASSERT_TRUE(session.SaveWorldState());
    auto expected_energy = *session.GetEnergyFieldState();
    session.PinActiveRegion(kOwner, true);
    recent.PinActiveRegion(kOwner, true);
    ASSERT_NO_FATAL_FAILURE(Step(session));
    ASSERT_NO_FATAL_FAILURE(Step(recent));
    const auto& coarse = session.GetRegistry().get<C::CreatureCoarseComponent>(creature);
    EXPECT_EQ(coarse.age_cadences, 9u);
    EXPECT_EQ(coarse.reproduce_cooldown_cadences, 7u);
    EXPECT_EQ(Ledger(session).energy_ages().at(kOwner), 8u);
    EXPECT_EQ(Ledger(recent).energy_ages().at(kOwner), 8u);
    EXPECT_EQ(Record(session).frozen_at, 0u);
    EXPECT_EQ(Record(recent).frozen_at, 0u);
    const std::array<W::RegionKey, 1> due{{kOwner}};
    luminumbra::fields::EnergyFieldState::RegionalResult result;
    ASSERT_TRUE(expected_energy.TickRegional(
        session.GetWorldClock().tick(), Ledger(session).records(), due, {{kOwner, 7}}, result));
    EXPECT_EQ(session.GetEnergyFieldState()->CanonicalRegionalBytes(),
              expected_energy.CanonicalRegionalBytes());
    EXPECT_GT(session.GetEnergyFieldState()->total_raw(), 0u);
    EXPECT_EQ(ConsumerBytes(session), ConsumerBytes(recent));
    EXPECT_NE(ConsumerBytes(session).at("creatures"), before.at("creatures"));
    EXPECT_NE(ConsumerBytes(session).at("energy"), before.at("energy"));
}

TEST(FrozenAuthority, FailedPublicReplacementPreservesCurrentSaveability) {
    FrozenWorld fixture;
    auto& current = fixture.session;
    Creature(current);
    const auto item = current.SpawnGroundObject(7, 5'200'000, 100'000, 8'000, 1000, 99);
    ASSERT_NE(item, 0u);
    const auto id = current.GetMetadata().worldId;
    const auto* world = current.GetWorldSystem();
    const auto ledger = Ledger(current).canonical_bytes();
    const auto content = ConsumerBytes(current);
    const auto current_disk = Tree(current.GetWorldSaveDir());
    W::GameSession target;
    fixture.Configure(target);
    ASSERT_TRUE(target.CreateWorld("Rejected replacement", "1337", "default"));
    Creature(target);
    ASSERT_TRUE(target.SaveWorldState());
    const auto target_save = target.GetWorldSaveDir();
    const auto path = P::WorldSaveService::active_regions_path(target_save);
    auto tlvs = Extensions(Read(path));
    tlvs.at(5).at(6) = '\0';
    ASSERT_NO_FATAL_FAILURE(ReplaceLedgerFixture(target_save, Envelope(Read(path), tlvs)));
    const auto rejected_disk = Tree(target_save);
    for (const auto& rejected : {target.GetMetadata().worldId, std::string("missing-world")}) {
        EXPECT_FALSE(current.LoadWorld(rejected));
        EXPECT_FALSE(current.GetWorldOpenError().empty());
        EXPECT_EQ(current.GetMetadata().worldId, id);
        EXPECT_EQ(current.GetWorldSystem(), world);
        EXPECT_EQ(current.GetWorldClock().tick(), 100u);
        EXPECT_EQ(Ledger(current).canonical_bytes(), ledger);
        EXPECT_EQ(ConsumerBytes(current), content);
        EXPECT_EQ(Tree(current.GetWorldSaveDir()), current_disk);
        EXPECT_EQ(Tree(target_save), rejected_disk);
    }
    ASSERT_TRUE(current.SaveWorld());
    ASSERT_TRUE(current.SaveWorldState());
    EXPECT_EQ(ConsumerBytes(current), content);
    EXPECT_EQ(Tree(target_save), rejected_disk);
    W::GameSession recovered;
    fixture.Configure(recovered);
    ASSERT_TRUE(recovered.LoadWorld(id)) << recovered.GetWorldOpenError();
    EXPECT_EQ(ConsumerBytes(recovered), content);
    EXPECT_EQ(Ledger(recovered).canonical_bytes(), ledger);
    EXPECT_NE(recovered.GetGroundObjects().find(item), nullptr);
}
} // namespace
