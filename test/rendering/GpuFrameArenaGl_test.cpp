#include "luminumbra_client/rendering/GpuFrameArena.h"

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>
#include <gtest/gtest.h>

#include <array>
#include <cstring>
#include <limits>
#include <numeric>
#include <vector>

namespace {
using Luminumbra::Rendering::GpuFrameArena;
using Use = GpuFrameArena::Use;

template<typename Function>
class ScopedGlEntry {
public:
    ScopedGlEntry(Function& entry, Function replacement)
        : m_entry(entry)
        , m_original(entry) {
        m_entry = replacement;
    }
    ~ScopedGlEntry() {
        m_entry = m_original;
    }
    ScopedGlEntry(const ScopedGlEntry&) = delete;
    ScopedGlEntry& operator=(const ScopedGlEntry&) = delete;

private:
    Function& m_entry;
    Function m_original;
};

GLenum APIENTRY BusyFence(GLsync, GLbitfield flags, GLuint64 timeout) {
    EXPECT_EQ(flags, static_cast<GLbitfield>(GL_SYNC_FLUSH_COMMANDS_BIT));
    EXPECT_EQ(timeout, 0u);
    return GL_TIMEOUT_EXPIRED;
}

GLenum APIENTRY FailedFence(GLsync, GLbitfield flags, GLuint64 timeout) {
    EXPECT_EQ(flags, static_cast<GLbitfield>(GL_SYNC_FLUSH_COMMANDS_BIT));
    EXPECT_EQ(timeout, 0u);
    return GL_WAIT_FAILED;
}

GLenum APIENTRY CompletedFence(GLsync, GLbitfield flags, GLuint64 timeout) {
    EXPECT_EQ(flags, static_cast<GLbitfield>(GL_SYNC_FLUSH_COMMANDS_BIT));
    EXPECT_EQ(timeout, 0u);
    // Finish actual commands before authorizing reuse, independent of llvmpipe
    // scheduling. The allocator itself must never call a blocking wait.
    glFinish();
    return GL_CONDITION_SATISFIED;
}

GLsync APIENTRY MissingFence(GLenum, GLbitfield) {
    return nullptr;
}

void* APIENTRY RejectMapping(GLenum, GLintptr, GLsizeiptr, GLbitfield) {
    return nullptr;
}

void* APIENTRY RejectOverflowMapping(GLenum target,
                                     GLintptr offset,
                                     GLsizeiptr length,
                                     GLbitfield access) {
    // The failure fixture has two primary regions and one overflow region,
    // with one requested byte rounded up to the queried binding alignments.
    GLint uniform_alignment = 0;
    GLint storage_alignment = 0;
    glGetIntegerv(GL_UNIFORM_BUFFER_OFFSET_ALIGNMENT, &uniform_alignment);
    glGetIntegerv(GL_SHADER_STORAGE_BUFFER_OFFSET_ALIGNMENT, &storage_alignment);
    const auto stride = std::lcm(std::lcm(static_cast<GLsizeiptr>(uniform_alignment),
                                          static_cast<GLsizeiptr>(storage_alignment)),
                                 GLsizeiptr{4});
    if (length == stride) {
        return nullptr;
    }
    const auto original =
        reinterpret_cast<PFNGLMAPBUFFERRANGEPROC>(glfwGetProcAddress("glMapBufferRange"));
    return original(target, offset, length, access);
}

void APIENTRY RejectBuffer(GLsizei count, GLuint* buffers) {
    for (GLsizei index = 0; index < count; ++index) {
        buffers[index] = 0;
    }
}

void APIENTRY RejectAlignment(GLenum, GLint* value) {
    *value = 0;
}

void APIENTRY UnexpectedFinish() {
    ADD_FAILURE() << "The arena must not finish GPU work";
}

class GpuFrameArenaGl : public ::testing::Test {
protected:
    void SetUp() override {
        m_initialized = glfwInit() == GLFW_TRUE;
        ASSERT_TRUE(m_initialized);
        glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
        glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 4);
        glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 5);
        glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
        m_window = glfwCreateWindow(32, 32, "gpu_frame_arena", nullptr, nullptr);
        ASSERT_NE(m_window, nullptr);
        glfwMakeContextCurrent(m_window);
        m_loaded = gladLoadGLLoader(reinterpret_cast<GLADloadproc>(glfwGetProcAddress)) != 0;
        ASSERT_TRUE(m_loaded);
    }

    void TearDown() override {
        if (m_loaded) {
            EXPECT_EQ(glGetError(), static_cast<GLenum>(GL_NO_ERROR));
        }
        if (m_window != nullptr) {
            glfwDestroyWindow(m_window);
        }
        if (m_initialized) {
            glfwTerminate();
        }
    }

    static void Disjoint(const GpuFrameArena::Allocation& allocation,
                         const std::vector<GpuFrameArena::Allocation>& live) {
        for (const auto& previous : live) {
            if (previous.buffer == allocation.buffer) {
                EXPECT_TRUE(previous.offset + previous.bytes <= allocation.offset ||
                            allocation.offset + allocation.bytes <= previous.offset);
            }
        }
    }

    static void CopyMatches(const GpuFrameArena::Allocation& allocation) {
        const std::array<unsigned char, 13> expected{3, 1, 4, 1, 5, 9, 2, 6, 5, 3, 5, 8, 9};
        ASSERT_EQ(allocation.bytes, expected.size());
        std::memcpy(allocation.data, expected.data(), expected.size());
        GLuint destination = 0;
        glGenBuffers(1, &destination);
        glBindBuffer(GL_COPY_WRITE_BUFFER, destination);
        glBufferData(GL_COPY_WRITE_BUFFER,
                     static_cast<GLsizeiptr>(expected.size()),
                     nullptr,
                     GL_STREAM_READ);
        glBindBuffer(GL_COPY_READ_BUFFER, allocation.buffer);
        glCopyBufferSubData(GL_COPY_READ_BUFFER,
                            GL_COPY_WRITE_BUFFER,
                            static_cast<GLintptr>(allocation.offset),
                            0,
                            static_cast<GLsizeiptr>(expected.size()));
        std::array<unsigned char, 13> actual{};
        glGetBufferSubData(
            GL_COPY_WRITE_BUFFER, 0, static_cast<GLsizeiptr>(actual.size()), actual.data());
        EXPECT_EQ(actual, expected);
        glDeleteBuffers(1, &destination);
        glBindBuffer(GL_COPY_READ_BUFFER, 0);
    }

    static void FencePressure(PFNGLCLIENTWAITSYNCPROC poll) {
        GpuFrameArena arena;
        ASSERT_TRUE(
            arena.initialize({.region_bytes = 1024, .frame_regions = 1, .overflow_regions = 1}));
        ASSERT_TRUE(arena.begin_frame());
        const auto primary = arena.allocate(13, Use::Vertex);
        ASSERT_TRUE(primary);
        ASSERT_TRUE(arena.end_frame());
        const ScopedGlEntry<PFNGLCLIENTWAITSYNCPROC> busy(glad_glClientWaitSync, poll);
        const ScopedGlEntry<PFNGLFINISHPROC> no_finish(glad_glFinish, UnexpectedFinish);
        ASSERT_TRUE(arena.begin_frame());
        const auto overflow = arena.allocate(13, Use::Vertex);
        ASSERT_TRUE(overflow);
        EXPECT_TRUE(overflow.fallback);
        EXPECT_NE(overflow.buffer, primary.buffer);
        CopyMatches(overflow);
        EXPECT_EQ(arena.diagnostics().fallbacks, 1u);
        EXPECT_EQ(arena.diagnostics().fence_polls, 1u);
        EXPECT_EQ(arena.diagnostics().fence_waits, 0u);
        ASSERT_TRUE(arena.end_frame());
        ASSERT_TRUE(arena.begin_frame());
        EXPECT_FALSE(arena.allocate(13, Use::Vertex));
        EXPECT_FALSE(arena.allocate(13, Use::Vertex));
        EXPECT_EQ(arena.diagnostics().fence_polls, 2u);
        EXPECT_EQ(arena.diagnostics().refusals, 2u);
        EXPECT_EQ(arena.diagnostics().fence_waits, 0u);
        EXPECT_EQ(arena.storage_bytes(), 2u * arena.region_bytes());
        ASSERT_TRUE(arena.end_frame());
    }

