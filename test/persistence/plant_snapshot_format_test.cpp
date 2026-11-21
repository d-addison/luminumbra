#include <gtest/gtest.h>

#include "../support/UniqueTempPath.h"
#include "luminumbra_common/core/SystemConfig.h"
#include "luminumbra_common/persistence/PlantSnapshot.h"
#include "luminumbra_common/persistence/WorldSaveService.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <utility>
#include <vector>

namespace {
namespace P = Luminumbra::Persistence;
namespace E = Luminumbra::Ecs;
namespace W = Luminumbra::world;
using Json = nlohmann::json;
using Service = P::WorldSaveService;

E::EntitySnapshotRecord Plant(std::uint64_t id) {
    E::EntitySnapshotRecord record;
    record.entity_id = id;
    record.name = "plant";
    record.components = {{"PlantGenome", {{"genes", {0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0}}}},
                         {"PlantGrowth",
                          {{"species_id", 1u},
                           {"stage", 0u},
                           {"quality", 100u},
                           {"growth_points", 0u},
                           {"stress_points", 0u},
                           {"tended", 0u},
                           {"planted_tick", 0u},
                           {"last_tick", 0u}}},
                         {"Transform", {{"x", 0.0}, {"y", 0.0}, {"z", 0.0}}}};
    return record;
}

P::PlantSnapshot Minimal() {
    return {.next_id = 2, .entities = {Plant(1)}};
}

P::PlantSnapshot Complete() {
    auto snapshot = Minimal();
    auto& components = snapshot.entities.front().components;
    components.push_back({"SoilFeeder", {{"uptake", 1u}, {"absorbed", 2u}}});
    components.push_back(
        {"PlantHealth",
         {{"infection", 0u}, {"resistance", 1000u}, {"state", 0u}, {"infected_ticks", 0u}}});
    components.push_back({"Pollination",
                          {{"pollinated", false},
                           {"last_pollen_tick", 0u},
                           {"crosses", 0u},
                           {"next_genome", {0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 1.0}}}});
    components.push_back({"CropLifecycle",
                          {{"perennial", true},
                           {"ripe_ticks", 0u},
                           {"lifespan_ticks", 0u},
                           {"generations", 0u},
                           {"species_id", 1u}}});
    components.push_back({"PlantRegion", {{"owner_x", 0}, {"owner_z", 0}, {"region_stamp", 0u}}});
    components.push_back({"PollinationTag", Json::object()});
    components.push_back({"Combustible",
                          {{"fuel_milli", 1000u},
                           {"moisture_milli", 0u},
                           {"burn_state", 0u},
                           {"burn_ticks_remaining", 0u},
                           {"ignition_radius", 0.0}}});
    components.push_back({"Mortal", {{"age_ticks", 0u}, {"lifespan_ticks", 10u}, {"dead", 0u}}});
    components.push_back(
        {"Grazeable",
         {{"biomass", 0.25}, {"last_grazed_tick", std::numeric_limits<std::uint32_t>::max()}}});
    components.push_back({"WaterSource", {{"output", 1000u}}});
    components.push_back({"IgnitionSource", {{"radius", 1.0}, {"intensity", 1.0}}});
    components.push_back({"FlowingWaterSource", {{"flow_rate", 0.5}}});
    components.push_back({"Decay",
                          {{"decay_ticks", 0u},
                           {"decay_duration", 100u},
                           {"nutrient_released_milli", 0u},
                           {"fully_decomposed", 0u}}});
    components.push_back(
        {"FieldEmitter", {{"channel", 0u}, {"radius_cells", 31u}, {"rate_raw_per_tick", 100u}}});
    return snapshot;
}

std::string Encode(const P::PlantSnapshot& snapshot) {
    std::string bytes;
    std::string error;
    EXPECT_TRUE(P::EncodePlantSnapshot(snapshot, bytes, error)) << error;
    return bytes;
}

Json Document(const P::PlantSnapshot& snapshot) {
    return Json::parse(Encode(snapshot));
}

void ExpectRefusal(std::string_view bytes) {
    auto target = Minimal();
    const auto before = Encode(target);
    std::string error;
    EXPECT_FALSE(P::DecodePlantSnapshot(bytes, target, error)) << bytes;
    EXPECT_FALSE(error.empty());
    EXPECT_EQ(Encode(target), before);
    P::MemberDigest digest{.present = true, .bytes = 42, .fnv64 = 73};
    EXPECT_FALSE(Service::decode_plant_snapshot_member(bytes, target, digest, error));
    EXPECT_EQ(Encode(target), before);
    EXPECT_TRUE(digest.present);
    EXPECT_EQ(digest.bytes, 42u);
    EXPECT_EQ(digest.fnv64, 73u);
}

Json& Data(Json& document, std::string_view type) {
    auto& components = document.at("entities").at(0).at("components");
    const auto found =
        std::find_if(components.begin(), components.end(), [&](const auto& component) {
            return component.at("type") == type;
        });
    if (found == components.end())
        throw std::runtime_error("missing component in format fixture");
    return found->at("data");
}

TEST(PlantSnapshotFormat, ExactEmptyBytesRetainEveryCursorIncludingExhaustion) {
    for (const auto cursor :
         {std::uint64_t{1}, std::uint64_t{37}, W::ActiveRegionLedger::kPersistentIdLimit}) {
        const P::PlantSnapshot snapshot{.next_id = cursor, .entities = {}};
        const auto expected =
            "{\"authority_revision\":" +
            std::to_string(luminumbra::core::SimulationIdentityValues::kAuthorityRulesRevision) +
            ",\"component_count\":0,\"entities\":[],\"entity_count\":0,\"next_id\":" +
            std::to_string(cursor) +
            ",\"order_contract\":\"simulation_id_ascending_component_type_ascending\",\"schema\":"
            "\"luminumbra.plants.v2\"}";
        EXPECT_EQ(Encode(snapshot), expected);
        P::PlantSnapshot restored;
        std::string error;
        ASSERT_TRUE(P::DecodePlantSnapshot(expected, restored, error)) << error;
        EXPECT_EQ(restored.next_id, cursor);
        EXPECT_TRUE(restored.entities.empty());
    }
}

TEST(PlantSnapshotFormat, ExactNonemptyBytesAndPermutationIndependentEncoding) {
    const auto expected =
        "{\"authority_revision\":" +
        std::to_string(luminumbra::core::SimulationIdentityValues::kAuthorityRulesRevision) +
        R"(,"component_count":3,"entities":[{"components":[{"data":{"genes":[0.0,0.0,0.0,0.0,0.0,0.0,0.0,0.0]},"type":"PlantGenome","version":1},{"data":{"growth_points":0,"last_tick":0,"planted_tick":0,"quality":100,"species_id":1,"stage":0,"stress_points":0,"tended":0},"type":"PlantGrowth","version":1},{"data":{"x":0.0,"y":0.0,"z":0.0},"type":"Transform","version":1}],"id":1}],"entity_count":1,"next_id":2,"order_contract":"simulation_id_ascending_component_type_ascending","schema":"luminumbra.plants.v2"})";
    EXPECT_EQ(Encode(Minimal()), expected);
    auto snapshot = Minimal();
    snapshot.entities.front().legacy_entity_id = 9999;
    std::reverse(snapshot.entities.front().components.begin(),
                 snapshot.entities.front().components.end());
    EXPECT_EQ(Encode(snapshot), expected);
    snapshot.next_id = 4;
    snapshot.entities.push_back(Plant(3));
    const auto ascending = Encode(snapshot);
    std::reverse(snapshot.entities.begin(), snapshot.entities.end());
    EXPECT_EQ(Encode(snapshot), ascending);
}

TEST(PlantSnapshotFormat, AllSeventeenComponentsRoundTripWithExplicitAbsenceAndSignedZero) {
    auto snapshot = Complete();
    ASSERT_EQ(snapshot.entities.front().components.size(), P::kMaxPlantSnapshotComponents);
    snapshot.next_id = 3;
    snapshot.entities.push_back(Plant(2));
    snapshot.entities.back().components.back().data["x"] = -0.0f;
    const auto bytes = Encode(snapshot);
    P::PlantSnapshot restored;
    std::string error;
    ASSERT_TRUE(P::DecodePlantSnapshot(bytes, restored, error)) << error;
    EXPECT_EQ(Encode(restored), bytes);
    EXPECT_EQ(restored.entities.front().components.size(), 17u);
    EXPECT_EQ(restored.entities.back().components.size(), 3u);
    EXPECT_TRUE(std::signbit(restored.entities.back().components.back().data.at("x").get<float>()));
}

TEST(PlantSnapshotFormat, MixedOwnersValidateOnlyDeclaredOwnershipAndShareTheLedgerCursor) {
    auto snapshot = Complete();
    snapshot.next_id = 4;
    snapshot.entities.push_back(Plant(2));
    snapshot.entities.back().components.back().data["x"] = 1024.0f;
    W::ActiveRegionLedger ledger;
    ledger.activate_owned({0, 0}, W::WorldClock{});
    ASSERT_TRUE(ledger.reserve_persistent_ids(snapshot.next_id));
    W::GroundObjectStore ground;
    std::string error;
    EXPECT_TRUE(P::ValidatePlantSnapshot(snapshot, ledger, ground, error)) << error;
    W::ActiveRegionLedger absent_owner;
    ASSERT_TRUE(absent_owner.reserve_persistent_ids(snapshot.next_id));
    EXPECT_FALSE(P::ValidatePlantSnapshot(snapshot, absent_owner, ground, error));
    ASSERT_TRUE(ledger.reserve_persistent_ids(5));
    EXPECT_FALSE(P::ValidatePlantSnapshot(snapshot, ledger, ground, error));
    snapshot.next_id = 5;
    W::GroundObject object;
    object.id = 2;
    object.type = 1;
    ASSERT_TRUE(ground.insert(object));
    EXPECT_FALSE(P::ValidatePlantSnapshot(snapshot, ledger, ground, error));
    EXPECT_EQ(snapshot.entities.size(), 2u);
    ASSERT_TRUE(ground.erase(2));
    object.id = 5;
    ASSERT_TRUE(ground.insert(object));
    EXPECT_FALSE(P::ValidatePlantSnapshot(snapshot, ledger, ground, error));
    ASSERT_TRUE(ground.erase(5));
    object.id = 3;
    ASSERT_TRUE(ground.insert(object));
    EXPECT_TRUE(P::ValidatePlantSnapshot(snapshot, ledger, ground, error));
}

TEST(PlantSnapshotFormat, NegativeOwnersAndMaximalUnownedTicksRemainValid) {
    auto snapshot = Complete();
    for (auto& component : snapshot.entities.front().components) {
        if (component.type == "Transform")
            component.data["x"] = -512.0f;
        if (component.type == "PlantRegion")
            component.data["owner_x"] = -1;
    }
    W::ActiveRegionLedger ledger;
    ledger.activate_owned({-1, 0}, W::WorldClock{});
    ASSERT_TRUE(ledger.reserve_persistent_ids(snapshot.next_id));
    std::string error;
    EXPECT_TRUE(P::ValidatePlantSnapshot(snapshot, ledger, W::GroundObjectStore{}, error));
    auto document = Document(Minimal());
    Data(document, "PlantGrowth")["last_tick"] = W::WorldClock::kTickLimit - 1;
    P::PlantSnapshot restored;
    ASSERT_TRUE(P::DecodePlantSnapshot(document.dump(), restored, error)) << error;
    EXPECT_EQ(Document(restored), document);
}

TEST(PlantSnapshotFormat, RefusesInvalidEnvelopeRepresentationsCountsVersionsAndUnknownFields) {
    const auto valid = Document(Minimal());
    const std::vector<std::pair<std::string, Json>> mutations = {
        {"/schema", "luminumbra.plants.v3"},
        {"/schema", E::EntitySnapshotSchema()},
        {"/order_contract", "unknown"},
        {"/authority_revision", 0u},
        {"/authority_revision", 1e38},
        {"/authority_revision", -1},
        {"/next_id", 0u},
        {"/next_id", -1},
        {"/next_id", 1.0},
        {"/next_id", 1e38},
        {"/next_id", W::ActiveRegionLedger::kPersistentIdLimit + 1},
        {"/next_id", 1u},
        {"/entity_count", 2u},
        {"/entity_count", -1},
        {"/entity_count", P::kMaxPlantSnapshotRecords + 1},
        {"/component_count", 4u},
        {"/component_count", 3.0},
        {"/entities", Json::object()},
        {"/entities/0/id", 0u},
        {"/entities/0/id", -1},
        {"/entities/0/id", 1.0},
        {"/entities/0/id", W::ActiveRegionLedger::kPersistentIdLimit},
        {"/entities/0/components/0/version", 2u},
        {"/entities/0/components/0/version", 1.0},
        {"/entities/0/components/0/type", "FutureComponent"},
        {"/unknown", true},
        {"/entities/0/unknown", true},
        {"/entities/0/components/0/unknown", true}};
    for (const auto& [path, value] : mutations) {
        SCOPED_TRACE(path + " = " + value.dump());
        auto document = valid;
        document[Json::json_pointer(path)] = value;
        ExpectRefusal(document.dump());
    }
    for (const auto* key : {"schema",
                            "next_id",
                            "authority_revision",
                            "entity_count",
                            "component_count",
                            "entities"}) {
        auto document = valid;
        document.erase(key);
        ExpectRefusal(document.dump());
    }
}

TEST(PlantSnapshotFormat, EveryComponentRefusesInvalidDomainsWidthsAndFieldSets) {
    const auto valid = Document(Complete());
    struct Mutation {
        std::string type;
        std::string field;
        Json value;
    };
    const std::vector<Mutation> mutations = {
        {"Transform", "x", 1e38},
        {"Transform", "x", 0},
        {"Transform", "y", nullptr},
        {"PlantGenome", "genes", {0.0}},
        {"PlantGenome", "genes", {2.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0}},
        {"PlantGrowth", "species_id", 65536u},
        {"PlantGrowth", "stage", 6u},
        {"PlantGrowth", "quality", 101u},
        {"PlantGrowth", "growth_points", 1e38},
        {"PlantGrowth", "growth_points", 4294967296ull},
        {"PlantGrowth", "stress_points", -1},
        {"PlantGrowth", "tended", 256u},
        {"PlantGrowth", "last_tick", W::WorldClock::kTickLimit},
        {"PlantGrowth", "planted_tick", 1u},
        {"SoilFeeder", "uptake", 65536u},
        {"SoilFeeder", "absorbed", 4294967296ull},
        {"PlantHealth", "infection", 1001u},
        {"PlantHealth", "resistance", 1001u},
        {"PlantHealth", "state", 255u},
        {"Pollination", "pollinated", 1u},
        {"Pollination", "last_pollen_tick", -1},
        {"Pollination", "last_pollen_tick", W::WorldClock::kTickLimit},
        {"Pollination", "next_genome", Json::array()},
        {"CropLifecycle", "perennial", 1u},
        {"CropLifecycle", "generations", 65536u},
        {"PlantRegion", "owner_x", -32769},
        {"PlantRegion", "owner_z", 32768u},
        {"PlantRegion", "region_stamp", 1u},
        {"PlantRegion", "region_stamp", W::WorldClock::kTickLimit},
        {"PlantRegion", "owner_x", 1u},
        {"Combustible", "fuel_milli", 1001u},
        {"Combustible", "burn_state", 255u},
        {"Combustible", "ignition_radius", -1.0},
        {"Mortal", "dead", 2u},
        {"Grazeable", "biomass", 1.1},
        {"Grazeable", "last_grazed_tick", 4294967296ull},
        {"WaterSource", "output", 65536u},
        {"IgnitionSource", "intensity", 2.0},
        {"FlowingWaterSource", "flow_rate", -1.0},
        {"Decay", "decay_ticks", 101u},
        {"Decay", "nutrient_released_milli", 1001u},
        {"Decay", "fully_decomposed", 1u},
        {"FieldEmitter", "channel", 64u},
        {"FieldEmitter", "radius_cells", 32u},
        {"FieldEmitter", "rate_raw_per_tick", -1}};
    for (const auto& mutation : mutations) {
        SCOPED_TRACE(mutation.type + "." + mutation.field);
        auto document = valid;
        Data(document, mutation.type)[mutation.field] = mutation.value;
        ExpectRefusal(document.dump());
    }
    for (const auto& component : valid.at("entities").at(0).at("components")) {
        const auto type = component.at("type").get<std::string>();
        SCOPED_TRACE(type);
        auto document = valid;
        Data(document, type)["unknown"] = 0u;
        ExpectRefusal(document.dump());
        if (!component.at("data").empty()) {
            document = valid;
            Data(document, type).erase(component.at("data").begin().key());
            ExpectRefusal(document.dump());
        }
    }
}

TEST(PlantSnapshotFormat, RefusesDuplicateKeysIdsComponentsNoncanonicalOrderAndPartialInput) {
    const auto bytes = Encode(Minimal());
    ExpectRefusal("{\"next_id\":2," + bytes.substr(1));
    auto document = Document(Minimal());
    document["entities"].push_back(document["entities"].front());
    document["entity_count"] = 2u;
    document["component_count"] = 6u;
    ExpectRefusal(document.dump());
    document = Document(Minimal());
    document["entities"][0]["components"].push_back(document["entities"][0]["components"][0]);
    document["component_count"] = 4u;
    ExpectRefusal(document.dump());
    document = Document(Minimal());
    auto& components = document["entities"][0]["components"];
    std::reverse(components.begin(), components.end());
    ExpectRefusal(document.dump());
    for (const auto length : {std::size_t{0}, bytes.size() / 2, bytes.size() - 1})
        ExpectRefusal(std::string_view(bytes).substr(0, length));
    auto snapshot = Minimal();
    snapshot.next_id = 3;
    snapshot.entities.push_back(Plant(2));
    document = Document(snapshot);
    std::reverse(document["entities"].begin(), document["entities"].end());
    ExpectRefusal(document.dump());
}

TEST(PlantSnapshotFormat, RefusesMissingCoreOversizedStructuresAndJsonDepthBeforeAdmission) {
    for (const auto* core : {"Transform", "PlantGenome", "PlantGrowth"}) {
        auto document = Document(Minimal());
        auto& components = document["entities"][0]["components"];
        components.erase(std::find_if(components.begin(), components.end(), [&](const auto& value) {
            return value.at("type") == core;
        }));
        document["component_count"] = 2u;
        ExpectRefusal(document.dump());
    }
    auto document = Document(Minimal());
    auto& components = document["entities"][0]["components"];
    for (std::size_t index = components.size(); index <= P::kMaxPlantSnapshotComponents; ++index)
        components.push_back(components.front());
    ExpectRefusal(document.dump());
    document = Document(Minimal());
    Data(document, "PlantGenome")["genes"].push_back(0.0);
    ExpectRefusal(document.dump());
    ExpectRefusal(std::string(65, '[') + std::string(65, ']'));
    document = Document(Minimal());
    Data(document, "Transform")["x"] = std::string(P::SavedInputLimits::kPlantComponentBytes, 'x');
    ExpectRefusal(document.dump());
}

TEST(PlantSnapshotFormat, CandidatePreparationRefusesWithoutPublishingOrKeepingStaleBytes) {
    Service::PreparedPlantSnapshotWrite prepared;
    std::vector<std::string> errors;
    ASSERT_TRUE(prepared.prepare(Complete(), &errors));
    EXPECT_EQ(prepared.bytes(), Encode(Complete()));
    auto invalid = Minimal();
    invalid.entities.push_back(Plant(1));
    std::string output = "untouched";
    std::string error;
    EXPECT_FALSE(P::EncodePlantSnapshot(invalid, output, error));
    EXPECT_EQ(output, "untouched");
    EXPECT_FALSE(prepared.prepare(invalid, &errors));
    EXPECT_TRUE(prepared.bytes().empty());
    EXPECT_FALSE(errors.empty());
}

bool WriteBytes(const std::filesystem::path& path, const std::string& bytes) {
    std::ofstream stream(path, std::ios::binary);
    stream.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    stream.flush();
    return stream.good();
}

TEST(PlantSnapshotFormat, CandidateLoaderAndIntegrityUseTheCodecAndPreserveOutputOnRefusal) {
    const Luminumbra::TestSupport::ScopedTempFile temporary("plant_codec_", ".json");
    const auto root = temporary.Path().parent_path();
    std::filesystem::create_directories(Service::region_directory(root));
    auto target = Minimal();
    const auto before = Encode(target);
    std::vector<std::string> errors;
    EXPECT_FALSE(Service::load_plant_snapshot(target, root, &errors));
    EXPECT_EQ(Encode(target), before);
    const P::PlantSnapshot empty{.next_id = 77, .entities = {}};
    const auto bytes = Encode(empty);
    ASSERT_TRUE(WriteBytes(Service::plant_entities_path(root), bytes));
    ASSERT_TRUE(Service::load_plant_snapshot(target, root, &errors));
    EXPECT_EQ(Encode(target), bytes);
    P::MemberDigest digest;
    std::string error;
    ASSERT_TRUE(Service::decode_plant_snapshot_member(bytes, target, digest, error));
    EXPECT_TRUE(digest.present);
    EXPECT_EQ(digest.bytes, bytes.size());
    // Independent, explicitly specified FNV-1a member fingerprint.
    std::uint64_t expected_hash = 14695981039346656037ull;
    for (const char value : bytes) {
        expected_hash ^= static_cast<unsigned char>(value);
        expected_hash *= 1099511628211ull;
    }
    EXPECT_EQ(digest.fnv64, expected_hash);
    std::stop_source cancelled;
    cancelled.request_stop();
    EXPECT_FALSE(Service::load_plant_snapshot(target, root, &errors, cancelled.get_token()));
    EXPECT_EQ(Encode(target), bytes);
    ASSERT_TRUE(WriteBytes(Service::plant_identities_path(root), "{}"));
    EXPECT_FALSE(Service::load_plant_snapshot(target, root, &errors));
    EXPECT_EQ(Encode(target), bytes);
    std::filesystem::remove(Service::plant_identities_path(root));
    ASSERT_TRUE(WriteBytes(Service::plant_entities_path(root), "{"));
    EXPECT_FALSE(Service::load_plant_snapshot(target, root, &errors));
    EXPECT_EQ(Encode(target), bytes);
    std::filesystem::remove(Service::plant_entities_path(root));
    std::filesystem::create_directory(Service::plant_entities_path(root));
    EXPECT_FALSE(Service::load_plant_snapshot(target, root, &errors));
    EXPECT_EQ(Encode(target), bytes);
}

TEST(PlantSnapshotFormat, ShippingV1PreparationAndReaderSelectionRemainUnchanged) {
    E::EntityRegistrySnapshot legacy;
    legacy.entities.push_back(Plant(0));
    const auto expected = E::SerializeEntityRegistrySnapshotJson(legacy);
    std::string bytes;
    std::string sidecar;
    std::vector<std::string> errors;
    ASSERT_TRUE(Service::prepare_plant_entities(legacy, bytes, &errors, &sidecar));
    EXPECT_EQ(bytes, expected);
    EXPECT_TRUE(sidecar.empty());
    ExpectRefusal(bytes);
    auto refused = Minimal();
    std::string error;
    EXPECT_FALSE(P::DecodePlantSnapshot(bytes, refused, error));
    EXPECT_EQ(error, P::kUnsupportedPlantSnapshot);
    E::EntityRegistrySnapshot output;
    const auto candidate = Encode(Minimal());
    EXPECT_FALSE(E::LoadEntityRegistrySnapshotJson(candidate, output, errors));
    ASSERT_TRUE(Service::prepare_plant_entities(legacy, bytes, &errors, &sidecar));
    EXPECT_EQ(bytes, expected);
}
} // namespace
