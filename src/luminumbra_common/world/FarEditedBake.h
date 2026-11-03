#pragma once

#include "FarAuthorityOverlay.h"
#include "FarVolumeTile.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace Luminumbra::World {

// Closed world-metre sample footprint, including the upper chunk boundary.
struct FarEditedFootprint {
    FarVolumePosition min, max;
};

enum class FarEditedImageState : std::uint8_t {
    Prepared,
    Committed
};
struct FarEditedImageIdentity {
    std::uint64_t generation = 0;
    FarEditedImageState state = FarEditedImageState::Committed;
};

// Complete copied authoritative lattice, x fastest, then y, then z. Its closed
// footprint is origin .. origin + (side - 1) * spacing on each axis. Empty
// material means material 0; a nonempty material array must match density size.
struct FarEditedLattice {
    FarVolumePosition origin;
    std::uint32_t spacing_meters = 1;
    std::array<std::uint32_t, 3> side{};
    std::span<const float> density;
    std::span<const std::uint8_t> material;
    // Generation of the contributing indexed region member, not voxel revision.
    std::uint64_t member_generation = 0;
};

// Producer seam: the implementation owns every lattice and all reachable data,
// including disk-only edited records, for its entire immutable lifetime. The
// inventory must be complete for due tiles and their sampling/normal halos;
// unreadable expected authority must refuse capture rather than disappear here.
// Capture, provenance and prepared/committed eligibility belong to the producer.
// A prepared image may be baked, but these bytes are publishable only after its
// transaction commits. An identity stamp alone does not prove eligibility.
// Both accessors return bounded views without I/O or world callbacks.
class FarEditedSourceView {
public:
    virtual ~FarEditedSourceView() = default;
    virtual FarEditedImageIdentity Identity() const = 0;
    virtual std::span<const FarEditedLattice> Lattices() const = 0;
};

struct FarEditedBrickKey {
    FarTileKey tile;
    std::int32_t bx = 0, by = 0, bz = 0;
    bool operator==(const FarEditedBrickKey&) const = default;
};
bool operator<(const FarEditedBrickKey& a, const FarEditedBrickKey& b);

struct FarEditedAffectedSet {
    // Canonical (tier, tz, tx, bz, bx, by) order, with no duplicates.
    std::vector<FarEditedBrickKey> bricks;
    std::vector<FarTileKey> tiles;
};

struct FarEditedBakeLimits {
    // Required caller caps; zero admits no work. These are not shipping defaults.
    std::size_t max_edits = 0, max_source_records = 0, max_affected_bricks = 0, max_tiles = 0;
    // Candidate visits include duplicates, changed footprints and full inventory.
    std::array<std::uint64_t, kFarTierCount> max_brick_visits_per_tier{};
    std::uint64_t max_source_samples = 0, max_source_probes = 0, max_density_samples = 0;
    // Requested vector storage and worst-case retained brick/encoded payloads.
    // Caller-owned images, previous output and allocator bookkeeping are excluded.
    std::uint64_t max_scratch_bytes = 0, max_output_bytes = 0;
};

enum class FarEditedBakeError : std::uint8_t {
    None,
    InvalidFootprint,
    CoordinateRange,
    WorkLimit,
    MemoryLimit,
    InvalidLattice,
    InvalidMaterial,
    NonFiniteDensity,
    BoundaryConflict,
    IdentityMismatch,
    InvalidSampler,
    SamplerFailure,
    EncodingFailure,
    AllocationFailure
};

// Optional affected-set diagnostics, reset on entry and retained on refusal.
// Slots count requested vector reservations; visits count enumerated candidates,
// including duplicates and candidates rejected by the support test.
struct FarEditedAffectedWork {
    std::array<std::uint64_t, kFarTierCount> candidate_visits_per_tier{};
    std::size_t reserved_brick_slots = 0, reserved_tile_slots = 0;
    std::size_t unique_bricks = 0, unique_tiles = 0;
};

// Intersect closed brick sample supports, including the six central-difference
// normal supports displaced by one tier spacing. Checks footprint lower bounds
// and candidate work before allocation/enumeration, then caps unique set growth.
// Negative coordinates use floor division. Failure preserves out.
FarEditedBakeError ComputeFarEditedAffectedSet(std::span<const FarEditedFootprint> edits,
                                               const FarEditedBakeLimits& limits,
                                               FarEditedAffectedSet& out,
                                               FarEditedAffectedWork* work = nullptr);

// Frozen pristine observations (or a separately sealed immutable field) for the
// finite request and halo, with the caller's terrain/cave policy already bound.
// Sampling must be bounded, pure and repeatable by coordinate/tier; it never
// reads a live world or changing save paths. The tier is one-based.
using FarEditedPristineSampler =
    std::function<std::optional<FarVolumeSample>(std::uint8_t, const FarVolumePosition&)>;

struct FarEditedBakedTile {
    FarOverlayTile overlay;
    std::string bytes;
    std::uint64_t max_member_generation = 0; // Includes normal-halo contributors.
    // Requested edited extent, including changed/removed footprints and halos.
    // The caller unions this with its existing span ledger; rebake never resets it.
    std::optional<FarVolumeSpan> edited_span;
};

struct FarEditedBakeResult {
    FarEditedImageIdentity source;
    FarEditedAffectedSet affected;
    std::vector<FarEditedBakedTile> tiles;
};

// Rebuild every due tile's COMPLETE edited-brick set from the inventory, not just
// changed records. An empty rebuilt overlay removes old overrides. Lattice
// samples are interpolated in double in Z/Y/X order; material uses the nearest
// lattice node (ties choose the lower node). All overlapping observations must
// agree before quantization. Exact zero stays Present. FSV1 v1 is unchanged.
// Validates all source samples and shared lattice nodes, even when a coarse
// lattice misses an edit. Every refusal preserves the complete previous output.
FarEditedBakeError BakeFarEditedTiles(const FarEditedSourceView& source,
                                      std::span<const FarEditedFootprint> changed,
                                      const FarEditedPristineSampler& pristine,
                                      const FarEditedBakeLimits& limits,
                                      FarEditedBakeResult& out);

} // namespace Luminumbra::World
