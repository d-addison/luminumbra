#include "FarVolumeGlBackend.h"

#include <glad/glad.h>
#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

#include <exception>
#include <limits>
#include <type_traits>

namespace Luminumbra::Rendering {
namespace {
static_assert(std::is_same_v<GLuint, std::uint32_t>);
static_assert(std::numeric_limits<GLsizeiptr>::max() == std::numeric_limits<std::intptr_t>::max());
static_assert(std::numeric_limits<GLsizei>::max() == std::numeric_limits<std::int32_t>::max());
static_assert(std::numeric_limits<GLint64>::max() == std::numeric_limits<std::int64_t>::max());

GLenum Target(FarVolumeBufferKind kind) noexcept {
    return kind == FarVolumeBufferKind::Vertex ? GL_ARRAY_BUFFER : GL_ELEMENT_ARRAY_BUFFER;
}
bool FunctionsLoaded() noexcept {
    return glGetError && glGetIntegerv && glGenVertexArrays && glGenBuffers &&
           glDeleteVertexArrays && glDeleteBuffers && glBindVertexArray && glBindBuffer &&
           glBufferData && glGetBufferParameteri64v && glVertexAttribPointer &&
           glVertexAttribIPointer && glEnableVertexAttribArray && glDrawElements;
}
} // namespace

FarVolumeGlDriver::FarVolumeGlDriver() noexcept
    : context_(glfwGetCurrentContext())
    , owner_(std::this_thread::get_id()) {}

FarVolumeGlDriver::~FarVolumeGlDriver() noexcept {
    ReleaseLiveOwnership();
}
bool FarVolumeGlDriver::BindLiveOwnership(std::span<const FarWorldLiveCell> cells) noexcept {
    if (!ValidContext() || ownership_bound_ || cells.size() > kFarWorldLiveCellLimit ||
        !glCreateBuffers || !glNamedBufferData || !glNamedBufferSubData ||
        !glGetNamedBufferParameteri64v || !glGetIntegeri_v || !glGetInteger64i_v ||
        !glBindBufferBase || !glBindBufferRange || HasError())
        return false;
    if (!ownership_buffer_) {
        glCreateBuffers(1, &ownership_buffer_);
        if (!ownership_buffer_ || HasError()) {
            ReleaseLiveOwnership();
            return false;
        }
        glNamedBufferData(ownership_buffer_,
                          static_cast<GLsizeiptr>(kFarWorldOwnershipGpuBytes),
                          nullptr,
                          GL_DYNAMIC_DRAW);
        GLint64 actual = 0;
        glGetNamedBufferParameteri64v(ownership_buffer_, GL_BUFFER_SIZE, &actual);
        if (HasError() || actual != static_cast<GLint64>(kFarWorldOwnershipGpuBytes)) {
            ReleaseLiveOwnership();
            return false;
        }
    }
    if (!cells.empty())
        glNamedBufferSubData(
            ownership_buffer_, 0, static_cast<GLsizeiptr>(cells.size_bytes()), cells.data());
    if (HasError())
        return false;
    // Binding 7 is reserved for this synchronous terrain submission. Preserve
    // the indexed range and generic target independently on every exit path.
    glGetIntegerv(GL_SHADER_STORAGE_BUFFER_BINDING, &saved_ssbo_);
    glGetIntegeri_v(GL_SHADER_STORAGE_BUFFER_BINDING, 7, &saved_indexed_ssbo_);
    GLint64 start = 0, size = 0;
    glGetInteger64i_v(GL_SHADER_STORAGE_BUFFER_START, 7, &start);
    glGetInteger64i_v(GL_SHADER_STORAGE_BUFFER_SIZE, 7, &size);
    saved_ssbo_start_ = static_cast<std::int64_t>(start);
    saved_ssbo_size_ = static_cast<std::int64_t>(size);
    if (HasError())
        return false;
    saved_clip_enabled_ = glIsEnabled(GL_CLIP_DISTANCE0) == GL_TRUE;
    ownership_bound_ = true;
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 7, ownership_buffer_);
    glEnable(GL_CLIP_DISTANCE0);
    if (!HasError())
        return true;
    UnbindLiveOwnership();
    return false;
}
void FarVolumeGlDriver::UnbindLiveOwnership() noexcept {
    if (!ownership_bound_)
        return;
    if (!ValidContext())
        std::terminate();
    if (saved_indexed_ssbo_ && saved_ssbo_size_ > 0)
        glBindBufferRange(GL_SHADER_STORAGE_BUFFER,
                          7,
                          static_cast<GLuint>(saved_indexed_ssbo_),
                          static_cast<GLintptr>(saved_ssbo_start_),
                          static_cast<GLsizeiptr>(saved_ssbo_size_));
    else
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 7, static_cast<GLuint>(saved_indexed_ssbo_));
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, static_cast<GLuint>(saved_ssbo_));
    if (!saved_clip_enabled_)
        glDisable(GL_CLIP_DISTANCE0);
    ownership_bound_ = false;
}
void FarVolumeGlDriver::ReleaseLiveOwnership() noexcept {
    UnbindLiveOwnership();
    if (!ownership_buffer_)
        return;
    if (!ValidContext())
        std::terminate();
    glDeleteBuffers(1, &ownership_buffer_);
    ownership_buffer_ = 0;
}

