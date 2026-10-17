#pragma once

#include "SaveTransaction.h"
#include "luminumbra/core/Types.h"
#include "world/FarEditedBake.h"
#include "world/FarVolumeMemory.h"

#include <functional>
#include <memory>
#include <span>
#include <vector>

namespace Luminumbra {
class Chunk;
}

namespace Luminumbra::Persistence {

struct EditedSourceTileSupport {
    World::FarTileKey key;
    // Complete closed tile sample support. Capture adds six axial normal supports.
    World::FarEditedFootprint support;
};
struct EditedSourceSelection {
    std::span<const EditedSourceTileSupport> tiles;
};
struct EditedSourceLimits {
    std::size_t max_tiles = 0, max_members = 0, max_records = 0, max_directory_entries = 0;
    std::uint64_t max_file_bytes = 0, max_decode_bytes = 0, max_samples = 0;
    std::uint64_t max_boundary_probes = 0, max_selection_probes = 0;
    std::uint64_t max_image_bytes = 0, max_scratch_bytes = 0;
    // Required shared budget: aliases retain both the pool and their allocation reservation.
    std::shared_ptr<World::FarVolumeMemoryPool> memory;
};
enum class EditedSourceError : std::uint8_t {
    None,
    InvalidSelection,
    GenerationMismatch,
    Unavailable,
    InvalidRecord,
    BoundaryConflict,
    WorkLimit,
    ByteLimit,
    AllocationFailure,
    Ineligible
};
struct EditedSourceInventoryEntry {
    std::uint64_t chunk_id = 0;
    World::FarEditedFootprint footprint;
    std::size_t member_index = 0;
    std::uint64_t member_generation = 0, authoritative_digest = 0;
};
struct EditedSourceWater {
    std::uint64_t chunk_id = 0;
    bool has_sim = false, sleeping = false;
    int resolution = 0, ticks_below_threshold = 0;
    float max_delta = 0;
    std::vector<float> level, terrain_height, rest_level;
    std::vector<Vec2> flow;
    std::vector<std::int32_t> depth_mm, bed_mm, edge_flux, source_mm;
};
struct EditedSourceData;

// Only persistence factories construct durable images. Accessors borrow owned immutable data;
// they perform no I/O and retain no mutable chunk or world callback.
class EditedSourceImage final : public World::FarEditedSourceView {
public:
    World::FarEditedImageIdentity Identity() const override;
    std::span<const World::FarEditedLattice> Lattices() const override;
    std::span<const EditedSourceInventoryEntry> Inventory() const;
    std::span<const SnapshotMember> Dependencies() const;
    std::span<const EditedSourceWater> Water() const;
    std::span<const EditedSourceTileSupport> Selection() const;
    bool LegacyAuthority() const;
    bool RendererEligible() const;
    std::shared_ptr<const SnapshotCommitReceipt> PublicationReceipt() const;
    std::uint64_t ReservedBytes() const;

private:
    friend class EditedSourceCaptureAccess;
    EditedSourceImage(std::shared_ptr<const EditedSourceData> data,
                      World::FarEditedImageIdentity identity,
                      std::shared_ptr<EditedSourcePublicationToken> publication,
                      std::shared_ptr<const void> handle_reservation = {});
    std::shared_ptr<const void> m_handleReservation;
    std::shared_ptr<const EditedSourceData> m_data;
    World::FarEditedImageIdentity m_identity;
    std::shared_ptr<EditedSourcePublicationToken> m_publication;
};

// Volatile layers cannot be supplied as a durable bake source or renderer receipt.
class EditedSourcePreviewLayer final {
public:
    std::span<const World::FarEditedLattice> Lattices() const;
    std::span<const EditedSourceInventoryEntry> Inventory() const;
    std::span<const EditedSourceWater> Water() const;

private:
    friend class EditedSourceCaptureAccess;
    explicit EditedSourcePreviewLayer(std::shared_ptr<const EditedSourceData> data);
    std::shared_ptr<const EditedSourceData> m_data;
};
struct EditedSourceCapture {
    std::shared_ptr<const EditedSourceImage> committed;
    std::shared_ptr<const EditedSourcePreviewLayer> live, parked;
};

// Owner-thread only, at a quiescent publication boundary. All refusals preserve out.
// Committed capture recovers before reading. It refuses an open transaction in this directory.
// Absent index is validated legacy generation zero; capture never stamps or invents edits.
// Invalid density samples/counts or materials outside integer [0, 255] refuse with InvalidRecord.
EditedSourceError CaptureCommittedEditedSource(const std::filesystem::path& save_root,
                                               std::uint64_t expected_generation,
                                               const EditedSourceSelection& selection,
                                               const EditedSourceLimits& limits,
                                               std::shared_ptr<const EditedSourceImage>& out);
EditedSourceError CapturePreparedEditedSource(SaveTransaction& transaction,
                                              const EditedSourceSelection& selection,
                                              const EditedSourceLimits& limits,
                                              std::shared_ptr<const EditedSourceImage>& out);
// Creates a new committed receipt retaining the same payload, only after the exact captured
// dependencies committed. The original shared image keeps its Prepared identity forever.
EditedSourceError
PublishCommittedEditedSource(const std::shared_ptr<const EditedSourceImage>& prepared,
                             std::shared_ptr<const EditedSourceImage>& out);

using EditedSourcePreviewVisitor = std::function<bool(const Chunk&, bool parked)>;
using EditedSourcePreviewProducer =
    std::function<bool(std::size_t max_records, const EditedSourcePreviewVisitor&)>;
EditedSourceError
CaptureEditedSourcePreview(const EditedSourcePreviewProducer& producer,
                           const EditedSourceSelection& selection,
                           const EditedSourceLimits& limits,
                           std::shared_ptr<const EditedSourcePreviewLayer>& live,
                           std::shared_ptr<const EditedSourcePreviewLayer>& parked);

} // namespace Luminumbra::Persistence
