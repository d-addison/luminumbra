#pragma once

#include "FarFrozenObservation.h"
#include "FarWorldAuthorityChanges.h"
#include "persistence/WorldSaveService.h"
#include <bitset>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
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
        std::vector<float> sdf;
        std::vector<std::uint8_t> material;
        std::vector<std::int32_t> bed, depth;
    };
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
        std::vector<Image> published;
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
                         std::vector<Image>& output);
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
    const std::string& capture_read_error() const;
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
    std::unique_ptr<Impl> impl_;
};
struct FarWorldAuthorityConfig {
    FarWorldAuthorityProducer::Limits source;
    FarWorldAuthorityProducer::CaptureLimits capture;
};
} // namespace Luminumbra::World
