#pragma once

#include "DiagnosticCapture.h"

#include <glad/glad.h>

#include <cstdint>

namespace Luminumbra::Rendering {

// Render-thread owner; destruction requires the creating GL context. Standalone
// storage for later MRT integration: never modifies a caller's framebuffer.
class DiagnosticTargets {
public:
    DiagnosticTargets() = default;
    DiagnosticTargets(const DiagnosticTargets&) = delete;
    DiagnosticTargets& operator=(const DiagnosticTargets&) = delete;
    ~DiagnosticTargets() {
        reset();
    }

    // Off performs no GL calls when already empty. Refusal retains old storage.
    // At most 16M RGBA32UI pixels (256 MiB); transactional resize peaks at 512 MiB.
    bool ensure(bool enabled, const DiagnosticExtent& extent, std::uint32_t draw_buffers = 1) {
        if (!enabled) {
            reset();
            return true;
        }
        constexpr std::uint32_t max_pixels = 16777216;
        if (extent.width == 0 || extent.height == 0 || extent.width > 16384 ||
            extent.height > 16384 || extent.width > max_pixels / extent.height ||
            draw_buffers == 0) {
            return false;
        }
        if (glGetInternalformativ == nullptr || glTexStorage2D == nullptr ||
            glClearTexImage == nullptr) {
            return false;
        }
        GLint max_size = 0;
        GLint max_buffers = 0;
        GLint max_attachments = 0;
        GLint renderable = 0;
        glGetIntegerv(GL_MAX_TEXTURE_SIZE, &max_size);
        glGetIntegerv(GL_MAX_DRAW_BUFFERS, &max_buffers);
        glGetIntegerv(GL_MAX_COLOR_ATTACHMENTS, &max_attachments);
        glGetInternalformativ(
            GL_TEXTURE_2D, GL_RGBA32UI, GL_FRAMEBUFFER_RENDERABLE, 1, &renderable);
        if (max_size < static_cast<GLint>(extent.width) ||
            max_size < static_cast<GLint>(extent.height) || max_buffers <= 0 ||
            max_attachments <= 0 || draw_buffers > static_cast<std::uint32_t>(max_buffers) ||
            draw_buffers > static_cast<std::uint32_t>(max_attachments) ||
            (renderable != GL_FULL_SUPPORT && renderable != GL_CAVEAT_SUPPORT)) {
            return false;
        }
        if (texture_ != 0 && extent_.width == extent.width && extent_.height == extent.height) {
            return true;
        }
        BindingGuard guard;
        // Retiring a bound owned object would invalidate the caller's binding.
        // Refuse instead; consumers must finish using old storage before resize.
        if (texture_bound() ||
            (framebuffer_ != 0 && (static_cast<GLuint>(guard.framebuffer) == framebuffer_ ||
                                   static_cast<GLuint>(guard.read_framebuffer) == framebuffer_))) {
            return false;
        }
        GLuint texture = 0;
        GLuint framebuffer = 0;
        glGenTextures(1, &texture);
        glGenFramebuffers(1, &framebuffer);
        bool complete = false;
        if (texture != 0 && framebuffer != 0) {
            glBindTexture(GL_TEXTURE_2D, texture);
            glTexStorage2D(GL_TEXTURE_2D,
                           1,
                           GL_RGBA32UI,
                           static_cast<GLsizei>(extent.width),
                           static_cast<GLsizei>(extent.height));
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            glBindFramebuffer(GL_DRAW_FRAMEBUFFER, framebuffer);
            glFramebufferTexture2D(
                GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture, 0);
            glDrawBuffer(GL_COLOR_ATTACHMENT0);
            complete = glCheckFramebufferStatus(GL_DRAW_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
        }
        if (!complete) {
            destroy(texture, framebuffer);
            return false;
        }
        // Texture clear ignores scissor, color masks, pack/unpack and framebuffer state.
        glClearTexImage(texture, 0, GL_RGBA_INTEGER, GL_UNSIGNED_INT, nullptr);
        destroy(texture_, framebuffer_);
        texture_ = texture;
        framebuffer_ = framebuffer;
        extent_ = extent;
        return true;
    }

    // End ownership after consumers finish; GL unbinds any deleted owned handles.
    void reset() {
        destroy(texture_, framebuffer_);
        texture_ = 0;
        framebuffer_ = 0;
        extent_ = {};
    }
    GLuint texture() const {
        return texture_;
    }
    GLuint framebuffer() const {
        return framebuffer_;
    }
    DiagnosticExtent extent() const {
        return extent_;
    }

private:
    bool texture_bound() const {
        if (texture_ == 0) {
            return false;
        }
        GLint units = 0;
        GLint active = 0;
        glGetIntegerv(GL_MAX_COMBINED_TEXTURE_IMAGE_UNITS, &units);
        glGetIntegerv(GL_ACTIVE_TEXTURE, &active);
        // Resize-only scan, bounded to 256 texture units; refuse larger contexts.
        if (units <= 0 || units > 256) {
            return true;
        }
        bool bound = false;
        for (GLint unit = 0; unit < units; ++unit) {
            glActiveTexture(GL_TEXTURE0 + static_cast<GLenum>(unit));
            GLint binding = 0;
            glGetIntegerv(GL_TEXTURE_BINDING_2D, &binding);
            if (static_cast<GLuint>(binding) == texture_) {
                bound = true;
                break;
            }
        }
        glActiveTexture(static_cast<GLenum>(active));
        return bound || image_bound();
    }
    // Deleting a texture also clears image-unit bindings, so those count as bound.
    bool image_bound() const {
        GLint images = 0;
        glGetIntegerv(GL_MAX_IMAGE_UNITS, &images);
        // Resize-only scan, bounded to 256 image units; refuse larger contexts.
        if (images < 0 || images > 256) {
            return true;
        }
        for (GLint unit = 0; unit < images; ++unit) {
            GLint name = 0;
            glGetIntegeri_v(GL_IMAGE_BINDING_NAME, static_cast<GLuint>(unit), &name);
            if (static_cast<GLuint>(name) == texture_) {
                return true;
            }
        }
        return false;
    }
    struct BindingGuard {
        GLint texture = 0;
        GLint framebuffer = 0;
        GLint read_framebuffer = 0;
        BindingGuard() {
            glGetIntegerv(GL_TEXTURE_BINDING_2D, &texture);
            glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &framebuffer);
            glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &read_framebuffer);
        }
        ~BindingGuard() {
            glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(texture));
            glBindFramebuffer(GL_DRAW_FRAMEBUFFER, static_cast<GLuint>(framebuffer));
        }
    };
    static void destroy(GLuint texture, GLuint framebuffer) {
        if (framebuffer != 0) {
            glDeleteFramebuffers(1, &framebuffer);
        }
        if (texture != 0) {
            glDeleteTextures(1, &texture);
        }
    }
    GLuint texture_ = 0;
    GLuint framebuffer_ = 0;
    DiagnosticExtent extent_{};
};

} // namespace Luminumbra::Rendering
