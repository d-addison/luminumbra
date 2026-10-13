#pragma once

#include "FarVolumeGpuBackend.h"

#include <thread>

struct GLFWwindow;

namespace Luminumbra::Rendering {

// Construct on the owner thread with the exact context current, after glad was
// loaded by the client (OpenGL 4.5 minimum). This adapter never loads functions.
// Context, function table and this driver must outlive all its allocations.
class FarVolumeGlDriver final : public FarVolumeGpuDriver {
public:
    FarVolumeGlDriver() noexcept;
    FarVolumeGlDriver(const FarVolumeGlDriver&) = delete;
    FarVolumeGlDriver& operator=(const FarVolumeGlDriver&) = delete;
    FarVolumeGlDriver(FarVolumeGlDriver&&) = delete;
    FarVolumeGlDriver& operator=(FarVolumeGlDriver&&) = delete;
    bool ValidContext() const noexcept override;
    bool HasError() noexcept override;
    void GetVertexArrayBinding(std::uint32_t& name) noexcept override;
    void GetVertexBufferBinding(std::uint32_t& name) noexcept override;
    void CreateVertexArray(std::uint32_t& name) noexcept override;
    void CreateBuffer(std::uint32_t& name) noexcept override;
    void DeleteVertexArray(std::uint32_t name) noexcept override;
    void DeleteBuffer(std::uint32_t name) noexcept override;
    void BindVertexArray(std::uint32_t name) noexcept override;
    void BindBuffer(FarVolumeBufferKind kind, std::uint32_t name) noexcept override;
    void
    UploadBuffer(FarVolumeBufferKind kind, std::intptr_t bytes, const void* data) noexcept override;
    void GetBufferBytes(FarVolumeBufferKind kind, std::int64_t& bytes) noexcept override;
    void SetAttribute(FarVolumeVertexAttribute attribute,
                      std::int32_t stride,
                      std::size_t offset) noexcept override;
    void EnableAttribute(FarVolumeVertexAttribute attribute) noexcept override;

private:
    GLFWwindow* const context_;
    const std::thread::id owner_;
};

} // namespace Luminumbra::Rendering
