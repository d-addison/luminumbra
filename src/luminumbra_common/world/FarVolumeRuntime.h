#pragma once

#include "FarVolume.h"
#include "FarWantedSet.h"

namespace Luminumbra::World {

// Runtime keys address the same tier table as the canonical volume model.
// Conversion also admits the complete horizontal tile plus its normal halo in
// the facade's exact-coordinate domain. Refusal leaves output unchanged.
bool ToFarVolumeTileKey(const FarTileKey& key, FarVolumeTileKey& output);
bool ToFarTileKey(const FarVolumeTileKey& key, FarTileKey& output);

struct FarVolumeSpanLedgerLimits {
    // Explicit caller limits on requested extensions, not shipped runtime caps.
    float min_y = 0, max_y = 0;
    float max_extent_meters = 0;
};

// Allocation-free per-tile requested-extension ledger. The facade still unions
// this extension with the complete pristine surface band. Camera, anchor,
// sightline and durable-chunk requests all use Extend; accepted requests never
// shrink until Reset at a new world binding. The owner must retain world_epoch
// in FarBuildIdentity when resetting so prior results cannot become current.
class FarVolumeSpanLedger {
public:
    bool Reset(const FarTileKey& key);
    bool Extend(const FarVolumeCoverageSpan& request, const FarVolumeSpanLedgerLimits& limits);
    std::optional<FarVolumeCoverageIntent> Intent() const;
    std::uint64_t generation() const noexcept {
        return generation_;
    }
    // Whether a resolved build span contains every accepted extension. Currency
    // additionally requires the complete FarBuildIdentity to match.
    bool Covers(const FarVolumeSpan& built) const;

private:
    std::optional<FarVolumeTileKey> key_;
    std::optional<FarVolumeCoverageSpan> extra_;
    std::uint64_t generation_ = 0;
};

} // namespace Luminumbra::World
