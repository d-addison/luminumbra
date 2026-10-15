#include "../fixtures/smoke_artifact/FixedInputs.h"
#include "../support/SimulationContentFixture.h"
#include "../support/UniqueTempPath.h"
#include "luminumbra_common/components/CreatureComponents.h"
#include "luminumbra_common/components/InstinctComponents.h"
#include "luminumbra_common/core/FilesystemPath.h"
#include "luminumbra_common/fields/EnergyFieldState.h"
#include "luminumbra_common/persistence/PlantPersistence.h"
#include "luminumbra_common/simulation/SimulationEventBus.h"
#include "luminumbra_common/systems/WaterSystem.h"
#include "luminumbra_common/world/AuthorityHashState.h"
#include "luminumbra_server/HeavyComparison.h"
#include "luminumbra_server/SimBudgetArtifact.h"
#include "luminumbra_server/modes/ModeHelpers.h"
#include "luminumbra_server/modes/Modes.h"
#include "luminumbra_server/modes/SmokeArtifact.h"
#include <limits>

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

namespace {
TEST_F(HeavyReference, HeavyContinuationRegionalCapture) {
    auto options = Options();
    options.sim_budget = true;
    ASSERT_EQ(HeavyReferenceMode::RunHeavy(options), 0);
    const auto artifact = nlohmann::json::parse(Read(root / "heavy.json"));
    ASSERT_TRUE(artifact.at("passed").get<bool>());
    const auto& budget = artifact.at("sim_budget");
    EXPECT_EQ(budget.at("schema"), "luminumbra.sim_budget.v2");
    EXPECT_TRUE(budget.at("work_replay_match").get<bool>());
    const auto& regional = budget.at("regions");
    EXPECT_EQ(regional.at("schema"), "luminumbra.sim_budget.regions.v1");
    EXPECT_EQ(regional.at("samples"), options.heavy_resim);
    EXPECT_EQ(regional.at("first_tick"), options.ticks + 1);
    EXPECT_EQ(regional.at("last_tick"), options.ticks + options.heavy_resim);
    EXPECT_TRUE(regional.at("complete").get<bool>());
    EXPECT_TRUE(regional.at("replay").at("work_replay_match").get<bool>());
    EXPECT_EQ(regional.at("dropped_samples"), 0);
}
} // namespace

namespace {
using RegionCollector = luminumbra::simulation::RegionTelemetry;
namespace RegionalServer = Luminumbra::Server;
namespace RegionalWorld = Luminumbra::world;

void RecordRegionSample(RegionCollector& collector, std::uint64_t tick, double duration) {
    const RegionCollector::SchedulerObservation observation{1, 1, 0, 0, 5};
    collector.RecordScheduler(tick, duration, observation, 1, 5, 10, 0, 0, 0, "0123456789abcdef");
}
std::string ReadRegionFixture(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input)
        throw std::runtime_error("Cannot read telemetry fixture");
    return {std::istreambuf_iterator<char>(input), {}};
}
std::string NativeRegionFixture(std::string bytes) {
#ifdef _WIN32
    std::string native;
    for (const char value : bytes) {
        if (value == '\n')
            native += '\r';
        native += value;
    }
    return native;
#else
    return bytes;
#endif
}

TEST(RegionTelemetry, RegionsBlockRequiresBothFlags) {
    for (const bool regions_enabled : {false, true}) {
        for (const bool budget_enabled : {false, true}) {
            SCOPED_TRACE(regions_enabled);
            SCOPED_TRACE(budget_enabled);
            RegionalWorld::GameSession session;
            session.SetSimBudgetTelemetryEnabled(budget_enabled);
            session.SetActiveRegionsEnabled(regions_enabled);
            const auto capture = session.CaptureSimBudgetTelemetry();
            EXPECT_EQ(capture.Regions().Enabled(), regions_enabled && budget_enabled);
            EXPECT_EQ(session.GetSimBudgetTelemetry().Regions().RetainedSampleBytes() != 0,
                      regions_enabled && budget_enabled);
            EXPECT_EQ(capture.Regions().RetainedSampleBytes(), 0u);
            nlohmann::json artifact = {{"legacy", 1}};
            const auto before = artifact.dump(2);
            RegionalServer::AppendSimBudgetArtifact(artifact, capture, capture);
            if (!budget_enabled) {
                EXPECT_EQ(artifact.dump(2), before);
            } else {
                ASSERT_TRUE(artifact.contains("sim_budget"));
                EXPECT_EQ(artifact["sim_budget"].contains("regions"), regions_enabled);
                EXPECT_EQ(artifact["sim_budget"]["stages"].size(),
                          luminumbra::simulation::kSimBudgetStageNames.size());
            }
            session.SetSimBudgetTelemetryEnabled(false);
            EXPECT_FALSE(session.GetSimBudgetTelemetry().Regions().Enabled());
            EXPECT_EQ(session.GetSimBudgetTelemetry().Regions().RetainedSampleBytes(), 0u);
        }
    }
}

