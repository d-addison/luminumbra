#pragma once

// Shared inline GL helpers for the extracted render pass classes.
// These mirror the anonymous-namespace helpers in RenderPipeline.cpp, which
// keeps its own copies for the resources it still owns; the duplication is
// intentional so the extraction stays a mechanical move with zero behavior
// change.

#include <glad/glad.h>
#include <glm/glm.hpp>
#include <string>

// only ShadowMap is needed here (cascade-split helpers), so
// include the extracted header instead of the RenderPipeline god-object — this
// decouples every pass that includes PassGlHelpers from the pipeline.
#include "../ShadowMap.h"

namespace Luminumbra::Rendering::PassGl {

inline bool is_valid_gl_object_name(GLenum identifier, GLuint name) {
    switch (identifier) {
        case GL_BUFFER:
            return glIsBuffer(name) == GL_TRUE;
        case GL_FRAMEBUFFER:
            return glIsFramebuffer(name) == GL_TRUE;
        case GL_PROGRAM:
            return glIsProgram(name) == GL_TRUE;
        case GL_QUERY:
            return glIsQuery(name) == GL_TRUE;
        case GL_RENDERBUFFER:
            return glIsRenderbuffer(name) == GL_TRUE;
        case GL_TEXTURE:
            return glIsTexture(name) == GL_TRUE;
        case GL_VERTEX_ARRAY:
            return glIsVertexArray(name) == GL_TRUE;
        default:
            return true;
    }
}

inline void label_gl_object(GLenum identifier, GLuint name, const std::string& label) {
    if (name == 0) {
        return;
    }
    if (!is_valid_gl_object_name(identifier, name)) {
        return;
    }
#ifdef GL_VERSION_4_3
    if (glObjectLabel) {
        glObjectLabel(identifier, name, -1, label.c_str());
    }
#else
    (void)identifier;
    (void)label;
#endif
}

// --- Nsight / RenderDoc debug-group markers -----------------
// KHR_debug command-stream groups so GPU-capture tools show named per-pass spans.
// Guarded on GL 4.3 + a non-null entry point (same discipline as label_gl_object);
// no-ops on contexts that lack KHR_debug, so they never affect rendered pixels.
// A shared depth counter lets a frame-end assert catch mismatched push/pop nesting
// (which would garble a capture even though it is invisible to a pixel diff).
inline int& debug_group_depth() {
    static int depth = 0; // one shared instance across TUs (inline fn local static)
    return depth;
}

inline void push_debug_group(const std::string& label) {
#ifdef GL_VERSION_4_3
    if (glPushDebugGroup) {
        glPushDebugGroup(GL_DEBUG_SOURCE_APPLICATION, 0u, -1, label.c_str());
        ++debug_group_depth();
    }
#else
    (void)label;
#endif
}

inline void pop_debug_group() {
#ifdef GL_VERSION_4_3
    if (glPopDebugGroup && debug_group_depth() > 0) {
        glPopDebugGroup();
        --debug_group_depth();
    }
#endif
}

// RAII scoped group: balance is guaranteed even if a pass early-returns.
struct ScopedDebugGroup {
    explicit ScopedDebugGroup(const std::string& label) {
        push_debug_group(label);
    }
    ~ScopedDebugGroup() {
        pop_debug_group();
    }
    ScopedDebugGroup(const ScopedDebugGroup&) = delete;
    ScopedDebugGroup& operator=(const ScopedDebugGroup&) = delete;
};

inline void ExtractFrustumPlanes(const glm::mat4& m, glm::vec4 planes[6]) {
    planes[0] =
        glm::vec4(m[0][3] + m[0][0], m[1][3] + m[1][0], m[2][3] + m[2][0], m[3][3] + m[3][0]);
    planes[1] =
        glm::vec4(m[0][3] - m[0][0], m[1][3] - m[1][0], m[2][3] - m[2][0], m[3][3] - m[3][0]);
    planes[2] =
        glm::vec4(m[0][3] + m[0][1], m[1][3] + m[1][1], m[2][3] + m[2][1], m[3][3] + m[3][1]);
    planes[3] =
        glm::vec4(m[0][3] - m[0][1], m[1][3] - m[1][1], m[2][3] - m[2][1], m[3][3] - m[3][1]);
    planes[4] =
        glm::vec4(m[0][3] + m[0][2], m[1][3] + m[1][2], m[2][3] + m[2][2], m[3][3] + m[3][2]);
    planes[5] =
        glm::vec4(m[0][3] - m[0][2], m[1][3] - m[1][2], m[2][3] - m[2][2], m[3][3] - m[3][2]);
    for (int i = 0; i < 6; ++i) {
        float inv_len = 1.0f / glm::length(glm::vec3(planes[i]));
        planes[i] *= inv_len;
    }
}

inline void set_default_shadow_cascade_splits(ShadowMap& shadow_map) {
    shadow_map.cascade_splits.resize(ShadowMap::CASCADE_COUNT + 1);
    shadow_map.cascade_splits[0] = 0.1f;
    shadow_map.cascade_splits[1] = 15.0f;
    shadow_map.cascade_splits[2] = 40.0f;
    shadow_map.cascade_splits[3] = 100.0f;
    shadow_map.cascade_splits[4] = 250.0f;
}

inline bool has_valid_shadow_cascade_splits(const ShadowMap& shadow_map) {
    return shadow_map.cascade_splits.size() >= ShadowMap::CASCADE_COUNT + 1;
}

} // namespace Luminumbra::Rendering::PassGl