private:
    GLFWwindow* m_window = nullptr;
    bool m_initialized = false;
    bool m_loaded = false;
};

TEST_F(GpuFrameArenaGl, SixFramesKeepAllUnsignalledRangesDisjointAndAligned) {
    GpuFrameArena arena;
    ASSERT_TRUE(arena.initialize());
    GLint uniform_alignment = 0;
    GLint storage_alignment = 0;
    glGetIntegerv(GL_UNIFORM_BUFFER_OFFSET_ALIGNMENT, &uniform_alignment);
    glGetIntegerv(GL_SHADER_STORAGE_BUFFER_OFFSET_ALIGNMENT, &storage_alignment);
    EXPECT_EQ(arena.alignment(Use::Uniform), static_cast<std::size_t>(uniform_alignment));
    EXPECT_EQ(arena.alignment(Use::Storage), static_cast<std::size_t>(storage_alignment));
    const ScopedGlEntry<PFNGLCLIENTWAITSYNCPROC> busy(glad_glClientWaitSync, BusyFence);
    // Bounded fixture: 6 frames * 16 allocations, kept live by expired polls.
    std::vector<GpuFrameArena::Allocation> live;
    live.reserve(96);
    constexpr std::array uses{Use::Uniform, Use::Storage, Use::Vertex, Use::Indirect};
    for (std::size_t frame = 0; frame < 6; ++frame) {
        ASSERT_TRUE(arena.begin_frame());
        for (std::size_t index = 0; index < 16; ++index) {
            const auto use = uses[index % uses.size()];
            const auto allocation = arena.allocate(13, use, 12);
            ASSERT_TRUE(allocation);
            EXPECT_EQ(allocation.offset % arena.alignment(use), 0u);
            EXPECT_EQ(allocation.offset % 12u, 0u);
            EXPECT_EQ(allocation.fallback, frame >= 3);
            Disjoint(allocation, live);
            CopyMatches(allocation);
            live.push_back(allocation);
        }
        EXPECT_EQ(arena.diagnostics().bytes, 16u * 13u);
        EXPECT_EQ(arena.diagnostics().allocations, 16u);
        EXPECT_EQ(arena.diagnostics().fallbacks, frame >= 3 ? 16u : 0u);
        EXPECT_EQ(arena.diagnostics().fence_waits, 0u);
        ASSERT_TRUE(arena.end_frame());
    }
    ASSERT_TRUE(arena.begin_frame());
    EXPECT_FALSE(arena.allocate(13, Use::Uniform));
    EXPECT_EQ(arena.diagnostics().fence_polls, 4u);
    ASSERT_TRUE(arena.end_frame());
}

