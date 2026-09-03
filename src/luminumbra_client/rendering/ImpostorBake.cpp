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
    glm::vec3 flatColor;
};
const BakePart kParts[] = {
    {"data/models/trees/tree_small_02_trunk.lmesh", glm::vec3(0.30f, 0.19f, 0.10f)},
    {"data/models/trees/tree_small_02_branches.lmesh", glm::vec3(0.32f, 0.20f, 0.10f)},
    {"data/models/trees/tree_small_02_leaves.lmesh", glm::vec3(0.18f, 0.42f, 0.14f)},
};
constexpr glm::vec3 kBackground(1.0f, 0.0f, 1.0f); // magenta key

GLuint CompileBakeProgram(std::string& err) {
    const char* kVert =
        "#version 450 core\n"
        "layout(location=0) in vec3 aPos;\n"
        "layout(location=1) in vec3 aNorm;\n"
        "layout(location=2) in vec2 aUV;\n"
        "uniform mat4 uMVP;\n"
        "out vec2 vUV;\n out vec3 vNorm;\n"
        "void main(){ vUV = aUV; vNorm = aNorm; gl_Position = uMVP * vec4(aPos, 1.0); }\n";
    const char* kFrag =
        "#version 450 core\n"
        "in vec2 vUV;\n in vec3 vNorm;\n"
        "uniform sampler2DArray uTex;\n"
        "uniform int  uLayer;\n uniform vec3 uFlat;\n uniform int uAlphaTest;\n"
        "layout(location=0) out vec4 oAlbedo;\n"
        "layout(location=1) out vec4 oNormal;\n"
        "void main(){\n"
        "  vec3 col;\n"
        "  if (uLayer < 0) { col = uFlat; }\n"
        "  else {\n"
        "    vec3 a = texture(uTex, vec3(vUV, float(uLayer))).rgb;\n"
        "    if (uAlphaTest == 1) { if (dot(a, vec3(0.299,0.587,0.114)) < 0.025) discard; }\n"
        "    col = a;\n"
        "  }\n"
        "  oAlbedo = vec4(pow(col, vec3(1.0/2.2)), 1.0);\n"
        "  oNormal = vec4(normalize(vNorm) * 0.5 + 0.5, 1.0);\n"
        "}\n";
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
    float sphereY = 0, radius = 0;
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
        glm::vec3 flat;
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
        LoadedPart lp;
        lp.mesh = std::move(m);
        lp.layer = tex ? tex->albedoLayer : -1;
        lp.alphaTest = (tex && tex->alphaTest) ? 1 : 0;
        lp.flat = p.flatColor;
        parts.push_back(std::move(lp));
    }
    if (unionR <= 0.0f)
        unionR = 1.0f;
    out.sphereY = unionC.y;
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
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, atlas, atlas);
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
    const GLint locFlat = glGetUniformLocation(prog, "uFlat");
    const GLint locAlpha = glGetUniformLocation(prog, "uAlphaTest");
    glUniform1i(glGetUniformLocation(prog, "uTex"), 0);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D_ARRAY, rp.static_model_texture_array());
    glEnable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glViewport(0, 0, atlas, atlas);
    glClearColor(kBackground.r, kBackground.g, kBackground.b, 0.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    const float D = unionR * 2.0f;
    const float hr = unionR * 0.52f;
    for (int j = 0; j < n; ++j) {
        for (int i = 0; i < n; ++i) {
            const Vec3f d3 = OctaTileDirection(i, j, grid);
            glm::vec3 dir(d3.x, d3.y, d3.z);
            const glm::vec3 eye = unionC + dir * D;
            const glm::vec3 up =
                (std::fabs(dir.y) > 0.99f) ? glm::vec3(0, 0, 1) : glm::vec3(0, 1, 0);
            const glm::mat4 mvp = glm::ortho(-hr, hr, -hr, hr, 0.01f, D + unionR * 2.0f) *
                                  glm::lookAt(eye, unionC, up);
            glViewport(i * tile, j * tile, tile, tile);
            glUniformMatrix4fv(locMVP, 1, GL_FALSE, &mvp[0][0]);
            for (const LoadedPart& lp : parts) {
                glUniform1i(locLayer, lp.layer);
                glUniform1i(locAlpha, lp.alphaTest);
                glUniform3f(locFlat, lp.flat.r, lp.flat.g, lp.flat.b);
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
    std::vector<unsigned char> nrm(static_cast<std::size_t>(atlas) * atlas * 3u);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glBindTexture(GL_TEXTURE_2D, ra.albedo);
    glGetTexImage(GL_TEXTURE_2D, 0, GL_RGB, GL_UNSIGNED_BYTE, rgb.data());
    glBindTexture(GL_TEXTURE_2D, ra.normal);
    glGetTexImage(GL_TEXTURE_2D, 0, GL_RGB, GL_UNSIGNED_BYTE, nrm.data());

    const unsigned char bgR = static_cast<unsigned char>(kBackground.r * 255.0f);
    const unsigned char bgB = static_cast<unsigned char>(kBackground.b * 255.0f);
    std::vector<float> tileCov(static_cast<std::size_t>(n) * n, 0.0f);
    double covSum = 0.0;
    out.min_coverage = 1.0f;
    for (int tj = 0; tj < n; ++tj)
        for (int ti = 0; ti < n; ++ti) {
            long covered = 0;
            for (int py = 0; py < tile; ++py)
                for (int px = 0; px < tile; ++px) {
                    const std::size_t idx =
                        (static_cast<std::size_t>(tj * tile + py) * atlas + (ti * tile + px)) * 3u;
                    if (!(rgb[idx] == bgR && rgb[idx + 1] == 0 && rgb[idx + 2] == bgB))
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
    WritePpm(normalPath, atlas, atlas, nrm);
    out.ok = WritePpm(outPpmPath, atlas, atlas, rgb);
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
    out.sphereY = ra.sphereY;
    out.radius = ra.radius;
    return out;
}

} // namespace Luminumbra::Rendering
