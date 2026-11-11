#include "FarWorldAuthorityProducer.h"
#include "Chunk.h"
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
        : limits(source_limits) {}
    Producer::Limits limits;
    mutable std::mutex mutex;
    // At most limits.index_records nodes, capped at the fixed 512-slot envelope.
    // Column lookup probes bounded coordinates and visits only participating Y layers.
    std::map<std::tuple<int, int, int>, Members> index;
    std::array<Save::AuthorityWindow, Producer::kSlots> wanted{};
    std::array<Change, Producer::kSlots> pending{};
    std::size_t wanted_count = 0, pending_count = 0;
    bool overflow = false, all_dirty = false, index_failed = false;
    Producer::Counters counters;
};
namespace {
bool ValidLimits(const Producer::Limits& limits) {
    return limits.index_records > 0 && limits.index_records <= Producer::kSlots &&
           limits.copy_bytes > 0 && limits.query_columns > 0;
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
            try {
                member = state_->index.emplace(key, FarWorldAuthorityState::Members{}).first;
            } catch (const std::bad_alloc&) {
                state_->index_failed = true;
                state_->all_dirty = true;
                return;
            }
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
            state_->index.erase(member);
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
    state_->index.clear();
    state_->index_failed = false;
    state_->pending_count = 0;
    state_->overflow = false;
    state_->all_dirty = true;
}
struct FarWorldAuthorityProducer::Impl {
    explicit Impl(const Limits& limits)
        : state(std::make_shared<FarWorldAuthorityState>(limits))
        , changes(std::make_shared<FarWorldAuthorityChanges>(state)) {}
    std::shared_ptr<FarWorldAuthorityState> state;
    std::shared_ptr<FarWorldAuthorityChanges> changes;
    Save::AuthorityWindowReader reader;
    CaptureLimits capture_limits{};
    Save::AuthorityWindow capture_window{};
    // At most one active and one serving capture. Volatile arrays in each are
    // bounded by copy_bytes; committed arrays and parser scratch by read limits.
    // The successor retains only the fixed wanted descriptors, never source data.
    std::unique_ptr<SourceCapture> active, serving;
    std::uint64_t serial = 0;
    std::uint32_t age = 0;
    Error error = Error::None;
    std::string read_error;
    bool configured = false, source_ready = false, queued = false, paused = false,
         requested = false;
};
FarWorldAuthorityProducer::FarWorldAuthorityProducer(const Limits& limits)
    : impl_(std::make_unique<Impl>(limits)) {}
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
FarWorldAuthorityProducer::copy_published(const Save::AuthorityWindow& window,
                                          std::vector<Image>& output) {
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
        std::vector<Image> images;
        images.reserve(count);
        for (std::size_t item = 0; item < count; ++item) {
            const auto& source = selected[item];
            const auto& chunk = *source.chunk;
            Image image{.coords = chunk.get_coords(),
                        .parked = source.parked,
                        .terrain_authority = source.terrain,
                        .water_authority = source.water,
                        .water_resolution = 0,
                        .sdf = {},
                        .material = {},
                        .bed = {},
                        .depth = {}};
            if (image.terrain_authority) {
                image.sdf = chunk.sdf_data;
                image.material = chunk.material_data;
            }
            if (image.water_authority) {
                image.water_resolution = chunk.current_water_resolution.load();
                image.bed = chunk.water_bed_mm;
                image.depth = chunk.water_depth_mm;
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
        refused.error = impl_->paused ? "Source reads are paused." : "Source is unavailable.";
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
            impl_->read_error = std::move(committed.error);
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
        auto candidate = std::make_unique<SourceCapture>();
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
        impl_->read_error.clear();
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
        impl_->read_error = error;
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
const std::string& FarWorldAuthorityProducer::capture_read_error() const {
    return impl_->read_error;
}
} // namespace Luminumbra::World
