#include "WaterSystem.h"

#include "../components/CoreComponents.h"
#include "../core/WaterComponents.h"
#include "../world/Chunk.h"
#include "SHIELD_WorldSystem.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <set>
#include <stdexcept>
#include <tuple>
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
    if (chunk.ticks_below_threshold < 0 || chunk.water_mesh_dirty_ticks < 0 ||
        chunk.water_mesh_dirty_ticks > 15 || !std::isfinite(chunk.max_water_delta_last_tick) ||
        chunk.max_water_delta_last_tick < 0.0f)
        RefuseWater("Regional water continuation refused");
    for (std::size_t i = 0; i < cells; ++i) {
        if (chunk.water_depth_mm[i] < 0 || !std::isfinite(chunk.water_level_data[i]) ||
            !std::isfinite(chunk.water_flow_data[i].x) ||
            !std::isfinite(chunk.water_flow_data[i].y) ||
            !std::isfinite(chunk.water_sim_terrain_height[i]))
            RefuseWater("Regional water cell refused");
    }
}

struct ResidentWater {
    std::shared_ptr<Chunk> chunk;
    RegionKey owner;
    bool preparation = false, awake = false;
};
using WaterSnapshot = std::map<ChunkID, ResidentWater>;

WaterSnapshot SnapshotWater(std::span<const std::shared_ptr<Chunk>> resident,
                            const ActiveRegionLedger& ledger,
                            int resolution) {
    WaterSnapshot snapshot;
    std::set<ChunkID> identities;
    for (const auto& chunk : resident) {
        if (!chunk || chunk->get_state() == ChunkState::Loading)
            continue;
        const auto owner = WaterOwner(*chunk);
        if (!identities.insert(chunk->get_id()).second)
            RefuseWater("Regional water duplicate resident identity");
        const auto record = ledger.records().find(owner);
        if (record == ledger.records().end() || record->second.state == RegionState::Frozen)
            continue;
        ValidateWater(*chunk);
        snapshot.emplace(
            chunk->get_id(),
            ResidentWater{chunk,
                          owner,
                          !chunk->has_water_sim.load() ||
                              chunk->current_water_resolution.load() != resolution,
                          chunk->has_water_sim.load() && !chunk->is_water_sleeping.load()});
    }
    return snapshot;
}

WaterSnapshot::const_iterator
NeighborWater(const WaterSnapshot& snapshot, const IVec3& coords, const IVec3& offset) {
    const auto neighbor = coords + offset;
    // Prevent ChunkID packing from wrapping a world-limit face onto the opposite edge.
    if (neighbor.x < -1048576 || neighbor.x >= 1048576 || neighbor.z < -1048576 ||
        neighbor.z >= 1048576)
        return snapshot.end();
    return snapshot.find(Chunk::calculate_id(neighbor));
}

struct SourceWater {
    ChunkID target;
    std::size_t cell;
    std::int32_t millimetres;
    auto operator<=>(const SourceWater&) const = default;
};

std::vector<SourceWater>
SnapshotSources(entt::registry& registry, const WaterSnapshot& snapshot, int resolution) {
    std::vector<SourceWater> result;
    const auto sources = registry.view<const Components::TransformComponent,
                                       const Components::WaterFlowSourceComponent>();
    for (const auto entity : sources) {
        const auto& position = sources.get<const Components::TransformComponent>(entity).position;
        if (!ActiveRegionLedger::valid_anchor(position) ||
            std::abs(static_cast<double>(position.y)) >= 2097152.0 * CHUNK_SIZE_Y)
            RefuseWater("Regional water source position refused");
        auto coords = SHIELD_WorldSystem::world_to_chunk_coords(position);
        auto target = snapshot.find(Chunk::calculate_id(coords));
        // Resident direct chunk first; if it has no prior grid, use the resident y=0 column.
        // Never load a parked/durable record to resolve a source.
        if (target == snapshot.end() || !target->second.chunk->has_water_sim.load()) {
            coords.y = 0;
            target = snapshot.find(Chunk::calculate_id(coords));
        }
        if (target == snapshot.end())
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
        const auto resolved = target->second.chunk->get_coords();
        const auto x = static_cast<int>(
            std::floor((static_cast<double>(position.x) - resolved.x * CHUNK_SIZE_X) * resolution /
                       CHUNK_SIZE_X));
        const auto z = static_cast<int>(
            std::floor((static_cast<double>(position.z) - resolved.z * CHUNK_SIZE_Z) * resolution /
                       CHUNK_SIZE_Z));
        if (x < 0 || x >= resolution || z < 0 || z >= resolution)
            RefuseWater("Regional water source cell refused");
        result.push_back({target->first, static_cast<std::size_t>(z * resolution + x), amount});
    }
    std::sort(result.begin(), result.end());
    return result;
}

