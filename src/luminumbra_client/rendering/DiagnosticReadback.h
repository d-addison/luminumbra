#pragma once

#include "DiagnosticDrawTable.h"
#include "DiagnosticTargets.h"
#include "GBufferTargets.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <memory>
#include <utility>

namespace Luminumbra::Rendering {

// Admission includes the identity target, the core's largest plane and workspace,
// retained RGB capture pixels, one 64 KiB decode row and a conservative 128 MiB
// bound for the draw registry.
// No image allocation or GL call precedes the overflow-safe size check.
inline bool DiagnosticReadbackBudget(const DiagnosticExtent& extent, std::size_t& retained) {
    constexpr std::size_t limit = 512U * 1024U * 1024U;
    constexpr std::size_t registry = 128U * 1024U * 1024U;
    constexpr std::size_t workspace = 64U * 1024U * 1024U + 4096U;
    constexpr std::size_t row = 16384U * 4U;
    if (extent.width == 0 || extent.height == 0 || extent.width > 16384 || extent.height > 16384 ||
        extent.width > 16777216U / extent.height) {
        return false;
    }
    const auto pixels = static_cast<std::size_t>(extent.width) * extent.height;
    if (pixels > (limit - registry - workspace - row) / (2U * sizeof(DiagnosticId) + 3U)) {
        return false;
    }
    retained = registry + row + pixels * (sizeof(DiagnosticId) + 3U);
    return true;
}

// Synchronous, bottom-first transport into the bundle core's single plane.
// RGBA8 normal/material decoding uses only one row, bounded to 64 KiB.
class DiagnosticGlSource final : public DiagnosticBundleSource {
public:
    DiagnosticGlSource(const GBuffer& gbuffer,
                       const DiagnosticTargets& targets,
                       const DiagnosticExtent& extent)
        : gbuffer_(gbuffer)
        , targets_(targets)
        , extent_(extent) {}