TEST(RegionTelemetry, RegionPercentilesAndEmptyCapture) {
    RegionCollector collector;
    collector.SetEnabled(true);
    EXPECT_EQ(collector.Samples(), 0u);
    EXPECT_FALSE(collector.FirstTick());
    EXPECT_FALSE(collector.LastTick());
    EXPECT_FALSE(collector.SchedulerDuration());
    EXPECT_FALSE(collector.SaveDuration());
    const auto empty = RegionalServer::RegionTelemetryJson(collector, collector);
    EXPECT_TRUE(empty.at("duration_ms").is_null());
    EXPECT_TRUE(empty.at("first_tick").is_null());
    EXPECT_TRUE(empty.at("last_tick").is_null());
    EXPECT_TRUE(empty.at("water").at("bound").is_null());
    EXPECT_FALSE(empty.at("water").at("bound_known").get<bool>());
    EXPECT_TRUE(empty.at("replay").at("work_replay_match").get<bool>());
    RecordRegionSample(collector, 7, 30.0);
    const auto single = collector.SchedulerDuration();
    ASSERT_TRUE(single);
    EXPECT_DOUBLE_EQ(single->p99, 30.0);
    RecordRegionSample(collector, 8, 0.0);
    RecordRegionSample(collector, 9, 20.0);
    RecordRegionSample(collector, 10, 10.0);
    const auto distribution = collector.SchedulerDuration();
    ASSERT_TRUE(distribution);
    EXPECT_DOUBLE_EQ(distribution->p50, 15.0);
    EXPECT_DOUBLE_EQ(distribution->p95, 28.5);
    EXPECT_DOUBLE_EQ(distribution->p99, 29.7);
    EXPECT_DOUBLE_EQ(distribution->maximum, 30.0);
    EXPECT_EQ(collector.FirstTick(), 7u);
    EXPECT_EQ(collector.LastTick(), 10u);
    collector.RecordSave(true, 10.0);
    collector.RecordSave(false, 30.0);
    const auto save_distribution = collector.SaveDuration();
    ASSERT_TRUE(save_distribution);
    EXPECT_DOUBLE_EQ(save_distribution->p50, 20.0);
    EXPECT_DOUBLE_EQ(save_distribution->p99, 29.8);
    EXPECT_EQ(collector.saves_total, 2u);
    EXPECT_EQ(collector.save_failures_total, 1u);
    collector.Clear();
    EXPECT_TRUE(collector.Enabled());
    EXPECT_EQ(collector.Samples(), 0u);
    EXPECT_FALSE(collector.SchedulerDuration());
    EXPECT_EQ(collector.saves_total, 0u);
}

TEST(RegionTelemetry, RegionCollectorAndArtifactRespectCaps) {
    RegionCollector collector;
    EXPECT_EQ(collector.RetainedSampleBytes(), 0u);
    collector.SetEnabled(true);
    const auto retained_bytes = collector.RetainedSampleBytes();
    EXPECT_EQ(retained_bytes,
              (RegionCollector::kMaxSamples + RegionCollector::kMaxSaveSamples) * sizeof(double));
    EXPECT_EQ(retained_bytes, RegionCollector::kSampleStorageBytes);
    EXPECT_LT(RegionCollector::kHeavySampleStorageBytes, 2u * 1024u * 1024u);
    for (std::size_t index = 0; index < RegionCollector::kMaxSamples; ++index)
        RecordRegionSample(collector, static_cast<std::uint64_t>(index) + 1, 1.0);
    for (std::size_t index = 0; index < RegionCollector::kMaxSaveSamples; ++index)
        collector.RecordSave(true, 2.0);
    EXPECT_EQ(collector.Samples(), RegionCollector::kMaxSamples);
    EXPECT_EQ(collector.SaveSamples(), RegionCollector::kMaxSaveSamples);
    EXPECT_TRUE(collector.Complete());
    const auto duration = collector.SchedulerDuration();
    ASSERT_TRUE(duration);
    EXPECT_DOUBLE_EQ(duration->p99, 1.0);
    RecordRegionSample(collector, RegionCollector::kMaxSamples + 1, 3.0);
    collector.RecordSave(false, 4.0);
    EXPECT_FALSE(collector.Complete());
    EXPECT_EQ(collector.DroppedSamples(), 2u);
    EXPECT_EQ(collector.Samples(), RegionCollector::kMaxSamples);
    EXPECT_EQ(collector.SaveSamples(), RegionCollector::kMaxSaveSamples);
    EXPECT_EQ(collector.RetainedSampleBytes(), retained_bytes);
    collector.RecordFault(42, std::string(1000, '"'));
    collector.RecordFault(43, "second fault");
    EXPECT_EQ(collector.FaultTick(), 42u);
    EXPECT_EQ(collector.FaultMessage().size(), RegionCollector::kMaxFaultBytes);
    const auto maximum = std::numeric_limits<std::uint64_t>::max();
    RegionCollector::Activities large;
    large.fill(maximum);
    collector.ObserveActivity(large);
    collector.ObserveGauges(maximum, maximum, maximum, maximum, maximum);
    collector.RecordWater(maximum, maximum, 0, maximum, maximum);
    const auto block = RegionalServer::RegionTelemetryJson(collector, collector);
    EXPECT_LE(block.dump(8).size(), RegionCollector::kMaxBlockBytes);
    EXPECT_FALSE(block.at("complete").get<bool>());
    EXPECT_FALSE(block.at("replay").at("work_replay_match").get<bool>());
    for (const auto& value : block.items())
        EXPECT_FALSE(value.value().is_array());
    collector.SetEnabled(false);
    EXPECT_EQ(collector.RetainedSampleBytes(), 0u);
    RecordRegionSample(collector, 1, 1.0);
    EXPECT_EQ(collector.Samples(), 0u);
}

