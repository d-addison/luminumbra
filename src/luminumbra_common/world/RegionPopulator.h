#pragma once

#include "GroundObjectStore.h"
#include "RegionRng.h"
#include "simulation/SimulationEventBus.h"
#include <entt/entt.hpp>
#include <functional>
#include <map>
#include <memory>
#include <span>
#include <string>
#include <variant>
#include <vector>

namespace Luminumbra::world {

struct CreatureSpawnCommand {
    std::uint16_t species_id = 0;
    Vec3 position{};
    bool physics_controlled = true;
};
struct GroundObjectSpawnCommand {
    std::uint32_t type = 0;
    Vec3 position{};
    std::uint32_t despawn_after = 0, payload = 0;
};
using RegionSpawnCommand = std::variant<CreatureSpawnCommand, GroundObjectSpawnCommand>;
class RegionSpawnSink {
public:
    void spawn(CreatureSpawnCommand command);
    void spawn(GroundObjectSpawnCommand command);
    [[nodiscard]] std::span<const RegionSpawnCommand> commands() const noexcept {
        return m_commands;
    }

private:
    void append(RegionSpawnCommand command);
    std::vector<RegionSpawnCommand> m_commands;
};
// A deterministic surface sample at the region centre, independent of chunk residency.
struct RegionBiomeInformation {
    std::uint8_t surface_id = 255;
    bool operator==(const RegionBiomeInformation&) const = default;
};
struct RegionPopulateRequest {
    const RegionKey key;
    const std::uint64_t absolute_tick;
    const RegionRng rng;
    const RegionBiomeInformation biome;
    RegionSpawnSink& sink;
};
struct RegionPopulator {
    std::string contract;
    std::string canonical_configuration;
    // Registered types and inclusive payload ceilings. No unregistered object type is admitted.
    std::map<std::uint32_t, std::uint32_t> ground_types;
    std::function<void(const RegionPopulateRequest&)> populate;
    [[nodiscard]] std::string identity_configuration() const;
};
struct PopulationPreparationHooks {
    std::function<void()> entity, events;
};
// One region is one transaction. All throwing work occurs before publication. New component
// pools are prepared privately; publication exchanges data without firing live EnTT signals.
class RegionPopulationTransaction {
public:
    using CreatureFactory = std::function<void(entt::registry&,
                                               const CreatureSpawnCommand&,
                                               std::uint64_t,
                                               RegionKey,
                                               std::uint64_t,
                                               std::uint64_t)>;
    RegionPopulationTransaction(ActiveRegionLedger& ledger,
                                GroundObjectStore& ground,
                                entt::registry& registry,
                                luminumbra::simulation::OrderedEventBus& events,
                                std::vector<RegionWork>& work);
    ~RegionPopulationTransaction();
    RegionPopulationTransaction(const RegionPopulationTransaction&) = delete;
    RegionPopulationTransaction& operator=(const RegionPopulationTransaction&) = delete;
    void execute(const RegionPopulator& populator,
                 RegionKey key,
                 std::uint64_t tick,
                 std::uint64_t seed,
                 RegionBiomeInformation biome,
                 const CreatureFactory& factory,
                 const PopulationPreparationHooks& hooks = {});

private:
    struct Candidate;
    std::unique_ptr<Candidate> m_candidate;
};

} // namespace Luminumbra::world
