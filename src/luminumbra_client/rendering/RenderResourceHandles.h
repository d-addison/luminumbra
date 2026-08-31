#pragma once

#include "../../include/luminumbra/core/Types.h"

// typed render-resource handles.
//
// A handle is a thin, strongly-typed wrapper over a GL object name. It exists so
// passes reference render targets/textures/buffers BY HANDLE through the
// RenderContext + RenderResourceRegistry instead of reaching into RenderPipeline
// privates via `friend`. The registry can either ALLOCATE a resource (owning it)
// or ADOPT an externally-owned GL object (wrap-existing) during the incremental
// migration — both yield the same handle type, so a pass cannot tell (nor should
// it) whether the pipeline or the registry owns the underlying object.
//
// This is deliberately the strict "pass-descriptor / resource-registry" layer
// (  lean), NOT a frame graph with automatic barrier insertion / transient
// aliasing. That can be added later if the Vulkan port needs it.

namespace Luminumbra::Rendering {

// Distinct handle types prevent passing an FBO where a texture is expected.
struct TextureHandle {
    u32 id = 0; // GL texture name (0 == invalid)
    explicit operator bool() const {
        return id != 0;
    }
    bool operator==(const TextureHandle& o) const {
        return id == o.id;
    }
};

struct FboHandle {
    u32 id = 0;         // GL framebuffer name (0 is VALID here: the default framebuffer)
    bool valid = false; // distinguishes "default framebuffer (0)" from "unset"
    explicit operator bool() const {
        return valid;
    }
    bool operator==(const FboHandle& o) const {
        return id == o.id && valid == o.valid;
    }
};

struct BufferHandle {
    u32 id = 0; // GL buffer name (0 == invalid)
    explicit operator bool() const {
        return id != 0;
    }
    bool operator==(const BufferHandle& o) const {
        return id == o.id;
    }
};

// a registry-owned renderbuffer (e.g. the lighting FBO's depth
// attachment). Distinct from TextureHandle because a renderbuffer is not
// samplable and attaches via glFramebufferRenderbuffer, not glFramebufferTexture.
struct RenderbufferHandle {
    u32 id = 0; // GL renderbuffer name (0 == invalid)
    explicit operator bool() const {
        return id != 0;
    }
    bool operator==(const RenderbufferHandle& o) const {
        return id == o.id;
    }
};

// Helpers to construct handles when adopting an existing GL name.
inline TextureHandle adopt_texture(u32 gl_id) {
    return TextureHandle{gl_id};
}
inline FboHandle adopt_fbo(u32 gl_id) {
    return FboHandle{gl_id, true};
}
inline FboHandle default_framebuffer() {
    return FboHandle{0u, true};
}
inline BufferHandle adopt_buffer(u32 gl_id) {
    return BufferHandle{gl_id};
}

} // namespace Luminumbra::Rendering
