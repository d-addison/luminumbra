#include "../support/SimulationContentFixture.h"
#include "../support/UniqueTempPath.h"
#include "luminumbra_common/components/CreatureComponents.h"
#include "luminumbra_common/components/InstinctComponents.h"
#include "luminumbra_common/core/FilesystemPath.h"
#include "luminumbra_common/fields/EnergyFieldState.h"
#include "luminumbra_common/persistence/PlantPersistence.h"
#include "luminumbra_common/simulation/SimulationEventBus.h"
#include "luminumbra_common/world/AuthorityHashState.h"
#include "luminumbra_server/HeavyComparison.h"
#include "luminumbra_server/modes/ModeHelpers.h"
#include "luminumbra_server/modes/Modes.h"

#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <gtest/gtest.h>
#include <iterator>
#include <lz4.h>
#include <map>
#include <memory>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <utility>
#include <vector>

// Exercise the shipping mode and its actual artifact serializer.
namespace HeavyReferenceMode {
#include "../../src/luminumbra_server/modes/Heavy.cpp"
}

namespace LegacyHeavyContract {
#include "../support/LegacyHeavyContract.h"
}

namespace {
namespace W = Luminumbra::world;
namespace P = Luminumbra::Persistence;
namespace C = Luminumbra::Components;
namespace S = Luminumbra::Server;
namespace fs = std::filesystem;
using S::HeavyAuthorityReference;

using ConstWorldObservation =
    decltype(std::declval<const W::GameSession&>().ObserveWorldAuthority());
using ObservedWorld = ConstWorldObservation::value_type;
static_assert(std::is_same_v<typename decltype(ObservedWorld::live)::value_type,
                             std::shared_ptr<const Luminumbra::Chunk>>);
static_assert(std::is_same_v<typename decltype(ObservedWorld::parked)::value_type,
                             std::shared_ptr<const Luminumbra::Chunk>>);
static_assert(
    std::is_same_v<typename decltype(S::HeavyReferenceInputs::ChunkInventory::live)::value_type,
                   std::shared_ptr<const Luminumbra::Chunk>>);
static_assert(
    std::is_same_v<typename decltype(S::HeavyReferenceInputs::ChunkInventory::parked)::value_type,
                   std::shared_ptr<const Luminumbra::Chunk>>);

std::string Read(const fs::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input)
        throw std::runtime_error("Cannot read Heavy fixture");
    return {std::istreambuf_iterator<char>(input), {}};
}
void Write(const fs::path& path, std::string_view bytes) {
    fs::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    output.close();
    if (!output.good())
        throw std::runtime_error("Cannot write Heavy fixture");
}
class HeavyReference : public testing::Test {
protected:
    fs::path root;
    void SetUp() override {
        root = Luminumbra::TestSupport::CreateUniqueTempDirectory("heavy_reference_");
        Luminumbra::TestSupport::WriteSimulationContentFixture(root);
        for (const auto* species : {"grovestrider", "ridgeback_stalker"})
            Write(root / "data/common/creatures/species" / (std::string(species) + ".json"),
                  "{\"id\":\"" + std::string(species) + "\"}");
        Write(root / "data/common/systems.json",
              R"({"sim":{"active_regions":{"enabled":true},"aether_state":{"enabled":true}}})");
        Write(root / "worlds/atlas/presets/default.json", R"({
            "schema_rev":6,"generation_params":{
                "terrain":{"base_frequency":0.01,"base_amplitude":0.0,"octaves":1,
                    "persistence":0.5,"lacunarity":2.0,"height_offset":8.0,
                    "shaping":{"peaks_amplitude":0.0,"domain_warp_amplitude":0.0,
                        "continental_spline":[[-1.0,0.0],[1.0,0.0]]},
                    "hydro":{"enabled":false}},
                "features":{"caves_enabled":false,"cave_frequency":0.02}}})");
    }
    void TearDown() override {
        std::error_code error;
        fs::remove_all(root, error);
    }
    ServerCliOptions Options() const {
        ServerCliOptions options;
        options.root = Luminumbra::Filesystem::GenericPathString(root) + "/";
        options.seed = "1337";
        options.ticks = 3;
        options.heavy_resim = 2;
        options.surface_radius = 1;
        options.collision_radius = 1;
        options.test_streaming_radius_cap = 1;
        options.ecology_roster = true;
        options.planted_roster = true;
        options.artifact_path = Luminumbra::Filesystem::GenericPathString(root / "heavy.json");
        return options;
    }
};

