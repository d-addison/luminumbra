#include "WaterSystem.h"

#include "../components/CoreComponents.h"
#include "../core/WaterComponents.h"
#include "../world/Chunk.h"
#include "SHIELD_WorldSystem.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <type_traits>

namespace Luminumbra::Systems {
namespace {
using world::ActiveRegionLedger;
using world::RegionKey;
using world::RegionState;

[[noreturn]] void RefuseWater(const char* message) {
    throw std::runtime_error(message);
}

bool LegalResolution(int resolution) {
    return resolution == 4 || resolution == 8 || resolution == 16 || resolution == 32;
}

RegionKey WaterOwner(const Chunk& chunk) {
    const auto coords = chunk.get_coords();
    // ChunkID packing must be injective, and every coordinate product below must fit int.
    if (coords.x < -1048576 || coords.x >= 1048576 || coords.z < -1048576 || coords.z >= 1048576 ||
        coords.y < -2097152 || coords.y >= 2097152 || chunk.get_id() != Chunk::calculate_id(coords))
        RefuseWater("Regional water chunk identity refused");
    return ActiveRegionLedger::region_at(Vec3(static_cast<float>(coords.x * CHUNK_SIZE_X),
                                              0.0f,
                                              static_cast<float>(coords.z * CHUNK_SIZE_Z)));
}

// Empty authoritative storage is a legitimate initialization opportunity; partial saved
// storage is never repaired into a fresh grid. Required mirrors are checked independently.
void ValidateWater(const Chunk& chunk) {
    if (!chunk.has_water_sim.load()) {
        if (!chunk.water_depth_mm.empty() || !chunk.water_bed_mm.empty() ||
            !chunk.water_edge_flux.empty() || !chunk.water_level_data.empty() ||
            !chunk.water_flow_data.empty() || !chunk.water_sim_terrain_height.empty())
            RefuseWater("Regional water uninitialized storage refused");
        return;
    }
    const int resolution = chunk.current_water_resolution.load();
    if (!LegalResolution(resolution))
        RefuseWater("Regional water resolution refused");
    // Bounded resolution makes both products exact on all supported size_t widths.
    const auto cells = static_cast<std::size_t>(resolution) * static_cast<std::size_t>(resolution);
    if (chunk.water_depth_mm.size() != cells || chunk.water_bed_mm.size() != cells ||
        chunk.water_edge_flux.size() != 2 * cells || chunk.water_level_data.size() != cells ||
        chunk.water_flow_data.size() != cells || chunk.water_sim_terrain_height.size() != cells ||
        (!chunk.water_rest_level.empty() && chunk.water_rest_level.size() != cells))
        RefuseWater("Regional water grid shape refused");
    const auto* depth = chunk.water_depth_mm.data();
    const auto* level = chunk.water_level_data.data();
    const auto* flow = chunk.water_flow_data.data();
    const auto* terrain = chunk.water_sim_terrain_height.data();
    const int calm_ticks = chunk.ticks_below_threshold;
    const int mesh_ticks = chunk.water_mesh_dirty_ticks;
    const float delta = chunk.max_water_delta_last_tick;
    if (calm_ticks < 0 || mesh_ticks < 0 || mesh_ticks > 15 || !std::isfinite(delta) ||
        delta < 0.0f)
        RefuseWater("Regional water continuation refused");
    for (std::size_t i = 0; i < cells; ++i) {
        if (depth[i] < 0 || !std::isfinite(level[i]) || !std::isfinite(flow[i].x) ||
            !std::isfinite(flow[i].y) || !std::isfinite(terrain[i]))
            RefuseWater("Regional water cell refused");
    }
}

} // namespace

namespace RegionalWater {
Resident* PlanningCache::Find(ChunkID id) {
    const auto found = m_index.find(id);
    return found == m_index.end() ? nullptr : &entries[found->second];
}
const Resident* PlanningCache::Find(ChunkID id) const {
    const auto found = m_index.find(id);
    return found == m_index.end() ? nullptr : &entries[found->second];
}
Candidates* PlanningCache::FindCandidates(RegionKey owner) {
    const auto found = std::lower_bound(
        candidates.begin(), candidates.end(), owner, [](const auto& entry, RegionKey key) {
            return entry.owner < key;
        });
    return found == candidates.end() || found->owner != owner ? nullptr : &*found;
}

void PlanningCache::Rebuild(std::span<const std::shared_ptr<Chunk>> resident) {
    // Build off to the side. A duplicate/invalid identity or allocation failure must
    // not leave a partially indexed topology available to a retry.
    PlanningCache next;
    next.entries.reserve(resident.size());
    for (const auto position : m_roster) {
        const auto& chunk = resident[position];
        const auto owner = WaterOwner(*chunk);
        Resident entry;
        entry.id = chunk->get_id();
        entry.chunk = chunk.get();
        entry.identity = chunk;
        entry.coords = chunk->get_coords();
        entry.owner = owner;
        if (const auto* old = Find(entry.id))
            entry.workspace = old->workspace;
        next.entries.push_back(std::move(entry));
    }
    std::sort(next.entries.begin(), next.entries.end(), [](const auto& a, const auto& b) {
        return a.id < b.id;
    });
    next.m_index.reserve(next.entries.size());
    for (std::size_t i = 0; i < next.entries.size(); ++i) {
        if (!next.m_index.emplace(next.entries[i].id, i).second)
            RefuseWater("Regional water duplicate resident identity");
        next.candidates.push_back({next.entries[i].owner, {}});
    }
    std::sort(next.candidates.begin(), next.candidates.end(), [](const auto& a, const auto& b) {
        return a.owner < b.owner;
    });
    next.candidates.erase(
        std::unique(next.candidates.begin(),
                    next.candidates.end(),
                    [](const auto& a, const auto& b) { return a.owner == b.owner; }),
        next.candidates.end());
    constexpr IVec3 offsets[] = {{1, 0, 0}, {-1, 0, 0}, {0, 0, 1}, {0, 0, -1}};
    for (std::size_t i = 0; i < next.entries.size(); ++i) {
        auto& entry = next.entries[i];
        entry.candidate =
            static_cast<std::size_t>(next.FindCandidates(entry.owner) - next.candidates.data());
        for (std::size_t direction = 0; direction < 4; ++direction) {
            const auto coords = entry.coords + offsets[direction];
            // Never wrap packed x/z world limits onto the opposite edge.
            if (coords.x < -1048576 || coords.x >= 1048576 || coords.z < -1048576 ||
                coords.z >= 1048576)
                continue;
            const auto found = next.m_index.find(Chunk::calculate_id(coords));
            if (found == next.m_index.end())
                continue;
            entry.neighbors[direction] = found->second;
            if (direction == 0 || direction == 2) {
                const auto& other = next.entries[found->second];
                next.faces.push_back(
                    {{entry.owner, other.owner, entry.id, other.id, direction == 0},
                     i,
                     found->second});
            }
        }
    }
    std::sort(next.faces.begin(), next.faces.end(), [](const auto& a, const auto& b) {
        return a.face < b.face;
    });
    // Bit identity: these are the original (owner, neighbor, positive, other, is_x)
    // tuples. Removing sealed endpoints from this sorted superset gives exactly
    // SnapshotFaces' sorted admitted subset, without exchanging physical endpoints.
    next.m_seen.resize(next.entries.size());
    // Keep reusable per-tick buffers; only topology and its indexes are replaced.
    entries.swap(next.entries);
    faces.swap(next.faces);
    candidates.swap(next.candidates);
    m_index.swap(next.m_index);
    m_seen.swap(next.m_seen);
}

void PlanningCache::Refresh(std::span<const std::shared_ptr<Chunk>> resident,
                            const ActiveRegionLedger& ledger,
                            int resolution) {
    std::fill(m_seen.begin(), m_seen.end(), 0);
    m_roster.clear();
    bool rebuild = false;
    for (std::size_t position = 0; position < resident.size(); ++position) {
        const auto& chunk = resident[position];
        if (!chunk || chunk->get_state() == ChunkState::Loading)
            continue;
        m_roster.push_back(position);
        const auto found = m_index.find(chunk->get_id());
        if (found == m_index.end()) {
            rebuild = true;
            continue;
        }
        const auto index = found->second;
        if (m_seen[index]++)
            RefuseWater("Regional water duplicate resident identity");
        const auto& old = entries[index];
        if (old.chunk != chunk.get() || old.identity.owner_before(chunk) ||
            chunk.owner_before(old.identity) || old.coords != chunk->get_coords())
            rebuild = true;
    }
    if (rebuild || m_roster.size() != entries.size())
        Rebuild(resident);
    for (auto& group : candidates) {
        const auto record = ledger.records().find(group.owner);
        group.admitted =
            record != ledger.records().end() && record->second.state != RegionState::Frozen;
    }
    for (auto& entry : entries) {
        // Chunk coords/id are const. The validated identity therefore fixes WaterOwner;
        // grouping that owner's immutable ledger lookup preserves admission exactly.
        entry.admitted = candidates[entry.candidate].admitted;
        entry.preparation = entry.awake = entry.source = entry.selected = entry.staged =
            entry.changed = false;
        if (!entry.admitted) {
            entry.workspace.reset();
            continue;
        }
        // Public mutable water arrays/mirrors and continuation flags do not have a
        // complete writer revision. Retain full preflight, even on a warm topology.
        ValidateWater(*entry.chunk);
        entry.preparation = !entry.chunk->has_water_sim.load() ||
                            entry.chunk->current_water_resolution.load() != resolution;
        entry.awake = entry.chunk->has_water_sim.load() && !entry.chunk->is_water_sleeping.load();
    }
}

// Bit identity: topology index -> ChunkID is strictly increasing. Appending eligible
// indexes in that order yields the original ascending unique per-owner candidates.
// Eligibility reads only sealed prior flags; no newly woken chunk propagates a second hop.
void PlanningCache::PlanCandidates(const ActiveRegionLedger& ledger,
                                   const world::RegionSchedule& schedule,
                                   bool raining) {
    due.assign(schedule.due.begin(), schedule.due.end());
    std::sort(due.begin(), due.end());
    for (auto& group : candidates) {
        group.ids.clear();
        group.due = false;
    }
    for (std::size_t i = 0; i < due.size(); ++i) {
        const auto found = ledger.records().find(due[i]);
        if ((i && due[i] == due[i - 1]) || found == ledger.records().end() ||
            found->second.state == RegionState::Frozen)
            RefuseWater("Regional water due owner refused");
        if (auto* group = FindCandidates(due[i]))
            group->due = true;
    }
    for (std::size_t i = 0; i < entries.size(); ++i) {
        const auto& entry = entries[i];
        auto& group = candidates[entry.candidate];
        if (!entry.admitted || !group.due)
            continue;
        bool eligible = entry.preparation || entry.awake || raining || entry.source;
        for (const auto neighbor : entry.neighbors) {
            if (neighbor != missing && entries[neighbor].admitted && entries[neighbor].awake)
                eligible = true;
        }
        if (eligible)
            group.ids.push_back(i);
    }
    claims.clear();
    for (const auto owner : due) {
        const auto* group = FindCandidates(owner);
        claims.push_back({owner, group ? group->ids.size() : 0});
    }
}
} // namespace RegionalWater

namespace {
using WaterSnapshot = RegionalWater::PlanningCache;
using WaterFace = RegionalWater::Face;

void SnapshotSources(entt::registry& registry, WaterSnapshot& snapshot, int resolution) {
    auto& result = snapshot.sources;
    result.clear();
    const auto sources = registry.view<const Components::TransformComponent,
                                       const Components::WaterFlowSourceComponent>();
    for (const auto entity : sources) {
        const auto& position = sources.get<const Components::TransformComponent>(entity).position;
        if (!ActiveRegionLedger::valid_anchor(position) ||
            std::abs(static_cast<double>(position.y)) >= 2097152.0 * CHUNK_SIZE_Y)
            RefuseWater("Regional water source position refused");
        auto coords = SHIELD_WorldSystem::world_to_chunk_coords(position);
        auto* target = snapshot.Find(Chunk::calculate_id(coords));
        if (target && !target->admitted)
            target = nullptr;
        // Resident direct chunk first; if it has no prior grid, use the resident y=0 column.
        // Never load a parked/durable record to resolve a source.
        if (!target || !target->chunk->has_water_sim.load()) {
            coords.y = 0;
            target = snapshot.Find(Chunk::calculate_id(coords));
            if (target && !target->admitted)
                target = nullptr;
        }
        if (!target)
            continue;
        const float flow =
            sources.get<const Components::WaterFlowSourceComponent>(entity).flow_rate;
        const float cell_area =
            (static_cast<float>(CHUNK_SIZE_X) / static_cast<float>(resolution)) *
            (static_cast<float>(CHUNK_SIZE_Z) / static_cast<float>(resolution));
        const float quantized = flow * SECONDS_PER_TICK / cell_area * 1000.0f + 0.5f;
        if (!std::isfinite(flow) || flow < 0.0f || !std::isfinite(quantized) ||
            static_cast<double>(quantized) > std::numeric_limits<std::int32_t>::max())
            RefuseWater("Regional water source conversion refused");
        const auto amount = static_cast<std::int32_t>(quantized);
        if (amount == 0)
            continue;
        const auto resolved = target->chunk->get_coords();
        const auto x = static_cast<int>(
            std::floor((static_cast<double>(position.x) - resolved.x * CHUNK_SIZE_X) * resolution /
                       CHUNK_SIZE_X));
        const auto z = static_cast<int>(
            std::floor((static_cast<double>(position.z) - resolved.z * CHUNK_SIZE_Z) * resolution /
                       CHUNK_SIZE_Z));
        if (x < 0 || x >= resolution || z < 0 || z >= resolution)
            RefuseWater("Regional water source cell refused");
        result.push_back({target->id, static_cast<std::size_t>(z * resolution + x), amount});
        target->source = true;
    }
    std::sort(result.begin(), result.end());
}

bool CommonWaterPhase(const ActiveRegionLedger& ledger, const WaterFace& face, std::uint64_t tick) {
    const auto shift = std::max(ledger.records().at(face.owner).cadence_shift,
                                ledger.records().at(face.neighbor).cadence_shift);
    if (shift > 16)
        RefuseWater("Regional water cadence refused");
    return tick % (std::uint64_t{1} << shift) == 0;
}

void ReserveWaterFaces(WaterSnapshot& snapshot,
                       const ActiveRegionLedger& ledger,
                       std::uint64_t tick,
                       std::uint64_t windows,
                       bool positive_claim,
                       bool raining) {
    auto& reserved = snapshot.reserved;
    reserved.clear();
    const std::uint64_t keep = positive_claim && windows > 0 ? 1 : 0;
    for (std::size_t i = 0; i < snapshot.faces.size(); ++i) {
        const auto& indexed = snapshot.faces[i];
        const auto& face = indexed.face;
        const auto& a = snapshot.entries[indexed.positive];
        const auto& b = snapshot.entries[indexed.other];
        if (!a.admitted || !b.admitted || face.owner == face.neighbor || a.preparation ||
            b.preparation || (!a.awake && !b.awake && !raining && !a.source && !b.source) ||
            !CommonWaterPhase(ledger, face, tick))
            continue;
        if (windows - keep < 2)
            continue;
        reserved.push_back(i);
        windows -= 2;
    }
}

void StageWater(const Chunk& original, Chunk& workspace, bool reuse_sources) {
    auto* copy = &workspace;
    copy->water_depth_mm = original.water_depth_mm;
    copy->water_bed_mm = original.water_bed_mm;
    copy->water_edge_flux = original.water_edge_flux;
    copy->water_level_data = original.water_level_data;
    copy->water_flow_data = original.water_flow_data;
    copy->water_sim_terrain_height = original.water_sim_terrain_height;
    copy->water_rest_level = original.water_rest_level;
    // Keep the derived source mask: the kernel checks its resolution and generator inputs.
    // Discarding it here reran river noise for every selected cell on every tick.
    if (reuse_sources) {
        copy->water_src_mm = original.water_src_mm;
        copy->water_source_inputs = original.water_source_inputs;
    } else {
        copy->water_src_mm.clear();
        copy->water_source_inputs.reset();
    }
    copy->has_water_sim.store(original.has_water_sim.load());
    copy->current_water_resolution.store(original.current_water_resolution.load());
    copy->is_water_sleeping.store(original.is_water_sleeping.load());
    copy->water_mesh_generated.store(original.water_mesh_generated.load());
    copy->max_water_delta_last_tick = original.max_water_delta_last_tick;
    copy->ticks_below_threshold = original.ticks_below_threshold;
    copy->water_mesh_dirty_ticks = original.water_mesh_dirty_ticks;
}

bool WaterChanged(const Chunk& before, const Chunk& after) {
    return before.water_depth_mm != after.water_depth_mm ||
           before.water_bed_mm != after.water_bed_mm ||
           before.water_edge_flux != after.water_edge_flux ||
           before.has_water_sim.load() != after.has_water_sim.load() ||
           before.current_water_resolution.load() != after.current_water_resolution.load() ||
           before.is_water_sleeping.load() != after.is_water_sleeping.load() ||
           before.max_water_delta_last_tick != after.max_water_delta_last_tick ||
           before.ticks_below_threshold != after.ticks_below_threshold;
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
    destination.water_source_inputs = staged.water_source_inputs;
    destination.has_water_sim.store(staged.has_water_sim.load());
    destination.current_water_resolution.store(staged.current_water_resolution.load());
    destination.is_water_sleeping.store(staged.is_water_sleeping.load());
    destination.water_mesh_generated.store(staged.water_mesh_generated.load());
    destination.max_water_delta_last_tick = staged.max_water_delta_last_tick;
    destination.ticks_below_threshold = staged.ticks_below_threshold;
    destination.water_mesh_dirty_ticks = staged.water_mesh_dirty_ticks;
    // Publication is the only revision/dirty write to live chunks. This commit transfers
    // water divergence directly to sticky authority and the save-dirty bit, just like
    // SHIELD sync_water_authority. The pending promotion bit is then consumed; the water
    // revision still protects changes made after serialization from save acknowledgement.
    if (changed) {
        destination.mark_water_state_dirty();
        destination.set_water_authority();
        destination.mark_sdf_loaded_or_edited();
        destination.mark_dirty_for_save();
        destination.clear_water_state_dirty();
    }
}

std::int64_t DepthSum(const Chunk& chunk) {
    std::int64_t sum = 0;
    for (const auto depth : chunk.water_depth_mm)
        sum += depth;
    return sum;
}

std::int64_t WaterVolume(const Chunk& chunk, std::int64_t depth_sum) {
    if (!chunk.has_water_sim.load())
        return 0;
    const auto resolution = static_cast<std::int64_t>(chunk.current_water_resolution.load());
    // A chunk holds at most INT32_MAX * 1024 qmm. The window cap bounds the
    // participant union by 2^20 chunks, so all whole-plan volume totals fit i64.
    const auto cell_area =
        4 * static_cast<std::int64_t>(CHUNK_SIZE_X) * CHUNK_SIZE_Z / (resolution * resolution);
    return depth_sum * cell_area;
}

std::int32_t RoundedWaterMm(double value, bool depth) {
    if (!std::isfinite(value))
        RefuseWater("Regional water initialization conversion refused");
    const double rounded = std::round(value * 1000.0);
    if (rounded < std::numeric_limits<std::int32_t>::min() ||
        rounded > std::numeric_limits<std::int32_t>::max())
        RefuseWater("Regional water initialization overflow");
    return depth && rounded < 0 ? 0 : static_cast<std::int32_t>(rounded);
}
} // namespace

void WaterSystem::ValidateRegionalGrid(const Chunk& chunk) {
    ValidateWater(chunk);
}

void WaterSystem::PrepareRegionalGrid(Chunk& chunk) {
    const int old_resolution = chunk.current_water_resolution.load();
    const auto old_depth = chunk.water_depth_mm;
    const bool initialized = chunk.has_water_sim.load();
    const int resolution = m_sim_resolution;
    const auto cells = static_cast<std::size_t>(resolution) * static_cast<std::size_t>(resolution);
    chunk.water_depth_mm.assign(cells, 0);
    chunk.water_bed_mm.assign(cells, 0);
    chunk.water_edge_flux.assign(2 * cells, 0);
    chunk.water_level_data.resize(cells);
    chunk.water_flow_data.assign(cells, Vec2(0.0f));
    chunk.water_rest_level.resize(cells);
    chunk.water_sim_terrain_height.resize(cells);
    const auto coords = chunk.get_coords();
    for (int z = 0; z < resolution; ++z) {
        for (int x = 0; x < resolution; ++x) {
            const float wx = static_cast<float>(coords.x * CHUNK_SIZE_X) +
                             (static_cast<float>(x) + 0.5f) * static_cast<float>(CHUNK_SIZE_X) /
                                 static_cast<float>(resolution);
            const float wz = static_cast<float>(coords.z * CHUNK_SIZE_Z) +
                             (static_cast<float>(z) + 0.5f) * static_cast<float>(CHUNK_SIZE_Z) /
                                 static_cast<float>(resolution);
            const float bed = m_shield_system->GetTerrainHeightAt(wx, wz);
            const float rest = m_shield_system->WaterLevelAt(wx, wz);
            const auto cell = static_cast<std::size_t>(z * resolution + x);
            chunk.water_bed_mm[cell] = RoundedWaterMm(bed, false);
            chunk.water_sim_terrain_height[cell] = bed;
            if (!std::isfinite(rest))
                RefuseWater("Regional water rest level refused");
            chunk.water_rest_level[cell] = rest;
            if (initialized) {
                // Integer bilinear, weights rounded half up to 1/256, old authoritative depth.
                const auto xq =
                    (x * (old_resolution - 1) * 256 + (resolution - 1) / 2) / (resolution - 1);
                const auto zq =
                    (z * (old_resolution - 1) * 256 + (resolution - 1) / 2) / (resolution - 1);
                const int x0 = xq / 256, z0 = zq / 256;
                const int x1 = std::min(x0 + 1, old_resolution - 1);
                const int z1 = std::min(z0 + 1, old_resolution - 1);
                const std::int64_t fx = xq % 256, fz = zq % 256;
                const auto sample = [&](int sx, int sz) {
                    return static_cast<std::int64_t>(
                        old_depth[static_cast<std::size_t>(sz * old_resolution + sx)]);
                };
                chunk.water_depth_mm[cell] = static_cast<std::int32_t>(
                    ((256 - fx) * (256 - fz) * sample(x0, z0) + fx * (256 - fz) * sample(x1, z0) +
                     (256 - fx) * fz * sample(x0, z1) + fx * fz * sample(x1, z1) + 32768) /
                    65536);
            } else {
                chunk.water_depth_mm[cell] = RoundedWaterMm(static_cast<double>(rest) - bed, true);
            }
            chunk.water_level_data[cell] =
                static_cast<float>(static_cast<std::int64_t>(chunk.water_bed_mm[cell]) +
                                   chunk.water_depth_mm[cell]) /
                1000.0f;
        }
    }
    chunk.has_water_sim.store(true);
    chunk.current_water_resolution.store(resolution);
    chunk.water_mesh_generated.store(false);
    chunk.water_mesh_dirty_ticks = 0;
}

void WaterSystem::UpdateRegional(entt::registry& registry,
                                 std::span<const std::shared_ptr<Chunk>> resident,
                                 ActiveRegionLedger& ledger,
                                 const world::RegionSchedule& schedule,
                                 std::vector<world::RegionWork>& completed,
                                 std::uint64_t cell_budget) {
    if (!m_shield_system || m_shield_system->durable_read_failed())
        RefuseWater("Regional water refused after durable chunk read error");
    if (!LegalResolution(m_sim_resolution) || m_rain_mm_per_tick < 0 || m_evap_mm_per_tick < 0 ||
        m_weather_rain_scale_mm < 0 || schedule.tick != ledger.tick())
        RefuseWater("Regional water configuration refused");
    if (m_last_regional_tick && schedule.tick <= m_last_regional_tick.value())
        RefuseWater("Regional water repeated or backward tick refused");
    RegionalWork result;
    if (m_boot_paused) {
        m_regional_work = std::move(result);
        m_cells_stepped_last_update = 0;
        return;
    }
    const auto resolution = static_cast<std::uint64_t>(m_sim_resolution);
    const auto cells = resolution * resolution;
    // Each selected window covers one preparation, one kernel and all four same-owner
    // face endpoints. Reserved cross-owner faces pay two independent windows.
    const auto capacity = 2 * cells + 4 * resolution;
    result.global_windows = cell_budget / capacity;
    if (result.global_windows > world::water_share::kMaxWindowBudget)
        RefuseWater("Regional water window cap refused");
    auto& snapshot = m_regionalPlanning;
    snapshot.Refresh(resident, ledger, m_sim_resolution);
    SnapshotSources(registry, snapshot, m_sim_resolution);
    const auto& sources = snapshot.sources;
    const bool raining = m_rain_mm_per_tick > 0 || (m_weather_rain && m_weather_rain_scale_mm > 0);
    // Ascending topology indexes produce the same ascending unique ChunkID candidates.
    snapshot.PlanCandidates(ledger, schedule, raining);
    std::uint64_t awake = 0;
    for (const auto& claim : snapshot.claims)
        awake += claim.awake;
    ReserveWaterFaces(snapshot, ledger, schedule.tick, result.global_windows, awake > 0, raining);
    result.border_windows = 2 * snapshot.reserved.size();
    const auto interior_windows = result.global_windows - result.border_windows;
    auto next_ledger = ledger;
    auto debts = ledger.water_debts();
    if (!world::water_share::allocate(interior_windows, snapshot.claims, debts, result.grants) ||
        !next_ledger.set_water_debts(debts))
        RefuseWater("Regional water allocation refused");
    auto& selected = snapshot.selected;
    selected.clear();
    for (const auto& grant : result.grants) {
        result.allocated_windows += grant.windows;
        const auto* group = snapshot.FindCandidates(grant.key);
        const auto count = group ? group->ids.size() : 0;
        const auto used = std::min<std::uint64_t>(grant.windows, count);
        result.used_windows += used;
        if (used == 0)
            continue; // Includes A=0 old-debt grants; saved cursor is untouched.
        const auto& ids = group->ids;
        const auto start = ledger.records().at(grant.key).water_cursor % ids.size();
        for (std::uint64_t j = 0; j < used; ++j) {
            const auto index = ids[static_cast<std::size_t>((start + j) % ids.size())];
            auto& entry = snapshot.entries[index];
            if (!entry.selected) {
                selected.push_back(index);
                entry.selected = true;
            }
            result.visits.push_back(entry.id);
        }
        next_ledger.set_water_cursor(grant.key, (start + used) % ids.size());
    }
    result.unused_windows = result.allocated_windows - result.used_windows;
    result.unallocated_windows = interior_windows - result.allocated_windows;
    // Index order is ChunkID order, regardless of grant/cursor visitation order.
    std::sort(selected.begin(), selected.end());
    auto& executed = snapshot.executed;
    executed.assign(snapshot.reserved.begin(), snapshot.reserved.end());
    for (std::size_t i = 0; i < snapshot.faces.size(); ++i) {
        const auto& indexed = snapshot.faces[i];
        if (indexed.face.owner == indexed.face.neighbor &&
            snapshot.entries[indexed.positive].selected && snapshot.entries[indexed.other].selected)
            executed.push_back(i);
    }
    // Face indexes are already in exactly the old lexicographic Face order.
    std::sort(executed.begin(), executed.end());
    auto& staged = snapshot.staged;
    staged.clear();
    auto& work = snapshot.work;
    work.clear();
    const auto contribution_for = [&work](RegionKey owner) -> world::RegionWork& {
        auto found =
            std::lower_bound(work.begin(), work.end(), owner, [](const auto& entry, RegionKey key) {
                return entry.key < key;
            });
        if (found == work.end() || found->key != owner)
            found = work.insert(found, world::RegionWork{owner});
        return *found;
    };
    const auto stage = [&](std::size_t index) {
        auto& entry = snapshot.entries[index];
        if (!entry.staged) {
            if (!entry.workspace)
                entry.workspace = std::make_shared<Chunk>(entry.coords);
            StageWater(*entry.chunk, *entry.workspace, entry.selected && !m_finite_hydrology);
            staged.push_back(index);
            entry.staged = true;
        }
    };
    for (const auto index : selected) {
        stage(index);
        if (snapshot.entries[index].preparation)
            result.preparation_cells += cells;
    }
    for (const auto index : executed) {
        const auto& face = snapshot.faces[index];
        stage(face.positive);
        stage(face.other);
    }
    std::sort(staged.begin(), staged.end());
    result.water_cells = selected.size() * cells + executed.size() * 2 * resolution;
    result.total_cells = result.preparation_cells + result.water_cells;
    if (result.total_cells > cell_budget ||
        result.border_windows + result.allocated_windows > result.global_windows ||
        result.border_windows + result.used_windows > result.global_windows)
        RefuseWater("Regional water exact budget refused");
    // The union includes border-only participants and counts preparation/source deltas explicitly.
    std::int64_t before = 0, after = 0, injected = 0;
    for (const auto index : staged) {
        const auto& chunk = snapshot.entries[index].workspace;
        const auto depth = DepthSum(*chunk);
        before += depth;
        result.volume_before_qmm += WaterVolume(*chunk, depth);
    }
    for (const auto index : selected) {
        const auto& entry = snapshot.entries[index];
        const auto id = entry.id;
        auto& chunk = *entry.workspace;
        if (entry.preparation) {
            const auto prior = DepthSum(chunk);
            const auto prior_volume = WaterVolume(chunk, prior);
            PrepareRegionalGrid(chunk);
            const auto prepared = DepthSum(chunk);
            result.preparation_mm += prepared - prior;
            result.preparation_volume_qmm += WaterVolume(chunk, prepared) - prior_volume;
        }
        chunk.is_water_sleeping.store(false);
        for (const auto& source : sources) {
            if (source.target != id)
                continue;
            const auto depth =
                static_cast<std::int64_t>(chunk.water_depth_mm[source.cell]) + source.millimetres;
            if (depth > std::numeric_limits<std::int32_t>::max())
                RefuseWater("Regional water ECS source depth overflow");
            chunk.water_depth_mm[source.cell] = static_cast<std::int32_t>(depth);
            result.source_mm += source.millimetres;
            injected += source.millimetres;
        }
        StepRegionalInterior(chunk, result);
        auto& contribution = contribution_for(entry.owner);
        contribution.key = entry.owner;
        contribution.region_ticks = 1;
        contribution.water_cells += cells;
    }
    int wet_pairs = 0;
    for (const auto index : executed) {
        const auto& indexed = snapshot.faces[index];
        const auto& face = indexed.face;
        wet_pairs += StepSharedWaterFace(*snapshot.entries[indexed.positive].workspace,
                                         *snapshot.entries[indexed.other].workspace,
                                         face.is_x,
                                         true);
        for (const auto owner : {face.owner, face.neighbor}) {
            auto& contribution = contribution_for(owner);
            contribution.key = owner;
            contribution.region_ticks = 1;
            contribution.water_cells += resolution; // Both endpoints, including same-owner faces.
        }
    }
    for (const auto index : staged) {
        const auto& chunk = snapshot.entries[index].workspace;
        ValidateWater(*chunk);
        const auto depth = DepthSum(*chunk);
        after += depth;
        result.volume_after_qmm += WaterVolume(*chunk, depth);
    }
    const auto area_q = static_cast<std::int64_t>(4 * CHUNK_SIZE_X * CHUNK_SIZE_Z) /
                        static_cast<std::int64_t>(cells);
    result.source_volume_qmm = result.source_mm * area_q;
    result.sink_volume_qmm = result.sink_mm * area_q;
    result.mass_ok =
        after - before == result.source_mm - result.sink_mm + result.preparation_mm &&
        result.volume_after_qmm - result.volume_before_qmm ==
            result.source_volume_qmm - result.sink_volume_qmm + result.preparation_volume_qmm;
    if (!result.mass_ok)
        RefuseWater("Regional water mass invariant refused");
    if (injected > std::numeric_limits<std::int64_t>::max() - m_debug_source_injected_mm ||
        sources.size() > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max() -
                                                    m_debug_sources_seen))
        RefuseWater("Regional water diagnostic overflow");
    auto next_completed = completed;
    for (const auto& contribution : work)
        next_completed.push_back(contribution);
    auto& participants = snapshot.participants;
    participants.clear();
    participants.reserve(staged.size());
    for (const auto index : staged) {
        auto& entry = snapshot.entries[index];
        // Authority preparation reads provenance/coords only, so the second old
        // WaterChanged comparison at publication necessarily had the same result.
        entry.changed = WaterChanged(*entry.chunk, *entry.workspace);
        if (entry.changed)
            participants.push_back(entry.chunk);
    }
    auto authority = m_shield_system->prepare_water_authority_publication(participants);
    if (m_shield_system->durable_read_failed())
        RefuseWater("Regional water durable failure during preparation");
    // Everything that can allocate or refuse precedes this commit. These moves/swaps and
    // atomic/scalar writes cannot fail; no successful output is published on refusal.
    static_assert(std::is_nothrow_move_assignable_v<ActiveRegionLedger>);
    static_assert(std::is_nothrow_move_assignable_v<RegionalWork>);
    for (const auto index : staged) {
        auto& entry = snapshot.entries[index];
        PublishWater(*entry.chunk, *entry.workspace, entry.changed);
    }
    m_shield_system->publish_water_authority(std::move(authority));
    ledger = std::move(next_ledger);
    completed.swap(next_completed);
    m_debug_source_injected_mm += injected;
    m_debug_sources_seen += static_cast<std::int64_t>(sources.size());
    m_dbg_last_source_mm = result.source_mm;
    m_dbg_last_sink_mm = result.sink_mm;
    m_dbg_mass_ok = result.mass_ok;
    m_dbg_seam_wet_pairs = wet_pairs;
    m_dbg_awake_water_chunks = static_cast<std::size_t>(awake);
    m_dbg_cells_simmed = static_cast<std::size_t>(result.water_cells);
    m_cells_stepped_last_update = static_cast<std::size_t>(result.water_cells);
    m_regional_work = std::move(result);
    m_last_regional_tick = schedule.tick;
}
} // namespace Luminumbra::Systems
