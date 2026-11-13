#include "ImpostorBake.h"
#include "app/FrameAttribution.h"
#include "core/Log.h"
#include "core/Profiler.h"
#include "rendering/AttributedGl.h"

#include "Mesh.h"
#include "RenderPipeline.h"

#include <glad/glad.h>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <cstdio>
#include <fstream>
#include <vector>

namespace Luminumbra::Rendering {

namespace {

struct BakePart {
    const char* path;
};
const BakePart kParts[] = {
    {"game-assets/tree-small-02/1.0.0/data/models/trees/tree_small_02_trunk.lmesh"},
    {"game-assets/tree-small-02/1.0.0/data/models/trees/tree_small_02_branches.lmesh"},
    {"game-assets/tree-small-02/1.0.0/data/models/trees/tree_small_02_leaves.lmesh"},
};
constexpr std::array<unsigned char, 3> kPreviewBackground{255, 0, 255}; // Diagnostic export only.

// Bake/readback is startup or explicitly requested work, never a frame operation.
// Preserve pixel transfer state as well as the bindings changed by mesh upload.
struct BakeGlState {
    GLint program{}, vao{}, array_buffer{}, draw_fbo{}, read_fbo{}, renderbuffer{}, active{};
    GLint depth_func{}, clip_origin{}, clip_depth{}, pack_buffer{}, unpack_buffer{};
    GLboolean depth_mask{};
    std::array<GLint, 4> viewport{};
    std::array<GLfloat, 4> clear_color{};
    std::array<std::array<GLboolean, 4>, 2> color_masks{};
    std::array<GLboolean, 2> blends{};
    GLboolean scissor{};
    GLdouble clear_depth{};
    std::array<GLint, 3> texture_arrays{}, samplers{};
    static constexpr std::array<GLenum, 8> pack_names{GL_PACK_ALIGNMENT,
                                                      GL_PACK_ROW_LENGTH,
                                                      GL_PACK_SKIP_ROWS,
                                                      GL_PACK_SKIP_PIXELS,
                                                      GL_PACK_IMAGE_HEIGHT,
                                                      GL_PACK_SKIP_IMAGES,
                                                      GL_PACK_SWAP_BYTES,
                                                      GL_PACK_LSB_FIRST};
    static constexpr std::array<GLenum, 5> enable_names{
        GL_DEPTH_TEST, GL_CULL_FACE, GL_RASTERIZER_DISCARD, GL_COLOR_LOGIC_OP, GL_FRAMEBUFFER_SRGB};
    std::array<GLint, pack_names.size()> pack_values{};
    std::array<GLboolean, enable_names.size()> enable_values{};

