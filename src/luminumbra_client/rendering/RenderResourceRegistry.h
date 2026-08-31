#pragma once

#include "RenderResourceHandles.h"
#include "rhi/RhiResource.h"

#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

// Render resource registry.
//
// Owns / tracks render targets by NAME and hands out typed handles. Two entry
// classes:
//   * OWNED: the registry
//     ALLOCATES the GL objects from full-fidelity descriptors, holds their
//     lifetime (Persistent / History) + layout metadata, drives resize
//     (delete + recreate per desc, re-attaching any owned FBO that names the
//     texture), and destroys them. Owned entries SURVIVE frame boundaries —
//     never cleared by clear_adopted (the RegistryOwnershipParity ctest pin).
//   * ADOPTED (the original incremental-migration path): wrap an
//     externally-owned GL object under a name; re-adopting updates the id.
// Lookups consult OWNED first, then adopted, so a family migrates to
// ownership without any pass-side change.
//
// Descriptor fidelity: TextureDesc carries raw GL enums (as u32) for
// format/filter/wrap/compare state. Migrated objects remain parameter-identical
// to their direct-GL equivalents, while the RHI backing stays beneath these
// typed handles. Load/store/layout fields document each resource contract.
//
// Strict descriptor/registry layer only: no automatic barriers and no
// transient aliasing.

namespace Luminumbra::Rendering {

// Resource lifetime classes. History resources (TAAU history,
// froxel reproject) persist across frames BY DESIGN and must never be treated
// as transient; resize recreates storage without promising content
// preservation (matching today's behavior).
enum class ResourceLifetime : u8 {
    Persistent,
    History,
};

struct TextureDesc {
    u32 width = 0;
    u32 height = 0;
    u32 layers = 1;          // > 1 => GL_TEXTURE_2D_ARRAY (e.g. the shadow atlas)
    u32 internal_format = 0; // raw GL internal format (e.g. GL_RGBA16F)
    u32 format = 0;          // raw GL pixel format for the (null) upload
    u32 type = 0;            // raw GL pixel type
    u32 min_filter = 0;      // raw GL enums; 0 = leave default
    u32 mag_filter = 0;
    u32 wrap_s = 0;
    u32 wrap_t = 0;
    bool depth_compare = false; // GL_COMPARE_REF_TO_TEXTURE (shadow atlas)
    bool has_border_color = false;
    float border_color[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    ResourceLifetime lifetime = ResourceLifetime::Persistent;
    // Descriptive metadata shared with the RHI-backed path.
    // load/store: what a frame does with the contents ("clear", "load", "dont_care").
    const char* load_op = "dont_care";
    const char* store_op = "store";
    const char* expected_layout = "color_attachment"; // or "depth_attachment", "sampled"
    const char* debug_label = nullptr;
};

// A registry-owned renderbuffer descriptor (e.g. the lighting
// FBO's GL_DEPTH_COMPONENT24 depth attachment). Renderbuffers are non-samplable
// GPU storage attached via glFramebufferRenderbuffer.
struct RenderbufferDesc {
    u32 width = 0;
    u32 height = 0;
    u32 internal_format = 0; // raw GL internal format (e.g. GL_DEPTH_COMPONENT24)
    const char* expected_layout = "depth_attachment";
    const char* debug_label = nullptr;
};

struct FboAttachment {
    u32 attachment_point = 0; // raw GL enum (GL_COLOR_ATTACHMENT0 + i / GL_DEPTH_ATTACHMENT /...)
    std::string texture_name; // an OWNED texture OR renderbuffer name in this registry
                              // (attach_fbo resolves textures first, then renderbuffers)
};

struct FboDesc {
    std::vector<FboAttachment> attachments;
    std::vector<u32>
        draw_buffers;      // explicit glDrawBuffers order; empty + !no_color = single COLOR0
    bool no_color = false; // depth-only FBO (glDrawBuffer(GL_NONE)), e.g. the shadow atlas
    const char* debug_label = nullptr;
};

class RenderResourceRegistry {
public:
    // --- Owned path ----------------------------------------------------------
    // Allocate a registry-OWNED texture / FBO from a full-fidelity descriptor.
    // Returns an invalid handle on GL failure or name collision with an
    // existing owned entry. Requires a current GL context.
    TextureHandle create_texture(std::string_view name, const TextureDesc& desc);
    RenderbufferHandle create_renderbuffer(std::string_view name, const RenderbufferDesc& desc);
    FboHandle create_fbo(std::string_view name, const FboDesc& desc);
    // Delete + recreate the named owned texture's storage at the new size
    // (same desc otherwise) and re-attach every owned FBO that references it.
    // Contents are NOT preserved (matches the pre-registry resize behavior).
    bool resize_texture(std::string_view name, u32 new_width, u32 new_height);
    // Destroy one owned entry (texture or fbo) / all owned entries. Owned GL
    // objects are deleted HERE and nowhere else — the ownership contract.
    void destroy_owned(std::string_view name);
    void destroy_all_owned();
    bool owns_texture(std::string_view name) const {
        return m_owned_textures.count(std::string(name)) != 0;
    }
    bool owns_fbo(std::string_view name) const {
        return m_owned_fbos.count(std::string(name)) != 0;
    }
    bool owns_renderbuffer(std::string_view name) const {
        return m_owned_renderbuffers.count(std::string(name)) != 0;
    }
    const TextureDesc* owned_texture_desc(std::string_view name) const {
        auto it = m_owned_textures.find(std::string(name));
        return it == m_owned_textures.end() ? nullptr : &it->second.desc;
    }
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