TEST_F(HeavyReference, HeavySaveDoesNotChangeAuthority) {
    auto config = RunnerConfigFrom(Options());
    config.autosave_interval_ticks = 0;
    S::ServerWorldRunner runner(config);
    ASSERT_TRUE(runner.Boot());
    ASSERT_EQ(runner.RunFixedTicks(3).ticks_executed, 3u);
    auto* session = runner.Session();
    ASSERT_NE(session, nullptr);
    ASSERT_GT(runner.CreatureCount(), 0u);
    ASSERT_EQ(session->GetRegistry().view<const C::PlantTag>().size(), 6u);
    const auto before = HeavyReferenceMode::CaptureHashes(runner);
    ASSERT_TRUE(before.reference);
    EXPECT_EQ(before.reference->domains.size(), 11u);
    const auto repeated = HeavyReferenceMode::CaptureHashes(runner);
    EXPECT_TRUE(S::AuthoritativeStateEqual(before, repeated));
    const auto decodes = P::WorldSaveService::decode_count_for_testing();
    ASSERT_GT(runner.SaveFullSnapshot(), 0u);
    // The offline observations do not introduce work inside an ordinary save.
    EXPECT_LE(P::WorldSaveService::decode_count_for_testing() - decodes, 1u);
    const auto after = HeavyReferenceMode::CaptureHashes(runner);
    ASSERT_TRUE(after.reference);
    EXPECT_EQ(*before.reference, *after.reference);
    EXPECT_TRUE(S::AuthoritativeStateEqual(before, after));
    EXPECT_EQ(before.world_hash, after.world_hash);
    // Mutate the session's actual persisted anchor through its const observation, as a fault
    // injection only. The comparison retains every old production digest and reads the new raw
    // anchor through the shipping session overload. Restore it before shutdown.
    const auto observation = std::as_const(*session).ObserveSimulationAuthority();
    auto& anchor = const_cast<Luminumbra::Vec3&>(observation.ambient_anchor);
    const auto original_anchor = anchor;
    for (const auto axis : {0, 1, 2}) {
        if (axis == 0)
            anchor.x += 24.0f;
        else if (axis == 1)
            anchor.y += 1.0f;
        else
            anchor.z -= 24.0f;
        auto omitted_projection = after;
        omitted_projection.reference =
            HeavyAuthorityReference::Capture(std::as_const(*session), runner.Avatars());
        anchor = original_anchor;
        EXPECT_EQ(after.world_hash, omitted_projection.world_hash);
        EXPECT_EQ(after.sub.wind, omitted_projection.sub.wind);
        EXPECT_NE(after.reference->domains.at("clock"),
                  omitted_projection.reference->domains.at("clock"));
        EXPECT_FALSE(S::AuthoritativeStateEqual(after, omitted_projection));
    }
    const auto save = session->GetWorldSaveDir();
    runner.Shutdown();
    std::error_code error;
    fs::remove_all(save, error);
}

TEST_F(HeavyReference, HeavyImmediateLoadAndContinuationCompareAllDomains) {
    const auto options = Options();
    ASSERT_EQ(HeavyReferenceMode::RunHeavy(options), 0);
    const auto artifact = nlohmann::json::parse(Read(root / "heavy.json"));
    EXPECT_TRUE(artifact.at("passed").get<bool>());
    EXPECT_TRUE(artifact.at("save_unchanged").get<bool>());
    EXPECT_EQ(artifact.at("save_reference_difference"), "");
    EXPECT_TRUE(artifact.at("roundtrip_match").get<bool>());
    EXPECT_TRUE(artifact.at("resim_match").get<bool>());
    EXPECT_EQ(artifact.at("roundtrip_reference_difference"), "");
    EXPECT_EQ(artifact.at("resim_reference_difference"), "");
    for (const auto* domain : {"regions", "creatures", "items", "pages", "ecology", "plants"}) {
        SCOPED_TRACE(domain);
        const auto& saved = artifact.at("original_at_save").at("sub_hashes").at(domain);
        EXPECT_FALSE(saved.get<std::string>().empty());
        EXPECT_EQ(saved, artifact.at("loaded_at_load").at("sub_hashes").at(domain));
        EXPECT_EQ(artifact.at("original_final").at("sub_hashes").at(domain),
                  artifact.at("loaded_final").at("sub_hashes").at(domain));
    }
    EXPECT_EQ(artifact.at("original_at_save").at("world_hash"),
              artifact.at("loaded_at_load").at("world_hash"));
    EXPECT_EQ(artifact.at("original_final").at("world_hash"),
              artifact.at("loaded_final").at("world_hash"));
}