TEST_F(GpuFrameArenaGl, UnsignalledFenceUsesSeparateBufferWithoutWaiting) {
    FencePressure(BusyFence);
}

TEST_F(GpuFrameArenaGl, FailedPollAlsoQuarantinesStorage) {
    FencePressure(FailedFence);
}

TEST_F(GpuFrameArenaGl, CompletedFenceReusesRegionAndResetsCounters) {
    GpuFrameArena arena;
    ASSERT_TRUE(
        arena.initialize({.region_bytes = 1024, .frame_regions = 1, .overflow_regions = 1}));
    ASSERT_TRUE(arena.begin_frame());
    const auto first = arena.allocate(13, Use::Uniform);
    ASSERT_TRUE(first);
    CopyMatches(first);
    ASSERT_TRUE(arena.end_frame());
    const ScopedGlEntry<PFNGLCLIENTWAITSYNCPROC> complete(glad_glClientWaitSync, CompletedFence);
    ASSERT_TRUE(arena.begin_frame());
    EXPECT_EQ(arena.diagnostics().allocations, 0u);
    const auto second = arena.allocate(13, Use::Uniform);
    ASSERT_TRUE(second);
    EXPECT_EQ(first.buffer, second.buffer);
    EXPECT_EQ(first.offset, second.offset);
    EXPECT_FALSE(second.fallback);
    CopyMatches(second);
    EXPECT_EQ(arena.diagnostics().fence_polls, 1u);
    EXPECT_EQ(arena.diagnostics().fence_waits, 0u);
    ASSERT_TRUE(arena.end_frame());
}

