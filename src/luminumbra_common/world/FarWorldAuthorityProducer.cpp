#include "FarWorldAuthorityProducer.h"
#include "Chunk.h"
#include "FarVolumeRuntime.h"
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
                                              std::int64_t z,
                                              FarFrozenCaptureWork& work,
                                              bool& work_failed) {
    const auto center = [resolution](std::int64_t value, int edge) {
        const auto chunk = Floor(value, edge);
        const auto cell = (value - chunk * edge) * resolution / edge;
        return static_cast<float>(chunk * edge) +
               (static_cast<float>(cell) + 0.5f) *
                   (static_cast<float>(edge) / static_cast<float>(resolution));
    };
    const auto wx = center(x, CHUNK_SIZE_X), wz = center(z, CHUNK_SIZE_Z);
    if (!work.ChargeHeight()) {
        work_failed = true;
        return {};
    }
    const float terrain = world.GetTerrainHeightAt(wx, wz);
    if (!work.ChargeWater()) {
        work_failed = true;
        return {};
    }
    const float rest = world.WaterLevelAt(wx, wz);
    if (!work.CheckDeadline()) {
        work_failed = true;
        return {};
    }
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
    std::shared_ptr<FrozenComposition> frozen;
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
    bool retired = false;
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
FarWorldAuthorityProducer::~FarWorldAuthorityProducer() {
    retire();
}
std::shared_ptr<FarWorldAuthorityChanges> FarWorldAuthorityProducer::changes() const {
    return impl_->changes;
}
bool FarWorldAuthorityProducer::set_wanted(std::span<const Save::AuthorityWindow> wanted) {
    auto& state = *impl_->state;
    if (impl_->retired || !ValidLimits(state.limits) || wanted.size() > kSlots ||
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
    if (impl_->retired)
        return {};
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
    if (impl_->retired)
        return Error::AuthorityUnavailable;
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
    if (impl_->retired || impl_->paused) {
        error = impl_->retired ? "Source binding is retired." : "Source reads are paused.";
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
    if (impl_->retired || !ValidLimits(impl_->state->limits) || limits.deadline_frames == 0 ||
        !limits.clock.now || limits.clock.max_ticks == 0 || read.members == 0 ||
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
        if (impl_->active->deadline.expired()) {
            impl_->active.reset();
            impl_->error = Error::SourceReadExpired;
            return;
        }
        if (impl_->paused) {
            return;
        }
        auto committed = read_committed(impl_->active->window, impl_->capture_limits.read);
        if (impl_->active->deadline.expired()) {
            impl_->active.reset();
            impl_->error = Error::SourceReadExpired;
            return;
        }
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
    const auto& clock = impl_->capture_limits.clock;
    const auto start = clock.now(clock.context);
    if (clock.max_ticks > std::numeric_limits<std::uint64_t>::max() - start) {
        impl_->error = Error::InvalidLimits;
        return;
    }
    const FarFrozenCaptureDeadline deadline{clock.now, clock.context, start + clock.max_ticks};
    if (deadline.expired()) {
        impl_->error = Error::SourceReadExpired;
        return;
    }
    if (impl_->serial == std::numeric_limits<std::uint64_t>::max()) {
        impl_->error = Error::AuthorityUnavailable;
        return;
    }
    try {
        auto candidate = std::allocate_shared<SourceCapture>(
            FarVolumePoolAllocator<SourceCapture>(impl_->state->limits.memory));
        candidate->window = impl_->capture_window;
        candidate->deadline = deadline;
        const auto error = copy_published(candidate->window, candidate->published);
        if (candidate->deadline.expired()) {
            impl_->error = Error::SourceReadExpired;
            return;
        }
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
    if (!impl_->retired)
        impl_->paused = paused;
}
void FarWorldAuthorityProducer::invalidate_source() {
    impl_->active.reset();
    impl_->source_ready = false;
    impl_->queued = !impl_->retired;
    impl_->error = Error::AuthorityUnavailable;
}
void FarWorldAuthorityProducer::retire() {
    impl_->retired = true;
    // Retire first: no subsequent source operation can reacquire eligibility.
    cancel_frozen();
    invalidate_source();
    impl_->serving.reset();
    impl_->requested = false;
    impl_->paused = true;
    impl_->capture_limits = {};
}
bool FarWorldAuthorityProducer::retired() const {
    return impl_->retired;
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

struct FarWorldAuthorityProducer::FrozenContext {
    FrozenContext(const std::shared_ptr<Impl>& implementation,
                  const Systems::SHIELD_WorldSystem& sampling_world,
                  const std::shared_ptr<const Systems::SHIELD_WorldSystem>& owner,
                  const FarVolumeRequest& capture_request,
                  const FrozenPolicy& capture_policy,
                  const FarFrozenCaptureLimits& capture_limits,
                  const std::shared_ptr<FarVolumeMemoryPool>& capture_pool)
        : producer(implementation)
        , source(implementation->serving)
        , world(&sampling_world)
        , world_owner(owner)
        , request(capture_request)
        , policy(capture_policy)
        , limits(capture_limits)
        , pool(capture_pool)
        , work({capture_limits.max_height_samples,
                capture_limits.max_density_samples,
                capture_limits.max_water_samples},
               capture_limits.work,
               capture_limits.deadline,
               source->deadline,
               FrozenContext::Current,
               this) {
        frozen.pristine.field_identity = policy.pristine_identity;
        frozen.pristine.caves = policy.caves;
        frozen.pristine.numeric = policy.numeric;
        frozen.durable = {source->committed.generation, FarEditedImageState::Committed};
        frozen.content_identity = policy.pristine_identity;
        frozen.use = FarFrozenUse::SessionRender;
        frozen.session_water_resolution = policy.water_resolution;
        world_source.Start(*world, frozen);
    }
    std::weak_ptr<Impl> producer;
    std::shared_ptr<const SourceCapture> source;
    const Systems::SHIELD_WorldSystem* world;
    std::shared_ptr<const Systems::SHIELD_WorldSystem> world_owner;
    FarVolumeRequest request;
    FrozenPolicy policy;
    FarFrozenCaptureLimits limits;
    std::shared_ptr<FarVolumeMemoryPool> pool;
    FarFrozenCaptureWork work;
    FarFrozenWorldSource world_source;
    FarFrozenCaptureSource frozen;
    FarFrozenRegion region;
    // Fixed scratch bound: two published owners per slot and 512 committed records.
    std::array<FrozenRecord, 3u * kSlots> storage{};
    std::array<const FrozenRecord*, 3u * kSlots> water_storage{};
    std::size_t count = 0, water_count = 0;
    bool water_failed = false, work_failed = false;
    std::span<FrozenRecord> records() {
        return {storage.data(), count};
    }
    std::span<const FrozenRecord* const> water_records() const {
        return {water_storage.data(), water_count};
    }
    static bool Current(const void* context) noexcept {
        const auto& value = *static_cast<const FrozenContext*>(context);
        const auto impl = value.producer.lock();
        return impl && impl->source_ready && !impl->paused && impl->error == Error::None &&
               impl->serving == value.source && value.world_source.current();
    }
    void Bind() {
        world_source.Bind(frozen);
        frozen.committed = [this](std::uint8_t, const FarVolumePosition& p) {
            return TerrainAt(records(), 2, p);
        };
        frozen.parked = [this](std::uint8_t, const FarVolumePosition& p) {
            return TerrainAt(records(), 1, p);
        };
        frozen.live = [this](std::uint8_t, const FarVolumePosition& p) {
            return TerrainAt(records(), 0, p);
        };
        frozen.water = [this](std::int64_t x,
                              std::int64_t z) -> std::optional<FarFrozenWaterSample> {
            const auto value = WaterAt(water_records(), 2, x, z);
            const auto result =
                value.presence == FarFrozenPresence::Present
                    ? std::optional{value.sample}
                    : BaseWater(*world, policy.water_resolution, x, z, work, work_failed);
            water_failed = water_failed || (!result && !work_failed);
            return result;
        };
        frozen.parked_water = [this](std::int64_t x, std::int64_t z) {
            return WaterAt(water_records(), 1, x, z);
        };
        frozen.live_water = [this](std::int64_t x, std::int64_t z) {
            return WaterAt(water_records(), 0, x, z);
        };
    }
};
struct FarWorldAuthorityProducer::FrozenComposition {
    std::shared_ptr<FrozenContext> context;
    FarFrozenObservation::Capture observation;
    enum class Phase {
        Records,
        Sort,
        Header,
        Terrain,
        Water,
        Shared,
        Duplicates,
        Identity,
        Capture,
        CoverageRecords,
        CoverageCells,
        CoverageWater,
        CoverageHeight,
        CoveragePublish
    };
    Phase phase = Phase::Records;
    std::size_t cursor = 0, scalar = 0, neighbour = 0;
    FarVolumePosition lo{}, hi{}, node{};
    const FrozenRecord* other = nullptr;
    bool discovery = false;
    FarVolumeCoverageIntent coverage_intent;
    FarVolumeCoverageCursor coverage;
    FarVolumeRequest coverage_request;
    void Extend(float low, float high) {
        auto& extra = coverage_intent.extra_span;
        extra =
            extra ? FarVolumeCoverageSpan{std::min(extra->min_y, low), std::max(extra->max_y, high)}
                  : FarVolumeCoverageSpan{low, high};
    }
    FrozenResult Start(const std::shared_ptr<Impl>& impl,
                       const Systems::SHIELD_WorldSystem& world,
                       const std::shared_ptr<const Systems::SHIELD_WorldSystem>& owner,
                       const FarVolumeRequest& request,
                       const FrozenPolicy& policy,
                       const FarFrozenCaptureLimits& limits,
                       const std::shared_ptr<FarVolumeMemoryPool>& pool) {
        const auto* source = impl->serving.get();
        if (impl->error != Error::None) {
            return {impl->error, FarFrozenCaptureError::None};
        }
        if (!source || !impl->source_ready || impl->paused ||
            source->committed.membership == Save::AuthorityMembership::Unavailable) {
            return {Error::AuthorityUnavailable, FarFrozenCaptureError::None};
        }
        if (source->deadline.expired() || limits.deadline.expired()) {
            return {Error::None, FarFrozenCaptureError::DeadlineExceeded};
        }
        if (policy.water_resolution != 4 && policy.water_resolution != 8 &&
            policy.water_resolution != 16 && policy.water_resolution != 32) {
            return {Error::WaterUnavailable, FarFrozenCaptureError::None};
        }
        const auto tier = FarTierAt(request.key.tier);
        if (!tier ||
            request.key.x <= -kFarVolumeExactCoordinateLimitMeters / tier->tile_edge_meters ||
            request.key.x >= kFarVolumeExactCoordinateLimitMeters / tier->tile_edge_meters ||
            request.key.z <= -kFarVolumeExactCoordinateLimitMeters / tier->tile_edge_meters ||
            request.key.z >= kFarVolumeExactCoordinateLimitMeters / tier->tile_edge_meters) {
            return {Error::None, FarFrozenCaptureError::InvalidRegion};
        }
        const auto& window = source->window;
        // Lower shared lattice contributors must also have been copied at the source boundary.
        Save::AuthorityWindow required;
        if (!Producer::source_window(request.key, required) ||
            required.min_chunk_x < window.min_chunk_x ||
            required.max_chunk_x > window.max_chunk_x ||
            required.min_chunk_z < window.min_chunk_z ||
            required.max_chunk_z > window.max_chunk_z) {
            return {Error::InvalidWindow, FarFrozenCaptureError::None};
        }
        if (source->published.size() > 2u * kSlots || source->committed.chunks.size() > kSlots) {
            return {Error::None, FarFrozenCaptureError::WorkLimit};
        }

        try {
            const auto scope = world.acquire_worldgen_sample_scope();
            context = std::allocate_shared<FrozenContext>(
                FarVolumePoolAllocator<FrozenContext>(impl->state->limits.memory),
                impl,
                world,
                owner,
                request,
                policy,
                limits,
                pool);
        } catch (const std::bad_alloc&) {
            return {Error::AllocationFailure, FarFrozenCaptureError::None};
        } catch (...) {
            return {Error::None, FarFrozenCaptureError::Unavailable};
        }
        return {};
    }
    FrozenResult SharedStep() {
        auto& c = *context;
        if (cursor == c.count) {
            phase = Phase::Duplicates;
            cursor = 1;
            return {};
        }
        const auto& record = c.storage[cursor];
        if (!record.terrain || (!other && neighbour == 27)) {
            ++cursor;
            neighbour = 0;
            other = nullptr;
            return {};
        }
        if (!other) {
            const auto dx = static_cast<int>(neighbour % 3) - 1;
            const auto dy = static_cast<int>((neighbour / 3) % 3) - 1;
            const auto dz = static_cast<int>(neighbour / 9) - 1;
            const auto key = std::tuple(record.layer,
                                        std::int64_t(record.coords.x) + dx,
                                        std::int64_t(record.coords.z) + dz,
                                        std::int64_t(record.coords.y) + dy);
            ++neighbour;
            if (key <= RecordKey(record)) {
                return {};
            }
            const auto records = c.records();
            const auto found = std::lower_bound(
                records.begin(), records.end(), key, [](const auto& candidate, const auto& value) {
                    return RecordKey(candidate) < value;
                });
            if (found == records.end() || RecordKey(*found) != key || !found->terrain) {
                return {};
            }
            other = &*found;
            lo = {std::int64_t(std::max(record.coords.x, other->coords.x)) * CHUNK_SIZE_X,
                  std::int64_t(std::max(record.coords.y, other->coords.y)) * CHUNK_SIZE_Y,
                  std::int64_t(std::max(record.coords.z, other->coords.z)) * CHUNK_SIZE_Z};
            hi = {(std::int64_t(std::min(record.coords.x, other->coords.x)) + 1) * CHUNK_SIZE_X,
                  (std::int64_t(std::min(record.coords.y, other->coords.y)) + 1) * CHUNK_SIZE_Y,
                  (std::int64_t(std::min(record.coords.z, other->coords.z)) + 1) * CHUNK_SIZE_Z};
            node = lo;
            return {};
        }
        if (Node(record, node) != Node(*other, node)) {
            return {Error::AuthorityUnavailable, FarFrozenCaptureError::None};
        }
        if (++node.x > hi.x) {
            node.x = lo.x;
            if (++node.y > hi.y) {
                node.y = lo.y;
                if (++node.z > hi.z) {
                    other = nullptr;
                }
            }
        }
        return {};
    }
    FrozenResult Step() {
        auto& c = *context;
        const auto before = [](const auto& a, const auto& b) {
            return RecordKey(a) < RecordKey(b);
        };
        switch (phase) {
            case Phase::Records: {
                const auto published = c.source->published.size();
                if (cursor == published + c.source->committed.chunks.size()) {
                    phase = Phase::Sort;
                    cursor = c.count;
                    break;
                }
                if (cursor < published) {
                    const auto& image = c.source->published[cursor];
                    c.storage[c.count++] = {image.coords,
                                            image.parked ? 1u : 0u,
                                            image.terrain_authority,
                                            image.water_authority ? image.water_resolution : 0,
                                            image.sdf,
                                            image.material,
                                            image.bed,
                                            image.depth};
                } else {
                    const auto& chunk = c.source->committed.chunks[cursor - published];
                    if (!chunk) {
                        return {Error::AuthorityUnavailable, FarFrozenCaptureError::None};
                    }
                    c.storage[c.count++] = {
                        chunk->get_coords(),
                        2,
                        chunk->has_terrain_authority(),
                        chunk->has_water_authority() ? chunk->current_water_resolution.load() : 0,
                        chunk->sdf_data,
                        chunk->material_data,
                        chunk->water_bed_mm,
                        chunk->water_depth_mm};
                }
                ++cursor;
                std::push_heap(c.storage.begin(),
                               c.storage.begin() + static_cast<std::ptrdiff_t>(c.count),
                               before);
                break;
            }
            case Phase::Sort:
                if (cursor == 0) {
                    phase = Phase::Header;
                    break;
                }
                std::pop_heap(c.storage.begin(),
                              c.storage.begin() + static_cast<std::ptrdiff_t>(cursor),
                              before);
                --cursor;
                break;
            case Phase::Header: {
                if (cursor == c.count) {
                    phase = Phase::Shared;
                    cursor = 0;
                    break;
                }
                const auto& record = c.storage[cursor];
                constexpr std::size_t samples =
                    std::size_t{CHUNK_SIZE_X + 1} * (CHUNK_SIZE_Y + 1) * (CHUNK_SIZE_Z + 1);
                if ((cursor != 0 && RecordKey(c.storage[cursor - 1]) == RecordKey(record)) ||
                    (record.terrain &&
                     (record.sdf.size() != samples ||
                      (!record.material.empty() && record.material.size() != samples)))) {
                    return {Error::AuthorityUnavailable, FarFrozenCaptureError::None};
                }
                scalar = 0;
                phase = Phase::Terrain;
                break;
            }
            case Phase::Terrain: {
                const auto& record = c.storage[cursor];
                if (record.terrain && scalar < record.sdf.size()) {
                    if (!std::isfinite(record.sdf[scalar++])) {
                        return {Error::AuthorityUnavailable, FarFrozenCaptureError::None};
                    }
                    break;
                }
                if (record.resolution != 0) {
                    if (record.resolution != 4 && record.resolution != 8 &&
                        record.resolution != 16 && record.resolution != 32) {
                        return {Error::WaterUnavailable, FarFrozenCaptureError::None};
                    }
                    const auto cells =
                        std::size_t(record.resolution) * std::size_t(record.resolution);
                    if (record.bed.size() != cells || record.depth.size() != cells) {
                        return {Error::WaterUnavailable, FarFrozenCaptureError::None};
                    }
                    c.water_storage[c.water_count++] = &record;
                }
                phase = Phase::Water;
                scalar = 0;
                break;
            }
            case Phase::Water: {
                const auto& record = c.storage[cursor];
                if (record.resolution != 0 && scalar < record.depth.size()) {
                    if (record.depth[scalar++] < 0) {
                        return {Error::WaterUnavailable, FarFrozenCaptureError::None};
                    }
                    break;
                }
                ++cursor;
                phase = Phase::Header;
                break;
            }
            case Phase::Shared:
                return SharedStep();
            case Phase::Duplicates: {
                if (cursor < c.water_count) {
                    const auto& a = *c.water_storage[cursor - 1];
                    const auto& b = *c.water_storage[cursor++];
                    if (a.layer == b.layer && a.coords.x == b.coords.x &&
                        a.coords.z == b.coords.z) {
                        return {Error::WaterUnavailable, FarFrozenCaptureError::None};
                    }
                } else {
                    phase = Phase::Identity;
                }
                break;
            }
            case Phase::Identity: {
                const auto result = c.world_source.Pump(1);
                if (result.error != FarFrozenCaptureError::None) {
                    return {Error::None, result.error};
                }
                if (result.complete) {
                    c.Bind();
                    if (discovery) {
                        phase = Phase::CoverageRecords;
                        cursor = 0;
                        break;
                    }
                    const auto tier = FarTierAt(c.request.key.tier);
                    if (!tier) {
                        return {Error::None, FarFrozenCaptureError::InvalidRegion};
                    }
                    if (c.request.span.min_y >= c.request.span.max_y ||
                        c.request.span.min_y % tier->brick_edge_meters ||
                        c.request.span.max_y % tier->brick_edge_meters) {
                        return {Error::None, FarFrozenCaptureError::InvalidRegion};
                    }
                    if (c.request.field_identity != c.frozen.content_identity) {
                        return {Error::None, FarFrozenCaptureError::IdentityMismatch};
                    }
                    const auto edge = std::int64_t(tier->tile_edge_meters);
                    c.region = {
                        static_cast<std::uint8_t>(c.request.key.tier),
                        {{c.request.key.x * edge, c.request.span.min_y, c.request.key.z * edge},
                         {(c.request.key.x + 1) * edge,
                          c.request.span.max_y,
                          (c.request.key.z + 1) * edge}}};
                    const auto impl = c.producer.lock();
                    if (!impl) {
                        return {Error::AuthorityUnavailable, FarFrozenCaptureError::None};
                    }
                    auto input = std::allocate_shared<FarFrozenCaptureInput>(
                        FarVolumePoolAllocator<FarFrozenCaptureInput>(impl->state->limits.memory));
                    // Borrow stable callback objects from the retained context, never their
                    // targets.
                    input->source.pristine = c.frozen.pristine;
                    input->source.durable = c.frozen.durable;
                    input->source.content_identity = c.frozen.content_identity;
                    input->source.use = c.frozen.use;
                    input->source.session_water_resolution = c.frozen.session_water_resolution;
                    input->source.committed = std::cref(c.frozen.committed);
                    input->source.parked = std::cref(c.frozen.parked);
                    input->source.live = std::cref(c.frozen.live);
                    input->source.water = std::cref(c.frozen.water);
                    input->source.parked_water = std::cref(c.frozen.parked_water);
                    input->source.live_water = std::cref(c.frozen.live_water);
                    input->regions = {&c.region, 1};
                    input->owner = context;
                    input->current = FrozenContext::Current;
                    auto bounded = c.limits;
                    bounded.work = &c.work;
                    observation.Start(input, bounded, c.pool);
                    phase = Phase::Capture;
                }
                break;
            }
            case Phase::Capture:
                break;
            case Phase::CoverageRecords:
            case Phase::CoverageCells:
            case Phase::CoverageWater:
            case Phase::CoverageHeight:
            case Phase::CoveragePublish:
                return CoverageStep();
        }
        return {};
    }
    FrozenResult CoverageStep() {
        auto& c = *context;
        const auto tier = FarTierAt(c.request.key.tier);
        if (!tier) {
            return {Error::None, FarFrozenCaptureError::InvalidRegion};
        }
        const auto edge = std::int64_t(tier->tile_edge_meters);
        const auto step = std::int64_t(tier->sample_spacing_meters);
        const auto min_x = c.request.key.x * edge - step;
        const auto max_x = (c.request.key.x + 1) * edge + step;
        const auto min_z = c.request.key.z * edge - step;
        const auto max_z = (c.request.key.z + 1) * edge + step;
        if (phase == Phase::CoverageRecords) {
            if (cursor == c.count) {
                phase = Phase::CoverageWater;
                cursor = 0;
                return {};
            }
            const auto& record = c.storage[cursor];
            const auto rx = std::int64_t(record.coords.x) * CHUNK_SIZE_X;
            const auto rz = std::int64_t(record.coords.z) * CHUNK_SIZE_Z;
            if (rx > max_x || rx + CHUNK_SIZE_X < min_x || rz > max_z ||
                rz + CHUNK_SIZE_Z < min_z) {
                ++cursor;
                return {};
            }
            if (record.terrain) {
                const auto bottom = std::int64_t(record.coords.y) * CHUNK_SIZE_Y;
                Extend(static_cast<float>(bottom), static_cast<float>(bottom + CHUNK_SIZE_Y));
            }
            // Water authority owns a whole column. A higher layer replaces all
            // cells, even when its grid resolution differs or it is explicitly dry.
            bool selected = record.resolution != 0;
            for (unsigned layer = 0; layer < record.layer; ++layer) {
                selected = selected && WaterAt(c.water_records(), layer, rx, rz).presence ==
                                           FarFrozenPresence::Absent;
            }
            if (selected) {
                scalar = 0;
                phase = Phase::CoverageCells;
            } else {
                ++cursor;
            }
            return {};
        }
        if (phase == Phase::CoverageCells) {
            const auto& record = c.storage[cursor];
            if (scalar == record.depth.size()) {
                ++cursor;
                phase = Phase::CoverageRecords;
                return {};
            }
            const auto resolution = static_cast<std::size_t>(record.resolution);
            // Compare rational cell edges without truncating sub-metre cells
            // (resolution 32 also supports 16-metre chunk columns).
            const auto x = std::int64_t(record.coords.x) * CHUNK_SIZE_X * record.resolution +
                           static_cast<std::int64_t>(scalar % resolution) * CHUNK_SIZE_X;
            const auto z = std::int64_t(record.coords.z) * CHUNK_SIZE_Z * record.resolution +
                           static_cast<std::int64_t>(scalar / resolution) * CHUNK_SIZE_Z;
            if (record.depth[scalar] > 0 && x <= max_x * record.resolution &&
                x + CHUNK_SIZE_X > min_x * record.resolution && z <= max_z * record.resolution &&
                z + CHUNK_SIZE_Z > min_z * record.resolution) {
                const float height =
                    static_cast<float>(std::int64_t(record.bed[scalar]) + record.depth[scalar]) /
                    1000.0f;
                Extend(height, height);
            }
            ++scalar;
            return {};
        }
        if (phase == Phase::CoverageWater) {
            constexpr std::size_t side = 131;
            if (cursor == side * side) {
                if (coverage.Start(coverage_intent, c.frozen.content_identity) !=
                    FarVolumeFacadeError::None) {
                    return {Error::None, FarFrozenCaptureError::InvalidRegion};
                }
                phase = Phase::CoverageHeight;
                return {};
            }
            const auto x = min_x + static_cast<std::int64_t>(cursor % side) * step;
            const auto z = min_z + static_cast<std::int64_t>(cursor / side) * step;
            std::optional<FarFrozenWaterSample> sample;
            for (unsigned layer = 0; layer < 3; ++layer) {
                if (!c.work.ChargeWater()) {
                    return {Error::None, FarFrozenCaptureError::WorkLimit};
                }
                const auto value = WaterAt(c.water_records(), layer, x, z);
                if (value.presence == FarFrozenPresence::Present) {
                    sample = value.sample;
                    break;
                }
            }
            if (!sample) {
                sample =
                    BaseWater(*c.world, c.policy.water_resolution, x, z, c.work, c.work_failed);
            }
            if (c.work_failed) {
                return {Error::None, FarFrozenCaptureError::WorkLimit};
            }
            if (!sample || !std::isfinite(sample->surface_height)) {
                return {Error::WaterUnavailable, FarFrozenCaptureError::None};
            }
            if (sample->wet) {
                Extend(sample->surface_height, sample->surface_height);
            }
            ++cursor;
            return {};
        }
        if (phase == Phase::CoverageHeight) {
            if (coverage.Complete()) {
                if (coverage.Finish(coverage_request) != FarVolumeFacadeError::None) {
                    return {Error::None, FarFrozenCaptureError::InvalidRegion};
                }
                phase = Phase::CoveragePublish;
                return {};
            }
            if (!c.work.ChargeHeight()) {
                return {Error::None, FarFrozenCaptureError::WorkLimit};
            }
            const auto [x, z] = coverage.Column();
            const auto height =
                c.world->GetTerrainHeightAt(static_cast<float>(x), static_cast<float>(z));
            if (coverage.Observe(height) != FarVolumeFacadeError::None) {
                return {Error::None, FarFrozenCaptureError::NonFiniteSample};
            }
        }
        return {};
    }
    FrozenPump Pump(std::uint64_t budget,
                    std::shared_ptr<const FarFrozenObservation>& output,
                    FarVolumeRequest* coverage_output = nullptr) {
        FrozenPump result;
        if (!context) {
            result.result.source = Error::AuthorityUnavailable;
            return result;
        }
        try {
            const auto scope = context->world->acquire_worldgen_sample_scope();
            while (result.work < budget) {
                if (!FrozenContext::Current(context.get())) {
                    const auto impl = context->producer.lock();
                    result.result =
                        !impl || !impl->source_ready || impl->paused ||
                                impl->serving != context->source || impl->error != Error::None
                            ? FrozenResult{Error::AuthorityUnavailable, FarFrozenCaptureError::None}
                            : FrozenResult{Error::None, FarFrozenCaptureError::IdentityMismatch};
                    break;
                }
                if (!context->work.CheckDeadline()) {
                    result.result.capture = FarFrozenCaptureError::DeadlineExceeded;
                    break;
                }
                ++result.work;
                if (phase == Phase::Capture) {
                    const auto captured = observation.Pump(1, output);
                    result.complete = captured.complete;
                    result.result.capture = captured.error;
                    if (captured.error == FarFrozenCaptureError::DeadlineExceeded ||
                        context->work.deadline_exceeded()) {
                        result.result.capture = FarFrozenCaptureError::DeadlineExceeded;
                    } else if (context->work_failed) {
                        result.result.capture = FarFrozenCaptureError::WorkLimit;
                    } else if (context->water_failed) {
                        result.result.source = Error::WaterUnavailable;
                    }
                } else {
                    result.result = Step();
                }
                if (discovery) {
                    if (!FrozenContext::Current(context.get())) {
                        result.result = {Error::AuthorityUnavailable, FarFrozenCaptureError::None};
                    } else if (context->work.deadline_exceeded() ||
                               !context->work.CheckDeadline()) {
                        result.result = {Error::None, FarFrozenCaptureError::DeadlineExceeded};
                    } else if (result.result.ok() && phase == Phase::CoveragePublish &&
                               coverage_output) {
                        *coverage_output = coverage_request;
                        result.complete = true;
                    }
                }
                if (!result.result.ok() || result.complete) {
                    break;
                }
            }
        } catch (const std::bad_alloc&) {
            result.result.source = Error::AllocationFailure;
        } catch (...) {
            result.result.capture = FarFrozenCaptureError::Unavailable;
        }
        if (!result.result.ok() || result.complete) {
            observation.Cancel();
            context.reset();
        }
        return result;
    }
};

bool FarWorldAuthorityProducer::source_window(const FarVolumeTileKey& key,
                                              Save::AuthorityWindow& output) {
    FarTileKey checked;
    const auto tier = FarTierAt(key.tier);
    if (!tier || !ToFarTileKey(key, checked)) {
        return false;
    }
    const auto edge = std::int64_t(tier->tile_edge_meters);
    const auto step = std::int64_t(tier->sample_spacing_meters);
    const auto low_x = Floor(key.x * edge - step - 1, CHUNK_SIZE_X);
    const auto high_x = Floor((key.x + 1) * edge + step, CHUNK_SIZE_X);
    const auto low_z = Floor(key.z * edge - step - 1, CHUNK_SIZE_Z);
    const auto high_z = Floor((key.z + 1) * edge + step, CHUNK_SIZE_Z);
    if (low_x < Chunk::kPackedMinXz || high_x > Chunk::kPackedMaxXz ||
        low_z < Chunk::kPackedMinXz || high_z > Chunk::kPackedMaxXz) {
        return false;
    }
    output = {static_cast<int>(low_x),
              static_cast<int>(high_x),
              static_cast<int>(low_z),
              static_cast<int>(high_z)};
    return true;
}

FarWorldAuthorityProducer::FrozenResult FarWorldAuthorityProducer::start_coverage(
    const std::shared_ptr<const Systems::SHIELD_WorldSystem>& world,
    const FarVolumeCoverageIntent& intent,
    const FrozenPolicy& policy,
    const FarFrozenCaptureLimits& limits) {
    FarVolumeCoverageCursor validation;
    if (impl_->retired || !world || impl_->frozen) {
        return {Error::AuthorityUnavailable, FarFrozenCaptureError::None};
    }
    if (validation.Start(intent, policy.pristine_identity) != FarVolumeFacadeError::None) {
        return {Error::None, FarFrozenCaptureError::InvalidRegion};
    }
    try {
        auto composition = std::allocate_shared<FrozenComposition>(
            FarVolumePoolAllocator<FrozenComposition>(memory_));
        composition->discovery = true;
        composition->coverage_intent = intent;
        const FarVolumeRequest request{intent.key, {}, policy.pristine_identity};
        const auto result =
            composition->Start(impl_, *world, world, request, policy, limits, memory_);
        if (result.ok()) {
            impl_->frozen = std::move(composition);
        }
        return result;
    } catch (const std::bad_alloc&) {
        return {Error::AllocationFailure, FarFrozenCaptureError::None};
    }
}

FarWorldAuthorityProducer::FrozenPump
FarWorldAuthorityProducer::pump_coverage(std::uint64_t budget, FarVolumeRequest& output) {
    if (!impl_->frozen || !impl_->frozen->discovery) {
        return {{Error::AuthorityUnavailable, FarFrozenCaptureError::None}, 0, false};
    }
    std::shared_ptr<const FarFrozenObservation> unused;
    const auto result = impl_->frozen->Pump(budget, unused, &output);
    if (!result.result.ok() || result.complete) {
        cancel_frozen();
    }
    return result;
}

FarWorldAuthorityProducer::FrozenResult FarWorldAuthorityProducer::compose_frozen(
    const Systems::SHIELD_WorldSystem& world,
    const FarVolumeRequest& request,
    const FrozenPolicy& policy,
    const FarFrozenCaptureLimits& limits,
    const std::shared_ptr<FarVolumeMemoryPool>& pool,
    std::shared_ptr<const FarFrozenObservation>& output) const {
    FrozenComposition composition;
    const auto start = composition.Start(impl_, world, {}, request, policy, limits, pool);
    if (!start.ok()) {
        return start;
    }
    for (;;) {
        const auto result = composition.Pump(std::numeric_limits<std::uint64_t>::max(), output);
        if (!result.result.ok() || result.complete) {
            return result.result;
        }
    }
}
FarWorldAuthorityProducer::FrozenResult FarWorldAuthorityProducer::start_frozen(
    const std::shared_ptr<const Systems::SHIELD_WorldSystem>& world,
    const FarVolumeRequest& request,
    const FrozenPolicy& policy,
    const FarFrozenCaptureLimits& limits,
    const std::shared_ptr<FarVolumeMemoryPool>& pool) {
    cancel_frozen();
    if (impl_->retired || !world) {
        return {Error::AuthorityUnavailable, FarFrozenCaptureError::None};
    }
    try {
        auto composition = std::allocate_shared<FrozenComposition>(
            FarVolumePoolAllocator<FrozenComposition>(memory_));
        const auto result = composition->Start(impl_, *world, world, request, policy, limits, pool);
        if (result.ok()) {
            impl_->frozen = std::move(composition);
        }
        return result;
    } catch (const std::bad_alloc&) {
        return {Error::AllocationFailure, FarFrozenCaptureError::None};
    }
}
FarWorldAuthorityProducer::FrozenPump
FarWorldAuthorityProducer::pump_frozen(std::uint64_t budget,
                                       std::shared_ptr<const FarFrozenObservation>& output) {
    if (!impl_->frozen || impl_->frozen->discovery) {
        return {{Error::AuthorityUnavailable, FarFrozenCaptureError::None}, 0, false};
    }
    const auto result = impl_->frozen->Pump(budget, output);
    if (!result.result.ok() || result.complete) {
        cancel_frozen();
    }
    return result;
}
void FarWorldAuthorityProducer::cancel_frozen() {
    impl_->frozen.reset();
}
bool FarWorldAuthorityProducer::frozen_active() const {
    return impl_->frozen != nullptr;
}

} // namespace Luminumbra::World
