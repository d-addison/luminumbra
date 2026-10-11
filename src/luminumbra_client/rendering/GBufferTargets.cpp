#include "GBufferTargets.h"
#include "RenderResourceRegistry.h"
#include "core/Log.h"
#include <glad/glad.h>

namespace Luminumbra::Rendering {
void CreateGBufferTargets(
    GBuffer& gbuffer, RenderResourceRegistry& registry, u32 width, u32 height, bool authored) {
    // allocate the FBO + attachments THROUGH the registry.
    // Every desc reproduces the exact GL parameters of the retired
    // glTexImage/glTexParameter calls so the migration is byte-neutral
    // (flip-score-0 on same-pose captures); the struct caches the owned ids.
    const auto color_desc = [&](u32 internal_format, u32 format, u32 type, const char* label) {
        TextureDesc d;
        d.width = width;
        d.height = height;
        d.internal_format = internal_format;
        d.format = format;
        d.type = type;
        d.min_filter = GL_NEAREST;
        d.mag_filter = GL_NEAREST;
        d.expected_layout = "color_attachment";
        d.debug_label = label;
        return d;
    };

    // Authored views support distances beyond binary16's finite 65504 limit.
    // Retain the existing world profile while storing authored positions in full precision.
    gbuffer.position_texture =
        registry
            .create_texture(
                "gbuffer_position",
                color_desc(authored ? GL_RGB32F : GL_RGB16F, GL_RGB, GL_FLOAT, "gbuffer.position"))
            .id;
    // Normal/Material: RGBA8 (octahedral normal + material ID).
    gbuffer.normal_texture =
        registry
            .create_texture(
                "gbuffer_normal",
                color_desc(GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE, "gbuffer.normal_material"))
            .id;
    // Albedo/Roughness: RGBA8.
    gbuffer.albedo_texture =
        registry
            .create_texture(
                "gbuffer_albedo",
                color_desc(GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE, "gbuffer.albedo_roughness"))
            .id;
    // Metallic/AO: RG16F.
    gbuffer.material_texture =
        registry
            .create_texture("gbuffer_material",
                            color_desc(GL_RG16F, GL_RG, GL_FLOAT, "gbuffer.metallic_ao"))
            .id;
    // Motion vectors: RG16F (signed NDC delta).   TAAU foundation.
    gbuffer.motion_vector_texture =
        registry
            .create_texture("gbuffer_motion",
                            color_desc(GL_RG16F, GL_RG, GL_FLOAT, "gbuffer.motion_vectors"))
            .id;

    // Depth: DEPTH_COMPONENT32F, reversed-Z, clamp-to-border with cleared sky depth.
    TextureDesc depth_desc =
        color_desc(GL_DEPTH_COMPONENT32F, GL_DEPTH_COMPONENT, GL_FLOAT, "gbuffer.depth");
    depth_desc.wrap_s = GL_CLAMP_TO_BORDER;
    depth_desc.wrap_t = GL_CLAMP_TO_BORDER;
    depth_desc.has_border_color = true;
    depth_desc.border_color[0] = 0.0f;
    depth_desc.border_color[1] = 0.0f;
    depth_desc.border_color[2] = 0.0f;
    depth_desc.border_color[3] = 0.0f;
    depth_desc.expected_layout = "depth_attachment";
    gbuffer.depth_texture = registry.create_texture("gbuffer_depth", depth_desc).id;

    FboDesc fbo_desc;
    fbo_desc.attachments = {
        {GL_COLOR_ATTACHMENT0, "gbuffer_position"},
        {GL_COLOR_ATTACHMENT1, "gbuffer_normal"},
        {GL_COLOR_ATTACHMENT2, "gbuffer_albedo"},
        {GL_COLOR_ATTACHMENT3, "gbuffer_material"},
        {GL_COLOR_ATTACHMENT4, "gbuffer_motion"},
        {GL_DEPTH_ATTACHMENT, "gbuffer_depth"},
    };
    fbo_desc.draw_buffers = {GL_COLOR_ATTACHMENT0,
                             GL_COLOR_ATTACHMENT1,
                             GL_COLOR_ATTACHMENT2,
                             GL_COLOR_ATTACHMENT3,
                             GL_COLOR_ATTACHMENT4};
    if (authored) {
        gbuffer.authored_texture =
            registry
                .create_texture(
                    "gbuffer_authored",
                    color_desc(GL_RGBA16F, GL_RGBA, GL_FLOAT, "gbuffer.authored_emission"))
                .id;
        fbo_desc.attachments.push_back({GL_COLOR_ATTACHMENT5, "gbuffer_authored"});
        fbo_desc.draw_buffers.push_back(GL_COLOR_ATTACHMENT5);
    }
    fbo_desc.debug_label = "gbuffer.fbo";
    gbuffer.fbo_id = registry.create_fbo("gbuffer_fbo", fbo_desc).id;
    if (gbuffer.fbo_id == 0) {
        LUMINUMBRA_CORE_ERROR("G-Buffer FBO not complete!");
    }
}

void DestroyGBufferTargets(GBuffer& gbuffer, RenderResourceRegistry& registry) {
    // Ownership contract: the registry deletes the owned GL objects.
    registry.destroy_owned("gbuffer_fbo");
    registry.destroy_owned("gbuffer_position");
    registry.destroy_owned("gbuffer_normal");
    registry.destroy_owned("gbuffer_albedo");
    registry.destroy_owned("gbuffer_material");
    registry.destroy_owned("gbuffer_motion");
    registry.destroy_owned("gbuffer_depth");
    registry.destroy_owned("gbuffer_authored");
    gbuffer = GBuffer{};
}

} // namespace Luminumbra::Rendering