struct RawAuthority {
    entt::registry registry;
    W::WorldClock clock;
    W::ActiveRegionLedger ledger;
    W::GroundObjectStore ground;
    W::FieldPageState fields;
    luminumbra::fields::EnergyFieldState energy;
    luminumbra::simulation::OrderedEventBus events;
    std::vector<std::shared_ptr<Luminumbra::Chunk>> live;
    fs::path directory;
    Luminumbra::Vec3 ambient_anchor{};
    entt::entity creature = entt::null, plant = entt::null;
    explicit RawAuthority(bool shift_runtime_ids = false) {
        entt::entity temporary = entt::null;
        if (shift_runtime_ids)
            temporary = registry.create();
        ledger.mark_edited({0, 0}, clock);
        ledger.mark_edited({100, -100}, clock);
        std::uint64_t creature_id = 0, plant_id = 0;
        if (!ledger.allocate_persistent_id(creature_id) || !ledger.allocate_persistent_id(plant_id))
            throw std::runtime_error("Cannot allocate fixture identities");
        creature = registry.create();
        registry.emplace<C::TransformComponent>(creature).position = {8.0f, 0.0f, 8.0f};
        registry.emplace<C::CreatureComponent>(creature).species_id = 1;
        registry.emplace<C::CreatureRegionComponent>(creature).persistent_id = creature_id;
        registry.emplace<C::CreatureCoarseComponent>(creature);
        registry.emplace<C::CreatureLocomotionStateComponent>(creature);
        plant = registry.create();
        registry.emplace<C::TransformComponent>(plant).position = {8.0f, 0.0f, 8.0f};
        registry.emplace<C::PlantTag>(plant);
        registry.emplace<C::PlantRegionComponent>(plant).persistent_id = plant_id;
        registry.emplace<C::PlantGrowthComponent>(plant);
        registry.emplace<C::PlantGenomeComponent>(plant);
        events.set_durable(true);
        if (temporary != entt::null)
            registry.destroy(temporary);
    }
    S::HeavyReferenceInputs Inputs() const {
        return {.directory = directory,
                .chunks = {.live = {live.begin(), live.end()}, .parked = {}, .durable = {}},
                .registry = registry,
                .clock = clock,
                .ledger = ledger,
                .ground = ground,
                .fields = fields,
                .energy = &energy,
                .events = &events,
                .avatars = {},
                .ambient_anchor = ambient_anchor};
    }
    S::HeavyHashes Capture() const {
        const auto capture = W::CaptureAuthorityHashState({.directory = directory,
                                                           .chunks = {.live = live,
                                                                      .parked = {},
                                                                      .durable = {},
                                                                      .ground_object_bytes = {},
                                                                      .field_page_bytes = {},
                                                                      .creature_bytes = {}},
                                                           .registry = registry,
                                                           .ledger = ledger,
                                                           .ground = ground,
                                                           .fields = fields,
                                                           .energy = &energy,
                                                           .clock_bytes = clock.canonical_bytes(),
                                                           .identity_bytes = "raw_fixture",
                                                           .avatar_bytes = {},
                                                           .pending_events = &events});
        if (!capture.state)
            throw std::runtime_error(capture.error);
        S::HeavyHashes result;
        P::ComputeAuthorityWorldHashes(*capture.state, {}, result.world_hash, result.sub);
        result.active_regions = true;
        result.scent_hash = result.sub.scents;
        result.reference = HeavyAuthorityReference::Capture(Inputs());
        return result;
    }
};
// Holding every production digest equal models an omission shared with persistence. Only the
// raw input changes. The independent observation must make Heavy refuse the comparison.
void ExpectIndependentDetection(const S::HeavyHashes& before,
                                const RawAuthority& changed,
                                std::string_view domain) {
    ASSERT_TRUE(before.reference);
    ASSERT_TRUE(S::AuthoritativeStateEqual(before, before));
    auto omitted_projection = before;
    omitted_projection.reference = HeavyAuthorityReference::Capture(changed.Inputs());
    ASSERT_TRUE(omitted_projection.reference);
    EXPECT_EQ(before.world_hash, omitted_projection.world_hash);
    EXPECT_EQ(before.sub.creatures, omitted_projection.sub.creatures);
    EXPECT_NE(before.reference->domains.at(std::string(domain)),
              omitted_projection.reference->domains.at(std::string(domain)));
    EXPECT_FALSE(S::AuthoritativeStateEqual(before, omitted_projection));
    EXPECT_FALSE(S::AuthoritativeStateEqual(omitted_projection, before));
}

TEST(HeavyReference, ReferenceDetectsProductionProjectionOmissions) {
    struct Mutation {
        const char* domain;
        std::function<void(RawAuthority&)> apply;
    };
    const std::array<Mutation, 10> mutations{{
        {"creatures",
         [](RawAuthority& raw) {
             raw.registry.get<C::CreatureComponent>(raw.creature).energy = 0.25f;
         }},
        {"creatures",
         [](RawAuthority& raw) {
             raw.registry.get<C::CreatureCoarseComponent>(raw.creature)
                 .reproduce_cooldown_cadences = 17;
         }},
        {"creatures",
         [](RawAuthority& raw) {
             raw.registry.get<C::TransformComponent>(raw.creature).rotation.x = -0.0f;
         }},
        {"plants",
         [](RawAuthority& raw) {
             raw.registry.get<C::PlantGrowthComponent>(raw.plant).stress_points = 7;
         }},
        {"plants",
         [](RawAuthority& raw) {
             raw.registry.get<C::PlantGenomeComponent>(raw.plant).genes[0] = 0.125f;
         }},
        {"regions",
         [](RawAuthority& raw) {
             raw.ledger.publish_completed_work(
                 std::array<W::RegionWork, 1>{{{{0, 0}, 0, 3, 0, 0}}});
         }},
        {"pending",
         [](RawAuthority& raw) {
             raw.events.publish(100, "future", "payload", -1);
         }},
        {"items",
         [](RawAuthority& raw) {
             std::uint64_t id = 0;
             if (!raw.ledger.allocate_persistent_id(id))
                 throw std::runtime_error("Fixture allocator");
             W::GroundObject object;
             object.id = id;
             object.type = 1;
             object.x_mm = 8000;
             object.z_mm = 8000;
             if (!raw.ground.insert(object))
                 throw std::runtime_error("Fixture item admission");
         }},
        {"regions",
         [](RawAuthority& raw) {
             if (!raw.ledger.set_water_debts({{{0, 0}, 1}}))
                 throw std::runtime_error("Fixture debt");
         }},
        {"pages",
         [](RawAuthority& raw) {
             if (!raw.fields.set_weather_age({100, -100}, 7))
                 throw std::runtime_error("Fixture weather");
         }},
    }};
    for (const auto& mutation : mutations) {
        SCOPED_TRACE(mutation.domain);
        RawAuthority raw;
        const auto before = raw.Capture();
        mutation.apply(raw);
        ExpectIndependentDetection(before, raw, mutation.domain);
        const auto production = raw.Capture();
        EXPECT_NE(before.world_hash, production.world_hash);
    }
    // The persisted session anchor is distinct from the ledger anchor. Retain every old
    // production digest to model a serializer and hash both forgetting this raw session field.
    for (const auto axis : {0, 1, 2}) {
        RawAuthority raw;
        const auto before = raw.Capture();
        if (axis == 0)
            raw.ambient_anchor.x = 24.0f;
        else if (axis == 1)
            raw.ambient_anchor.y = 1.0f;
        else
            raw.ambient_anchor.z = -24.0f;
        ExpectIndependentDetection(before, raw, "clock");
    }
    // Recreating the registry in another allocation order preserves persistent references.
    RawAuthority first;
    RawAuthority second(true);
    ASSERT_NE(first.creature, second.creature);
    EXPECT_EQ(first.Capture().reference, second.Capture().reference);
}

