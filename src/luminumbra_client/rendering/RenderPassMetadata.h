#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace Luminumbra::Rendering {
struct RenderPassMetadata {
    std::string name;
    std::vector<std::string> inputs;
    std::vector<std::string> outputs;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::string clear;
    std::string load_store;
    std::size_t draw_count = 0;
    std::size_t dispatch_count = 0;
};

class RenderPassMetadataCache {
public:
    static constexpr std::size_t kPassCount = 9;
    void Refresh(std::uint32_t width,
                 std::uint32_t height,
                 std::uint32_t shadow_resolution,
                 bool authored,
                 const std::array<std::size_t, kPassCount>& draws) {
        if (passes_.empty()) {
            BuildSchema();
        }
        if (authored != authored_) {
            std::swap(passes_[1], alternate_gbuffer_);
            authored_ = authored;
        }
        // Exactly nine passes; refresh only frame-varying scalar values.
        for (std::size_t i = 0; i < kPassCount; ++i) {
            auto& pass = passes_[i];
            pass.width = i == 0 ? shadow_resolution : width;
            pass.height = i == 0 ? shadow_resolution : height;
            pass.draw_count = draws[i];
            pass.dispatch_count = 0;
        }
    }
    const std::vector<RenderPassMetadata>& passes() const noexcept {
        return passes_;
    }
    // Render-only work counter; no schema construction during warmed frames.
    std::size_t schema_build_count() const noexcept {
        return schema_build_count_;
    }
    void Clear() {
        passes_.clear();
        alternate_gbuffer_ = {};
        authored_ = false;
    }

private:
    void BuildSchema() {
        // Fixed nine-pass schema plus one retained authored G-buffer variant.
        std::vector<RenderPassMetadata> schema{
            {"shadow",
             {"terrain_depth"},
             {"shadow.depth_texture_array"},
             0,
             0,
             "depth",
             "store depth cascades",
             0,
             0},
            {"gbuffer",
             {"terrain_meshes",
              "farlod_region_meshes",
              "static_meshes",
              "skinned_meshes",
              "material_lut",
              "terrain_texture_array",
              "terrain_normal_array"},
             {"gbuffer.position",
              "gbuffer.normal_material",
              "gbuffer.albedo_roughness",
              "gbuffer.metallic_ao",
              "gbuffer.depth"},
             0,
             0,
             "color+depth",
             "store deferred attachments",
             0,
             0},
            {"ssao",
             {"gbuffer.position", "gbuffer.normal_material", "ssao.noise"},
             {"ssao.raw"},
             0,
             0,
             "color",
             "store ambient occlusion",
             0,
             0},
            {"ssao_blur",
             {"ssao.raw"},
             {"ssao.blur"},
             0,
             0,
             "color",
             "store blurred ambient occlusion",
             0,
             0},
            {"lighting",
             {"gbuffer.*",
              "shadow.depth_texture_array",
              "ssao.blur",
              "terrain_texture_array",
              "material_lut",
              "lighting.environment_brdf",
              "water.fallback.black"},
             {"lighting.color", "lighting.depth"},
             0,
             0,
             "color+depth",
             "store lit scene",
             0,
             0},
            {"skybox",
             {"skybox_vertices"},
             {"lighting.color"},
             0,
             0,
             "load lighting",
             "store sky contribution",
             0,
             0},
            {"water",
             {"lighting.opaque_color_copy", "gbuffer.depth", "water_meshes", "water.fallback.*"},
             {"lighting.color"},
             0,
             0,
             "load sky+lighting",
             "blend water over sky+lighting",
             0,
             0},
            {"particles",
             {"particle_instances", "gbuffer.depth"},
             {"lighting.color"},
             0,
             0,
             "load lighting",
             "blend forward-lit particles",
             0,
             0},
            {"final_blit",
             {"lighting.color"},
             {"swapchain.color"},
             0,
             0,
             "default color+depth",
             "present-ready color",
             0,
             0}};
        auto authored_gbuffer = schema[1];
        authored_gbuffer.inputs.push_back("authored_static_meshes");
        authored_gbuffer.outputs.push_back("gbuffer.authored_surface");
        passes_ = std::move(schema);
        alternate_gbuffer_ = std::move(authored_gbuffer);
        ++schema_build_count_;
    }
    std::vector<RenderPassMetadata> passes_;
    RenderPassMetadata alternate_gbuffer_;
    bool authored_ = false;
    std::size_t schema_build_count_ = 0;
};
} // namespace Luminumbra::Rendering
