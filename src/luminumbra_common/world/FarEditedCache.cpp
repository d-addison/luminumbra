#include "FarEditedCache.h"

#include "core/FilesystemPath.h"
#include "persistence/WorldSaveService.h"
#include <algorithm>
#include <limits>
#include <tuple>

namespace Luminumbra::World {
class FarEditedCacheCatalogAccess {
public:
    static void Verify(FarCatalogReceipt& receipt, std::uint64_t generation, bool valid) {
        receipt.publication_verified_ = valid;
        receipt.publication_generation_ = generation;
    }
};
namespace {
using namespace Persistence;
using Error = FarCacheError;
FarEditedCacheSaveScope*& ActiveScope() {
    static thread_local FarEditedCacheSaveScope* scope = nullptr;
    return scope;
}

bool AddBytes(std::uint64_t n, std::uint64_t cap, std::uint64_t& total) {
    if (total > cap || n > cap - total)
        return false;
    total += n;
    return true;
}
bool ProductBytes(std::uint64_t n, std::uint64_t size, std::uint64_t cap, std::uint64_t& total) {
    return (n == 0 || size <= cap / n) && AddBytes(n * size, cap, total);
}
bool AdmitPipeline(const FarEditedCacheLimits& limits, FarVolumeMemoryLease& lease) {
    if (!limits.source.memory || limits.max_pipeline_bytes == 0 ||
        limits.numeric != FarVolumeNumericProfile::LegacySigned16 ||
        (limits.caves != FarCaveMode::BandLimited && limits.caves != FarCaveMode::BoxFiltered))
        return false;
    std::uint64_t bytes = 4096;
    if (!AddBytes(kFsv1MaxFileBytes, limits.max_pipeline_bytes, bytes) ||
        !ProductBytes(kFsv1MaxBricks, sizeof(FarOverlayBrick), limits.max_pipeline_bytes, bytes) ||
        !ProductBytes(limits.catalog.max_bytes, 256, limits.max_pipeline_bytes, bytes) ||
        !ProductBytes(limits.catalog.max_dependencies,
                      4 * sizeof(FarCatalogDependency) + 512,
                      limits.max_pipeline_bytes,
                      bytes) ||
        !ProductBytes(limits.source.max_tiles,
                      4 * sizeof(FarCacheDueTile) + 512,
                      limits.max_pipeline_bytes,
                      bytes) ||
        !ProductBytes(limits.bake.max_affected_bricks,
                      4 * sizeof(FarFrozenRegion),
                      limits.max_pipeline_bytes,
                      bytes) ||
        !ProductBytes(limits.bake.max_scratch_bytes, 3, limits.max_pipeline_bytes, bytes) ||
        !ProductBytes(limits.bake.max_output_bytes, 2, limits.max_pipeline_bytes, bytes) ||
        !ProductBytes(limits.bake.max_edits,
                      4 * sizeof(FarEditedFootprint),
                      limits.max_pipeline_bytes,
                      bytes))
        return false;
    lease = FarVolumeMemoryLease(*limits.source.memory);
    return lease.TryCharge(limits.max_pipeline_bytes);
}
bool Intersects(const FarEditedFootprint& a, const FarEditedFootprint& b) {
    return a.min.x <= b.max.x && a.max.x >= b.min.x && a.min.y <= b.max.y && a.max.y >= b.min.y &&
           a.min.z <= b.max.z && a.max.z >= b.min.z;
}
bool Relevant(const FarEditedFootprint& footprint, const EditedSourceTileSupport& selection) {
    if (Intersects(footprint, selection.support))
        return true;
    const auto step =
        static_cast<std::int64_t>(kFarTierTable[selection.key.tier - 1].sample_spacing_meters);
    for (int axis = 0; axis < 3; ++axis) {
        for (const auto sign : {-1, 1}) {
            auto support = selection.support;
            if (axis == 0) {
                support.min.x += sign * step;
                support.max.x += sign * step;
            } else if (axis == 1) {
                support.min.y += sign * step;
                support.max.y += sign * step;
            } else {
                support.min.z += sign * step;
                support.max.z += sign * step;
            }
            if (Intersects(footprint, support))
                return true;
        }
    }
    return false;
}
const FarCatalogReceipt* FindReceipt(const FarEditedCatalog& catalog, const FarTileKey& key) {
    const auto it = std::lower_bound(catalog.tiles.begin(),
                                     catalog.tiles.end(),
                                     key,
                                     [](const auto& r, const auto& k) { return r.key < k; });
    return it != catalog.tiles.end() && it->key == key ? &*it : nullptr;
}
Error Dependencies(const EditedSourceImage& image,
                   const EditedSourceTileSupport& selection,
                   const FarEditedCacheLimits& limits,
                   std::uint64_t& probes,
                   std::size_t& dependency_count,
                   std::vector<FarCatalogDependency>& out) {
    const auto members = image.Dependencies();
    for (const auto& entry : image.Inventory()) {
        if (probes == limits.max_reconcile_probes)
            return Error::WorkLimit;
        ++probes;
        if (!Relevant(entry.footprint, selection))
            continue;
        if (entry.member_index >= members.size())
            return Error::InvalidSource;
        if (dependency_count == limits.catalog.max_dependencies)
            return Error::WorkLimit;
        const auto& member = members[entry.member_index];
        if (!member.present || member.cls != MemberClass::Authoritative ||
            member.generation != entry.member_generation)
            return Error::InvalidSource;
        FarCatalogDependency dependency;
        dependency.member_path = member.path;
        dependency.chunk_id = entry.chunk_id;
        dependency.member_generation = entry.member_generation;
        dependency.member_bytes = member.bytes;
        dependency.member_digest = member.fnv64;
        dependency.chunk_digest = entry.authoritative_digest;
        dependency.footprint = entry.footprint;
        out.push_back(std::move(dependency));
        ++dependency_count;
    }
    std::sort(out.begin(), out.end(), [](const auto& a, const auto& b) {
        return std::tie(a.member_path, a.chunk_id) < std::tie(b.member_path, b.chunk_id);
    });
    return Error::None;
}
bool SameDependencies(const std::vector<FarCatalogDependency>& a,
                      const std::vector<FarCatalogDependency>& b) {
    return a.size() == b.size() &&
           std::equal(a.begin(), a.end(), b.begin(), SameFarCatalogDependency);
}
bool SamePolicy(const FarCatalogReceipt& r, const FarEditedCacheLimits& limits) {
    return r.terrain_identity == limits.terrain_identity &&
           r.content_identity == limits.content_identity && r.numeric == limits.numeric &&
           r.caves == limits.caves;
}
bool AddFootprints(std::span<const FarCatalogDependency> dependencies,
                   std::size_t cap,
                   std::vector<FarEditedFootprint>& changed) {
    for (const auto& d : dependencies) {
        const auto same = [&](const auto& f) {
            return f.min.x == d.footprint.min.x && f.min.y == d.footprint.min.y &&
                   f.min.z == d.footprint.min.z && f.max.x == d.footprint.max.x &&
                   f.max.y == d.footprint.max.y && f.max.z == d.footprint.max.z;
        };
        if (std::find_if(changed.begin(), changed.end(), same) != changed.end())
            continue;
        if (changed.size() == cap)
            return false;
        changed.push_back(d.footprint);
    }
    return true;
}
Error Reconcile(const EditedSourceImage& image,
                const FarEditedCatalog& catalog,
                const FarEditedCacheLimits& limits,
                FarCacheReconciliation& out,
                bool adopting = false) {
    if (ValidateFarEditedCatalog(catalog, limits.catalog) != FarCatalogError::None)
        return Error::InvalidCatalog;
    if (image.Selection().size() > limits.source.max_tiles ||
        image.Selection().size() > limits.catalog.max_tiles)
        return Error::WorkLimit;
    FarCacheReconciliation result;
    std::uint64_t probes = 0;
    std::size_t dependencies = 0;
    for (const auto& selection : image.Selection()) {
        FarCacheDueTile due;
        due.key = selection.key;
        auto error = Dependencies(image, selection, limits, probes, dependencies, due.dependencies);
        if (error != Error::None)
            return error;
        const auto* previous = FindReceipt(catalog, selection.key);
        if (!previous && due.dependencies.empty())
            continue;
        if (previous) {
            due.previous_span = previous->edited_span;
            // Do not let a smaller capture silently lose tall/deep prior contributors.
            for (const auto& d : previous->dependencies) {
                if (!Relevant(d.footprint, selection))
                    return Error::InvalidSource;
            }
            if (!adopting && !image.LegacyAuthority() &&
                previous->source_generation > image.Identity().generation)
                return Error::InvalidCatalog;
            const auto base_generation = image.Identity().state == FarEditedImageState::Prepared
                                             ? image.Identity().generation - 1
                                             : image.Identity().generation;
            if (previous->PublicationVerifiedFor(base_generation) &&
                SamePolicy(*previous, limits) &&
                SameDependencies(previous->dependencies, due.dependencies))
                continue;
            if (!AddFootprints(previous->dependencies, limits.bake.max_edits, result.changed))
                return Error::WorkLimit;
        }
        if (!AddFootprints(due.dependencies, limits.bake.max_edits, result.changed))
            return Error::WorkLimit;
        // Empty receipts retain their historical span after the last edit was removed.
        if (due.dependencies.empty() && previous && previous->dependencies.empty()) {
            if (result.changed.size() == limits.bake.max_edits)
                return Error::WorkLimit;
            const FarVolumePosition point{
                selection.support.min.x, previous->edited_span.min_y, selection.support.min.z};
            result.changed.push_back({point, point});
        }
        result.due.push_back(std::move(due));
    }
    out = std::move(result);
    return Error::None;
}
bool CacheBaseBytes(const EditedSourceImage& image,
                    const FarEditedCacheLimits& limits,
                    std::uint64_t& retained) {
    retained = sizeof(FarCommittedCache) + 512;
    return ProductBytes(image.Selection().size(),
                        2 * sizeof(FarOverlayTile),
                        limits.max_cache_bytes,
                        retained) &&
           ProductBytes(limits.catalog.max_bytes, 4, limits.max_cache_bytes, retained) &&
           ProductBytes(limits.catalog.max_dependencies,
                        2 * sizeof(FarCatalogDependency) + 256,
                        limits.max_cache_bytes,
                        retained);
}
Error LoadCatalog(const std::filesystem::path& root,
                  const FarEditedCacheLimits& limits,
                  FarEditedCatalog& catalog,
                  SnapshotIndex& index,
                  FarCommittedCache* cache = nullptr,
                  const EditedSourceImage* image = nullptr) {
    if (!read_snapshot_index(root, index, nullptr))
        return Error::InvalidSource;
    if (ReadFarEditedCatalog(root, limits.catalog, catalog) != FarCatalogError::None)
        return Error::InvalidCatalog;
    std::uint64_t retained = 0;
    if (cache && (!image || !CacheBaseBytes(*image, limits, retained)))
        return Error::ByteLimit;
    DerivedMemberState catalog_state = DerivedMemberState::Missing;
    bool byte_limit = false;
    for (auto& receipt : catalog.tiles)
        FarEditedCacheCatalogAccess::Verify(receipt, index.generation, false);
    const auto consume = [&](std::string_view path, DerivedMemberValidation&& verified) {
        if (path == "far/catalog.json") {
            catalog_state = verified.state;
            return true;
        }
        if (!verified.digest.present)
            return true;
        const auto& tile = verified.overlay;
        auto receipt = std::lower_bound(catalog.tiles.begin(),
                                        catalog.tiles.end(),
                                        tile.key,
                                        [](const auto& r, const auto& key) { return r.key < key; });
        if (receipt == catalog.tiles.end() || receipt->key != tile.key)
            return true;
        const bool valid = catalog_state == DerivedMemberState::Receipted &&
                           verified.state == DerivedMemberState::Receipted &&
                           verified.digest.bytes == receipt->overlay_bytes &&
                           verified.digest.fnv64 == receipt->overlay_fingerprint &&
                           tile.generation == receipt->source_generation;
        FarEditedCacheCatalogAccess::Verify(*receipt, index.generation, valid);
        if (cache && valid &&
            std::any_of(image->Selection().begin(),
                        image->Selection().end(),
                        [&](const auto& selected) { return selected.key == tile.key; })) {
            // The pipeline admits one full decode; retained selected tiles use the cache lease.
            if (!ProductBytes(tile.bricks.capacity(),
                              sizeof(FarOverlayBrick),
                              limits.max_cache_bytes,
                              retained)) {
                byte_limit = true;
                return false;
            }
            cache->tiles.push_back(std::move(verified.overlay));
        }
        return true;
    };
    // Catalog sorts before overlays. Validation, membership inspection and receipt checks share
    // this one decode per overlay, including corrupt/stale/unreceipted files outside the selection.
    if (!WorldSaveService::verify_derived_members(root,
                                                  index,
                                                  nullptr,
                                                  &InspectFarEditedCatalogDigest,
                                                  limits.source.max_directory_entries,
                                                  limits.source.max_file_bytes,
                                                  consume))
        return byte_limit ? Error::ByteLimit : Error::InvalidOverlay;
    return Error::None;
}
Error CompleteImage(const std::filesystem::path& root,
                    SaveTransaction* transaction,
                    const FarEditedCatalog& catalog,
                    const FarEditedCacheLimits& limits,
                    std::shared_ptr<const EditedSourceImage>& image) {
    if (limits.source.max_records > kMaxSnapshotMembers)
        return Error::WorkLimit;
    // Selection must cover the complete bricks that will be rebaked, rather than just the
    // changed chunk. Discover extra vertical contributors by bounded immutable recapture.
    std::vector<EditedSourceTileSupport> selection(image->Selection().begin(),
                                                   image->Selection().end());
    std::uint64_t probes = 0;
    for (std::size_t pass = 0; pass <= limits.source.max_records; ++pass) {
        std::vector<FarEditedFootprint> footprints;
        for (const auto& entry : image->Inventory()) {
            if (footprints.size() == limits.bake.max_edits)
                return Error::WorkLimit;
            footprints.push_back(entry.footprint);
        }
        bool expanded = false;
        for (auto& selected : selection) {
            const auto* previous = FindReceipt(catalog, selected.key);
            if (!previous)
                continue;
            if (!AddFootprints(previous->dependencies, limits.bake.max_edits, footprints))
                return Error::WorkLimit;
            const auto lo = std::min(selected.support.min.y, previous->edited_span.min_y);
            const auto hi = std::max(selected.support.max.y, previous->edited_span.max_y);
            expanded = expanded || lo != selected.support.min.y || hi != selected.support.max.y;
            selected.support.min.y = lo;
            selected.support.max.y = hi;
        }
        FarEditedAffectedSet affected;
        if (ComputeFarEditedAffectedSet(footprints, limits.bake, affected) !=
            FarEditedBakeError::None)
            return Error::WorkLimit;
        for (const auto& brick : affected.bricks) {
            if (probes == limits.max_reconcile_probes)
                return Error::WorkLimit;
            ++probes;
            const auto selected = std::lower_bound(
                selection.begin(), selection.end(), brick.tile, [](const auto& s, const auto& key) {
                    return s.key < key;
                });
            if (selected == selection.end() || selected->key != brick.tile)
                continue;
            const auto edge =
                static_cast<std::int64_t>(kFarTierTable[brick.tile.tier - 1].brick_edge_meters);
            const auto lo = std::min(selected->support.min.y, brick.by * edge);
            const auto hi =
                std::max(selected->support.max.y, (static_cast<std::int64_t>(brick.by) + 1) * edge);
            expanded = expanded || lo != selected->support.min.y || hi != selected->support.max.y;
            selected->support.min.y = lo;
            selected->support.max.y = hi;
        }
        if (!expanded)
            return Error::None;
        std::shared_ptr<const EditedSourceImage> complete;
        const auto capture =
            transaction
                ? CapturePreparedEditedSource(*transaction, {selection}, limits.source, complete)
                : CaptureCommittedEditedSource(
                      root, image->Identity().generation, {selection}, limits.source, complete);
        if (capture != EditedSourceError::None || !complete)
            return Error::CaptureFailure;
        image = std::move(complete);
    }
    return Error::WorkLimit;
}
Error Bake(const EditedSourceImage& image,
           const FarCacheReconciliation& plan,
           const FarEditedCacheLimits& limits,
           FarEditedBakeResult& out) {
    if (plan.due.empty())
        return Error::None;
    if (!limits.capture_source)
        return Error::CaptureFailure;
    // Capture the finite brick supports needed for complete rebake, including old removals.
    std::vector<FarEditedFootprint> footprints = plan.changed;
    if (footprints.size() > limits.bake.max_edits ||
        image.Inventory().size() > limits.bake.max_edits - footprints.size())
        return Error::WorkLimit;
    for (const auto& entry : image.Inventory())
        footprints.push_back(entry.footprint);
    FarEditedAffectedSet affected;
    if (ComputeFarEditedAffectedSet(footprints, limits.bake, affected) != FarEditedBakeError::None)
        return Error::WorkLimit;
    if (affected.bricks.size() > limits.frozen.max_regions)
        return Error::WorkLimit;
    std::vector<FarFrozenRegion> regions;
    regions.reserve(affected.bricks.size());
    for (const auto& brick : affected.bricks) {
        const auto edge =
            static_cast<std::int64_t>(kFarTierTable[brick.tile.tier - 1].brick_edge_meters);
        regions.push_back(
            {brick.tile.tier,
             {{brick.bx * edge, brick.by * edge, brick.bz * edge},
              {(brick.bx + 1ll) * edge, (brick.by + 1ll) * edge, (brick.bz + 1ll) * edge}}});
    }
    const auto boundary = limits.capture_boundary ? limits.capture_boundary() : nullptr;
    if (limits.capture_boundary && !boundary)
        return Error::InvalidSource;
    auto source = limits.capture_source(image);
    if (source.durable.state != image.Identity().state ||
        source.durable.generation != image.Identity().generation ||
        source.use != FarFrozenUse::Committed ||
        source.pristine.field_identity != limits.terrain_identity ||
        source.content_identity != limits.content_identity ||
        source.pristine.numeric != limits.numeric || source.pristine.caves != limits.caves ||
        source.live || source.parked || source.live_water || source.parked_water)
        return Error::InvalidSource;
    std::shared_ptr<const FarFrozenObservation> frozen;
    if (CaptureFarFrozenObservation(regions, source, limits.frozen, limits.source.memory, frozen) !=
            FarFrozenCaptureError::None ||
        !frozen)
        return Error::CaptureFailure;
    if (BakeFarEditedTilesFrozen(image, plan.changed, *frozen, limits.bake, out) !=
        FarEditedBakeError::None)
        return Error::BakeFailure;
    return Error::None;
}
Error UpdateCatalog(FarEditedCatalog& catalog,
                    const FarCacheReconciliation& plan,
                    const FarEditedBakeResult& baked,
                    const FarEditedCacheLimits& limits) {
    if (plan.due.empty())
        return Error::None;
    catalog.present = true;
    for (const auto& due : plan.due) {
        const auto tile = std::lower_bound(
            baked.tiles.begin(), baked.tiles.end(), due.key, [](const auto& t, const auto& k) {
                return t.overlay.key < k;
            });
        if (tile == baked.tiles.end() || tile->overlay.key != due.key ||
            !tile->edited_span.has_value())
            return Error::BakeFailure;
        FarCatalogReceipt receipt;
        receipt.key = due.key;
        receipt.path = Filesystem::GenericPathString(FarOverlayPath({}, due.key));
        receipt.source_generation = tile->overlay.generation;
        receipt.overlay_bytes = tile->bytes.size();
        receipt.overlay_fingerprint = fnv1a64_bytes(tile->bytes.data(), tile->bytes.size());
        receipt.terrain_identity = limits.terrain_identity;
        receipt.content_identity = limits.content_identity;
        receipt.numeric = limits.numeric;
        receipt.caves = limits.caves;
        receipt.edited_span = *tile->edited_span;
        if (due.previous_span.has_value()) {
            receipt.edited_span.min_y =
                std::min(receipt.edited_span.min_y, due.previous_span->min_y);
            receipt.edited_span.max_y =
                std::max(receipt.edited_span.max_y, due.previous_span->max_y);
        }
        receipt.dependencies = due.dependencies;
        const auto previous = std::lower_bound(
            catalog.tiles.begin(), catalog.tiles.end(), due.key, [](const auto& r, const auto& k) {
                return r.key < k;
            });
        if (previous != catalog.tiles.end() && previous->key == due.key)
            *previous = std::move(receipt);
        else {
            if (catalog.tiles.size() == limits.catalog.max_tiles)
                return Error::WorkLimit;
            catalog.tiles.insert(previous, std::move(receipt));
        }
    }
    return ValidateFarEditedCatalog(catalog, limits.catalog) == FarCatalogError::None
               ? Error::None
               : Error::InvalidCatalog;
}
bool SameMember(const SnapshotMember& a, const SnapshotMember& b) {
    return a.path == b.path && a.cls == b.cls && a.present == b.present && a.bytes == b.bytes &&
           a.fnv64 == b.fnv64 && a.generation == b.generation;
}
bool LegacyPaths(const SaveTransaction& transaction,
                 const FarEditedCacheLimits& limits,
                 std::vector<std::string>& paths) {
    if (transaction.was_indexed())
        return true;
    const auto directory = transaction.save_dir() / "far";
    if (!std::filesystem::exists(directory))
        return true;
    std::size_t visits = 0;
    for (const auto& entry : std::filesystem::directory_iterator(directory)) {
        if (visits == limits.source.max_directory_entries)
            return false;
        ++visits;
        const auto relative = "far/" + Filesystem::GenericPathString(entry.path().filename());
        const auto cls = ClassifySnapshotMember(relative);
        if (cls.has_value() && *cls == MemberClass::Derived) {
            if (paths.size() == std::min(limits.catalog.max_tiles, kMaxSnapshotMembers - 1))
                return false;
            paths.push_back(relative);
        }
    }
    std::sort(paths.begin(), paths.end());
    return true;
}
bool RemoveLegacyCache(SaveTransaction& transaction,
                       const FarEditedCacheLimits& limits,
                       FarCacheReport& report) {
    std::vector<std::string> paths;
    if (!LegacyPaths(transaction, limits, paths))
        return false;
    if (paths.empty())
        return true;
    report.derived_due = true;
    report.derived_success = false;
    const auto group = transaction.BeginDerivedGroup(paths, nullptr, &FarEditedCatalogDigest);
    if (!group.has_value())
        return false;
    if (!transaction.RequireDerivedGroupForCommit(*group, &WorldSaveService::member_digest))
        return false;
    try {
        for (const auto& path : paths)
            if (!transaction.RemoveDerivedMember(*group, path)) {
                report.refusal = Error::PublicationFailure;
                return false;
            }
        return true;
    } catch (...) {
        transaction.RollbackDerivedGroup(*group);
        report.refusal = Error::PublicationFailure;
        return false;
    }
}
Error ValidatePrepared(SaveTransaction& transaction,
                       const EditedSourceImage& image,
                       const std::filesystem::path& destination,
                       const FarEditedCacheLimits& limits,
                       std::shared_ptr<const EditedSourceImage>& verified) {
    if (SaveTransaction::active_for(destination) != &transaction ||
        transaction.save_dir() != destination ||
        image.Identity().state != FarEditedImageState::Prepared ||
        image.Identity().generation != transaction.new_generation())
        return Error::InvalidSource;
    if (CapturePreparedEditedSource(transaction, {image.Selection()}, limits.source, verified) !=
            EditedSourceError::None ||
        !verified)
        return Error::CaptureFailure;
    const auto a = image.Dependencies(), b = verified->Dependencies();
    if (a.size() != b.size() || !std::equal(a.begin(), a.end(), b.begin(), SameMember))
        return Error::InvalidSource;
    return Error::None;
}
bool Publish(SaveTransaction& transaction,
             const FarCacheReconciliation& plan,
             const FarEditedBakeResult& baked,
             const FarEditedCatalog& catalog,
             const FarEditedCacheLimits& limits,
             FarCacheReport& report,
             std::optional<SaveTransaction::DerivedGroup>* publication) {
    std::string bytes;
    if (EncodeFarEditedCatalog(catalog, limits.catalog, bytes) != FarCatalogError::None) {
        report.refusal = Error::InvalidCatalog;
        return RemoveLegacyCache(transaction, limits, report);
    }
    std::vector<std::string> paths{"far/catalog.json"};
    for (const auto& due : plan.due)
        paths.push_back(Filesystem::GenericPathString(FarOverlayPath({}, due.key)));
    std::vector<std::string> legacy;
    if (!LegacyPaths(transaction, limits, legacy)) {
        report.refusal = Error::WorkLimit;
        return false;
    }
    paths.insert(paths.end(), legacy.begin(), legacy.end());
    std::sort(paths.begin(), paths.end());
    paths.erase(std::unique(paths.begin(), paths.end()), paths.end());
    const auto group = transaction.BeginDerivedGroup(paths, nullptr, &FarEditedCatalogDigest);
    if (!group.has_value()) {
        report.refusal = Error::PublicationFailure;
        return legacy.empty(); // Adoption cannot stamp unchecked legacy generation claims.
    }
    if (!legacy.empty() &&
        !transaction.RequireDerivedGroupForCommit(*group, &WorldSaveService::member_digest)) {
        report.refusal = Error::PublicationFailure;
        return false;
    }
    if (publication)
        *publication = *group;
    bool written = true, evicted = false;
    try {
        for (const auto& due : plan.due) {
            const auto tile = std::lower_bound(
                baked.tiles.begin(), baked.tiles.end(), due.key, [](const auto& t, const auto& k) {
                    return t.overlay.key < k;
                });
            if (tile == baked.tiles.end() || tile->overlay.key != due.key ||
                !transaction.WriteDerivedMember(
                    *group,
                    Filesystem::GenericPathString(FarOverlayPath({}, due.key)),
                    tile->bytes)) {
                written = false;
                break;
            }
        }
        if (written)
            written = transaction.WriteDerivedMember(*group, "far/catalog.json", bytes);
        for (const auto& path : legacy) {
            if (!written)
                break;
            if (path != "far/catalog.json" &&
                std::none_of(plan.due.begin(), plan.due.end(), [&](const auto& due) {
                    return Filesystem::GenericPathString(FarOverlayPath({}, due.key)) == path;
                })) {
                written = transaction.RemoveDerivedMember(*group, path);
                evicted = true;
            }
        }
    } catch (...) {
        written = false;
    }
    if (!written) {
        if (!transaction.RollbackDerivedGroup(*group)) {
            report.refusal = Error::RollbackFailure;
            return false;
        }
        report.refusal = Error::PublicationFailure;
        return legacy.empty();
    }
    report.derived_due = evicted;
    report.derived_success = true;
    report.rebuilt_tiles = plan.due.size();
    return true;
}
Error CopyTiles(const EditedSourceImage& image,
                const FarCacheReconciliation& plan,
                FarEditedBakeResult& baked,
                const FarEditedCacheLimits& limits,
                FarCommittedCache& cache) {
    const auto due = [&](const auto& key) {
        return std::any_of(
            plan.due.begin(), plan.due.end(), [&](const auto& d) { return d.key == key; });
    };
    cache.tiles.erase(std::remove_if(cache.tiles.begin(),
                                     cache.tiles.end(),
                                     [&](const auto& tile) { return due(tile.key); }),
                      cache.tiles.end());
    std::uint64_t retained = 0;
    if (!CacheBaseBytes(image, limits, retained))
        return Error::ByteLimit;
    for (const auto& tile : cache.tiles)
        if (!ProductBytes(
                tile.bricks.capacity(), sizeof(FarOverlayBrick), limits.max_cache_bytes, retained))
            return Error::ByteLimit;
    for (const auto& selection : image.Selection()) {
        if (!due(selection.key))
            continue; // Unchanged tiles already own the decode validated by LoadCatalog.
        const auto tile =
            std::lower_bound(baked.tiles.begin(),
                             baked.tiles.end(),
                             selection.key,
                             [](const auto& t, const auto& k) { return t.overlay.key < k; });
        if (tile == baked.tiles.end() || tile->overlay.key != selection.key)
            return Error::BakeFailure;
        if (!ProductBytes(tile->overlay.bricks.capacity(),
                          sizeof(FarOverlayBrick),
                          limits.max_cache_bytes,
                          retained))
            return Error::ByteLimit;
        cache.tiles.push_back(std::move(tile->overlay));
    }
    std::sort(cache.tiles.begin(), cache.tiles.end(), [](const auto& a, const auto& b) {
        return a.key < b.key;
    });
    return Error::None;
}
} // namespace

FarCacheError ReconcileFarEditedCache(const EditedSourceImage& image,
                                      const FarEditedCatalog& catalog,
                                      const FarEditedCacheLimits& limits,
                                      FarCacheReconciliation& out) {
    if (image.Identity().state != FarEditedImageState::Committed || !image.RendererEligible())
        return Error::InvalidSource;
    try {
        FarVolumeMemoryLease pipeline;
        if (!AdmitPipeline(limits, pipeline))
            return Error::ByteLimit;
        FarCacheReconciliation result;
        const auto error = Reconcile(image, catalog, limits, result);
        if (error != Error::None)
            return error;
        result.memory = limits.source.memory;
        result.reservation = std::move(pipeline);
        out = std::move(result);
        return Error::None;
    } catch (const std::bad_alloc&) {
        return Error::AllocationFailure;
    } catch (const std::exception&) {
        return Error::InvalidSource;
    }
}
namespace {
bool FinalizeCaptured(SaveTransaction& transaction,
                      std::shared_ptr<const EditedSourceImage> verified,
                      const std::filesystem::path& destination,
                      const FarEditedCacheLimits& limits,
                      FarCacheReport& report,
                      std::optional<SaveTransaction::DerivedGroup>* publication = nullptr) {
    report = {};
    try {
        FarVolumeMemoryLease pipeline;
        if (!AdmitPipeline(limits, pipeline)) {
            report.refusal = Error::ByteLimit;
            return transaction.was_indexed() || !std::filesystem::exists(destination / "far");
        }
        if (!verified || SaveTransaction::active_for(destination) != &transaction ||
            transaction.save_dir() != destination ||
            verified->Identity().state != FarEditedImageState::Prepared ||
            verified->Identity().generation != transaction.new_generation()) {
            report.refusal = Error::InvalidSource;
            return false;
        }
        FarEditedCatalog catalog;
        SnapshotIndex index;
        report.refusal = LoadCatalog(destination, limits, catalog, index);
        if (report.refusal != Error::None)
            return false; // Present corruption must never be blessed by a save.
        report.refusal = CompleteImage(destination, &transaction, catalog, limits, verified);
        if (report.refusal != Error::None)
            return RemoveLegacyCache(transaction, limits, report);
        FarCacheReconciliation plan;
        report.refusal = Reconcile(*verified, catalog, limits, plan, !transaction.was_indexed());
        if (report.refusal != Error::None)
            return RemoveLegacyCache(transaction, limits, report);
        if (plan.due.empty()) {
            report.derived_success = true;
            report.derived_due = false;
            return RemoveLegacyCache(transaction, limits, report);
        }
        FarEditedBakeResult baked;
        report.refusal = Bake(*verified, plan, limits, baked);
        if (report.refusal != Error::None)
            return RemoveLegacyCache(transaction, limits, report);
        report.refusal = UpdateCatalog(catalog, plan, baked, limits);
        if (report.refusal != Error::None)
            return RemoveLegacyCache(transaction, limits, report);
        if (!transaction.was_indexed())
            catalog.tiles.erase(std::remove_if(catalog.tiles.begin(),
                                               catalog.tiles.end(),
                                               [&](const auto& receipt) {
                                                   return std::none_of(plan.due.begin(),
                                                                       plan.due.end(),
                                                                       [&](const auto& due) {
                                                                           return due.key ==
                                                                                  receipt.key;
                                                                       });
                                               }),
                                catalog.tiles.end());
        return Publish(transaction, plan, baked, catalog, limits, report, publication);
    } catch (const std::bad_alloc&) {
        report.refusal = Error::AllocationFailure;
        return transaction.was_indexed();
    } catch (const std::exception&) {
        report.refusal = Error::CaptureFailure;
        return transaction.was_indexed();
    }
}
} // namespace

bool FinalizeFarEditedCache(SaveTransaction& transaction,
                            const EditedSourceImage& image,
                            const std::filesystem::path& destination,
                            const FarEditedCacheLimits& limits,
                            FarCacheReport& report) {
    report = {};
    try {
        // External callers need a fresh producer check. The registered finalizer already owns
        // that exact capture and enters FinalizeCaptured directly, without rereading authority.
        std::shared_ptr<const EditedSourceImage> verified;
        report.refusal = ValidatePrepared(transaction, image, destination, limits, verified);
        if (report.refusal != Error::None)
            return RemoveLegacyCache(transaction, limits, report);
        return FinalizeCaptured(transaction, std::move(verified), destination, limits, report);
    } catch (const std::bad_alloc&) {
        report.refusal = Error::AllocationFailure;
        return transaction.was_indexed();
    } catch (const std::exception&) {
        report.refusal = Error::CaptureFailure;
        return transaction.was_indexed();
    }
}
FarCacheError AcquireFarCommittedCache(const FarCommittedCacheSelection& selection,
                                       std::uint64_t expected_generation,
                                       const FarEditedCacheLimits& limits,
                                       std::shared_ptr<const FarCommittedCache>& out) {
    try {
        FarVolumeMemoryLease pipeline;
        if (!AdmitPipeline(limits, pipeline))
            return Error::ByteLimit;
        std::shared_ptr<const EditedSourceImage> image;
        const auto capture = CaptureCommittedEditedSource(
            selection.save_root, expected_generation, selection.source, limits.source, image);
        if (capture != EditedSourceError::None || !image)
            return capture == EditedSourceError::GenerationMismatch ? Error::GenerationMismatch
                                                                    : Error::CaptureFailure;
        FarVolumeMemoryLease retained(*limits.source.memory);
        if (!retained.TryCharge(limits.max_cache_bytes) || limits.max_cache_bytes == 0)
            return Error::ByteLimit;
        auto cache = std::make_shared<FarCommittedCache>();
        cache->memory = limits.source.memory;
        cache->reservation = std::move(retained);
        SnapshotIndex index;
        auto error = LoadCatalog(
            selection.save_root, limits, cache->catalog, index, cache.get(), image.get());
        if (error != Error::None)
            return error;
        error = CompleteImage(selection.save_root, nullptr, cache->catalog, limits, image);
        if (error != Error::None)
            return error;
        cache->source = image;
        FarCacheReconciliation plan;
        error = Reconcile(*image, cache->catalog, limits, plan);
        if (error != Error::None)
            return error;
        FarEditedBakeResult baked;
        error = Bake(*image, plan, limits, baked);
        if (error != Error::None)
            return error; // Never return pristine substitution for pending edited authority.
        error = UpdateCatalog(cache->catalog, plan, baked, limits);
        if (error != Error::None)
            return error;
        error = CopyTiles(*image, plan, baked, limits, *cache);
        if (error != Error::None)
            return error;
        cache->report.authoritative_success = true;
        cache->report.derived_success = true;
        cache->report.derived_due =
            !plan.due.empty(); // In-memory reconstruction still needs durable publication.
        cache->report.rebuilt_tiles = plan.due.size();
        cache->report.committed_generation = expected_generation;
        out = std::move(cache);
        return Error::None;
    } catch (const std::bad_alloc&) {
        return Error::AllocationFailure;
    } catch (const std::exception&) {
        return Error::CaptureFailure;
    }
}
FarEditedCacheSaveScope::FarEditedCacheSaveScope(std::filesystem::path destination,
                                                 std::shared_ptr<const FarEditedCachePolicy> policy)
    : destination_(std::move(destination))
    , policy_(std::move(policy))
    , previous_(ActiveScope()) {
    ActiveScope() = this;
}
FarEditedCacheSaveScope::~FarEditedCacheSaveScope() {
    ActiveScope() = previous_;
}
std::shared_ptr<const FarEditedCachePolicy>
FarEditedCacheSaveScope::PolicyFor(const std::filesystem::path& destination) {
    for (auto* scope = ActiveScope(); scope; scope = scope->previous_) {
        if (scope->policy_ && scope->destination_ == destination)
            return scope->policy_;
    }
    return {};
}
bool RegisterFarEditedCacheFinalizer(SaveTransaction& transaction,
                                     std::shared_ptr<const FarEditedCachePolicy> policy) {
    if (!policy || transaction.HasFinalizer("far-edited-cache"))
        return true;
    // Reserve retained descriptors/callback controls before copying the caller's policy.
    if (!policy->limits.source.memory || policy->selection.size() > policy->limits.source.max_tiles)
        return false;
    struct Retention {
        std::shared_ptr<FarVolumeMemoryPool> memory;
        FarVolumeMemoryLease lease;
        std::shared_ptr<const FarEditedCachePolicy> policy;
    };
    auto retention = std::make_shared<Retention>();
    retention->memory = policy->limits.source.memory;
    retention->lease = FarVolumeMemoryLease(*retention->memory);
    std::uint64_t bytes = 4096;
    if (!ProductBytes(policy->selection.capacity(),
                      sizeof(EditedSourceTileSupport),
                      std::numeric_limits<std::uint64_t>::max(),
                      bytes) ||
        !retention->lease.TryCharge(bytes))
        return false;
    retention->policy = std::move(policy);
    return transaction.AddFinalizerOnce(
        "far-edited-cache", [retention](SaveTransaction& tx, std::vector<std::string>*) {
            const auto& p = *retention->policy;
            FarCacheReport report;
            std::optional<SaveTransaction::DerivedGroup> publication;
            std::shared_ptr<const EditedSourceImage> prepared;
            const auto error =
                CapturePreparedEditedSource(tx, {p.selection}, p.limits.source, prepared);
            if (error != EditedSourceError::None || !prepared) {
                report.refusal = Error::CaptureFailure;
                // Refuse adoption when uninspected legacy cache generations could become future.
                if (!tx.was_indexed() && std::filesystem::exists(tx.save_dir() / "far"))
                    return false;
            } else if (!FinalizeCaptured(
                           tx, std::move(prepared), tx.save_dir(), p.limits, report, &publication))
                return false;
            tx.defer_until_commit([retention, report, publication, &tx]() mutable {
                report.authoritative_success = true;
                report.committed_generation = tx.new_generation();
                if (publication && !tx.DerivedGroupCommitted(*publication)) {
                    report.derived_success = false;
                    report.derived_due = true;
                    report.refusal = Error::PublicationFailure;
                    report.rebuilt_tiles = 0;
                }
                if (retention->policy->after_commit)
                    retention->policy->after_commit(report);
            });
            return true;
        });
}
} // namespace Luminumbra::World