TEST(HeavyReference, ReferenceDetectsOptionalPresenceTravelAndControllerDifferences) {
    struct Mutation {
        std::function<void(RawAuthority&)> apply;
    };
    const std::array<Mutation, 6> mutations{{
        {[](RawAuthority& raw) {
            raw.registry.emplace<C::ThirstComponent>(raw.creature);
        }},
        {[](RawAuthority& raw) {
            auto& coarse = raw.registry.get<C::CreatureCoarseComponent>(raw.creature);
            coarse.travel_pending = true;
            coarse.destination_x = 100;
            coarse.destination_z = -100;
        }},
        {[](RawAuthority& raw) {
            raw.registry.emplace<C::CreatureControllerComponent>(raw.creature, true);
        }},
        {[](RawAuthority& raw) {
            raw.registry.get<C::CreatureLocomotionStateComponent>(raw.creature).grounded = true;
        }},
        {[](RawAuthority& raw) {
            raw.registry.get<C::CreatureLocomotionStateComponent>(raw.creature).velocity_x = -0.0f;
        }},
        {[](RawAuthority& raw) {
            raw.registry.emplace<C::SensesComponent>(raw.creature).nearest_food_source = raw.plant;
        }},
    }};
    for (const auto& mutation : mutations) {
        RawAuthority raw;
        const auto before = raw.Capture();
        mutation.apply(raw);
        ExpectIndependentDetection(before, raw, "creatures");
        EXPECT_NE(before.world_hash, raw.Capture().world_hash);
    }
    RawAuthority raw;
    raw.registry.emplace<C::SensesComponent>(raw.creature).nearest_food_source = raw.plant;
    const auto before = raw.Capture();
    // Changing a target identity is visible; runtime entity slots are never reference identity.
    raw.registry.get<C::SensesComponent>(raw.creature).nearest_food_source = raw.creature;
    ExpectIndependentDetection(before, raw, "creatures");
    const auto dangling = raw.registry.create();
    raw.registry.get<C::SensesComponent>(raw.creature).nearest_food_source = dangling;
    EXPECT_THROW(HeavyAuthorityReference::Capture(raw.Inputs()), std::runtime_error);
    EXPECT_EQ(raw.registry.get<C::SensesComponent>(raw.creature).nearest_food_source, dangling);
    raw.registry.get<C::SensesComponent>(raw.creature).nearest_food_source = raw.creature;
    EXPECT_EQ(before.reference->domains.at("regions"),
              HeavyAuthorityReference::Capture(raw.Inputs()).domains.at("regions"));
}

