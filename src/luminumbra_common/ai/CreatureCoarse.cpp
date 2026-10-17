#include "CreatureCoarse.h"

#include "world/RegionRng.h"

#include <algorithm>
#include <limits>
#include <map>
#include <set>
#include <stdexcept>
#include <string>

namespace luminumbra::ai {
namespace {
namespace C = Luminumbra::Components;
using Luminumbra::world::RegionKey;
using Luminumbra::world::RegionRng;
using Luminumbra::world::RegionRngTag;
using Luminumbra::world::RegionState;
constexpr std::int64_t kHorizontalMm = 16777216000ll;
constexpr std::int64_t kVerticalMm = 1048576000ll;

void Require(bool condition) {
    if (!condition)
        throw std::invalid_argument("Invalid coarse creature snapshot.");
}

bool ValidKey(RegionKey key) {
    return key.x >= -32768 && key.x < 32768 && key.z >= -32768 && key.z < 32768;
}

std::int64_t FloorRegion(std::int64_t mm) {
    return mm / kCreatureRegionMm - (mm < 0 && mm % kCreatureRegionMm != 0 ? 1 : 0);
}

template<typename T>
T Increment(T value) {
    return value == std::numeric_limits<T>::max() ? value : value + 1;
}

std::uint32_t AddNeed(std::uint32_t value, std::uint32_t rate) {
    return value + std::min(rate, kCoarsePpm - value);
}

RegionKey Owner(const CoarseCreature& creature) {
    return {creature.region.owner_x, creature.region.owner_z};
}

void Validate(const CoarseCreature& creature, std::uint64_t tick) {
    const auto& c = creature.coarse;
    Require(creature.region.persistent_id > 0 &&
            creature.region.persistent_id < (std::uint64_t{1} << 63) &&
            creature.region.region_stamp <= tick && ValidKey(Owner(creature)) &&
            creature.species_id != 0 && creature.stamina_ppm <= kCoarsePpm &&
            creature.mate_hunger_limit_ppm <= kCoarsePpm && static_cast<unsigned>(c.tier) <= 1 &&
            static_cast<unsigned>(c.transition) <= 2 && c.y_mm >= -kVerticalMm &&
            c.y_mm < kVerticalMm && c.hunger_ppm <= kCoarsePpm && c.thirst_ppm <= kCoarsePpm &&
            ValidKey({c.destination_x, c.destination_z}));
    const auto key = CreatureRegionAtMillimetres(c.x_mm, c.z_mm);
    Require(c.tier != C::CreatureTier::Coarse || key == Owner(creature));
    Require(c.needs.size() <= 256);
    std::set<std::string> names;
    for (const auto& need : c.needs)
        Require(!need.name.empty() && need.name.size() <= 1024 && need.pressure_ppm <= kCoarsePpm &&
                names.insert(need.name).second);
}

bool Accept(const CoarseCreature& creature,
            const std::set<RegionKey>& due,
            const CoarseRegionRecords& regions,
            std::uint64_t tick) {
    const auto stamp = creature.region.region_stamp;
    if (!creature.alive || creature.coarse.tier != C::CreatureTier::Coarse || stamp >= tick ||
        !due.contains(Owner(creature)))
        return false;
    // Region phases choose the due tick within an absolute cadence window.
    // Reinterpret the persisted last tick at the current owner's shift, never
    // move the stamp into the future or suppress the next cadence window.
    const auto shift = regions.at(Owner(creature)).cadence_shift;
    return stamp == 0 || (stamp >> shift) < (tick >> shift);
}

bool Ready(const CoarseCreature& creature) {
    const auto& c = creature.coarse;
    return creature.has_genome && !creature.is_predator &&
           creature.policy.reproduce_cadences != 0 && c.age_cadences >= kCoarseMaturityCadences &&
           c.reproduce_cadences >= creature.policy.reproduce_cadences &&
           c.reproduce_cooldown_cadences == 0 && c.hunger_ppm <= creature.mate_hunger_limit_ppm &&
           c.thirst_ppm <= kCoarseMateThirstLimitPpm &&
           creature.stamina_ppm >= kCoarseHealthyStaminaPpm;
}

// Cardinal travel checks every crossed owner, so a large policy rate can never
// jump over a frozen/absent region. World edges are sealed without an invalid key.
void Travel(CoarseCreature& creature,
            const CoarseRegionRecords& regions,
            std::uint64_t seed,
            std::uint64_t tick) {
    const auto metres = creature.policy.coarse_travel_m_per_cadence;
    if (metres == 0)
        return;
    Require(static_cast<std::uint64_t>(metres) <=
            static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) / 1000);
    const auto distance = static_cast<std::int64_t>(metres) * 1000;
    auto& c = creature.coarse;
    auto key = Owner(creature);
    unsigned direction;
    if (c.travel_pending && c.destination_x != key.x)
        direction = c.destination_x > key.x ? 0 : 1;
    else if (c.travel_pending && c.destination_z != key.z)
        direction = c.destination_z > key.z ? 2 : 3;
    else
        direction = static_cast<unsigned>(
            RegionRng(seed, key, RegionRngTag::CreatureTravel, tick, creature.region.persistent_id)
                .draw(0) &
            3);
    const bool x_axis = direction < 2;
    const bool positive = (direction & 1) == 0;
    auto& position = x_axis ? c.x_mm : c.z_mm;
    // Check the add/subtract by remaining world space before performing it.
    const auto available = positive ? kHorizontalMm - 1 - position : position + kHorizontalMm;
    const auto travel = std::min(distance, available);
    const auto target = positive ? position + travel : position - travel;
    c.travel_pending = false;
    while (true) {
        const auto coordinate = x_axis ? key.x : key.z;
        const auto boundary =
            (static_cast<std::int64_t>(coordinate) + (positive ? 1 : 0)) * kCreatureRegionMm;
        if (positive ? target < boundary : target >= boundary) {
            position = target;
            break;
        }
        auto destination = key;
        auto& next_coordinate = x_axis ? destination.x : destination.z;
        next_coordinate += positive ? 1 : -1;
        const auto found = regions.find(destination);
        if (!ValidKey(destination) || found == regions.end() ||
            found->second.state == RegionState::Frozen) {
            position = positive ? boundary - 1 : boundary;
            c.travel_pending = ValidKey(destination);
            c.destination_x = destination.x;
            c.destination_z = destination.z;
            break;
        }
        key = destination;
    }
    creature.region.owner_x = key.x;
    creature.region.owner_z = key.z;
    if (!c.travel_pending) {
        c.destination_x = key.x;
        c.destination_z = key.z;
    }
}
} // namespace