    BakeGlState() {
        glGetIntegerv(GL_CURRENT_PROGRAM, &program);
        glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &vao);
        glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &array_buffer);
        glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &draw_fbo);
        glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &read_fbo);
        glGetIntegerv(GL_RENDERBUFFER_BINDING, &renderbuffer);
        glGetIntegerv(GL_ACTIVE_TEXTURE, &active);
        glGetIntegerv(GL_DEPTH_FUNC, &depth_func);
        glGetIntegerv(GL_CLIP_ORIGIN, &clip_origin);
        glGetIntegerv(GL_CLIP_DEPTH_MODE, &clip_depth);
        glGetBooleanv(GL_DEPTH_WRITEMASK, &depth_mask);
        glGetIntegerv(GL_VIEWPORT, viewport.data());
        glGetFloatv(GL_COLOR_CLEAR_VALUE, clear_color.data());
        for (GLuint attachment = 0; attachment < color_masks.size(); ++attachment) {
            glGetBooleani_v(GL_COLOR_WRITEMASK, attachment, color_masks[attachment].data());
            blends[attachment] = glIsEnabledi(GL_BLEND, attachment);
        }
        scissor = glIsEnabledi(GL_SCISSOR_TEST, 0);
        glGetDoublev(GL_DEPTH_CLEAR_VALUE, &clear_depth);
        glGetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING, &pack_buffer);
        glGetIntegerv(GL_PIXEL_UNPACK_BUFFER_BINDING, &unpack_buffer);
        for (std::size_t i = 0; i < pack_names.size(); ++i) {
            glGetIntegerv(pack_names[i], &pack_values[i]);
            glPixelStorei(pack_names[i], i == 0 ? 1 : 0);
        }
        glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
        glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
        for (std::size_t i = 0; i < enable_names.size(); ++i) {
            enable_values[i] = glIsEnabled(enable_names[i]);
        }
        for (GLuint unit = 0; unit < texture_arrays.size(); ++unit) {
            glActiveTexture(GL_TEXTURE0 + unit);
            glGetIntegerv(GL_TEXTURE_BINDING_2D_ARRAY, &texture_arrays[unit]);
            glGetIntegeri_v(GL_SAMPLER_BINDING, unit, &samplers[unit]);
            glBindSampler(unit, 0);
        }
    }
    ~BakeGlState() {
        for (GLuint unit = 0; unit < texture_arrays.size(); ++unit) {
            glActiveTexture(GL_TEXTURE0 + unit);
            glBindTexture(GL_TEXTURE_2D_ARRAY, static_cast<GLuint>(texture_arrays[unit]));
            glBindSampler(unit, static_cast<GLuint>(samplers[unit]));
        }
        glActiveTexture(static_cast<GLenum>(active));
        AttributedGl::UseProgram(glUseProgram, static_cast<GLuint>(program));
        glBindVertexArray(static_cast<GLuint>(vao));
        glBindBuffer(GL_ARRAY_BUFFER, static_cast<GLuint>(array_buffer));
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, static_cast<GLuint>(draw_fbo));
        glBindFramebuffer(GL_READ_FRAMEBUFFER, static_cast<GLuint>(read_fbo));
        glBindRenderbuffer(GL_RENDERBUFFER, static_cast<GLuint>(renderbuffer));
        glDepthFunc(static_cast<GLenum>(depth_func));
        glDepthMask(depth_mask);
        glClipControl(static_cast<GLenum>(clip_origin), static_cast<GLenum>(clip_depth));
        glViewport(viewport[0], viewport[1], viewport[2], viewport[3]);
        glClearColor(clear_color[0], clear_color[1], clear_color[2], clear_color[3]);
        glClearDepth(clear_depth);
        for (GLuint attachment = 0; attachment < color_masks.size(); ++attachment) {
            const auto& mask = color_masks[attachment];
            glColorMaski(attachment, mask[0], mask[1], mask[2], mask[3]);
            if (blends[attachment]) {
                glEnablei(GL_BLEND, attachment);
            } else {
                glDisablei(GL_BLEND, attachment);
            }
        }
        if (scissor) {
            glEnablei(GL_SCISSOR_TEST, 0);
        } else {
            glDisablei(GL_SCISSOR_TEST, 0);
        }
        for (std::size_t i = 0; i < enable_names.size(); ++i) {
            if (enable_values[i]) {
                glEnable(enable_names[i]);
            } else {
                glDisable(enable_names[i]);
            }
        }
        glBindBuffer(GL_PIXEL_PACK_BUFFER, static_cast<GLuint>(pack_buffer));
        glBindBuffer(GL_PIXEL_UNPACK_BUFFER, static_cast<GLuint>(unpack_buffer));
        for (std::size_t i = 0; i < pack_names.size(); ++i) {
            glPixelStorei(pack_names[i], pack_values[i]);
        }
    }
};

Luminumbra::Rendering::AttributedGl::Program CompileBakeProgram(std::string& err) {
    const Luminumbra::Client::Attribution::PhaseScope shader_phase(
        Luminumbra::Client::Attribution::Phase::ShaderLoad);
    const char* kVert = R"GLSL(#version 450 core
layout(location=0) in vec3 aPos;
layout(location=1) in vec3 aNorm;
layout(location=2) in vec2 aUV;
uniform mat4 uMVP;
out vec2 vUV;
out vec3 vNorm;
out vec3 vPos;
void main() { vUV = aUV; vNorm = aNorm; vPos = aPos; gl_Position = uMVP * vec4(aPos, 1.0); }
)GLSL";
    const char* kFrag = R"GLSL(#version 450 core
