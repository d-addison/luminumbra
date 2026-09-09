#include "ImpostorBake.h"

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
constexpr glm::vec3 kPreviewBackground(1.0f, 0.0f, 1.0f); // Diagnostic export only.

GLuint CompileBakeProgram(std::string& err) {
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
        glCompileShader(s);
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
        return 0;
    GLuint fs = compile(GL_FRAGMENT_SHADER, kFrag);
    if (!fs) {
        glDeleteShader(vs);
        return 0;
    }
    GLuint prog = glCreateProgram();
    glAttachShader(prog, vs);
    glAttachShader(prog, fs);
    glLinkProgram(prog);
    glDeleteShader(vs);
    glDeleteShader(fs);
    GLint ok = 0;
    glGetProgramiv(prog, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[512];
        glGetProgramInfoLog(prog, sizeof(log), nullptr, log);
        err = log;
        glDeleteProgram(prog);
        return 0;
    }
    return prog;
}

bool WritePpm(const std::string& path, int w, int h, const std::vector<unsigned char>& rgb) {
    std::ofstream f(path, std::ios::binary);
    if (!f)
        return false;
    f << "P6\n" << w << ' ' << h << "\n255\n";
    f.write(reinterpret_cast<const char*>(rgb.data()), static_cast<std::streamsize>(rgb.size()));
    return static_cast<bool>(f);
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
};
RenderedAtlas
RenderAtlas(const std::string& rootDir, const RenderPipeline& rp, const OctaImpostorGrid& grid) {
    RenderedAtlas out;
    const int n = std::max(1, grid.gridResolution);
    const int tile = std::max(1, grid.tileResolution);
    const int atlas = n * tile;
    out.size = atlas;

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
    const GLuint prog = CompileBakeProgram(err);
    if (!prog) {
        out.err = "bake shader: " + err;
        return out;
    }

    GLuint fbo = 0, depthRb = 0;
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    auto makeColor = [&](GLuint& tex, int attach) {
        glGenTextures(1, &tex);
        glBindTexture(GL_TEXTURE_2D, tex);
        glTexImage2D(
            GL_TEXTURE_2D, 0, GL_RGBA8, atlas, atlas, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
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
        glDeleteProgram(prog);
        out.albedo = out.normal = 0;
        return out;
    }

    glUseProgram(prog);
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
                glDrawElements(GL_TRIANGLES,
                               static_cast<GLsizei>(lp.mesh->indexCount),
                               GL_UNSIGNED_INT,
                               nullptr);
            }
        }
    }
    glBindVertexArray(0);
    glDeleteFramebuffers(1, &fbo);
    glDeleteRenderbuffers(1, &depthRb);
    glDeleteProgram(prog);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    out.ok = true;
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
    std::vector<unsigned char> rgb(static_cast<std::size_t>(atlas) * atlas * 3u);
    std::vector<unsigned char> rgba(static_cast<std::size_t>(atlas) * atlas * 4u);
    std::vector<unsigned char> nrm(static_cast<std::size_t>(atlas) * atlas * 3u);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glBindTexture(GL_TEXTURE_2D, ra.albedo);
    glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
    glBindTexture(GL_TEXTURE_2D, ra.normal);
    glGetTexImage(GL_TEXTURE_2D, 0, GL_RGB, GL_UNSIGNED_BYTE, nrm.data());

    for (std::size_t pixel = 0; pixel < rgba.size() / 4; ++pixel) {
        for (std::size_t channel = 0; channel < 3; ++channel) {
            rgb[pixel * 3 + channel] =
                rgba[pixel * 4 + 3] > 0
                    ? rgba[pixel * 4 + channel]
                    : static_cast<unsigned char>(kPreviewBackground[channel] * 255.0f);
        }
    }
    std::vector<float> tileCov(static_cast<std::size_t>(n) * n, 0.0f);
    double covSum = 0.0;
    out.min_coverage = 1.0f;
    for (int tj = 0; tj < n; ++tj)
        for (int ti = 0; ti < n; ++ti) {
            long covered = 0;
            for (int py = 0; py < tile; ++py)
                for (int px = 0; px < tile; ++px) {
                    const std::size_t idx =
                        (static_cast<std::size_t>(tj * tile + py) * atlas + (ti * tile + px)) * 4u;
                    if (rgba[idx + 3] >= 128)
                        ++covered;
                }
            const float cov = static_cast<float>(covered) / static_cast<float>(tile * tile);
            tileCov[static_cast<std::size_t>(tj) * n + ti] = cov;
            covSum += cov;
            out.min_coverage = glm::min(out.min_coverage, cov);
        }
    out.mean_coverage = static_cast<float>(covSum / (static_cast<double>(n) * n));

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
            for (std::size_t k = 0; k < tileCov.size(); ++k) {
                if (k)
                    jf << ", ";
                jf << tileCov[k];
            }
            jf << "]\n}\n";
        }
    }
    glBindTexture(GL_TEXTURE_2D, 0);
    glDeleteTextures(1, &ra.albedo);
    glDeleteTextures(1, &ra.normal);
    return out;
}

ImpostorAtlasTextures BakeTreeImpostorAtlasToTextures(const std::string& rootDir,
                                                      const RenderPipeline& rp,
                                                      const OctaImpostorGrid& grid) {
    ImpostorAtlasTextures out;
    RenderedAtlas ra = RenderAtlas(rootDir, rp, grid);
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
