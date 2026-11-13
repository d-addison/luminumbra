#include "../support/IndexedSaveFixture.h"
#include "../support/UniqueTempPath.h"
#include "../support/WorldgenTestData.h"
#include "luminumbra_common/components/CreatureComponents.h"
#include "luminumbra_common/components/DecayComponents.h"
#include "luminumbra_common/components/FieldEmitterComponents.h"
#include "luminumbra_common/components/SimulationIdentityComponent.h"
#include "luminumbra_common/core/JobSystem.h"
#include "luminumbra_common/core/WaterComponents.h"
#include "luminumbra_common/persistence/PlantPersistence.h"
#include "luminumbra_common/simulation/SimulationIdentity.h"
#include "luminumbra_common/world/GameSession.h"

#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <future>
#include <iterator>
#include <limits>
#include <new>
#include <stop_token>
#include <string>
#include <type_traits>
#include <vector>

#ifdef _WIN32
#include <malloc.h>
#endif

#include <gtest/gtest.h>

namespace {
struct AllocationState {
    std::size_t remaining = std::numeric_limits<std::size_t>::max();
    std::size_t attempts = 0;
    bool enabled = false;
    std::stop_source* cancel = nullptr;
};
AllocationState& Allocations() {
    static thread_local AllocationState state;
    return state;
}
void CountAllocation() {
    auto& state = Allocations();
    if (!state.enabled)
        return;
    ++state.attempts;
    if (state.remaining == 0 && state.cancel) {
        state.enabled = false;
        state.cancel->request_stop();
        return;
    }
    if (state.remaining == 0)
        throw std::bad_alloc();
    --state.remaining;
}
class AllocationBudget {
public:
    explicit AllocationBudget(std::size_t count = std::numeric_limits<std::size_t>::max(),
                              std::stop_source* cancel = nullptr) {
        Allocations() = {.remaining = count, .attempts = 0, .enabled = true, .cancel = cancel};
    }
    ~AllocationBudget() {
        Allocations().enabled = false;
    }
    AllocationBudget(const AllocationBudget&) = delete;
    AllocationBudget& operator=(const AllocationBudget&) = delete;
};
} // namespace

// This executable is isolated so allocation refusal cannot affect sibling test binaries.
void* operator new(std::size_t size) {
    CountAllocation();
    if (void* pointer = std::malloc(size == 0 ? 1 : size))
        return pointer;
    throw std::bad_alloc();
}
void* operator new[](std::size_t size) {
    return ::operator new(size);
}
void operator delete(void* pointer) noexcept {
    std::free(pointer);
}
void operator delete[](void* pointer) noexcept {
    ::operator delete(pointer);
}
void operator delete(void* pointer, std::size_t) noexcept {
    ::operator delete(pointer);
}
void operator delete[](void* pointer, std::size_t) noexcept {
    ::operator delete(pointer);
}
void* operator new(std::size_t size, const std::nothrow_t&) noexcept {
    try {
        return ::operator new(size);
    } catch (...) {
        return nullptr;
    }
}
void* operator new[](std::size_t size, const std::nothrow_t& tag) noexcept {
    return ::operator new(size, tag);
}
void operator delete(void* pointer, const std::nothrow_t&) noexcept {
    ::operator delete(pointer);
}
void operator delete[](void* pointer, const std::nothrow_t&) noexcept {
    ::operator delete(pointer);
}
void* operator new(std::size_t size, std::align_val_t alignment) {
    CountAllocation();
    const auto boundary = static_cast<std::size_t>(alignment);
    if (size > std::numeric_limits<std::size_t>::max() - (boundary - 1))
        throw std::bad_alloc();
    const auto rounded = size == 0 ? boundary : ((size + boundary - 1) / boundary) * boundary;
#ifdef _WIN32
    void* pointer = _aligned_malloc(rounded, boundary);
#else
    void* pointer = std::aligned_alloc(boundary, rounded);
#endif
    if (pointer)
        return pointer;
    throw std::bad_alloc();
}
void* operator new[](std::size_t size, std::align_val_t alignment) {
    return ::operator new(size, alignment);
}
void operator delete(void* pointer, std::align_val_t) noexcept {
#ifdef _WIN32
    _aligned_free(pointer);
#else
    std::free(pointer);
#endif
}
void operator delete[](void* pointer, std::align_val_t alignment) noexcept {
    ::operator delete(pointer, alignment);
}
void operator delete(void* pointer, std::size_t, std::align_val_t alignment) noexcept {
    ::operator delete(pointer, alignment);
}
void operator delete[](void* pointer, std::size_t, std::align_val_t alignment) noexcept {
    ::operator delete(pointer, alignment);
}
void* operator new(std::size_t size, std::align_val_t alignment, const std::nothrow_t&) noexcept {
    try {
        return ::operator new(size, alignment);
    } catch (...) {
        return nullptr;
    }
}
void* operator new[](std::size_t size,
                     std::align_val_t alignment,
                     const std::nothrow_t& tag) noexcept {
    return ::operator new(size, alignment, tag);
}
void operator delete(void* pointer, std::align_val_t alignment, const std::nothrow_t&) noexcept {
    ::operator delete(pointer, alignment);
}
void operator delete[](void* pointer, std::align_val_t alignment, const std::nothrow_t&) noexcept {
    ::operator delete(pointer, alignment);
}