in vec2 vUV;
in vec3 vNorm;
in vec3 vPos;
uniform sampler2DArray uTex;
uniform sampler2DArray uNormals;
uniform sampler2DArray uSurface;
uniform int uLayer;
uniform int uNormalLayer;
uniform int uAlphaTest;
uniform int uDoubleSided;
layout(location=0) out vec4 oAlbedo;
layout(location=1) out vec4 oNormal;
vec2 encodeNormal(vec3 n) {
    n /= abs(n.x) + abs(n.y) + abs(n.z);
    vec2 p = n.xy;
    if (n.z < 0.0) p = (1.0 - abs(p.yx)) * (step(0.0, p.xy) * 2.0 - 1.0);
    return p * 0.5 + 0.5;
}
void main() {
    vec4 color = texture(uTex, vec3(vUV, float(uLayer)));
    if (uAlphaTest == 1 && color.a < 0.5) discard;
    vec3 normal = normalize(vNorm);
    vec3 tn = texture(uNormals, vec3(vUV, float(uNormalLayer))).xyz * 2.0 - 1.0;
    vec3 dp1 = dFdx(vPos), dp2 = dFdy(vPos);
    vec2 duv1 = dFdx(vUV), duv2 = dFdy(vUV);
    vec3 perpendicular2 = cross(dp2, normal), perpendicular1 = cross(normal, dp1);
    vec3 tangent = perpendicular2 * duv1.x + perpendicular1 * duv2.x;
    vec3 bitangent = perpendicular2 * duv1.y + perpendicular1 * duv2.y;
    float extent = max(dot(tangent, tangent), dot(bitangent, bitangent));
    if (extent > 1e-12) {
        float scale = inversesqrt(extent);
        normal = normalize(tangent * (tn.x * scale) + bitangent * (tn.y * scale) + normal * tn.z);
    }
    if (uDoubleSided == 1 && !gl_FrontFacing) normal = -normal;
    vec3 surface = texture(uSurface, vec3(vUV, float(uLayer))).rgb;
    // Both atlases have zero outside the silhouette. Coverage in albedo alpha
    // lets the runtime undo the black contribution after bilinear filtering.
    oAlbedo = vec4(color.rgb, 1.0);
    oNormal = vec4(encodeNormal(normal), surface.g, surface.r);
}
)GLSL";
    auto compile = [&](GLenum type, const char* src) -> GLuint {
        GLuint s = glCreateShader(type);
        glShaderSource(s, 1, &src, nullptr);
        {
            LUMIN_PROFILE_ZONE_N("Shader compile");
            glCompileShader(s);
        }
        GLint ok = 0;
        glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
        if (!ok) {
            char log[512];
            glGetShaderInfoLog(s, sizeof(log), nullptr, log);
            err = log;
            glDeleteShader(s);
            return 0;
        }
        return s;
    };
    GLuint vs = compile(GL_VERTEX_SHADER, kVert);
    if (!vs)
        return {};
    GLuint fs = compile(GL_FRAGMENT_SHADER, kFrag);
    if (!fs) {
        glDeleteShader(vs);
        return {};
    }
    auto prog = Luminumbra::Rendering::AttributedGl::CreateProgram(glCreateProgram);
    glAttachShader(prog, vs);
    glAttachShader(prog, fs);
    {
        LUMIN_PROFILE_ZONE_N("Shader link");
        glLinkProgram(prog);
    }
    glDeleteShader(vs);
    glDeleteShader(fs);
    GLint ok = 0;
    glGetProgramiv(prog, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[512];
        glGetProgramInfoLog(prog, sizeof(log), nullptr, log);
        err = log;
        Luminumbra::Rendering::AttributedGl::DeleteProgram(glDeleteProgram, prog);
        return {};
    }
    return prog;
}

bool WritePpm(const std::string& path, int w, int h, const std::vector<unsigned char>& rgb) {
    std::ofstream f(path, std::ios::binary);
    if (!f)
        return false;
    f << "P6\n" << w << ' ' << h << "\n255\n";
    f.write(reinterpret_cast<const char*>(rgb.data()), static_cast<std::streamsize>(rgb.size()));
    f.close();
    return !f.fail();
}