TEST(RegionTelemetry, CaptureSummariesOwnNoSampleArrays) {
    RegionCollector collector;
    collector.SetEnabled(true);
    RecordRegionSample(collector, 1, 30.0);
    collector.RecordSave(true, 10.0);
    auto captured = collector.Capture();
    EXPECT_TRUE(captured.Enabled());
    EXPECT_EQ(captured.RetainedSampleBytes(), 0u);
    EXPECT_TRUE(captured.WorkMatches(collector));
    RecordRegionSample(collector, 2, 0.0);
    collector.RecordSave(false, 30.0);
    auto copied = captured;
    auto moved = std::move(captured);
    EXPECT_FALSE(captured.Enabled());
    EXPECT_EQ(copied.RetainedSampleBytes(), 0u);
    EXPECT_EQ(moved.RetainedSampleBytes(), 0u);
    const auto scheduler_duration = moved.SchedulerDuration();
    const auto save_duration = moved.SaveDuration();
    ASSERT_TRUE(scheduler_duration);
    ASSERT_TRUE(save_duration);
    EXPECT_DOUBLE_EQ(scheduler_duration->p99, 30.0);
    EXPECT_DOUBLE_EQ(save_duration->p99, 10.0);
    EXPECT_EQ(moved.Samples(), 1u);
    EXPECT_EQ(moved.SaveSamples(), 1u);
    EXPECT_EQ(collector.Samples(), 2u);
    EXPECT_FALSE(moved.WorkMatches(collector));
    const auto resumed = collector.SchedulerDuration();
    ASSERT_TRUE(resumed);
    EXPECT_DOUBLE_EQ(resumed->p50, 15.0);
    const auto work_trace = moved.TraceDigest();
    RecordRegionSample(moved, 2, 1.0);
    moved.RecordSave(true, 1.0);
    EXPECT_EQ(moved.Samples(), 1u);
    EXPECT_EQ(moved.SaveSamples(), 1u);
    EXPECT_EQ(moved.TraceDigest(), work_trace);
    EXPECT_EQ(moved.DroppedSamples(), 2u);
    EXPECT_FALSE(moved.Complete());
    moved.Clear();
    EXPECT_TRUE(moved.Enabled());
    EXPECT_EQ(moved.RetainedSampleBytes(), RegionCollector::kSampleStorageBytes);
    EXPECT_EQ(moved.Samples(), 0u);
    EXPECT_FALSE(moved.SchedulerDuration());

    luminumbra::simulation::SimBudgetTelemetry live_budget;
    live_budget.SetEnabled(true);
    live_budget.Regions().SetEnabled(true);
    const std::uint64_t observed_bytes = 55;
    live_budget.SetRegionCaptureObserver(
        &observed_bytes, [](const void* context, RegionCollector& captured_regions) {
            captured_regions.ObserveParkedBytes(*static_cast<const std::uint64_t*>(context));
        });
    const auto observed = live_budget;
    EXPECT_EQ(observed.Regions().parked_bytes.current, observed_bytes);
    const auto detached = std::move(live_budget);
    EXPECT_FALSE(live_budget.Enabled());
    const auto detached_capture = detached.Capture();
    EXPECT_EQ(detached_capture.Regions().parked_bytes.current, 0u);
}

