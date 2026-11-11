#include "FarWorldAuthorityProducer.h"
#include "Chunk.h"
#include "systems/SHIELD_WorldSystem.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <map>
#include <mutex>
#include <new>
#include <tuple>
#include <utility>

namespace Luminumbra::World {
using Save = Persistence::WorldSaveService;
using Producer = FarWorldAuthorityProducer;
struct FarWorldAuthorityState {
    struct Members {
        std::shared_ptr<Chunk> live, parked;
    };
    struct Change {
        IVec3 coords{};
        FarWorldAuthorityChange kind{};
    };
    explicit FarWorldAuthorityState(const Producer::Limits& source_limits)
        : limits(source_limits)
        , index(FarVolumePoolAllocator<IndexEntry>(source_limits.memory)) {
        // Attachment replaces the world's old change sink. Admit every index
        // node before that boundary so membership cannot allocate afterwards.
        if (limits.index_records <= Producer::kSlots) {
            for (std::size_t i = 0; i < limits.index_records; ++i) {
                const auto entry = index.emplace(std::tuple{0, 0, 0}, Members{}).first;
                spare_nodes[spare_count++] = index.extract(entry);
            }
        }
    }
    Producer::Limits limits;
    mutable std::mutex mutex;
    // At most limits.index_records nodes, capped at the fixed 512-slot envelope.
    // Column lookup probes bounded coordinates and visits only participating Y layers.
    using IndexEntry = std::pair<const std::tuple<int, int, int>, Members>;
    std::map<std::tuple<int, int, int>, Members, std::less<>, FarVolumePoolAllocator<IndexEntry>>
        index;
    // At most index_records nodes, including unused capacity. Extraction and
    // reinsertion preserve their allocator and require no further allocation.
    std::array<decltype(index)::node_type, Producer::kSlots> spare_nodes{};
    std::size_t spare_count = 0;
    std::array<Save::AuthorityWindow, Producer::kSlots> wanted{};
    std::array<Change, Producer::kSlots> pending{};
    std::size_t wanted_count = 0, pending_count = 0;
    bool overflow = false, all_dirty = false, index_failed = false;
    Producer::Counters counters;
};
namespace {
FarVolumeMemoryLease AdmitProducerControl(const Producer::Limits& limits) {
    if (!limits.memory) {
        throw FarVolumeMemoryRefused();
    }
    FarVolumeMemoryLease control(*limits.memory);
    if (!control.TryCharge(sizeof(Producer))) {
        throw FarVolumeMemoryRefused();
    }
    return control;
}
bool ValidLimits(const Producer::Limits& limits) {
    return limits.index_records > 0 && limits.index_records <= Producer::kSlots &&
           limits.copy_bytes > 0 && limits.query_columns > 0 && limits.memory;
}
bool ValidWindow(const Save::AuthorityWindow& window, std::uint64_t columns) {
    if (window.min_chunk_x < Chunk::kPackedMinXz || window.max_chunk_x > Chunk::kPackedMaxXz ||
        window.min_chunk_z < Chunk::kPackedMinXz || window.max_chunk_z > Chunk::kPackedMaxXz ||
        window.min_chunk_x > window.max_chunk_x || window.min_chunk_z > window.max_chunk_z) {
        return false;
    }
    const auto width = static_cast<std::uint64_t>(static_cast<std::int64_t>(window.max_chunk_x) -
                                                  window.min_chunk_x + 1);
    const auto depth = static_cast<std::uint64_t>(static_cast<std::int64_t>(window.max_chunk_z) -
                                                  window.min_chunk_z + 1);
    return width <= columns / depth;
}
bool Contains(const Save::AuthorityWindow& window, const IVec3& coords) {
    return coords.x >= window.min_chunk_x && coords.x <= window.max_chunk_x &&
           coords.z >= window.min_chunk_z && coords.z <= window.max_chunk_z;
}
void NotifyLocked(FarWorldAuthorityState& state,
                  const IVec3& coords,
                  FarWorldAuthorityChange change) {
    if (state.overflow) {
        return;
    }
    bool wanted = false;
    for (std::size_t slot = 0; slot < state.wanted_count; ++slot) {
        wanted = wanted || Contains(state.wanted[slot], coords);
    }
    if (!wanted) {
        return;
    }
    if (state.pending_count == Producer::kSlots) {
        state.overflow = true;
        ++state.counters.dirty_overflows;
        return;
    }
    state.pending[state.pending_count++] = {.coords = coords, .kind = change};
}
bool Eligible(const std::shared_ptr<Chunk>& chunk) {
    if (!chunk || !chunk->has_simulation_authority()) {
        return false;
    }
    const auto state = chunk->get_state();
    return state == ChunkState::Idle || state == ChunkState::Ready || state == ChunkState::Meshing;
}
bool Charge(std::uint64_t count, std::uint64_t size, std::uint64_t limit, std::uint64_t& bytes) {
    if (count > (limit - bytes) / size) {
        return false;
    }
    bytes += count * size;
    return true;
}
Producer::Error
Validate(const Chunk& chunk, bool terrain, bool water, std::uint64_t limit, std::uint64_t& bytes) {
    constexpr std::size_t samples =
        static_cast<std::size_t>(CHUNK_SIZE_X + 1) * (CHUNK_SIZE_Y + 1) * (CHUNK_SIZE_Z + 1);
    if (terrain) {
        if (chunk.sdf_data.size() != samples ||
            (!chunk.material_data.empty() && chunk.material_data.size() != samples) ||
            !std::all_of(chunk.sdf_data.begin(), chunk.sdf_data.end(), [](float value) {
                return std::isfinite(value);
            })) {
            return Producer::Error::AuthorityUnavailable;
        }
        if (!Charge(chunk.sdf_data.size(), sizeof(float), limit, bytes) ||
            !Charge(chunk.material_data.size(), sizeof(std::uint8_t), limit, bytes)) {
            return Producer::Error::CopyLimit;
        }
    }
    if (water) {
        const int resolution = chunk.current_water_resolution.load();
        if (resolution != 4 && resolution != 8 && resolution != 16 && resolution != 32) {
            return Producer::Error::WaterUnavailable;
        }
        // Resolution validation bounds multiplication and all water traversal to 1,024 cells.
        const auto cells =
            static_cast<std::size_t>(resolution) * static_cast<std::size_t>(resolution);
        if (chunk.water_bed_mm.size() != cells || chunk.water_depth_mm.size() != cells ||
            std::any_of(chunk.water_depth_mm.begin(),
                        chunk.water_depth_mm.end(),
                        [](std::int32_t depth) { return depth < 0; })) {
            return Producer::Error::WaterUnavailable;
        }
        if (!Charge(cells, 2u * sizeof(std::int32_t), limit, bytes)) {
            return Producer::Error::CopyLimit;
        }
    }
    return Producer::Error::None;
}

std::int64_t Floor(std::int64_t value, int edge) {
    const auto quotient = value / edge;
    return quotient - (value % edge < 0 ? 1 : 0);
}
struct FrozenRecord {
    IVec3 coords{};
    unsigned layer = 0;
    bool terrain = false;
    int resolution = 0;
    std::span<const float> sdf;
    std::span<const std::uint8_t> material;
    std::span<const std::int32_t> bed, depth;
};
auto RecordKey(const FrozenRecord& record) {
    return std::tuple(record.layer, record.coords.x, record.coords.z, record.coords.y);
}
bool ValidRecord(const FrozenRecord& record) {
    constexpr std::size_t samples =
        std::size_t{CHUNK_SIZE_X + 1} * (CHUNK_SIZE_Y + 1) * (CHUNK_SIZE_Z + 1);
    return !record.terrain || (record.sdf.size() == samples &&
                               (record.material.empty() || record.material.size() == samples) &&
                               std::all_of(record.sdf.begin(), record.sdf.end(), [](float value) {
                                   return std::isfinite(value);
                               }));
}
bool ValidWater(const FrozenRecord& record) {
    if (record.resolution != 4 && record.resolution != 8 && record.resolution != 16 &&
        record.resolution != 32) {
        return false;
    }
    const auto cells = std::size_t(record.resolution) * std::size_t(record.resolution);
    return record.bed.size() == cells && record.depth.size() == cells &&
           std::all_of(record.depth.begin(), record.depth.end(), [](std::int32_t value) {
               return value >= 0;
           });
}
FarVolumeSample Node(const FrozenRecord& record, const FarVolumePosition& p) {
    const auto index = static_cast<std::size_t>(
        p.x - std::int64_t(record.coords.x) * CHUNK_SIZE_X +
        (CHUNK_SIZE_X + 1) *
            (p.y - std::int64_t(record.coords.y) * CHUNK_SIZE_Y +
             (CHUNK_SIZE_Y + 1) * (p.z - std::int64_t(record.coords.z) * CHUNK_SIZE_Z)));
    const auto density = record.sdf[index];
    return {density == 0 ? 0.0f : density,
            record.material.empty() ? std::uint8_t{0} : record.material[index]};
}
bool SharedNodesAgree(std::span<const FrozenRecord> records, const FrozenRecord& record) {
    // At most 26 neighbours per record; each intersection has at most 17x17
    // lod-0 nodes. Validate even nodes skipped by the requested far-tier lattice.
    for (int dz = -1; dz <= 1; ++dz) {
        for (int dy = -1; dy <= 1; ++dy) {
            for (int dx = -1; dx <= 1; ++dx) {
                const auto key = std::tuple(record.layer,
                                            std::int64_t(record.coords.x) + dx,
                                            std::int64_t(record.coords.z) + dz,
                                            std::int64_t(record.coords.y) + dy);
                if (key <= RecordKey(record)) {
                    continue;
                }
                const auto found = std::lower_bound(records.begin(),
                                                    records.end(),
                                                    key,
                                                    [](const auto& candidate, const auto& value) {
                                                        return RecordKey(candidate) < value;
                                                    });
                if (found == records.end() || RecordKey(*found) != key || !found->terrain) {
                    continue;
                }
                const FarVolumePosition lo{
                    std::int64_t(std::max(record.coords.x, found->coords.x)) * CHUNK_SIZE_X,
                    std::int64_t(std::max(record.coords.y, found->coords.y)) * CHUNK_SIZE_Y,
                    std::int64_t(std::max(record.coords.z, found->coords.z)) * CHUNK_SIZE_Z};
                const FarVolumePosition hi{
                    (std::int64_t(std::min(record.coords.x, found->coords.x)) + 1) * CHUNK_SIZE_X,
                    (std::int64_t(std::min(record.coords.y, found->coords.y)) + 1) * CHUNK_SIZE_Y,
                    (std::int64_t(std::min(record.coords.z, found->coords.z)) + 1) * CHUNK_SIZE_Z};
                for (auto z = lo.z; z <= hi.z; ++z) {
                    for (auto y = lo.y; y <= hi.y; ++y) {
                        for (auto x = lo.x; x <= hi.x; ++x) {
                            if (Node(record, {x, y, z}) != Node(*found, {x, y, z})) {
                                return false;
                            }
                        }
                    }
                }
            }
        }
    }
    return true;
}
FarFrozenOverride<FarVolumeSample> TerrainAt(std::span<const FrozenRecord> records,
                                             unsigned layer,
                                             const FarVolumePosition& position) {
    FarFrozenOverride<FarVolumeSample> result;
    const auto cx = Floor(position.x, CHUNK_SIZE_X), cy = Floor(position.y, CHUNK_SIZE_Y),
               cz = Floor(position.z, CHUNK_SIZE_Z);
    // Closed lod-0 lattices: at most eight contributors, including negative boundaries.
    for (auto z = cz - (position.z % CHUNK_SIZE_Z == 0 ? 1 : 0); z <= cz; ++z) {
        for (auto y = cy - (position.y % CHUNK_SIZE_Y == 0 ? 1 : 0); y <= cy; ++y) {
            for (auto x = cx - (position.x % CHUNK_SIZE_X == 0 ? 1 : 0); x <= cx; ++x) {
                const auto key = std::tuple(layer, x, z, y);
                const auto found = std::lower_bound(
                    records.begin(), records.end(), key, [](const auto& record, const auto& value) {
                        return RecordKey(record) < value;
                    });
                if (found == records.end() || RecordKey(*found) != key || !found->terrain) {
                    continue;
                }
                // Every requested position is an integer lod-0 node. This is the
                // existing edited interpolation's exact-node and material-tie result.
                const auto sample = Node(*found, position);
                if (result.presence == FarFrozenPresence::Present && result.sample != sample) {
                    return {FarFrozenPresence::Unavailable, {}};
                }
                result = {FarFrozenPresence::Present, sample};
            }
        }
    }
    return result;
}
FarFrozenOverride<FarFrozenWaterSample> WaterAt(std::span<const FrozenRecord* const> records,
                                                unsigned layer,
                                                std::int64_t x,
                                                std::int64_t z) {
    const auto cx = Floor(x, CHUNK_SIZE_X), cz = Floor(z, CHUNK_SIZE_Z);
    const auto key = std::tuple(layer, cx, cz);
    const auto found = std::lower_bound(
        records.begin(), records.end(), key, [](const auto* record, const auto& value) {
            return std::tuple(record->layer, record->coords.x, record->coords.z) < value;
        });
    if (found == records.end() ||
        std::tuple((*found)->layer, (*found)->coords.x, (*found)->coords.z) != key) {
        return {};
    }
    const auto& record = **found;
    // Water cells own half-open XZ supports. Exact chunk edges select the
    // positive-side owner, independently of lookup order or residency.
    const auto ix = (x - cx * CHUNK_SIZE_X) * record.resolution / CHUNK_SIZE_X;
    const auto iz = (z - cz * CHUNK_SIZE_Z) * record.resolution / CHUNK_SIZE_Z;
    const auto cell = static_cast<std::size_t>(ix + iz * record.resolution);
    const bool wet = record.depth[cell] > 0;
    return {
        FarFrozenPresence::Present,
        {wet, static_cast<float>(std::int64_t(record.bed[cell]) + record.depth[cell]) / 1000.0f}};
}
std::optional<FarFrozenWaterSample> BaseWater(const Systems::SHIELD_WorldSystem& world,
                                              int resolution,
                                              std::int64_t x,
                                              std::int64_t z) {
    const auto center = [resolution](std::int64_t value, int edge) {
        const auto chunk = Floor(value, edge);
        const auto cell = (value - chunk * edge) * resolution / edge;
        return static_cast<float>(chunk * edge) +
               (static_cast<float>(cell) + 0.5f) *
                   (static_cast<float>(edge) / static_cast<float>(resolution));
    };
    const auto wx = center(x, CHUNK_SIZE_X), wz = center(z, CHUNK_SIZE_Z);
    const float terrain = world.GetTerrainHeightAt(wx, wz), rest = world.WaterLevelAt(wx, wz);
    if (!std::isfinite(terrain) || !std::isfinite(rest)) {
        return {};
    }
    const auto quantize = [](double value) {
        return static_cast<std::int32_t>(
            std::clamp(value,
                       static_cast<double>(std::numeric_limits<std::int32_t>::min()),
                       static_cast<double>(std::numeric_limits<std::int32_t>::max())));
    };
    const auto bed = quantize(std::round(terrain * 1000.0f));
    const auto depth =
        std::max(0, quantize(std::round(static_cast<double>(rest - terrain) * 1000)));
    return FarFrozenWaterSample{depth > 0, static_cast<float>(std::int64_t(bed) + depth) / 1000.0f};
}
} // namespace

FarWorldAuthorityChanges::FarWorldAuthorityChanges(std::shared_ptr<FarWorldAuthorityState> state)
    : state_(std::move(state)) {}
void FarWorldAuthorityChanges::notify(const IVec3& coords, FarWorldAuthorityChange change) {
    const std::lock_guard lock(state_->mutex);
    NotifyLocked(*state_, coords, change);
}
std::size_t FarWorldAuthorityChanges::record_limit() const {
    return state_->limits.index_records;
}
void FarWorldAuthorityChanges::membership(const std::shared_ptr<Chunk>& chunk,
                                          bool parked,
                                          bool present) {
    if (!chunk) {
        return;
    }
    const std::lock_guard lock(state_->mutex);
    const auto& coords = chunk->get_coords();
    const auto key = std::tuple(coords.x, coords.z, coords.y);
    auto member = state_->index.find(key);
    if (present) {
        chunk->bind_far_world_authority_changes(weak_from_this());
        if (member == state_->index.end()) {
            if (!ValidLimits(state_->limits) ||
                state_->index.size() >= state_->limits.index_records) {
                state_->index_failed = true;
                state_->all_dirty = true;
                return;
            }
            auto node = std::move(state_->spare_nodes[--state_->spare_count]);
            node.key() = key;
            member = state_->index.insert(std::move(node)).position;
        }
        auto& destination = parked ? member->second.parked : member->second.live;
        if (destination == chunk) {
            return;
        }
        const auto& other = parked ? member->second.live : member->second.parked;
        if (destination && destination != other) {
            destination->bind_far_world_authority_changes({});
        }
        destination = chunk;
    } else {
        if (member == state_->index.end()) {
            return;
        }
        auto& destination = parked ? member->second.parked : member->second.live;
        if (destination != chunk) {
            return;
        }
        destination.reset();
        if (member->second.live != chunk && member->second.parked != chunk) {
            chunk->bind_far_world_authority_changes({});
        }
        if (!member->second.live && !member->second.parked) {
            state_->spare_nodes[state_->spare_count++] = state_->index.extract(member);
        }
    }
    NotifyLocked(*state_, coords, FarWorldAuthorityChange::Membership);
}
void FarWorldAuthorityChanges::clear() {
    const std::lock_guard lock(state_->mutex);
    for (const auto& [key, members] : state_->index) {
        (void)key;
        if (members.live) {
            members.live->bind_far_world_authority_changes({});
        }
        if (members.parked) {
            members.parked->bind_far_world_authority_changes({});
        }
    }
    for (auto entry = state_->index.begin(); entry != state_->index.end();) {
        auto node = state_->index.extract(entry++);
        node.mapped() = {};
        state_->spare_nodes[state_->spare_count++] = std::move(node);
    }
    state_->index_failed = false;
    state_->pending_count = 0;
    state_->overflow = false;
    state_->all_dirty = true;
}
struct FarWorldAuthorityProducer::Impl {
    explicit Impl(const Limits& limits)
        : state(std::allocate_shared<FarWorldAuthorityState>(
              FarVolumePoolAllocator<FarWorldAuthorityState>(limits.memory), limits))
        , changes(std::allocate_shared<FarWorldAuthorityChanges>(
              FarVolumePoolAllocator<FarWorldAuthorityChanges>(limits.memory), state))
        , reader(limits.memory) {}
    std::shared_ptr<FarWorldAuthorityState> state;
    std::shared_ptr<FarWorldAuthorityChanges> changes;
    Save::AuthorityWindowReader reader;
    CaptureLimits capture_limits{};
    Save::AuthorityWindow capture_window{};
    // At most one active and one serving capture. Volatile arrays in each are
    // bounded by copy_bytes; committed arrays and parser scratch by read limits.
    // The successor retains only the fixed wanted descriptors, never source data.
    std::shared_ptr<SourceCapture> active, serving;
    std::uint64_t serial = 0;
    std::uint32_t age = 0;
    Error error = Error::None;
    // Diagnostic storage is included in the admitted Impl allocation.
    std::array<char, 256> read_error{};
    std::size_t read_error_size = 0;
    void set_read_error(std::string_view value) {
        read_error_size = std::min(value.size(), read_error.size() - 1);
        std::copy_n(value.begin(), read_error_size, read_error.begin());
    }
    bool configured = false, source_ready = false, queued = false, paused = false,
         requested = false;
};
FarWorldAuthorityProducer::FarWorldAuthorityProducer(const Limits& limits)
    : FarWorldAuthorityProducer(limits, AdmitProducerControl(limits)) {}
FarWorldAuthorityProducer::FarWorldAuthorityProducer(const Limits& limits,
                                                     FarVolumeMemoryLease&& control)
    : memory_(limits.memory)
    , control_(std::move(control)) {
    if (!memory_ || control_.pool() != memory_.get() || control_.bytes() != sizeof(Producer)) {
        throw FarVolumeMemoryRefused();
    }
    impl_ = std::allocate_shared<Impl>(FarVolumePoolAllocator<Impl>(memory_), limits);
}
FarWorldAuthorityProducer::~FarWorldAuthorityProducer() = default;
std::shared_ptr<FarWorldAuthorityChanges> FarWorldAuthorityProducer::changes() const {
    return impl_->changes;
}
bool FarWorldAuthorityProducer::set_wanted(std::span<const Save::AuthorityWindow> wanted) {
    auto& state = *impl_->state;
    if (!ValidLimits(state.limits) || wanted.size() > kSlots ||
        !std::all_of(wanted.begin(), wanted.end(), [&](const auto& window) {
            return ValidWindow(window, state.limits.query_columns);
        })) {
        return false;
    }
    const std::lock_guard lock(state.mutex);
    std::copy(wanted.begin(), wanted.end(), state.wanted.begin());
    state.wanted_count = wanted.size();
    state.pending_count = 0;
    state.overflow = false;
    state.all_dirty = true;
    return true;
}
FarWorldAuthorityProducer::DirtyTiles FarWorldAuthorityProducer::tick() {
    auto& state = *impl_->state;
    const std::lock_guard lock(state.mutex);
    DirtyTiles dirty;
    dirty.overflow = state.overflow;
    if (state.overflow || state.all_dirty) {
        for (std::size_t slot = 0; slot < state.wanted_count; ++slot) {
            dirty.tiles.set(slot);
        }
    }
    for (std::size_t item = 0; item < state.pending_count; ++item) {
        const auto& change = state.pending[item];
        ++dirty.visited;
        dirty.terrain += change.kind == FarWorldAuthorityChange::Terrain ? 1u : 0u;
        dirty.water += change.kind == FarWorldAuthorityChange::Water ? 1u : 0u;
        for (std::size_t slot = 0; slot < state.wanted_count; ++slot) {
            if (Contains(state.wanted[slot], change.coords)) {
                dirty.tiles.set(slot);
            }
        }
    }
    state.counters.tick_items += dirty.visited;
    state.pending_count = 0;
    state.overflow = false;
    state.all_dirty = false;
    return dirty;
}
FarWorldAuthorityProducer::Counters FarWorldAuthorityProducer::counters() const {
    const std::lock_guard lock(impl_->state->mutex);
    return impl_->state->counters;
}
FarWorldAuthorityProducer::Error
FarWorldAuthorityProducer::copy_published(const Save::AuthorityWindow& window, Images& output) {
    auto& state = *impl_->state;
    const std::lock_guard lock(state.mutex);
    if (!ValidLimits(state.limits)) {
        return Error::InvalidLimits;
    }
    if (!ValidWindow(window, state.limits.query_columns)) {
        return Error::InvalidWindow;
    }
    if (state.index_failed) {
        return Error::IndexCapacity;
    }
    struct Selected {
        std::shared_ptr<Chunk> chunk;
        bool parked = false, terrain = false, water = false;
    };
    FarVolumeMemoryLease scratch(*state.limits.memory);
    if (!scratch.TryCharge(sizeof(std::array<Selected, 2u * kSlots>))) {
        return Error::AllocationFailure;
    }
    // Each indexed coordinate can contribute distinct terrain and water owners.
    std::array<Selected, 2u * kSlots> selected{};
    std::size_t count = 0;
    std::uint64_t bytes = 0;
    // Coordinate probes are capped by query_columns; records by index_records.
    for (int x = window.min_chunk_x; x <= window.max_chunk_x; ++x) {
        for (int z = window.min_chunk_z; z <= window.max_chunk_z; ++z) {
            bool water_seen = false;
            auto member = state.index.lower_bound({x, z, std::numeric_limits<int>::min()});
            for (; member != state.index.end() && std::get<0>(member->first) == x &&
                   std::get<1>(member->first) == z;
                 ++member) {
                ++state.counters.query_items;
                const auto& sources = member->second;
                const bool live = Eligible(sources.live), parked = Eligible(sources.parked);
                const auto terrain = live && sources.live->has_terrain_authority() ? sources.live
                                     : parked && sources.parked->has_terrain_authority()
                                         ? sources.parked
                                         : nullptr;
                const auto water = live && sources.live->has_water_authority() ? sources.live
                                   : parked && sources.parked->has_water_authority()
                                       ? sources.parked
                                       : nullptr;
                if (water && std::exchange(water_seen, true)) {
                    return Error::WaterUnavailable;
                }
                for (const auto& chunk : {terrain, water == terrain ? nullptr : water}) {
                    if (!chunk) {
                        continue;
                    }
                    const bool owns_terrain = chunk == terrain, owns_water = chunk == water;
                    if (!Charge(1, sizeof(Image), state.limits.copy_bytes, bytes)) {
                        return Error::CopyLimit;
                    }
                    const auto error =
                        Validate(*chunk, owns_terrain, owns_water, state.limits.copy_bytes, bytes);
                    if (error != Error::None) {
                        return error;
                    }
                    selected[count++] = {.chunk = chunk,
                                         .parked = chunk == sources.parked && chunk != sources.live,
                                         .terrain = owns_terrain,
                                         .water = owns_water};
                }
            }
        }
    }
    try {
        Images images{FarVolumePoolAllocator<Image>(state.limits.memory)};
        images.reserve(count);
        for (std::size_t item = 0; item < count; ++item) {
            const auto& source = selected[item];
            const auto& chunk = *source.chunk;
            Image image{
                .coords = chunk.get_coords(),
                .parked = source.parked,
                .terrain_authority = source.terrain,
                .water_authority = source.water,
                .water_resolution = 0,
                .sdf = decltype(Image::sdf)(FarVolumePoolAllocator<float>(state.limits.memory)),
                .material = decltype(Image::material)(
                    FarVolumePoolAllocator<std::uint8_t>(state.limits.memory)),
                .bed =
                    decltype(Image::bed)(FarVolumePoolAllocator<std::int32_t>(state.limits.memory)),
                .depth = decltype(Image::depth)(
                    FarVolumePoolAllocator<std::int32_t>(state.limits.memory))};
            if (image.terrain_authority) {
                image.sdf.assign(chunk.sdf_data.begin(), chunk.sdf_data.end());
                image.material.assign(chunk.material_data.begin(), chunk.material_data.end());
            }
            if (image.water_authority) {
                image.water_resolution = chunk.current_water_resolution.load();
                image.bed.assign(chunk.water_bed_mm.begin(), chunk.water_bed_mm.end());
                image.depth.assign(chunk.water_depth_mm.begin(), chunk.water_depth_mm.end());
            }
            images.push_back(std::move(image));
        }
        state.counters.copied_records += count;
        state.counters.copied_bytes += bytes;
        output.swap(images);
        return Error::None;
    } catch (const std::bad_alloc&) {
        return Error::AllocationFailure;
    }
}
bool FarWorldAuthorityProducer::admit_saved_source(const std::filesystem::path& root,
                                                   const Save::AuthorityReadLimits& limits,
                                                   std::string& error) {
    if (impl_->paused) {
        error = "Source reads are paused.";
        return false;
    }
    if (!impl_->reader.admit(root, limits, error)) {
        impl_->error = Error::CommittedReadRefused;
        impl_->set_read_error(error);
        return false;
    }
    impl_->source_ready = true;
    if (impl_->requested) {
        impl_->active.reset();
        impl_->queued = true;
    }
    return true;
}
Save::AuthorityWindowResult
FarWorldAuthorityProducer::read_committed(const Save::AuthorityWindow& window,
                                          const Save::AuthorityReadLimits& limits) const {
    if (impl_->paused || !impl_->source_ready) {
        Save::AuthorityWindowResult refused;
        // Fits inline storage on supported standard libraries, including after
        // the source memory domain has refused all further allocations.
        refused.error = impl_->paused ? "source paused" : "no source";
        return refused;
    }
    return impl_->reader.read(window, limits);
}
bool FarWorldAuthorityProducer::configure_capture(const CaptureLimits& limits) {
    const auto& read = limits.read;
    if (!ValidLimits(impl_->state->limits) || limits.deadline_frames == 0 || read.members == 0 ||
        read.records == 0 || read.headers == 0 || read.probes == 0 ||
        read.compressed_member_bytes == 0 || read.compressed_total_bytes == 0 ||
        read.decoded_member_bytes == 0 || read.decoded_total_bytes == 0 ||
        read.control_bytes == 0 || read.compressed_member_bytes > read.compressed_total_bytes ||
        read.decoded_member_bytes > read.decoded_total_bytes || impl_->active) {
        return false;
    }
    impl_->capture_limits = limits;
    impl_->configured = true;
    return true;
}

bool FarWorldAuthorityProducer::request_capture(std::span<const Save::AuthorityWindow> windows) {
    if (!impl_->configured || !impl_->source_ready || windows.empty() || windows.size() > kSlots) {
        return false;
    }
    auto combined = windows.front();
    for (const auto& window : windows) {
        if (!ValidWindow(window, impl_->state->limits.query_columns)) {
            return false;
        }
        combined.min_chunk_x = std::min(combined.min_chunk_x, window.min_chunk_x);
        combined.max_chunk_x = std::max(combined.max_chunk_x, window.max_chunk_x);
        combined.min_chunk_z = std::min(combined.min_chunk_z, window.min_chunk_z);
        combined.max_chunk_z = std::max(combined.max_chunk_z, window.max_chunk_z);
    }
    if (!ValidWindow(combined, impl_->state->limits.query_columns) || !set_wanted(windows)) {
        return false;
    }
    impl_->requested = true;
    impl_->capture_window = combined;
    impl_->queued = true;
    return true;
}

bool FarWorldAuthorityProducer::needs_capture_boundary() const {
    if (!impl_->requested || !impl_->source_ready || impl_->paused || impl_->active) {
        return false;
    }
    const auto& state = *impl_->state;
    const std::lock_guard lock(state.mutex);
    return impl_->queued || state.all_dirty || state.overflow || state.pending_count != 0;
}

void FarWorldAuthorityProducer::pump_capture() {
    if (!impl_->requested) {
        return;
    }
    const auto dirty = tick();
    impl_->queued = impl_->queued || dirty.tiles.any();
    if (impl_->active) {
        // Check before increment so even the maximum finite deadline cannot wrap.
        if (impl_->age >= impl_->capture_limits.deadline_frames) {
            impl_->active.reset();
            impl_->error = Error::DeadlineExceeded;
            return;
        }
        ++impl_->age;
        if (impl_->paused) {
            return;
        }
        auto committed = read_committed(impl_->active->window, impl_->capture_limits.read);
        if (committed.membership == Save::AuthorityMembership::Unavailable) {
            impl_->active.reset();
            impl_->error = Error::CommittedReadRefused;
            impl_->set_read_error(committed.error);
            return;
        }
        impl_->active->committed = std::move(committed);
        impl_->serving = std::move(impl_->active);
        impl_->error = Error::None;
        return;
    }
    if (impl_->paused || !impl_->queued || !impl_->source_ready) {
        return;
    }
    impl_->queued = false;
    if (impl_->serial == std::numeric_limits<std::uint64_t>::max()) {
        impl_->error = Error::AuthorityUnavailable;
        return;
    }
    try {
        auto candidate = std::allocate_shared<SourceCapture>(
            FarVolumePoolAllocator<SourceCapture>(impl_->state->limits.memory));
        candidate->window = impl_->capture_window;
        const auto error = copy_published(candidate->window, candidate->published);
        if (error != Error::None) {
            impl_->error = error;
            return;
        }
        candidate->serial = ++impl_->serial;
        impl_->active = std::move(candidate);
        impl_->age = 1;
        impl_->error = Error::None;
        impl_->read_error_size = 0;
    } catch (const std::bad_alloc&) {
        impl_->error = Error::AllocationFailure;
    }
}

void FarWorldAuthorityProducer::fence_source_reads(bool paused) {
    // Reads are synchronous and retain no handles. Returning closes admission;
    // there are no outstanding completions or workers to discard or join.
    impl_->paused = paused;
}
void FarWorldAuthorityProducer::invalidate_source() {
    impl_->active.reset();
    impl_->source_ready = false;
    impl_->queued = true;
    impl_->error = Error::AuthorityUnavailable;
}
bool FarWorldAuthorityProducer::refresh_saved_source(const std::filesystem::path& root,
                                                     std::string& error) {
    if (impl_->paused) {
        error = "Source reads are paused.";
        return false;
    }
    invalidate_source();
    const bool admitted = admit_saved_source(root, impl_->capture_limits.read, error);
    if (!admitted) {
        impl_->set_read_error(error);
    }
    return admitted;
}
const FarWorldAuthorityProducer::SourceCapture* FarWorldAuthorityProducer::capture() const {
    return impl_->serving.get();
}
FarWorldAuthorityProducer::CaptureStatus FarWorldAuthorityProducer::capture_status() const {
    const auto serving = impl_->serving ? impl_->serving->serial : 0;
    return {.error = impl_->error,
            .serving_serial = serving,
            .target_serial =
                impl_->queued && impl_->serial < std::numeric_limits<std::uint64_t>::max()
                    ? impl_->serial + 1
                    : impl_->serial,
            .active = impl_->active != nullptr,
            .queued = impl_->queued,
            .paused = impl_->paused,
            .latest_ready = serving != 0 && !impl_->active && !impl_->queued &&
                            impl_->source_ready && impl_->error == Error::None};
}
std::string_view FarWorldAuthorityProducer::capture_read_error() const {
    return {impl_->read_error.data(), impl_->read_error_size};
}

FarWorldAuthorityProducer::FrozenResult FarWorldAuthorityProducer::compose_frozen(
    const Systems::SHIELD_WorldSystem& world,
    const FarVolumeRequest& request,
    const FrozenPolicy& policy,
    const FarFrozenCaptureLimits& limits,
    const std::shared_ptr<FarVolumeMemoryPool>& pool,
    std::shared_ptr<const FarFrozenObservation>& output) const {
    const auto* source = capture();
    if (impl_->error != Error::None) {
        return {impl_->error, FarFrozenCaptureError::None};
    }
    if (!source || !impl_->source_ready || impl_->paused ||
        source->committed.membership == Save::AuthorityMembership::Unavailable) {
        return {Error::AuthorityUnavailable, FarFrozenCaptureError::None};
    }
    if (policy.water_resolution != 4 && policy.water_resolution != 8 &&
        policy.water_resolution != 16 && policy.water_resolution != 32) {
        return {Error::WaterUnavailable, FarFrozenCaptureError::None};
    }
    const auto tier = FarTierAt(request.key.tier);
    if (!tier || request.key.x <= -kFarVolumeExactCoordinateLimitMeters / tier->tile_edge_meters ||
        request.key.x >= kFarVolumeExactCoordinateLimitMeters / tier->tile_edge_meters ||
        request.key.z <= -kFarVolumeExactCoordinateLimitMeters / tier->tile_edge_meters ||
        request.key.z >= kFarVolumeExactCoordinateLimitMeters / tier->tile_edge_meters) {
        return {Error::None, FarFrozenCaptureError::InvalidRegion};
    }
    const auto edge = std::int64_t(tier->tile_edge_meters),
               step = std::int64_t(tier->sample_spacing_meters);
    const auto& window = source->window;
    // Lower shared lattice contributors must also have been copied at the source boundary.
    if (Floor(request.key.x * edge - step - 1, CHUNK_SIZE_X) < window.min_chunk_x ||
        Floor((request.key.x + 1) * edge + step, CHUNK_SIZE_X) > window.max_chunk_x ||
        Floor(request.key.z * edge - step - 1, CHUNK_SIZE_Z) < window.min_chunk_z ||
        Floor((request.key.z + 1) * edge + step, CHUNK_SIZE_Z) > window.max_chunk_z) {
        return {Error::InvalidWindow, FarFrozenCaptureError::None};
    }
    if (source->published.size() > 2u * kSlots || source->committed.chunks.size() > kSlots) {
        return {Error::None, FarFrozenCaptureError::WorkLimit};
    }
    // Fixed scratch: two published owners per index slot plus 512 committed records.
    FarVolumeMemoryLease scratch(*impl_->state->limits.memory);
    if (!scratch.TryCharge(sizeof(std::array<FrozenRecord, 3u * kSlots>) +
                           sizeof(std::array<const FrozenRecord*, 3u * kSlots>))) {
        return {Error::AllocationFailure, FarFrozenCaptureError::None};
    }
    std::array<FrozenRecord, 3u * kSlots> storage{};
    std::array<const FrozenRecord*, 3u * kSlots> water_storage{};
    std::size_t count = 0, water_count = 0;
    for (const auto& image : source->published) {
        storage[count++] = {image.coords,
                            image.parked ? 1u : 0u,
                            image.terrain_authority,
                            image.water_authority ? image.water_resolution : 0,
                            image.sdf,
                            image.material,
                            image.bed,
                            image.depth};
    }
    for (const auto& chunk : source->committed.chunks) {
        if (!chunk) {
            return {Error::AuthorityUnavailable, FarFrozenCaptureError::None};
        }
        storage[count++] = {chunk->get_coords(),
                            2,
                            chunk->has_terrain_authority(),
                            chunk->has_water_authority() ? chunk->current_water_resolution.load()
                                                         : 0,
                            chunk->sdf_data,
                            chunk->material_data,
                            chunk->water_bed_mm,
                            chunk->water_depth_mm};
    }
    const std::span<FrozenRecord> records(storage.data(), count);
    std::sort(records.begin(), records.end(), [](const auto& a, const auto& b) {
        return RecordKey(a) < RecordKey(b);
    });
    for (std::size_t i = 0; i < count; ++i) {
        const auto& record = records[i];
        if (!ValidRecord(record) || (i != 0 && RecordKey(records[i - 1]) == RecordKey(record))) {
            return {Error::AuthorityUnavailable, FarFrozenCaptureError::None};
        }
        if (record.resolution != 0) {
            if (!ValidWater(record)) {
                return {Error::WaterUnavailable, FarFrozenCaptureError::None};
            }
            water_storage[water_count++] = &record;
        }
    }
    const std::span<const FrozenRecord* const> water_records(water_storage.data(), water_count);
    for (const auto& record : records) {
        if (record.terrain && !SharedNodesAgree(records, record)) {
            return {Error::AuthorityUnavailable, FarFrozenCaptureError::None};
        }
    }
    for (std::size_t i = 1; i < water_count; ++i) {
        const auto& a = *water_records[i - 1];
        const auto& b = *water_records[i];
        if (a.layer == b.layer && a.coords.x == b.coords.x && a.coords.z == b.coords.z) {
            return {Error::WaterUnavailable, FarFrozenCaptureError::None};
        }
    }
    const auto committed = [&records](std::uint8_t, const FarVolumePosition& p) {
        return TerrainAt(records, 2, p);
    };
    const auto parked = [&records](std::uint8_t, const FarVolumePosition& p) {
        return TerrainAt(records, 1, p);
    };
    const auto live = [&records](std::uint8_t, const FarVolumePosition& p) {
        return TerrainAt(records, 0, p);
    };
    bool water_failed = false;
    const auto water = [&](std::int64_t x, std::int64_t z) -> std::optional<FarFrozenWaterSample> {
        const auto value = WaterAt(water_records, 2, x, z);
        const auto result = value.presence == FarFrozenPresence::Present
                                ? std::optional{value.sample}
                                : BaseWater(world, policy.water_resolution, x, z);
        water_failed = water_failed || !result;
        return result;
    };
    const auto parked_water = [&water_records](std::int64_t x, std::int64_t z) {
        return WaterAt(water_records, 1, x, z);
    };
    const auto live_water = [&water_records](std::int64_t x, std::int64_t z) {
        return WaterAt(water_records, 0, x, z);
    };
    FarFrozenCaptureSource frozen;
    frozen.pristine.field_identity = policy.pristine_identity;
    frozen.pristine.caves = policy.caves;
    frozen.pristine.numeric = policy.numeric;
    frozen.durable = {source->committed.generation, FarEditedImageState::Committed};
    frozen.content_identity = policy.pristine_identity;
    frozen.use = FarFrozenUse::SessionRender;
    frozen.session_water_resolution = policy.water_resolution;
    frozen.committed = std::cref(committed);
    frozen.parked = std::cref(parked);
    frozen.live = std::cref(live);
    frozen.water = std::cref(water);
    frozen.parked_water = std::cref(parked_water);
    frozen.live_water = std::cref(live_water);
    auto bounded = limits;
    // In addition to the frozen seam, base water may call both pristine height
    // and rest-level projection once per column. Admit their worst case too.
    bounded.max_height_samples /= 2;
    bounded.max_water_samples /= 3;
    const auto error =
        CaptureWorldFarVolumeObservation(world, request, frozen, bounded, pool, output);
    return {water_failed ? Error::WaterUnavailable : Error::None, error};
}
} // namespace Luminumbra::World
