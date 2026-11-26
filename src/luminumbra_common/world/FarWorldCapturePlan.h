#pragma once

#include "FarVolumeRuntime.h"
#include "FarWorldAuthorityProducer.h"
#include <memory>
#include <span>
#include <vector>

namespace Luminumbra::World {

// Owner-thread planning only. The host fences publication during each pump.
// Discovery and frozen capture serialize use of the producer's sampling cursor.
// No production profile, scheduler, renderer binding or save identity is selected.
class FarWorldCapturePlan final {
public:
    struct Limits {
        std::size_t tiles = 0;
        std::uint64_t operations = 0;
        FarVolumeSpanLedgerLimits spans;
    };
    enum class Error {
        None,
        InvalidLimits,
        InvalidWanted,
        Capacity,
        InvalidSpan,
        SourceChanged,
        SourceRefused,
        WorkLimit,
        AllocationFailure,
        Inactive
    };
    struct Result {
        Error error = Error::None;
        FarWorldAuthorityProducer::FrozenResult source{};
        std::uint64_t work = 0;
        bool complete = false;
        bool ok() const {
            return error == Error::None;
        }
    };
    struct Tile {
        FarTileKey key;
        FarVolumeRequest request;
        std::uint64_t span_generation = 0;
    };
    struct Image {
        explicit Image(const std::shared_ptr<FarVolumeMemoryPool>& pool)
            : tiles(FarVolumePoolAllocator<Tile>(pool))
            , spans(FarVolumePoolAllocator<Tile>(pool)) {}
        std::uint64_t source_serial = 0, committed_generation = 0;
        std::vector<Tile, FarVolumePoolAllocator<Tile>> tiles;
        // Canonical monotone history, including keys that temporarily leave the
        // wanted set. Bounded to 512 entries; exhaustion refuses publication.
        std::vector<Tile, FarVolumePoolAllocator<Tile>> spans;
    };
    explicit FarWorldCapturePlan(const std::shared_ptr<FarVolumeMemoryPool>& pool);
    ~FarWorldCapturePlan();
    FarWorldCapturePlan(const FarWorldCapturePlan&) = delete;
    FarWorldCapturePlan& operator=(const FarWorldCapturePlan&) = delete;
    // Sorts and deduplicates at most the existing 512 producer slots. Invalid
    // keys/capacity/active transactions preserve wanted descriptors and image.
    Error SetWanted(std::span<const FarTileKey> keys);
    Error SetWanted(double camera_x, double camera_z);
    std::span<const FarTileKey> wanted_keys() const;
    std::span<const Persistence::WorldSaveService::AuthorityWindow> source_windows() const;
    Result Start(const std::shared_ptr<FarWorldAuthorityProducer>& producer,
                 const std::shared_ptr<const Systems::SHIELD_WorldSystem>& world,
                 const FarWorldAuthorityProducer::FrozenPolicy& policy,
                 const FarFrozenCaptureLimits& sampling,
                 const Limits& limits);
    // A unit starts one tile, pumps one producer unit, commits one descriptor,
    // or publishes the image. operations bounds the entire transaction. Refusal
    // (including expiry, invalid span and pool exhaustion) preserves accepted().
    Result Pump(std::uint64_t allowance);
    void Cancel();
    // Drop the binding and monotone history before using another world/epoch.
    void Reset();
    bool active() const;
    std::shared_ptr<const Image> accepted() const;

private:
    struct Impl;
    std::shared_ptr<FarVolumeMemoryPool> pool_;
    FarVolumeMemoryLease control_;
    std::shared_ptr<Impl> impl_;
};

} // namespace Luminumbra::World
