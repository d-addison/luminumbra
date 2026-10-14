#include "FarVolumeGpuBackend.h"

#include <exception>
#include <limits>
#include <type_traits>
#include <utility>

namespace Luminumbra::Rendering {
namespace {
static_assert(std::is_standard_layout_v<VoxelVertex>);
static_assert(std::is_same_v<decltype(std::declval<VoxelVertex>().material_id), std::uint32_t>);
static_assert(std::is_same_v<decltype(std::declval<VoxelVertex>().position.x), float>);
static_assert(std::is_same_v<decltype(std::declval<VoxelVertex>().normal.x), float>);
static_assert(std::is_standard_layout_v<Vec3>);
static_assert(sizeof(float) == 4);
static_assert(offsetof(Vec3, x) == 0);
static_assert(offsetof(Vec3, y) == sizeof(float));
static_assert(offsetof(Vec3, z) == 2 * sizeof(float));
static_assert(sizeof(VoxelVertex) <= std::numeric_limits<std::int32_t>::max());

class RestoreBindings {
public:
    RestoreBindings(FarVolumeGpuDriver& driver, std::uint32_t vao, std::uint32_t vbo) noexcept
        : driver_(driver)
        , vao_(vao)
        , vbo_(vbo) {}
    ~RestoreBindings() {
        if (active_)
            Restore();
    }
    bool Restore() noexcept {
        active_ = false;
        driver_.BindVertexArray(vao_);
        const bool vao_ok = !driver_.HasError();
        driver_.BindBuffer(FarVolumeBufferKind::Vertex, vbo_);
        const bool vbo_ok = !driver_.HasError();
        return vao_ok && vbo_ok;
    }

private:
    FarVolumeGpuDriver& driver_;
    std::uint32_t vao_, vbo_;
    bool active_ = true;
};
} // namespace

bool CheckFarVolumeGpuSizes(std::uint64_t vertices,
                            std::uint64_t indices,
                            FarVolumeGpuSizes& sizes) noexcept {
    constexpr auto maximum = std::numeric_limits<std::uint64_t>::max();
    constexpr auto store_max = std::numeric_limits<std::intptr_t>::max();
    constexpr auto count_max = std::numeric_limits<std::int32_t>::max();
    if (!vertices || !indices || indices % 3 != 0 || vertices > maximum / sizeof(VoxelVertex) ||
        indices > maximum / sizeof(std::uint32_t))
        return false;
    const auto vertex_bytes = vertices * sizeof(VoxelVertex);
    const auto index_bytes = indices * sizeof(std::uint32_t);
    if (index_bytes > maximum - vertex_bytes || vertex_bytes > std::uint64_t(store_max) ||
        index_bytes > std::uint64_t(store_max) || indices > std::uint64_t(count_max))
        return false;
    sizes = {vertex_bytes, index_bytes, vertex_bytes + index_bytes, std::int32_t(indices)};
    return true;
}

FarVolumeGlAllocation::~FarVolumeGlAllocation() noexcept {
    if (!vao_ && !vbo_ && !ebo_)
        return;
    if (!driver_.ValidContext())
        std::terminate();
    if (vao_)
        driver_.DeleteVertexArray(vao_);
    if (vbo_)
        driver_.DeleteBuffer(vbo_);
    if (ebo_)
        driver_.DeleteBuffer(ebo_);
}

std::unique_ptr<FarVolumeGpuAllocation> UploadFarVolumeGpu(const FarVolumeRenderMesh& mesh,
                                                           FarVolumeGpuDriver& driver) {
    FarVolumeGpuSizes sizes;
    if (mesh.content != World::FarVolumeContent::Mixed ||
        !CheckFarVolumeGpuSizes(mesh.vertices.size(), mesh.indices.size(), sizes))
        return nullptr;
    for (const auto index : mesh.indices)
        if (index >= mesh.vertices.size())
            return nullptr;
    if (!driver.ValidContext() || driver.HasError())
        return nullptr;
    std::uint32_t saved_vao = 0, saved_vbo = 0;
    driver.GetVertexArrayBinding(saved_vao);
    if (driver.HasError())
        return nullptr;
    driver.GetVertexBufferBinding(saved_vbo);
    if (driver.HasError())
        return nullptr;

    auto allocation =
        std::unique_ptr<FarVolumeGlAllocation>(new FarVolumeGlAllocation(driver, sizes));
    RestoreBindings restore(driver, saved_vao, saved_vbo);
    driver.CreateVertexArray(allocation->vao_);
    if (driver.HasError() || !allocation->vao_)
        return nullptr;
    driver.CreateBuffer(allocation->vbo_);
    if (driver.HasError() || !allocation->vbo_)
        return nullptr;
    driver.CreateBuffer(allocation->ebo_);
    if (driver.HasError() || !allocation->ebo_)
        return nullptr;
    driver.BindVertexArray(allocation->vao_);
    if (driver.HasError())
        return nullptr;
    driver.BindBuffer(FarVolumeBufferKind::Vertex, allocation->vbo_);
    if (driver.HasError())
        return nullptr;
    driver.UploadBuffer(
        FarVolumeBufferKind::Vertex, std::intptr_t(sizes.vertex_bytes), mesh.vertices.data());
    if (driver.HasError())
        return nullptr;
    std::int64_t actual_bytes = 0;
    driver.GetBufferBytes(FarVolumeBufferKind::Vertex, actual_bytes);
    if (driver.HasError() || actual_bytes != std::int64_t(sizes.vertex_bytes))
        return nullptr;
    driver.BindBuffer(FarVolumeBufferKind::Index, allocation->ebo_);
    if (driver.HasError())
        return nullptr;
    driver.UploadBuffer(
        FarVolumeBufferKind::Index, std::intptr_t(sizes.index_bytes), mesh.indices.data());
    if (driver.HasError())
        return nullptr;
    driver.GetBufferBytes(FarVolumeBufferKind::Index, actual_bytes);
    if (driver.HasError() || actual_bytes != std::int64_t(sizes.index_bytes))
        return nullptr;
    constexpr FarVolumeVertexAttribute attributes[] = {FarVolumeVertexAttribute::Position,
                                                       FarVolumeVertexAttribute::Normal,
                                                       FarVolumeVertexAttribute::Material};
    constexpr std::size_t offsets[] = {offsetof(VoxelVertex, position),
                                       offsetof(VoxelVertex, normal),
                                       offsetof(VoxelVertex, material_id)};
    for (std::size_t i = 0; i < 3; ++i) {
        driver.SetAttribute(attributes[i], std::int32_t(sizeof(VoxelVertex)), offsets[i]);
        if (driver.HasError())
            return nullptr;
        driver.EnableAttribute(attributes[i]);
        if (driver.HasError())
            return nullptr;
    }
    if (!restore.Restore())
        return nullptr;
    return allocation;
}
bool DrawFarVolumeGpu(const FarVolumeGpuAllocation& resource, FarVolumeGpuDriver& driver) noexcept {
    const auto* allocation = dynamic_cast<const FarVolumeGlAllocation*>(&resource);
    if (!allocation || !allocation->UsesDriver(driver) || !allocation->native_vao() ||
        allocation->index_count() <= 0 || !driver.ValidContext() || driver.HasError())
        return false;
    std::uint32_t saved_vao = 0;
    driver.GetVertexArrayBinding(saved_vao);
    if (driver.HasError())
        return false;
    driver.BindVertexArray(allocation->native_vao());
    bool success = !driver.HasError();
    if (success) {
        success = driver.DrawIndexed(allocation->index_count());
        const bool clean = !driver.HasError();
        success = success && clean;
    }
    driver.BindVertexArray(saved_vao);
    const bool restored = !driver.HasError();
    return success && restored;
}

} // namespace Luminumbra::Rendering