TEST_F(HeavyReference, HeavyIncludesDiskOnlyAndDormantAuthority) {
    RawAuthority raw;
    raw.directory = root / "disk";
    auto disk = std::make_shared<Luminumbra::Chunk>(Luminumbra::IVec3{3200, 0, -3200});
    disk->set_state(Luminumbra::ChunkState::Idle);
    disk->heightmap_data = {3.0f};
    disk->mark_voxel_data_dirty();
    const auto save = [&raw, &disk]() {
        Luminumbra::WorldStreamingState state;
        state.insert_chunk(disk);
        std::vector<std::string> errors;
        P::SaveTransaction transaction(raw.directory, 0, {}, &P::WorldSaveService::member_digest);
        return transaction.begin(&errors) &&
               P::WorldSaveService{}.save_world(state, raw.directory, &errors) &&
               transaction.commit(&errors);
    };
    ASSERT_TRUE(save());
    const auto before = raw.Capture();
    disk->heightmap_data[0] = 9.0f;
    disk->mark_voxel_data_dirty();
    ASSERT_TRUE(save());
    ASSERT_TRUE(raw.live.empty());
    ExpectIndependentDetection(before, raw, "chunks");
    const auto stored = raw.Capture();
    // A Loading or pristine resident must not hide the durable record.
    auto shadow = std::make_shared<Luminumbra::Chunk>(disk->get_coords());
    shadow->heightmap_data = {99.0f};
    shadow->set_state(Luminumbra::ChunkState::Loading);
    raw.live = {shadow};
    EXPECT_EQ(stored.reference, raw.Capture().reference);
    raw.live.clear();
    ASSERT_TRUE(raw.fields.activate_region({100, -100}, 1337, 0));
    ExpectIndependentDetection(stored, raw, "pages");
    auto& records =
        const_cast<std::map<W::RegionKey, W::ActiveRegionRecord>&>(raw.ledger.records());
    ASSERT_TRUE(raw.energy.QueueRegionalDeposit(7, 2134, -2133, 0, 1000));
    luminumbra::fields::EnergyFieldState::RegionalResult energy_result;
    const std::array<W::RegionKey, 1> due{{{100, -100}}};
    ASSERT_TRUE(raw.energy.TickRegional(1, raw.ledger.records(), due, {}, energy_result));
    ASSERT_TRUE(raw.ledger.set_energy_ages(energy_result.ages));
    ASSERT_GT(raw.energy.total_raw(), 0u);
    records.at({100, -100}).state = W::RegionState::Frozen;
    const auto dormant = raw.Capture();
    ASSERT_TRUE(raw.fields.set_soil({51208, -51192}, 1234));
    ExpectIndependentDetection(dormant, raw, "pages");
    const auto soil = raw.Capture();
    // Move the near window elsewhere: stored dormant values still participate.
    raw.energy.SetAnchorCell(0, 0);
    EXPECT_EQ(soil.reference, raw.Capture().reference);
    luminumbra::fields::EnergyFieldState changed_energy;
    ASSERT_TRUE(changed_energy.QueueRegionalDeposit(7, 2134, -2133, 0, 2000));
    records.at({100, -100}).state = W::RegionState::Active;
    ASSERT_TRUE(changed_energy.TickRegional(1, raw.ledger.records(), due, {}, energy_result));
    records.at({100, -100}).state = W::RegionState::Frozen;
    raw.energy = std::move(changed_energy);
    ExpectIndependentDetection(soil, raw, "energy");
    const auto energy_before_pending = raw.Capture();
    ASSERT_TRUE(raw.energy.QueueRegionalDeposit(7, 2133, -2133, 0, 90));
    ExpectIndependentDetection(energy_before_pending, raw, "energy");
    const auto region_file = P::WorldSaveService::region_file_path(raw.directory, 100, -100);
    const auto committed_bytes = Read(region_file);
    const auto intact = HeavyAuthorityReference::Capture(raw.Inputs());
    Write(region_file, "0");
    EXPECT_THROW(HeavyAuthorityReference::Capture(raw.Inputs()), std::runtime_error);
    EXPECT_EQ(Read(region_file), "0");
    Write(region_file, committed_bytes);
    EXPECT_EQ(intact, HeavyAuthorityReference::Capture(raw.Inputs()));
    auto inputs = raw.Inputs();
    inputs.chunks.durable = {{3201, 0, -3200}};
    EXPECT_THROW(HeavyAuthorityReference::Capture(inputs), std::runtime_error);
    ASSERT_TRUE(fs::remove(P::WorldSaveService::region_file_path(raw.directory, 100, -100)));
    EXPECT_THROW(HeavyAuthorityReference::Capture(inputs), std::runtime_error);
}

// Independent durable fixture: no production snapshot writer, reader or member validator.
void AppendLittle(std::string& bytes, std::uint64_t value, unsigned width) {
    for (unsigned index = 0; index < width; ++index)
        bytes.push_back(static_cast<char>((value >> (index * 8)) & 255u));
}
std::uint64_t FixtureFnv(std::string_view bytes) {
    std::uint64_t hash = 14695981039346656037ull;
    for (const char byte : bytes) {
        hash ^= static_cast<unsigned char>(byte);
        hash *= 1099511628211ull;
    }
    return hash;
}
std::string FixtureHex(std::uint64_t value) {
    constexpr std::string_view digits = "0123456789abcdef";
    std::string result(16, '0');
    for (std::size_t index = 0; index < result.size(); ++index) {
        result[15 - index] = digits[static_cast<std::size_t>(value & 15u)];
        value >>= 4;
    }
    return result;
}
void WriteRawReferenceRecord(const fs::path& directory,
                             std::string_view payload,
                             Luminumbra::ChunkID id = Luminumbra::Chunk::calculate_id({0, 0, 0})) {
    if (payload.size() > S::HeavyReferenceDecodeLimits::kRecordBytes)
        throw std::runtime_error("Fixture payload too large");
    std::string compressed(
        static_cast<std::size_t>(LZ4_compressBound(static_cast<int>(payload.size()))), '\0');
    const int written = LZ4_compress_default(payload.data(),
                                             compressed.data(),
                                             static_cast<int>(payload.size()),
                                             static_cast<int>(compressed.size()));
    if (written <= 0)
        throw std::runtime_error("Fixture compression failed");
    compressed.resize(static_cast<std::size_t>(written));
    std::string record;
    AppendLittle(record, id, 8);
    AppendLittle(record, 1, 1); // edited authority
    AppendLittle(record, payload.size(), 4);
    AppendLittle(record, compressed.size(), 4);
    record += compressed;
    std::string container = "LMR1";
    AppendLittle(container, 2, 2);
    AppendLittle(container, 1, 2);
    AppendLittle(container, id, 8);
    AppendLittle(container, 0, 1); // lod-0
    container.append(record, 8, 9);
    container += compressed;
    Write(directory / "chunks/region/r.0.0.lmr", container);
    const nlohmann::json index{{"schema", "luminumbra.snapshot.v1"},
                               {"generation", 1u},
                               {"tick", 0u},
                               {"txn_id", 1u},
                               {"anchors", nlohmann::json::array()},
                               {"members",
                                nlohmann::json::array({{{"path", "chunks/region/r.0.0.lmr"},
                                                        {"class", "authoritative"},
                                                        {"state", "present"},
                                                        {"bytes", record.size()},
                                                        {"fnv64", FixtureHex(FixtureFnv(record))},
                                                        {"generation", 1u}}})}};
    Write(directory / "chunks/region/snapshot.json", index.dump());
}