TEST_F(GpuFrameArenaGl, CapacityFallbackIsBoundedAndMissingFencesNeverPermitReuse) {
    GpuFrameArena arena;
    ASSERT_TRUE(
        arena.initialize({.region_bytes = 1024, .frame_regions = 1, .overflow_regions = 1}));
    ASSERT_TRUE(arena.begin_frame());
    const auto primary = arena.allocate(arena.region_bytes(), Use::Vertex);
    const auto overflow = arena.allocate(arena.region_bytes(), Use::Indirect);
    ASSERT_TRUE(primary);
    ASSERT_TRUE(overflow);
    EXPECT_FALSE(primary.fallback);
    EXPECT_TRUE(overflow.fallback);
    EXPECT_NE(primary.buffer, overflow.buffer);
    for (const GLuint buffer : {primary.buffer, overflow.buffer}) {
        glBindBuffer(GL_COPY_WRITE_BUFFER, buffer);
        GLint flags = 0;
        glGetBufferParameteriv(GL_COPY_WRITE_BUFFER, GL_BUFFER_STORAGE_FLAGS, &flags);
        EXPECT_EQ(flags, GL_MAP_WRITE_BIT | GL_MAP_PERSISTENT_BIT | GL_MAP_COHERENT_BIT);
        GLint mapped = 0;
        glGetBufferParameteriv(GL_COPY_WRITE_BUFFER, GL_BUFFER_MAPPED, &mapped);
        EXPECT_EQ(mapped, GL_TRUE);
    }
    EXPECT_FALSE(arena.allocate(1, Use::Vertex));
    EXPECT_EQ(arena.diagnostics().allocations, 2u);
    EXPECT_EQ(arena.diagnostics().fallbacks, 1u);
    EXPECT_EQ(arena.diagnostics().refusals, 1u);
    {
        const ScopedGlEntry<PFNGLFENCESYNCPROC> missing(glad_glFenceSync, MissingFence);
        EXPECT_FALSE(arena.end_frame());
    }
    const ScopedGlEntry<PFNGLCLIENTWAITSYNCPROC> complete(glad_glClientWaitSync, CompletedFence);
    ASSERT_TRUE(arena.begin_frame());
    EXPECT_FALSE(arena.allocate(1, Use::Vertex));
    EXPECT_EQ(arena.diagnostics().fence_polls, 0u);
    ASSERT_TRUE(arena.end_frame());
}

TEST_F(GpuFrameArenaGl, RealFenceCompletionRecyclesPrimaryAndOverflow) {
    GpuFrameArena arena;
    ASSERT_TRUE(
        arena.initialize({.region_bytes = 1024, .frame_regions = 1, .overflow_regions = 1}));
    ASSERT_TRUE(arena.begin_frame());
    const auto primary = arena.allocate(arena.region_bytes(), Use::Vertex);
    const auto overflow = arena.allocate(arena.region_bytes(), Use::Vertex);
    ASSERT_TRUE(primary);
    ASSERT_TRUE(overflow);
    ASSERT_TRUE(arena.end_frame());
    glFinish();
    ASSERT_TRUE(arena.begin_frame());
    const auto reused_primary = arena.allocate(arena.region_bytes(), Use::Vertex);
    const auto reused_overflow = arena.allocate(arena.region_bytes(), Use::Vertex);
    ASSERT_TRUE(reused_primary);
    ASSERT_TRUE(reused_overflow);
    EXPECT_EQ(reused_primary.buffer, primary.buffer);
    EXPECT_EQ(reused_primary.offset, primary.offset);
    EXPECT_EQ(reused_overflow.buffer, overflow.buffer);
    EXPECT_EQ(reused_overflow.offset, overflow.offset);
    EXPECT_EQ(arena.diagnostics().fence_polls, 2u);
    EXPECT_EQ(arena.diagnostics().fence_waits, 0u);
    ASSERT_TRUE(arena.end_frame());
}

