//  /  (  — the rendering pilot-gate ownership
// leg): the registry's OWNED-resource implementation. All GL object creation,
// resize (delete + recreate per desc, re-attaching referencing FBOs), and
// destruction for owned entries lives HERE and nowhere else. Descriptor
// fidelity note: descs carry raw GL enums so migrated objects are
// parameter-identical to the pass code they replace (see the header).

#include "RenderResourceRegistry.h"

#include "luminumbra/core/Types.h"

#include <algorithm>
#include <cstddef>
#include <glad/glad.h>
#include <limits>
#include <span>
#include <string>
#include <utility>

#include "core/Log.h"
#include "passes/PassGlHelpers.h"

namespace Luminumbra::Rendering {

namespace {

std::size_t add_allocation_bytes(std::size_t total, std::size_t bytes) {
    return bytes > std::numeric_limits<std::size_t>::max() - total
               ? std::numeric_limits<std::size_t>::max()
               : total + bytes;
}

std::size_t format_bytes(u32 format) {
    switch (format) {
        case GL_R8:
            return 1;
        case GL_R16F:
        case GL_RG8:
            return 2;
        case GL_RGB8:
        case GL_SRGB8:
            return 3;
        case GL_R32F:
        case GL_RG16F:
        case GL_RGBA8:
        case GL_SRGB8_ALPHA8:
        case GL_RGB10_A2:
        case GL_DEPTH_COMPONENT24: // Estimate the 32-bit depth storage slot.
        case GL_DEPTH_COMPONENT32F:
            return 4;
        case GL_RGB16F:
            return 6;
        case GL_RG32F:
        case GL_RGBA16F:
            return 8;
        case GL_RGB32F:
            return 12;
        case GL_RGBA32F:
            return 16;
        default:
            return std::numeric_limits<std::size_t>::max();
    }
}

std::size_t target_bytes(const RenderbufferDesc& desc) {
    TextureDesc texture;
    texture.width = desc.width;
    texture.height = desc.height;
    texture.internal_format = desc.internal_format;
    return RenderResourceRegistry::texture_allocation_bytes(texture);
}

bool valid_texture_extent(const TextureDesc& desc) {
    const auto maximum = static_cast<u32>(std::numeric_limits<GLsizei>::max());
    return desc.width != 0 && desc.height != 0 && desc.layers != 0 && desc.width <= maximum &&
           desc.height <= maximum && desc.layers <= maximum && desc.depth <= maximum &&
           (desc.depth == 0 || desc.layers == 1) &&
           RenderResourceRegistry::texture_allocation_bytes(desc) !=
               std::numeric_limits<std::size_t>::max();
}

bool allocate_texture_storage(u32 gl_id, const TextureDesc& desc) {
    if (!valid_texture_extent(desc))
        return false;
    // GL error state is global and sticky: drain any pre-existing flag so the
    // post-allocation check reflects THIS allocation only (init runs right
    // after other GL setup that may have left a flag set — otherwise the first
    // create_texture would false-fail on a stale error).
    while (glGetError() != GL_NO_ERROR) {}
    const GLenum target =
        desc.depth != 0 ? GL_TEXTURE_3D : (desc.layers > 1 ? GL_TEXTURE_2D_ARRAY : GL_TEXTURE_2D);
    glBindTexture(target, gl_id);
    if (desc.depth != 0 || desc.layers > 1) {
        glTexImage3D(target,
                     0,
                     static_cast<GLint>(desc.internal_format),
                     static_cast<GLsizei>(desc.width),
                     static_cast<GLsizei>(desc.height),
                     static_cast<GLsizei>(desc.depth != 0 ? desc.depth : desc.layers),
                     0,
                     static_cast<GLenum>(desc.format),
                     static_cast<GLenum>(desc.type),
                     nullptr);
    } else {
        glTexImage2D(target,
                     0,
                     static_cast<GLint>(desc.internal_format),
                     static_cast<GLsizei>(desc.width),
                     static_cast<GLsizei>(desc.height),
                     0,
                     static_cast<GLenum>(desc.format),
                     static_cast<GLenum>(desc.type),
                     nullptr);
    }
    if (desc.min_filter != 0)
        glTexParameteri(target, GL_TEXTURE_MIN_FILTER, static_cast<GLint>(desc.min_filter));
    if (desc.mag_filter != 0)
        glTexParameteri(target, GL_TEXTURE_MAG_FILTER, static_cast<GLint>(desc.mag_filter));
    if (desc.wrap_s != 0)
        glTexParameteri(target, GL_TEXTURE_WRAP_S, static_cast<GLint>(desc.wrap_s));
    if (desc.wrap_t != 0)
        glTexParameteri(target, GL_TEXTURE_WRAP_T, static_cast<GLint>(desc.wrap_t));
    if (desc.wrap_r != 0 && desc.depth != 0)
        glTexParameteri(target, GL_TEXTURE_WRAP_R, static_cast<GLint>(desc.wrap_r));
    if (desc.depth_compare) {
        glTexParameteri(target, GL_TEXTURE_COMPARE_MODE, GL_COMPARE_REF_TO_TEXTURE);
        glTexParameteri(target, GL_TEXTURE_COMPARE_FUNC, GL_LEQUAL);
    }
    if (desc.has_border_color) {
        glTexParameterfv(target, GL_TEXTURE_BORDER_COLOR, desc.border_color);
    }
    glBindTexture(target, 0);
    return glGetError() == GL_NO_ERROR;
}

} // namespace

std::size_t RenderResourceRegistry::texture_allocation_bytes(const TextureDesc& desc,
                                                             u32 mip_levels) {
    if (desc.width == 0 || desc.height == 0 || desc.layers == 0 || mip_levels == 0) {
        return 0;
    }
    const auto maximum = std::numeric_limits<std::size_t>::max();
    if (desc.depth != 0 && desc.layers != 1)
        return maximum;
    const auto texel_bytes = format_bytes(desc.internal_format);
    if (texel_bytes == maximum) {
        return maximum;
    }
    std::size_t total = 0;
    u32 width = desc.width;
    u32 height = desc.height;
    u32 slices = desc.depth != 0 ? desc.depth : desc.layers;
    // A u32 extent has at most 32 mip levels; volume depth shrinks, array layers do not.
    for (u32 mip = 0; mip < std::min(mip_levels, 32u); ++mip) {
        if (static_cast<std::size_t>(width) > maximum / height / slices / texel_bytes) {
            return maximum;
        }
        total = add_allocation_bytes(
            total, static_cast<std::size_t>(width) * height * slices * texel_bytes);
        if (width == 1 && height == 1 && (desc.depth == 0 || slices == 1)) {
            break;
        }
        width = std::max(1u, width / 2u);
        height = std::max(1u, height / 2u);
        if (desc.depth != 0)
            slices = std::max(1u, slices / 2u);
    }
    return total;
}

void RenderResourceRegistry::track_texture_allocation(std::string_view name,
                                                      const u32& gl_id,
                                                      const TextureDesc& desc,
                                                      u32 mip_levels) {
    m_tracked_allocations.insert_or_assign(
        std::string(name),
        TrackedAllocation{
            AllocationKind::Texture, &gl_id, nullptr, texture_allocation_bytes(desc, mip_levels)});
}

void RenderResourceRegistry::track_buffer_allocation(std::string_view name,
                                                     const u32& gl_id,
                                                     std::size_t bytes) {
    m_tracked_allocations.insert_or_assign(
        std::string(name), TrackedAllocation{AllocationKind::Buffer, &gl_id, nullptr, bytes});
}

void RenderResourceRegistry::track_backing_allocation(std::string_view name,
                                                      const std::size_t& bytes) {
    m_tracked_allocations.insert_or_assign(
        std::string(name), TrackedAllocation{AllocationKind::BackingStorage, nullptr, &bytes, 0});
}

void RenderResourceRegistry::record_buffer_allocation(u32 gl_id, std::size_t bytes) {
    if (gl_id == 0) {
        return;
    }
    const auto entry = m_buffer_allocations.try_emplace(gl_id, 0).first;
    if (m_buffer_bytes_low < entry->second) {
        --m_buffer_bytes_high;
    }
    m_buffer_bytes_low -= entry->second;
    if (bytes > std::numeric_limits<std::size_t>::max() - m_buffer_bytes_low) {
        ++m_buffer_bytes_high;
    }
    m_buffer_bytes_low += bytes;
    entry->second = bytes;
}

void RenderResourceRegistry::forget_buffer_allocation(u32 gl_id) {
    const auto entry = m_buffer_allocations.find(gl_id);
    if (entry != m_buffer_allocations.end()) {
        if (m_buffer_bytes_low < entry->second) {
            --m_buffer_bytes_high;
        }
        m_buffer_bytes_low -= entry->second;
        m_buffer_allocations.erase(entry);
    }
}

std::size_t RenderResourceRegistry::allocated_bytes() const {
    std::size_t total = 0;
    for (const auto& [name, texture] : m_owned_textures) {
        total = add_allocation_bytes(total, texture.allocation_bytes);
    }
    for (const auto& [name, buffer] : m_owned_renderbuffers) {
        total = add_allocation_bytes(total, target_bytes(buffer.desc));
    }
    for (const auto& [name, allocation] : m_tracked_allocations) {
        total = add_allocation_bytes(total, allocation.current_bytes());
    }
    return add_allocation_bytes(total,
                                m_buffer_bytes_high == 0 ? m_buffer_bytes_low
                                                         : std::numeric_limits<std::size_t>::max());
}

std::vector<RenderResourceRegistry::AllocationRecord>
RenderResourceRegistry::allocation_ledger() const {
    std::vector<AllocationRecord> result;
    for (const auto& [name, texture] : m_owned_textures) {
        result.push_back({name, AllocationKind::Texture, texture.gl_id, texture.allocation_bytes});
    }
    for (const auto& [name, buffer] : m_owned_renderbuffers) {
        result.push_back(
            {name, AllocationKind::Renderbuffer, buffer.gl_id, target_bytes(buffer.desc)});
    }
    for (const auto& [name, allocation] : m_tracked_allocations) {
        if (const auto bytes = allocation.current_bytes(); bytes != 0) {
            result.push_back(
                {name, allocation.kind, allocation.gl_id ? *allocation.gl_id : 0, bytes});
        }
    }
    for (const auto& [id, bytes] : m_buffer_allocations) {
        result.push_back({"buffer." + std::to_string(id), AllocationKind::Buffer, id, bytes});
    }
    std::sort(result.begin(), result.end(), [](const auto& left, const auto& right) {
        return left.name < right.name;
    });
    return result;
}

bool RenderResourceRegistry::replace_targets(std::span<const TargetExtent> extents) {
    if (extents.empty() || extents.size() > 32)
        return false;
    // Validate the complete request before allocating. The maximum scalar
    // extent fits GLsizei; division checks avoid overflow in width*height*layers.
    for (std::size_t i = 0; i < extents.size(); ++i) {
        const auto& e = extents[i];
        if (e.width == 0 || e.height == 0 ||
            e.width > static_cast<u32>(std::numeric_limits<GLsizei>::max()) ||
            e.height > static_cast<u32>(std::numeric_limits<GLsizei>::max()))
            return false;
        const auto key = std::string(e.name);
        const auto texture = m_owned_textures.find(key);
        const auto buffer = m_owned_renderbuffers.find(key);
        if (texture == m_owned_textures.end() && buffer == m_owned_renderbuffers.end())
            return false;
        const auto layers = texture == m_owned_textures.end()
                                ? 1u
                                : (texture->second.desc.depth != 0 ? texture->second.desc.depth
                                                                   : texture->second.desc.layers);
        if (layers == 0 || layers > static_cast<u32>(std::numeric_limits<GLsizei>::max()) ||
            static_cast<std::size_t>(e.width) >
                std::numeric_limits<std::size_t>::max() / e.height / layers / 16u)
            return false; // At most 16 bytes/texel for these render targets.
        for (std::size_t j = 0; j < i; ++j)
            if (e.name == extents[j].name)
                return false;
    }
    RenderResourceRegistry replacement;
    // Also clean partial allocations when an allocator throws.
    class Cleanup {
        RenderResourceRegistry& m_registry;

    public:
        explicit Cleanup(RenderResourceRegistry& registry)
            : m_registry(registry) {}
        ~Cleanup() {
            m_registry.destroy_all_owned();
        }
    };
    const Cleanup cleanup{replacement};
    for (const auto& e : extents) {
        const auto key = std::string(e.name);
        if (const auto it = m_owned_textures.find(key); it != m_owned_textures.end()) {
            auto desc = it->second.desc;
            desc.width = e.width;
            desc.height = e.height;
            if (!replacement.create_texture(e.name, desc).id)
                return false;
        } else {
            auto desc = m_owned_renderbuffers.at(key).desc;
            desc.width = e.width;
            desc.height = e.height;
            if (!replacement.create_renderbuffer(e.name, desc).id)
                return false;
        }
    }
    for (const auto& [name, fbo] : m_owned_fbos) {
        const auto contained = [&](const FboAttachment& attachment) {
            return replacement.m_owned_textures.contains(attachment.texture_name) ||
                   replacement.m_owned_renderbuffers.contains(attachment.texture_name);
        };
        if (!std::any_of(fbo.desc.attachments.begin(), fbo.desc.attachments.end(), contained))
            continue;
        if (!std::all_of(fbo.desc.attachments.begin(), fbo.desc.attachments.end(), contained) ||
            !replacement.create_fbo(name, fbo.desc).id)
            return false;
    }
    // No allocations or draws during publication. The replacement registry now
    // owns the retired objects and deletes FBOs before their attachments.
    for (auto& [name, value] : replacement.m_owned_textures)
        std::swap(value, m_owned_textures.at(name));
    for (auto& [name, value] : replacement.m_owned_renderbuffers)
        std::swap(value, m_owned_renderbuffers.at(name));
    for (auto& [name, value] : replacement.m_owned_fbos)
        std::swap(value, m_owned_fbos.at(name));
    return true;
}

TextureHandle RenderResourceRegistry::create_texture(std::string_view name,
                                                     const TextureDesc& desc) {
    const std::string key(name);
    if (m_owned_textures.count(key) != 0) {
        LUMINUMBRA_CORE_ERROR("RenderResourceRegistry: owned texture '{}' already exists", key);
        return TextureHandle{};
    }
    if (!valid_texture_extent(desc) || desc.internal_format == 0) {
        LUMINUMBRA_CORE_ERROR("RenderResourceRegistry: invalid desc for owned texture '{}'", key);
        return TextureHandle{};
    }
    OwnedTexture owned;
    owned.desc = desc;
    glGenTextures(1, &owned.gl_id);
    if (!allocate_texture_storage(owned.gl_id, owned.desc)) {
        LUMINUMBRA_CORE_ERROR("RenderResourceRegistry: GL storage allocation failed for '{}'", key);
        glDeleteTextures(1, &owned.gl_id);
        return TextureHandle{};
    }
    if (desc.debug_label != nullptr) {
        PassGl::label_gl_object(GL_TEXTURE, owned.gl_id, desc.debug_label);
    }
    owned.allocation_bytes = texture_allocation_bytes(owned.desc);
    const TextureHandle handle{owned.gl_id};
    m_owned_textures.emplace(key, owned);
    return handle;
}

RenderbufferHandle RenderResourceRegistry::create_renderbuffer(std::string_view name,
                                                               const RenderbufferDesc& desc) {
    const std::string key(name);
    if (m_owned_renderbuffers.count(key) != 0) {
        LUMINUMBRA_CORE_ERROR("RenderResourceRegistry: owned renderbuffer '{}' already exists",
                              key);
        return RenderbufferHandle{};
    }
    if (desc.width == 0 || desc.height == 0 || desc.internal_format == 0) {
        LUMINUMBRA_CORE_ERROR("RenderResourceRegistry: invalid desc for owned renderbuffer '{}'",
                              key);
        return RenderbufferHandle{};
    }
    // Drain the sticky global GL error flag so the post-allocation check reflects
    // THIS allocation only (see allocate_texture_storage).
    while (glGetError() != GL_NO_ERROR) {}
    OwnedRenderbuffer owned;
    owned.desc = desc;
    glGenRenderbuffers(1, &owned.gl_id);
    glBindRenderbuffer(GL_RENDERBUFFER, owned.gl_id);
    glRenderbufferStorage(GL_RENDERBUFFER,
                          static_cast<GLenum>(desc.internal_format),
                          static_cast<GLsizei>(desc.width),
                          static_cast<GLsizei>(desc.height));
    glBindRenderbuffer(GL_RENDERBUFFER, 0);
    if (glGetError() != GL_NO_ERROR) {
        LUMINUMBRA_CORE_ERROR(
            "RenderResourceRegistry: GL storage allocation failed for renderbuffer '{}'", key);
        glDeleteRenderbuffers(1, &owned.gl_id);
        return RenderbufferHandle{};
    }
    if (desc.debug_label != nullptr) {
        PassGl::label_gl_object(GL_RENDERBUFFER, owned.gl_id, desc.debug_label);
    }
    const RenderbufferHandle handle{owned.gl_id};
    m_owned_renderbuffers.emplace(key, owned);
    return handle;
}

bool RenderResourceRegistry::attach_fbo(OwnedFbo& fbo_entry) {
    glBindFramebuffer(GL_FRAMEBUFFER, fbo_entry.gl_id);
    for (const FboAttachment& attachment : fbo_entry.desc.attachments) {
        // Resolve owned textures first, then owned renderbuffers.
        const auto tex_it = m_owned_textures.find(attachment.texture_name);
        if (tex_it != m_owned_textures.end()) {
            const OwnedTexture& tex = tex_it->second;
            if (tex.desc.depth != 0 || tex.desc.layers > 1) {
                // Layered attachment (e.g. the cascaded shadow atlas).
                glFramebufferTexture(
                    GL_FRAMEBUFFER, static_cast<GLenum>(attachment.attachment_point), tex.gl_id, 0);
            } else {
                glFramebufferTexture2D(GL_FRAMEBUFFER,
                                       static_cast<GLenum>(attachment.attachment_point),
                                       GL_TEXTURE_2D,
                                       tex.gl_id,
                                       0);
            }
            continue;
        }
        const auto rb_it = m_owned_renderbuffers.find(attachment.texture_name);
        if (rb_it != m_owned_renderbuffers.end()) {
            // Renderbuffer attachment (e.g. the lighting FBO depth buffer).
            glFramebufferRenderbuffer(GL_FRAMEBUFFER,
                                      static_cast<GLenum>(attachment.attachment_point),
                                      GL_RENDERBUFFER,
                                      rb_it->second.gl_id);
            continue;
        }
        LUMINUMBRA_CORE_ERROR(
            "RenderResourceRegistry: FBO attachment references unknown owned resource '{}'",
            attachment.texture_name);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        return false;
    }
    if (fbo_entry.desc.no_color) {
        glDrawBuffer(GL_NONE);
        glReadBuffer(GL_NONE);
    } else if (!fbo_entry.desc.draw_buffers.empty()) {
        glDrawBuffers(static_cast<GLsizei>(fbo_entry.desc.draw_buffers.size()),
                      reinterpret_cast<const GLenum*>(fbo_entry.desc.draw_buffers.data()));
    }
    const GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    if (status != GL_FRAMEBUFFER_COMPLETE) {
        LUMINUMBRA_CORE_ERROR("RenderResourceRegistry: owned FBO incomplete (status 0x{:x})",
                              static_cast<unsigned>(status));
        return false;
    }
    return true;
}

FboHandle RenderResourceRegistry::create_fbo(std::string_view name, const FboDesc& desc) {
    const std::string key(name);
    if (m_owned_fbos.count(key) != 0) {
        LUMINUMBRA_CORE_ERROR("RenderResourceRegistry: owned FBO '{}' already exists", key);
        return FboHandle{};
    }
    OwnedFbo owned;
    owned.desc = desc;
    glGenFramebuffers(1, &owned.gl_id);
    if (!attach_fbo(owned)) {
        glDeleteFramebuffers(1, &owned.gl_id);
        return FboHandle{};
    }
    if (desc.debug_label != nullptr) {
        PassGl::label_gl_object(GL_FRAMEBUFFER, owned.gl_id, desc.debug_label);
    }
    const FboHandle handle{owned.gl_id, true};
    m_owned_fbos.emplace(key, std::move(owned));
    return handle;
}

bool RenderResourceRegistry::resize_texture(std::string_view name, u32 new_width, u32 new_height) {
    const auto it = m_owned_textures.find(std::string(name));
    if (it == m_owned_textures.end()) {
        return false;
    }
    OwnedTexture& owned = it->second;
    auto replacement_desc = owned.desc;
    replacement_desc.width = new_width;
    replacement_desc.height = new_height;
    // Recreate storage in place (same GL name — attachments referencing the id
    // still need re-attachment because glTexImage respecifies the storage, and
    // completeness must be re-verified at the new size).
    if (!allocate_texture_storage(owned.gl_id, replacement_desc)) {
        LUMINUMBRA_CORE_ERROR("RenderResourceRegistry: resize storage failed for '{}'", it->first);
        return false;
    }
    owned.desc = replacement_desc;
    owned.allocation_bytes = texture_allocation_bytes(owned.desc);
    bool ok = true;
    for (auto& [fbo_name, fbo_entry] : m_owned_fbos) {
        for (const FboAttachment& attachment : fbo_entry.desc.attachments) {
            if (attachment.texture_name == it->first) {
                ok = attach_fbo(fbo_entry) && ok;
                break;
            }
        }
    }
    return ok;
}

void RenderResourceRegistry::destroy_owned(std::string_view name) {
    const std::string key(name);
    if (const auto fbo_it = m_owned_fbos.find(key); fbo_it != m_owned_fbos.end()) {
        glDeleteFramebuffers(1, &fbo_it->second.gl_id);
        m_owned_fbos.erase(fbo_it);
    }
    if (const auto tex_it = m_owned_textures.find(key); tex_it != m_owned_textures.end()) {
        glDeleteTextures(1, &tex_it->second.gl_id);
        m_owned_textures.erase(tex_it);
    }
    if (const auto rb_it = m_owned_renderbuffers.find(key); rb_it != m_owned_renderbuffers.end()) {
        glDeleteRenderbuffers(1, &rb_it->second.gl_id);
        m_owned_renderbuffers.erase(rb_it);
    }
}

void RenderResourceRegistry::destroy_all_owned() {
    for (auto& [name, fbo_entry] : m_owned_fbos) {
        (void)name;
        glDeleteFramebuffers(1, &fbo_entry.gl_id);
    }
    m_owned_fbos.clear();
    for (auto& [name, tex_entry] : m_owned_textures) {
        (void)name;
        glDeleteTextures(1, &tex_entry.gl_id);
    }
    m_owned_textures.clear();
    for (auto& [name, rb_entry] : m_owned_renderbuffers) {
        (void)name;
        glDeleteRenderbuffers(1, &rb_entry.gl_id);
    }
    m_owned_renderbuffers.clear();
}

} // namespace Luminumbra::Rendering
