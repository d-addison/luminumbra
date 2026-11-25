#include "rendering/Shader.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <new>
#include <string>
#include <string_view>

#if defined(_WIN32)
#include <malloc.h>
#endif

namespace {
struct AllocationState {
    bool enabled = false;
    std::size_t calls = 0;
};

AllocationState& Allocations() noexcept {
    static thread_local AllocationState state;
    return state;
}

void RecordAllocation() noexcept {
    auto& state = Allocations();
    if (state.enabled) {
        ++state.calls;
    }
}

class AllocationScope {
public:
    AllocationScope() {
        Allocations() = {true, 0};
    }
    ~AllocationScope() {
        Allocations().enabled = false;
    }
    [[nodiscard]] std::size_t calls() const {
        return Allocations().calls;
    }
};

void FreeAligned(void* storage) noexcept {
#if defined(_WIN32)
    ::_aligned_free(storage);
#else
    std::free(storage);
#endif
}
} // namespace

void* operator new(std::size_t size) {
    if (void* storage = std::malloc(size == 0 ? 1 : size)) {
        RecordAllocation();
        return storage;
    }
    throw std::bad_alloc();
}
void* operator new[](std::size_t size) {
    return ::operator new(size);
}
void operator delete(void* storage) noexcept {
    std::free(storage);
}
void operator delete[](void* storage) noexcept {
    ::operator delete(storage);
}
void operator delete(void* storage, std::size_t) noexcept {
    ::operator delete(storage);
}
void operator delete[](void* storage, std::size_t) noexcept {
    ::operator delete(storage);
}
void* operator new(std::size_t size, std::align_val_t alignment) {
    void* storage = nullptr;
#if defined(_WIN32)
    storage = ::_aligned_malloc(size == 0 ? 1 : size, static_cast<std::size_t>(alignment));
#else
    if (::posix_memalign(&storage, static_cast<std::size_t>(alignment), size == 0 ? 1 : size) !=
        0) {
        storage = nullptr;
    }
#endif
    if (storage == nullptr) {
        throw std::bad_alloc();
    }
    RecordAllocation();
    return storage;
}
void* operator new[](std::size_t size, std::align_val_t alignment) {
    return ::operator new(size, alignment);
}
void operator delete(void* storage, std::align_val_t) noexcept {
    FreeAligned(storage);
}
void operator delete[](void* storage, std::align_val_t alignment) noexcept {
    ::operator delete(storage, alignment);
}
void operator delete(void* storage, std::size_t, std::align_val_t alignment) noexcept {
    ::operator delete(storage, alignment);
}
void operator delete[](void* storage, std::size_t, std::align_val_t alignment) noexcept {
    ::operator delete(storage, alignment);
}
// The standard library also allocates through the nothrow forms (for example
// std::stable_sort's temporary buffer), so they must share the replaced heap.
void* operator new(std::size_t size, const std::nothrow_t&) noexcept {
    try {
        return ::operator new(size);
    } catch (...) {
        return nullptr;
    }
}
void* operator new[](std::size_t size, const std::nothrow_t&) noexcept {
    return ::operator new(size, std::nothrow);
}
void operator delete(void* storage, const std::nothrow_t&) noexcept {
    ::operator delete(storage);
}
void operator delete[](void* storage, const std::nothrow_t&) noexcept {
    ::operator delete(storage);
}
void* operator new(std::size_t size, std::align_val_t alignment, const std::nothrow_t&) noexcept {
    try {
        return ::operator new(size, alignment);
    } catch (...) {
        return nullptr;
    }
}
void* operator new[](std::size_t size, std::align_val_t alignment, const std::nothrow_t&) noexcept {
    return ::operator new(size, alignment, std::nothrow);
}
void operator delete(void* storage, std::align_val_t alignment, const std::nothrow_t&) noexcept {
    ::operator delete(storage, alignment);
}
void operator delete[](void* storage, std::align_val_t alignment, const std::nothrow_t&) noexcept {
    ::operator delete(storage, alignment);
}

