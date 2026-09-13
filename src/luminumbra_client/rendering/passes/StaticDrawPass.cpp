#include "StaticDrawPass.h"
#include "../GBuffer.h"
#include "../Shader.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <glad/glad.h>
#include <glm/glm.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <luminumbra/rendering/RenderView.h>
#include <luminumbra/rendering/StaticScene.h>
#include <map>
#include <set>
#include <stdexcept>
#include <tuple>

namespace Luminumbra::Rendering {
namespace {
void Require(bool condition, const char* message) {
    if (!condition)
        throw std::invalid_argument(message);
}
struct MeshObject {
    GLuint vao = 0, vbo = 0, ebo = 0;
    GLsizei count = 0;
    std::shared_ptr<const StaticMesh> source;
    ~MeshObject() {
        if (vao)
            glDeleteVertexArrays(1, &vao);
        if (vbo)
            glDeleteBuffers(1, &vbo);
        if (ebo)
            glDeleteBuffers(1, &ebo);
    }
};
struct TextureObject {
    GLuint name = 0;
    std::shared_ptr<const StaticTexture> source;
    ~TextureObject() {
        if (name)
            glDeleteTextures(1, &name);
    }
};
struct GlState {
    GLint program{}, vao{}, framebuffer{}, depth_func{}, front_face{}, cull_face{}, active{},
        array_buffer{}, unpack_buffer{};
    GLboolean depth{}, cull{}, depth_mask{}, blend{}, framebuffer_srgb{};
    static constexpr std::array<GLenum, 8> unpack_names{GL_UNPACK_ALIGNMENT,
                                                        GL_UNPACK_ROW_LENGTH,
                                                        GL_UNPACK_SKIP_ROWS,
                                                        GL_UNPACK_SKIP_PIXELS,
                                                        GL_UNPACK_IMAGE_HEIGHT,
                                                        GL_UNPACK_SKIP_IMAGES,
                                                        GL_UNPACK_SWAP_BYTES,
                                                        GL_UNPACK_LSB_FIRST};
    std::array<GLint, 8> unpack_values{};
    std::array<GLint, 5> textures{}, samplers{};
    GlState() {
        glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &array_buffer);
        glGetIntegerv(GL_PIXEL_UNPACK_BUFFER_BINDING, &unpack_buffer);
        for (size_t i = 0; i < unpack_names.size(); ++i) {
            glGetIntegerv(unpack_names[i], &unpack_values[i]);
            glPixelStorei(unpack_names[i], i == 0 ? 1 : 0);
        }
        glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
        glGetIntegerv(GL_CURRENT_PROGRAM, &program);
        glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &vao);
        glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &framebuffer);
        glGetIntegerv(GL_DEPTH_FUNC, &depth_func);
        glGetIntegerv(GL_FRONT_FACE, &front_face);
        glGetIntegerv(GL_CULL_FACE_MODE, &cull_face);
        glGetIntegerv(GL_ACTIVE_TEXTURE, &active);
        glGetBooleanv(GL_DEPTH_WRITEMASK, &depth_mask);
        depth = glIsEnabled(GL_DEPTH_TEST);
        cull = glIsEnabled(GL_CULL_FACE);
        blend = glIsEnabled(GL_BLEND);
        framebuffer_srgb = glIsEnabled(GL_FRAMEBUFFER_SRGB);
        for (GLuint i = 0; i < textures.size(); ++i) {
            glActiveTexture(GL_TEXTURE0 + i);
            glGetIntegerv(GL_TEXTURE_BINDING_2D, &textures[i]);
            glGetIntegeri_v(GL_SAMPLER_BINDING, i, &samplers[i]);
        }
    }
    ~GlState() {
        for (GLuint i = 0; i < textures.size(); ++i) {
            glActiveTexture(GL_TEXTURE0 + i);
            glBindTexture(GL_TEXTURE_2D, textures[i]);
            glBindSampler(i, samplers[i]);
        }
        glActiveTexture(active);
        glUseProgram(program);
        glBindVertexArray(vao);
        glBindBuffer(GL_ARRAY_BUFFER, array_buffer);
        glBindBuffer(GL_PIXEL_UNPACK_BUFFER, unpack_buffer);
        for (size_t i = 0; i < unpack_names.size(); ++i)
            glPixelStorei(unpack_names[i], unpack_values[i]);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, framebuffer);
        glDepthFunc(depth_func);
        glFrontFace(front_face);
        glCullFace(cull_face);
        glDepthMask(depth_mask);
        if (depth)
            glEnable(GL_DEPTH_TEST);
        else
            glDisable(GL_DEPTH_TEST);
        if (cull)
            glEnable(GL_CULL_FACE);
        else
            glDisable(GL_CULL_FACE);
        if (blend)
            glEnable(GL_BLEND);
        else
            glDisable(GL_BLEND);
        if (framebuffer_srgb)
            glEnable(GL_FRAMEBUFFER_SRGB);
        else
            glDisable(GL_FRAMEBUFFER_SRGB);
    }
};
using Key = std::tuple<std::string, std::string, std::uint32_t>;
using SamplerKey = std::tuple<std::uint32_t, std::uint32_t, std::uint32_t, std::uint32_t>;
float Value(double value) {
    Require(std::isfinite(value) && std::abs(value) <= 1e6,
            "Static draw scalar outside GPU profile bounds");
    return static_cast<float>(value);
}
} // namespace

