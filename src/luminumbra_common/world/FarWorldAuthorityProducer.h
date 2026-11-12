#pragma once

#include "FarFrozenObservation.h"
#include "FarVolumeMemory.h"
#include "FarWorldAuthorityChanges.h"
#include "persistence/WorldSaveService.h"
#include <bitset>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace Luminumbra::World {

// Inactive owner-thread source service. Copies require the world's publication
// boundary; no worker receives the index, mutable chunks, or the saved reader.
class FarWorldAuthorityProducer final {
public:
    static constexpr std::size_t kSlots = 512;
    struct Limits {
        std::size_t index_records = 0;
        std::uint64_t copy_bytes = 0;
        std::uint64_t query_columns = 0;
        // Required shared admission domain; may alias a handoff tier pool.
        std::shared_ptr<FarVolumeMemoryPool> memory;
    };
    enum class Error {
        None,
        InvalidLimits,
        InvalidWindow,
        IndexCapacity,
        CopyLimit,
        AuthorityUnavailable,
        WaterUnavailable,
        AllocationFailure,
        DeadlineExceeded,
        CommittedReadRefused
    };
    struct Image {
        IVec3 coords{};
        bool parked = false, terrain_authority = false, water_authority = false;
        int water_resolution = 0;
        std::vector<float, FarVolumePoolAllocator<float>> sdf;
        std::vector<std::uint8_t, FarVolumePoolAllocator<std::uint8_t>> material;
        std::vector<std::int32_t, FarVolumePoolAllocator<std::int32_t>> bed, depth;
    };
    using Images = std::vector<Image, FarVolumePoolAllocator<Image>>;
    struct DirtyTiles {
        std::bitset<kSlots> tiles;
        std::uint64_t visited = 0, terrain = 0, water = 0;
        bool overflow = false;
    };
    struct Counters {
        std::uint64_t tick_items = 0, query_items = 0;
        std::uint64_t copied_records = 0, copied_bytes = 0, dirty_overflows = 0;
    };
    struct CaptureLimits {
        Persistence::WorldSaveService::AuthorityReadLimits read;
        std::uint32_t deadline_frames = 0;
    };
    // Source assembly only. These owned inputs have no frozen-use eligibility.
    // The pointer returned by capture() lasts until replacement or destruction.
    struct SourceCapture {
        std::uint64_t serial = 0;
        Persistence::WorldSaveService::AuthorityWindow window;
        Images published;
        Persistence::WorldSaveService::AuthorityWindowResult committed;
    };
    struct CaptureStatus {
        Error error = Error::None;
        std::uint64_t serving_serial = 0, target_serial = 0;
        bool active = false, queued = false, paused = false, latest_ready = false;
    };
    struct FrozenPolicy {
        std::uint64_t pristine_identity = 0;
        FarCaveMode caves = FarCaveMode::BandLimited;
        FarVolumeNumericProfile numeric = FarVolumeNumericProfile::RawFloat;
        int water_resolution = 0;
    };
    struct FrozenResult {
        Error source = Error::None;
        FarFrozenCaptureError capture = FarFrozenCaptureError::None;
        bool ok() const {
            return source == Error::None && capture == FarFrozenCaptureError::None;
        }
    };
    explicit FarWorldAuthorityProducer(const Limits& limits);
    // Heap owners admit the object before allocation and transfer this same-pool
    // token. Stack construction admits its control storage in the convenience form.
    FarWorldAuthorityProducer(const Limits& limits, FarVolumeMemoryLease&& control);
    ~FarWorldAuthorityProducer();
    FarWorldAuthorityProducer(const FarWorldAuthorityProducer&) = delete;
    FarWorldAuthorityProducer& operator=(const FarWorldAuthorityProducer&) = delete;
    std::shared_ptr<FarWorldAuthorityChanges> changes() const;
    // Windows include the complete sampling/normal halo. Replacement is
    // transactional; at most 512 descriptors and dirty bits are retained.
    bool set_wanted(std::span<const Persistence::WorldSaveService::AuthorityWindow> wanted);
    DirtyTiles tick();
    Counters counters() const;
    // Refusal preserves output. Only published owned terrain and integer water
    // are copied; resident generation and staged arrays never become authority.
    Error copy_published(const Persistence::WorldSaveService::AuthorityWindow& window,
                         Images& output);
    bool admit_saved_source(const std::filesystem::path& root,
                            const Persistence::WorldSaveService::AuthorityReadLimits& limits,
                            std::string& error);
    Persistence::WorldSaveService::AuthorityWindowResult
    read_committed(const Persistence::WorldSaveService::AuthorityWindow& window,
                   const Persistence::WorldSaveService::AuthorityReadLimits& limits) const;

    // All calls run on the owner thread. Pump once per prepared frame; pause
    // stops admissions, while an already started capture still ages to refusal.
    bool configure_capture(const CaptureLimits& limits);
    bool request_capture(std::span<const Persistence::WorldSaveService::AuthorityWindow> windows);
    bool needs_capture_boundary() const;
    void pump_capture();
    void fence_source_reads(bool paused);
    void invalidate_source();
    bool refresh_saved_source(const std::filesystem::path& root, std::string& error);
    const SourceCapture* capture() const;
    CaptureStatus capture_status() const;
    // Bounded to 255 bytes, independent of hostile member/path diagnostics.
    std::string_view capture_read_error() const;
    // Synchronous composition from the accepted owned source capture. The world
    // is used only for pristine terrain/base water under its sampling scope.
    // The complete rectangular halo must lie in that capture's source window.
    // Refusal preserves output and its pool charge; no mutable source is reread.
    FrozenResult compose_frozen(const Systems::SHIELD_WorldSystem& world,
                                const FarVolumeRequest& request,
                                const FrozenPolicy& policy,
                                const FarFrozenCaptureLimits& limits,
                                const std::shared_ptr<FarVolumeMemoryPool>& pool,
                                std::shared_ptr<const FarFrozenObservation>& output) const;

private:
    struct Impl;
    std::shared_ptr<FarVolumeMemoryPool> memory_;
    FarVolumeMemoryLease control_;
    std::shared_ptr<Impl> impl_;
};
struct FarWorldAuthorityConfig {
    FarWorldAuthorityProducer::Limits source;
    FarWorldAuthorityProducer::CaptureLimits capture;
};
} // namespace Luminumbra::World