    bool read_aov(DiagnosticAov aov, std::span<float> samples) override {
        if (aov != DiagnosticAov::Albedo && aov != DiagnosticAov::Normal &&
            aov != DiagnosticAov::Depth && aov != DiagnosticAov::Material) {
            return false;
        }
        const bool rgb = aov == DiagnosticAov::Albedo || aov == DiagnosticAov::Normal;
        if (!shape(samples.size(), rgb ? 3U : 1U)) {
            return false;
        }
        ReadState guard(gbuffer_.fbo_id);
        if (glCheckFramebufferStatus(GL_READ_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
            return false;
        }
        if (aov == DiagnosticAov::Albedo || aov == DiagnosticAov::Depth) {
            glReadBuffer(GL_COLOR_ATTACHMENT2);
            glReadPixels(0,
                         0,
                         static_cast<GLsizei>(extent_.width),
                         static_cast<GLsizei>(extent_.height),
                         rgb ? GL_RGB : GL_DEPTH_COMPONENT,
                         GL_FLOAT,
                         samples.data());
            return glGetError() == GL_NO_ERROR;
        }
        glReadBuffer(GL_COLOR_ATTACHMENT1);
        std::array<unsigned char, 16384U * 4U> row{};
        for (std::uint32_t y = 0; y < extent_.height; ++y) {
            glReadPixels(0,
                         static_cast<GLint>(y),
                         static_cast<GLsizei>(extent_.width),
                         1,
                         GL_RGBA,
                         GL_UNSIGNED_BYTE,
                         row.data());
            if (glGetError() != GL_NO_ERROR) {
                return false;
            }
            for (std::uint32_t x = 0; x < extent_.width; ++x) {
                const auto pixel = static_cast<std::size_t>(y) * extent_.width + x;
                const auto offset = static_cast<std::size_t>(x) * 4U;
                if (aov == DiagnosticAov::Material) {
                    samples[pixel] = row[offset + 3];
                } else {
                    float nx = static_cast<float>(row[offset]) / 255.0f * 2.0f - 1.0f;
                    float ny = static_cast<float>(row[offset + 1]) / 255.0f * 2.0f - 1.0f;
                    const float nz = 1.0f - std::abs(nx) - std::abs(ny);
                    if (nz < 0.0f) {
                        const float old_x = nx;
                        nx = (1.0f - std::abs(ny)) * (nx >= 0.0f ? 1.0f : -1.0f);
                        ny = (1.0f - std::abs(old_x)) * (ny >= 0.0f ? 1.0f : -1.0f);
                    }
                    const float length = std::sqrt(nx * nx + ny * ny + nz * nz);
                    samples[pixel * 3] = nx / length;
                    samples[pixel * 3 + 1] = ny / length;
                    samples[pixel * 3 + 2] = nz / length;
                }
            }
        }
        return true;
    }

    bool read_ids(std::span<DiagnosticId> pixels) override {
        if (!shape(pixels.size(), 1) || targets_.texture() == 0 ||
            targets_.extent().width != extent_.width ||
            targets_.extent().height != extent_.height) {
            return false;
        }
        ReadState guard(targets_.framebuffer());
        if (glCheckFramebufferStatus(GL_READ_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
            return false;
        }
        glReadBuffer(GL_COLOR_ATTACHMENT0);
        glReadPixels(0,
                     0,
                     static_cast<GLsizei>(extent_.width),
                     static_cast<GLsizei>(extent_.height),
                     GL_RGBA_INTEGER,
                     GL_UNSIGNED_INT,
                     pixels.data());
        return glGetError() == GL_NO_ERROR;
    }

private:
    struct ReadState {
        // Pixel-store skips and byte order also affect client-memory readback.
        static constexpr std::array<GLenum, 7> names{GL_PACK_ALIGNMENT,
                                                     GL_PACK_ROW_LENGTH,
                                                     GL_PACK_SKIP_ROWS,
                                                     GL_PACK_SKIP_PIXELS,
                                                     GL_PACK_SWAP_BYTES,
                                                     GL_PACK_LSB_FIRST,
                                                     GL_PIXEL_PACK_BUFFER_BINDING};
        std::array<GLint, names.size()> values{};
        GLint framebuffer = 0;
        GLint read_buffer = 0;
        explicit ReadState(GLuint source) {
            glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &framebuffer);
            for (std::size_t i = 0; i < names.size(); ++i) {
                glGetIntegerv(names[i], &values[i]);
            }
            glBindFramebuffer(GL_READ_FRAMEBUFFER, source);
            glGetIntegerv(GL_READ_BUFFER, &read_buffer);
            glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
            for (std::size_t i = 0; i + 1 < names.size(); ++i) {
                glPixelStorei(names[i], i == 0 ? 1 : 0);
            }
        }
        ~ReadState() {
            glReadBuffer(static_cast<GLenum>(read_buffer));
            glBindFramebuffer(GL_READ_FRAMEBUFFER, static_cast<GLuint>(framebuffer));
            for (std::size_t i = 0; i + 1 < names.size(); ++i) {
                glPixelStorei(names[i], values[i]);
            }
            glBindBuffer(GL_PIXEL_PACK_BUFFER, static_cast<GLuint>(values.back()));
        }
        ReadState(const ReadState&) = delete;
        ReadState& operator=(const ReadState&) = delete;
    };
    bool shape(std::size_t count, std::size_t channels) const {
        std::size_t retained = 0;
        return DiagnosticReadbackBudget(extent_, retained) && gbuffer_.fbo_id != 0 &&
               count == static_cast<std::size_t>(extent_.width) * extent_.height * channels;
    }
    const GBuffer& gbuffer_;
    const DiagnosticTargets& targets_;
    DiagnosticExtent extent_;
};

// One frame's scope and storage. Destruction ends capture on every refusal or
// exception; finish consumes the scope even when bundle publication fails.
class DiagnosticFrameCapture {
public:
    static bool Admitted(const DiagnosticExtent& extent, std::size_t& retained) {
        if (!DiagnosticReadbackBudget(extent, retained) || glGetInternalformativ == nullptr ||
            glTexStorage2D == nullptr || glClearTexImage == nullptr || glReadPixels == nullptr) {
            return false;
        }
        GLint buffers = 0, attachments = 0, size = 0, renderable = 0;
        glGetIntegerv(GL_MAX_DRAW_BUFFERS, &buffers);
        glGetIntegerv(GL_MAX_COLOR_ATTACHMENTS, &attachments);
        glGetIntegerv(GL_MAX_TEXTURE_SIZE, &size);
        glGetInternalformativ(
            GL_TEXTURE_2D, GL_RGBA32UI, GL_FRAMEBUFFER_RENDERABLE, 1, &renderable);
        return buffers >= 7 && buffers <= 32 && attachments >= 7 &&
               size >= static_cast<GLint>(extent.width) &&
               size >= static_cast<GLint>(extent.height) &&
               (renderable == GL_FULL_SUPPORT || renderable == GL_CAVEAT_SUPPORT);
    }
    explicit DiagnosticFrameCapture(const DiagnosticExtent& extent) {
        std::size_t retained = 0;
        if (Admitted(extent, retained)) {
            scope_ = std::make_unique<GBufferDiagnosticCapture>(targets_, draws_);
        }
    }
    bool finish(const std::filesystem::path& path,
                const DiagnosticCaptureOptions& options,
                DiagnosticBundleFrame& frame,
                const GBuffer& gbuffer) {
        auto ending = std::move(scope_);
        if (!options.enabled) {
            return true;
        }
        if (!ending) {
            return false;
        }
        ending->detach();
        frame.identity_complete = ending->complete();
        if (!DiagnosticReadbackBudget(frame.extent, frame.staging_bytes)) {
            return false;
        }
        DiagnosticGlSource source(gbuffer, targets_, frame.extent);
        return WriteDiagnosticBundle(path, options, frame, draws_, source);
    }

private:
    DiagnosticTargets targets_;
    DiagnosticDrawTable draws_;
    std::unique_ptr<GBufferDiagnosticCapture> scope_;
};
} // namespace Luminumbra::Rendering