// Shared render core: loads the tree parts, renders all octahedral tiles into two KEPT
// GL_TEXTURE_2D (albedo + normal), and returns them + the local bounding sphere. The caller owns
// the textures.
struct RenderedAtlas {
    bool ok = false;
    GLuint albedo = 0, normal = 0;
    int size = 0;
    glm::vec3 center{0.0f};
    float radius = 0;
    std::string err;
    ImpostorCoverage alpha_coverage;
    std::vector<unsigned char> rgba;
};
RenderedAtlas
RenderAtlas(const std::string& rootDir, const RenderPipeline& rp, const OctaImpostorGrid& grid) {
    RenderedAtlas out;
    const int n = grid.gridResolution;
    const int tile = grid.tileResolution;
    if (n <= 0 || n > kImpostorMaxGrid || tile <= 0 || tile > kImpostorMaxAtlasSide / n) {
        out.err = "invalid impostor atlas dimensions";
        return out;
    }
    const int atlas = n * tile;
    const auto bytes = ImpostorRgbaBytes(atlas, atlas, n);
    // At most 8192^2 RGBA8 texels (256 MiB), shared by validation and export.
    out.rgba.resize(bytes);
    out.size = atlas;
    const BakeGlState gl_state;

    struct LoadedPart {
        std::unique_ptr<Mesh> mesh;
        int layer;
        int alphaTest;
        int normalLayer;
        bool doubleSided;
    };
    std::vector<LoadedPart> parts;
    glm::vec3 unionC(0.0f);
    float unionR = 0.0f;
    bool first = true;
    for (const BakePart& p : kParts) {
        const std::string full = rootDir.empty() ? std::string(p.path) : (rootDir + "/" + p.path);
        std::unique_ptr<Mesh> m = MeshLoader::Load(full);
        if (!m || m->indexCount == 0) {
            out.err = std::string("failed to load tree part: ") + full;
            return out;
        }
        const glm::vec3 c(m->boundingSphere.x, m->boundingSphere.y, m->boundingSphere.z);
        const float r = m->boundingSphere.w;
        if (first) {
            unionC = c;
            unionR = r;
            first = false;
        } else {
            const glm::vec3 mid = (unionC + c) * 0.5f;
            unionR = glm::max(glm::length(mid - unionC) + unionR, glm::length(mid - c) + r);
            unionC = mid;
        }
        const RenderPipeline::StaticModelTex* tex = rp.static_model_tex(p.path);
        if (!tex || tex->albedoLayer < 0 || tex->normalLayer < 0) {
            out.err = std::string("required tree material is unavailable: ") + p.path;
            return out;
        }
        LoadedPart lp;
        lp.mesh = std::move(m);
        lp.layer = tex->albedoLayer;
        lp.normalLayer = tex->normalLayer;
        lp.doubleSided = tex->doubleSided;
        lp.alphaTest = (tex && tex->alphaTest) ? 1 : 0;

        parts.push_back(std::move(lp));
    }
    if (unionR <= 0.0f)
        unionR = 1.0f;
    out.center = unionC;
    out.radius = unionR;

    std::string err;
    const auto prog = CompileBakeProgram(err);
    if (!prog) {
        out.err = "bake shader: " + err;
        return out;
    }

    GLuint fbo = 0, depthRb = 0;
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    auto makeColor = [&](GLuint& tex, GLenum attach) {
        glCreateTextures(GL_TEXTURE_2D, 1, &tex);
        glTextureStorage2D(tex, 1, GL_RGBA8, atlas, atlas);
        glTextureParameteri(tex, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTextureParameteri(tex, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTextureParameteri(tex, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTextureParameteri(tex, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glFramebufferTexture2D(
            GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0 + attach, GL_TEXTURE_2D, tex, 0);
    };
    makeColor(out.albedo, 0);
    makeColor(out.normal, 1);
    const GLenum drawBufs[2] = {GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1};
    glDrawBuffers(2, drawBufs);
    glGenRenderbuffers(1, &depthRb);
    glBindRenderbuffer(GL_RENDERBUFFER, depthRb);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT32F, atlas, atlas);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, depthRb);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        out.err = "impostor atlas FBO incomplete";
        glDeleteFramebuffers(1, &fbo);
        glDeleteTextures(1, &out.albedo);
        glDeleteTextures(1, &out.normal);
        glDeleteRenderbuffers(1, &depthRb);
        Luminumbra::Rendering::AttributedGl::DeleteProgram(glDeleteProgram, prog);
        out.albedo = out.normal = 0;
        return out;
    }

    Luminumbra::Rendering::AttributedGl::UseProgram(glUseProgram, prog);
    const GLint locMVP = glGetUniformLocation(prog, "uMVP");
    const GLint locLayer = glGetUniformLocation(prog, "uLayer");
    const GLint locNormal = glGetUniformLocation(prog, "uNormalLayer");
    const GLint locAlpha = glGetUniformLocation(prog, "uAlphaTest");
    const GLint locDoubleSided = glGetUniformLocation(prog, "uDoubleSided");
    glUniform1i(glGetUniformLocation(prog, "uSurface"), 2);
    glActiveTexture(GL_TEXTURE2);
    glBindTexture(GL_TEXTURE_2D_ARRAY, rp.static_model_surface_array());
    glUniform1i(glGetUniformLocation(prog, "uTex"), 0);
    glUniform1i(glGetUniformLocation(prog, "uNormals"), 1);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D_ARRAY, rp.static_model_normal_array());
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D_ARRAY, rp.static_model_texture_array());
    glClipControl(GL_LOWER_LEFT, GL_ZERO_TO_ONE);
    glClearDepth(0.0);
    glDepthFunc(GL_GREATER);
    glDepthMask(GL_TRUE);
    for (const GLenum capability :
         std::array<GLenum, 3>{GL_RASTERIZER_DISCARD, GL_COLOR_LOGIC_OP, GL_FRAMEBUFFER_SRGB}) {
        glDisable(capability);
    }
    glDisablei(GL_SCISSOR_TEST, 0);
    for (GLuint attachment = 0; attachment < 2; ++attachment) {
        glDisablei(GL_BLEND, attachment);
        glColorMaski(attachment, GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    }
    glEnable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glViewport(0, 0, atlas, atlas);
    glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    const float D = unionR * 2.0f;
    const float hr = unionR; // Match the runtime quad half-size; retain the complete silhouette.
    for (int j = 0; j < n; ++j) {
        for (int i = 0; i < n; ++i) {
            const Vec3f d3 = OctaTileDirection(i, j, grid);
            glm::vec3 dir(d3.x, d3.y, d3.z);
            const glm::vec3 eye = unionC + dir * D;
            const glm::vec3 up =
                (std::fabs(dir.y) > 0.99f) ? glm::vec3(0, 0, 1) : glm::vec3(0, 1, 0);
            const glm::mat4 mvp = glm::orthoRH_ZO(-hr, hr, -hr, hr, D + unionR * 2.0f, 0.01f) *
                                  glm::lookAt(eye, unionC, up);
            glViewport(i * tile, j * tile, tile, tile);
            glUniformMatrix4fv(locMVP, 1, GL_FALSE, &mvp[0][0]);
            for (const LoadedPart& lp : parts) {
                glUniform1i(locLayer, lp.layer);
                glUniform1i(locAlpha, lp.alphaTest);
                glUniform1i(locDoubleSided, lp.doubleSided ? 1 : 0);
                glUniform1i(locNormal, lp.normalLayer);
                glBindVertexArray(lp.mesh->vao);
                Luminumbra::Rendering::AttributedGl::Submit(
                    glDrawElements,
                    static_cast<GLenum>(GL_TRIANGLES),
                    static_cast<GLsizei>(lp.mesh->indexCount),
                    static_cast<GLenum>(GL_UNSIGNED_INT),
                    nullptr);
            }
        }
    }
    glBindVertexArray(0);
    // DSA keeps texture bindings intact. Zero-initialized storage fails closed
    // (empty coverage) if the driver cannot supply the readback.
    glGetTextureImage(
        out.albedo, 0, GL_RGBA, GL_UNSIGNED_BYTE, static_cast<GLsizei>(bytes), out.rgba.data());
    out.alpha_coverage = MeasureImpostorCoverage(out.rgba, atlas, atlas, n);
    glDeleteFramebuffers(1, &fbo);
    glDeleteRenderbuffers(1, &depthRb);
    Luminumbra::Rendering::AttributedGl::DeleteProgram(glDeleteProgram, prog);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    out.ok = out.alpha_coverage.usable();
    if (!out.ok) {
        out.err = std::string("atlas alpha coverage refused: ") + out.alpha_coverage.status();
        LUMINUMBRA_CORE_ERROR(
            "Tree impostor atlas {} refused: alpha coverage {:.6f} ({}); using mesh LODs",
            kTreeImpostorAtlasName,
            out.alpha_coverage.coverage,
            out.alpha_coverage.status());
        glDeleteTextures(1, &out.albedo);
        glDeleteTextures(1, &out.normal);
        out.albedo = out.normal = 0;
    }
    return out;
}

} // namespace

