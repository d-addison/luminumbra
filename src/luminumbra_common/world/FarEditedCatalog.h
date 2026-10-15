#pragma once

#include "FarEditedBake.h"
#include "FarVolume.h"
#include "persistence/SaveTransaction.h"

namespace Luminumbra::World {

inline constexpr const char* kFarCatalogSchema = "luminumbra.far_catalog.v1";
struct FarCatalogLimits {
    std::size_t max_tiles = Persistence::kMaxSnapshotMembers;
    std::size_t max_dependencies = Persistence::kMaxSnapshotMembers;
    std::size_t max_bytes = Persistence::kMaxSnapshotIndexBytes;
};
enum class FarCatalogError : std::uint8_t {
    None,
    IoError,
    FutureVersion,
    Malformed,
    Ordering,
    PathMismatch,
    LimitExceeded
};
struct FarCatalogDependency {
    std::string member_path;
    std::uint64_t chunk_id = 0, member_generation = 0, member_bytes = 0;
    std::uint64_t member_digest = 0, chunk_digest = 0;
    FarEditedFootprint footprint;
};
struct FarCatalogReceipt {
    // Only the persistence verifier establishes this snapshot-local proof; never serialized.
    bool PublicationVerifiedFor(std::uint64_t generation) const {
        return publication_verified_ && publication_generation_ == generation;
    }
    FarTileKey key;
    std::string path;
    std::uint64_t source_generation = 0, overlay_bytes = 0, overlay_fingerprint = 0;
    std::uint64_t terrain_identity = 0, content_identity = 0;
    FarVolumeNumericProfile numeric = FarVolumeNumericProfile::LegacySigned16;
    FarCaveMode caves = FarCaveMode::BandLimited;
    FarVolumeSpan edited_span;
    std::vector<FarCatalogDependency> dependencies;

private:
    friend class FarEditedCacheCatalogAccess;
    bool publication_verified_ = false;
    std::uint64_t publication_generation_ = 0;
};
struct FarEditedCatalog {
    // Absence is adoption/reconstruction, never invented receipts.
    bool present = false;
    std::vector<FarCatalogReceipt> tiles;
};

bool SameFarCatalogDependency(const FarCatalogDependency&, const FarCatalogDependency&);
FarCatalogError ValidateFarEditedCatalog(const FarEditedCatalog&, const FarCatalogLimits&);
FarCatalogError
EncodeFarEditedCatalog(const FarEditedCatalog&, const FarCatalogLimits&, std::string& out);
FarCatalogError
DecodeFarEditedCatalog(std::string_view, const FarCatalogLimits&, FarEditedCatalog& out);
// Missing is a successful empty result. All refusals preserve out.
FarCatalogError
ReadFarEditedCatalog(const std::filesystem::path&, const FarCatalogLimits&, FarEditedCatalog& out);
// Strict bounded validator for the existing persistence catalog seam.
bool FarEditedCatalogDigest(const std::filesystem::path&,
                            std::string_view,
                            Persistence::MemberDigest&,
                            std::string& error);
// Installed baseline inspection uses the published index, including explicit legacy zero.
bool InspectFarEditedCatalogDigest(const std::filesystem::path&,
                                   std::string_view,
                                   Persistence::MemberDigest&,
                                   std::string& error);

} // namespace Luminumbra::World