    // Lookup; OWNED entries win, then adopted; invalid handle if unknown.
    FboHandle fbo(std::string_view name) const {
        auto owned = m_owned_fbos.find(std::string(name));
        if (owned != m_owned_fbos.end()) {
            return FboHandle{owned->second.gl_id, true};
        }
        auto it = m_fbos.find(std::string(name));
        return it == m_fbos.end() ? FboHandle{} : it->second;
    }
    TextureHandle texture(std::string_view name) const {
        auto owned = m_owned_textures.find(std::string(name));
        if (owned != m_owned_textures.end()) {
            return TextureHandle{owned->second.gl_id};
        }
        auto it = m_textures.find(std::string(name));
        return it == m_textures.end() ? TextureHandle{} : it->second;
    }

    bool has_fbo(std::string_view name) const {
        return m_owned_fbos.count(std::string(name)) != 0 || m_fbos.count(std::string(name)) != 0;
    }

    // Drop per-frame ADOPTED entries only — owned entries persist across frame
    // boundaries by contract (the RegistryOwnershipParity pin).
    void clear_adopted() {
        m_fbos.clear();
        m_textures.clear();
    }

private:
    // An owned entry carries its direct-GL object
    // object (gl_id) AND an optional layer-2 backend backing (rhi_backing). The
    // backing is null unless a pass is driven through a Diligent device. A pass never
    // sees either field -- only the layer-1 handle -- so it cannot tell GL from RHI.
    struct OwnedTexture {
        u32 gl_id = 0;
        TextureDesc desc;
        Rhi::RhiTexture rhi_backing{};
    };
    struct OwnedFbo {
        u32 gl_id = 0;
        FboDesc desc;
    };
    struct OwnedRenderbuffer {
        u32 gl_id = 0;
        RenderbufferDesc desc;
        Rhi::RhiTexture rhi_backing{};
    };

    // Disambiguate from the free helpers in RenderResourceHandles.h.
    static FboHandle adopt_fbo_handle(u32 gl_id) {
        return FboHandle{gl_id, true};
    }
    static TextureHandle adopt_texture_handle(u32 gl_id) {
        return TextureHandle{gl_id};
    }

    bool attach_fbo(OwnedFbo& fbo_entry); // (re)binds attachments per desc

    std::unordered_map<std::string, FboHandle> m_fbos;
    std::unordered_map<std::string, TextureHandle> m_textures;
    std::unordered_map<std::string, OwnedTexture> m_owned_textures;
    std::unordered_map<std::string, OwnedFbo> m_owned_fbos;
    std::unordered_map<std::string, OwnedRenderbuffer> m_owned_renderbuffers;
};

} // namespace Luminumbra::Rendering