nlohmann::json RawReferenceChunkRecord(Luminumbra::ChunkID id) {
    return nlohmann::json{{"schema", "luminumbra.persistence.world_state_snapshot.v1"},
                          {"order_contract", "chunk_id_ascending"},
                          {"chunk_count", 1u},
                          {"chunks",
                           nlohmann::json::array({{{"coords", {{"x", 0}, {"y", 0}, {"z", 0}}},
                                                   {"chunk_id", id},
                                                   {"sdf_data", nlohmann::json::array()},
                                                   {"heightmap_data", {3.0f}},
                                                   {"material_data", nlohmann::json::array()},
                                                   {"water_depth_mm", nlohmann::json::array()},
                                                   {"water_bed_mm", nlohmann::json::array()},
                                                   {"water_edge_flux", nlohmann::json::array()},
                                                   {"water_state",
                                                    {{"has_water_sim", false},
                                                     {"current_water_resolution", 8},
                                                     {"is_water_sleeping", false},
                                                     {"ticks_below_threshold", 0},
                                                     {"max_water_delta_last_tick", 0.0f}}}}})}};
}

TEST_F(HeavyReference, ReferenceDurableDecodeIsIndependentOfProductionSerializers) {
    RawAuthority raw;
    auto chunk = std::make_shared<Luminumbra::Chunk>(Luminumbra::IVec3{0, 0, 0});
    chunk->set_state(Luminumbra::ChunkState::Idle);
    chunk->heightmap_data = {3.0f};
    chunk->mark_voxel_data_dirty();
    raw.live = {chunk};
    const auto before = raw.Capture();
    // This authority record deliberately has no render fields. A projection-dependent loader
    // would reject it, hiding which raw authority the reference actually observes.
    auto record = RawReferenceChunkRecord(chunk->get_id());
    raw.live.clear();
    raw.directory = root / "independent";
    WriteRawReferenceRecord(raw.directory, record.dump());
    EXPECT_EQ(before.reference, HeavyAuthorityReference::Capture(raw.Inputs()));
    record["chunks"][0]["heightmap_data"][0] = 9.0f;
    WriteRawReferenceRecord(raw.directory, record.dump());
    ExpectIndependentDetection(before, raw, "chunks");
    // A serializer dropping a required authoritative field must cause refusal, not normalize
    // the omission by round-tripping through that same serializer.
    record["chunks"][0].erase("water_edge_flux");
    WriteRawReferenceRecord(raw.directory, record.dump());
    EXPECT_THROW(HeavyAuthorityReference::Capture(raw.Inputs()), std::runtime_error);
}

