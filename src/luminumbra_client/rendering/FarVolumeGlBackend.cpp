#include "FarVolumeGlBackend.h"

#include <glad/glad.h>
#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

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
