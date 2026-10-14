#include "FarVolumeUpload.h"

#include <cmath>
#include <limits>

namespace Luminumbra::Rendering {
namespace {
bool ValidIntent(const World::FarVolumeCoverageIntent& intent) {
    World::FarTileKey key;
    return World::ToFarTileKey(intent.key, key) &&
           (!intent.extra_span ||
            (std::isfinite(intent.extra_span->min_y) && std::isfinite(intent.extra_span->max_y) &&
             intent.extra_span->min_y <= intent.extra_span->max_y));
}
bool Covers(const World::FarVolumeRequest& request, const World::FarVolumeCoverageIntent& intent) {
    return request.key == intent.key &&
           (!intent.extra_span || (request.span.min_y <= intent.extra_span->min_y &&
                                   request.span.max_y >= intent.extra_span->max_y));
}
} // namespace
FarVolumeUploadedTile::FarVolumeUploadedTile(FarVolumeUploadedTile&& other) noexcept {
    *this = std::move(other);
}
FarVolumeUploadedTile& FarVolumeUploadedTile::operator=(FarVolumeUploadedTile&& other) noexcept {
    if (this != &other) {
        resource_.reset();
        charge_.reset();
        if (other.charge_) {
            charge_.emplace(std::move(*other.charge_));
            other.charge_.reset();
        }
        resource_ = std::move(other.resource_);
        valid_ = other.valid_;
        empty_ = other.empty_;
        identity_ = other.identity_;
        request_ = other.request_;
        other.valid_ = false;
    }
    return *this;
}
World::FarTileStatus
FarVolumeUploadedTile::Status(const World::FarBuildIdentity& current,
                              const World::FarVolumeCoverageIntent& intent) const noexcept {
    return {valid_ ? (empty_ ? World::FarTileState::Empty : World::FarTileState::Uploaded)
                   : World::FarTileState::Wanted,
            valid_ && identity_ == current,
            valid_ && ValidIntent(intent) && Covers(request_, intent)};
}
bool UploadFarVolume(const FarVolumeQueueCompletion& completion,
                     const World::FarBuildIdentity& expected,
                     const World::FarVolumeCoverageIntent& intent,
                     World::FarVolumeTierMemory& memory,
                     std::uint64_t max_upload_bytes,
                     const FarVolumeUploadBackend& backend,
                     FarVolumeUploadedTile& output,
                     FarVolumeUploadError* error) {
    auto fail = [error](FarVolumeUploadError why) {
        if (error)
            *error = why;
        return false;
    };
    if (error)
        *error = FarVolumeUploadError::None;
    if (!ValidIntent(intent))
        return fail(FarVolumeUploadError::InvalidIntent);
    if (completion.status() != FarVolumeBuildStatus::Ready &&
        completion.status() != FarVolumeBuildStatus::ReadyEmpty)
        return fail(FarVolumeUploadError::NotReady);
    World::FarTileKey key;
    if (!World::ToFarTileKey(intent.key, key) || key != expected.key ||
        completion.identity() != expected || !Covers(completion.mesh().request, intent))
        return fail(FarVolumeUploadError::Stale);
    if (completion.memory_pool() != memory.Pool(expected.key.tier))
        return fail(FarVolumeUploadError::InvalidBinding);
    FarVolumeUploadedTile staged;
    staged.valid_ = true;
    staged.empty_ = completion.status() == FarVolumeBuildStatus::ReadyEmpty;
    staged.identity_ = expected;
    staged.request_ = completion.mesh().request;
    if (!staged.empty_) {
        const auto& mesh = completion.mesh();
        constexpr auto maximum = std::numeric_limits<std::uint64_t>::max();
        if (mesh.vertices.size() > maximum / sizeof(VoxelVertex) ||
            mesh.indices.size() > maximum / sizeof(std::uint32_t))
            return fail(FarVolumeUploadError::ByteLimit);
        const auto vertex_bytes = std::uint64_t(mesh.vertices.size()) * sizeof(VoxelVertex);
        const auto index_bytes = std::uint64_t(mesh.indices.size()) * sizeof(std::uint32_t);
        if (index_bytes > maximum - vertex_bytes || vertex_bytes + index_bytes > max_upload_bytes)
            return fail(FarVolumeUploadError::ByteLimit);
        const auto bytes = vertex_bytes + index_bytes;
        if (!backend)
            return fail(FarVolumeUploadError::BackendFailure);
        auto charge = memory.ReserveGpu(expected.key.tier, bytes);
        if (!charge)
            return fail(FarVolumeUploadError::AdmissionRefused);
        staged.charge_.emplace(std::move(*charge));
        try {
            staged.resource_ = backend(mesh);
        } catch (...) {
            return fail(FarVolumeUploadError::BackendFailure);
        }
        if (!staged.resource_ || staged.resource_->bytes() != bytes)
            return fail(FarVolumeUploadError::BackendFailure);
    }
    // Owner backend may change the current binding/intent before it returns.
    if (!ValidIntent(intent))
        return fail(FarVolumeUploadError::InvalidIntent);
    if (staged.identity_ != expected || !Covers(staged.request_, intent))
        return fail(FarVolumeUploadError::Stale);
    output = std::move(staged);
    return true;
}
} // namespace Luminumbra::Rendering