namespace {
namespace fs = std::filesystem;
namespace C = Luminumbra::Components;
namespace P = Luminumbra::Persistence;
namespace W = Luminumbra::world;
using Service = P::WorldSaveService;

struct UnrelatedResident {
    std::string text;
};

Luminumbra::Ecs::EntitySnapshotRecord
Plant(std::uint64_t id, bool owned = false, bool complete = false) {
    Luminumbra::Ecs::EntitySnapshotRecord record;
    record.entity_id = id;
    record.name = "plant";
    record.components = {
        {"Transform", {{"x", 0.0}, {"y", 0.0}, {"z", 0.0}}},
        {"PlantGenome", {{"genes", {0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0}}}},
        {"PlantGrowth",
         {{"species_id", 1u},
          {"stage", 0u},
          {"quality", 100u},
          {"growth_points", 0u},
          {"stress_points", 0u},
          {"tended", 0u},
          {"planted_tick", 0u},
          {"last_tick", 0u}}},
        {"Grazeable", {{"biomass", 0.25}, {"last_grazed_tick", 4294967295u}}},
        {"Pollination",
         {{"pollinated", false},
          {"last_pollen_tick", 0u},
          {"crosses", 3u},
          {"next_genome", {0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 1.0}}}},
        {"FlowingWaterSource", {{"flow_rate", 0.5}}},
        {"Decay",
         {{"decay_ticks", 1u},
          {"decay_duration", 100u},
          {"nutrient_released_milli", 10u},
          {"fully_decomposed", 0u}}},
        {"FieldEmitter", {{"channel", 2u}, {"rate_raw_per_tick", 100u}, {"radius_cells", 3u}}}};
    if (owned)
        record.components.push_back(
            {"PlantRegion", {{"owner_x", 0}, {"owner_z", 0}, {"region_stamp", 0u}}});
    if (complete) {
        record.components.push_back({"SoilFeeder", {{"uptake", 2u}, {"absorbed", 9u}}});
        record.components.push_back(
            {"PlantHealth",
             {{"infection", 1u}, {"resistance", 100u}, {"state", 0u}, {"infected_ticks", 2u}}});
        record.components.push_back({"CropLifecycle",
                                     {{"perennial", true},
                                      {"ripe_ticks", 2u},
                                      {"lifespan_ticks", 100u},
                                      {"generations", 3u},
                                      {"species_id", 1u}}});
        record.components.push_back({"PollinationTag", nlohmann::json::object()});
        record.components.push_back({"Combustible",
                                     {{"fuel_milli", 1000u},
                                      {"moisture_milli", 20u},
                                      {"burn_state", 0u},
                                      {"burn_ticks_remaining", 0u},
                                      {"ignition_radius", 1.0}}});
        record.components.push_back(
            {"Mortal", {{"age_ticks", 1u}, {"lifespan_ticks", 100u}, {"dead", 0u}}});
        record.components.push_back({"WaterSource", {{"output", 100u}}});
        record.components.push_back({"IgnitionSource", {{"radius", 1.0}, {"intensity", 0.25}}});
    }
    return record;
}

class SessionAdoption : public testing::TestWithParam<bool> {
protected:
    fs::path root;
    Luminumbra::JobSystem jobs;
    unsigned publications = 0;
    void SetUp() override {
        root = Luminumbra::TestSupport::CreateUniqueTempDirectory("session_adoption_");
        fs::create_directories(root / "worlds/atlas/presets");
        fs::copy_file(fs::path(LUMINUMBRA_SOURCE_ROOT) / "worlds/atlas/presets/default.json",
                      root / "worlds/atlas/presets/default.json");
        Luminumbra::TestSupport::CopyWorldgenTestData(LUMINUMBRA_SOURCE_ROOT, root);
        jobs.startup(1);
    }
    void TearDown() override {
        jobs.shutdown();
        std::error_code error;
        fs::remove_all(root, error);
    }
    void Configure(W::GameSession& session) {
        session.SetRootPath(root.generic_string() + "/");
        session.SetJobSystem(&jobs);
        session.SetActiveRegionsEnabled(GetParam());
    }
    fs::path Publish(const P::PlantSnapshot& plants,
                     bool ground = true,
                     const W::GroundNotifications& notifications = {}) {
        const auto directory = root / ("candidate-" + std::to_string(++publications));
        W::ActiveRegionLedger ledger;
        EXPECT_TRUE(ledger.reserve_persistent_ids(plants.next_id));
        ledger.mark_edited({0, 0}, W::WorldClock{});
        EXPECT_TRUE(Luminumbra::Test::PublishPlantSnapshotSave(
            directory, plants, GetParam() ? &ledger : nullptr));
        W::GroundObjectStore objects;
        if (ground) {
            EXPECT_TRUE(objects.insert(W::GroundObject{.id = 2, .type = 7, .payload = 99}));
        }
        const P::PlantSnapshotContract candidate;
        EXPECT_TRUE(Luminumbra::Test::PublishIndexedSave(directory, [&] {
            return Service::save_ground_objects(objects, directory, nullptr, notifications) &&
                   (GetParam() ||
                    Service::save_metadata(
                        nlohmann::json{{"container_version", Service::kContainerVersion}}.dump(),
                        directory));
        }));
        return directory;
    }
    std::shared_ptr<W::GameSession::WorldPreparation>
    Prepare(W::GameSession& session, const fs::path& directory, const std::stop_token& stop = {}) {
        auto options = session.CaptureWorldPreparationOptions();
        options.simulation_v2 = true;
        return W::GameSession::PrepareWorldState(options, directory, stop);
    }
    static std::string Bytes(W::GameSession& session) {
        auto& registry = session.GetRegistry();
        nlohmann::json entities = nlohmann::json::array();
        for (const auto entity : registry.storage<entt::entity>().each()) {
            const auto handle = std::get<0>(entity);
            nlohmann::json value = {{"handle", entt::to_integral(handle)}};
            if (const auto* id = registry.try_get<C::SimulationIdentityComponent>(handle))
                value["identity"] = id->id;
            if (const auto* sentinel = registry.try_get<UnrelatedResident>(handle))
                value["sentinel"] = sentinel->text;
            entities.push_back(std::move(value));
        }
        const auto plant_bytes = Luminumbra::Ecs::SerializeEntityRegistrySnapshotJson(
            luminumbra::foliage::BuildPlantEntitySnapshot(registry));
        nlohmann::json events = nlohmann::json::array();
        for (const auto& event : session.GetSimulationEventBus().pending_events())
            events.push_back(event.canonical_bytes());
        return nlohmann::json{{"entities", entities},
                              {"plants", plant_bytes},
                              {"ledger", session.GetActiveRegionLedger().encode()},
                              {"ground", session.GetGroundObjects().serialize()},
                              {"fields", session.GetFieldPages().canonical_bytes()},
                              {"tick", session.GetSimulationTickCount()},
                              {"revision", session.WorldRevision()},
                              {"events", events},
                              {"event_cursor", session.GetSimulationEventBus().next_sequence()},
                              {"hash", session.ComputePlantSubHash()},
                              {"name", session.GetMetadata().name}}
            .dump();
    }
};

void ExpectRestoredPlantPayloads(const entt::registry& registry) {
    for (const auto plant : registry.view<const C::PlantTag>()) {
        const auto complete = registry.get<C::SimulationIdentityComponent>(plant).id == 4;
        EXPECT_EQ(registry.all_of<C::PollinationTag>(plant), complete);
        if (complete) {
            EXPECT_EQ(registry.get<C::SoilFeederComponent>(plant).absorbed, 9u);
            EXPECT_EQ(registry.get<C::PlantHealthComponent>(plant).infected_ticks, 2u);
            EXPECT_EQ(registry.get<C::CropLifecycleComponent>(plant).generations, 3u);
            EXPECT_EQ(registry.get<C::CombustibleComponent>(plant).moisture_milli, 20u);
            EXPECT_EQ(registry.get<C::MortalComponent>(plant).age_ticks, 1u);
            EXPECT_EQ(registry.get<C::WaterSourceComponent>(plant).output, 100u);
            EXPECT_EQ(registry.get<C::IgnitionSourceComponent>(plant).intensity, 0.25f);
        }
        EXPECT_EQ(registry.get<C::GrazeableComponent>(plant).last_grazed_tick, 4294967295u);
        EXPECT_EQ(registry.get<C::FieldEmitterComponent>(plant).radius_cells, 3);
        EXPECT_EQ(registry.get<C::FlowingWaterSourceComponent>(plant).flow_rate, 0.5f);
        EXPECT_EQ(registry.get<C::DecayComponent>(plant).decay_ticks, 1u);
    }
}

TEST_P(SessionAdoption, MixedRosterRepeatedAndEmptyReplacementPreserveUnrelatedResidents) {
    W::GameSession session;
    Configure(session);
    ASSERT_TRUE(session.CreateWorld("Adopt", "1337", "default"));
    auto& registry = session.GetRegistry();
    const auto creature = registry.create();
    registry.emplace<C::CreatureComponent>(creature).wish_x = 0.75f;
    std::vector<unsigned char> creature_bytes(sizeof(C::CreatureComponent));
    std::memcpy(creature_bytes.data(),
                &registry.get<C::CreatureComponent>(creature),
                creature_bytes.size());
    registry.emplace<C::SimulationIdentityComponent>(creature, 3u);
    registry.emplace<UnrelatedResident>(creature, "creature state");
    const auto scenery = registry.create();
    registry.emplace<UnrelatedResident>(scenery, "scenery state");
    registry.emplace<C::TransformComponent>(scenery).position.x = 19.0f;
    registry.ctx().emplace<UnrelatedResident>("context state");
    session.GetSimulationEventBus().publish(9, "unrelated", "retained");
    const auto revision = session.WorldRevision();
    const auto directory =
        Publish({.next_id = 10, .entities = {Plant(1), Plant(4, GetParam(), true)}});
    for (unsigned repeat = 0; repeat < 3; ++repeat) {
        const auto prepared = Prepare(session, directory);
        ASSERT_TRUE(W::GameSession::WorldPreparationError(*prepared).empty());
        ASSERT_TRUE(session.AdoptPreparedSimulationState(prepared));
        EXPECT_FALSE(session.AdoptPreparedSimulationState(prepared));
        EXPECT_EQ(registry.view<C::PlantTag>().size(), 2u);
        EXPECT_TRUE(registry.valid(creature));
        EXPECT_TRUE(registry.valid(scenery));
        EXPECT_EQ(registry.get<UnrelatedResident>(creature).text, "creature state");
        EXPECT_EQ(registry.get<C::SimulationIdentityComponent>(creature).id, 3u);
        EXPECT_EQ(std::memcmp(creature_bytes.data(),
                              &registry.get<C::CreatureComponent>(creature),
                              creature_bytes.size()),
                  0);
        EXPECT_EQ(registry.ctx().get<UnrelatedResident>().text, "context state");
        EXPECT_EQ(registry.get<C::TransformComponent>(scenery).position.x, 19.0f);
        EXPECT_EQ(session.WorldRevision(), revision);
        ASSERT_NE(session.GetGroundObjects().find(2), nullptr);
        EXPECT_EQ(session.GetGroundObjects().find(2)->payload, 99u);
        ExpectRestoredPlantPayloads(registry);
    }
    const auto empty = Publish({.next_id = 5, .entities = {}}, false);
    ASSERT_TRUE(session.AdoptPreparedSimulationState(Prepare(session, empty)));
    EXPECT_TRUE(registry.view<C::PlantTag>().empty());
    EXPECT_TRUE(session.GetGroundObjects().empty());
    EXPECT_EQ(session.GetActiveRegionLedger().next_persistent_id(), 10u);
    EXPECT_TRUE(registry.valid(creature));
    EXPECT_EQ(session.GetSimulationEventBus().pending_events().front().payload, "retained");
    auto allocator = session.GetActiveRegionLedger();
    const auto next = Luminumbra::simulation::AllocateSimulationIdentity(allocator);
    if (next.has_value()) {
        EXPECT_EQ(next->id, 10u);
    } else {
        FAIL() << "Restored allocator unexpectedly exhausted";
    }
    if (GetParam()) {
        EXPECT_EQ(session.SpawnGroundObject(1, Luminumbra::Vec3{}, 0, 0), 10u);
    }
    ASSERT_TRUE(session.AdoptPreparedSimulationState(Prepare(session, empty)));
    EXPECT_EQ(session.GetActiveRegionLedger().next_persistent_id(), GetParam() ? 11u : 10u);
}

TEST_P(SessionAdoption, FullReopenIgnoresResidentHandlesAndRetainsSessionCreatureIdentities) {
    W::GameSession session;
    Configure(session);
    ASSERT_TRUE(session.CreateWorld("Full reopen", "1337", "default"));
    const auto creature = session.GetRegistry().create();
    session.GetRegistry().emplace<C::CreatureComponent>(creature);
    session.GetRegistry().emplace<C::SimulationIdentityComponent>(creature, 3u);
    // Handle holes and an unrelated runtime component have no saved allocator descriptor.
    const auto hole = session.GetRegistry().create();
    session.GetRegistry().destroy(hole);
    const auto scenery = session.GetRegistry().create();
    session.GetRegistry().emplace<UnrelatedResident>(scenery, "retained");
    const auto candidate = Publish({.next_id = 10, .entities = {Plant(1)}});
    const auto snapshot_root = root / "full";
    const auto id = session.GetMetadata().worldId;
    const auto directory = snapshot_root / "worlds/saves" / id;
    fs::create_directories(directory.parent_path());
    fs::copy(candidate, directory, fs::copy_options::recursive);
    fs::copy_file(session.GetWorldSaveDir() / "preset.json", directory / "preset.json");
    std::ifstream source(session.GetWorldSaveDir() / "world_info.json");
    const std::string metadata(std::istreambuf_iterator<char>(source), {});
    source.close();
    std::shared_ptr<W::GameSession::WorldPreparation> prepared;
    {
        const P::PlantSnapshotContract contract;
        ASSERT_TRUE(Luminumbra::Test::PublishIndexedSave(
            directory, [&] { return Service::save_metadata(metadata, directory); }));
        auto options = session.CaptureWorldPreparationOptions();
        options.save_root = snapshot_root;
        options.simulation_v2 = true;
        prepared = std::async(std::launch::async, [options, id] {
                       return W::GameSession::PrepareWorldLoad(options, id);
                   }).get();
    }
    ASSERT_TRUE(W::GameSession::WorldPreparationError(*prepared).empty());
    const auto before = Bytes(session);
    EXPECT_FALSE(session.AdoptPreparedWorld(prepared));
    EXPECT_EQ(Bytes(session), before);
    ASSERT_TRUE(session.AdoptPreparedSimulationState(prepared));
    EXPECT_TRUE(session.GetRegistry().valid(creature));
    EXPECT_EQ(session.GetRegistry().get<C::SimulationIdentityComponent>(creature).id, 3u);
    EXPECT_EQ(session.GetRegistry().get<UnrelatedResident>(scenery).text, "retained");
    EXPECT_EQ(session.GetActiveRegionLedger().next_persistent_id(), 10u);
    ASSERT_NE(session.GetGroundObjects().find(2), nullptr);
}

TEST_P(SessionAdoption, CorruptionCancellationCollisionAndStalePreparationPreserveEveryByte) {
    W::GameSession session;
    Configure(session);
    ASSERT_TRUE(session.CreateWorld("Refusal", "1337", "default"));
    const auto directory = Publish({.next_id = 10, .entities = {Plant(1), Plant(4)}});
    ASSERT_TRUE(session.AdoptPreparedSimulationState(Prepare(session, directory)));
    const auto before = Bytes(session);
    std::stop_source stopped;
    const auto cancelled = Prepare(session, directory, stopped.get_token());
    stopped.request_stop();
    EXPECT_FALSE(session.AdoptPreparedSimulationState(cancelled));
    EXPECT_EQ(Bytes(session), before);
    EXPECT_FALSE(
        session.AdoptPreparedSimulationState(Prepare(session, directory, stopped.get_token())));
    EXPECT_EQ(Bytes(session), before);
    const auto resident = session.GetRegistry().create();
    session.GetRegistry().emplace<C::SimulationIdentityComponent>(resident, 4u);
    const auto with_resident = Bytes(session);
    EXPECT_FALSE(session.AdoptPreparedSimulationState(Prepare(session, directory)));
    EXPECT_EQ(Bytes(session), with_resident);
    session.GetRegistry().get<C::SimulationIdentityComponent>(resident).id = 2;
    const auto ground_collision = Bytes(session);
    EXPECT_FALSE(session.AdoptPreparedSimulationState(Prepare(session, directory)));
    EXPECT_EQ(Bytes(session), ground_collision);
    session.GetRegistry().destroy(resident);
    const auto stale = Prepare(session, directory);
    ASSERT_TRUE(session.CreateTransientWorld("Replacement", "1337", "default"));
    const auto replaced = Bytes(session);
    EXPECT_FALSE(session.AdoptPreparedSimulationState(stale));
    EXPECT_EQ(Bytes(session), replaced);
    auto document = nlohmann::json::parse([&] {
        std::ifstream input(Service::plant_entities_path(directory));
        return std::string(std::istreambuf_iterator<char>(input), {});
    }());
    document["entities"].back()["components"].back()["data"] = nlohmann::json::object();
    ASSERT_TRUE(
        Service::write_file_durably(Service::plant_entities_path(directory), document.dump()));
    const auto corrupt_before = Bytes(session);
    const auto corrupt = Prepare(session, directory);
    EXPECT_FALSE(W::GameSession::WorldPreparationError(*corrupt).empty());
    EXPECT_FALSE(session.AdoptPreparedSimulationState(corrupt));
    EXPECT_EQ(Bytes(session), corrupt_before);
}

TEST_P(SessionAdoption, AllocationRefusalIncludingLastAllocationLeavesSessionUnchanged) {
    W::GameSession session;
    Configure(session);
    ASSERT_TRUE(session.CreateWorld("Allocation", "1337", "default"));
    const auto directory = Publish({.next_id = 10, .entities = {Plant(1), Plant(4)}});
    // Warm the same roster/pool topology before measuring the cold transaction's allocations.
    ASSERT_TRUE(session.AdoptPreparedSimulationState(Prepare(session, directory)));
    const auto measured = Prepare(session, directory);
    bool adopted = false;
    std::size_t count = 0;
    {
        const AllocationBudget budget;
        adopted = session.AdoptPreparedSimulationState(measured);
        count = Allocations().attempts;
    }
    ASSERT_TRUE(adopted);
    ASSERT_GT(count, 2u);
    for (const auto cut : {std::size_t{0}, count / 2, count - 1}) {
        const auto prepared = Prepare(session, directory);
        const auto before = Bytes(session);
        {
            const AllocationBudget budget(cut);
            adopted = session.AdoptPreparedSimulationState(prepared);
        }
        EXPECT_FALSE(adopted) << cut << '/' << count;
        EXPECT_EQ(Bytes(session), before);
        ASSERT_TRUE(session.AdoptPreparedSimulationState(prepared));
    }
    std::stop_source stopped;
    const auto prepared = Prepare(session, directory, stopped.get_token());
    const auto before = Bytes(session);
    {
        const AllocationBudget budget(count - 1, &stopped);
        adopted = session.AdoptPreparedSimulationState(prepared);
    }
    EXPECT_TRUE(stopped.stop_requested());
    EXPECT_FALSE(adopted);
    EXPECT_EQ(Bytes(session), before);
}

TEST_P(SessionAdoption, InvalidMissingAndDuplicateResidentIdentitiesRefuseBeforeReplacement) {
    W::GameSession session;
    Configure(session);
    ASSERT_TRUE(session.CreateWorld("Residents", "1337", "default"));
    const auto directory = Publish({.next_id = 10, .entities = {Plant(1)}});
    ASSERT_TRUE(session.AdoptPreparedSimulationState(Prepare(session, directory)));
    for (unsigned invalid = 0; invalid < 5; ++invalid) {
        const auto first = session.GetRegistry().create();
        const auto second = session.GetRegistry().create();
        const std::uint64_t id = invalid == 0   ? 0
                                 : invalid == 1 ? W::ActiveRegionLedger::kPersistentIdLimit
                                 : invalid == 2 ? 10
                                                : 7;
        if (invalid == 4)
            session.GetRegistry().emplace<C::CreatureComponent>(first);
        else
            session.GetRegistry().emplace<C::SimulationIdentityComponent>(first, id);
        if (invalid == 3)
            session.GetRegistry().emplace<C::SimulationIdentityComponent>(second, id);
        const auto before = Bytes(session);
        EXPECT_FALSE(session.AdoptPreparedSimulationState(Prepare(session, directory))) << invalid;
        EXPECT_EQ(Bytes(session), before);
        session.GetRegistry().destroy(first);
        session.GetRegistry().destroy(second);
    }
}

TEST_P(SessionAdoption, NotificationsReplaceTogetherAndKeepUnrelatedPublicationsAndSubscribers) {
    W::GameSession session;
    Configure(session);
    ASSERT_TRUE(session.CreateWorld("Notifications", "1337", "default"));
    const auto pending = [](std::uint64_t sequence) {
        return W::GroundNotifications{.next_sequence = sequence + 1,
                                      .events = {{.tick = 0,
                                                  .lane = 0,
                                                  .sequence = sequence,
                                                  .topic = "ground.spawned",
                                                  .payload = "restored"}}};
    };
    session.GetSimulationEventBus().publish(0, "unrelated", "retained");
    const auto collision = Publish({.next_id = 10, .entities = {Plant(1)}}, true, pending(0));
    const auto before = Bytes(session);
    EXPECT_FALSE(session.AdoptPreparedSimulationState(Prepare(session, collision)));
    EXPECT_EQ(Bytes(session), before);
    const auto directory = Publish({.next_id = 10, .entities = {Plant(1)}}, true, pending(11));
    std::vector<std::string> delivered;
    session.GetSimulationEventBus().subscribe(
        [&delivered](const auto& event) { delivered.push_back(event.payload); });
    ASSERT_TRUE(session.AdoptPreparedSimulationState(Prepare(session, directory)));
    EXPECT_EQ(session.GetSimulationEventBus().next_sequence(), 12u);
    const auto events = session.GetSimulationEventBus().drain(0);
    EXPECT_EQ(events.size(), 2u);
    EXPECT_EQ(delivered, (std::vector<std::string>{"retained", "restored"}));
    const auto empty =
        Publish({.next_id = W::ActiveRegionLedger::kPersistentIdLimit, .entities = {}}, false);
    ASSERT_TRUE(session.AdoptPreparedSimulationState(Prepare(session, empty)));
    EXPECT_EQ(session.GetActiveRegionLedger().next_persistent_id(),
              W::ActiveRegionLedger::kPersistentIdLimit);
    auto exhausted = session.GetActiveRegionLedger();
    EXPECT_FALSE(Luminumbra::simulation::AllocateSimulationIdentity(exhausted).has_value());
}

TEST_P(SessionAdoption, ShippingV1PreparationAndAdoptionRemainSelected) {
    W::GameSession session;
    Configure(session);
    ASSERT_TRUE(session.CreateWorld("Shipping", "1337", "default"));
    ASSERT_TRUE(session.SaveWorld());
    const auto prepared = W::GameSession::PrepareWorldState(
        session.CaptureWorldPreparationOptions(), session.GetWorldSaveDir());
    EXPECT_FALSE(session.AdoptPreparedSimulationState(prepared));
    EXPECT_TRUE(session.AdoptPreparedWorld(prepared));
    EXPECT_FALSE(P::PlantSnapshotContract::selected());
}

INSTANTIATE_TEST_SUITE_P(RegionsEnabledAndDisabled, SessionAdoption, testing::Bool());
} // namespace
