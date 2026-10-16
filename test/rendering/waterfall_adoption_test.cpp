#include "rendering/passes/ParticlePass.h"
#include "rendering/passes/WaterfallPass.h"

#include <glad/glad.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <stdexcept>
#include <utility>
#include <vector>

namespace {
using Luminumbra::Rendering::ParticlePass;
using Luminumbra::Rendering::WaterfallPass;

template<typename Function>
class ReplaceEntryPoint {
public:
    ReplaceEntryPoint(Function& entry, Function replacement)
        : m_entry(entry)
        , m_original(entry) {
        m_entry = replacement;
    }
    ~ReplaceEntryPoint() {
        m_entry = m_original;
    }
    ReplaceEntryPoint(const ReplaceEntryPoint&) = delete;
    ReplaceEntryPoint& operator=(const ReplaceEntryPoint&) = delete;

private:
    Function& m_entry;
    Function m_original;
};

// Intercept the GL API without creating a context. Retain at most the 288 KiB
// fixture payload so a bad size cannot turn a regression into a large allocation.
class UploadCapture {
public:
    UploadCapture() {
        active = this;
    }
    ~UploadCapture() {
        active = nullptr;
    }
    UploadCapture(const UploadCapture&) = delete;
    UploadCapture& operator=(const UploadCapture&) = delete;
    std::vector<float> received;
    std::vector<std::size_t> writes;
    unsigned allocations = 0;
    std::size_t current_page_base = 0;
    unsigned deleted_buffers = 0;
    unsigned deleted_arrays = 0;

private:
    static inline UploadCapture* active = nullptr;
    static void GLAPIENTRY Gen(GLsizei count, GLuint* names) {
        for (GLsizei index = 0; index < count; ++index)
            names[index] = static_cast<GLuint>(index + 1);
    }
    static void GLAPIENTRY BindArray(GLuint) {}
    static void GLAPIENTRY BindBuffer(GLenum, GLuint) {}
    static void GLAPIENTRY Attribute(GLuint, GLint, GLenum, GLboolean, GLsizei, const void*) {}
    static void GLAPIENTRY Enable(GLuint) {}
    static GLboolean GLAPIENTRY IsObject(GLuint) {
        return GL_FALSE;
    }
    static void GLAPIENTRY Data(GLenum, GLsizeiptr bytes, const void* source, GLenum) {
        ++active->allocations;
        EXPECT_EQ(source, nullptr);
        ASSERT_GE(bytes, 0);
        ASSERT_LE(bytes, static_cast<GLsizeiptr>(64 * 1024));
        active->current_page_base = active->received.size();
        const auto count = static_cast<std::size_t>(bytes) / sizeof(float);
        ASSERT_LE(active->received.size(), 1000u * 72u - count);
        active->received.resize(active->received.size() + count);
    }
    static void GLAPIENTRY SubData(GLenum, GLintptr offset, GLsizeiptr bytes, const void* source) {
        ASSERT_GE(offset, 0);
        ASSERT_GE(bytes, 0);
        const auto first =
            active->current_page_base + static_cast<std::size_t>(offset) / sizeof(float);
        const auto count = static_cast<std::size_t>(bytes) / sizeof(float);
        ASSERT_LE(first, active->received.size());
        ASSERT_LE(count, active->received.size() - first);
        active->writes.push_back(static_cast<std::size_t>(bytes));
        std::copy_n(static_cast<const float*>(source), count, active->received.data() + first);
    }
    static void GLAPIENTRY DeleteBuffer(GLsizei count, const GLuint*) {
        active->deleted_buffers += static_cast<unsigned>(count);
    }
    static void GLAPIENTRY DeleteArray(GLsizei count, const GLuint*) {
        active->deleted_arrays += static_cast<unsigned>(count);
    }
    ReplaceEntryPoint<PFNGLGENVERTEXARRAYSPROC> m_gen_arrays{glad_glGenVertexArrays, Gen};
    ReplaceEntryPoint<PFNGLGENBUFFERSPROC> m_gen_buffers{glad_glGenBuffers, Gen};
    ReplaceEntryPoint<PFNGLBINDVERTEXARRAYPROC> m_bind_array{glad_glBindVertexArray, BindArray};
    ReplaceEntryPoint<PFNGLBINDBUFFERPROC> m_bind_buffer{glad_glBindBuffer, BindBuffer};
    ReplaceEntryPoint<PFNGLBUFFERDATAPROC> m_data{glad_glBufferData, Data};
    ReplaceEntryPoint<PFNGLBUFFERSUBDATAPROC> m_sub_data{glad_glBufferSubData, SubData};
    ReplaceEntryPoint<PFNGLVERTEXATTRIBPOINTERPROC> m_attribute{glad_glVertexAttribPointer,
                                                                Attribute};
    ReplaceEntryPoint<PFNGLENABLEVERTEXATTRIBARRAYPROC> m_enable{glad_glEnableVertexAttribArray,
                                                                 Enable};
    ReplaceEntryPoint<PFNGLISBUFFERPROC> m_is_buffer{glad_glIsBuffer, IsObject};
    ReplaceEntryPoint<PFNGLISVERTEXARRAYPROC> m_is_array{glad_glIsVertexArray, IsObject};
    ReplaceEntryPoint<PFNGLDELETEBUFFERSPROC> m_delete_buffers{glad_glDeleteBuffers, DeleteBuffer};
    ReplaceEntryPoint<PFNGLDELETEVERTEXARRAYSPROC> m_delete_arrays{glad_glDeleteVertexArrays,
                                                                   DeleteArray};
};

WaterfallPass::PreparedGeometry Geometry(std::size_t sites) {
    // Fixture bound: at most 1000 sites / 288 KiB, checked before multiplication.
    if (sites > 1000)
        throw std::length_error("Waterfall adoption fixture exceeds 1000 sites");
    WaterfallPass::PreparedGeometry geometry;
    geometry.sites.resize(sites);
    geometry.vertices.resize(sites * 72);
    for (std::size_t index = 0; index < geometry.vertices.size(); ++index)
        geometry.vertices[index] = static_cast<float>(index);
    return geometry;
}

TEST(WaterfallAdoption, UploadIsBoundedAndRetainsEveryVertexInOrder) {
    UploadCapture capture;
    WaterfallPass pass;
    auto geometry = Geometry(1000);
    const auto expected = geometry.vertices;
    pass.begin_adoption(std::move(geometry));
    EXPECT_EQ(capture.allocations, 0u); // Adoption is deferred until the loading stage advances.
    bool complete = false;
    for (unsigned frame = 0; frame < 5; ++frame) {
        complete = pass.adopt_next(nullptr, {});
        ASSERT_EQ(capture.writes.size(), frame + 1u);
        EXPECT_EQ(capture.allocations, frame + 1u);
        EXPECT_LE(capture.writes.back(), 64u * 1024u);
        EXPECT_EQ(complete, frame == 4);
    }
    EXPECT_TRUE(complete);
    EXPECT_EQ(capture.received, expected);
    EXPECT_EQ(capture.allocations, 5u);
    EXPECT_TRUE(pass.adopt_next(nullptr, {}));
    EXPECT_EQ(capture.writes.size(), 5u);
    pass.destroy_geometry();
    EXPECT_EQ(capture.deleted_buffers, 5u);
    EXPECT_EQ(capture.deleted_arrays, 5u);
}

TEST(WaterfallAdoption, CancelDuringUploadReleasesObjectsAndCanRestart) {
    UploadCapture capture;
    WaterfallPass pass;
    pass.begin_adoption(Geometry(1000));
    EXPECT_FALSE(pass.adopt_next(nullptr, {}));
    pass.destroy_geometry();
    EXPECT_EQ(capture.deleted_buffers, 1u);
    EXPECT_EQ(capture.deleted_arrays, 1u);
    EXPECT_TRUE(pass.adopt_next(nullptr, {}));
    EXPECT_EQ(capture.writes.size(), 1u);
    capture.received.clear();
    pass.begin_adoption(Geometry(1));
    EXPECT_TRUE(pass.adopt_next(nullptr, {}));
    EXPECT_EQ(capture.allocations, 2u);
    pass.destroy_geometry();
    pass.destroy_geometry();
    EXPECT_EQ(capture.deleted_buffers, 2u);
    EXPECT_EQ(capture.deleted_arrays, 2u);
}

TEST(WaterfallAdoption, AddsOneSprayPerFrameInSiteOrderWithExistingCap) {
    UploadCapture capture;
    WaterfallPass pass;
    ParticlePass particles;
    auto geometry = Geometry(30);
    for (std::size_t index = 0; index < geometry.sites.size(); ++index)
        geometry.sites[index].foot.x = static_cast<float>(index);
    pass.begin_adoption(std::move(geometry));
    const auto root = std::filesystem::path(LUMINUMBRA_SOURCE_ROOT);
    for (std::size_t frame = 0; frame < 24; ++frame) {
        EXPECT_EQ(pass.adopt_next(&particles, root), frame == 23);
        particles.rebuild_emitter_descriptors(123, 0);
        ASSERT_EQ(particles.emitter_descriptors().size(), frame + 1);
        EXPECT_EQ(particles.emitter_descriptors().back().origin_region[0],
                  static_cast<int>(frame * 1000));
    }
    EXPECT_TRUE(pass.adopt_next(&particles, root));
    particles.clear_emitters();
    EXPECT_FALSE(particles.has_emitters());
    pass.destroy_geometry();
}

TEST(WaterfallAdoption, InvalidPayloadIsRefusedBeforeGlAllocation) {
    UploadCapture capture;
    WaterfallPass pass;
    auto geometry = Geometry(1);
    geometry.vertices.pop_back();
    EXPECT_THROW(pass.begin_adoption(std::move(geometry)), std::length_error);
    EXPECT_EQ(capture.allocations, 0u);
    EXPECT_TRUE(pass.adopt_next(nullptr, {}));
}
} // namespace