bool FarVolumeGlDriver::ValidContext() const noexcept {
    return context_ && std::this_thread::get_id() == owner_ &&
           glfwGetCurrentContext() == context_ && FunctionsLoaded();
}
bool FarVolumeGlDriver::HasError() noexcept {
    return glGetError() != GL_NO_ERROR;
}
void FarVolumeGlDriver::GetVertexArrayBinding(std::uint32_t& name) noexcept {
    GLint binding = 0;
    glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &binding);
    name = static_cast<std::uint32_t>(binding);
}
void FarVolumeGlDriver::GetVertexBufferBinding(std::uint32_t& name) noexcept {
    GLint binding = 0;
    glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &binding);
    name = static_cast<std::uint32_t>(binding);
}
void FarVolumeGlDriver::CreateVertexArray(std::uint32_t& name) noexcept {
    glGenVertexArrays(1, &name);
}
void FarVolumeGlDriver::CreateBuffer(std::uint32_t& name) noexcept {
    glGenBuffers(1, &name);
}
void FarVolumeGlDriver::DeleteVertexArray(std::uint32_t name) noexcept {
    glDeleteVertexArrays(1, &name);
}
void FarVolumeGlDriver::DeleteBuffer(std::uint32_t name) noexcept {
    glDeleteBuffers(1, &name);
}
void FarVolumeGlDriver::BindVertexArray(std::uint32_t name) noexcept {
    glBindVertexArray(name);
}
void FarVolumeGlDriver::BindBuffer(FarVolumeBufferKind kind, std::uint32_t name) noexcept {
    glBindBuffer(Target(kind), name);
}
void FarVolumeGlDriver::UploadBuffer(FarVolumeBufferKind kind,
                                     std::intptr_t bytes,
                                     const void* data) noexcept {
    glBufferData(Target(kind), static_cast<GLsizeiptr>(bytes), data, GL_STATIC_DRAW);
}
void FarVolumeGlDriver::GetBufferBytes(FarVolumeBufferKind kind, std::int64_t& bytes) noexcept {
    GLint64 actual = 0;
    glGetBufferParameteri64v(Target(kind), GL_BUFFER_SIZE, &actual);
    bytes = static_cast<std::int64_t>(actual);
}
void FarVolumeGlDriver::SetAttribute(FarVolumeVertexAttribute attribute,
                                     std::int32_t stride,
                                     std::size_t offset) noexcept {
    const auto location = static_cast<GLuint>(attribute);
    const auto* pointer = reinterpret_cast<const void*>(offset);
    if (attribute == FarVolumeVertexAttribute::Material)
        glVertexAttribIPointer(location, 1, GL_UNSIGNED_INT, static_cast<GLsizei>(stride), pointer);
    else
        glVertexAttribPointer(
            location, 3, GL_FLOAT, GL_FALSE, static_cast<GLsizei>(stride), pointer);
}
void FarVolumeGlDriver::EnableAttribute(FarVolumeVertexAttribute attribute) noexcept {
    glEnableVertexAttribArray(static_cast<GLuint>(attribute));
}
bool FarVolumeGlDriver::DrawIndexed(std::int32_t index_count) noexcept {
    if (index_count <= 0 || !ValidContext())
        return false;
    glDrawElements(GL_TRIANGLES, static_cast<GLsizei>(index_count), GL_UNSIGNED_INT, nullptr);
    return true;
}
} // namespace Luminumbra::Rendering