TEST_F(HeavyReference, ReferenceRefusesDecodeLimitsBeforeDomOrFieldAllocation) {
    RawAuthority raw;
    raw.directory = root / "bounded";
    const auto before = raw.registry.get<C::CreatureComponent>(raw.creature).energy;
    const auto expect_refusal = [&raw](std::string_view expected_message) {
        try {
            (void)HeavyAuthorityReference::Capture(raw.Inputs());
            FAIL() << "Heavy accepted unbounded durable authority";
        } catch (const std::runtime_error& error) {
            EXPECT_EQ(error.what(), expected_message);
        }
    };
    const auto index_path = raw.directory / "chunks/region/snapshot.json";
    Write(index_path, "0");
    fs::resize_file(index_path, S::HeavyReferenceDecodeLimits::kIndexBytes + 1u);
    expect_refusal("Heavy file byte limit"); // file_size gate precedes allocating a read buffer
    fs::remove(index_path);
    std::string nested(S::HeavyReferenceDecodeLimits::kJsonDepth + 1u, '[');
    nested += '0';
    nested.append(S::HeavyReferenceDecodeLimits::kJsonDepth + 1u, ']');
    WriteRawReferenceRecord(raw.directory, nested);
    expect_refusal("Heavy JSON nesting limit"); // lexical pass precedes the DOM parser
    std::string collection = "[0";
    for (std::size_t index = 1; index <= S::HeavyReferenceDecodeLimits::kCollection; ++index)
        collection += ",0";
    collection += ']';
    WriteRawReferenceRecord(raw.directory, collection);
    expect_refusal("Heavy JSON collection limit");
    std::string many_nodes = "[";
    for (std::size_t index = 0; index < S::HeavyReferenceDecodeLimits::kJsonNodes / 4u; ++index) {
        if (index != 0)
            many_nodes += ',';
        many_nodes += "[0,0,0]";
    }
    many_nodes += ']';
    WriteRawReferenceRecord(raw.directory, many_nodes);
    expect_refusal("Heavy JSON node limit");
    // A hostile declared raw size is rejected from the manifest, before decompression storage.
    WriteRawReferenceRecord(raw.directory, "0");
    auto container = Read(raw.directory / "chunks/region/r.0.0.lmr");
    std::string oversized;
    AppendLittle(oversized, S::HeavyReferenceDecodeLimits::kRecordBytes + 1u, 4);
    container.replace(18, 4, oversized);
    Write(raw.directory / "chunks/region/r.0.0.lmr", container);
    expect_refusal("Heavy invalid LMR record bounds");
    EXPECT_EQ(Read(raw.directory / "chunks/region/r.0.0.lmr"), container);
    EXPECT_EQ(raw.registry.get<C::CreatureComponent>(raw.creature).energy, before);
    // A one-byte scalar is rejected without indexing past its end.
    WriteRawReferenceRecord(raw.directory, "0");
    EXPECT_THROW(HeavyAuthorityReference::Capture(raw.Inputs()), std::runtime_error);
    // A pending transaction is never recovered or modified by an observation.
    fs::create_directory(raw.directory / "journal");
    expect_refusal("Heavy requires committed authority");
    EXPECT_TRUE(fs::exists(raw.directory / "journal"));
}

TEST_F(HeavyReference, ReferenceRefusesMalformedCommittedInventoryWithoutMutation) {
    RawAuthority raw;
    raw.directory = root / "malformed";
    const auto id = Luminumbra::Chunk::calculate_id({0, 0, 0});
    const auto valid = RawReferenceChunkRecord(id);
    WriteRawReferenceRecord(raw.directory, valid.dump());
    const auto baseline = HeavyAuthorityReference::Capture(raw.Inputs());
    const auto original_energy = raw.registry.get<C::CreatureComponent>(raw.creature).energy;
    const std::array<std::function<void(nlohmann::json&)>, 6> mutations{{
        [](auto& record) { record["chunks"][0]["coords"]["x"] = 2147483648ull; },
        [](auto& record) { record["chunks"][0]["chunk_id"] = 1u; },
        [](auto& record) { record["chunks"][0]["water_state"]["current_water_resolution"] = 3; },
        [](auto& record) { record["chunks"][0]["water_state"]["has_water_sim"] = true; },
        [](auto& record) { record["chunks"][0]["water_depth_mm"] = {-1}; },
        [](auto& record) { record["chunks"][0]["material_data"] = {256}; },
    }};
    for (const auto& mutate : mutations) {
        auto record = valid;
        mutate(record);
        WriteRawReferenceRecord(raw.directory, record.dump());
        EXPECT_THROW(HeavyAuthorityReference::Capture(raw.Inputs()), std::runtime_error);
        EXPECT_EQ(raw.registry.get<C::CreatureComponent>(raw.creature).energy, original_energy);
    }
    for (const auto payload : {R"({"a":0,"a":1})", "[0}", "[0", "0"}) {
        WriteRawReferenceRecord(raw.directory, payload);
        EXPECT_THROW(HeavyAuthorityReference::Capture(raw.Inputs()), std::runtime_error);
    }
    const auto index_path = raw.directory / "chunks/region/snapshot.json";
    const std::array<std::function<void(nlohmann::json&)>, 4> index_mutations{{
        [](auto& index) { index["members"][0]["path"] = "chunks/region/../escape"; },
        [](auto& index) { index["members"][0]["fnv64"] = "0000000000000000"; },
        [](auto& index) { index["members"] = nlohmann::json::array(); },
        [](auto& index) { index["members"].push_back(index["members"][0]); },
    }};
    for (const auto& mutate : index_mutations) {
        WriteRawReferenceRecord(raw.directory, valid.dump());
        auto index = nlohmann::json::parse(Read(index_path));
        mutate(index);
        const auto corrupt = index.dump();
        Write(index_path, corrupt);
        EXPECT_THROW(HeavyAuthorityReference::Capture(raw.Inputs()), std::runtime_error);
        EXPECT_EQ(Read(index_path), corrupt);
    }
    WriteRawReferenceRecord(raw.directory, valid.dump());
    EXPECT_EQ(baseline, HeavyAuthorityReference::Capture(raw.Inputs()));
    // Full legal Y range and every supported water resolution remain admissible.
    for (const int coordinate_y : {-2097152, 2097151}) {
        for (const int resolution : {4, 8, 16, 32}) {
            auto boundary = valid;
            const auto boundary_id = Luminumbra::Chunk::calculate_id({0, coordinate_y, 0});
            boundary["chunks"][0]["coords"]["y"] = coordinate_y;
            boundary["chunks"][0]["chunk_id"] = boundary_id;
            auto& water = boundary["chunks"][0];
            water["water_state"]["has_water_sim"] = true;
            water["water_state"]["current_water_resolution"] = resolution;
            const auto cells =
                static_cast<std::size_t>(resolution) * static_cast<std::size_t>(resolution);
            water["water_depth_mm"] = std::vector<std::int32_t>(cells, 0);
            water["water_bed_mm"] = std::vector<std::int32_t>(cells, 0);
            water["water_edge_flux"] = std::vector<std::int32_t>(2 * cells, 0);
            WriteRawReferenceRecord(raw.directory, boundary.dump(), boundary_id);
            EXPECT_NO_THROW((void)HeavyAuthorityReference::Capture(raw.Inputs()));
        }
    }
}

