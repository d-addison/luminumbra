#include "RegionPopulator.h"
#include "ai/CreaturePersistence.h"
#include "components/CircadianComponents.h"
#include "components/CoreComponents.h"
#include "components/CreatureComponents.h"
#include "components/InstinctComponents.h"
#include "persistence/PlantPersistence.h"
#include <algorithm>
#include <cmath>
#include <limits>
#include <set>
#include <stdexcept>
#include <tuple>
#include <type_traits>
#include <utility>

namespace Luminumbra::world {
namespace {
namespace C = Components;
namespace A = luminumbra::ai;
using PopulationComponents = std::tuple<C::TransformComponent,
                                        C::CreatureComponent,
                                        C::CreatureGenomeComponent,
                                        C::PerceptionComponent,
                                        C::CircadianComponent,
                                        C::CreatureRegionComponent,
                                        C::CreatureCoarseComponent,
                                        C::CreatureControllerComponent,
                                        C::CreatureLocomotionStateComponent>;
template<class Fn, class... Types>
void ForComponents(Fn&& fn, std::tuple<Types...>*) {
    (fn.template operator()<Types>(), ...);
}
void CloneEntities(const entt::registry& live, entt::registry& stage) {
    const auto* original = live.storage<entt::entity>();
    if (!original)
        throw std::runtime_error("Missing entity storage");
    auto& entities = stage.storage<entt::entity>();
    for (std::size_t i = 0; i < original->size(); ++i)
        entities.push(original->data()[i]);
    entities.free_list(original->free_list());
}
// Only these data pools can be written by the engine population factory. The original
// mixins, signals, context, all other pools and runtime attachments stay in the live registry.
void CloneComponents(entt::registry& live, entt::registry& stage) {
    ForComponents(
        [&]<class T> {
            auto& source = live.storage<T>();
            auto& target = stage.storage<T>();
            // EnTT installs destruction listeners for both owning and non-owning groups,
            // including excluded types. Pool exchange cannot update those cached memberships.
            // Refuse conservatively before staging rather than publish a stale group. Ordinary
            // construction observers remain connected and are not invoked by staging/commit.
            if (live.owned<T>() || !source.on_destroy().empty())
                throw std::logic_error(
                    "Population requires component pools without groups or destruction listeners");
            for (std::size_t i = 0; i < source.size(); ++i) {
                const auto entity = source.data()[i];
                target.emplace(entity, source.get(entity));
            }
        },
        static_cast<PopulationComponents*>(nullptr));
}
void CommitComponents(entt::registry& live, entt::registry& stage) noexcept {
    // All pools were assured during CloneComponents, before the last failure boundary.
    ForComponents(
        [&]<class T> {
            using Storage = entt::basic_storage<T, entt::entity, std::allocator<T>>;
            static_assert(noexcept(std::declval<Storage&>().swap(std::declval<Storage&>())));
            static_cast<Storage&>(live.storage<T>())
                .swap(static_cast<Storage&>(stage.storage<T>()));
        },
        static_cast<PopulationComponents*>(nullptr));
    // Entity storage also owns its generation/recycling state. Moving the complete storage
    // carries that state, including its next-unused cursor, without emitting signals.
    using EntityStorage =
        entt::basic_storage<entt::entity, entt::entity, std::allocator<entt::entity>>;
    static_assert(std::is_nothrow_move_assignable_v<EntityStorage>);
    static_cast<EntityStorage&>(live.storage<entt::entity>()) =
        std::move(static_cast<EntityStorage&>(stage.storage<entt::entity>()));
}
std::int64_t Millimetres(float value, std::int64_t low, std::int64_t high) {
    const auto rounded = std::round(static_cast<double>(value) * 1000.0);
    if (!std::isfinite(rounded) || rounded < static_cast<double>(low) ||
        rounded > static_cast<double>(high))
        throw std::invalid_argument("Invalid population position");
    return static_cast<std::int64_t>(rounded);
}
GroundObject Quantized(Vec3 position) {
    GroundObject result;
    result.x_mm = Millimetres(position.x,
                              -GroundObjectStore::kMaxCoordinateMm + 1,
                              GroundObjectStore::kMaxCoordinateMm - 1);
    result.y_mm = Millimetres(
        position.y, -GroundObjectStore::kMaxVerticalMm + 1, GroundObjectStore::kMaxVerticalMm - 1);
    result.z_mm = Millimetres(position.z,
                              -GroundObjectStore::kMaxCoordinateMm + 1,
                              GroundObjectStore::kMaxCoordinateMm - 1);
    return result;
}
void ValidateOwner(Vec3 position, RegionKey key) {
    GroundObject rounded;
    rounded.x_mm = Millimetres(
        position.x, -GroundObjectStore::kMaxCoordinateMm, GroundObjectStore::kMaxCoordinateMm - 1);
    rounded.y_mm = Millimetres(
        position.y, -GroundObjectStore::kMaxVerticalMm, GroundObjectStore::kMaxVerticalMm - 1);
    rounded.z_mm = Millimetres(
        position.z, -GroundObjectStore::kMaxCoordinateMm, GroundObjectStore::kMaxCoordinateMm - 1);
    if (!ActiveRegionLedger::valid_anchor(position) ||
        ActiveRegionLedger::region_at(position) != key ||
        GroundObjectStore::region_of(rounded) != key)
        throw std::invalid_argument("Population command outside requested owner");
}
} // namespace

void RegionSpawnSink::append(RegionSpawnCommand command) {
    if (m_commands.size() >= GroundObjectStore::kMaxObjects + A::kMaxCreatures)
        throw std::length_error("Population command capacity reached");
    m_commands.push_back(std::move(command));
}
void RegionSpawnSink::spawn(CreatureSpawnCommand command) {
    append(std::move(command));
}
void RegionSpawnSink::spawn(GroundObjectSpawnCommand command) {
    append(std::move(command));
}
std::string RegionPopulator::identity_configuration() const {
    std::string bytes =
        std::to_string(canonical_configuration.size()) + ":" + canonical_configuration;
    for (const auto& [type, maximum] : ground_types)
        bytes += ";" + std::to_string(type) + "," + std::to_string(maximum);
    return bytes;
}
struct RegionPopulationTransaction::Candidate {
    ActiveRegionLedger& live_ledger;
    GroundObjectStore& live_ground;
    entt::registry& live_registry;
    luminumbra::simulation::OrderedEventBus& live_events;
    std::vector<RegionWork>& live_work;
    ActiveRegionLedger ledger;
    GroundObjectStore ground;
    entt::registry registry;
    std::vector<RegionWork> work;
    std::vector<luminumbra::simulation::SimulationEvent> events;
    Candidate(ActiveRegionLedger& l,
              GroundObjectStore& g,
              entt::registry& r,
              luminumbra::simulation::OrderedEventBus& e,
              std::vector<RegionWork>& w)
        : live_ledger(l)
        , live_ground(g)
        , live_registry(r)
        , live_events(e)
        , live_work(w)
        , ledger(l)
        , ground(g)
        , work(w) {}
};
RegionPopulationTransaction::RegionPopulationTransaction(
    ActiveRegionLedger& ledger,
    GroundObjectStore& ground,
    entt::registry& registry,
    luminumbra::simulation::OrderedEventBus& events,
    std::vector<RegionWork>& work)
    : m_candidate(std::make_unique<Candidate>(ledger, ground, registry, events, work)) {}
RegionPopulationTransaction::~RegionPopulationTransaction() = default;
void RegionPopulationTransaction::execute(const RegionPopulator& populator,
                                          RegionKey key,
                                          std::uint64_t tick,
                                          std::uint64_t seed,
                                          RegionBiomeInformation biome,
                                          const CreatureFactory& factory,
                                          const PopulationPreparationHooks& hooks) {
    auto& next = *m_candidate;
    const auto owner = next.ledger.records().find(key);
    if (!populator.populate || populator.contract == kDefaultPopulationContract || tick == 0 ||
        tick != next.ledger.tick() || owner == next.ledger.records().end() ||
        owner->second.state != RegionState::Active || owner->second.populated)
        throw std::invalid_argument("Population request is not eligible");
    RegionSpawnSink sink;
    const RegionRng rng(seed, key, RegionRngTag::Population, tick);
    populator.populate(RegionPopulateRequest{key, tick, rng, biome, sink});
    const bool creatures =
        std::any_of(sink.commands().begin(), sink.commands().end(), [](const auto& command) {
            return std::holds_alternative<CreatureSpawnCommand>(command);
        });
    auto snapshot = A::BuildCreatureSnapshot(next.live_registry);
    if (creatures) {
        CloneEntities(next.live_registry, next.registry);
        CloneComponents(next.live_registry, next.registry);
    }
    const auto first_id = next.ledger.next_persistent_id();
    std::uint64_t index = 0;
    for (const auto& command : sink.commands()) {
        std::visit(
            [&](const auto& spawn) {
                ValidateOwner(spawn.position, key);
                std::uint64_t id = 0;
                if (!next.ledger.allocate_persistent_id(id))
                    throw std::length_error("Population identity capacity reached");
                using T = std::decay_t<decltype(spawn)>;
                if constexpr (std::is_same_v<T, CreatureSpawnCommand>) {
                    if (!factory)
                        throw std::invalid_argument("Missing population creature factory");
                    factory(next.registry, spawn, id, key, tick, rng.draw(index));
                    if (hooks.entity)
                        hooks.entity();
                } else {
                    const auto type = populator.ground_types.find(spawn.type);
                    if (spawn.type == 0 || type == populator.ground_types.end() ||
                        spawn.payload > type->second)
                        throw std::invalid_argument("Unknown population object type or payload");
                    auto object = Quantized(spawn.position);
                    object.id = id;
                    object.type = spawn.type;
                    object.spawn_tick = tick;
                    object.despawn_after = spawn.despawn_after;
                    object.payload = spawn.payload;
                    if (!next.ground.insert(object))
                        throw std::length_error("Population object capacity reached");
                    next.events.push_back({tick,
                                           0,
                                           0,
                                           "ground.spawned",
                                           std::to_string(id) + "," + std::to_string(object.type) +
                                               "," + std::to_string(object.x_mm) + "," +
                                               std::to_string(object.y_mm) + "," +
                                               std::to_string(object.z_mm)});
                }
            },
            command);
        ++index;
    }
    if (creatures) {
        const auto staged = A::BuildCreatureSnapshot(next.registry);
        for (const auto& record : staged.entities)
            if (record.id >= first_id)
                snapshot.entities.push_back(record);
    }
    std::sort(snapshot.entities.begin(), snapshot.entities.end(), [](const auto& a, const auto& b) {
        return a.id < b.id;
    });
    std::string error;
    const auto plants = luminumbra::foliage::BuildPlantEntitySnapshot(next.live_registry, true);
    std::set<std::uint64_t> creature_ids;
    for (const auto& record : snapshot.entities)
        creature_ids.insert(record.id);
    if (!luminumbra::foliage::ValidatePlantEntitySnapshot(
            plants, next.ledger, next.ground, error, creature_ids) ||
        !A::ValidateCreatureSnapshot(snapshot, next.ledger, next.ground, plants, error))
        throw std::invalid_argument(error);
    // Exercise the same aggregate byte/node envelope used by the writer and loader.
    const auto bytes = A::SerializeCreatureSnapshot(snapshot);
    A::CreatureSnapshot checked_creatures;
    if (!A::LoadCreatureSnapshot(bytes, checked_creatures, error))
        throw std::invalid_argument(error);
    next.ledger.set_populated(key, true);
    if (!sink.commands().empty())
        next.work.push_back(RegionWork{key, 0, sink.commands().size(), 0, 0});
    ActiveRegionLedger checked;
    if (!ActiveRegionLedger::decode(next.ledger.encode(), checked, error))
        throw std::invalid_argument(error);
    auto events = next.live_events.prepare_batch(next.events, hooks.events);
    static_assert(std::is_nothrow_move_assignable_v<ActiveRegionLedger>);
    static_assert(std::is_nothrow_move_assignable_v<GroundObjectStore>);
    // No callbacks, allocations, entity construction or signal publication below this point.
    if (creatures)
        CommitComponents(next.live_registry, next.registry);
    next.live_ledger = std::move(next.ledger);
    next.live_ground = std::move(next.ground);
    next.live_work.swap(next.work);
    next.live_events.commit_batch(std::move(events));
}
} // namespace Luminumbra::world
