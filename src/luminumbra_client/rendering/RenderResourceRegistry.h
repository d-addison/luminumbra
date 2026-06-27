#pragma once

#include "RenderResourceHandles.h"

#include <string>
#include <string_view>
#include <unordered_map>

// Spec 016 (FR-B-001/002/003): the render resource registry.
//
// Owns / tracks render targets by NAME and hands out typed handles. During the
// incremental migration it primarily ADOPTS externally-owned GL objects (the
// G-Buffer, the lighting FBO, intermediate targets currently owned by passes /
// the pipeline) so a converting pass can reference them by handle WITHOUT a
// `friend` reach into RenderPipeline. As ownership moves off the pipeline
// (later 016 phases), the same names become registry-allocated resources and no
// pass code changes.
//
// Strict descriptor/registry layer only (016 OQ-1): no automatic barriers, no
// transient aliasing. Header-only + trivially cheap; cleared/repopulated per
// frame for adopted (wrap-existing) entries.

namespace Luminumbra::Rendering {

class RenderResourceRegistry {
public:
    // Adopt (wrap) an externally-owned GL framebuffer under `name`. Re-adopting
    // the same name updates the wrapped id (the pilot re-adopts per frame).
    FboHandle adopt_fbo(std::string_view name, u32 gl_id) {
        FboHandle h = adopt_fbo_handle(gl_id);
        m_fbos[std::string(name)] = h;
        return h;
    }

    TextureHandle adopt_texture(std::string_view name, u32 gl_id) {
        TextureHandle h = adopt_texture_handle(gl_id);
        m_textures[std::string(name)] = h;
        return h;
    }

    // Lookup; returns an invalid handle if the name was never registered.
    FboHandle fbo(std::string_view name) const {
        auto it = m_fbos.find(std::string(name));
        return it == m_fbos.end() ? FboHandle{} : it->second;
    }
    TextureHandle texture(std::string_view name) const {
        auto it = m_textures.find(std::string(name));
        return it == m_textures.end() ? TextureHandle{} : it->second;
    }

    bool has_fbo(std::string_view name) const { return m_fbos.count(std::string(name)) != 0; }

    // Drop per-frame adopted entries (kept minimal: the pilot simply re-adopts).
    void clear_adopted() { m_fbos.clear(); m_textures.clear(); }

private:
    // Disambiguate from the free helpers in RenderResourceHandles.h.
    static FboHandle adopt_fbo_handle(u32 gl_id) { return FboHandle{gl_id, true}; }
    static TextureHandle adopt_texture_handle(u32 gl_id) { return TextureHandle{gl_id}; }

    std::unordered_map<std::string, FboHandle> m_fbos;
    std::unordered_map<std::string, TextureHandle> m_textures;
};

} // namespace Luminumbra::Rendering
