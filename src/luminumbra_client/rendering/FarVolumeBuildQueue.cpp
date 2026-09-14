#include "FarVolumeBuildQueue.h"

#include "luminumbra_common/systems/SHIELD_WorldSystem.h"
#include "luminumbra_common/world/FarLodStore.h"
#include "luminumbra_common/world/MarchingCubes.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <utility>

namespace Luminumbra::Rendering {
namespace {

enum class SlotState {
    Free,
    Running,
    Complete,
    Leased,
    Retired
};
struct Slot {
    SlotState state = SlotState::Free;
    JobHandle handle;
    std::optional<FarVolumeBuildStatus> cancellation;
    bool reserved = false;
    FarVolumeBuildResult result;
};
struct WorldStamp {
    std::uint64_t params_hash = 0, authority_revision = 0;
    int seed = 0;
    bool operator==(const WorldStamp&) const = default;
};
WorldStamp ReadWorldStampUnderScope(const Systems::SHIELD_WorldSystem& world) {
    return {World::ComputeTerrainParamsHash(world.get_params(), world.get_seed()),
            world.far_lod_authority_revision(),
            world.get_seed()};
}
bool Ready(FarVolumeBuildStatus status) {
    return status == FarVolumeBuildStatus::Ready || status == FarVolumeBuildStatus::ReadyEmpty;
}
void Fail(FarVolumeBuildResult& result, FarVolumeBuildStatus status, const char* error) {
    result.mesh = {};
    result.span_complete = false;
    result.status = status;
    result.error.fill('\0');
    for (std::size_t i = 0; i + 1 < result.error.size() && error[i] != '\0'; ++i)
        result.error[i] = error[i];
}
bool Finished(const JobHandle& handle) {
    return !handle.counter || handle.counter->load(std::memory_order_acquire) == 0;
}
void Increment(std::uint64_t& counter) {
    if (counter != std::numeric_limits<std::uint64_t>::max())
        ++counter;
}
bool SameTile(const World::FarVolumeRequest& a, const World::FarVolumeRequest& b) {
    return a.tier == b.tier && a.tile_x == b.tile_x && a.tile_z == b.tile_z;
}
bool SameSpan(const std::optional<World::FarVolumeSpan>& a,
              const std::optional<World::FarVolumeSpan>& b) {
    if (!a.has_value())
        return !b.has_value();
    if (!b.has_value())
        return false;
    return a.value().min_y == b.value().min_y && a.value().max_y == b.value().max_y;
}
bool SameRequest(const World::FarVolumeRequest& a, const World::FarVolumeRequest& b) {
    return SameTile(a, b) && a.caves == b.caves && SameSpan(a.extra_span, b.extra_span);
}
bool ValidRequest(const World::FarVolumeRequest& request) {
    const auto tier = World::FarTierAt(request.tier);
    if (!tier.has_value() || (request.caves != World::FarCaveMode::BandLimited &&
                              request.caves != World::FarCaveMode::BoxFiltered))
        return false;
    const auto edge = tier.value().tile_edge_meters;
    constexpr auto limit = World::kFarVolumeExactCoordinateLimitMeters;
    for (const auto tile : {request.tile_x, request.tile_z}) {
        const auto origin = static_cast<std::int64_t>(tile) * edge;
        if (origin <= -limit || origin + edge >= limit)
            return false;
    }
    if (request.extra_span.has_value()) {
        const auto span = request.extra_span.value();
        if (!std::isfinite(span.min_y) || !std::isfinite(span.max_y) || span.min_y > span.max_y ||
            span.min_y <= -limit + 1024 || span.max_y >= limit - 1024)
            return false;
    }
    return true;
}
void ValidateResultTile(const World::FarVolumeTile& tile, const World::FarVolumeRequest& request) {
    if (tile.tier != request.tier || tile.tile_x != request.tile_x ||
        tile.tile_z != request.tile_z || tile.caves != request.caves)
        throw std::invalid_argument("Far-volume result identity differs from request");
    const auto dimensions = World::FarTierAt(request.tier);
    if (!dimensions.has_value())
        throw std::invalid_argument("Invalid far-volume result tier");
    if (request.extra_span.has_value()) {
        const auto edge = dimensions.value().brick_edge_meters;
        const auto first_y = static_cast<std::int64_t>(tile.first_brick_y) * edge;
        const auto last_y = static_cast<std::int64_t>(tile.last_brick_y) * edge;
        const auto span = request.extra_span.value();
        if (static_cast<double>(first_y) > span.min_y || static_cast<double>(last_y) <= span.max_y)
            throw std::invalid_argument("Far-volume result truncates requested span");
    }
}

} // namespace

struct FarVolumeQueueShared {
    std::mutex mutex;
    std::array<Slot, kFarVolumeResultCapacity> slots;
    std::uint64_t dispatches = 0, stale_results = 0;
};

FarVolumeResultLease::FarVolumeResultLease(std::shared_ptr<FarVolumeQueueShared> state,
                                           std::size_t slot)
    : m_state(std::move(state))
    , m_slot(slot) {}
FarVolumeResultLease::FarVolumeResultLease(FarVolumeResultLease&& other) noexcept
    : m_state(std::move(other.m_state))
    , m_slot(other.m_slot) {}
FarVolumeResultLease& FarVolumeResultLease::operator=(FarVolumeResultLease&& other) noexcept {
    if (this != &other) {
        release();
        m_state = std::move(other.m_state);
        m_slot = other.m_slot;
    }
    return *this;
}
FarVolumeResultLease::~FarVolumeResultLease() {
    release();
}
const FarVolumeBuildResult& FarVolumeResultLease::result() const {
    if (!m_state)
        throw std::logic_error("Released far-volume result lease");
    // Leased results are immutable even across a binding swap. The owner checks
    // is_current before consuming geometry after a subsequent world change.
    return m_state->slots[m_slot].result;
}
void FarVolumeResultLease::release() noexcept {
    if (!m_state)
        return;
    {
        std::lock_guard lock(m_state->mutex);
        auto& slot = m_state->slots[m_slot];
        slot.result = {};
        slot.reserved = false;
        // A worker publishes before JobSystem's final completion decrement.
        // Preserve its handle until that epilogue returns, even for a fast lease.
        slot.state = SlotState::Retired;
    }
    m_state.reset();
}

struct FarVolumeBuildQueue::Impl {
    struct Descriptor {
        bool used = false, pending = false, cancelled = false;
        FarVolumeBuildIdentity identity;
    };
    struct Hooks {
        PhaseHook function = nullptr;
        void* context = nullptr;
        void call(Phase phase,
                  const FarVolumeBuildIdentity& identity,
                  World::FarVolumeTile* tile = nullptr,
                  World::FarVolumeMesh* mesh = nullptr) const {
            if (function)
                function(context, phase, identity, tile, mesh);
        }
    };