TEST(RegionTelemetry, TimingExcludedFromReplayMatch) {
    luminumbra::simulation::SimBudgetTelemetry first, replay;
    first.SetEnabled(true);
    replay.SetEnabled(true);
    first.Regions().SetEnabled(true);
    replay.Regions().SetEnabled(true);
    RecordRegionSample(first.Regions(), 1, 0.01);
    RecordRegionSample(replay.Regions(), 1, 999.0);
    first.Regions().RecordSave(true, 0.1);
    replay.Regions().RecordSave(true, 1234.0);
    EXPECT_TRUE(first.WorkMatches(replay));
    EXPECT_EQ(first.Regions().TraceDigest(), replay.Regions().TraceDigest());
    EXPECT_NE(
        RegionalServer::RegionTelemetryJson(first.Regions(), replay.Regions())["duration_ms"],
        RegionalServer::RegionTelemetryJson(replay.Regions(), first.Regions())["duration_ms"]);
    replay.Regions().RecordWater(1, 1, 0, 0, 64);
    EXPECT_FALSE(first.WorkMatches(replay));
    replay = first;
    RegionCollector::Activities runtime_only{};
    runtime_only[RegionCollector::ControllersCreated] = 3;
    runtime_only[RegionCollector::ControllersRemoved] = 2;
    runtime_only[RegionCollector::StaleHandles] = 1;
    // Observe the same completed boundary in both collectors, with different runtime receipts.
    first.Regions().ObserveActivity({});
    replay.Regions().ObserveActivity(runtime_only);
    EXPECT_TRUE(first.WorkMatches(replay));
    EXPECT_NE(first.Regions().Totals(), replay.Regions().Totals());
    replay = first;
    RegionCollector::Activities changed{};
    changed[RegionCollector::FullUpdates] = 1;
    replay.Regions().ObserveActivity(changed);
    EXPECT_FALSE(first.WorkMatches(replay));
    replay = first;
    replay.Regions().Clear();
    const RegionCollector::SchedulerObservation observation{1, 1, 0, 0, 5};
    replay.Regions().RecordScheduler(1, 1.0, observation, 1, 5, 10, 0, 0, 0, "fedcba9876543210");
    replay.Regions().ObserveActivity({});
    EXPECT_FALSE(first.WorkMatches(replay));
    replay = first;
    EXPECT_TRUE(first.WorkMatches(replay));
    first.Regions().RecordScheduler(1, -1.0, {}, 0, 0, 0, 0, 0, 0, "");
    EXPECT_FALSE(first.Regions().Complete());
    EXPECT_FALSE(first.WorkMatches(replay));
}

TEST(RegionTelemetry, RefusalsPreserveAcceptedObservations) {
    for (const double invalid : {-1.0,
                                 std::numeric_limits<double>::infinity(),
                                 std::numeric_limits<double>::quiet_NaN()}) {
        RegionCollector collector;
        collector.SetEnabled(true);
        RecordRegionSample(collector, 1, 1.0);
        const auto trace = collector.TraceDigest();
        RecordRegionSample(collector, 2, invalid);
        EXPECT_EQ(collector.Samples(), 1u);
        EXPECT_EQ(collector.accepted_work_total, 5u);
        EXPECT_EQ(collector.TraceDigest(), trace);
        EXPECT_EQ(collector.DroppedSamples(), 1u);
        collector.RecordSave(true, invalid);
        EXPECT_EQ(collector.SaveSamples(), 0u);
        EXPECT_EQ(collector.DroppedSamples(), 2u);
        EXPECT_FALSE(collector.Complete());
    }
    for (const auto rejected_tick : std::array<std::uint64_t, 3>{0, 1, 3}) {
        RegionCollector collector;
        collector.SetEnabled(true);
        RecordRegionSample(collector, 1, 1.0);
        RecordRegionSample(collector, rejected_tick, 2.0);
        EXPECT_EQ(collector.Samples(), 1u);
        EXPECT_EQ(collector.DroppedSamples(), 1u);
    }
    RegionCollector collector;
    collector.SetEnabled(true);
    collector.RecordScheduler(1, 1.0, {}, 0, 0, 0, 0, 0, 0, "");
    EXPECT_EQ(collector.Samples(), 0u);
    EXPECT_EQ(collector.DroppedSamples(), 1u);
    collector.Clear();
    const auto maximum = std::numeric_limits<std::uint64_t>::max();
    collector.RecordWater(maximum, maximum, 0, 0, maximum);
    collector.RecordWater(1, 1, 0, 0, 1);
    EXPECT_EQ(collector.water_reserved, maximum);
    EXPECT_EQ(collector.water_used, maximum);
    EXPECT_EQ(collector.water_completed_cells, maximum);
    EXPECT_FALSE(collector.Complete());
    collector.Clear();
    RegionCollector::Activities values{};
    values[RegionCollector::FullUpdates] = 4;
    collector.SetActivityBaseline(values);
    values[RegionCollector::FullUpdates] = 3;
    collector.ObserveActivity(values);
    EXPECT_FALSE(collector.Complete());
    EXPECT_EQ(collector.Totals()[RegionCollector::FullUpdates], 0u);
    collector.Clear();
    collector.RecordFault(7, "bounded\ntext");
    EXPECT_EQ(collector.FaultMessage(), "bounded?text");
    EXPECT_TRUE(collector.Complete());
    auto copied = collector;
    RecordRegionSample(collector, 1, 2.0);
    EXPECT_FALSE(copied.SchedulerDuration());
    EXPECT_EQ(copied.Samples(), 0u);
    auto moved = std::move(copied);
    EXPECT_TRUE(moved.Enabled());
    EXPECT_FALSE(copied.Enabled());
    RecordRegionSample(copied, 1, 1.0);
    EXPECT_EQ(copied.Samples(), 0u);
    collector.Clear();
    collector.RecordFault(8, std::string(RegionCollector::kMaxFaultBytes + 1, 'x'));
    EXPECT_FALSE(collector.Complete());
    EXPECT_EQ(collector.DroppedSamples(), 1u);
}

