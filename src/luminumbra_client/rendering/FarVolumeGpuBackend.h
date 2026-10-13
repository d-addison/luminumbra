#pragma once

#include "FarVolumeUpload.h"
#include "FarWorldOwnership.h"

#include <cstddef>
#include <cstdint>

namespace Luminumbra::Rendering {

enum class FarVolumeBufferKind {
    Vertex,
    Index
};
enum class FarVolumeVertexAttribute {
    Position = 0,
    Normal = 1,
    Material = 2
};

// Only the operations used by the uploader. Attribute formats are fixed:
// 0 position = 3 float, 1 normal = 3 float, 2 material = 1 unsigned integer.
// Driver and exact context outlive every allocation. All methods, including
// allocation access/destruction, belong to the context's owner thread.
class FarVolumeGpuDriver {
public:
    virtual ~FarVolumeGpuDriver() = default;
    virtual bool ValidContext() const noexcept = 0;
    // Reads one error only. An earlier error is consumed and causes refusal;
    // the uploader never drains or silently continues past unknown errors.
    virtual bool HasError() noexcept = 0;
    virtual void GetVertexArrayBinding(std::uint32_t& name) noexcept = 0;
    virtual void GetVertexBufferBinding(std::uint32_t& name) noexcept = 0;
    virtual void CreateVertexArray(std::uint32_t& name) noexcept = 0;
    virtual void CreateBuffer(std::uint32_t& name) noexcept = 0;
    virtual void DeleteVertexArray(std::uint32_t name) noexcept = 0;
    virtual void DeleteBuffer(std::uint32_t name) noexcept = 0;
    virtual void BindVertexArray(std::uint32_t name) noexcept = 0;
    virtual void BindBuffer(FarVolumeBufferKind kind, std::uint32_t name) noexcept = 0;
    virtual void
    UploadBuffer(FarVolumeBufferKind kind, std::intptr_t bytes, const void* data) noexcept = 0;
    virtual void GetBufferBytes(FarVolumeBufferKind kind, std::int64_t& bytes) noexcept = 0;
    virtual void SetAttribute(FarVolumeVertexAttribute attribute,
                              std::int32_t stride,
                              std::size_t offset) noexcept = 0;
    virtual void EnableAttribute(FarVolumeVertexAttribute attribute) noexcept = 0;
    // Fixed indexed GL_TRIANGLES / GL_UNSIGNED_INT, offset zero. Upload-only
    // drivers refuse by default; the renderer never assumes a draw capability.
    // Owner reserves the fixed logical store before entry. Empty-only fake
    // drivers need no GL; nonempty masks require an explicit backend.
    virtual bool BindLiveOwnership(std::span<const FarWorldLiveCell> cells) noexcept {
        return cells.empty();
    }
    virtual void UnbindLiveOwnership() noexcept {}
    virtual void ReleaseLiveOwnership() noexcept {}
    virtual bool DrawIndexed(std::int32_t index_count) noexcept {
        (void)index_count;
        return false;
    }
};

struct FarVolumeGpuSizes {
    std::uint64_t vertex_bytes = 0, index_bytes = 0, total_bytes = 0;
    std::int32_t index_count = 0;
};

// Pure count validation, also usable without constructing giant vectors.
bool CheckFarVolumeGpuSizes(std::uint64_t vertex_count,
                            std::uint64_t index_count,
                            FarVolumeGpuSizes& sizes) noexcept;

// One VAO, VBO and EBO, with immediate ownership even on partial creation.
// bytes() reports logical buffer stores, excluding driver overhead/physical VRAM.
// Resources MUST be unbound before retirement/replacement by the renderer.
// Destruction on the wrong thread/context terminates so the upstream charge
// cannot be released while GPU objects still exist. No deferred deletion.
class FarVolumeGlAllocation final : public FarVolumeGpuAllocation {
public:
    ~FarVolumeGlAllocation() noexcept override;
    FarVolumeGlAllocation(const FarVolumeGlAllocation&) = delete;
    FarVolumeGlAllocation& operator=(const FarVolumeGlAllocation&) = delete;
    FarVolumeGlAllocation(FarVolumeGlAllocation&&) = delete;
    FarVolumeGlAllocation& operator=(FarVolumeGlAllocation&&) = delete;
    std::uint64_t bytes() const noexcept override {
        return sizes_.total_bytes;
    }
    bool UsesDriver(const FarVolumeGpuDriver& driver) const noexcept {
        return &driver_ == &driver;
    }
    std::uint32_t native_vao() const noexcept {
        return vao_;
    }
    std::int32_t index_count() const noexcept {
        return sizes_.index_count;
    }

private:
    friend std::unique_ptr<FarVolumeGpuAllocation> UploadFarVolumeGpu(const FarVolumeRenderMesh&,
                                                                      FarVolumeGpuDriver&);
    FarVolumeGlAllocation(FarVolumeGpuDriver& driver, FarVolumeGpuSizes sizes) noexcept
        : driver_(driver)
        , sizes_(sizes) {}
    FarVolumeGpuDriver& driver_;
    FarVolumeGpuSizes sizes_;
    std::uint32_t vao_ = 0, vbo_ = 0, ebo_ = 0;
};

// Call only after UploadFarVolume's exact-byte admission. No extra reservation,
// cache, reuse or staging stores. Invalid geometry makes no driver calls.
// Saves/restores VAO and ARRAY_BUFFER (the saved VAO owns its EBO binding).
// Restoration is checked before success; guarantees require a functioning
// context. Allocation failure may throw before any object creation.
std::unique_ptr<FarVolumeGpuAllocation> UploadFarVolumeGpu(const FarVolumeRenderMesh& mesh,
                                                           FarVolumeGpuDriver& driver);

// Checked backend-specific adapter: rejects foreign allocation implementations
// and allocations from another driver/context. Restores VAO on every draw path.
bool DrawFarVolumeGpu(const FarVolumeGpuAllocation& allocation,
                      FarVolumeGpuDriver& driver) noexcept;

} // namespace Luminumbra::Rendering