Luminumbra::world::RegionKey CreatureRegionAtMillimetres(std::int64_t x, std::int64_t z) {
    Require(x >= -kHorizontalMm && x < kHorizontalMm && z >= -kHorizontalMm && z < kHorizontalMm);
    return {static_cast<std::int32_t>(FloorRegion(x)), static_cast<std::int32_t>(FloorRegion(z))};
}

CoarseStepResult StepCoarseCreatures(std::span<const CoarseCreature> previous,
                                     std::span<const RegionKey> due,
                                     const CoarseRegionRecords& regions,
                                     std::uint64_t world_seed,
                                     std::uint64_t tick) {
    Require(previous.size() <= 16384);
    const std::set<RegionKey> eligible(due.begin(), due.end());
    for (const auto& [key, record] : regions) {
        Require(ValidKey(key) && static_cast<unsigned>(record.state) <= 2 &&
                record.cadence_shift <= 16 &&
                record.cadence_phase < (std::uint32_t{1} << record.cadence_shift));
    }
    for (auto key : eligible) {
        const auto found = regions.find(key);
        Require(found != regions.end() && found->second.state != RegionState::Frozen);
        const auto& record = found->second;
        Require((tick & ((std::uint64_t{1} << record.cadence_shift) - 1)) == record.cadence_phase);
    }
    CoarseStepResult result;
    result.creatures.assign(previous.begin(), previous.end());
    auto& next = result.creatures;
    std::sort(next.begin(), next.end(), [](const auto& a, const auto& b) {
        return a.region.persistent_id < b.region.persistent_id;
    });
    for (std::size_t i = 0; i < next.size(); ++i) {
        Validate(next[i], tick);
        Require(i == 0 || next[i - 1].region.persistent_id != next[i].region.persistent_id);
    }
    // Keep the complete read set sealed across needs, births and transfers.
    const auto snapshot = next;
    std::map<RegionKey, std::uint64_t> work;
    std::vector<bool> accepted(next.size(), false), paired(next.size(), false);
    for (std::size_t i = 0; i < next.size(); ++i) {
        auto& creature = next[i];
        if (!Accept(snapshot[i], eligible, regions, tick))
            continue;
        accepted[i] = true;
        ++work[Owner(snapshot[i])];
        creature.region.region_stamp = tick;
        auto& c = creature.coarse;
        c.age_cadences = Increment(c.age_cadences);
        c.reproduce_cadences = Increment(c.reproduce_cadences);
        if (c.reproduce_cooldown_cadences != 0)
            --c.reproduce_cooldown_cadences;
        c.courting_cadences = 0;
        c.hunger_ppm = AddNeed(c.hunger_ppm, creature.policy.coarse_need_rate_ppm);
        c.thirst_ppm = AddNeed(c.thirst_ppm, creature.policy.coarse_need_rate_ppm);
        for (auto& need : c.needs)
            need.pressure_ppm = AddNeed(need.pressure_ppm, creature.policy.coarse_need_rate_ppm);
        if (creature.policy.lifespan_cadences != 0 &&
            c.age_cadences >= creature.policy.lifespan_cadences) {
            creature.alive = false;
            result.deaths.push_back(creature.region.persistent_id);
        }
    }

    // Female PersistentId order, nearest unused same-species male, PersistentId
    // tie break. Neither needs accrued above nor movement below changes selection.
    for (std::size_t f = 0; f < snapshot.size(); ++f) {
        const auto& mother = snapshot[f];
        if (!accepted[f] || !next[f].alive || !mother.female || !Ready(mother))
            continue;
        auto best = snapshot.size();
        auto best_distance = kCoarseCourtshipRadiusMm * kCoarseCourtshipRadiusMm + 1;
        for (std::size_t m = 0; m < snapshot.size(); ++m) {
            const auto& father = snapshot[m];
            if (!accepted[m] || !next[m].alive || paired[m] || father.female || !Ready(father) ||
                father.species_id != mother.species_id || Owner(father) != Owner(mother))
                continue;
            const auto dx = father.coarse.x_mm - mother.coarse.x_mm;
            const auto dz = father.coarse.z_mm - mother.coarse.z_mm;
            if (dx < -kCoarseCourtshipRadiusMm || dx > kCoarseCourtshipRadiusMm ||
                dz < -kCoarseCourtshipRadiusMm || dz > kCoarseCourtshipRadiusMm)
                continue;
            const auto distance = dx * dx + dz * dz;
            if (distance < best_distance) {
                best = m;
                best_distance = distance;
            }
        }
        if (best == snapshot.size())
            continue;
        paired[best] = true;
        auto& female = next[f].coarse;
        female.courting_cadences = Increment(mother.coarse.courting_cadences);
        if (female.courting_cadences < kCoarseCourtshipCadences)
            continue;
        const auto& father = snapshot[best];
        const RegionRng rng(world_seed,
                            Owner(mother),
                            RegionRngTag::CreatureReproduction,
                            tick,
                            mother.region.persistent_id);
        const auto seed = rng.draw(father.region.persistent_id);
        result.births.push_back({mother.region.persistent_id,
                                 father.region.persistent_id,
                                 seed,
                                 Owner(mother),
                                 mother.coarse.x_mm,
                                 mother.coarse.y_mm,
                                 mother.coarse.z_mm,
                                 mother.species_id,
                                 Increment(std::max(mother.generation, father.generation)),
                                 (luminumbra::core::DeterministicRng::mix(seed) & 1) == 0});
        for (auto index : {f, best}) {
            auto& state = next[index].coarse;
            state.reproduce_cadences = 0;
            state.reproduce_cooldown_cadences = snapshot[index].policy.reproduce_cadences;
            state.courting_cadences = 0;
        }
    }

    for (std::size_t i = 0; i < next.size(); ++i)
        if (accepted[i] && next[i].alive)
            Travel(next[i], regions, world_seed, tick);
    for (const auto& [key, updates] : work)
        result.work.push_back({.key = key, .entity_updates = updates});
    return result;
}
} // namespace luminumbra::ai