ImpostorBakeResult BakeTreeImpostorAtlas(const std::string& outPpmPath,
                                         const std::string& rootDir,
                                         const RenderPipeline& rp,
                                         const OctaImpostorGrid& grid) {
    ImpostorBakeResult out;
    RenderedAtlas ra = RenderAtlas(rootDir, rp, grid);
    out.atlas_size = ra.size;
    if (!ra.ok) {
        out.error = ra.err;
        return out;
    }

    const int atlas = ra.size;
    const int n = std::max(1, grid.gridResolution);
    const int tile = std::max(1, grid.tileResolution);
    const BakeGlState gl_state;
    const auto& rgba = ra.rgba;
    const auto pixels = rgba.size() / 4u;
    // Export buffers share the validated atlas bound: at most 8192^2 RGB texels each.
    std::vector<unsigned char> rgb(pixels * 3u);
    std::vector<unsigned char> nrm(pixels * 3u);
    glGetTextureImage(
        ra.normal, 0, GL_RGB, GL_UNSIGNED_BYTE, static_cast<GLsizei>(nrm.size()), nrm.data());
    for (std::size_t pixel = 0; pixel < pixels; ++pixel) {
        for (std::size_t channel = 0; channel < 3; ++channel) {
            rgb[pixel * 3 + channel] = rgba[pixel * 4 + 3] >= 128 ? rgba[pixel * 4 + channel]
                                                                  : kPreviewBackground[channel];
        }
    }
    out.min_coverage = static_cast<float>(ra.alpha_coverage.min_cell);
    out.mean_coverage = static_cast<float>(ra.alpha_coverage.coverage);

    std::string normalPath = outPpmPath;
    {
        const auto dot = normalPath.find_last_of('.');
        if (dot != std::string::npos)
            normalPath.insert(dot, "_normal");
        else
            normalPath += "_normal";
    }
    const bool normal_written = WritePpm(normalPath, atlas, atlas, nrm);
    out.ok = WritePpm(outPpmPath, atlas, atlas, rgb) && normal_written;
    if (!out.ok)
        out.error = "failed to write atlas PPM: " + outPpmPath;
    else {
        std::ofstream jf(outPpmPath + ".json");
        if (jf) {
            jf << "{\n  \"atlas_size\": " << atlas << ",\n  \"grid\": " << n
               << ",\n  \"tile\": " << tile << ",\n  \"mean_coverage\": " << out.mean_coverage
               << ",\n  \"min_coverage\": " << out.min_coverage << ",\n  \"tile_coverage\": [";
            for (std::size_t k = 0; k < ra.alpha_coverage.cell_count; ++k) {
                if (k)
                    jf << ", ";
                jf << ra.alpha_coverage.cells[k];
            }
            jf << "]\n}\n";
            jf.close();
        }
        if (!jf) {
            out.ok = false;
            out.error = "failed to write atlas coverage JSON: " + outPpmPath;
        }
    }
    glDeleteTextures(1, &ra.albedo);
    glDeleteTextures(1, &ra.normal);
    return out;
}

ImpostorAtlasTextures BakeTreeImpostorAtlasToTextures(const std::string& rootDir,
                                                      const RenderPipeline& rp,
                                                      const OctaImpostorGrid& grid) {
    ImpostorAtlasTextures out;
    RenderedAtlas ra = RenderAtlas(rootDir, rp, grid);
    out.alpha_coverage = ra.alpha_coverage;
    if (!ra.ok) {
        out.error = ra.err;
        return out;
    }
    out.ok = true;
    out.albedoTex = ra.albedo;
    out.normalTex = ra.normal;
    out.grid = std::max(1, grid.gridResolution);
    out.center = {ra.center.x, ra.center.y, ra.center.z};
    out.radius = ra.radius;
    return out;
}

} // namespace Luminumbra::Rendering
