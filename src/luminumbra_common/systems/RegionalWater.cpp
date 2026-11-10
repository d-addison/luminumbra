#include "RegionalWater.h"

#include "../world/Chunk.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <stdexcept>

namespace Luminumbra::Systems::RegionalWater {
namespace {
using world::ActiveRegionLedger;
using world::RegionKey;
using world::RegionState;

[[noreturn]] void Refuse() {
    throw std::invalid_argument("Regional water planning input refused");
}

bool LegalResolution(int resolution) {
    return resolution == 4 || resolution == 8 || resolution == 16 || resolution == 32;
}

struct Entry {
    Resident resident{};
    IVec3 coords{};
    RegionKey owner{};
    const world::ActiveRegionRecord* record = nullptr;
    bool admitted = false, preparation = false, source = false, selected = false;
};
struct Group {
    const world::ActiveRegionRecord* record = nullptr;
    bool ready = true, due = false;
    std::vector<std::size_t> candidates;
};
using Groups = std::map<RegionKey, Group>;

bool OnPhase(std::uint64_t tick, const world::ActiveRegionRecord& record) {
    return tick % (std::uint64_t{1} << record.cadence_shift) == record.cadence_phase;
}

bool Awake(const Entry& entry) {
    return entry.resident.initialized && entry.resident.awake;
}

const world::ActiveRegionRecord* FindRecord(const ActiveRegionLedger& ledger, RegionKey owner) {
    const auto found = ledger.records().find(owner);
    if (found == ledger.records().end()) {
        return nullptr;
    }
    const auto& record = found->second;
    if (record.cadence_shift > 16 ||
        record.cadence_phase >= (std::uint32_t{1} << record.cadence_shift) ||
        (record.state != RegionState::Active && record.state != RegionState::Reduced &&
         record.state != RegionState::Frozen)) {
        Refuse();
    }
    return &record;
}

Entry* Find(std::vector<Entry>& entries, const IVec3& coords) {
    // Packed coordinates must never wrap onto the opposite world boundary.
    if (coords.x < -1048576 || coords.x >= 1048576 || coords.z < -1048576 || coords.z >= 1048576) {
        return nullptr;
    }
    const auto id = Chunk::calculate_id(coords);
    const auto found =
        std::lower_bound(entries.begin(), entries.end(), id, [](const auto& e, ChunkID key) {
            return e.resident.id < key;
        });
    return found == entries.end() || found->resident.id != id ? nullptr : &*found;
}

std::vector<Entry> Admit(std::span<const Resident> residents,
                         const ActiveRegionLedger& ledger,
                         const Inputs& inputs,
                         Groups& groups,
                         std::size_t& visits) {
    std::vector<Entry> entries;
    entries.reserve(residents.size());
    for (const auto& resident : residents) {
        ++visits;
        if (resident.initialized && !LegalResolution(resident.resolution)) {
            Refuse();
        }
        Entry entry;
        entry.resident = resident;
        entry.coords = Chunk::decode_id(resident.id);
        entry.owner =
            ActiveRegionLedger::region_at({static_cast<float>(entry.coords.x * CHUNK_SIZE_X),
                                           0.0f,
                                           static_cast<float>(entry.coords.z * CHUNK_SIZE_Z)});
        entry.record = FindRecord(ledger, entry.owner);
        entry.admitted = resident.available && entry.record != nullptr &&
                         entry.record->state != RegionState::Frozen;
        entry.preparation = !resident.initialized || resident.resolution != inputs.resolution;
        if (entry.admitted) {
            auto& group = groups[entry.owner];
            group.record = entry.record;
            group.ready = group.ready && (!entry.preparation || resident.preparation_ready);
        }
        entries.push_back(entry);
    }
    std::sort(entries.begin(), entries.end(), [](const auto& a, const auto& b) {
        return a.resident.id < b.resident.id;
    });
    for (std::size_t i = 1; i < entries.size(); ++i) {
        if (entries[i - 1].resident.id == entries[i].resident.id) {
            Refuse();
        }
    }
    for (auto& entry : entries) {
        if (entry.admitted && !groups.at(entry.owner).ready) {
            entry.admitted = false;
        }
    }
    return entries;
}

std::int32_t SourceAmount(const SourceInput& source, int resolution) {
    const auto& position = source.position;
    if (!std::isfinite(position.y) || !ActiveRegionLedger::valid_anchor(position) ||
        static_cast<double>(position.y) < -2097152.0 * CHUNK_SIZE_Y ||
        static_cast<double>(position.y) >= 2097152.0 * CHUNK_SIZE_Y) {
        Refuse();
    }
    const float flow = source.physical.flow_rate;
    const float cell_area = (static_cast<float>(CHUNK_SIZE_X) / static_cast<float>(resolution)) *
                            (static_cast<float>(CHUNK_SIZE_Z) / static_cast<float>(resolution));
    const float quantized = flow * SECONDS_PER_TICK / cell_area * 1000.0f + 0.5f;
    if (!std::isfinite(flow) || flow < 0.0f || !std::isfinite(quantized) ||
        static_cast<double>(quantized) > std::numeric_limits<std::int32_t>::max()) {
        Refuse();
    }
    return static_cast<std::int32_t>(quantized);
}

void PlanSources(std::span<const SourceInput> sources,
                 int resolution,
                 std::vector<Entry>& entries,
                 Plan& plan) {
    // IDs are validated even when a source has no resident destination.
    std::vector<std::uint64_t> ids;
    ids.reserve(sources.size());
    for (const auto& source : sources) {
        ids.push_back(source.id);
        const auto amount = SourceAmount(source, resolution);
        const auto& position = source.position;
        IVec3 coords{static_cast<int>(std::floor(static_cast<double>(position.x) / CHUNK_SIZE_X)),
                     static_cast<int>(std::floor(static_cast<double>(position.y) / CHUNK_SIZE_Y)),
                     static_cast<int>(std::floor(static_cast<double>(position.z) / CHUNK_SIZE_Z))};
        auto* target = Find(entries, coords);
        if (!target || !target->admitted || !target->resident.initialized) {
            coords.y = 0;
            target = Find(entries, coords);
        }
        if (!target || !target->admitted || amount == 0) {
            continue;
        }
        // Floor before subtracting the integer origin: a tiny negative position
        // can otherwise round up to the chunk width when the origin is added.
        const auto x = static_cast<int>(std::floor(static_cast<double>(position.x) * resolution /
                                                   CHUNK_SIZE_X)) -
                       coords.x * resolution;
        const auto z = static_cast<int>(std::floor(static_cast<double>(position.z) * resolution /
                                                   CHUNK_SIZE_Z)) -
                       coords.z * resolution;
        if (x < 0 || x >= resolution || z < 0 || z >= resolution) {
            Refuse();
        }
        plan.sources.push_back(
            {target->resident.id, static_cast<std::size_t>(z * resolution + x), amount});
        target->source = true;
    }
    std::sort(ids.begin(), ids.end());
    if (std::adjacent_find(ids.begin(), ids.end()) != ids.end()) {
        Refuse();
    }
    std::sort(plan.sources.begin(), plan.sources.end());
}

void PlanCandidates(std::vector<Entry>& entries,
                    Groups& groups,
                    const ActiveRegionLedger& ledger,
                    const world::RegionSchedule& schedule,
                    bool raining) {
    auto due = schedule.due;
    std::sort(due.begin(), due.end());
    if (std::adjacent_find(due.begin(), due.end()) != due.end()) {
        Refuse();
    }
    for (const auto owner : due) {
        if (owner.x < -32768 || owner.x >= 32768 || owner.z < -32768 || owner.z >= 32768) {
            Refuse();
        }
        const auto* record = FindRecord(ledger, owner);
        if (!record || record->state == RegionState::Frozen || !OnPhase(schedule.tick, *record)) {
            continue;
        }
        auto& group = groups[owner];
        group.record = record;
        group.due = group.ready;
    }
    constexpr IVec3 offsets[] = {{1, 0, 0}, {-1, 0, 0}, {0, 0, 1}, {0, 0, -1}};
    for (std::size_t i = 0; i < entries.size(); ++i) {
        const auto& entry = entries[i];
        if (!entry.admitted || !groups.at(entry.owner).due) {
            continue;
        }
        bool eligible = entry.preparation || Awake(entry) || entry.source || raining;
        for (const auto& offset : offsets) {
            const auto* neighbor = Find(entries, entry.coords + offset);
            eligible = eligible || (neighbor && neighbor->admitted && Awake(*neighbor));
        }
        if (eligible) {
            groups.at(entry.owner).candidates.push_back(i);
        }
    }
}

std::vector<Face> Topology(std::vector<Entry>& entries, const Groups& groups) {
    std::vector<Face> faces;
    for (const auto& entry : entries) {
        if (!entry.admitted || !groups.at(entry.owner).due) {
            continue;
        }
        for (const bool is_x : {false, true}) {
            const auto* other =
                Find(entries, entry.coords + (is_x ? IVec3(1, 0, 0) : IVec3(0, 0, 1)));
            if (!other || !other->admitted || !groups.at(other->owner).due) {
                continue;
            }
            faces.push_back({entry.owner,
                             other->owner,
                             entry.resident.id,
                             other->resident.id,
                             is_x,
                             std::max(entry.record->cadence_shift, other->record->cadence_shift)});
        }
    }
    std::sort(faces.begin(), faces.end());
    return faces;
}

void Allocate(Groups& groups,
              const ActiveRegionLedger& ledger,
              std::uint64_t windows,
              std::vector<Entry>& entries,
              Plan& plan) {
    std::vector<world::water_share::Claim> claims;
    // Copy only due debt; absent/frozen/off-phase registers remain with the caller.
    ActiveRegionLedger::WaterDebts debts;
    std::uint64_t serviceable_demand = 0;
    for (const auto& [owner, group] : groups) {
        if (!group.due) {
            continue;
        }
        claims.push_back({owner, static_cast<std::uint64_t>(group.candidates.size())});
        // Each admitted due candidate can use one interior window per plan.
        // Candidate groups partition the snapshot, so the sum is <= kMaxResidents.
        serviceable_demand += claims.back().awake;
        const auto found = ledger.water_debts().find(owner);
        if (found != ledger.water_debts().end()) {
            debts.emplace(owner, found->second);
        }
    }
    std::vector<world::water_share::Grant> grants;
    if (!world::water_share::allocate(
            std::min(windows, serviceable_demand), claims, debts, grants)) {
        Refuse();
    }
    std::sort(
        grants.begin(), grants.end(), [](const auto& a, const auto& b) { return a.key < b.key; });
    auto grant = grants.begin();
    for (const auto& claim : claims) {
        const auto& group = groups.at(claim.key);
        const auto granted =
            grant != grants.end() && grant->key == claim.key ? (grant++)->windows : 0;
        const auto count = static_cast<std::uint64_t>(group.candidates.size());
        const auto awarded = std::min(granted, count);
        plan.allocated_windows += awarded;
        auto cursor = group.record->water_cursor;
        if (awarded != 0) {
            const auto start = cursor % count;
            for (std::uint64_t j = 0; j < awarded; ++j) {
                auto& entry =
                    entries[group.candidates[static_cast<std::size_t>((start + j) % count)]];
                entry.selected = true;
            }
            cursor = (start + awarded) % count;
        }
        const auto debt = debts.find(claim.key);
        // Grants are budget-bounded. Restoring their unselected Q32 units cannot
        // exceed the allocator's checked post-credit debt (at most kMaxDebt).
        const auto carried_debt = (debt == debts.end() ? 0 : debt->second) +
                                  (granted - awarded) * world::water_share::kQ32;
        plan.regions.push_back({claim.key,
                                group.record->cadence_shift,
                                group.record->cadence_phase,
                                count,
                                awarded,
                                cursor,
                                carried_debt});
    }
}
} // namespace

Plan Build(std::span<const Resident> residents,
           std::span<const SourceInput> sources,
           const ActiveRegionLedger& ledger,
           const world::RegionSchedule& schedule,
           const Inputs& inputs) {
    if (residents.size() > kMaxResidents || sources.size() > kMaxSources ||
        schedule.due.size() > ActiveRegionLedger::kMaxRegions ||
        !LegalResolution(inputs.resolution) || schedule.tick >= world::WorldClock::kTickLimit) {
        Refuse();
    }
    const auto resolution = static_cast<std::uint64_t>(inputs.resolution);
    const auto cells = resolution * resolution;
    const auto capacity = 2 * cells + 4 * resolution;
    Plan plan;
    plan.global_windows = inputs.cell_budget / capacity;
    if (plan.global_windows > world::water_share::kMaxWindowBudget) {
        Refuse();
    }
    Groups groups;
    auto entries = Admit(residents, ledger, inputs, groups, plan.residents_visited);
    PlanSources(sources, inputs.resolution, entries, plan);
    PlanCandidates(entries, groups, ledger, schedule, inputs.raining);
    bool positive_claim = false;
    for (const auto& [owner, group] : groups) {
        if (!group.ready) {
            plan.refused_preparation.push_back(owner);
        }
        positive_claim = positive_claim || !group.candidates.empty();
    }
    const auto faces = Topology(entries, groups);
    auto remaining = plan.global_windows;
    const std::uint64_t keep = positive_claim && remaining > 0 ? 1 : 0;
    for (const auto& face : faces) {
        if (face.owner == face.neighbor) {
            continue;
        }
        const auto* a = Find(entries, Chunk::decode_id(face.positive));
        const auto* b = Find(entries, Chunk::decode_id(face.other));
        if (a->preparation || b->preparation ||
            (!Awake(*a) && !Awake(*b) && !inputs.raining && !a->source && !b->source)) {
            continue;
        }
        plan.faces.push_back(face);
    }
    if (!plan.faces.empty()) {
        std::uint8_t common_shift = 0;
        for (const auto& face : plan.faces) {
            common_shift = std::max(common_shift, face.common_shift);
        }
        const auto opportunity = schedule.tick >> common_shift;
        const auto start =
            static_cast<std::vector<Face>::difference_type>(opportunity % plan.faces.size());
        std::rotate(plan.faces.begin(), plan.faces.begin() + start, plan.faces.end());
        const auto pairs =
            std::min(static_cast<std::uint64_t>(plan.faces.size()), (remaining - keep) / 2);
        plan.faces.resize(static_cast<std::size_t>(pairs));
        remaining -= 2 * pairs;
    }
    plan.border_windows = plan.global_windows - remaining;
    Allocate(groups, ledger, remaining, entries, plan);
    for (const auto& entry : entries) {
        if (entry.selected) {
            plan.interiors.push_back(entry.resident.id);
            plan.participants.push_back(entry.resident.id);
            if (entry.preparation) {
                plan.preparation_cells += cells;
            }
        }
    }
    for (const auto& face : faces) {
        if (face.owner == face.neighbor &&
            Find(entries, Chunk::decode_id(face.positive))->selected &&
            Find(entries, Chunk::decode_id(face.other))->selected) {
            plan.faces.push_back(face);
        }
    }
    std::sort(plan.faces.begin(), plan.faces.end());
    for (const auto& face : plan.faces) {
        plan.participants.push_back(face.positive);
        plan.participants.push_back(face.other);
    }
    std::sort(plan.participants.begin(), plan.participants.end());
    plan.participants.erase(std::unique(plan.participants.begin(), plan.participants.end()),
                            plan.participants.end());
    plan.total_cells = plan.preparation_cells +
                       static_cast<std::uint64_t>(plan.interiors.size()) * cells +
                       static_cast<std::uint64_t>(plan.faces.size()) * 2 * resolution;
    return plan;
}
} // namespace Luminumbra::Systems::RegionalWater