struct StaticDrawPass::Impl {
    explicit Impl(const std::filesystem::path& root)
        : shader((root / "res/shaders/static_asset.vert").string().c_str(),
                 (root / "res/shaders/static_asset.frag").string().c_str()) {
        if (!shader.IsValid())
            throw std::runtime_error("Static material shader failed to compile/link");
        ExpectedLayout layout;
        layout.pass_name = "gbuffer_authored";
        for (const char* name :
             {"u_baseColor", "u_metallicRoughness", "u_normalMap", "u_occlusion", "u_emissive"}) {
            const auto* reflected = shader.Reflected().find_sampler(name);
            Require(reflected && reflected->type == GL_SAMPLER_2D,
                    "Static material sampler missing or wrong type");
            layout.samplers.push_back({name, GL_SAMPLER_2D, -1});
        }
        Require(shader.ValidateLayout(layout), "Static material sampler layout mismatch");
        const std::array<const char*, 6> outputs{"gPosition",
                                                 "gNormalMaterial",
                                                 "gAlbedoRoughness",
                                                 "gMetallicAO",
                                                 "gMotionVector",
                                                 "gAuthoredSurface"};
        for (size_t i = 0; i < outputs.size(); ++i)
            Require(glGetFragDataLocation(shader.Id(), outputs[i]) == static_cast<GLint>(i),
                    "Static material attachment output missing or wrong location");
        GlState state;
        glGenTextures(1, &white);
        glBindTexture(GL_TEXTURE_2D, white);
        const std::array<unsigned char, 4> rgba{255, 255, 255, 255};
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
    }
    ~Impl() {
        for (const auto& [key, sampler] : samplers) {
            (void)key;
            glDeleteSamplers(1, &sampler);
        }
        if (white)
            glDeleteTextures(1, &white);
    }
    Shader shader;
    GLuint white = 0;
    std::map<std::string, std::shared_ptr<MeshObject>> meshes;
    std::map<std::string, std::shared_ptr<TextureObject>> textures;
    std::map<SamplerKey, GLuint> samplers;
    std::shared_ptr<const StaticDrawSnapshot> previous;