    JobSystem& jobs;
    const FarVolumeBuildLimits limits;
    const std::thread::id owner = std::this_thread::get_id();
    const Systems::SHIELD_WorldSystem* world = nullptr;
    WorldStamp stamp;
    std::uint64_t epoch = 0, sequence = 0;
    std::array<Descriptor, kFarVolumeRequestCapacity> descriptors;
    std::shared_ptr<FarVolumeQueueShared> shared = std::make_shared<FarVolumeQueueShared>();
    Hooks hooks;

    Impl(JobSystem& system, FarVolumeBuildLimits budget)
        : jobs(system)
        , limits(budget) {}
    void check_owner() const {
        if (owner != std::this_thread::get_id())
            throw std::logic_error("Far-volume queue called outside its owner thread");
    }
    bool binding_current() const {
        if (!world)
            return false;
        const auto scope = world->acquire_worldgen_sample_scope();
        return ReadWorldStampUnderScope(*world) == stamp;
    }
    bool current(const FarVolumeBuildIdentity& identity) const {
        if (!world || identity.binding_epoch != epoch ||
            identity.params_hash != stamp.params_hash || identity.seed != stamp.seed ||
            identity.authority_revision != stamp.authority_revision)
            return false;
        return std::any_of(descriptors.begin(), descriptors.end(), [&](const auto& descriptor) {
            return descriptor.used && !descriptor.cancelled &&
                   descriptor.identity.request_generation == identity.request_generation &&
                   SameRequest(descriptor.identity.submitted_request, identity.submitted_request) &&
                   SameRequest(descriptor.identity.effective_request, identity.effective_request);
        });
    }
    static bool invalidate(Slot& slot, FarVolumeBuildStatus status) {
        if (slot.state == SlotState::Running)
            slot.cancellation = status;
        else if (slot.state == SlotState::Complete && Ready(slot.result.status)) {
            Fail(slot.result, status, "Far-volume request invalidated");
            slot.reserved = false;
            return status == FarVolumeBuildStatus::Stale;
        }
        return false;
    }
    void invalidate_binding() {
        for (auto& descriptor : descriptors) {
            descriptor.pending = false;
            descriptor.cancelled = true;
        }
        std::lock_guard lock(shared->mutex);
        for (auto& slot : shared->slots)
            if (invalidate(slot, FarVolumeBuildStatus::Stale))
                Increment(shared->stale_results);
    }
    void wait() {
        for (std::size_t i = 0; i < kFarVolumeResultCapacity; ++i) {
            JobHandle handle;
            {
                std::lock_guard lock(shared->mutex);
                handle = shared->slots[i].handle;
            }
            jobs.wait(handle);
        }
    }
    static bool cancelled(const std::shared_ptr<FarVolumeQueueShared>& state,
                          std::size_t index,
                          FarVolumeBuildResult& result) {
        std::lock_guard lock(state->mutex);
        const auto cancellation = state->slots[index].cancellation;
        if (!cancellation.has_value())
            return false;
        Fail(result, cancellation.value(), "Far-volume request invalidated");
        return true;
    }
    static void build(const Systems::SHIELD_WorldSystem& world,
                      FarVolumeBuildResult& result,
                      const FarVolumeBuildLimits& limits,
                      const Hooks& hooks,
                      const std::shared_ptr<FarVolumeQueueShared>& state,
                      std::size_t index) {
        if (cancelled(state, index, result))
            return;
        hooks.call(Phase::BeforeSampling, result.identity);
        World::FarVolumeTile tile;
        {
            const auto scope = world.acquire_worldgen_sample_scope();
            const auto stamp = ReadWorldStampUnderScope(world);
            if (stamp.params_hash != result.identity.params_hash ||
                stamp.seed != result.identity.seed ||
                stamp.authority_revision != result.identity.authority_revision) {
                Fail(result,
                     FarVolumeBuildStatus::Stale,
                     "World changed before far-volume sampling");
                return;
            }
            if (cancelled(state, index, result))
                return;
            tile = World::BuildPristineFarVolumeTile(
                world, result.identity.effective_request, limits.generation);
        }
        hooks.call(Phase::AfterSampling, result.identity, &tile);
        if (cancelled(state, index, result))
            return;
        ValidateResultTile(tile, result.identity.effective_request);
        if (tile.bricks.capacity() >
            limits.generation.max_buffer_bytes / sizeof(World::FarVolumeBrick))
            throw std::length_error("Far-volume retained brick capacity budget exceeded");
        auto geometry =
            World::MarchingCubes::PolygoniseFarVolume(tile, limits.generation, limits.geometry);
        result.first_brick_y = tile.first_brick_y;
        result.last_brick_y = tile.last_brick_y;
        result.sampled_bricks = tile.sampled_bricks;
        result.density_samples =
            129ull * 129ull *
            (4ull * static_cast<std::uint64_t>(static_cast<std::int64_t>(tile.last_brick_y) -
                                               tile.first_brick_y) +
             1ull);
        result.tile_crc32 = tile.crc32;
        result.tile_capacity_bytes = tile.bricks.capacity() * sizeof(World::FarVolumeBrick);
        hooks.call(Phase::AfterMeshing, result.identity, &tile, &geometry);
        if (geometry.vertices.capacity() >
                limits.geometry.max_buffer_bytes / sizeof(World::FarVolumeVertex) ||
            geometry.indices.capacity() >
                (limits.geometry.max_buffer_bytes -
                 geometry.vertices.capacity() * sizeof(World::FarVolumeVertex)) /
                    sizeof(std::uint32_t))
            throw std::length_error("Far-volume geometry capacity budget exceeded");
        result.geometry_capacity_bytes =
            geometry.vertices.capacity() * sizeof(World::FarVolumeVertex) +
            geometry.indices.capacity() * sizeof(std::uint32_t);
        if (cancelled(state, index, result))
            return;
        result.mesh = AdaptFarVolumeRenderMesh(geometry, limits.converted_bytes);
        result.span_complete = true;
        result.status = result.mesh.indices.empty() ? FarVolumeBuildStatus::ReadyEmpty
                                                    : FarVolumeBuildStatus::Ready;
        hooks.call(Phase::BeforePublish, result.identity, &tile, &geometry);
    }
    static void run(const Systems::SHIELD_WorldSystem& world,
                    FarVolumeBuildIdentity identity,
                    FarVolumeBuildLimits limits,
                    Hooks hooks,
                    const std::shared_ptr<FarVolumeQueueShared>& state,
                    std::size_t index) {
        FarVolumeBuildResult result;
        result.identity = identity;
        try {
            build(world, result, limits, hooks, state, index);
        } catch (const std::bad_alloc&) {
            Fail(result, FarVolumeBuildStatus::BudgetRefused, "Far-volume allocation failed");
        } catch (const std::length_error& error) {
            Fail(result, FarVolumeBuildStatus::BudgetRefused, error.what());
        } catch (const std::invalid_argument& error) {
            Fail(result, FarVolumeBuildStatus::Invalid, error.what());
        } catch (const std::exception& error) {
            Fail(result, FarVolumeBuildStatus::Failed, error.what());
        } catch (...) {
            Fail(result, FarVolumeBuildStatus::Failed, "Unknown far-volume worker failure");
        }
        {
            std::lock_guard lock(state->mutex);
            auto& slot = state->slots[index];
            if (slot.cancellation.has_value())
                Fail(result, slot.cancellation.value(), "Far-volume request invalidated");
            slot.result = std::move(result);
            slot.reserved = Ready(slot.result.status);
            slot.state = SlotState::Complete;
            if (slot.result.status == FarVolumeBuildStatus::Stale)
                Increment(state->stale_results);
        }
        // The private fixture can hold the real JobSystem completion epilogue
        // while the owner consumes an already-visible result. Never hold the
        // slot mutex here, and never inspect a result the owner may have released.
        hooks.call(Phase::AfterPublish, identity);
    }
};

FarVolumeBuildQueue::FarVolumeBuildQueue(JobSystem& jobs, FarVolumeBuildLimits limits)
    : m_impl(std::make_unique<Impl>(jobs, limits)) {
    const FarVolumeBuildLimits ceiling;
    if (limits.generation.max_density_samples > ceiling.generation.max_density_samples ||
        limits.generation.max_bricks > ceiling.generation.max_bricks ||
        limits.generation.max_buffer_bytes > ceiling.generation.max_buffer_bytes ||
        limits.geometry.max_vertices > ceiling.geometry.max_vertices ||
        limits.geometry.max_indices > ceiling.geometry.max_indices ||
        limits.geometry.max_buffer_bytes > ceiling.geometry.max_buffer_bytes ||
        limits.converted_bytes > ceiling.converted_bytes)
        throw std::invalid_argument("Far-volume fixture limits exceed slot reservation");
    static_assert(sizeof(Impl) + sizeof(FarVolumeQueueShared) < 64u * 1024u);
}
FarVolumeBuildQueue::~FarVolumeBuildQueue() {
    m_impl->invalidate_binding();
    m_impl->wait();
}
bool FarVolumeBuildQueue::bind_fixture_world(const Systems::SHIELD_WorldSystem& world) {
    prepare_world_swap();
    if (m_impl->epoch == std::numeric_limits<std::uint64_t>::max())
        throw std::overflow_error("Far-volume binding epoch exhausted");
    const auto scope = world.acquire_worldgen_sample_scope();
    const auto stamp = ReadWorldStampUnderScope(world);
    if (stamp.authority_revision != 0)
        return false;
    m_impl->stamp = stamp;
    m_impl->world = &world;
    return true;
}
void FarVolumeBuildQueue::prepare_world_swap() {
    auto& impl = *m_impl;
    impl.check_owner();
    impl.invalidate_binding();
    impl.wait();
    impl.world = nullptr;
    Increment(impl.epoch);
    impl.descriptors = {};
    std::lock_guard lock(impl.shared->mutex);
    for (auto& slot : impl.shared->slots)
        if (slot.state != SlotState::Leased)
            slot = {};
}
FarVolumeSubmission FarVolumeBuildQueue::submit(const World::FarVolumeRequest& request) {
    auto& impl = *m_impl;
    impl.check_owner();
    if (!ValidRequest(request))
        return {FarVolumeSubmitStatus::Invalid};
    if (!impl.world)
        return {FarVolumeSubmitStatus::Unbound};
    if (!impl.binding_current()) {
        impl.invalidate_binding();
        return {FarVolumeSubmitStatus::IdentityChanged};
    }
    auto found = std::find_if(impl.descriptors.begin(), impl.descriptors.end(), [&](const auto& d) {
        return d.used && SameTile(d.identity.effective_request, request);
    });
    auto effective = request;
    if (found != impl.descriptors.end()) {
        const auto& previous = found->identity.effective_request;
        if (previous.extra_span.has_value()) {
            if (!effective.extra_span.has_value())
                effective.extra_span = previous.extra_span;
            else {
                auto& span = effective.extra_span.value();
                span.min_y = std::min(span.min_y, previous.extra_span.value().min_y);
                span.max_y = std::max(span.max_y, previous.extra_span.value().max_y);
            }
        }
        if (previous.caves == effective.caves &&
            SameSpan(previous.extra_span, effective.extra_span))
            return {FarVolumeSubmitStatus::Duplicate, found->identity.request_generation};
    } else {
        found = std::find_if(impl.descriptors.begin(),
                             impl.descriptors.end(),
                             [](const auto& descriptor) { return !descriptor.used; });
        if (found == impl.descriptors.end())
            return {FarVolumeSubmitStatus::Full};
    }
    if (impl.sequence == std::numeric_limits<std::uint64_t>::max())
        return {FarVolumeSubmitStatus::SequenceExhausted};
    if (found->used) {
        std::lock_guard lock(impl.shared->mutex);
        for (auto& slot : impl.shared->slots)
            if (slot.result.identity.request_generation == found->identity.request_generation)
                if (Impl::invalidate(slot, FarVolumeBuildStatus::Stale))
                    Increment(impl.shared->stale_results);
    }
    ++impl.sequence;
    *found = {true,
              true,
              false,
              {impl.epoch,
               impl.sequence,
               impl.stamp.params_hash,
               impl.stamp.authority_revision,
               impl.stamp.seed,
               request,
               effective}};
    return {FarVolumeSubmitStatus::Queued, impl.sequence};
}
bool FarVolumeBuildQueue::cancel(std::uint64_t generation) {
    auto& impl = *m_impl;
    impl.check_owner();
    for (auto& descriptor : impl.descriptors) {
        if (!descriptor.used || descriptor.identity.request_generation != generation)
            continue;
        descriptor.cancelled = true;
        descriptor.pending = false;
        std::lock_guard lock(impl.shared->mutex);
        for (auto& slot : impl.shared->slots)
            if (slot.result.identity.request_generation == generation)
                Impl::invalidate(slot, FarVolumeBuildStatus::Cancelled);
        return true;
    }
    return false;
}
bool FarVolumeBuildQueue::forget(std::uint64_t generation) {
    if (!cancel(generation))
        return false;
    for (auto& descriptor : m_impl->descriptors)
        if (descriptor.used && descriptor.identity.request_generation == generation)
            descriptor = {};
    return true;
}
bool FarVolumeBuildQueue::pump() {
    auto& impl = *m_impl;
    impl.check_owner();
    if (!impl.binding_current()) {
        impl.invalidate_binding();
        return false;
    }
    const auto pending = std::find_if(impl.descriptors.begin(),
                                      impl.descriptors.end(),
                                      [](const auto& d) { return d.used && d.pending; });
    if (pending == impl.descriptors.end())
        return false;
    std::size_t index = kFarVolumeResultCapacity;
    {
        std::lock_guard lock(impl.shared->mutex);
        for (std::size_t i = 0; i < kFarVolumeResultCapacity; ++i) {
            auto& slot = impl.shared->slots[i];
            if (slot.state == SlotState::Retired && Finished(slot.handle))
                slot = {};
            if (slot.state == SlotState::Free && index == kFarVolumeResultCapacity)
                index = i;
        }
        if (index == kFarVolumeResultCapacity)
            return false;
        auto& slot = impl.shared->slots[index];
        slot.state = SlotState::Running;
        slot.reserved = true;
        slot.result.identity = pending->identity;
        Increment(impl.shared->dispatches);
    }
    pending->pending = false;
    const auto state = impl.shared;
    try {
        const std::vector<Job> batch{[world = impl.world,
                                      identity = pending->identity,
                                      limits = impl.limits,
                                      hooks = impl.hooks,
                                      state,
                                      index] {
            Impl::run(*world, identity, limits, hooks, state, index);
        }};
        const auto handle = impl.jobs.dispatch_batch(batch, JobPriority::Normal);
        std::lock_guard lock(state->mutex);
        auto& slot = state->slots[index];
        slot.handle = handle;
        if (slot.state == SlotState::Running && Finished(handle)) {
            Fail(slot.result,
                 FarVolumeBuildStatus::Failed,
                 "JobSystem rejected far-volume dispatch");
            slot.reserved = false;
            slot.state = SlotState::Complete;
        }
    } catch (const std::exception& error) {
        std::lock_guard lock(state->mutex);
        auto& slot = state->slots[index];
        Fail(slot.result, FarVolumeBuildStatus::Failed, error.what());
        slot.reserved = false;
        slot.state = SlotState::Complete;
    }
    return true;
}
std::optional<FarVolumeResultLease> FarVolumeBuildQueue::take_completed() {
    auto& impl = *m_impl;
    impl.check_owner();
    const bool binding_current = impl.binding_current();
    if (!binding_current)
        impl.invalidate_binding();
    std::lock_guard lock(impl.shared->mutex);
    for (std::size_t index = 0; index < kFarVolumeResultCapacity; ++index) {
        auto& slot = impl.shared->slots[index];
        if (slot.state != SlotState::Complete)
            continue;
        if (Ready(slot.result.status) &&
            (!binding_current || !impl.current(slot.result.identity))) {
            Fail(
                slot.result, FarVolumeBuildStatus::Stale, "Far-volume result is no longer current");
            slot.reserved = false;
            Increment(impl.shared->stale_results);
        }
        slot.state = SlotState::Leased;
        return FarVolumeResultLease(impl.shared, index);
    }
    return std::nullopt;
}
bool FarVolumeBuildQueue::is_current(const FarVolumeBuildIdentity& identity) const {
    m_impl->check_owner();
    return m_impl->current(identity) && m_impl->binding_current();
}
FarVolumeQueueStats FarVolumeBuildQueue::stats() const {
    const auto& impl = *m_impl;
    impl.check_owner();
    FarVolumeQueueStats out;
    out.control_storage_bytes = sizeof(Impl) + sizeof(FarVolumeQueueShared);
    out.descriptor_stride = sizeof(Impl::Descriptor);
    out.result_slot_stride = sizeof(Slot);
    for (const auto& descriptor : impl.descriptors) {
        out.descriptors += descriptor.used ? 1u : 0u;
        out.pending += descriptor.used && descriptor.pending ? 1u : 0u;
    }
    std::lock_guard lock(impl.shared->mutex);
    for (const auto& slot : impl.shared->slots) {
        out.running += slot.state == SlotState::Running ? 1u : 0u;
        out.completed += slot.state == SlotState::Complete ? 1u : 0u;
        out.leased += slot.state == SlotState::Leased ? 1u : 0u;
        out.occupied_slots += slot.state != SlotState::Free ? 1u : 0u;
        out.payload_reserved_bytes += slot.reserved ? kFarVolumeSlotReservation : 0u;
        out.retained_mesh_bytes += slot.result.mesh.owned_bytes();
    }
    out.dispatches = impl.shared->dispatches;
    out.stale_results = impl.shared->stale_results;
    return out;
}
void FarVolumeBuildQueue::drain() {
    m_impl->check_owner();
    m_impl->wait();
}
void FarVolumeBuildQueue::set_test_hook(PhaseHook hook, void* context) {
    m_impl->check_owner();
    m_impl->wait();
    m_impl->hooks = {hook, context};
}
void FarVolumeBuildQueue::set_test_sequence(std::uint64_t sequence) {
    m_impl->check_owner();
    if (stats().descriptors != 0 || stats().occupied_slots != 0)
        throw std::logic_error("Cannot change a live far-volume fixture sequence");
    m_impl->sequence = sequence;
}

} // namespace Luminumbra::Rendering