class RegionalTelemetryFixture : public testing::Test {
protected:
    std::filesystem::path fixture_root;
    void SetUp() override {
        fixture_root = Luminumbra::TestSupport::CreateUniqueTempDirectory("regional_telemetry_");
        Luminumbra::TestSupport::WriteSimulationContentFixture(fixture_root);
        for (const auto* species : {"grovestrider", "ridgeback_stalker"})
            WriteRegionInput(fixture_root / "data/common/creatures/species" /
                                 (std::string(species) + ".json"),
                             "{\"id\":\"" + std::string(species) + "\"}");
        WriteRegionInput(
            fixture_root / "data/common/systems.json",
            R"({"sim":{"active_regions":{"enabled":true},"aether_state":{"enabled":true}}})");
        WriteRegionInput(fixture_root / "worlds/atlas/presets/default.json", R"({
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
        std::filesystem::remove_all(fixture_root, error);
    }
    static void WriteRegionInput(const std::filesystem::path& path, std::string_view bytes) {
        std::filesystem::create_directories(path.parent_path());
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        output.close();
        if (!output.good())
            throw std::runtime_error("Cannot write regional fixture");
    }
    RegionalServer::ServerWorldRunnerConfig Configuration(bool budget_enabled) const {
        RegionalServer::ServerWorldRunnerConfig config;
        config.root_path = Luminumbra::Filesystem::GenericPathString(fixture_root) + "/";
        config.seed = "1337";
        config.surface_radius = 1;
        config.collision_radius = 1;
        config.test_streaming_radius_cap = 1;
        config.avatar_count = 1;
        config.ecology_roster = true;
        config.planted_roster = true;
        config.sim_budget = budget_enabled;
        config.autosave_interval_ticks = 0;
        return config;
    }
};

TEST_F(RegionalTelemetryFixture, CachedParkedBytesAreConstObservations) {
    using WorldSystem = Luminumbra::Systems::SHIELD_WorldSystem;
    static_assert(noexcept(std::declval<const WorldSystem&>().parked_byte_count()));
    RegionalServer::ServerWorldRunner runner(Configuration(true));
    ASSERT_TRUE(runner.Boot());
    auto* session = runner.Session();
    ASSERT_NE(session, nullptr);
    auto* world = session->GetWorldSystem();
    ASSERT_NE(world, nullptr);
    const WorldSystem& observation = *world;
    EXPECT_EQ(observation.parked_byte_count(), 0u);
    const auto spawn = session->GetMetadata().spawnPoint;
    const Luminumbra::Vec3 near_anchor(
        spawn.x, world->GetTerrainHeightAt(spawn.x, spawn.z) + 2.0f, spawn.z);
    const Luminumbra::Vec3 surface(near_anchor.x, near_anchor.y - 2.0f, near_anchor.z);
    for (int pass = 0; pass < 12; ++pass) {
        world->update(session->GetRegistry(), {near_anchor}, session->GetPhysicsSystem());
        world->wait_for_streaming_jobs();
    }
    ASSERT_GT(world->EditTerrainVoxel(surface, 3.0f, false, session->GetPhysicsSystem()), 0);
    const float far_x = near_anchor.x + 40.0f * static_cast<float>(Luminumbra::CHUNK_SIZE_X);
    const Luminumbra::Vec3 far_anchor(
        far_x, world->GetTerrainHeightAt(far_x, near_anchor.z) + 2.0f, near_anchor.z);
    world->note_persistence_tick(7);
    for (int pass = 0; pass < 12; ++pass) {
        world->update(session->GetRegistry(), {far_anchor}, session->GetPhysicsSystem());
        world->wait_for_streaming_jobs();
    }
    const auto status = world->parking_status();
    ASSERT_GT(status.parked_count, 0u);
    ASSERT_GT(status.parked_bytes, 0u);
    const auto parked = world->snapshot_parked_chunks();
    ASSERT_FALSE(parked.empty());
    ASSERT_TRUE(parked.front());
    EXPECT_EQ(observation.parked_byte_count(), status.parked_bytes);
    EXPECT_EQ(observation.parked_byte_count(), status.parked_bytes);
    const auto captured = session->CaptureSimBudgetTelemetry();
    EXPECT_EQ(captured.Regions().parked_bytes.current, status.parked_bytes);
    EXPECT_EQ(captured.Regions().parked_bytes.maximum, status.parked_bytes);
    EXPECT_EQ(world->snapshot_parked_chunks(), parked);
    EXPECT_EQ(world->parking_status().oldest_parked_tick, status.oldest_parked_tick);
    EXPECT_FALSE(session->GetRegionSchedulerFault());
    ASSERT_TRUE(world->unpark_chunk(parked.front()->get_id()));
    EXPECT_LT(observation.parked_byte_count(), status.parked_bytes);
    EXPECT_EQ(observation.parked_byte_count(), world->parking_status().parked_bytes);
    const auto after_unpark = session->CaptureSimBudgetTelemetry();
    EXPECT_EQ(after_unpark.Regions().parked_bytes.current, observation.parked_byte_count());
    runner.Shutdown();
}

TEST_F(RegionalTelemetryFixture, ProductionCapturePeakRespectsBudget) {
    RegionalServer::ServerWorldRunner first(Configuration(true)), replay(Configuration(true));
    ASSERT_TRUE(first.Boot());
    ASSERT_TRUE(replay.Boot());
    ASSERT_NE(first.Session(), nullptr);
    ASSERT_NE(replay.Session(), nullptr);
    const auto baseline = RegionCollector::SampleStorageUsageForTesting().current;
    ASSERT_EQ(baseline, RegionCollector::kHeavySampleStorageBytes);
    RegionCollector::ResetSampleStoragePeakForTesting();
    for (auto* session : {first.Session(), replay.Session()}) {
        session->SetSimBudgetTelemetryEnabled(true);
        auto& collector = session->GetSimBudgetTelemetry().Regions();
        for (std::size_t index = 0; index < RegionCollector::kMaxSamples; ++index)
            RecordRegionSample(
                collector, static_cast<std::uint64_t>(index) + 1, static_cast<double>(index % 100));
        for (std::size_t index = 0; index < RegionCollector::kMaxSaveSamples; ++index)
            collector.RecordSave(true, static_cast<double>(index % 10));
    }
    const auto first_capture = first.Session()->CaptureSimBudgetTelemetry();
    const auto replay_capture = replay.Session()->CaptureSimBudgetTelemetry();
    const auto repeated = first.Session()->CaptureSimBudgetTelemetry();
    const auto copied = first.Session()->GetSimBudgetTelemetry();
    nlohmann::json artifact;
    RegionalServer::AppendSimBudgetArtifact(artifact, first_capture, replay_capture);
    EXPECT_TRUE(first_capture.WorkMatches(replay_capture));
    EXPECT_TRUE(first_capture.WorkMatches(repeated));
    EXPECT_TRUE(first_capture.WorkMatches(copied));
    EXPECT_EQ(first_capture.Regions().RetainedSampleBytes(), 0u);
    EXPECT_EQ(replay_capture.Regions().RetainedSampleBytes(), 0u);
    EXPECT_EQ(copied.Regions().RetainedSampleBytes(), 0u);
    const auto usage = RegionCollector::SampleStorageUsageForTesting();
    EXPECT_EQ(usage.current, baseline);
    EXPECT_EQ(usage.peak, baseline);
    EXPECT_LT(usage.peak, 2u * 1024u * 1024u);
    const auto& block = artifact.at("sim_budget").at("regions");
    EXPECT_EQ(block.at("samples"), RegionCollector::kMaxSamples);
    EXPECT_EQ(block.at("persistence").at("save_samples"), RegionCollector::kMaxSaveSamples);
    EXPECT_TRUE(block.at("complete").get<bool>());
    EXPECT_DOUBLE_EQ(block.at("duration_ms").at("p50").get<double>(), 49.5);
    EXPECT_DOUBLE_EQ(block.at("persistence").at("save_duration_ms").at("maximum").get<double>(),
                     9.0);
    first.Shutdown();
    replay.Shutdown();
    EXPECT_EQ(RegionCollector::SampleStorageUsageForTesting().current, 0u);
    // Exported summaries and their copies must not retain a destroyed session callback.
    const auto after_shutdown = copied;
    EXPECT_TRUE(after_shutdown.WorkMatches(first_capture));
    EXPECT_EQ(after_shutdown.Regions().RetainedSampleBytes(), 0u);
    EXPECT_EQ(
        RegionalServer::RegionTelemetryJson(first_capture.Regions(), replay_capture.Regions()),
        block);
}

TEST_F(RegionalTelemetryFixture, RegionCountersMeasureAcceptedWork) {
    RegionalWorld::WorldClock schedule_clock;
    RegionalWorld::ActiveRegionLedger ledger;
    ledger.mark_edited({0, 0}, schedule_clock);
    const std::array<RegionalWorld::RegionWork, 2> inputs{
        {{{0, 0}, 17, 0, 0, 0}, {{30000, 30000}, 999999, 0, 0, 0}}};
    schedule_clock.set_tick(1);
    std::uint64_t retained_records = 0;
    const auto scheduled = ledger.schedule(
        schedule_clock,
        std::nullopt,
        inputs,
        {},
        {},
        [&retained_records](const RegionalWorld::ActiveRegionRecord&) { ++retained_records; });
    EXPECT_EQ(scheduled.work, 17u);
    EXPECT_EQ(scheduled.rejected.work, 1u);
    EXPECT_EQ(retained_records, ledger.records().size());
    const auto committed = ledger.canonical_bytes();
    EXPECT_THROW(ledger.schedule(schedule_clock,
                                 std::nullopt,
                                 inputs,
                                 {},
                                 {},
                                 [&retained_records](const RegionalWorld::ActiveRegionRecord&) {
                                     ++retained_records;
                                 }),
                 std::invalid_argument);
    EXPECT_EQ(ledger.canonical_bytes(), committed);
    EXPECT_EQ(retained_records, ledger.records().size());
    RegionalServer::ServerWorldRunner runner(Configuration(true));
    ASSERT_TRUE(runner.Boot());
    auto* session = runner.Session();
    ASSERT_NE(session, nullptr);
    session->RecordActiveRegionWork({{30000, 30000}, 999999, 0, 0, 0});
    ASSERT_EQ(runner.RunFixedTicks(3).ticks_executed, 3u);
    ASSERT_FALSE(session->GetRegionSchedulerFault());
    const auto capture = session->CaptureSimBudgetTelemetry();
    const auto& collector = capture.Regions();
    const auto activity = session->GetCreatureActivity();
    const auto population = session->GetPopulationActivity();
    EXPECT_EQ(collector.Samples(), 3u);
    EXPECT_EQ(collector.rejected_work, 1u);
    EXPECT_LT(collector.accepted_work_total, 999999u);
    EXPECT_GT(activity.full_updates + activity.coarse_updates, 0u);
    const RegionCollector::Activities expected{activity.promotions,
                                               activity.demotions,
                                               activity.full_updates,
                                               activity.coarse_updates,
                                               activity.births,
                                               activity.deaths,
                                               activity.removals,
                                               activity.parked_travel,
                                               activity.controllers_created,
                                               activity.controllers_removed,
                                               activity.stale_handles,
                                               population.successes,
                                               population.failures};
    EXPECT_EQ(collector.Totals(), expected);
    EXPECT_EQ(collector.creatures_full, activity.full);
    EXPECT_EQ(collector.creatures_coarse, activity.coarse);
    EXPECT_EQ(collector.creatures_frozen, activity.frozen);
    EXPECT_EQ(collector.pending_activations, population.pending_activations);
    EXPECT_EQ(collector.ledger_records.current, session->GetActiveRegionLedger().records().size());
    EXPECT_EQ(collector.active.current + collector.reduced.current + collector.frozen.current,
              collector.ledger_records.current);
    EXPECT_EQ(collector.water_reserved, collector.water_used + collector.water_unused);
    const auto before = session->CaptureAuthorityState();
    ASSERT_TRUE(before.state);
    const auto repeated = session->CaptureSimBudgetTelemetry();
    EXPECT_TRUE(capture.WorkMatches(repeated));
    const auto after = session->CaptureAuthorityState();
    ASSERT_TRUE(after.state);
    EXPECT_EQ(before.state->chunk_hash(), after.state->chunk_hash());
    runner.Shutdown();
}

TEST_F(RegionalTelemetryFixture, OverflowMarksCaptureIncompleteWithoutSimulationEffect) {
    std::string reference_hash, reference_ledger;
    for (const bool overflow : {false, true}) {
        SCOPED_TRACE(overflow);
        RegionalServer::ServerWorldRunner runner(Configuration(overflow));
        ASSERT_TRUE(runner.Boot());
        auto* session = runner.Session();
        ASSERT_NE(session, nullptr);
        const auto before = session->CaptureAuthorityState();
        ASSERT_TRUE(before.state);
        if (overflow) {
            auto& collector = session->GetSimBudgetTelemetry().Regions();
            for (std::size_t index = 0; index <= RegionCollector::kMaxSamples; ++index)
                RecordRegionSample(collector, static_cast<std::uint64_t>(index) + 1, 1.0);
            EXPECT_FALSE(collector.Complete());
            EXPECT_EQ(collector.DroppedSamples(), 1u);
            const auto after = session->CaptureAuthorityState();
            ASSERT_TRUE(after.state);
            EXPECT_EQ(before.state->chunk_hash(), after.state->chunk_hash());
            EXPECT_FALSE(session->GetRegionSchedulerFault());
        }
        EXPECT_EQ(runner.RunFixedTicks(3).ticks_executed, 3u);
        ASSERT_FALSE(session->GetRegionSchedulerFault());
        const auto hash = runner.ComputeWorldHash();
        ASSERT_FALSE(hash.empty());
        if (!overflow) {
            reference_hash = hash;
            reference_ledger = session->GetActiveRegionLedger().canonical_bytes();
        } else {
            EXPECT_EQ(hash, reference_hash);
            EXPECT_EQ(session->GetActiveRegionLedger().canonical_bytes(), reference_ledger);
            const auto capture = session->CaptureSimBudgetTelemetry();
            EXPECT_EQ(capture.Regions().Samples(), RegionCollector::kMaxSamples);
            EXPECT_EQ(capture.Regions().DroppedSamples(), 4u);
            nlohmann::json artifact;
            RegionalServer::AppendSimBudgetArtifact(artifact, capture, capture);
            EXPECT_FALSE(artifact["sim_budget"]["regions"]["complete"].get<bool>());
            EXPECT_FALSE(artifact["sim_budget"]["work_replay_match"].get<bool>());
        }
        runner.Shutdown();
    }
}

TEST(RegionTelemetry, DisabledArtifactsMatchExactExistingFixtures) {
    const Luminumbra::TestSupport::ScopedTempFile file("regional_disabled_", ".json");
    for (int scenario = 0; scenario < 3; ++scenario) {
        const auto fixture = std::filesystem::path(LUMINUMBRA_SOURCE_ROOT) /
                             "test/fixtures/smoke_artifact" /
                             ("devel-" + std::to_string(scenario) + ".json");
        const auto baseline = ReadRegionFixture(fixture);
        for (const bool active_regions : {false, true}) {
            ServerCliOptions options;
            RegionalServer::SmokeRunResult first, replay;
            FixedSmokeInputs(scenario, options, first, replay);
            RegionalWorld::GameSession session;
            session.SetActiveRegionsEnabled(active_regions);
            session.SetSimBudgetTelemetryEnabled(false);
            first.sim_budget = session.CaptureSimBudgetTelemetry();
            replay.sim_budget = first.sim_budget;
            options.artifact_path = Luminumbra::Filesystem::GenericPathString(file.Path());
            EXPECT_EQ(RegionalServer::WriteSmokeArtifact(options, first, replay),
                      scenario == 2 ? 1 : 0);
            EXPECT_EQ(ReadRegionFixture(file.Path()), NativeRegionFixture(baseline));
        }
        // Freeze the pre-extension v2 output independently, starting from the exact fixture.
        auto expected = nlohmann::json::parse(baseline);
        auto stages = nlohmann::json::array();
        for (const auto name : luminumbra::simulation::kSimBudgetStageNames)
            stages.push_back({{"name", name},
                              {"samples", 0},
                              {"work_total", 0},
                              {"work_trace", nlohmann::json::array()},
                              {"duration_ms", nullptr}});
        expected["sim_budget"] = {{"schema", "luminumbra.sim_budget.v2"},
                                  {"work_replay_match", true},
                                  {"stages", std::move(stages)}};
        ServerCliOptions options;
        RegionalServer::SmokeRunResult first, replay;
        FixedSmokeInputs(scenario, options, first, replay);
        options.sim_budget = true;
        first.sim_budget.SetEnabled(true);
        replay.sim_budget.SetEnabled(true);
        options.artifact_path = Luminumbra::Filesystem::GenericPathString(file.Path());
        EXPECT_EQ(RegionalServer::WriteSmokeArtifact(options, first, replay),
                  scenario == 2 ? 1 : 0);
        EXPECT_EQ(ReadRegionFixture(file.Path()), NativeRegionFixture(expected.dump(2) + "\n"));
    }
}
} // namespace