    std::shared_ptr<MeshObject> Mesh(const std::shared_ptr<const StaticMesh>& mesh,
                                     StaticDrawStats& stats) {
        Require(mesh && !mesh->identity.empty(), "Missing immutable static mesh identity");
        if (const auto found = meshes.find(mesh->identity); found != meshes.end()) {
            const auto& old = *found->second->source;
            Require(found->second->source == mesh ||
                        (old.vertices.size() == mesh->vertices.size() &&
                         old.indices == mesh->indices &&
                         std::memcmp(old.vertices.data(),
                                     mesh->vertices.data(),
                                     old.vertices.size() * sizeof(StaticVertex)) == 0),
                    "Static mesh identity reused for different bytes");
            return found->second;
        }
        Require(!mesh->vertices.empty() && mesh->vertices.size() <= 4000000 &&
                    !mesh->indices.empty() && mesh->indices.size() <= 12000000 &&
                    mesh->indices.size() % 3 == 0,
                "Unsupported static mesh extent");
        for (const auto& v : mesh->vertices) {
            for (const auto x : v.position)
                Value(x);
            for (const auto x : v.normal)
                Value(x);
            for (const auto x : v.uv)
                Value(x);
            Require(glm::length(glm::make_vec3(v.normal.data())) > 1e-8f,
                    "Static mesh has zero normal");
        }
        for (auto index : mesh->indices)
            Require(index < mesh->vertices.size(), "Static mesh index outside vertices");
        static_assert(sizeof(StaticVertex) == 32);
        auto result = std::make_shared<MeshObject>();
        result->source = mesh;
        result->count = static_cast<GLsizei>(mesh->indices.size());
        glGenVertexArrays(1, &result->vao);
        glBindVertexArray(result->vao);
        glGenBuffers(1, &result->vbo);
        glBindBuffer(GL_ARRAY_BUFFER, result->vbo);
        glBufferData(GL_ARRAY_BUFFER,
                     mesh->vertices.size() * sizeof(StaticVertex),
                     mesh->vertices.data(),
                     GL_STATIC_DRAW);
        glGenBuffers(1, &result->ebo);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, result->ebo);
        glBufferData(GL_ELEMENT_ARRAY_BUFFER,
                     mesh->indices.size() * sizeof(std::uint32_t),
                     mesh->indices.data(),
                     GL_STATIC_DRAW);
        for (GLuint attribute = 0; attribute < 3; ++attribute) {
            glEnableVertexAttribArray(attribute);
            const size_t offset = attribute == 0 ? 0 : attribute == 1 ? 12 : 24;
            glVertexAttribPointer(attribute,
                                  attribute == 2 ? 2 : 3,
                                  GL_FLOAT,
                                  GL_FALSE,
                                  sizeof(StaticVertex),
                                  reinterpret_cast<void*>(offset));
        }
        Require(glGetError() == GL_NO_ERROR, "Static mesh GPU allocation/upload failed");
        meshes.emplace(mesh->identity, result);
        ++stats.uploaded_meshes;
        return result;
    }
    GLuint Texture(const std::shared_ptr<const StaticTexture>& texture, StaticDrawStats& stats) {
        if (!texture)
            return white;
        Require(!texture->identity.empty() && !texture->mips.empty(),
                "Missing immutable static texture data");
        if (const auto found = textures.find(texture->identity); found != textures.end()) {
            const auto& old = *found->second->source;
            Require(old.encoding == texture->encoding && old.mips.size() == texture->mips.size(),
                    "Static texture identity reused with a different format");
            if (found->second->source != texture)
                for (size_t i = 0; i < old.mips.size(); ++i)
                    Require(old.mips[i].width == texture->mips[i].width &&
                                old.mips[i].height == texture->mips[i].height &&
                                old.mips[i].rgba8 == texture->mips[i].rgba8,
                            "Static texture identity reused for different bytes");
            return found->second->name;
        }
        Require(texture->encoding == StaticEncoding::Linear ||
                    texture->encoding == StaticEncoding::Srgb,
                "Unsupported static texture encoding");
        Require(texture->mips.size() <= 15, "Too many static texture mips");
        auto result = std::make_shared<TextureObject>();
        result->source = texture;
        glGenTextures(1, &result->name);
        glBindTexture(GL_TEXTURE_2D, result->name);
        std::uint32_t width = texture->mips.front().width, height = texture->mips.front().height;
        Require(width > 0 && height > 0 && width <= 16384 && height <= 16384,
                "Unsupported static texture extent");
        for (size_t i = 0; i < texture->mips.size(); ++i) {
            const auto& mip = texture->mips[i];
            Require(mip.width == width && mip.height == height &&
                        mip.rgba8.size() == 4ull * width * height,
                    "Static texture mip extent mismatch");
            glTexImage2D(GL_TEXTURE_2D,
                         static_cast<GLint>(i),
                         texture->encoding == StaticEncoding::Srgb ? GL_SRGB8_ALPHA8 : GL_RGBA8,
                         width,
                         height,
                         0,
                         GL_RGBA,
                         GL_UNSIGNED_BYTE,
                         mip.rgba8.data());
            if (i + 1 == texture->mips.size())
                Require(width == 1 && height == 1, "Incomplete static texture mip chain");
            else
                Require(width != 1 || height != 1, "Duplicate terminal static texture mip");
            width = std::max(1u, width / 2);
            height = std::max(1u, height / 2);
        }
        glTexParameteri(
            GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, static_cast<GLint>(texture->mips.size() - 1));
        Require(glGetError() == GL_NO_ERROR, "Static texture GPU allocation/upload failed");
        textures.emplace(texture->identity, result);
        ++stats.uploaded_textures;
        return result->name;
    }
    GLuint Sampler(const StaticSampler& sampler) {
        const SamplerKey key{
            sampler.wrap_s, sampler.wrap_t, sampler.min_filter, sampler.mag_filter};
        if (const auto found = samplers.find(key); found != samplers.end())
            return found->second;
        const auto wrap = [](auto value) {
            return value == GL_REPEAT || value == GL_CLAMP_TO_EDGE || value == GL_MIRRORED_REPEAT;
        };
        Require(wrap(sampler.wrap_s) && wrap(sampler.wrap_t), "Unsupported static sampler wrap");
        Require(sampler.mag_filter == GL_NEAREST || sampler.mag_filter == GL_LINEAR,
                "Unsupported static sampler mag filter");
        Require(sampler.min_filter == GL_NEAREST || sampler.min_filter == GL_LINEAR ||
                    (sampler.min_filter >= GL_NEAREST_MIPMAP_NEAREST &&
                     sampler.min_filter <= GL_LINEAR_MIPMAP_LINEAR),
                "Unsupported static sampler min filter");
        GLuint result;
        glGenSamplers(1, &result);
        glSamplerParameteri(result, GL_TEXTURE_WRAP_S, sampler.wrap_s);
        glSamplerParameteri(result, GL_TEXTURE_WRAP_T, sampler.wrap_t);
        glSamplerParameteri(result, GL_TEXTURE_MIN_FILTER, sampler.min_filter);
        glSamplerParameteri(result, GL_TEXTURE_MAG_FILTER, sampler.mag_filter);
        samplers.emplace(key, result);
        return result;
    }
};
StaticDrawPass::StaticDrawPass(const std::filesystem::path& root)
    : m_impl(std::make_unique<Impl>(root)) {}