// This pass reads only prior sleeping flags. Sources/rain make their own chunk eligible;
// newly woken neighbors cannot propagate a second hop within this tick.
std::map<RegionKey, std::vector<ChunkID>> WaterCandidates(const WaterSnapshot& snapshot,
                                                          const std::vector<SourceWater>& sources,
                                                          const ActiveRegionLedger& ledger,
                                                          const world::RegionSchedule& schedule,
                                                          bool raining) {
    std::map<RegionKey, std::vector<ChunkID>> candidates;
    for (const auto key : schedule.due) {
        const auto owner = ledger.records().find(key);
        if (owner == ledger.records().end() || owner->second.state == RegionState::Frozen ||
            !candidates.emplace(key, std::vector<ChunkID>{}).second)
            RefuseWater("Regional water due owner refused");
    }
    std::set<ChunkID> source_targets;
    for (const auto& source : sources)
        source_targets.insert(source.target);
    constexpr IVec3 offsets[] = {{1, 0, 0}, {-1, 0, 0}, {0, 0, 1}, {0, 0, -1}};
    for (const auto& [id, entry] : snapshot) {
        const auto owner = candidates.find(entry.owner);
        if (owner == candidates.end())
            continue;
        bool eligible = entry.preparation || entry.awake || raining || source_targets.contains(id);
        for (const auto& offset : offsets) {
            const auto neighbor = NeighborWater(snapshot, entry.chunk->get_coords(), offset);
            if (neighbor != snapshot.end() && neighbor->second.awake)
                eligible = true;
        }
        if (eligible)
            owner->second.push_back(id);
    }
    return candidates;
}

struct WaterFace {
    RegionKey owner, neighbor;
    ChunkID positive, other;
    bool is_x;
    auto operator<=>(const WaterFace&) const = default;
};

std::vector<WaterFace> SnapshotFaces(const WaterSnapshot& snapshot) {
    std::vector<WaterFace> faces;
    for (const auto& [id, entry] : snapshot) {
        for (const bool is_x : {true, false}) {
            const auto neighbor = NeighborWater(
                snapshot, entry.chunk->get_coords(), is_x ? IVec3(1, 0, 0) : IVec3(0, 0, 1));
            if (neighbor != snapshot.end())
                faces.push_back({entry.owner, neighbor->second.owner, id, neighbor->first, is_x});
        }
    }
    // Region-key sorting never exchanges the geometrically positive endpoint.
    std::sort(faces.begin(), faces.end());
    return faces;
}

bool CommonWaterPhase(const ActiveRegionLedger& ledger, const WaterFace& face, std::uint64_t tick) {
    const auto shift = std::max(ledger.records().at(face.owner).cadence_shift,
                                ledger.records().at(face.neighbor).cadence_shift);
    if (shift > 16)
        RefuseWater("Regional water cadence refused");
    return tick % (std::uint64_t{1} << shift) == 0;
}