namespace {
using Luminumbra::Rendering::Shader;

struct UniformCall {
    int kind = 0;
    GLint location = 0;
    GLsizei count = 1;
    GLboolean transpose = GL_FALSE;
    GLint integer = 0;
    // The largest payload is one 4x4 matrix (16 floats).
    std::array<GLfloat, 16> values{};
    bool operator==(const UniformCall&) const = default;
};

struct GlState {
    // One trace holds exactly one call from each of the twelve setter overloads.
    std::array<UniformCall, 12> writes{};
    std::size_t write_count = 0;
    std::size_t query_count = 0;
    std::string queried_name;
    GLuint queried_program = 0;
    GLuint next_program = 40;
};

GlState& State() {
    static GlState state;
    return state;
}

void Record(int kind,
            GLint location,
            const GLfloat* values,
            std::size_t width,
            GLsizei count = 1,
            GLboolean transpose = GL_FALSE,
            GLint integer = 0) {
    auto& state = State();
    if (state.write_count < state.writes.size()) {
        auto& call = state.writes[state.write_count];
        call = {.kind = kind,
                .location = location,
                .count = count,
                .transpose = transpose,
                .integer = integer,
                .values = {}};
        std::copy_n(values, width, call.values.begin());
    }
    ++state.write_count;
}

GLint APIENTRY Lookup(GLuint program, const GLchar* name) {
    auto& state = State();
    ++state.query_count;
    state.queried_program = program;
    state.queried_name = name;
    if (state.queried_name.starts_with("u_missing")) {
        return -1;
    }
    return state.queried_name.empty() ? 0 : 37;
}

void APIENTRY Uniform1i(GLint location, GLint value) {
    Record(1, location, nullptr, 0, 1, GL_FALSE, value);
}
void APIENTRY Uniform1f(GLint location, GLfloat value) {
    Record(2, location, &value, 1);
}
void APIENTRY Uniform2f(GLint location, GLfloat x, GLfloat y) {
    const std::array values{x, y};
    Record(3, location, values.data(), values.size());
}
void APIENTRY Uniform3f(GLint location, GLfloat x, GLfloat y, GLfloat z) {
    const std::array values{x, y, z};
    Record(4, location, values.data(), values.size());
}
void APIENTRY Uniform4f(GLint location, GLfloat x, GLfloat y, GLfloat z, GLfloat w) {
    const std::array values{x, y, z, w};
    Record(5, location, values.data(), values.size());
}
void APIENTRY Uniform2fv(GLint location, GLsizei count, const GLfloat* values) {
    Record(6, location, values, 2, count);
}
void APIENTRY UniformMatrix2fv(GLint location,
                               GLsizei count,
                               GLboolean transpose,
                               const GLfloat* values) {
    Record(9, location, values, 4, count, transpose);
}
void APIENTRY Uniform3fv(GLint location, GLsizei count, const GLfloat* values) {
    Record(7, location, values, 3, count);
}
void APIENTRY UniformMatrix3fv(GLint location,
                               GLsizei count,
                               GLboolean transpose,
                               const GLfloat* values) {
    Record(10, location, values, 9, count, transpose);
}
void APIENTRY Uniform4fv(GLint location, GLsizei count, const GLfloat* values) {
    Record(8, location, values, 4, count);
}
void APIENTRY UniformMatrix4fv(GLint location,
                               GLsizei count,
                               GLboolean transpose,
                               const GLfloat* values) {
    Record(11, location, values, 16, count, transpose);
}

GLuint APIENTRY CreateShader(GLenum) {
    return 1;
}
GLuint APIENTRY CreateProgram() {
    return ++State().next_program;
}
void APIENTRY ShaderSource(GLuint, GLsizei, const GLchar* const*, const GLint*) {}
void APIENTRY ShaderOperation(GLuint) {}
void APIENTRY AttachShader(GLuint, GLuint) {}
void APIENTRY Status(GLuint, GLenum, GLint* result) {
    *result = GL_TRUE;
}
void APIENTRY Interface(GLuint, GLenum, GLenum, GLint* result) {
    *result = 0;
}

template<typename Function>
class GlOverride {
public:
    GlOverride(Function& entry, Function replacement)
        : m_entry(entry)
        , m_original(entry) {
        m_entry = replacement;
    }
    ~GlOverride() {
        m_entry = m_original;
    }
    GlOverride(const GlOverride&) = delete;
    GlOverride& operator=(const GlOverride&) = delete;

private:
    Function& m_entry;
    Function m_original;
};

class ShaderUniformLookup : public ::testing::Test {
protected:
    void SetUp() override {
        State() = {};
    }
    static void ResetWrites() {
        State().writes = {};
        State().write_count = 0;
    }

private:
    const GlOverride<decltype(glad_glCreateShader)> m_CreateShader{glad_glCreateShader,
                                                                   CreateShader};
    const GlOverride<decltype(glad_glCreateProgram)> m_CreateProgram{glad_glCreateProgram,
                                                                     CreateProgram};
    const GlOverride<decltype(glad_glShaderSource)> m_ShaderSource{glad_glShaderSource,
                                                                   ShaderSource};
    const GlOverride<decltype(glad_glCompileShader)> m_CompileShader{glad_glCompileShader,
                                                                     ShaderOperation};
    const GlOverride<decltype(glad_glLinkProgram)> m_LinkProgram{glad_glLinkProgram,
                                                                 ShaderOperation};
    const GlOverride<decltype(glad_glDeleteShader)> m_DeleteShader{glad_glDeleteShader,
                                                                   ShaderOperation};
    const GlOverride<decltype(glad_glDeleteProgram)> m_DeleteProgram{glad_glDeleteProgram,
                                                                     ShaderOperation};
    const GlOverride<decltype(glad_glAttachShader)> m_AttachShader{glad_glAttachShader,
                                                                   AttachShader};
    const GlOverride<decltype(glad_glGetShaderiv)> m_GetShaderiv{glad_glGetShaderiv, Status};
    const GlOverride<decltype(glad_glGetProgramiv)> m_GetProgramiv{glad_glGetProgramiv, Status};
    const GlOverride<decltype(glad_glGetProgramInterfaceiv)> m_GetProgramInterfaceiv{
        glad_glGetProgramInterfaceiv, Interface};
    const GlOverride<decltype(glad_glGetUniformLocation)> m_GetUniformLocation{
        glad_glGetUniformLocation, Lookup};
    const GlOverride<decltype(glad_glUniform1i)> m_Uniform1i{glad_glUniform1i, Uniform1i};
    const GlOverride<decltype(glad_glUniform1f)> m_Uniform1f{glad_glUniform1f, Uniform1f};
    const GlOverride<decltype(glad_glUniform2f)> m_Uniform2f{glad_glUniform2f, Uniform2f};
    const GlOverride<decltype(glad_glUniform3f)> m_Uniform3f{glad_glUniform3f, Uniform3f};
    const GlOverride<decltype(glad_glUniform4f)> m_Uniform4f{glad_glUniform4f, Uniform4f};
    const GlOverride<decltype(glad_glUniform2fv)> m_Uniform2fv{glad_glUniform2fv, Uniform2fv};
    const GlOverride<decltype(glad_glUniformMatrix2fv)> m_UniformMatrix2fv{glad_glUniformMatrix2fv,
                                                                           UniformMatrix2fv};
    const GlOverride<decltype(glad_glUniform3fv)> m_Uniform3fv{glad_glUniform3fv, Uniform3fv};
    const GlOverride<decltype(glad_glUniformMatrix3fv)> m_UniformMatrix3fv{glad_glUniformMatrix3fv,
                                                                           UniformMatrix3fv};
    const GlOverride<decltype(glad_glUniform4fv)> m_Uniform4fv{glad_glUniform4fv, Uniform4fv};
    const GlOverride<decltype(glad_glUniformMatrix4fv)> m_UniformMatrix4fv{glad_glUniformMatrix4fv,
                                                                           UniformMatrix4fv};
};

template<typename Name>
void SetAll(const Shader& shader, const Name& name) {
    shader.setBool(name, true);
    shader.setInt(name, -7);
    shader.setFloat(name, 0.25f);
    shader.setVec2(name, glm::vec2(1, 2));
    shader.setVec2(name, 3, 4);
    shader.setVec3(name, glm::vec3(5, 6, 7));
    shader.setVec3(name, 8, 9, 10);
    shader.setVec4(name, glm::vec4(11, 12, 13, 14));
    shader.setVec4(name, 15, 16, 17, 18);
    const glm::mat4 matrix(1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16);
    shader.setMat2(name, glm::mat2(matrix));
    shader.setMat3(name, glm::mat3(matrix));
    shader.setMat4(name, matrix);
}

void ReferenceCalls(GLint location) {
    const std::array<GLfloat, 4> vector{11, 12, 13, 14};
    const std::array<GLfloat, 4> matrix2{1, 2, 5, 6};
    const std::array<GLfloat, 9> matrix3{1, 2, 3, 5, 6, 7, 9, 10, 11};
    const std::array<GLfloat, 16> matrix4{1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};
    const std::array<GLfloat, 2> vector2{1, 2};
    const std::array<GLfloat, 3> vector3{5, 6, 7};
    glUniform1i(location, 1);
    glUniform1i(location, -7);
    glUniform1f(location, 0.25f);
    glUniform2fv(location, 1, vector2.data());
    glUniform2f(location, 3, 4);
    glUniform3fv(location, 1, vector3.data());
    glUniform3f(location, 8, 9, 10);
    glUniform4fv(location, 1, vector.data());
    glUniform4f(location, 15, 16, 17, 18);
    glUniformMatrix2fv(location, 1, GL_FALSE, matrix2.data());
    glUniformMatrix3fv(location, 1, GL_FALSE, matrix3.data());
    glUniformMatrix4fv(location, 1, GL_FALSE, matrix4.data());
}

TEST_F(ShaderUniformLookup, WarmedLiteralSettersAllocateNothingIncludingMissingUniforms) {
    Shader shader(LUMINUMBRA_SOURCE_ROOT "/res/shaders/water.vert",
                  LUMINUMBRA_SOURCE_ROOT "/res/shaders/water.frag");
    ASSERT_TRUE(shader.IsValid());
    // Both literals exceed every supported standard library's small-string capacity.
    SetAll(shader,
           "u_present_uniform_name_longer_than_small_string_storage_capacity_for_all_setters");
    ResetWrites();
    SetAll(shader,
           "u_missing_uniform_name_longer_than_small_string_storage_capacity_for_all_setters");
    ASSERT_EQ(State().query_count, 2u);
    std::size_t allocations = 0;
    {
        const AllocationScope scope;
        // A fixed 64 batches measure both positive and negative cache hits.
        for (int batch = 0; batch < 64; ++batch) {
            ResetWrites();
            SetAll(
                shader,
                "u_present_uniform_name_longer_than_small_string_storage_capacity_for_all_setters");
            ResetWrites();
            SetAll(
                shader,
                "u_missing_uniform_name_longer_than_small_string_storage_capacity_for_all_setters");
        }
        allocations = scope.calls();
    }
    EXPECT_EQ(allocations, 0u);
    EXPECT_EQ(State().query_count, 2u);
    EXPECT_EQ(State().write_count, 12u);
}

TEST_F(ShaderUniformLookup, EverySetterMatchesReferenceLocationsAndPayloads) {
    Shader shader(LUMINUMBRA_SOURCE_ROOT "/res/shaders/water.vert",
                  LUMINUMBRA_SOURCE_ROOT "/res/shaders/water.frag");
    ASSERT_TRUE(shader.IsValid());
    for (const auto* name : {"u_present_uniform_name_longer_than_small_string_storage_capacity",
                             "u_missing_uniform_name_longer_than_small_string_storage_capacity"}) {
        const GLint location = std::string_view(name).starts_with("u_missing") ? -1 : 37;
        ResetWrites();
        ReferenceCalls(location);
        const auto expected = State().writes;
        const auto queries = State().query_count;
        for (int call = 0; call < 2; ++call) {
            ResetWrites();
            SetAll(shader, name);
            EXPECT_EQ(State().write_count, expected.size());
            EXPECT_EQ(State().writes, expected);
            EXPECT_EQ(State().query_count, queries + 1);
            EXPECT_EQ(State().queried_program, shader.Id());
            EXPECT_EQ(State().queried_name, name);
        }
    }
}

TEST_F(ShaderUniformLookup, ViewsKeepExactOwnedKeysAndReloadInvalidatesCachedLocations) {
    Shader shader(LUMINUMBRA_SOURCE_ROOT "/res/shaders/water.vert",
                  LUMINUMBRA_SOURCE_ROOT "/res/shaders/water.frag");
    ASSERT_TRUE(shader.IsValid());
    std::string storage = "u_owned_uniform_name_followed_by_unrelated_bytes";
    const auto length = std::string_view("u_owned_uniform_name").size();
    shader.setInt(std::string_view(storage.data(), length), 1);
    EXPECT_EQ(State().queried_name, "u_owned_uniform_name");
    storage[0] = 'v';
    shader.setInt("u_owned_uniform_name", 2);
    EXPECT_EQ(State().query_count, 1u);
    shader.setInt(std::string_view{}, 3);
    EXPECT_TRUE(State().queried_name.empty());
    EXPECT_EQ(State().writes[2].location, 0);
    const std::string embedded("u_owned_uniform_name\0suffix", 27);
    shader.setInt(embedded, 4);
    EXPECT_EQ(State().queried_name, "u_owned_uniform_name");
    shader.setInt(embedded, 5);
    EXPECT_EQ(State().query_count, 3u);
    shader.setInt("u_missing_uniform_name", 6);
    shader.setInt("u_missing_uniform_name", 7);
    EXPECT_EQ(State().query_count, 4u);
    EXPECT_EQ(State().writes[6].location, -1);
    const auto generation = shader.ProgramGeneration();
    ASSERT_TRUE(shader.Reload());
    EXPECT_EQ(shader.ProgramGeneration(), generation + 1);
    ResetWrites();
    shader.setInt("u_owned_uniform_name", 8);
    shader.setInt("u_missing_uniform_name", 9);
    EXPECT_EQ(State().query_count, 6u);
    EXPECT_EQ(State().queried_program, shader.Id());
    EXPECT_EQ(State().writes[0].location, 37);
    EXPECT_EQ(State().writes[1].location, -1);
}
} // namespace