TEST_F(GpuFrameArenaGl, InvalidRequestsAndSequencingLeaveExistingBytesUsable) {
    GpuFrameArena arena;
    EXPECT_FALSE(arena.begin_frame());
    EXPECT_FALSE(arena.end_frame());
    EXPECT_FALSE(arena.allocate(1, Use::Vertex));
    ASSERT_TRUE(
        arena.initialize({.region_bytes = 1024, .frame_regions = 1, .overflow_regions = 1}));
    ASSERT_TRUE(arena.begin_frame());
    const auto first = arena.allocate(1, Use::Vertex);
    ASSERT_TRUE(first);
    EXPECT_FALSE(arena.initialize());
    EXPECT_FALSE(arena.begin_frame());
    EXPECT_FALSE(arena.allocate(0, Use::Vertex));
    EXPECT_FALSE(arena.allocate(arena.region_bytes() + 1u, Use::Vertex));
    EXPECT_FALSE(arena.allocate((std::numeric_limits<std::size_t>::max)(), Use::Vertex));
    EXPECT_FALSE(arena.allocate(1, Use::Vertex, 0));
    EXPECT_FALSE(arena.allocate(1, Use::Vertex, (std::numeric_limits<std::size_t>::max)()));
    EXPECT_FALSE(arena.allocate(1, static_cast<Use>(255)));
    const auto second = arena.allocate(13, Use::Vertex);
    ASSERT_TRUE(second);
    EXPECT_EQ(second.offset, 4u);
    EXPECT_EQ(first.buffer, second.buffer);
    CopyMatches(second);
    EXPECT_EQ(arena.diagnostics().allocations, 2u);
    EXPECT_EQ(arena.diagnostics().refusals, 6u);
    ASSERT_TRUE(arena.end_frame());
    EXPECT_FALSE(arena.end_frame());
    EXPECT_FALSE(arena.allocate(1, Use::Vertex));
    arena.destroy();
    arena.destroy();
    EXPECT_EQ(arena.storage_bytes(), 0u);
    ASSERT_TRUE(arena.initialize());
}

TEST_F(GpuFrameArenaGl, ConfigurationBoundsRefuseBeforeAllocatingStorage) {
    GpuFrameArena arena;
    const std::array invalid{
        GpuFrameArena::Config{.region_bytes = 0, .frame_regions = 3, .overflow_regions = 3},
        GpuFrameArena::Config{.region_bytes = 1, .frame_regions = 0, .overflow_regions = 3},
        GpuFrameArena::Config{.region_bytes = 1, .frame_regions = 9, .overflow_regions = 3},
        GpuFrameArena::Config{.region_bytes = 1, .frame_regions = 3, .overflow_regions = 0},
        GpuFrameArena::Config{.region_bytes = 1, .frame_regions = 3, .overflow_regions = 9},
        GpuFrameArena::Config{.region_bytes = (std::numeric_limits<std::size_t>::max)(),
                              .frame_regions = 3,
                              .overflow_regions = 3},
        GpuFrameArena::Config{.region_bytes = GpuFrameArena::kMaxStorageBytes,
                              .frame_regions = 3,
                              .overflow_regions = 3}};
    for (const auto& config : invalid) {
        EXPECT_FALSE(arena.initialize(config));
        EXPECT_EQ(arena.storage_bytes(), 0u);
    }
    ASSERT_TRUE(arena.initialize({.region_bytes = 1, .frame_regions = 8, .overflow_regions = 8}));
    EXPECT_GE(arena.region_bytes(), 1u);
    EXPECT_EQ(arena.storage_bytes(), 16u * arena.region_bytes());
    ASSERT_TRUE(arena.begin_frame());
    ASSERT_TRUE(arena.end_frame());
}

TEST_F(GpuFrameArenaGl, InitializationFailuresReleasePartialStorageAndAllowRetry) {
    GpuFrameArena arena;
    {
        const ScopedGlEntry<PFNGLGETINTEGERVPROC> reject(glad_glGetIntegerv, RejectAlignment);
        EXPECT_FALSE(arena.initialize());
    }
    {
        const ScopedGlEntry<PFNGLGENBUFFERSPROC> reject(glad_glGenBuffers, RejectBuffer);
        EXPECT_FALSE(arena.initialize());
    }
    {
        const ScopedGlEntry<PFNGLMAPBUFFERRANGEPROC> reject(glad_glMapBufferRange, RejectMapping);
        EXPECT_FALSE(arena.initialize());
    }
    {
        const ScopedGlEntry<PFNGLMAPBUFFERRANGEPROC> reject(glad_glMapBufferRange,
                                                            RejectOverflowMapping);
        EXPECT_FALSE(
            arena.initialize({.region_bytes = 1, .frame_regions = 2, .overflow_regions = 1}));
    }
    EXPECT_EQ(arena.storage_bytes(), 0u);
    EXPECT_FALSE(arena.begin_frame());
    ASSERT_TRUE(arena.initialize());
    ASSERT_TRUE(arena.begin_frame());
    ASSERT_TRUE(arena.allocate(13, Use::Storage));
    // Destruction also supports a frame that has not been submitted/sealed.
}
} // namespace