std::vector<WaterFace> ReserveWaterFaces(const WaterSnapshot& snapshot,
                                         const std::vector<WaterFace>& faces,
                                         const ActiveRegionLedger& ledger,
                                         std::uint64_t tick,
                                         std::uint64_t windows,
                                         bool positive_claim,
                                         bool raining,
                                         const std::vector<SourceWater>& sources) {
    std::set<ChunkID> source_targets;
    for (const auto& source : sources)
        source_targets.insert(source.target);
    std::vector<WaterFace> reserved;
    const std::uint64_t keep = positive_claim && windows > 0 ? 1 : 0;
    for (const auto& face : faces) {
        const auto& a = snapshot.at(face.positive);
        const auto& b = snapshot.at(face.other);
        if (face.owner == face.neighbor || a.preparation || b.preparation ||
            (!a.awake && !b.awake && !raining && !source_targets.contains(face.positive) &&
             !source_targets.contains(face.other)) ||
            !CommonWaterPhase(ledger, face, tick))
            continue;
        if (windows - keep < 2)
            continue;
        reserved.push_back(face);
        windows -= 2;
    }
    return reserved;
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

std::int64_t WaterVolume(const Chunk& chunk) {
    if (!chunk.has_water_sim.load())
        return 0;
    const auto resolution = static_cast<std::int64_t>(chunk.current_water_resolution.load());
    // A chunk holds at most INT32_MAX * 1024 qmm. The window cap bounds the
    // participant union by 2^20 chunks, so all whole-plan volume totals fit i64.
    const auto cell_area =
        4 * static_cast<std::int64_t>(CHUNK_SIZE_X) * CHUNK_SIZE_Z / (resolution * resolution);
    return DepthSum(chunk) * cell_area;
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
    const auto snapshot = SnapshotWater(resident, ledger, m_sim_resolution);
    const auto sources = SnapshotSources(registry, snapshot, m_sim_resolution);
    const bool raining = m_rain_mm_per_tick > 0 || (m_weather_rain && m_weather_rain_scale_mm > 0);
    // Construct this executable ascending unique array once, before any claims or mutations.
    const auto candidates = WaterCandidates(snapshot, sources, ledger, schedule, raining);
    std::vector<world::water_share::Claim> claims;
    std::uint64_t awake = 0;
    for (const auto& [owner, ids] : candidates) {
        claims.push_back({owner, ids.size()});
        awake += ids.size();
    }
    const auto faces = SnapshotFaces(snapshot);
    const auto reserved = ReserveWaterFaces(
        snapshot, faces, ledger, schedule.tick, result.global_windows, awake > 0, raining, sources);
    result.border_windows = 2 * reserved.size();
    const auto interior_windows = result.global_windows - result.border_windows;
    auto next_ledger = ledger;
    auto debts = ledger.water_debts();
    if (!world::water_share::allocate(interior_windows, claims, debts, result.grants) ||
        !next_ledger.set_water_debts(debts))
        RefuseWater("Regional water allocation refused");
    std::set<ChunkID> selected;
    for (const auto& grant : result.grants) {
        result.allocated_windows += grant.windows;
        const auto& ids = candidates.at(grant.key);
        const auto used = std::min<std::uint64_t>(grant.windows, ids.size());
        result.used_windows += used;
        if (used == 0)
            continue; // Includes A=0 old-debt grants; saved cursor is untouched.
        const auto start = ledger.records().at(grant.key).water_cursor % ids.size();
        for (std::uint64_t j = 0; j < used; ++j) {
            const auto id = ids[static_cast<std::size_t>((start + j) % ids.size())];
            selected.insert(id);
            result.visits.push_back(id);
        }
        next_ledger.set_water_cursor(grant.key, (start + used) % ids.size());
    }
    result.unused_windows = result.allocated_windows - result.used_windows;
    result.unallocated_windows = interior_windows - result.allocated_windows;
    std::vector<WaterFace> executed = reserved;
    for (const auto& face : faces) {
        if (face.owner == face.neighbor && selected.contains(face.positive) &&
            selected.contains(face.other))
            executed.push_back(face);
    }
    std::sort(executed.begin(), executed.end());
    // Workspace mutation cannot publish live state, including after a refusal.
    // Prune against this tick's validated snapshot so streaming/freeze transitions
    // cannot retain an unbounded history of chunk buffers.
    std::erase_if(m_regionalStaging,
                  [&snapshot](const auto& entry) { return !snapshot.contains(entry.first); });
    std::map<ChunkID, Chunk*> staged;
    std::map<RegionKey, world::RegionWork> work;
    const auto stage = [&](ChunkID id) -> Chunk& {
        auto found = staged.find(id);
        if (found == staged.end()) {
            auto& workspace = m_regionalStaging[id];
            const auto& original = *snapshot.at(id).chunk;
            if (!workspace)
                workspace = std::make_shared<Chunk>(original.get_coords());
            StageWater(original, *workspace, selected.contains(id) && !m_finite_hydrology);
            found = staged.emplace(id, workspace.get()).first;
        }
        return *found->second;
    };
    for (const auto id : selected) {
        stage(id);
        if (snapshot.at(id).preparation)
            result.preparation_cells += cells;
    }
    for (const auto& face : executed) {
        stage(face.positive);
        stage(face.other);
    }
    result.water_cells = selected.size() * cells + executed.size() * 2 * resolution;
    result.total_cells = result.preparation_cells + result.water_cells;
    if (result.total_cells > cell_budget ||
        result.border_windows + result.allocated_windows > result.global_windows ||
        result.border_windows + result.used_windows > result.global_windows)
        RefuseWater("Regional water exact budget refused");
    // The union includes border-only participants and counts preparation/source deltas explicitly.
    std::int64_t before = 0, after = 0, injected = 0;
    for (const auto& [id, chunk] : staged) {
        before += DepthSum(*chunk);
        result.volume_before_qmm += WaterVolume(*chunk);
        (void)id;
    }
    for (const auto id : selected) {
        auto& chunk = *staged.at(id);
        const auto& entry = snapshot.at(id);
        if (entry.preparation) {
            const auto prior = DepthSum(chunk);
            const auto prior_volume = WaterVolume(chunk);
            PrepareRegionalGrid(chunk);
            result.preparation_mm += DepthSum(chunk) - prior;
            result.preparation_volume_qmm += WaterVolume(chunk) - prior_volume;
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
        auto& contribution = work[entry.owner];
        contribution.key = entry.owner;
        contribution.region_ticks = 1;
        contribution.water_cells += cells;
    }
    int wet_pairs = 0;
    for (const auto& face : executed) {
        wet_pairs +=
            StepSharedWaterFace(*staged.at(face.positive), *staged.at(face.other), face.is_x, true);
        for (const auto owner : {face.owner, face.neighbor}) {
            auto& contribution = work[owner];
            contribution.key = owner;
            contribution.region_ticks = 1;
            contribution.water_cells += resolution; // Both endpoints, including same-owner faces.
        }
    }
    for (const auto& [id, chunk] : staged) {
        ValidateWater(*chunk);
        after += DepthSum(*chunk);
        result.volume_after_qmm += WaterVolume(*chunk);
        (void)id;
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
    for (const auto& [owner, contribution] : work) {
        next_completed.push_back(contribution);
        (void)owner;
    }
    std::vector<Chunk*> participants;
    participants.reserve(staged.size());
    for (const auto& [id, chunk] : staged) {
        if (WaterChanged(*snapshot.at(id).chunk, *chunk))
            participants.push_back(snapshot.at(id).chunk.get());
    }
    auto authority = m_shield_system->prepare_water_authority_publication(participants);
    if (m_shield_system->durable_read_failed())
        RefuseWater("Regional water durable failure during preparation");
    // Everything that can allocate or refuse precedes this commit. These moves/swaps and
    // atomic/scalar writes cannot fail; no successful output is published on refusal.
    static_assert(std::is_nothrow_move_assignable_v<ActiveRegionLedger>);
    static_assert(std::is_nothrow_move_assignable_v<RegionalWork>);
    for (const auto& [id, chunk] : staged)
        PublishWater(*snapshot.at(id).chunk, *chunk, WaterChanged(*snapshot.at(id).chunk, *chunk));
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