StaticDrawPass::~StaticDrawPass() = default;

StaticDrawStats StaticDrawPass::Render(const RenderView& view,
                                       std::shared_ptr<const StaticDrawSnapshot> snapshot,
                                       const GBuffer& target,
                                       bool temporal_enabled) {
    Require(!temporal_enabled,
            "Static inspection draws have no temporal history; TAAU must be disabled");
    Require(snapshot && snapshot->draws.size() <= 65536,
            "Missing or oversized static draw snapshot");
    Require(target.fbo_id && target.authored_texture,
            "Static draws require the authored G-buffer attachment");
    std::set<std::string> budget_meshes, budget_textures;
    std::uint64_t decoded_bytes = 0;
    for (const auto& draw : snapshot->draws) {
        Require(draw.mesh && draw.material, "Missing static mesh/material binding");
        if (budget_meshes.insert(draw.mesh->identity).second)
            decoded_bytes +=
                static_cast<std::uint64_t>(draw.mesh->vertices.size()) * sizeof(StaticVertex) +
                static_cast<std::uint64_t>(draw.mesh->indices.size()) * sizeof(std::uint32_t);
        for (const auto& binding : draw.material->textures)
            if (binding.texture && budget_textures.insert(binding.texture->identity).second)
                for (const auto& mip : binding.texture->mips)
                    decoded_bytes += mip.rgba8.size();
        Require(decoded_bytes <= 512ull * 1024 * 1024, "Static draw resource set exceeds 512 MiB");
    }
    GlState state;
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, target.fbo_id);
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_GREATER);
    glDepthMask(GL_TRUE);
    glDisable(GL_BLEND);
    glDisable(GL_FRAMEBUFFER_SRGB);
    glCullFace(GL_BACK);
    auto& impl = *m_impl;
    auto& shader = impl.shader;
    shader.use();
    shader.setMat4("u_view", glm::make_mat4(view.view().data()));
    shader.setMat4("u_projection", glm::make_mat4(view.projection().data()));
    shader.setMat3("u_normalView", glm::mat3(glm::make_mat4(view.view().data())));
    const std::array<const char*, 5> sampler_names{
        "u_baseColor", "u_metallicRoughness", "u_normalMap", "u_occlusion", "u_emissive"};
    for (size_t i = 0; i < sampler_names.size(); ++i)
        shader.setInt(sampler_names[i], static_cast<int>(i));
    StaticDrawStats stats;
    std::map<Key, const StaticDraw*> previous;
    if (impl.previous && impl.previous != snapshot)
        for (const auto& draw : impl.previous->draws)
            previous.emplace(Key{draw.key.instance_id, draw.key.node_id, draw.key.primitive_index},
                             &draw);
    std::set<Key> keys;
    std::set<std::string> used_meshes, used_textures;
    for (const auto& draw : snapshot->draws) {
        const Key key{draw.key.instance_id, draw.key.node_id, draw.key.primitive_index};
        Require(keys.insert(key).second && draw.material,
                "Duplicate draw identity or missing material");
        if (impl.previous != snapshot) {
            const auto found = previous.find(key);
            if (found == previous.end() || found->second->model != draw.model ||
                found->second->material != draw.material || found->second->mesh != draw.mesh)
                ++stats.updated_instances;
        }
        for (auto value : draw.model)
            Value(value);
        for (auto value : draw.normal)
            Value(value);
        const auto mesh = impl.Mesh(draw.mesh, stats);
        used_meshes.insert(draw.mesh->identity);
        const auto& material = *draw.material;
        Require(material.alpha_mode == StaticAlphaMode::Opaque ||
                    material.alpha_mode == StaticAlphaMode::Mask,
                "Unsupported static alpha mode");
        for (auto value : material.base_color)
            Require(Value(value) >= 0 && value <= 1, "Invalid base color factor");
        for (auto value : material.emissive)
            Require(Value(value) >= 0 && value <= 1, "Invalid emissive factor");
        for (auto value : {material.metallic,
                           material.roughness,
                           material.occlusion_strength,
                           material.alpha_cutoff})
            Require(Value(value) >= 0 && value <= 1, "Invalid static material scalar");
        Require(Value(material.normal_scale) >= 0, "Invalid normal scale");
        const glm::dmat4 world = glm::make_mat4(draw.model.data());
        const double determinant = glm::determinant(glm::dmat3(world));
        Require(world[0][3] == 0 && world[1][3] == 0 && world[2][3] == 0 && world[3][3] == 1 &&
                    std::abs(determinant) > 1e-12 && (determinant < 0) == draw.reverse_front_face,
                "Invalid static affine matrix or mirrored winding");
        const glm::dmat3 normal = glm::transpose(glm::inverse(glm::dmat3(world)));
        for (size_t i = 0; i < 9; ++i)
            Require(std::abs(glm::value_ptr(normal)[i] - draw.normal[i]) <=
                        1e-6 * std::max(1.0, std::abs(glm::value_ptr(normal)[i])),
                    "Static inverse-transpose normal differs from the full affine matrix");
        shader.setMat4("u_model", glm::mat4(glm::make_mat4(draw.model.data())));
        shader.setMat3("u_normal", glm::mat3(glm::make_mat3(draw.normal.data())));
        shader.setVec4("u_baseFactor", glm::vec4(glm::make_vec4(material.base_color.data())));
        shader.setVec3("u_emissiveFactor", glm::vec3(glm::make_vec3(material.emissive.data())));
        shader.setFloat("u_metallicFactor", Value(material.metallic));
        shader.setFloat("u_roughnessFactor", Value(material.roughness));
        shader.setFloat("u_normalScale", Value(material.normal_scale));
        shader.setFloat("u_occlusionStrength", Value(material.occlusion_strength));
        shader.setFloat("u_alphaCutoff", Value(material.alpha_cutoff));
        shader.setInt("u_mask", material.alpha_mode == StaticAlphaMode::Mask ? 1 : 0);
        shader.setInt("u_doubleSided", material.double_sided ? 1 : 0);
        shader.setInt("u_hasNormal", material.textures[2].texture ? 1 : 0);
        for (size_t i = 0; i < material.textures.size(); ++i) {
            const auto& binding = material.textures[i];
            glActiveTexture(GL_TEXTURE0 + static_cast<GLenum>(i));
            glBindTexture(GL_TEXTURE_2D, impl.Texture(binding.texture, stats));
            glBindSampler(static_cast<GLuint>(i), impl.Sampler(binding.sampler));
            if (binding.texture)
                used_textures.insert(binding.texture->identity);
            const float cosine = std::cos(Value(binding.rotation)),
                        sine = std::sin(Value(binding.rotation));
            const float x = Value(binding.scale[0]), y = Value(binding.scale[1]);
            const glm::mat3 uv(cosine * x,
                               sine * x,
                               0,
                               -sine * y,
                               cosine * y,
                               0,
                               Value(binding.offset[0]),
                               Value(binding.offset[1]),
                               1);
            shader.setMat3("u_uv[" + std::to_string(i) + "]", uv);
        }
        glFrontFace(draw.reverse_front_face ? GL_CW : GL_CCW);
        if (material.double_sided)
            glDisable(GL_CULL_FACE);
        else
            glEnable(GL_CULL_FACE);
        glBindVertexArray(mesh->vao);
        glDrawElements(GL_TRIANGLES, mesh->count, GL_UNSIGNED_INT, nullptr);
        ++stats.draws;
        stats.indices += static_cast<size_t>(mesh->count);
    }
    Require(glGetError() == GL_NO_ERROR, "Static production draw failed");
    impl.previous = std::move(snapshot);
    std::erase_if(impl.meshes, [&](const auto& pair) { return !used_meshes.contains(pair.first); });
    std::erase_if(impl.textures,
                  [&](const auto& pair) { return !used_textures.contains(pair.first); });
    return stats;
}
} // namespace Luminumbra::Rendering
