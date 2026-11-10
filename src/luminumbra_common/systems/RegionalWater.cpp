#include "RegionalWater.h"

#include "../components/CoreComponents.h"
#include "../persistence/WorldSaveService.h"
#include "../world/Chunk.h"
#include "SHIELD_WorldSystem.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <stdexcept>
#include <type_traits>
#include <utility>

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
    if (!ActiveRegionLedger::valid_anchor(position) ||
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
        const auto x = static_cast<int>(
            std::floor((static_cast<double>(position.x) - coords.x * CHUNK_SIZE_X) * resolution /
                       CHUNK_SIZE_X));
        const auto z = static_cast<int>(
            std::floor((static_cast<double>(position.z) - coords.z * CHUNK_SIZE_Z) * resolution /
                       CHUNK_SIZE_Z));
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

std::vector<Face> Topology(std::vector<Entry>& entries) {
    std::vector<Face> faces;
    for (const auto& entry : entries) {
        if (!entry.admitted) {
            continue;
        }
        for (const bool is_x : {false, true}) {
            const auto* other =
                Find(entries, entry.coords + (is_x ? IVec3(1, 0, 0) : IVec3(0, 0, 1)));
            if (!other || !other->admitted) {
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
    for (const auto& [owner, group] : groups) {
        if (!group.due) {
            continue;
        }
        claims.push_back({owner, static_cast<std::uint64_t>(group.candidates.size())});
        const auto found = ledger.water_debts().find(owner);
        if (found != ledger.water_debts().end()) {
            debts.emplace(owner, found->second);
        }
    }
    std::vector<world::water_share::Grant> grants;
    if (!world::water_share::allocate(windows, claims, debts, grants)) {
        Refuse();
    }
    std::sort(
        grants.begin(), grants.end(), [](const auto& a, const auto& b) { return a.key < b.key; });
    auto grant = grants.begin();
    for (const auto& claim : claims) {
        const auto& group = groups.at(claim.key);
        const auto awarded =
            grant != grants.end() && grant->key == claim.key ? (grant++)->windows : 0;
        plan.allocated_windows += awarded;
        const auto count = static_cast<std::uint64_t>(group.candidates.size());
        const auto used = std::min(awarded, count);
        auto cursor = group.record->water_cursor;
        if (used != 0) {
            const auto start = cursor % count;
            for (std::uint64_t j = 0; j < used; ++j) {
                auto& entry =
                    entries[group.candidates[static_cast<std::size_t>((start + j) % count)]];
                entry.selected = true;
            }
            cursor = (start + used) % count;
        }
        const auto debt = debts.find(claim.key);
        plan.regions.push_back({claim.key,
                                group.record->cadence_shift,
                                group.record->cadence_phase,
                                count,
                                awarded,
                                cursor,
                                debt == debts.end() ? 0 : debt->second});
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
    const auto faces = Topology(entries);
    auto remaining = plan.global_windows;
    const std::uint64_t keep = positive_claim && remaining > 0 ? 1 : 0;
    for (const auto& face : faces) {
        if (face.owner == face.neighbor ||
            schedule.tick % (std::uint64_t{1} << face.common_shift) != 0) {
            continue;
        }
        const auto* a = Find(entries, Chunk::decode_id(face.positive));
        const auto* b = Find(entries, Chunk::decode_id(face.other));
        if (a->preparation || b->preparation || remaining - keep < 2 ||
            (!Awake(*a) && !Awake(*b) && !inputs.raining && !a->source && !b->source)) {
            continue;
        }
        plan.faces.push_back(face);
        remaining -= 2;
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

namespace {
void ValidateStageGrid(const Chunk& chunk) {
    if (!chunk.has_water_sim.load()) {
        if (!chunk.water_depth_mm.empty() || !chunk.water_bed_mm.empty() ||
            !chunk.water_edge_flux.empty() || !chunk.water_level_data.empty() ||
            !chunk.water_flow_data.empty() || !chunk.water_sim_terrain_height.empty() ||
            !chunk.water_rest_level.empty() || !chunk.water_src_mm.empty()) {
            Refuse();
        }
    } else if (!chunk.has_complete_water_state() ||
               (!chunk.water_src_mm.empty() &&
                chunk.water_src_mm.size() != chunk.water_depth_mm.size())) {
        Refuse();
    }
    if (!LegalResolution(chunk.current_water_resolution.load()) ||
        !std::isfinite(chunk.max_water_delta_last_tick) || chunk.max_water_delta_last_tick < 0.0f ||
        chunk.ticks_below_threshold < 0 || chunk.water_mesh_dirty_ticks < 0 ||
        chunk.water_mesh_dirty_ticks > 15) {
        Refuse();
    }
}

RegionKey StageOwner(const Chunk& chunk) {
    const auto& coords = chunk.get_coords();
    if (coords.x < Chunk::kPackedMinXz || coords.x > Chunk::kPackedMaxXz ||
        coords.z < Chunk::kPackedMinXz || coords.z > Chunk::kPackedMaxXz ||
        coords.y < Chunk::kPackedMinY || coords.y > Chunk::kPackedMaxY) {
        Refuse();
    }
    return ActiveRegionLedger::region_at({static_cast<float>(coords.x * CHUNK_SIZE_X),
                                          0.0f,
                                          static_cast<float>(coords.z * CHUNK_SIZE_Z)});
}

void CaptureWater(const Chunk& original, Chunk& copy) {
    copy.water_depth_mm = original.water_depth_mm;
    copy.water_bed_mm = original.water_bed_mm;
    copy.water_edge_flux = original.water_edge_flux;
    copy.water_level_data = original.water_level_data;
    copy.water_flow_data = original.water_flow_data;
    copy.water_sim_terrain_height = original.water_sim_terrain_height;
    copy.water_rest_level = original.water_rest_level;
    copy.water_src_mm = original.water_src_mm;
    copy.has_water_sim.store(original.has_water_sim.load());
    copy.current_water_resolution.store(original.current_water_resolution.load());
    copy.is_water_sleeping.store(original.is_water_sleeping.load());
    copy.water_mesh_generated.store(original.water_mesh_generated.load());
    copy.max_water_delta_last_tick = original.max_water_delta_last_tick;
    copy.ticks_below_threshold = original.ticks_below_threshold;
    copy.water_mesh_dirty_ticks = original.water_mesh_dirty_ticks;
}

void PublishWater(Chunk& destination, Chunk& staged, bool changed) noexcept {
    destination.water_depth_mm.swap(staged.water_depth_mm);
    destination.water_bed_mm.swap(staged.water_bed_mm);
    destination.water_edge_flux.swap(staged.water_edge_flux);
    destination.water_level_data.swap(staged.water_level_data);
    destination.water_flow_data.swap(staged.water_flow_data);
    destination.water_sim_terrain_height.swap(staged.water_sim_terrain_height);
    destination.water_rest_level.swap(staged.water_rest_level);
    destination.water_src_mm.swap(staged.water_src_mm);
    destination.has_water_sim.store(staged.has_water_sim.load());
    destination.current_water_resolution.store(staged.current_water_resolution.load());
    destination.is_water_sleeping.store(staged.is_water_sleeping.load());
    destination.water_mesh_generated.store(staged.water_mesh_generated.load());
    destination.max_water_delta_last_tick = staged.max_water_delta_last_tick;
    destination.ticks_below_threshold = staged.ticks_below_threshold;
    destination.water_mesh_dirty_ticks = staged.water_mesh_dirty_ticks;
    if (changed) {
        destination.publish_water_state();
    }
}

std::int64_t CellArea(const Chunk& grid) {
    const auto resolution = static_cast<std::int64_t>(grid.current_water_resolution.load());
    return 4 * static_cast<std::int64_t>(CHUNK_SIZE_X) * CHUNK_SIZE_Z / (resolution * resolution);
}

std::int64_t Volume(const Chunk& grid) {
    std::int64_t depth = 0;
    for (const auto value : grid.water_depth_mm) {
        depth += value;
    }
    // At most INT32_MAX * 1024 per chunk and 2^20 participants: totals fit i64,
    // independent of the admitted resolution. Empty uninitialized grids contribute zero.
    return depth * CellArea(grid);
}
} // namespace

void Transaction::Begin(ActiveRegionLedger& ledger,
                        std::uint64_t tick,
                        std::span<const std::shared_ptr<Chunk>> participants,
                        std::span<const RegionPlan> regions) {
    if (pending() || (m_lastTick && tick <= *m_lastTick) || tick != ledger.tick() ||
        tick >= world::WorldClock::kTickLimit || participants.size() > kMaxResidents ||
        regions.size() > ActiveRegionLedger::kMaxRegions) {
        Refuse();
    }
    try {
        // Validate every shape and bound before any participant array is copied.
        for (const auto& chunk : participants) {
            ++m_visited;
            if (!chunk) {
                Refuse();
            }
            ValidateStageGrid(*chunk);
            const auto* record = FindRecord(ledger, StageOwner(*chunk));
            if (!record || record->state == RegionState::Frozen) {
                Refuse();
            }
        }
        m_entries.reserve(participants.size());
        for (const auto& chunk : participants) {
            m_entries.push_back({.live = chunk,
                                 .grid = {},
                                 .authority_revision = chunk->authority_hash_revision(),
                                 .water_revision = chunk->water_revision(),
                                 .changed = false});
        }
        std::sort(m_entries.begin(), m_entries.end(), [](const auto& a, const auto& b) {
            return a.live->get_id() < b.live->get_id();
        });
        for (std::size_t i = 1; i < m_entries.size(); ++i) {
            if (m_entries[i - 1].live->get_id() == m_entries[i].live->get_id()) {
                Refuse();
            }
        }
        m_patches.reserve(regions.size());
        for (const auto& region : regions) {
            const auto* record = FindRecord(ledger, region.owner);
            if (!record || record->cadence_shift != region.cadence_shift ||
                record->cadence_phase != region.cadence_phase) {
                Refuse();
            }
            m_patches.push_back({region.owner, region.debt, region.next_cursor});
        }
        if (!ledger.prepare_water_patch(m_patches, m_debts)) {
            Refuse();
        }
        for (auto& entry : m_entries) {
            entry.grid = std::make_shared<Chunk>(entry.live->get_coords());
            CaptureWater(*entry.live, *entry.grid);
            m_expectedVolume += Volume(*entry.grid);
        }
        m_lastTick = tick;
        m_ledger = &ledger;
    } catch (...) {
        Rollback();
        throw;
    }
}

const Chunk* Transaction::staged(ChunkID id) const noexcept {
    const auto found = std::lower_bound(
        m_entries.begin(), m_entries.end(), id, [](const auto& entry, ChunkID key) {
            return entry.live->get_id() < key;
        });
    return found == m_entries.end() || found->live->get_id() != id ? nullptr : found->grid.get();
}

Chunk& Transaction::Grid(ChunkID id) {
    const auto found = std::lower_bound(
        m_entries.begin(), m_entries.end(), id, [](const auto& entry, ChunkID key) {
            return entry.live->get_id() < key;
        });
    if (!pending() || found == m_entries.end() || found->live->get_id() != id) {
        Refuse();
    }
    return *found->grid;
}

void Transaction::Account(const WaterKernels::Result& result, const Chunk& grid) {
    if (!result.applied ||
        result.depth_before + result.source_mm != result.depth_after + result.sink_mm) {
        Refuse();
    }
    m_expectedVolume += (result.depth_after - result.depth_before) * CellArea(grid);
}

WaterKernels::Result Transaction::StepInterior(ChunkID id,
                                               SHIELD_WorldSystem& shield,
                                               const WaterKernels::InteriorInputs& inputs) {
    try {
        if (inputs.rain_mm < 0 || inputs.evap_mm < 0 || inputs.weather_scale_mm < 0) {
            Refuse();
        }
        auto& grid = Grid(id);
        const auto result = WaterKernels::StepInterior(grid, shield, inputs);
        Account(result, grid);
        return result;
    } catch (...) {
        Rollback();
        throw;
    }
}

WaterKernels::Result Transaction::StepFace(ChunkID positive, ChunkID other, bool is_x) {
    try {
        auto& a = Grid(positive);
        auto& b = Grid(other);
        const auto result = WaterKernels::StepSharedFace(a, b, is_x);
        Account(result, a);
        return result;
    } catch (...) {
        Rollback();
        throw;
    }
}

namespace {
std::int32_t SampleInteger(
    const std::vector<std::int32_t>& values, int old_resolution, int resolution, int x, int z) {
    const int denominator = resolution - 1;
    const int nx = x * (old_resolution - 1), nz = z * (old_resolution - 1);
    const int x0 = nx / denominator, z0 = nz / denominator;
    const int x1 = std::min(x0 + 1, old_resolution - 1), z1 = std::min(z0 + 1, old_resolution - 1);
    const std::int64_t fx = (2 * (nx % denominator) * 256 + denominator) / (2 * denominator);
    const std::int64_t fz = (2 * (nz % denominator) * 256 + denominator) / (2 * denominator);
    const auto at = [&](int sx, int sz) {
        return static_cast<std::int64_t>(
            values[static_cast<std::size_t>(sz * old_resolution + sx)]);
    };
    const auto sum = (256 - fx) * (256 - fz) * at(x0, z0) + fx * (256 - fz) * at(x1, z0) +
                     (256 - fx) * fz * at(x0, z1) + fx * fz * at(x1, z1) + 32768;
    return static_cast<std::int32_t>(sum >= 0 ? sum / 65536 : -((-sum + 65535) / 65536));
}

std::int32_t RoundedMillimetres(double metres) {
    const auto rounded = std::round(metres * 1000.0);
    if (!std::isfinite(rounded) || rounded < std::numeric_limits<std::int32_t>::min() ||
        rounded > std::numeric_limits<std::int32_t>::max()) {
        Refuse();
    }
    return static_cast<std::int32_t>(rounded);
}
} // namespace

void Transaction::PrepareGrid(ChunkID id, SHIELD_WorldSystem& shield, int resolution) {
    try {
        if (!LegalResolution(resolution)) {
            Refuse();
        }
        auto& grid = Grid(id);
        const bool initialized = grid.has_water_sim.load();
        const int old_resolution = grid.current_water_resolution.load();
        if (initialized && old_resolution == resolution) {
            return;
        }
        const auto before = Volume(grid);
        auto old_depth = std::move(grid.water_depth_mm);
        auto old_bed = std::move(grid.water_bed_mm);
        // Legal resolution was checked before allocation: at most 1,024 cells.
        const auto cells = static_cast<std::size_t>(resolution * resolution);
        grid.water_depth_mm.resize(cells);
        grid.water_bed_mm.resize(cells);
        grid.water_edge_flux.assign(2 * cells, 0);
        grid.water_src_mm.clear();
        grid.water_level_data.resize(cells);
        grid.water_flow_data.assign(cells, Vec2(0.0f));
        grid.water_rest_level.resize(cells);
        grid.water_sim_terrain_height.resize(cells);
        const auto coords = grid.get_coords();
        for (int z = 0; z < resolution; ++z) {
            for (int x = 0; x < resolution; ++x) {
                const auto cell = static_cast<std::size_t>(z * resolution + x);
                const float wx = static_cast<float>(coords.x * CHUNK_SIZE_X) +
                                 (static_cast<float>(x) + 0.5f) * static_cast<float>(CHUNK_SIZE_X) /
                                     static_cast<float>(resolution);
                const float wz = static_cast<float>(coords.z * CHUNK_SIZE_Z) +
                                 (static_cast<float>(z) + 0.5f) * static_cast<float>(CHUNK_SIZE_Z) /
                                     static_cast<float>(resolution);
                const float rest = shield.WaterLevelAt(wx, wz);
                if (!std::isfinite(rest)) {
                    Refuse();
                }
                if (initialized) {
                    grid.water_bed_mm[cell] =
                        SampleInteger(old_bed, old_resolution, resolution, x, z);
                    grid.water_depth_mm[cell] =
                        SampleInteger(old_depth, old_resolution, resolution, x, z);
                } else {
                    const auto bed = shield.GetTerrainHeightAt(wx, wz);
                    grid.water_bed_mm[cell] = RoundedMillimetres(bed);
                    grid.water_depth_mm[cell] = std::max<std::int32_t>(
                        0, RoundedMillimetres(static_cast<double>(rest) - bed));
                }
                grid.water_rest_level[cell] = rest;
                grid.water_sim_terrain_height[cell] =
                    static_cast<float>(grid.water_bed_mm[cell]) / 1000.0f;
                grid.water_level_data[cell] =
                    static_cast<float>(static_cast<std::int64_t>(grid.water_bed_mm[cell]) +
                                       grid.water_depth_mm[cell]) /
                    1000.0f;
            }
        }
        grid.has_water_sim.store(true);
        grid.current_water_resolution.store(resolution);
        grid.water_mesh_generated.store(false);
        grid.water_mesh_dirty_ticks = 0;
        ValidateStageGrid(grid);
        m_expectedVolume += Volume(grid) - before;
    } catch (...) {
        Rollback();
        throw;
    }
}

void Transaction::Inject(const Source& source) {
    try {
        auto& grid = Grid(source.target);
        if (source.millimetres < 0 || source.cell >= grid.water_depth_mm.size()) {
            Refuse();
        }
        const auto depth =
            static_cast<std::int64_t>(grid.water_depth_mm[source.cell]) + source.millimetres;
        if (depth > std::numeric_limits<std::int32_t>::max()) {
            Refuse();
        }
        grid.water_depth_mm[source.cell] = static_cast<std::int32_t>(depth);
        m_expectedVolume += static_cast<std::int64_t>(source.millimetres) * CellArea(grid);
    } catch (...) {
        Rollback();
        throw;
    }
}

void Transaction::FinishInterior(ChunkID id) {
    try {
        auto& grid = Grid(id);
        // Only interior visits advance calm time. Later seam flux can wake either endpoint.
        if (grid.max_water_delta_last_tick < 0.0001f) {
            grid.ticks_below_threshold = std::min(grid.ticks_below_threshold, 119) + 1;
            grid.is_water_sleeping.store(grid.ticks_below_threshold >= 120);
        } else {
            grid.ticks_below_threshold = 0;
            grid.is_water_sleeping.store(false);
        }
    } catch (...) {
        Rollback();
        throw;
    }
}

void Transaction::Publish(SHIELD_WorldSystem* shield) {
    try {
        if (!pending() || !m_lastTick || m_ledger->tick() != *m_lastTick) {
            Refuse();
        }
        std::int64_t volume = 0;
        for (auto& entry : m_entries) {
            if (entry.live->authority_hash_revision() != entry.authority_revision ||
                entry.live->water_revision() != entry.water_revision) {
                Refuse();
            }
            ValidateStageGrid(*entry.grid);
            volume += Volume(*entry.grid);
            entry.changed = !entry.live->same_water_state(*entry.grid);
        }
        if (volume != m_expectedVolume) {
            Refuse();
        }
        const Persistence::WorldSaveService::SaveOwnership ownership;
        std::optional<SHIELD_WorldSystem::WaterAuthorityPublication> authority;
        if (shield) {
            std::vector<Chunk*> changed;
            changed.reserve(m_entries.size());
            for (const auto& entry : m_entries) {
                if (entry.changed) {
                    changed.push_back(entry.live.get());
                }
            }
            if (shield->durable_read_failed()) {
                Refuse();
            }
            authority.emplace(shield->prepare_water_authority_publication(changed));
        }
        // Recheck ledger admission before the first live write. Everything below
        // transfers prepared storage or writes scalars; there is no fallible tail.
        if (!m_ledger->prepare_water_patch(m_patches, m_debts)) {
            Refuse();
        }
        for (auto& entry : m_entries) {
            PublishWater(*entry.live, *entry.grid, entry.changed);
        }
        m_ledger->commit_water_patch(m_patches, m_debts);
        if (shield && authority) {
            shield->publish_water_authority(std::move(*authority));
        }
        Rollback();
    } catch (...) {
        Rollback();
        throw;
    }
}

void Transaction::Rollback() noexcept {
    m_ledger = nullptr;
    m_entries.clear();
    m_patches.clear();
    m_debts.clear();
    m_expectedVolume = 0;
    m_visited = 0;
}
} // namespace Luminumbra::Systems::RegionalWater

namespace Luminumbra::Systems {
void WaterSystem::UpdateRegional(entt::registry& registry,
                                 std::span<const std::shared_ptr<Chunk>> resident,
                                 world::ActiveRegionLedger& ledger,
                                 const world::RegionSchedule& schedule,
                                 std::vector<world::RegionWork>& completed,
                                 std::uint64_t cell_budget) {
    if (!m_regional_scheduling || m_boot_paused) {
        return;
    }
    const Persistence::WorldSaveService::SaveOwnership ownership;
    if (!m_shield_system || m_shield_system->durable_read_failed() ||
        resident.size() > RegionalWater::kMaxResidents ||
        completed.size() > world::ActiveRegionLedger::kMaxRegions ||
        schedule.tick != ledger.tick() || !RegionalWater::LegalResolution(m_sim_resolution) ||
        (m_last_regional_tick && schedule.tick <= *m_last_regional_tick) ||
        m_rain_mm_per_tick < 0 || m_evap_mm_per_tick < 0 || m_weather_rain_scale_mm < 0) {
        throw std::invalid_argument("Regional water execution input refused");
    }
    // The supplied participation snapshot bounds every scan; no store or ledger traversal.
    std::vector<RegionalWater::Resident> residents;
    std::vector<std::shared_ptr<Chunk>> sorted(resident.begin(), resident.end());
    residents.reserve(resident.size());
    for (const auto& chunk : resident) {
        if (!chunk) {
            throw std::invalid_argument("Missing regional water resident");
        }
        residents.push_back({.id = chunk->get_id(),
                             .initialized = chunk->has_water_sim.load(),
                             .resolution = chunk->current_water_resolution.load(),
                             .awake = !chunk->is_water_sleeping.load(),
                             .preparation_ready = true,
                             .available = chunk->get_state() != ChunkState::Loading});
    }
    std::sort(sorted.begin(), sorted.end(), [](const auto& a, const auto& b) {
        return a->get_id() < b->get_id();
    });
    std::vector<RegionalWater::SourceInput> sources;
    const auto view = registry.view<const Components::TransformComponent,
                                    const Components::FlowingWaterSourceComponent>();
    // The entity domain bounds traversal, including entities rejected by the view filter.
    static_assert(entt::entt_traits<entt::entity>::entity_mask < RegionalWater::kMaxSources);
    for (const auto entity : view) {
        sources.push_back(
            {.id = static_cast<std::uint64_t>(entt::to_integral(entity)),
             .position = view.get<const Components::TransformComponent>(entity).position,
             .physical = view.get<const Components::FlowingWaterSourceComponent>(entity)});
    }
    const bool raining = m_rain_mm_per_tick > 0 || (m_weather_rain && m_weather_rain_scale_mm > 0);
    const auto plan = RegionalWater::Build(
        residents, sources, ledger, schedule, {m_sim_resolution, cell_budget, raining});
    if (plan.total_cells > cell_budget) {
        throw std::invalid_argument("Regional water budget exceeded");
    }
    std::vector<std::shared_ptr<Chunk>> participants;
    participants.reserve(plan.participants.size());
    for (const auto id : plan.participants) {
        const auto found =
            std::lower_bound(sorted.begin(), sorted.end(), id, [](const auto& chunk, ChunkID key) {
                return chunk->get_id() < key;
            });
        if (found == sorted.end() || (*found)->get_id() != id) {
            throw std::invalid_argument("Missing regional water participant");
        }
        participants.push_back(*found);
    }
    RegionalWater::Transaction stage;
    stage.Begin(ledger, schedule.tick, participants, plan.regions);
    RegionalWork result;
    result.global_windows = plan.global_windows;
    result.border_windows = plan.border_windows;
    result.allocated_windows = plan.allocated_windows;
    result.preparation_cells = plan.preparation_cells;
    result.total_cells = plan.total_cells;
    result.water_cells = plan.total_cells - plan.preparation_cells;
    result.residents_visited = plan.residents_visited;
    result.participants_visited = stage.participants_visited();
    result.visits = plan.interiors;
    for (const auto& chunk : participants) {
        result.volume_before_qmm += RegionalWater::Volume(*chunk);
    }
    const auto resolution = static_cast<std::uint64_t>(m_sim_resolution);
    const auto cells = resolution * resolution;
    std::map<world::RegionKey, world::RegionWork> contributions;
    const auto contribute = [&](ChunkID id, std::uint64_t count) {
        const auto owner = RegionalWater::StageOwner(*stage.staged(id));
        auto& work = contributions[owner];
        work.key = owner;
        work.region_ticks = 1;
        work.water_cells += count;
    };
    std::int64_t injected = 0;
    const auto area = static_cast<std::int64_t>(4 * CHUNK_SIZE_X * CHUNK_SIZE_Z) /
                      static_cast<std::int64_t>(cells);
    for (const auto id : plan.interiors) {
        const auto before = RegionalWater::Volume(*stage.staged(id));
        stage.PrepareGrid(id, *m_shield_system, m_sim_resolution);
        result.preparation_volume_qmm += RegionalWater::Volume(*stage.staged(id)) - before;
        auto source = std::lower_bound(
            plan.sources.begin(), plan.sources.end(), id, [](const auto& input, ChunkID key) {
                return input.target < key;
            });
        for (; source != plan.sources.end() && source->target == id; ++source) {
            stage.Inject(*source);
            injected += source->millimetres;
        }
        const auto kernel = stage.StepInterior(id,
                                               *m_shield_system,
                                               {.finite_hydrology = m_finite_hydrology,
                                                .rain_mm = m_rain_mm_per_tick,
                                                .evap_mm = m_evap_mm_per_tick,
                                                .weather_rain = m_weather_rain,
                                                .weather_scale_mm = m_weather_rain_scale_mm});
        result.source_mm += kernel.source_mm;
        result.sink_mm += kernel.sink_mm;
        stage.FinishInterior(id);
        contribute(id, cells);
    }
    result.source_mm += injected;
    int wet_pairs = 0;
    for (const auto& face : plan.faces) {
        wet_pairs += stage.StepFace(face.positive, face.other, face.is_x).wet_pairs;
        contribute(face.positive, resolution);
        contribute(face.other, resolution);
    }
    for (const auto& chunk : participants) {
        result.volume_after_qmm += RegionalWater::Volume(*stage.staged(chunk->get_id()));
    }
    result.source_volume_qmm = result.source_mm * area;
    result.sink_volume_qmm = result.sink_mm * area;
    if (result.volume_before_qmm + result.preparation_volume_qmm + result.source_volume_qmm !=
            result.volume_after_qmm + result.sink_volume_qmm ||
        injected > std::numeric_limits<std::int64_t>::max() - m_debug_source_injected_mm ||
        sources.size() > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max() -
                                                    m_debug_sources_seen) ||
        contributions.size() > world::ActiveRegionLedger::kMaxRegions - completed.size()) {
        throw std::invalid_argument("Regional water publication accounting refused");
    }
    auto next_completed = completed;
    next_completed.reserve(completed.size() + contributions.size());
    for (const auto& [owner, work] : contributions) {
        static_cast<void>(owner);
        next_completed.push_back(work);
    }
    static_assert(std::is_nothrow_swappable_v<RegionalWork>);
    stage.Publish(m_shield_system);
    // Every allocation/refusal precedes this tail: only swaps and scalar writes remain.
    completed.swap(next_completed);
    m_debug_source_injected_mm += injected;
    m_debug_sources_seen += static_cast<std::int64_t>(sources.size());
    m_dbg_last_source_mm = result.source_mm;
    m_dbg_last_sink_mm = result.sink_mm;
    m_dbg_mass_ok = true;
    m_dbg_seam_wet_pairs = wet_pairs;
    m_dbg_awake_water_chunks = 0;
    for (const auto& region : plan.regions) {
        m_dbg_awake_water_chunks += static_cast<std::size_t>(region.candidates);
    }
    m_dbg_cells_simmed = static_cast<std::size_t>(result.water_cells);
    m_cells_stepped_last_update = static_cast<std::size_t>(result.water_cells);
    std::swap(m_regional_work, result);
    m_last_regional_tick = schedule.tick;
}
} // namespace Luminumbra::Systems