TEST_F(HeavyReference, LegacyHeavyOutputUnchanged) {
    Write(root / "data/common/systems.json", R"({"sim":{"active_regions":{"enabled":false}}})");
    auto options = Options();
    // No participant opt-ins: this fixture exercises the default shipping flag-off boot.
    options.ecology_roster = false;
    options.planted_roster = false;
    options.artifact_path = Luminumbra::Filesystem::GenericPathString(root / "legacy.json");
    const int shipping_result = LegacyHeavyContract::RunHeavy(options);
    const auto shipping_bytes = Read(root / "legacy.json");
    const auto shipping = nlohmann::json::parse(shipping_bytes);
    ASSERT_EQ(shipping_result, 0);
    ASSERT_TRUE(shipping.at("passed").get<bool>());
    ASSERT_TRUE(shipping.at("roundtrip_match").get<bool>());
    ASSERT_TRUE(shipping.at("resim_match").get<bool>());
    ASSERT_TRUE(shipping.at("settle_contract_ok").get<bool>());
    ASSERT_FALSE(shipping.at("boot_settle_original").at("water_settle_skipped").get<bool>());
    ASSERT_TRUE(shipping.at("boot_settle_loaded").at("water_settle_skipped").get<bool>());
    ASSERT_EQ(shipping.at("boot_settle_original").at("uninited"), 0);
    ASSERT_EQ(shipping.at("boot_settle_loaded").at("uninited"), 0);
    ASSERT_EQ(shipping.at("water_sim_cursor_at_save"), shipping.at("water_sim_cursor_at_load"));
    options.artifact_path = Luminumbra::Filesystem::GenericPathString(root / "heavy.json");
    EXPECT_EQ(HeavyReferenceMode::RunHeavy(options), shipping_result);
    const auto actual_bytes = Read(root / "heavy.json");
    auto actual = nlohmann::json::parse(actual_bytes);
    ASSERT_FALSE(actual.at("world_id").get<std::string>().empty());
    // A fresh throwaway world's allocated label is the sole nondeterministic artifact value.
    // Substitute that label only; compare every other byte, including all outer keys/verdicts,
    // nested hash values, boot statistics, output formatting and the terminating newline.
    const auto actual_id = actual.at("world_id").get<std::string>();
    const auto expected_id = shipping.at("world_id").get<std::string>();
    std::string normalized = actual_bytes;
    const auto label = std::string{R"("world_id": ")"};
    const auto label_start = normalized.find(label);
    ASSERT_NE(label_start, std::string::npos);
    normalized.replace(label_start + label.size(), actual_id.size(), expected_id);
    EXPECT_EQ(normalized, shipping_bytes);

    S::HeavyHashes hashes;
    hashes.world_hash = "world";
    hashes.sub.terrain = "terrain";
    hashes.sub.mesh = "mesh";
    hashes.sub.water = "water";
    hashes.sub.entities = "entities";
    hashes.sub.wind = "wind";
    hashes.sub.weather = "weather";
    hashes.sub.aether = "aether";
    hashes.sub.aether_state = "state";
    hashes.scent_hash = "scent";
    const std::string expected =
        R"({"ground_objects":[],"sub_hashes":{"aether":"aether","aether_state":"state","entities":"entities","mesh":"mesh","scents":"scent","terrain":"terrain","water":"water","weather":"weather","wind":"wind"},"world_hash":"world"})";
    EXPECT_EQ(HeavyReferenceMode::HeavyHashJson(hashes).dump(), expected);
    hashes.sub.regions = "stale";
    hashes.sub.creatures = "stale";
    hashes.sub.items = "stale";
    hashes.sub.pages = "stale";
    hashes.reference.emplace();
    EXPECT_EQ(HeavyReferenceMode::HeavyHashJson(hashes).dump(), expected);
    auto changed = hashes;
    changed.world_hash = "historically excluded";
    changed.sub.mesh = "derived mesh";
    EXPECT_TRUE(S::AuthoritativeStateEqual(hashes, changed));
}
} // namespace
